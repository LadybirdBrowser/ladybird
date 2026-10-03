/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The thread a document's rendering runs on beside the host's: the StyleLayout thread, which every document's render
//! state lives on.
//!
//! There is one per process, spawned the first time it is handed a job. A WebContent process runs every document it
//! hosts on its one main thread, so a thread per process is also a thread per event loop. The host waits for every job
//! it hands the thread, so nothing it runs overlaps the host: what changes is where the work runs, and so what it may
//! reach.

use std::cell::UnsafeCell;
use std::panic::AssertUnwindSafe;
use std::ptr::NonNull;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};
use std::thread::Thread;
use std::time::{Duration, Instant};

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

/// A thread that runs the jobs other threads hand it, one at a time, each while the thread that handed it waits.
pub(crate) struct StageThread {
    thread: Thread,
    shared: &'static Shared,
}

/// What a stage thread shares with the threads that hand it jobs.
struct Shared {
    /// The job handed to the thread and not taken yet, if any.
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

// The size Linux and macOS give a process's main thread, where style and layout ran before.
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
                    // Only this thread takes a job, so one it sees stays there until it takes it.
                    let job = wait_for(|| {
                        NonNull::new(shared.handed.load(Ordering::Relaxed))?;
                        NonNull::new(shared.handed.swap(std::ptr::null_mut(), Ordering::Acquire))
                    });
                    tsan::acquire(&shared.busy);
                    shared.busy.store(true, Ordering::Relaxed);
                    // SAFETY: Only `StageThread::run` hands a job out, and it waits until the job is done.
                    unsafe { JobHeader::run(job, &shared.busy) };
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
        let header = std::ptr::from_ref(&job.header).cast_mut();
        tsan::release(&self.shared.busy);
        // Another thread's job leaves the slot as soon as this thread takes it.
        while self
            .shared
            .handed
            .compare_exchange_weak(std::ptr::null_mut(), header, Ordering::Release, Ordering::Relaxed)
            .is_err()
        {
            std::thread::yield_now();
        }
        self.thread.unpark();
        wait_for(|| job.header.done.load(Ordering::Acquire).then_some(()));
        tsan::acquire(&self.shared.busy);
        drop(job);
        match answer {
            Some(Ok(answer)) => answer,
            Some(Err(panic)) => std::panic::resume_unwind(panic),
            None => crate::render_state::render_state_died(),
        }
    }
}

/// What a stage thread knows of a job handed to it, which lives in the frame of the thread that handed it out until
/// `done`.
struct JobHeader {
    /// Runs the job this header heads.
    run: unsafe fn(NonNull<JobHeader>),
    /// The thread that waits for the job, which the stage thread takes to wake it.
    waiting: UnsafeCell<Option<Thread>>,
    done: AtomicBool,
}

impl JobHeader {
    /// Runs the job `header` heads, then tells the thread that waits for it that it is done.
    ///
    /// # Safety
    ///
    /// `header` must head a job that was handed out to the calling thread and is not done yet.
    unsafe fn run(header: NonNull<JobHeader>, busy: &AtomicBool) {
        // SAFETY: Guaranteed by the caller: the thread that handed the job out reaches none of it until `done`.
        let waiting = unsafe {
            let header = header.as_ref();
            (header.run)(NonNull::from(header));
            (*header.waiting.get()).take()
        };
        busy.store(false, Ordering::Release);
        tsan::release(busy);
        // SAFETY: As above. The waiting thread may leave the frame the job is in as soon as it sees `done`.
        unsafe { header.as_ref() }.done.store(true, Ordering::Release);
        if let Some(waiting) = waiting {
            waiting.unpark();
        }
    }
}

/// A job a thread hands a stage thread, in the frame of the thread that handed it.
#[repr(C)]
struct JobInFrame<F> {
    // First, so that a pointer to the header points to the job.
    header: JobHeader,
    job: UnsafeCell<Option<F>>,
}

impl<F: FnOnce() + Send> JobInFrame<F> {
    /// A job the calling thread is to hand out and wait for.
    fn new(job: F) -> Self {
        Self {
            header: JobHeader {
                run: Self::run_handed,
                waiting: UnsafeCell::new(Some(std::thread::current())),
                done: AtomicBool::new(false),
            },
            job: UnsafeCell::new(Some(job)),
        }
    }

    /// Runs the job `header` heads.
    ///
    /// # Safety
    ///
    /// `header` must head a live `JobInFrame<F>` whose job nothing else reaches meanwhile.
    unsafe fn run_handed(header: NonNull<JobHeader>) {
        // SAFETY: Guaranteed by the caller.
        if let Some(job) = unsafe { (*header.cast::<Self>().as_ref().job.get()).take() } {
            job();
        }
    }
}

/// How long a thread spins for what the other side of a hand-off answers before it sleeps. The host and a stage thread
/// take turns, and a turn is mostly shorter than waking a sleeping thread takes.
const SPIN: Duration = Duration::from_micros(200);

/// How long a thread then sleeps in slices of [`LIGHT_SLEEP_SLICE`] before it sleeps until woken. A thread that sleeps
/// with no timeout lets its core idle deeply, which takes tens of microseconds to leave; a short timeout keeps the core
/// in a light idle state, which it leaves in a few. A thread that just handed or took a job mostly sees the next well
/// within this.
const LIGHT_SLEEP: Duration = Duration::from_millis(10);
const LIGHT_SLEEP_SLICE: Duration = Duration::from_micros(250);

/// Waits until `ready` answers something, which the other side of a hand-off makes it answer before it wakes the
/// calling thread: spins for [`SPIN`], sleeps lightly until [`LIGHT_SLEEP`], then sleeps until woken.
fn wait_for<T>(mut ready: impl FnMut() -> Option<T>) -> T {
    let started = Instant::now();
    loop {
        if let Some(value) = ready() {
            return value;
        }
        let waited = started.elapsed();
        if waited < SPIN {
            std::hint::spin_loop();
        } else if waited < LIGHT_SLEEP {
            std::thread::park_timeout(LIGHT_SLEEP_SLICE);
        } else {
            std::thread::park();
        }
    }
}

static STYLE_LAYOUT_THREAD: OnceLock<StageThread> = OnceLock::new();

/// The StyleLayout thread, which every document's render state lives on.
pub(crate) fn style_layout_thread() -> &'static StageThread {
    STYLE_LAYOUT_THREAD.get_or_init(|| StageThread::spawn("StyleLayout"))
}

/// Whether the calling thread may reach what the render states hold: it is the StyleLayout thread, or that thread
/// waits for its next job, as it does whenever the host runs.
pub(crate) fn may_reach_render_states() -> bool {
    STYLE_LAYOUT_THREAD
        .get()
        .is_none_or(|thread| thread.is_current() || !thread.shared.busy.load(Ordering::Acquire))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_thread() -> &'static StageThread {
        static THREAD: OnceLock<StageThread> = OnceLock::new();
        THREAD.get_or_init(|| StageThread::spawn("Test"))
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
    fn a_job_that_panics_panics_in_the_thread_that_waits_for_it() {
        let panicked = std::panic::catch_unwind(|| test_thread().run(|| panic!("the job panicked")));
        assert!(panicked.is_err());
        assert_eq!(test_thread().run(|| 7), 7, "the stage thread goes on");
    }
}
