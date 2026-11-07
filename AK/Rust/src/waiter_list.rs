/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WaiterLists of a process, which Atomics.wait, Atomics.notify and their WebAssembly counterparts use: one list of
//! all that wait, in the order the agents began to wait, under one lock, with a condition variable per agent. The
//! WaiterList of an address is what has that address in this list.
//!
//! A word has one address in a process, as every agent shares one mapping of it, and it never has waiters in another
//! process, as shared memory stays in its agent cluster, and an agent cluster stays in its process. Having a condition
//! variable per agent means that a notify wakes the agents it took off the list, and that an interrupt wakes its agent,
//! and nobody else.
//!
//! An agent waits on one word at a time, so this list is as long as the number of agents that are asleep, and a process
//! has few of those. Going through it takes less time than a wait that allocates a list of its own, and frees it on
//! another thread.

use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use core::time::Duration;
use std::collections::VecDeque;
use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};
use std::time::Instant;

/// Core::AtomicWaitResult.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AtomicWaitResult {
    Woken,
    NotEqual,
    TimedOut,
    Interrupted,
}

/// The word of a shared typed array element that Atomics.wait waits on, which is 4 or 8 bytes wide.
#[derive(Clone, Copy)]
pub enum WaitWord<'a> {
    Word32(&'a AtomicU32),
    Word64(&'a AtomicU64),
}

impl WaitWord<'_> {
    /// The address of the word, which is what the WaiterList of a location is found by: a 4-byte and an 8-byte view of
    /// the same location share it.
    fn address(self) -> usize {
        match self {
            WaitWord::Word32(word) => word.as_ptr().addr(),
            WaitWord::Word64(word) => word.as_ptr().addr(),
        }
    }

    /// Whether the word equals `expected`, by a sequentially-consistent read, which matches the memory model's read for
    /// the wait comparison.
    fn equals(self, expected: u64) -> bool {
        match self {
            WaitWord::Word32(word) => word.load(Ordering::SeqCst) == expected as u32,
            WaitWord::Word64(word) => word.load(Ordering::SeqCst) == expected,
        }
    }
}

/// The node of one agent in the WaiterLists. An agent has one thread, so it is in one list at most.
pub struct AgentWaiter {
    condition: Condvar,
    is_interrupted: AtomicBool,
    /// Set by the notify that took the agent off its list.
    was_notified: AtomicBool,
}

impl AgentWaiter {
    pub fn new() -> Arc<Self> {
        Arc::new(Self {
            condition: Condvar::new(),
            is_interrupted: AtomicBool::new(false),
            was_notified: AtomicBool::new(false),
        })
    }

    /// Whether interrupt() was called.
    pub fn is_interrupted(&self) -> bool {
        self.is_interrupted.load(Ordering::SeqCst)
    }

    /// Whether the agent is on the list of a word, waiting for a notify. Any thread may call this.
    pub fn is_waiting(self: &Arc<Self>) -> bool {
        lists().iter().any(|waiting| Arc::ptr_eq(&waiting.waiter, self))
    }

    /// Ends the wait of this agent with AtomicWaitResult::Interrupted, and every later one at once. Any thread may call
    /// this. The agent loses its place in its list, which does not matter, as it never comes back.
    pub fn interrupt(&self) {
        self.is_interrupted.store(true, Ordering::SeqCst);
        // NB: A waiter looks at the flag and goes to sleep with the lock held, so this cannot go past it.
        let _lists = lists();
        self.condition.notify_one();
    }
}

struct Waiting {
    address: usize,
    waiter: Arc<AgentWaiter>,
}

type Lists = VecDeque<Waiting>;

fn lists() -> MutexGuard<'static, Lists> {
    static LISTS: Mutex<Lists> = Mutex::new(VecDeque::new());
    LISTS.lock().unwrap_or_else(PoisonError::into_inner)
}

fn remove(lists: &mut Lists, waiter: &Arc<AgentWaiter>) {
    lists.retain(|waiting| !Arc::ptr_eq(&waiting.waiter, waiter));
}

