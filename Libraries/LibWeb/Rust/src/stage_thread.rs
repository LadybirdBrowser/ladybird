/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The threads a document's rendering runs on beside the host's: the StyleLayout thread, which every document's
//! render state lives on, and the Paint thread, which records display lists from the frames the render states publish.
//!
//! There is one of each per process, spawned the first time it is handed a job. A WebContent process runs every
//! document it hosts on its one main thread, so a thread per process is also a thread per event loop. The host waits
//! for a job it hands a thread with [`StageThread::run`], which may borrow from the host's frame. A job it submits
//! with [`StageThread::submit`] owns everything it reads instead, and runs beside the host: the host goes on, and
//! takes the job's answer in from its [`InFlight`] once it has finished. A job it posts with [`StageThread::post`] owns
//! what it reads too, and nobody takes it in.
//!
//! A host can also lease a value to a thread with [`StageThread::lease`]: the value starts landed in a flight the host
//! takes back like a submitted job's answer, and a [`Ticker`] runs jobs on it in place, one at a time, between which it
//! stays parked in the flight. Taking it back waits for at most the one job that runs on it.
//!
//! A thread waiting on the other side of a hand-off sleeps until that side wakes it: a stage thread for its next job,
//! a host for the answer of a job it handed out. Nothing spins or polls, so a waiting thread takes no CPU from the
//! threads that run.

use crate::render_state::{TaskBoundary, render_state_died};
use std::cell::{Cell, UnsafeCell};
use std::marker::PhantomData;
use std::panic::AssertUnwindSafe;
use std::ptr::NonNull;
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};
use std::sync::{Arc, Mutex, OnceLock, PoisonError, Weak};
use std::thread::Thread;

// This crate is not instrumented by ThreadSanitizer, so TSan cannot see the ordering the hand-off gives between the
// host's thread and a stage thread. Tell it explicitly, or every access a stage thread makes to what the host wrote
// would be reported as a race.
#[cfg(feature = "thread-sanitizer")]
mod tsan {
    unsafe extern "C" {
        fn __tsan_acquire(address: *mut std::ffi::c_void);
        fn __tsan_release(address: *mut std::ffi::c_void);
    }

    pub(super) fn release(key: &std::sync::atomic::AtomicBool) {
        // SAFETY: The TSan runtime only uses the address as a synchronization key.
        unsafe { __tsan_release(std::ptr::from_ref(key).cast_mut().cast()) }
    }

    pub(super) fn acquire(key: &std::sync::atomic::AtomicBool) {
        // SAFETY: The TSan runtime only uses the address as a synchronization key.
        unsafe { __tsan_acquire(std::ptr::from_ref(key).cast_mut().cast()) }
    }
}

#[cfg(not(feature = "thread-sanitizer"))]
mod tsan {
    pub(super) fn release(_: &std::sync::atomic::AtomicBool) {}
    pub(super) fn acquire(_: &std::sync::atomic::AtomicBool) {}
}

/// A thread that runs the jobs other threads hand it, one at a time: each while the thread that handed it waits, or
/// beside the thread that submitted it.
pub(crate) struct StageThread {
    thread: Thread,
    shared: &'static Shared,
}

/// What a stage thread shares with the threads that hand it jobs.
struct Shared {
    /// The jobs handed to the thread and not taken yet, the last one handed first, each linked to the one handed
    /// before it.
    handed: AtomicPtr<JobHeader>,
    /// Whether the thread runs a job, rather than waiting for the next one. Its address names the thread to
    /// ThreadSanitizer.
    busy: AtomicBool,
}

static THREAD_SETUP: OnceLock<extern "C" fn()> = OnceLock::new();

/// Makes each stage thread run `setup` first thing once it starts. Call it before any is handed a job; the first setup
/// installed stays.
#[unsafe(no_mangle)]
pub extern "C" fn stage_thread_set_thread_setup(setup: extern "C" fn()) {
    let _ = THREAD_SETUP.set(setup);
}

static FLIGHT_FINISHED: OnceLock<extern "C" fn()> = OnceLock::new();

/// Makes a stage thread call `finished` each time a job submitted to it finishes, on that thread, so that the thread
/// that submitted the job takes its answer in. Call it before any job is submitted; the first callback installed stays.
#[unsafe(no_mangle)]
pub extern "C" fn stage_thread_set_flight_finished(finished: extern "C" fn()) {
    let _ = FLIGHT_FINISHED.set(finished);
}

