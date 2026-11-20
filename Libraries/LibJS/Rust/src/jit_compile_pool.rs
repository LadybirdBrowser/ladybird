/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A pool of threads that the VMs of a process share for their compiles. Each VM is a client with a queue of its own, and
//! the threads serve the queues in turn, so a VM with many jobs delays another by no more than the jobs already running.
//!
//! A task must be Send and 'static, and the snapshots that compiles work on hold no pointers, so a task cannot reach the
//! VM it came from, or its heap. That is why a client that goes away only has to drop what is queued: a task that is
//! running ends on its own, and has nobody left to give its result to.

use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};
use std::time::Duration;

pub type Task = Box<dyn FnOnce() + Send>;

/// The queue of the tasks that belong to no client, and outlive the client that made them.
const UNOWNED: u64 = 0;

#[derive(Default)]
struct State {
    queues: HashMap<u64, VecDeque<Task>>,
    /// The clients that have tasks, in the order the threads serve them.
    turns: VecDeque<u64>,
    queued_task_count: usize,
    next_client_id: u64,
    thread_count: usize,
    idle_thread_count: usize,
}

impl State {
    fn enqueue(&mut self, client: u64, task: Task) {
        let queue = self.queues.entry(client).or_default();
        if queue.is_empty() {
            self.turns.push_back(client);
        }
        queue.push_back(task);
        self.queued_task_count += 1;
    }

    fn take_next_task(&mut self) -> Option<Task> {
        let client = self.turns.pop_front()?;
        let queue = self.queues.get_mut(&client).expect("a client with a turn has a queue");
        let task = queue.pop_front().expect("a client with a turn has a task");
        if queue.is_empty() {
            self.queues.remove(&client);
        } else {
            self.turns.push_back(client);
        }
        self.queued_task_count -= 1;
        Some(task)
    }
}

pub struct CompilePool {
    state: Mutex<State>,
    task_available: Condvar,
    max_thread_count: usize,
    thread_name: &'static str,
    idle_delay: Duration,
    on_idle: Box<dyn Fn() + Send + Sync>,
}

impl CompilePool {
    /// A pool that starts threads as it needs them, up to `max_thread_count`. A thread that goes `idle_delay` without a
    /// task after having had some calls `on_idle` once.
    pub fn new(
        max_thread_count: usize,
        thread_name: &'static str,
        idle_delay: Duration,
        on_idle: impl Fn() + Send + Sync + 'static,
    ) -> Arc<Self> {
        Arc::new(Self {
            state: Mutex::new(State {
                next_client_id: UNOWNED + 1,
                ..State::default()
            }),
            task_available: Condvar::new(),
            max_thread_count: max_thread_count.max(1),
            thread_name,
            idle_delay,
            on_idle: Box::new(on_idle),
        })
    }

    pub fn add_client(self: &Arc<Self>) -> CompilePoolClient {
        let mut state = self.lock_state();
        let id = state.next_client_id;
        state.next_client_id += 1;
        CompilePoolClient {
            pool: Arc::clone(self),
            id,
        }
    }

    /// Runs a task that stays queued when the client that made it goes away.
    pub fn run_unowned(self: &Arc<Self>, task: Task) {
        self.enqueue(UNOWNED, task);
    }

    fn lock_state(&self) -> MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(PoisonError::into_inner)
    }

    fn enqueue(self: &Arc<Self>, client: u64, task: Task) {
        let mut state = self.lock_state();
        state.enqueue(client, task);
        if state.queued_task_count > state.idle_thread_count && state.thread_count < self.max_thread_count {
            self.start_thread(&mut state);
        }
        drop(state);
        self.task_available.notify_one();
    }

    fn start_thread(self: &Arc<Self>, state: &mut State) {
        let pool = Arc::clone(self);
        let spawned = std::thread::Builder::new()
            .name(self.thread_name.to_string())
            .spawn(move || pool.run_thread());
        match spawned {
            Ok(_) => state.thread_count += 1,
            // NB: The pool needs one thread. If the process cannot start more, the tasks run on those it started.
            Err(error) => assert!(state.thread_count > 0, "starting a compile thread: {error}"),
        }
    }

    fn run_thread(&self) {
        let mut has_run_tasks_since_idle = false;
        loop {
            match self.next_task(has_run_tasks_since_idle) {
                Some(task) => {
                    has_run_tasks_since_idle = true;
                    task();
                }
                None => {
                    has_run_tasks_since_idle = false;
                    (self.on_idle)();
                }
            }
        }
    }

    /// The next task, waiting for one. Gives up after the idle delay if `may_give_up`.
    fn next_task(&self, may_give_up: bool) -> Option<Task> {
        let mut state = self.lock_state();
        loop {
            if let Some(task) = state.take_next_task() {
                return Some(task);
            }
            state.idle_thread_count += 1;
            let timed_out = if may_give_up {
                let (next_state, wait) = self
                    .task_available
                    .wait_timeout(state, self.idle_delay)
                    .unwrap_or_else(PoisonError::into_inner);
                state = next_state;
                wait.timed_out()
            } else {
                state = self.task_available.wait(state).unwrap_or_else(PoisonError::into_inner);
                false
            };
            state.idle_thread_count -= 1;
            if timed_out && state.queued_task_count == 0 {
                return None;
            }
        }
    }
}

