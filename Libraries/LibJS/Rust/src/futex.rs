/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibCore/Futex.cpp: waiting on and waking a word of memory, which Atomics.wait and Atomics.notify use.

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use core::time::Duration;
#[cfg(any(target_os = "macos", target_os = "linux"))]
use std::time::Instant;

/// Core::AtomicWaitResult.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AtomicWaitResult {
    Woken,
    NotEqual,
    TimedOut,
}

/// The word of a shared typed array element that Atomics.wait waits on, which is 4 or 8 bytes wide.
#[derive(Clone, Copy)]
pub(crate) enum WaitWord<'a> {
    Word32(&'a AtomicU32),
    Word64(&'a AtomicU64),
}

impl WaitWord<'_> {
    /// The address waiters key on, which is that of the low 32-bit half of the word.
    fn address(self) -> *mut u32 {
        match self {
            WaitWord::Word32(word) => word.as_ptr(),
            WaitWord::Word64(word) => word.as_ptr().cast(),
        }
    }

    /// A sequentially-consistent read, matching the memory model's read for the wait comparison.
    fn load(self) -> u64 {
        match self {
            WaitWord::Word32(word) => u64::from(word.load(Ordering::SeqCst)),
            WaitWord::Word64(word) => word.load(Ordering::SeqCst),
        }
    }
}

// Whether the word currently equals expected, masked to the wait size.
fn word_equals(word: WaitWord<'_>, expected: u64) -> bool {
    let mask = match word {
        WaitWord::Word32(_) => u64::from(u32::MAX),
        WaitWord::Word64(_) => u64::MAX,
    };
    (word.load() & mask) == (expected & mask)
}

// Fallback where there's no wait/wake primitive of the operating system's: Each waiter waits on a condition variable of
// its own, in a list per address that atomic_notify() wakes waiters of in the order they began waiting. These lists only
// reach the waiters of this process. So, a waiter also polls for a change to the word, which observes the wakes of other
// processes that write the word before notifying (as the lock/futex protocol Emscripten uses does). A notify from
// another process that leaves the word unchanged wakes no waiter here.
#[cfg(not(target_os = "linux"))]
mod waiter_list {
    use super::{AtomicWaitResult, WaitWord, word_equals};
    use core::sync::atomic::{AtomicBool, Ordering};
    use core::time::Duration;
    use std::collections::{HashMap, VecDeque};
    use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};
    use std::time::Instant;

    const POLL_INTERVAL: Duration = Duration::from_micros(200);

    struct Waiter {
        woken: AtomicBool,
        condition: Condvar,
    }

    type Lists = HashMap<usize, VecDeque<Arc<Waiter>>>;

    fn lists() -> MutexGuard<'static, Lists> {
        static LISTS: OnceLock<Mutex<Lists>> = OnceLock::new();
        LISTS
            .get_or_init(Mutex::default)
            .lock()
            .unwrap_or_else(|error| error.into_inner())
    }

    fn remove(lists: &mut Lists, address: usize, waiter: &Arc<Waiter>) {
        let Some(list) = lists.get_mut(&address) else {
            return;
        };
        list.retain(|other| !Arc::ptr_eq(other, waiter));
        if list.is_empty() {
            lists.remove(&address);
        }
    }

    pub(super) fn wait(word: WaitWord<'_>, expected: u64, timeout: Option<Duration>) -> AtomicWaitResult {
        let address = word.address().addr();
        let deadline = timeout.and_then(|timeout| Instant::now().checked_add(timeout));
        // NB: The word is compared while the lists are locked, so a notify that follows a store to it finds this
        //     waiter in its list if the comparison did not see the store.
        let mut lists = lists();
        if !word_equals(word, expected) {
            return AtomicWaitResult::NotEqual;
        }
        let waiter = Arc::new(Waiter {
            woken: AtomicBool::new(false),
            condition: Condvar::new(),
        });
        lists.entry(address).or_default().push_back(waiter.clone());
        loop {
            let interval = match deadline {
                Some(deadline) => {
                    let remaining = deadline.saturating_duration_since(Instant::now());
                    if remaining.is_zero() {
                        remove(&mut lists, address, &waiter);
                        return AtomicWaitResult::TimedOut;
                    }
                    remaining.min(POLL_INTERVAL)
                }
                None => POLL_INTERVAL,
            };
            lists = waiter
                .condition
                .wait_timeout(lists, interval)
                .unwrap_or_else(|error| error.into_inner())
                .0;
            // A notify removed the waiter from its list before setting this.
            if waiter.woken.load(Ordering::Relaxed) {
                return AtomicWaitResult::Woken;
            }
            if !word_equals(word, expected) {
                remove(&mut lists, address, &waiter);
                return AtomicWaitResult::Woken;
            }
        }
    }

    pub(super) fn notify(address: *mut u32, max_count: usize) -> usize {
        let mut lists = lists();
        let address = address.addr();
        let Some(list) = lists.get_mut(&address) else {
            return 0;
        };
        let count = max_count.min(list.len());
        for waiter in list.drain(..count) {
            waiter.woken.store(true, Ordering::Relaxed);
            waiter.condition.notify_one();
        }
        if list.is_empty() {
            lists.remove(&address);
        }
        count
    }
}