// The size Linux and macOS give a process's main thread, where style, layout and recording ran before.
const STAGE_THREAD_STACK_SIZE: usize = 8 * 1024 * 1024;

impl StageThread {
    /// Spawns a thread named `name`, which lives as long as the process.
    fn spawn(name: &str) -> Self {
        let shared: &'static Shared = Box::leak(Box::new(Shared {
            handed: AtomicPtr::new(std::ptr::null_mut()),
            busy: AtomicBool::new(false),
        }));
        let spawned = std::thread::Builder::new()
            .name(name.into())
            .stack_size(STAGE_THREAD_STACK_SIZE)
            .spawn(move || {
                if let Some(setup) = THREAD_SETUP.get() {
                    setup();
                }
                loop {
                    let last = wait_for(|| NonNull::new(shared.handed.swap(std::ptr::null_mut(), Ordering::Acquire)));
                    tsan::acquire(&shared.busy);
                    let mut next = Some(in_handed_order(last));
                    while let Some(job) = next {
                        // SAFETY: `StageThread::run` keeps the job it hands out live until it is done, and
                        // `StageThread::submit` gives up the job it hands out to this thread. Its link is read before
                        // it runs, as the thread that handed it may leave the frame it is in once it is done.
                        unsafe {
                            next = job.as_ref().next.get();
                            shared.busy.store(true, Ordering::Relaxed);
                            (job.as_ref().run)(job, &shared.busy);
                        }
                    }
                }
            })
            .expect("a stage thread could not be started");
        Self {
            thread: spawned.thread().clone(),
            shared,
        }
    }

    /// Whether the calling thread is this one.
    pub(crate) fn is_current(&self) -> bool {
        std::thread::current().id() == self.thread.id()
    }

    /// Runs `job` on this thread, or right here on this thread itself, and returns what it answers. The calling thread
    /// waits until the job is done, so the job may borrow from its frame, as a scoped thread's may. A job that panics
    /// panics here.
    pub(crate) fn run<R: Send>(&self, job: impl FnOnce() -> R + Send) -> R {
        if self.is_current() {
            return job();
        }
        let mut answer = None;
        let job = JobInFrame::new(|| answer = Some(std::panic::catch_unwind(AssertUnwindSafe(job))));
        self.hand(NonNull::from(&job).cast());
        wait_for(|| job.done.load(Ordering::Acquire).then_some(()));
        tsan::acquire(&self.shared.busy);
        drop(job);
        match answer {
            Some(Ok(answer)) => answer,
            Some(Err(panic)) => std::panic::resume_unwind(panic),
            None => render_state_died(),
        }
    }

    /// Submits `job` to this thread and goes on: the job owns what it reads, and runs after the jobs handed to the
    /// thread before it. The calling thread takes what the job answers in from the flight once it has finished, and a
    /// job that panics panics there.
    pub(crate) fn submit<R: Send + 'static>(&self, job: impl FnOnce(&StopWord) -> R + Send + 'static) -> InFlight<R> {
        let flight = Arc::new(Flight {
            finished: AtomicBool::new(false),
            stop: StopWord::default(),
            landing: Mutex::default(),
        });
        self.hand(SubmittedJob::give_up(job, Arc::clone(&flight)));
        InFlight {
            flight,
            not_send_or_sync: PhantomData,
        }
    }

    #[cfg_attr(not(test), expect(dead_code, reason = "a clock lease ticks with it"))]
    /// Leases `value` to this thread: it lands at once in the flight the host takes it back from, and the ticker runs
    /// jobs on it there, on this thread, beside the host.
    pub(crate) fn lease<R: Send + 'static>(&'static self, value: R) -> (InFlight<R>, Ticker<R>) {
        let flight = Arc::new(Flight {
            finished: AtomicBool::new(true),
            stop: StopWord::default(),
            landing: Mutex::new(Landing {
                answer: Some(Ok(value)),
                joining: None,
            }),
        });
        let ticker = Ticker {
            flight: Arc::downgrade(&flight),
            thread: self,
        };
        (
            InFlight {
                flight,
                not_send_or_sync: PhantomData,
            },
            ticker,
        )
    }

    /// Posts `job` to this thread and goes on: the job owns what it reads, runs after the jobs handed to the thread
    /// before it, and answers nothing. A job posted from this thread runs once the job it is posted from is done.
    pub(crate) fn post(&self, job: impl FnOnce() + Send + 'static) {
        self.hand(PostedJob::give_up(job));
    }

    /// Hands the job `header` heads to this thread, which runs it after the jobs handed to it before.
    fn hand(&self, header: NonNull<JobHeader>) {
        tsan::release(&self.shared.busy);
        let before = self
            .shared
            .handed
            .update(Ordering::Release, Ordering::Relaxed, |before| {
                // SAFETY: Nothing else reaches the job until it is handed out.
                unsafe { header.as_ref() }.next.set(NonNull::new(before));
                header.as_ptr()
            });
        // The thread takes every job handed to it at once, so the one that handed the first job it has not taken yet
        // wakes it for all of them.
        if before.is_null() {
            self.thread.unpark();
        }
    }
}