/// The queue of one VM. Dropping this drops the tasks in it that no thread took yet.
pub struct CompilePoolClient {
    pool: Arc<CompilePool>,
    id: u64,
}

impl CompilePoolClient {
    pub fn submit(&self, task: Task) {
        self.pool.enqueue(self.id, task);
    }

    pub fn pool(&self) -> &Arc<CompilePool> {
        &self.pool
    }
}

impl Drop for CompilePoolClient {
    fn drop(&mut self) {
        let mut state = self.pool.lock_state();
        let tasks = state.queues.remove(&self.id);
        if let Some(tasks) = &tasks {
            state.queued_task_count -= tasks.len();
            state.turns.retain(|client| *client != self.id);
        }
        // NB: What the tasks hold may take locks of its own.
        drop(state);
        drop(tasks);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::mpsc::{Receiver, Sender, channel};

    const NEVER: Duration = Duration::from_secs(3600);
    const LONG_ENOUGH: Duration = Duration::from_secs(30);

    fn pool(max_thread_count: usize) -> Arc<CompilePool> {
        CompilePool::new(max_thread_count, "test pool", NEVER, || {})
    }

    /// Takes the only thread of a pool until the returned sender is used or dropped.
    fn occupy(client: &CompilePoolClient) -> Sender<()> {
        let (started_sender, started) = channel();
        let (release, released) = channel::<()>();
        client.submit(Box::new(move || {
            started_sender.send(()).unwrap();
            let _ = released.recv();
        }));
        started.recv_timeout(LONG_ENOUGH).unwrap();
        release
    }

    fn logging_task(log: &Sender<&'static str>, entry: &'static str) -> Task {
        let log = log.clone();
        Box::new(move || log.send(entry).unwrap())
    }

    fn next(log: &Receiver<&'static str>) -> &'static str {
        log.recv_timeout(LONG_ENOUGH).unwrap()
    }

    #[test]
    fn a_busy_client_delays_another_by_one_task_at_most() {
        let pool = pool(1);
        let (busy, other) = (pool.add_client(), pool.add_client());
        let release = occupy(&busy);
        let (log_sender, log) = channel();
        for _ in 0..100 {
            busy.submit(logging_task(&log_sender, "busy"));
        }
        other.submit(logging_task(&log_sender, "other"));
        drop(release);

        assert_eq!(next(&log), "busy");
        assert_eq!(next(&log), "other");
        for _ in 0..99 {
            assert_eq!(next(&log), "busy");
        }
    }

    #[test]
    fn clients_take_turns_and_keep_their_own_order() {
        let pool = pool(1);
        let (first, second) = (pool.add_client(), pool.add_client());
        let release = occupy(&first);
        let (log_sender, log) = channel();
        for entry in ["a1", "a2", "a3"] {
            first.submit(logging_task(&log_sender, entry));
        }
        for entry in ["b1", "b2"] {
            second.submit(logging_task(&log_sender, entry));
        }
        drop(release);
        let order: Vec<_> = (0..5).map(|_| next(&log)).collect();
        assert_eq!(order, ["a1", "b1", "a2", "b2", "a3"]);
    }

    #[test]
    fn a_client_that_goes_away_drops_only_its_queued_tasks() {
        let pool = pool(1);
        let (gone, stays) = (pool.add_client(), pool.add_client());
        let release = occupy(&stays);
        let (log_sender, log) = channel();
        gone.submit(logging_task(&log_sender, "dropped"));
        stays.submit(logging_task(&log_sender, "kept"));
        pool.run_unowned(logging_task(&log_sender, "unowned"));
        drop(gone);
        drop(release);
        let mut entries = [next(&log), next(&log)];
        entries.sort();
        assert_eq!(entries, ["kept", "unowned"]);
        drop(log_sender);
        assert!(log.recv_timeout(Duration::from_millis(50)).is_err());
    }

    #[test]
    fn a_task_that_is_running_survives_its_client_and_the_thread_goes_on() {
        let pool = pool(1);
        let client = pool.add_client();
        let release = occupy(&client);
        drop(client);
        drop(release);

        let (log_sender, log) = channel();
        let other = pool.add_client();
        other.submit(logging_task(&log_sender, "after"));
        assert_eq!(next(&log), "after");
    }

    #[test]
    fn dropping_a_client_drops_what_its_tasks_hold() {
        struct CountsDrop(Arc<AtomicUsize>);
        impl Drop for CountsDrop {
            fn drop(&mut self) {
                self.0.fetch_add(1, Ordering::SeqCst);
            }
        }
        let pool = pool(1);
        let (gone, stays) = (pool.add_client(), pool.add_client());
        let release = occupy(&stays);
        let drops = Arc::new(AtomicUsize::new(0));
        for _ in 0..3 {
            let held = CountsDrop(Arc::clone(&drops));
            gone.submit(Box::new(move || drop(held)));
        }
        assert_eq!(drops.load(Ordering::SeqCst), 0);
        drop(gone);
        assert_eq!(drops.load(Ordering::SeqCst), 3);
        drop(release);
    }

    #[test]
    fn the_pool_starts_threads_as_needed_up_to_its_limit() {
        let pool = pool(3);
        let client = pool.add_client();
        assert_eq!(pool.lock_state().thread_count, 0);

        let running = Arc::new(AtomicUsize::new(0));
        let (release, released) = channel::<()>();
        let released = Arc::new(Mutex::new(released));
        let (finished_sender, finished) = channel();
        for _ in 0..8 {
            let running = Arc::clone(&running);
            let released = Arc::clone(&released);
            let finished_sender = finished_sender.clone();
            client.submit(Box::new(move || {
                running.fetch_add(1, Ordering::SeqCst);
                let _ = released.lock().unwrap().recv();
                running.fetch_sub(1, Ordering::SeqCst);
                finished_sender.send(()).unwrap();
            }));
        }
        while running.load(Ordering::SeqCst) < 3 {
            std::thread::yield_now();
        }
        std::thread::sleep(Duration::from_millis(20));
        assert_eq!(running.load(Ordering::SeqCst), 3);
        assert_eq!(pool.lock_state().thread_count, 3);
        drop(release);
        for _ in 0..8 {
            finished.recv_timeout(LONG_ENOUGH).unwrap();
        }
    }

    #[test]
    fn idle_threads_are_reused() {
        let pool = pool(4);
        let client = pool.add_client();
        let (log_sender, log) = channel();
        for _ in 0..20 {
            client.submit(logging_task(&log_sender, "task"));
            assert_eq!(next(&log), "task");
            // NB: Until the thread is back to waiting for tasks, a new one is a fair answer to a new task.
            while pool.lock_state().idle_thread_count == 0 {
                std::thread::yield_now();
            }
        }
        assert_eq!(pool.lock_state().thread_count, 1);
    }

    #[test]
    fn a_thread_reports_idle_once_after_it_ran_tasks() {
        let idle_calls = Arc::new(AtomicUsize::new(0));
        let idle_calls_of_pool = Arc::clone(&idle_calls);
        let pool = CompilePool::new(1, "test pool", Duration::from_millis(10), move || {
            idle_calls_of_pool.fetch_add(1, Ordering::SeqCst);
        });
        let client = pool.add_client();
        let (log_sender, log) = channel();
        client.submit(logging_task(&log_sender, "task"));
        assert_eq!(next(&log), "task");
        std::thread::sleep(Duration::from_millis(100));
        assert_eq!(idle_calls.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn many_clients_on_many_threads_run_every_task_once() {
        let pool = pool(4);
        let ran = Arc::new(AtomicUsize::new(0));
        std::thread::scope(|scope| {
            for _ in 0..16 {
                let pool = Arc::clone(&pool);
                let ran = Arc::clone(&ran);
                scope.spawn(move || {
                    let client = pool.add_client();
                    let (finished_sender, finished) = channel();
                    for _ in 0..200 {
                        let ran = Arc::clone(&ran);
                        let finished_sender = finished_sender.clone();
                        client.submit(Box::new(move || {
                            ran.fetch_add(1, Ordering::SeqCst);
                            finished_sender.send(()).unwrap();
                        }));
                    }
                    for _ in 0..200 {
                        finished.recv_timeout(LONG_ENOUGH).unwrap();
                    }
                });
            }
        });
        assert_eq!(ran.load(Ordering::SeqCst), 16 * 200);
    }
}