/// Puts `waiter` on the list of the word unless it holds something else than `expected`, and sleeps until a notify takes
/// it off, its interrupt comes, or `timeout` passes.
pub fn atomic_wait(
    word: WaitWord<'_>,
    expected: u64,
    timeout: Option<Duration>,
    waiter: &Arc<AgentWaiter>,
) -> AtomicWaitResult {
    let address = word.address();
    // NB: A deadline too far away to express is no deadline.
    let deadline = timeout.and_then(|timeout| Instant::now().checked_add(timeout));

    // NB: The word is compared with the lock held, so a notify that follows a store to it finds this waiter on the list
    //     if the comparison did not see the store.
    let mut lists = lists();
    if waiter.is_interrupted() {
        return AtomicWaitResult::Interrupted;
    }
    if !word.equals(expected) {
        return AtomicWaitResult::NotEqual;
    }
    waiter.was_notified.store(false, Ordering::Relaxed);
    lists.push_back(Waiting {
        address,
        waiter: Arc::clone(waiter),
    });

    // NB: The condition variable can wake for no reason, so only what the wake left behind counts. A notify that came
    //     along with the timeout already counted this waiter, so it wins.
    loop {
        if waiter.is_interrupted() {
            remove(&mut lists, waiter);
            return AtomicWaitResult::Interrupted;
        }
        if waiter.was_notified.load(Ordering::Relaxed) {
            return AtomicWaitResult::Woken;
        }
        let Some(deadline) = deadline else {
            lists = waiter.condition.wait(lists).unwrap_or_else(PoisonError::into_inner);
            continue;
        };
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            remove(&mut lists, waiter);
            return AtomicWaitResult::TimedOut;
        }
        lists = waiter
            .condition
            .wait_timeout(lists, remaining)
            .unwrap_or_else(PoisonError::into_inner)
            .0;
    }
}