/// Relinks the jobs a stage thread took, the last one handed `last` and each linked to the one handed before it, so
/// that each links to the one handed after it, and answers the first one handed.
fn in_handed_order(last: NonNull<JobHeader>) -> NonNull<JobHeader> {
    let (mut job, mut after) = (last, None);
    // SAFETY: A job handed out and not run yet stays live, and only the stage thread that took it reaches it.
    while let Some(before) = unsafe { job.as_ref() }.next.replace(after) {
        after = Some(job);
        job = before;
    }
    job
}

/// What a stage thread knows of a job handed to it: how to run it, which the job's kind says.
struct JobHeader {
    /// Runs the job this header heads, and tells whoever takes its answer that it is done, after it has stored
    /// `false` to the thread's busy flag it is handed.
    run: unsafe fn(NonNull<JobHeader>, &AtomicBool),
    /// Until the stage thread takes the job, the job handed to it before this one and not taken yet; then, the job it
    /// runs after this one.
    next: Cell<Option<NonNull<JobHeader>>>,
}

/// Stores that the stage thread whose busy flag `busy` is has run its job.
fn ran(busy: &AtomicBool) {
    busy.store(false, Ordering::Release);
    tsan::release(busy);
}

/// A job a thread hands a stage thread, in the frame of the thread that handed it.
#[repr(C)]
struct JobInFrame<F> {
    // First, so that a pointer to the header points to the job.
    header: JobHeader,
    /// The thread that waits for the job, which the stage thread takes to wake it.
    waiting: UnsafeCell<Option<Thread>>,
    done: AtomicBool,
    job: UnsafeCell<Option<F>>,
}

impl<F: FnOnce() + Send> JobInFrame<F> {
    /// A job the calling thread is to hand out and wait for.
    fn new(job: F) -> Self {
        Self {
            header: JobHeader {
                run: Self::run_handed,
                next: Cell::new(None),
            },
            waiting: UnsafeCell::new(Some(std::thread::current())),
            done: AtomicBool::new(false),
            job: UnsafeCell::new(Some(job)),
        }
    }

    /// Runs the job `header` heads, then tells the thread that waits for it that it is done.
    ///
    /// # Safety
    ///
    /// `header` must head a live `JobInFrame<F>` that was handed out to the calling thread and is not done yet, which
    /// nothing else reaches meanwhile.
    unsafe fn run_handed(header: NonNull<JobHeader>, busy: &AtomicBool) {
        let job = header.cast::<Self>();
        // SAFETY: Guaranteed by the caller: the thread that handed the job out reaches none of it until `done`.
        let waiting = unsafe {
            if let Some(run) = (*job.as_ref().job.get()).take() {
                run();
            }
            (*job.as_ref().waiting.get()).take()
        };
        ran(busy);
        // SAFETY: As above. The waiting thread may leave the frame the job is in as soon as it sees `done`.
        unsafe { job.as_ref() }.done.store(true, Ordering::Release);
        if let Some(waiting) = waiting {
            waiting.unpark();
        }
    }
}

/// A job a thread submits to a stage thread, on the heap, which the stage thread frees once it has run it.
#[repr(C)]
struct SubmittedJob<F, R> {
    // First, so that a pointer to the header points to the job.
    header: JobHeader,
    job: F,
    flight: Arc<Flight<R>>,
}

impl<F: FnOnce(&StopWord) -> R + Send, R: Send> SubmittedJob<F, R> {
    /// The header of `job` on the heap, which the stage thread it is handed to frees, and lands in `flight`.
    fn give_up(job: F, flight: Arc<Flight<R>>) -> NonNull<JobHeader> {
        let job = Box::new(Self {
            header: JobHeader {
                run: Self::run_submitted,
                next: Cell::new(None),
            },
            job,
            flight,
        });
        NonNull::from(Box::leak(job)).cast()
    }