#[cfg(target_os = "macos")]
mod os_sync {
    use core::ffi::{c_int, c_void};
    use std::sync::OnceLock;

    pub const OS_SYNC_WAIT_ON_ADDRESS_SHARED: u32 = 0x1;
    pub const OS_SYNC_WAKE_BY_ADDRESS_SHARED: u32 = 0x1;
    pub const OS_CLOCK_MACH_ABSOLUTE_TIME: u32 = 32;

    pub type WaitOnAddress = unsafe extern "C" fn(*mut c_void, u64, usize, u32) -> c_int;
    pub type WaitOnAddressWithTimeout = unsafe extern "C" fn(*mut c_void, u64, usize, u32, u32, u64) -> c_int;
    pub type WakeByAddressAny = unsafe extern "C" fn(*mut c_void, usize, u32) -> c_int;

    /// The os_sync functions, which exist from macOS 14.4 on, looked up at runtime as __builtin_available() checks for
    /// them.
    pub struct Functions {
        pub wait_on_address: WaitOnAddress,
        pub wait_on_address_with_timeout: WaitOnAddressWithTimeout,
        pub wake_by_address_any: WakeByAddressAny,
    }

    pub fn functions() -> Option<&'static Functions> {
        static FUNCTIONS: OnceLock<Option<Functions>> = OnceLock::new();
        FUNCTIONS
            .get_or_init(|| {
                let lookup = |name: &core::ffi::CStr| {
                    // SAFETY: Looks up a symbol by its NUL-terminated name.
                    let symbol = unsafe { libc::dlsym(libc::RTLD_DEFAULT, name.as_ptr()) };
                    (!symbol.is_null()).then_some(symbol)
                };
                let wait_on_address = lookup(c"os_sync_wait_on_address")?;
                let wait_on_address_with_timeout = lookup(c"os_sync_wait_on_address_with_timeout")?;
                let wake_by_address_any = lookup(c"os_sync_wake_by_address_any")?;
                // SAFETY: The symbols are the libSystem functions with these signatures.
                unsafe {
                    Some(Functions {
                        wait_on_address: core::mem::transmute::<*mut c_void, WaitOnAddress>(wait_on_address),
                        wait_on_address_with_timeout: core::mem::transmute::<*mut c_void, WaitOnAddressWithTimeout>(
                            wait_on_address_with_timeout,
                        ),
                        wake_by_address_any: core::mem::transmute::<*mut c_void, WakeByAddressAny>(wake_by_address_any),
                    })
                }
            })
            .as_ref()
    }
}

#[cfg(target_os = "macos")]
fn errno() -> i32 {
    std::io::Error::last_os_error().raw_os_error().unwrap_or(0)
}

