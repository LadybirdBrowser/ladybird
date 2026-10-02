/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibCore/Futex.cpp: waiting on and waking a word of memory, which Atomics.wait and Atomics.notify use.

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use core::time::Duration;
use std::time::Instant;

/// Core::AtomicWaitResult.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AtomicWaitResult {
    Woken,
    NotEqual,
    TimedOut,
}

fn read_word(address: *mut u8, size: usize) -> u64 {
    // SAFETY: The caller passes the aligned word of a typed array element, which is only accessed atomically.
    unsafe {
        if size >= 8 {
            return AtomicU64::from_ptr(address.cast()).load(Ordering::SeqCst);
        }
        u64::from(AtomicU32::from_ptr(address.cast()).load(Ordering::SeqCst))
    }
}

// Whether the word at address currently equals expected, masked to the wait size (Atomics.wait uses 4-byte or 8-byte
// words). A sequentially-consistent read, matching the memory model's read for the wait comparison.
fn word_equals(address: *mut u8, expected: u64, size: usize) -> bool {
    let mask = if size >= 8 { u64::MAX } else { u64::from(u32::MAX) };
    (read_word(address, size) & mask) == (expected & mask)
}

// Fallback where there's no cross-process wait primitive: Poll for a change to the word. That observes value changes
// rather than notifications — which matches the lock/futex protocol Emscripten uses (the waker writes the word before
// notifying). The gap is a notify that leaves the word unchanged: It wakes no poll-waiter, and the paired atomic_notify
// reports zero woken. An untimed wait whose only wake would be such a notify therefore never returns.
fn poll_wait(address: *mut u8, expected: u64, size: usize, timeout: Option<Duration>) -> AtomicWaitResult {
    if !word_equals(address, expected, size) {
        return AtomicWaitResult::NotEqual;
    }

    let start = Instant::now();
    loop {
        std::thread::sleep(Duration::from_micros(200));
        if !word_equals(address, expected, size) {
            return AtomicWaitResult::Woken;
        }
        if timeout.is_some_and(|timeout| start.elapsed() >= timeout) {
            return AtomicWaitResult::TimedOut;
        }
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
pub fn atomic_wait(address: *mut u8, expected: u64, size: usize, timeout: Option<Duration>) -> AtomicWaitResult {
    // Compare the value in userspace first. os_sync_wait_on_address reports a value mismatch as a *successful* return
    // (it has no EAGAIN). So, without this, the "not-equal" result would be unreachable; it also faults the shared page
    // in — avoiding a transient first-touch EFAULT below. A store landing between this read and the kernel's own
    // comparison is reported as a wake, rather than "not-equal" — a nanoseconds-wide window inherent to the os_sync API
    // (Linux's kernel futex and the poll fallback don't have it).
    if !word_equals(address, expected, size) {
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
                        address.cast(),
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
                        address.cast(),
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
                if !word_equals(address, expected, size) {
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
            // os_sync couldn't key this memory (rare once faulted in above); fall back to polling for a change.
            break;
        }
    }
    if let Some(deadline) = deadline {
        let remaining = deadline.saturating_duration_since(Instant::now());
        return poll_wait(address, expected, size, Some(remaining));
    }
    poll_wait(address, expected, size, timeout)
}

#[cfg(target_os = "macos")]
pub fn atomic_notify(address: *mut u8, _size: usize, max_count: usize) -> usize {
    // Waiters key on the low 32-bit half — so, wake with size 4 regardless of the notifying view's element size. That
    // lets a 4-byte notify wake an 8-byte waiter (and vice versa) at the same address — which the spec's single per-
    // address WaiterList requires.
    let Some(functions) = os_sync::functions() else {
        // The poll_wait fallback observes value changes directly — so no explicit wake is required.
        return 0;
    };
    // os_sync wakes a single waiter at a time. So, wake up to max_count of them — stopping once there are none left.
    // ENOENT is that stop: it means the wait set is empty.
    let mut woken = 0;
    while woken < max_count {
        // SAFETY: The address is the aligned word of a shared typed array element.
        let result =
            unsafe { (functions.wake_by_address_any)(address.cast(), 4, os_sync::OS_SYNC_WAKE_BY_ADDRESS_SHARED) };
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
pub fn atomic_wait(address: *mut u8, expected: u64, size: usize, timeout: Option<Duration>) -> AtomicWaitResult {
    // Compare the value in userspace first — so a zero or already-elapsed timeout still yields "not-equal" (which the
    // spec orders before "timed-out"), and so 8-byte waits and the poll fallback share one compare.
    if !word_equals(address, expected, size) {
        return AtomicWaitResult::NotEqual;
    }

    // Atomics.wait only ever uses size 4 or 8 (ValidateIntegerTypedArray); keep the poll fallback as a guard.
    if size != 4 && size != 8 {
        return poll_wait(address, expected, size, timeout);
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
                address,
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
pub fn atomic_notify(address: *mut u8, _size: usize, max_count: usize) -> usize {
    // FUTEX_WAKE wakes waiters keyed on the low 32-bit half at 'address'. That covers both 4-byte and 8-byte waiters
    // (8-byte waits key on the low half too), and returns the exact number of waiters woken.
    let count = i32::try_from(max_count).unwrap_or(i32::MAX);
    // SAFETY: The address is the aligned word of a shared typed array element.
    let result = unsafe {
        libc::syscall(
            libc::SYS_futex,
            address,
            libc::FUTEX_WAKE,
            count,
            core::ptr::null::<libc::timespec>(),
            core::ptr::null::<u32>(),
            0,
        )
    };
    usize::try_from(result).unwrap_or(0)
}

// No native wait/wake primitive is wired up on this platform. Poll for a change to the word, instead of reporting
// "not-equal" unconditionally — which would make every Atomics.wait return immediately, and spin its caller.
#[cfg(not(any(target_os = "macos", target_os = "linux")))]
pub fn atomic_wait(address: *mut u8, expected: u64, size: usize, timeout: Option<Duration>) -> AtomicWaitResult {
    poll_wait(address, expected, size, timeout)
}

#[cfg(not(any(target_os = "macos", target_os = "linux")))]
pub fn atomic_notify(_address: *mut u8, _size: usize, _max_count: usize) -> usize {
    // The waiters poll for the value change the waker has already made — so there's nothing to signal.
    0
}