    /// Runs the job `header` heads, frees it, and lands what it answered in its flight.
    ///
    /// # Safety
    ///
    /// `header` must come from [`Self::give_up`], and be handed out to the calling thread only.
    unsafe fn run_submitted(header: NonNull<JobHeader>, busy: &AtomicBool) {
        // SAFETY: Guaranteed by the caller: the submitting thread gave the job up.
        let job = unsafe { Box::from_raw(header.cast::<Self>().as_ptr()) };
        let Self { job, flight, .. } = *job;
        let answer = std::panic::catch_unwind(AssertUnwindSafe(|| job(&flight.stop)));
        ran(busy);
        flight.land(answer);
        if let Some(finished) = FLIGHT_FINISHED.get() {
            finished();
        }
    }
}

/// A job a thread posts to a stage thread, on the heap, which the stage thread frees once it has run it.
#[repr(C)]
struct PostedJob<F> {
    // First, so that a pointer to the header points to the job.
    header: JobHeader,
    job: F,
}

impl<F: FnOnce() + Send> PostedJob<F> {
    /// The header of `job` on the heap, which the stage thread it is handed to frees.
    fn give_up(job: F) -> NonNull<JobHeader> {
        let job = Box::new(Self {
            header: JobHeader {
                run: Self::run_posted,
                next: Cell::new(None),
            },
            job,
        });
        NonNull::from(Box::leak(job)).cast()
    }

    /// Runs the job `header` heads and frees it. A job that panics ends the process, as nobody takes its panic in.
    ///
    /// # Safety
    ///
    /// `header` must come from [`Self::give_up`], and be handed out to the calling thread only.
    unsafe fn run_posted(header: NonNull<JobHeader>, busy: &AtomicBool) {
        // SAFETY: Guaranteed by the caller: the posting thread gave the job up.
        let job = unsafe { Box::from_raw(header.cast::<Self>().as_ptr()) };
        if std::panic::catch_unwind(AssertUnwindSafe(job.job)).is_err() {
            render_state_died();
        }
        ran(busy);
    }
}

#[cfg_attr(not(test), expect(dead_code, reason = "a clock lease ticks with it"))]
/// What runs jobs on a value a host leased to a stage thread, in place, while the value is parked in the flight the host
/// takes it back from. It never moves the value out, and it does nothing once the flight is gone.
pub(crate) struct Ticker<R> {
    flight: Weak<Flight<R>>,
    thread: &'static StageThread,
}

#[cfg_attr(not(test), expect(dead_code, reason = "a clock lease ticks with it"))]
impl<R: Send + 'static> Ticker<R> {
    /// Posts `job`, which runs on the leased value on the stage thread, unless the host has said the stop word by then:
    /// the job takes the value out of the flight, so that taking it back waits for the job, and parks it there again.
    pub(crate) fn run(&self, job: impl FnOnce(&mut R, &StopWord) + Send + 'static) {
        let flight = self.flight.clone();
        self.thread.post(move || {
            let Some(flight) = flight.upgrade() else {
                return;
            };
            let mut value = {
                let mut landing = flight.landing();
                if flight.stop.is_said() {
                    return;
                }
                // A tick that panicked left its panic in place of the value, for the host to take in.
                let value = match landing.answer.take() {
                    Some(Ok(value)) => value,
                    answer => {
                        landing.answer = answer;
                        return;
                    }
                };
                flight.finished.store(false, Ordering::Relaxed);
                value
            };
            let ran = std::panic::catch_unwind(AssertUnwindSafe(|| job(&mut value, &flight.stop)));
            flight.land(ran.map(|()| value));
        });
    }
}

/// What a submitted job shares with the thread that submitted it.
struct Flight<R> {
    /// Whether the job has finished, and its answer has landed.
    finished: AtomicBool,
    stop: StopWord,
    landing: Mutex<Landing<R>>,
}

/// What the thread that submitted a job tells it once it waits for it: the job skips what it may leave undone, to
/// answer sooner.
#[derive(Default)]
pub(crate) struct StopWord(AtomicBool);

impl StopWord {
    /// Whether the thread that submitted the job waits for it.
    pub(crate) fn is_said(&self) -> bool {
        self.0.load(Ordering::Relaxed)
    }
}