#[cfg(target_os = "macos")]
pub(crate) fn atomic_wait(word: WaitWord<'_>, expected: u64, timeout: Option<Duration>) -> AtomicWaitResult {
    // Compare the value in userspace first. os_sync_wait_on_address reports a value mismatch as a *successful* return
    // (it has no EAGAIN). So, without this, the "not-equal" result would be unreachable; it also faults the shared page
    // in — avoiding a transient first-touch EFAULT below. A store landing between this read and the kernel's own
    // comparison is reported as a wake, rather than "not-equal" — a nanoseconds-wide window inherent to the os_sync API
    // (Linux's kernel futex and the waiter lists don't have it).
    if !word_equals(word, expected) {
        return AtomicWaitResult::NotEqual;
    }

    // Anchor the timeout to a fixed deadline — so that nothing restarts it. Both an EINTR retry and the fallback to
    // polling below continue with only the time that's left.
    let deadline = timeout.and_then(|timeout| Instant::now().checked_add(timeout));

    if let Some(functions) = os_sync::functions() {
        // os_sync_wait_on_address makes the element size part of the wait/wake key. Wait on the low 32-bit half (size
        // 4) for both 4-byte and 8-byte waits: the full-width userspace pre-compare above already rejected any full-
        // value mismatch, so the low half only has to close the lost-wake race at entry — and normalizing the size lets
        // 4-byte and 8-byte waiters at one address share kernel state (a single spec WaiterList) and cross-wake.
        loop {
            let result = if let Some(deadline) = deadline {
                let remaining = deadline.saturating_duration_since(Instant::now());
                if remaining.is_zero() {
                    return AtomicWaitResult::TimedOut;
                }
                // SAFETY: The address is the aligned word of a shared typed array element.
                unsafe {
                    (functions.wait_on_address_with_timeout)(
                        word.address().cast(),
                        u64::from(expected as u32),
                        4,
                        os_sync::OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                        os_sync::OS_CLOCK_MACH_ABSOLUTE_TIME,
                        u64::try_from(remaining.as_nanos()).unwrap_or(u64::MAX),
                    )
                }
            } else {
                // SAFETY: As above.
                unsafe {
                    (functions.wait_on_address)(
                        word.address().cast(),
                        u64::from(expected as u32),
                        4,
                        os_sync::OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                    )
                }
            };

            if result >= 0 {
                return AtomicWaitResult::Woken;
            }
            let error = errno();
            if error == libc::EINTR {
                // os_sync reports a value mismatch as a successful wake — so retrying with the original expected would
                // turn a store that landed while we slept into a spurious "ok". Re-read first: If the word changed,
                // report "not-equal" (as if the critical section were entered late) — matching the Linux EAGAIN path.
                if !word_equals(word, expected) {
                    return AtomicWaitResult::NotEqual;
                }
                continue;
            }
            if error == libc::ETIMEDOUT {
                return AtomicWaitResult::TimedOut;
            }
            if error == libc::EAGAIN {
                return AtomicWaitResult::NotEqual;
            }
            // os_sync couldn't key this memory (rare once faulted in above); fall back to the waiter lists.
            break;
        }
    }
    let remaining = deadline.map(|deadline| deadline.saturating_duration_since(Instant::now()));
    waiter_list::wait(word, expected, remaining.or(timeout))
}

#[cfg(target_os = "macos")]
pub(crate) fn atomic_notify(word: &AtomicU32, max_count: usize) -> usize {
    // Waiters key on the low 32-bit half — so, wake with size 4 regardless of the notifying view's element size. That
    // lets a 4-byte notify wake an 8-byte waiter (and vice versa) at the same address — which the spec's single per-
    // address WaiterList requires.
    // NB: Waiters that os_sync could not serve wait in the fallback's lists.
    let mut woken = waiter_list::notify(word.as_ptr(), max_count);
    let Some(functions) = os_sync::functions() else {
        return woken;
    };
    // os_sync wakes a single waiter at a time. So, wake up to max_count of them — stopping once there are none left.
    // ENOENT is that stop: it means the wait set is empty.
    while woken < max_count {
        // SAFETY: The address is the aligned word of a shared typed array element.
        let result = unsafe {
            (functions.wake_by_address_any)(word.as_ptr().cast(), 4, os_sync::OS_SYNC_WAKE_BY_ADDRESS_SHARED)
        };
        if result == 0 {
            woken += 1;
            continue;
        }
        let error = errno();
        if error != libc::ENOENT {
            eprintln!("Core::atomic_notify: os_sync_wake_by_address_any failed with errno {error}");
        }
        break;
    }
    woken
}

#[cfg(target_os = "linux")]
pub(crate) fn atomic_wait(word: WaitWord<'_>, expected: u64, timeout: Option<Duration>) -> AtomicWaitResult {
    // Compare the value in userspace first — so a zero or already-elapsed timeout still yields "not-equal" (which the
    // spec orders before "timed-out"), and so 8-byte waits share one compare.
    if !word_equals(word, expected) {
        return AtomicWaitResult::NotEqual;
    }

    // Anchor the timeout to a fixed deadline — so EINTR retries reduce the remaining time, rather than restarting the
    // full timeout (FUTEX_WAIT takes a relative timeout that the kernel doesn't update across a retry).
    let deadline = timeout.and_then(|timeout| Instant::now().checked_add(timeout));

    loop {
        let mut relative_timeout = libc::timespec { tv_sec: 0, tv_nsec: 0 };
        let mut timeout_pointer: *const libc::timespec = core::ptr::null();
        if let Some(deadline) = deadline {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return AtomicWaitResult::TimedOut;
            }
            relative_timeout.tv_sec = remaining.as_secs() as libc::time_t;
            relative_timeout.tv_nsec = libc::c_long::from(remaining.subsec_nanos());
            timeout_pointer = &raw const relative_timeout;
        }

        // A shared (non-private) FUTEX_WAIT keys on the underlying page — so it coordinates across processes.
        // SAFETY: The address is the aligned word of a shared typed array element.
        let result = unsafe {
            libc::syscall(
                libc::SYS_futex,
                word.address(),
                libc::FUTEX_WAIT,
                expected as u32,
                timeout_pointer,
                core::ptr::null::<u32>(),
                0,
            )
        };
        if result == 0 {
            return AtomicWaitResult::Woken;
        }
        let error = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
        if error == libc::EINTR {
            continue;
        }
        if error == libc::ETIMEDOUT {
            return AtomicWaitResult::TimedOut;
        }
        // EAGAIN means the value at the address no longer equals expected.
        return AtomicWaitResult::NotEqual;
    }
}