/// Takes the first `max_count` agents off the list of the word, wakes them, and returns how many there were.
pub fn atomic_notify(word: &AtomicU32, max_count: usize) -> usize {
    let address = word.as_ptr().addr();
    let mut count = 0;
    lists().retain(|waiting| {
        if waiting.address != address {
            return true;
        }
        // NB: An interrupted agent is on its way out, and only has to get the lock to leave. Counting it would take the
        //     wake from an agent that is really waiting.
        if waiting.waiter.is_interrupted() {
            return false;
        }
        if count == max_count {
            return true;
        }
        count += 1;
        waiting.waiter.was_notified.store(true, Ordering::Relaxed);
        waiting.waiter.condition.notify_one();
        false
    });
    count
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    /// Long enough for a thread that was just started to get to sleep.
    const TIME_TO_SLEEP: Duration = Duration::from_millis(30);

    fn wait(word: &AtomicU32, expected: u64, timeout: Option<Duration>, waiter: &Arc<AgentWaiter>) -> AtomicWaitResult {
        atomic_wait(WaitWord::Word32(word), expected, timeout, waiter)
    }

    fn waiters_of(word: &AtomicU32) -> usize {
        let address = word.as_ptr().addr();
        lists().iter().filter(|waiting| waiting.address == address).count()
    }

    fn wait_for_waiters(word: &AtomicU32, count: usize) {
        while waiters_of(word) != count {
            std::thread::yield_now();
        }
    }

    #[test]
    fn notifies_wake_waiters_of_a_word_that_did_not_change() {
        let word = AtomicU32::new(0);
        std::thread::scope(|scope| {
            let waiter = scope.spawn(|| wait(&word, 0, None, &AgentWaiter::new()));
            wait_for_waiters(&word, 1);
            assert_eq!(atomic_notify(&word, 1), 1);
            assert_eq!(waiter.join().unwrap(), AtomicWaitResult::Woken);
        });
    }

    #[test]
    fn a_word_that_differs_is_not_waited_on() {
        let word = AtomicU32::new(1);
        assert_eq!(
            wait(&word, 0, Some(Duration::ZERO), &AgentWaiter::new()),
            AtomicWaitResult::NotEqual
        );
        // NB: Only the low half of a 4-byte word's expected value counts.
        assert_eq!(
            wait(&word, 0x1_0000_0001, Some(Duration::ZERO), &AgentWaiter::new()),
            AtomicWaitResult::TimedOut
        );
        let wide_word = AtomicU64::new(0x1_0000_0000);
        assert_eq!(
            atomic_wait(WaitWord::Word64(&wide_word), 0, None, &AgentWaiter::new()),
            AtomicWaitResult::NotEqual
        );
    }

    #[test]
    fn a_change_of_the_word_wakes_nobody() {
        let word = AtomicU32::new(0);
        std::thread::scope(|scope| {
            let waiter = scope.spawn(|| wait(&word, 0, Some(Duration::from_millis(100)), &AgentWaiter::new()));
            wait_for_waiters(&word, 1);
            word.store(1, Ordering::SeqCst);
            assert_eq!(waiter.join().unwrap(), AtomicWaitResult::TimedOut);
        });
    }

    #[test]
    fn waits_time_out_and_leave_the_list() {
        let word = AtomicU32::new(0);
        let start = Instant::now();
        assert_eq!(
            wait(&word, 0, Some(Duration::from_millis(20)), &AgentWaiter::new()),
            AtomicWaitResult::TimedOut
        );
        assert!(start.elapsed() >= Duration::from_millis(20));
        assert_eq!(atomic_notify(&word, 1), 0);
    }

    #[test]
    fn notifies_wake_in_the_order_of_the_waits_and_count_what_they_wake() {
        let word = AtomicU32::new(0);
        let wake_order = Mutex::new(Vec::new());
        std::thread::scope(|scope| {
            for index in 0..4 {
                let word = &word;
                let wake_order = &wake_order;
                scope.spawn(move || {
                    let result = wait(word, 0, None, &AgentWaiter::new());
                    assert_eq!(result, AtomicWaitResult::Woken);
                    wake_order.lock().unwrap().push(index);
                });
                wait_for_waiters(&word, index + 1);
            }
            for index in 0..4 {
                assert_eq!(atomic_notify(&word, 1), 1);
                while wake_order.lock().unwrap().len() != index + 1 {
                    std::thread::yield_now();
                }
            }
            assert_eq!(atomic_notify(&word, 1), 0);
        });
        assert_eq!(*wake_order.lock().unwrap(), vec![0, 1, 2, 3]);
    }

    #[test]
    fn notifies_wake_no_more_than_they_are_asked_to() {
        let word = AtomicU32::new(0);
        std::thread::scope(|scope| {
            for _ in 0..3 {
                scope.spawn(|| wait(&word, 0, None, &AgentWaiter::new()));
            }
            wait_for_waiters(&word, 3);
            assert_eq!(atomic_notify(&word, 0), 0);
            assert_eq!(waiters_of(&word), 3);
            assert_eq!(atomic_notify(&word, 2), 2);
            assert_eq!(waiters_of(&word), 1);
            assert_eq!(atomic_notify(&word, usize::MAX), 1);
        });
    }

    #[test]
    fn views_of_both_widths_share_a_list() {
        let word = AtomicU64::new(0);
        // SAFETY: The low half of the word is a valid 4-byte word, as this is little-endian.
        let low_half = unsafe { AtomicU32::from_ptr(word.as_ptr().cast()) };
        std::thread::scope(|scope| {
            let waiter = scope.spawn(|| atomic_wait(WaitWord::Word64(&word), 0, None, &AgentWaiter::new()));
            wait_for_waiters(low_half, 1);
            assert_eq!(atomic_notify(low_half, 1), 1);
            assert_eq!(waiter.join().unwrap(), AtomicWaitResult::Woken);
        });
    }

    #[test]
    fn interrupts_end_waits_with_and_without_a_timeout() {
        for timeout in [None, Some(Duration::from_secs(600))] {
            let word = AtomicU32::new(0);
            let agent = AgentWaiter::new();
            std::thread::scope(|scope| {
                let waiter = scope.spawn(|| wait(&word, 0, timeout, &agent));
                wait_for_waiters(&word, 1);
                agent.interrupt();
                assert_eq!(waiter.join().unwrap(), AtomicWaitResult::Interrupted);
            });
            assert_eq!(waiters_of(&word), 0);
        }
    }

    #[test]
    fn notifies_pass_over_interrupted_agents_that_did_not_leave_yet() {
        let word = AtomicU32::new(0);
        let interrupted_agent = AgentWaiter::new();
        let waiting_agent = AgentWaiter::new();
        // NB: The lock stops the interrupted agent from leaving, as if its thread did not run yet.
        let mut held_lists = lists();
        let address = word.as_ptr().addr();
        for waiter in [&interrupted_agent, &waiting_agent] {
            held_lists.push_back(Waiting {
                address,
                waiter: Arc::clone(waiter),
            });
        }
        interrupted_agent.is_interrupted.store(true, Ordering::SeqCst);
        drop(held_lists);
        assert_eq!(atomic_notify(&word, 1), 1);
        assert!(waiting_agent.was_notified.load(Ordering::Relaxed));
        assert!(!interrupted_agent.was_notified.load(Ordering::Relaxed));
        assert_eq!(waiters_of(&word), 0);
    }

    #[test]
    fn interrupted_agents_do_not_wait_again() {
        let word = AtomicU32::new(0);
        let agent = AgentWaiter::new();
        agent.interrupt();
        assert_eq!(wait(&word, 0, None, &agent), AtomicWaitResult::Interrupted);
        assert_eq!(wait(&word, 1, None, &agent), AtomicWaitResult::Interrupted);
    }

    #[test]
    fn interrupts_reach_waits_that_are_just_starting() {
        for _ in 0..2000 {
            let word = AtomicU32::new(0);
            let agent = AgentWaiter::new();
            std::thread::scope(|scope| {
                let waiter = scope.spawn(|| wait(&word, 0, None, &agent));
                agent.interrupt();
                assert_eq!(waiter.join().unwrap(), AtomicWaitResult::Interrupted);
            });
        }
    }

    #[test]
    fn interrupts_leave_the_other_waiters_of_a_word_alone() {
        let word = AtomicU32::new(0);
        let interrupted_agent = AgentWaiter::new();
        let woken = AtomicUsize::new(0);
        std::thread::scope(|scope| {
            let first = scope.spawn(|| wait(&word, 0, None, &interrupted_agent));
            wait_for_waiters(&word, 1);
            let second = scope.spawn(|| {
                let result = wait(&word, 0, None, &AgentWaiter::new());
                woken.fetch_add(1, Ordering::SeqCst);
                result
            });
            wait_for_waiters(&word, 2);
            interrupted_agent.interrupt();
            assert_eq!(first.join().unwrap(), AtomicWaitResult::Interrupted);
            std::thread::sleep(TIME_TO_SLEEP);
            assert_eq!(woken.load(Ordering::SeqCst), 0);
            assert_eq!(waiters_of(&word), 1);
            // NB: The interrupted agent gave its place up, so nothing is lost to it.
            assert_eq!(atomic_notify(&word, 1), 1);
            assert_eq!(second.join().unwrap(), AtomicWaitResult::Woken);
        });
    }

    #[test]
    fn every_agent_that_a_notify_counts_wakes_whatever_the_order_on_the_list() {
        let word = AtomicU32::new(0);
        std::thread::scope(|scope| {
            let waiters: Vec<_> = (0..64)
                .map(|_| scope.spawn(|| wait(&word, 0, None, &AgentWaiter::new())))
                .collect();
            wait_for_waiters(&word, waiters.len());
            for _ in 0..waiters.len() {
                assert_eq!(atomic_notify(&word, 1), 1);
            }
            assert_eq!(atomic_notify(&word, 1), 0);
            for waiter in waiters {
                assert_eq!(waiter.join().unwrap(), AtomicWaitResult::Woken);
            }
        });
    }

    #[test]
    fn many_agents_can_wait_on_one_word_and_be_interrupted_one_by_one() {
        // NB: More agents than the 31 interrupt slots that the futex path had.
        let word = AtomicU32::new(0);
        let agents: Vec<_> = (0..64).map(|_| AgentWaiter::new()).collect();
        std::thread::scope(|scope| {
            // NB: One at a time, as a notify wakes the first on the list, which the loop below expects to be this order.
            let waiters: Vec<_> = agents
                .iter()
                .enumerate()
                .map(|(index, agent)| {
                    let word = &word;
                    let waiter = scope.spawn(move || wait(word, 0, None, agent));
                    wait_for_waiters(word, index + 1);
                    waiter
                })
                .collect();
            for (index, (agent, waiter)) in agents.iter().zip(waiters).enumerate() {
                if index % 2 == 0 {
                    agent.interrupt();
                    assert_eq!(waiter.join().unwrap(), AtomicWaitResult::Interrupted);
                } else {
                    assert_eq!(atomic_notify(&word, 1), 1);
                    assert_eq!(waiter.join().unwrap(), AtomicWaitResult::Woken);
                }
            }
        });
    }
}