/// Where a submitted job's answer lands, and the thread that waits for it, if one does.
struct Landing<R> {
    answer: Option<std::thread::Result<R>>,
    joining: Option<Thread>,
}

impl<R> Default for Landing<R> {
    fn default() -> Self {
        Self {
            answer: None,
            joining: None,
        }
    }
}

impl<R> Flight<R> {
    fn landing(&self) -> std::sync::MutexGuard<'_, Landing<R>> {
        self.landing.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Lands `answer`, and wakes the thread that waits for it, if one does. It marks the flight finished before it lets
    /// go of the landing, so a joiner that sees the answer there, and so does not wait to be woken, sees it finished.
    fn land(&self, answer: std::thread::Result<R>) {
        let joining = {
            let mut landing = self.landing();
            landing.answer = Some(answer);
            tsan::release(&self.finished);
            self.finished.store(true, Ordering::Release);
            landing.joining.take()
        };
        if let Some(joining) = joining {
            joining.unpark();
        }
    }
}

/// What a job that flies answers, and the right the thread that submitted it spends to wait for it.
pub(crate) trait Flown {
    type JoinRight;
}

/// A job a stage thread runs beside the thread that submitted it, or a value it leased to one, until that thread takes its
/// answer in, which only it does: the flight stays on its thread. The submitting thread never waits for a flight at the
/// top of its event loop, and waits for one elsewhere only by spending the right to.
pub(crate) struct InFlight<R> {
    flight: Arc<Flight<R>>,
    not_send_or_sync: PhantomData<*const ()>,
}

impl<R> InFlight<R> {
    /// A flight that has landed with `answer`, for a unit test, whose render states stay on the test's own thread.
    #[cfg(test)]
    pub(crate) fn landed(answer: R) -> Self {
        Self {
            flight: Arc::new(Flight {
                finished: AtomicBool::new(true),
                stop: StopWord::default(),
                landing: Mutex::new(Landing {
                    answer: Some(Ok(answer)),
                    joining: None,
                }),
            }),
            not_send_or_sync: PhantomData,
        }
    }

    /// Whether the job has finished, so that taking its answer in waits for nothing.
    pub(crate) fn has_finished(&self) -> bool {
        self.flight.finished.load(Ordering::Acquire)
    }

    /// What the job answered, where it has finished, or the flight, where it has not. Only the top of the event loop
    /// takes a flight in this way, between two tasks, and it never waits.
    pub(crate) fn try_take(self, _: &TaskBoundary) -> Result<R, Self> {
        if !self.has_finished() {
            return Err(self);
        }
        Ok(self.take_answer())
    }

    /// Waits for the job to finish, spending `_right`, and answers what it answered. The job hears the stop word first.
    pub(crate) fn join(self, _right: R::JoinRight) -> R
    where
        R: Flown,
    {
        self.say_stop();
        {
            let mut landing = self.flight.landing();
            if landing.answer.is_none() {
                landing.joining = Some(std::thread::current());
            }
        }
        wait_for(|| self.has_finished().then_some(()));
        self.take_answer()
    }

    /// Tells the job that the thread that submitted it waits for it.
    fn say_stop(&self) {
        self.flight.stop.0.store(true, Ordering::Relaxed);
    }

    fn take_answer(self) -> R {
        tsan::acquire(&self.flight.finished);
        match self.flight.landing().answer.take() {
            Some(Ok(answer)) => answer,
            Some(Err(panic)) => std::panic::resume_unwind(panic),
            None => render_state_died(),
        }
    }
}

/// Waits until `ready` answers something, sleeping until woken. The other side of the hand-off makes `ready` answer
/// before it wakes the calling thread, and knows to wake it before the calling thread first asks `ready`, so the wake
/// is never lost: a thread woken before it sleeps does not sleep.
fn wait_for<T>(mut ready: impl FnMut() -> Option<T>) -> T {
    loop {
        if let Some(value) = ready() {
            return value;
        }
        std::thread::park();
    }
}

static STYLE_LAYOUT_THREAD: OnceLock<StageThread> = OnceLock::new();

/// The StyleLayout thread, which every document's render state lives on.
pub(crate) fn style_layout_thread() -> &'static StageThread {
    STYLE_LAYOUT_THREAD.get_or_init(|| StageThread::spawn("StyleLayout"))
}