#[cfg(target_os = "linux")]
pub(crate) fn atomic_notify(word: &AtomicU32, max_count: usize) -> usize {
    // FUTEX_WAKE wakes a waiter even when the requested count is zero.
    if max_count == 0 {
        return 0;
    }

    // FUTEX_WAKE wakes waiters keyed on the low 32-bit half at the word's address. That covers both 4-byte and 8-byte waiters
    // (8-byte waits key on the low half too), and returns the exact number of waiters woken.
    let count = i32::try_from(max_count).unwrap_or(i32::MAX);
    // SAFETY: The address is the aligned word of a shared typed array element.
    let result = unsafe {
        libc::syscall(
            libc::SYS_futex,
            word.as_ptr(),
            libc::FUTEX_WAKE,
            count,
            core::ptr::null::<libc::timespec>(),
            core::ptr::null::<u32>(),
            0,
        )
    };
    usize::try_from(result).unwrap_or(0)
}

// No native wait/wake primitive is wired up on this platform.
#[cfg(not(any(target_os = "macos", target_os = "linux")))]
pub(crate) fn atomic_wait(word: WaitWord<'_>, expected: u64, timeout: Option<Duration>) -> AtomicWaitResult {
    waiter_list::wait(word, expected, timeout)
}

#[cfg(not(any(target_os = "macos", target_os = "linux")))]
pub(crate) fn atomic_notify(word: &AtomicU32, max_count: usize) -> usize {
    waiter_list::notify(word.as_ptr(), max_count)
}

#[cfg(all(test, target_os = "linux"))]
mod linux_tests {
    use super::*;

    #[test]
    fn zero_count_does_not_wake_waiters() {
        let word = AtomicU64::new(0);
        // SAFETY: The storage is aligned for AtomicU32, and neither view writes to it. Both wait widths use this
        // address as their futex key.
        let notify_word = unsafe { &*word.as_ptr().cast::<AtomicU32>() };
        for wait_word in [WaitWord::Word32(notify_word), WaitWord::Word64(&word)] {
            std::thread::scope(|scope| {
                let waiter = scope.spawn(|| atomic_wait(wait_word, 0, Some(Duration::from_millis(100))));
                while !waiter.is_finished() {
                    assert_eq!(atomic_notify(notify_word, 0), 0);
                    std::thread::yield_now();
                }
                assert_eq!(waiter.join().unwrap(), AtomicWaitResult::TimedOut);
            });
        }
    }
}

#[cfg(all(test, not(target_os = "linux")))]
mod tests {
    use super::*;

    #[test]
    fn waiter_lists_wake_waiters_of_a_word_that_did_not_change() {
        static WORD: AtomicU32 = AtomicU32::new(0);
        let waiter = std::thread::spawn(|| waiter_list::wait(WaitWord::Word32(&WORD), 0, None));
        // The waiter is in its list once a notify wakes it.
        while waiter_list::notify(WORD.as_ptr(), 1) == 0 {
            std::thread::yield_now();
        }
        assert_eq!(waiter.join().unwrap(), AtomicWaitResult::Woken);
        assert_eq!(WORD.load(Ordering::SeqCst), 0);
    }

    #[test]
    fn waiter_lists_time_out() {
        static WORD: AtomicU32 = AtomicU32::new(0);
        let result = waiter_list::wait(WaitWord::Word32(&WORD), 0, Some(Duration::from_millis(1)));
        assert_eq!(result, AtomicWaitResult::TimedOut);
        assert_eq!(waiter_list::notify(WORD.as_ptr(), 1), 0);
    }
}