/// The Paint thread, which records display lists from the frames the render states publish.
pub(crate) fn paint_thread() -> &'static StageThread {
    static PAINT_THREAD: OnceLock<StageThread> = OnceLock::new();
    PAINT_THREAD.get_or_init(|| StageThread::spawn("Paint"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    fn test_thread() -> &'static StageThread {
        static THREAD: OnceLock<StageThread> = OnceLock::new();
        THREAD.get_or_init(|| StageThread::spawn("Test"))
    }

    impl Flown for std::thread::ThreadId {
        type JoinRight = ();
    }

    impl Flown for u32 {
        type JoinRight = ();
    }

    impl Flown for usize {
        type JoinRight = ();
    }

    #[test]
    fn a_job_runs_on_the_stage_thread() {
        assert_eq!(
            test_thread().run(|| std::thread::current().id()),
            test_thread().thread.id()
        );
        assert!(
            test_thread().run(|| test_thread().is_current()),
            "the thread knows itself"
        );
        assert!(!test_thread().is_current());
    }

    #[test]
    fn a_job_borrows_from_the_frame_that_waits_for_it() {
        let mut written = Vec::new();
        let read = [1, 2, 3];
        let sum = test_thread().run(|| {
            written.extend_from_slice(&read);
            read.iter().sum::<i32>()
        });
        assert_eq!(sum, 6);
        assert_eq!(written, read);
    }

    #[test]
    fn a_job_handed_from_the_stage_thread_runs_right_there() {
        let nested = test_thread().run(|| test_thread().run(|| std::thread::current().id()));
        assert_eq!(nested, test_thread().thread.id());
    }

    #[test]
    fn jobs_from_many_threads_each_run_once() {
        let sums: Vec<u64> = std::thread::scope(|scope| {
            let threads: Vec<_> = (0..4u64)
                .map(|index| scope.spawn(move || (0..200).map(|step| test_thread().run(move || index * step)).sum()))
                .collect();
            threads
                .into_iter()
                .map(|thread| thread.join().unwrap_or_default())
                .collect()
        });
        assert_eq!(sums, [0, 19_900, 39_800, 59_700]);
    }

    #[test]
    fn a_posted_job_runs_before_the_jobs_handed_after_it() {
        let ran = Arc::new(AtomicUsize::new(0));
        let posted = Arc::clone(&ran);
        test_thread().post(move || {
            posted.fetch_add(1, Ordering::Relaxed);
        });
        assert_eq!(test_thread().run(|| ran.load(Ordering::Relaxed)), 1);
    }

    #[test]
    fn a_job_that_panics_panics_in_the_thread_that_waits_for_it() {
        let panicked = std::panic::catch_unwind(|| test_thread().run(|| panic!("the job panicked")));
        assert!(panicked.is_err());
        assert_eq!(test_thread().run(|| 7), 7, "the stage thread goes on");
    }

    /// A flag a submitted job waits for, so that a test sees it in flight.
    fn gate() -> (Arc<AtomicBool>, impl FnOnce() + Send + 'static) {
        let open = Arc::new(AtomicBool::new(false));
        let waits = Arc::clone(&open);
        (open, move || {
            while !waits.load(Ordering::Acquire) {
                std::thread::yield_now();
            }
        })
    }

    #[test]
    fn a_submitted_job_runs_beside_the_thread_that_submitted_it() {
        let (open, wait) = gate();
        let mut flight = test_thread().submit(move |_| {
            wait();
            std::thread::current().id()
        });
        for _ in 0..3 {
            flight = match flight.try_take(&TaskBoundary::for_test()) {
                Ok(_) => panic!("a job that has not finished is not taken in"),
                Err(flight) => flight,
            };
        }
        open.store(true, Ordering::Release);
        let ran_on = flight.join(());
        assert_eq!(ran_on, test_thread().thread.id());
    }

    #[test]
    fn a_finished_flight_is_taken_in_at_a_task_boundary() {
        let flight = test_thread().submit(|_| 7);
        // A job handed after the flight runs after it.
        test_thread().run(|| ());
        assert!(flight.has_finished());
        assert!(matches!(flight.try_take(&TaskBoundary::for_test()), Ok(7)));
    }

    #[test]
    fn jobs_handed_while_the_thread_is_busy_run_in_the_order_they_were_handed() {
        let (open, wait) = gate();
        drop(test_thread().submit(move |_| wait()));
        let ran = Arc::new(AtomicUsize::new(0));
        let flights: Vec<_> = (0..8)
            .map(|_| {
                let ran = Arc::clone(&ran);
                test_thread().submit(move |_| ran.fetch_add(1, Ordering::Relaxed))
            })
            .collect();
        open.store(true, Ordering::Release);
        let order: Vec<_> = flights.into_iter().map(|flight| flight.join(())).collect();
        assert_eq!(order, (0..8).collect::<Vec<_>>());
    }

    #[test]
    fn a_submitted_job_that_panics_panics_where_it_is_taken_in() {
        let flight = test_thread().submit(|_| -> u32 { panic!("the job panicked") });
        let panicked = std::panic::catch_unwind(AssertUnwindSafe(|| flight.join(())));
        assert!(panicked.is_err());
        assert_eq!(test_thread().run(|| 7), 7, "the stage thread goes on");
    }

    #[test]
    fn a_flight_dropped_unjoined_lands_its_answer_on_the_stage_thread() {
        let (open, wait) = gate();
        let answer = Arc::new(());
        let held = Arc::clone(&answer);
        drop(test_thread().submit(move |_| {
            wait();
            held
        }));
        open.store(true, Ordering::Release);
        test_thread().run(|| ());
        assert_eq!(Arc::strong_count(&answer), 1, "the answer nobody took in is dropped");
    }

    impl Flown for bool {
        type JoinRight = ();
    }

    #[test]
    fn a_tick_runs_on_a_leased_value_beside_the_host() {
        let (lease, ticker) = test_thread().lease(1_u32);
        ticker.run(|value, _| *value += 1);
        ticker.run(|value, _| *value *= 10);
        // A job handed after the ticks runs after them.
        test_thread().run(|| ());
        assert!(lease.has_finished(), "the value is parked between ticks");
        assert_eq!(lease.join(()), 20);
    }

    #[test]
    fn taking_a_parked_lease_back_waits_for_nothing() {
        let (lease, _ticker) = test_thread().lease(7_u32);
        assert!(lease.has_finished());
        assert_eq!(lease.join(()), 7);
    }

    #[test]
    fn taking_a_lease_back_waits_for_the_tick_that_runs_and_no_other() {
        let (lease, ticker) = test_thread().lease(0_u32);
        let (started, wait_for_start) = std::sync::mpsc::channel();
        let (go, wait_for_go) = std::sync::mpsc::channel::<()>();
        ticker.run(move |value, _| {
            started.send(()).ok();
            wait_for_go.recv().ok();
            *value += 1;
        });
        ticker.run(|value, _| *value += 100);
        wait_for_start.recv().ok();
        assert!(!lease.has_finished(), "the running tick has the value");
        lease.say_stop();
        go.send(()).ok();
        assert_eq!(
            lease.join(()),
            1,
            "the tick queued behind the running one hears the stop word"
        );
    }

    #[test]
    fn a_ticker_outliving_its_lease_does_nothing() {
        let (lease, ticker) = test_thread().lease(0_u32);
        assert_eq!(lease.join(()), 0);
        let ran = Arc::new(AtomicBool::new(false));
        let ran_in_tick = Arc::clone(&ran);
        ticker.run(move |_, _| ran_in_tick.store(true, Ordering::Relaxed));
        test_thread().run(|| ());
        assert!(!ran.load(Ordering::Relaxed));
    }

    #[test]
    fn a_flight_hears_the_stop_word_its_join_says() {
        let (go, wait) = std::sync::mpsc::channel();
        let flight = test_thread().submit(move |stop| {
            wait.recv().ok();
            stop.is_said()
        });
        flight.say_stop();
        go.send(()).ok();
        assert!(flight.join(()));
        let unstopped = test_thread().submit(|stop| stop.is_said());
        test_thread().run(|| ());
        assert!(matches!(unstopped.try_take(&TaskBoundary::for_test()), Ok(false)));
    }

    trait AmbiguousIfSend<A> {
        fn marker() {}
    }

    impl<T: ?Sized> AmbiguousIfSend<()> for T {}
    impl<T: ?Sized + Send> AmbiguousIfSend<u8> for T {}

    // Fails to compile, as the call is ambiguous, if a flight ever becomes Send.
    #[test]
    fn a_flight_stays_on_the_thread_that_submitted_it() {
        <InFlight<u32> as AmbiguousIfSend<_>>::marker();
    }
}
