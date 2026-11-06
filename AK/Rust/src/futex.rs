/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibCore/Futex.cpp: waiting on and waking a word of memory, which Atomics.wait, Atomics.notify and their
//! WebAssembly counterparts use.

#[cfg(any(target_os = "macos", target_os = "linux"))]
use core::sync::atomic::AtomicPtr;
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use core::time::Duration;
#[cfg(target_os = "macos")]
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

/// Stops the waits of one agent for good, from any thread. Only a notify ends a wait otherwise, and a wait that ends
/// and starts over would lose its place among the waiters, and any notify that came in between.
///
/// A thread that sleeps in the kernel cannot be woken alone: a wake reaches the waiters of a word. On Linux, a waiter
/// also sleeps on a bit of its interrupt, which is what its interrupt wakes, but only 31 bits go around. On macOS,
/// every waiter of the word wakes. A waiter that such a wake was not for goes back to sleep (see
/// was_woken_by_an_interrupt()).
pub struct WaitInterrupt {
    is_requested: AtomicBool,
    /// The word a thread of this agent sleeps on, for as long as it may be asleep in the kernel.
    #[cfg(any(target_os = "macos", target_os = "linux"))]
    sleeping_on: AtomicPtr<u32>,
    /// Where in INTERRUPT_WAKES this interrupt wakes.
    #[cfg(any(target_os = "macos", target_os = "linux"))]
    wake_slot: usize,
    /// The thread of this agent that waits in the waiter lists.
    #[cfg(any(test, not(target_os = "linux")))]
    waiter: std::sync::Mutex<Option<std::sync::Arc<waiter_list::Waiter>>>,
}

/// The bit of a wake that a notify is, which every waiter sleeps on.
#[cfg(target_os = "linux")]
const NOTIFY_BIT: u32 = 1;

/// How many interrupts can wake waiters without that reaching a waiter of another one.
#[cfg(target_os = "linux")]
const INTERRUPT_WAKE_SLOTS: usize = 31;
#[cfg(target_os = "macos")]
const INTERRUPT_WAKE_SLOTS: usize = 1;

/// The word that an interrupt is waking, by wake slot, which tells a waiter that a wake may not have been for it.
#[cfg(any(target_os = "macos", target_os = "linux"))]
static INTERRUPT_WAKES: [AtomicPtr<u32>; INTERRUPT_WAKE_SLOTS] =
    [const { AtomicPtr::new(core::ptr::null_mut()) }; INTERRUPT_WAKE_SLOTS];

/// How long an interrupt waits for a thread to stop sleeping before it wakes the word again.
#[cfg(any(target_os = "macos", target_os = "linux"))]
const INTERRUPT_WAKE_INTERVAL: Duration = Duration::from_micros(50);

impl Default for WaitInterrupt {
    fn default() -> Self {
        Self::new()
    }
}

impl WaitInterrupt {
    pub fn new() -> Self {
        #[cfg(any(target_os = "macos", target_os = "linux"))]
        static NEXT_WAKE_SLOT: AtomicU32 = AtomicU32::new(0);
        Self {
            is_requested: AtomicBool::new(false),
            #[cfg(any(target_os = "macos", target_os = "linux"))]
            sleeping_on: AtomicPtr::new(core::ptr::null_mut()),
            // NB: A word can have waiters in other processes. Starting at the process ID spreads those over the bits as
            //     well, which is all this can do for them: their waits are out of reach of INTERRUPT_WAKES.
            #[cfg(any(target_os = "macos", target_os = "linux"))]
            wake_slot: std::process::id().wrapping_add(NEXT_WAKE_SLOT.fetch_add(1, Ordering::Relaxed)) as usize
                % INTERRUPT_WAKE_SLOTS,
            #[cfg(any(test, not(target_os = "linux")))]
            waiter: std::sync::Mutex::default(),
        }
    }

    /// Whether interrupt() was called.
    pub fn is_requested(&self) -> bool {
        self.is_requested.load(Ordering::SeqCst)
    }

    /// Ends the wait of this agent's thread with AtomicWaitResult::Interrupted, and every later one at once. Blocks until
    /// that thread is awake.
    pub fn interrupt(&self) {
        self.is_requested.store(true, Ordering::SeqCst);
        #[cfg(any(test, not(target_os = "linux")))]
        waiter_list::wake_interrupted(self);
        #[cfg(any(target_os = "macos", target_os = "linux"))]
        self.wake_sleeping_thread();
    }

    // NB: The kernel compares the word before a thread sleeps, but it cannot look at is_requested. A thread that looked at
    //     it just before it got set is about to sleep, and a wake would go past it. So the wake has to go on until the
    //     thread is known to be awake, which also covers that thread going away: a word that stopped being one only
    //     gets a wake that finds nothing, or wakes a waiter that has to cope with this anyway.
    #[cfg(any(target_os = "macos", target_os = "linux"))]
    fn wake_sleeping_thread(&self) {
        let interrupt_wake = &INTERRUPT_WAKES[self.wake_slot];
        let mut did_wake = false;
        loop {
            let word = self.sleeping_on.load(Ordering::SeqCst);
            if word.is_null() {
                break;
            }
            interrupt_wake.store(word, Ordering::SeqCst);
            did_wake = true;
            self.wake_word(word);
            std::thread::sleep(INTERRUPT_WAKE_INTERVAL);
        }
        if did_wake {
            interrupt_wake.store(core::ptr::null_mut(), Ordering::SeqCst);
        }
    }

    /// Whether a thread that this interrupt lets sleep on `word` was woken by an interrupt, of this agent or another.
    ///
    /// NB: Telling those apart from a notify only works for an interrupt of this process. A waiter in another process
    ///     that an interrupt wakes takes it for a notify. And a notify that arrives right while an interrupt wakes
    ///     the word is taken for that interrupt's wake, if it is one for the same bit, so that it gets lost.
    #[cfg(any(target_os = "macos", target_os = "linux"))]
    fn was_woken_by_an_interrupt(&self, word: *mut u32) -> bool {
        INTERRUPT_WAKES[self.wake_slot].load(Ordering::SeqCst) == word
    }

    /// Lets interrupt() know that this agent's thread may sleep on `word` until the result goes away. The thread has to
    /// look at is_requested() after this.
    #[cfg(any(target_os = "macos", target_os = "linux"))]
    fn will_sleep_on(&self, word: *mut u32) -> SleepingOn<'_> {
        self.sleeping_on.store(word, Ordering::SeqCst);
        SleepingOn(self)
    }

    #[cfg(target_os = "linux")]
    fn wake_bit(&self) -> u32 {
        NOTIFY_BIT << (self.wake_slot + 1)
    }

    #[cfg(target_os = "linux")]
    fn wake_word(&self, word: *mut u32) {
        wake_waiters_on_bit(word, i32::MAX, self.wake_bit());
    }

    #[cfg(target_os = "macos")]
    fn wake_word(&self, word: *mut u32) {
        if let Some(functions) = os_sync::functions() {
            // SAFETY: A wake of an address only compares it with those that waiters sleep on.
            unsafe {
                (functions.wake_by_address_all)(word.cast(), 4, os_sync::OS_SYNC_WAKE_BY_ADDRESS_SHARED);
            }
        }
    }
}

#[cfg(any(target_os = "macos", target_os = "linux"))]
struct SleepingOn<'a>(&'a WaitInterrupt);

#[cfg(any(target_os = "macos", target_os = "linux"))]
impl Drop for SleepingOn<'_> {
    fn drop(&mut self) {
        self.0.sleeping_on.store(core::ptr::null_mut(), Ordering::SeqCst);
    }
}

// Fallback where there's no wait/wake primitive of the operating system's: Each waiter waits on a condition variable of
// its own, in a list per address that atomic_notify() wakes waiters of in the order they began waiting. These lists only
// reach the waiters of this process. So, a waiter also polls for a change to the word, which observes the wakes of other
// processes that write the word before notifying (as the lock/futex protocol Emscripten uses does). A notify from
// another process that leaves the word unchanged wakes no waiter here.
#[cfg(any(test, not(target_os = "linux")))]
mod waiter_list {
    use super::{AtomicWaitResult, WaitInterrupt, WaitWord, word_equals};
    use core::sync::atomic::{AtomicBool, Ordering};
    use core::time::Duration;
    use std::collections::{HashMap, VecDeque};
    use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock, PoisonError};
    use std::time::Instant;

    const POLL_INTERVAL: Duration = Duration::from_micros(200);

    pub(super) struct Waiter {
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

    fn set_waiter_of_interrupt(interrupt: Option<&WaitInterrupt>, waiter: Option<Arc<Waiter>>) {
        if let Some(interrupt) = interrupt {
            *interrupt.waiter.lock().unwrap_or_else(PoisonError::into_inner) = waiter;
        }
    }

    /// Wakes the waiter of an interrupt that is already requested.
    pub(super) fn wake_interrupted(interrupt: &WaitInterrupt) {
        // NB: A waiter looks at the request and goes to sleep while the lists are locked, so this cannot go past it.
        let _lists = lists();
        if let Some(waiter) = &*interrupt.waiter.lock().unwrap_or_else(PoisonError::into_inner) {
            waiter.condition.notify_one();
        }
    }

    pub(super) fn wait(
        word: WaitWord<'_>,
        expected: u64,
        timeout: Option<Duration>,
        interrupt: Option<&WaitInterrupt>,
    ) -> AtomicWaitResult {
        let address = word.address().addr();
        let deadline = timeout.and_then(|timeout| Instant::now().checked_add(timeout));
        // NB: The word is compared while the lists are locked, so a notify that follows a store to it finds this
        //     waiter in its list if the comparison did not see the store.
        let mut lists = lists();
        if interrupt.is_some_and(WaitInterrupt::is_requested) {
            return AtomicWaitResult::Interrupted;
        }
        if !word_equals(word, expected) {
            return AtomicWaitResult::NotEqual;
        }
        let waiter = Arc::new(Waiter {
            woken: AtomicBool::new(false),
            condition: Condvar::new(),
        });
        lists.entry(address).or_default().push_back(waiter.clone());
        set_waiter_of_interrupt(interrupt, Some(waiter.clone()));
        let result = loop {
            let interval = match deadline {
                Some(deadline) => {
                    let remaining = deadline.saturating_duration_since(Instant::now());
                    if remaining.is_zero() {
                        remove(&mut lists, address, &waiter);
                        break AtomicWaitResult::TimedOut;
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
            if interrupt.is_some_and(WaitInterrupt::is_requested) {
                remove(&mut lists, address, &waiter);
                break AtomicWaitResult::Interrupted;
            }
            // A notify removed the waiter from its list before setting this.
            if waiter.woken.load(Ordering::Relaxed) {
                break AtomicWaitResult::Woken;
            }
            if !word_equals(word, expected) {
                remove(&mut lists, address, &waiter);
                break AtomicWaitResult::Woken;
            }
        };
        set_waiter_of_interrupt(interrupt, None);
        result
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
        pub wake_by_address_all: WakeByAddressAny,
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
                let wake_by_address_all = lookup(c"os_sync_wake_by_address_all")?;
                // SAFETY: The symbols are the libSystem functions with these signatures.
                unsafe {
                    Some(Functions {
                        wait_on_address: core::mem::transmute::<*mut c_void, WaitOnAddress>(wait_on_address),
                        wait_on_address_with_timeout: core::mem::transmute::<*mut c_void, WaitOnAddressWithTimeout>(
                            wait_on_address_with_timeout,
                        ),
                        wake_by_address_any: core::mem::transmute::<*mut c_void, WakeByAddressAny>(wake_by_address_any),
                        wake_by_address_all: core::mem::transmute::<*mut c_void, WakeByAddressAny>(wake_by_address_all),
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
pub fn atomic_wait(
    word: WaitWord<'_>,
    expected: u64,
    timeout: Option<Duration>,
    interrupt: Option<&WaitInterrupt>,
) -> AtomicWaitResult {
    if interrupt.is_some_and(WaitInterrupt::is_requested) {
        return AtomicWaitResult::Interrupted;
    }

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
        let _sleeping_on = interrupt.map(|interrupt| interrupt.will_sleep_on(word.address()));
        loop {
            if interrupt.is_some_and(WaitInterrupt::is_requested) {
                return AtomicWaitResult::Interrupted;
            }
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
                if !interrupt.is_some_and(|interrupt| interrupt.was_woken_by_an_interrupt(word.address())) {
                    return AtomicWaitResult::Woken;
                }
                // NB: This waiter may go back to sleep, and os_sync would report a value mismatch as a wake.
                if !word_equals(word, expected) && !interrupt.is_some_and(WaitInterrupt::is_requested) {
                    return AtomicWaitResult::NotEqual;
                }
                continue;
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
    waiter_list::wait(word, expected, remaining.or(timeout), interrupt)
}

#[cfg(target_os = "macos")]
pub fn atomic_notify(word: &AtomicU32, max_count: usize) -> usize {
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

// FUTEX_WAIT_BITSET takes a deadline, on the monotonic clock.
#[cfg(target_os = "linux")]
fn monotonic_deadline(timeout: Duration) -> libc::timespec {
    let mut now = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // SAFETY: The clock always exists, and `now` is a timespec.
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &raw mut now) };
    let nanoseconds = now.tv_nsec + libc::c_long::from(timeout.subsec_nanos() as i32);
    let seconds = libc::time_t::try_from(timeout.as_secs()).unwrap_or(libc::time_t::MAX);
    libc::timespec {
        tv_sec: now
            .tv_sec
            .saturating_add(seconds)
            .saturating_add(libc::time_t::from(nanoseconds >= 1_000_000_000)),
        tv_nsec: nanoseconds % 1_000_000_000,
    }
}

#[cfg(target_os = "linux")]
pub fn atomic_wait(
    word: WaitWord<'_>,
    expected: u64,
    timeout: Option<Duration>,
    interrupt: Option<&WaitInterrupt>,
) -> AtomicWaitResult {
    if interrupt.is_some_and(WaitInterrupt::is_requested) {
        return AtomicWaitResult::Interrupted;
    }

    // Compare the value in userspace first — so a zero or already-elapsed timeout still yields "not-equal" (which the
    // spec orders before "timed-out"), and so 8-byte waits share one compare.
    if !word_equals(word, expected) {
        return AtomicWaitResult::NotEqual;
    }

    // A fixed deadline — so that neither EINTR nor a wake that wasn't for this waiter restarts the timeout.
    let deadline = timeout.map(monotonic_deadline);
    let deadline_pointer = deadline.as_ref().map_or(core::ptr::null(), core::ptr::from_ref);
    let bits = NOTIFY_BIT | interrupt.map_or(0, WaitInterrupt::wake_bit);
    let _sleeping_on = interrupt.map(|interrupt| interrupt.will_sleep_on(word.address()));

    loop {
        if interrupt.is_some_and(WaitInterrupt::is_requested) {
            return AtomicWaitResult::Interrupted;
        }

        // A shared (non-private) futex keys on the underlying page — so it coordinates across processes.
        // SAFETY: The address is the aligned word of a shared typed array element.
        let result = unsafe {
            libc::syscall(
                libc::SYS_futex,
                word.address(),
                libc::FUTEX_WAIT_BITSET,
                expected as u32,
                deadline_pointer,
                core::ptr::null::<u32>(),
                bits,
            )
        };
        if result == 0 {
            if interrupt.is_some_and(|interrupt| interrupt.was_woken_by_an_interrupt(word.address())) {
                continue;
            }
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

// Wakes up to `count` waiters of the word with one of `bits` among those they sleep on, and returns how many it woke.
#[cfg(target_os = "linux")]
fn wake_waiters_on_bit(word: *mut u32, count: i32, bits: u32) -> usize {
    // SAFETY: Only the address of the word goes to the kernel, which gets nothing out of one that is no longer mapped.
    let result = unsafe {
        libc::syscall(
            libc::SYS_futex,
            word,
            libc::FUTEX_WAKE_BITSET,
            count,
            core::ptr::null::<libc::timespec>(),
            core::ptr::null::<u32>(),
            bits,
        )
    };
    usize::try_from(result).unwrap_or(0)
}

#[cfg(target_os = "linux")]
pub fn atomic_notify(word: &AtomicU32, max_count: usize) -> usize {
    // FUTEX_WAKE_BITSET wakes a waiter even when the requested count is zero.
    if max_count == 0 {
        return 0;
    }

    // This wakes waiters keyed on the low 32-bit half at the word's address. That covers both 4-byte and 8-byte waiters
    // (8-byte waits key on the low half too), and returns the exact number of waiters woken.
    wake_waiters_on_bit(word.as_ptr(), i32::try_from(max_count).unwrap_or(i32::MAX), NOTIFY_BIT)
}

// No native wait/wake primitive is wired up on this platform.
#[cfg(not(any(target_os = "macos", target_os = "linux")))]
pub fn atomic_wait(
    word: WaitWord<'_>,
    expected: u64,
    timeout: Option<Duration>,
    interrupt: Option<&WaitInterrupt>,
) -> AtomicWaitResult {
    waiter_list::wait(word, expected, timeout, interrupt)
}

#[cfg(not(any(target_os = "macos", target_os = "linux")))]
pub fn atomic_notify(word: &AtomicU32, max_count: usize) -> usize {
    waiter_list::notify(word.as_ptr(), max_count)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    /// Long enough for a thread that was just started to get to sleep.
    const TIME_TO_SLEEP: Duration = Duration::from_millis(30);

    type Wait = fn(&AtomicU32, u64, Option<Duration>, Option<&WaitInterrupt>) -> AtomicWaitResult;
    type Notify = fn(&AtomicU32, usize) -> usize;

    fn wait_in_kernel(
        word: &AtomicU32,
        expected: u64,
        timeout: Option<Duration>,
        interrupt: Option<&WaitInterrupt>,
    ) -> AtomicWaitResult {
        atomic_wait(WaitWord::Word32(word), expected, timeout, interrupt)
    }

    fn wait_in_list(
        word: &AtomicU32,
        expected: u64,
        timeout: Option<Duration>,
        interrupt: Option<&WaitInterrupt>,
    ) -> AtomicWaitResult {
        waiter_list::wait(WaitWord::Word32(word), expected, timeout, interrupt)
    }

    fn notify_in_list(word: &AtomicU32, max_count: usize) -> usize {
        waiter_list::notify(word.as_ptr(), max_count)
    }

    fn for_each_primitive(test: impl Fn(Wait, Notify)) {
        test(wait_in_kernel, atomic_notify);
        test(wait_in_list, notify_in_list);
    }

    #[test]
    fn notifies_wake_waiters_of_a_word_that_did_not_change() {
        for_each_primitive(|wait, notify| {
            let word = AtomicU32::new(0);
            std::thread::scope(|scope| {
                let waiter = scope.spawn(|| wait(&word, 0, None, None));
                // The waiter is among the waiters once a notify wakes it.
                while notify(&word, 1) == 0 {
                    std::thread::yield_now();
                }
                assert_eq!(waiter.join().unwrap(), AtomicWaitResult::Woken);
            });
        });
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn a_change_of_the_word_wakes_nobody_in_the_kernel() {
        let word = AtomicU32::new(0);
        std::thread::scope(|scope| {
            let waiter = scope.spawn(|| wait_in_kernel(&word, 0, Some(TIME_TO_SLEEP * 4), None));
            std::thread::sleep(TIME_TO_SLEEP);
            word.store(1, Ordering::SeqCst);
            assert_eq!(waiter.join().unwrap(), AtomicWaitResult::TimedOut);
        });
    }

    #[test]
    fn waits_time_out() {
        for_each_primitive(|wait, notify| {
            let word = AtomicU32::new(0);
            assert_eq!(
                wait(&word, 0, Some(Duration::from_millis(1)), None),
                AtomicWaitResult::TimedOut
            );
            assert_eq!(wait(&word, 0, Some(Duration::ZERO), None), AtomicWaitResult::TimedOut);
            assert_eq!(wait(&word, 1, Some(Duration::ZERO), None), AtomicWaitResult::NotEqual);
            assert_eq!(notify(&word, 1), 0);
        });
    }

    #[test]
    fn notifies_wake_in_the_order_of_the_waits_and_count_what_they_wake() {
        for_each_primitive(|wait, notify| {
            const WAITER_COUNT: usize = 5;
            let word = AtomicU32::new(0);
            let woken_count = AtomicUsize::new(0);
            let positions: [AtomicUsize; WAITER_COUNT] = Default::default();
            std::thread::scope(|scope| {
                for position in &positions {
                    let (word, woken_count) = (&word, &woken_count);
                    scope.spawn(move || {
                        assert_eq!(wait(word, 0, None, None), AtomicWaitResult::Woken);
                        position.store(woken_count.fetch_add(1, Ordering::SeqCst) + 1, Ordering::SeqCst);
                    });
                    std::thread::sleep(TIME_TO_SLEEP);
                }
                for (index, position) in positions.iter().take(2).enumerate() {
                    assert_eq!(notify(&word, 1), 1);
                    while position.load(Ordering::SeqCst) == 0 {
                        std::thread::yield_now();
                    }
                    assert_eq!(position.load(Ordering::SeqCst), index + 1);
                }
                assert_eq!(notify(&word, 0), 0);
                assert_eq!(notify(&word, usize::MAX), WAITER_COUNT - 2);
            });
            assert_eq!(notify(&word, usize::MAX), 0);
        });
    }

    #[test]
    fn interrupts_end_waits_with_and_without_a_timeout() {
        for_each_primitive(|wait, notify| {
            for timeout in [None, Some(Duration::from_secs(600))] {
                let word = AtomicU32::new(0);
                let interrupt = WaitInterrupt::new();
                std::thread::scope(|scope| {
                    let waiter = scope.spawn(|| wait(&word, 0, timeout, Some(&interrupt)));
                    std::thread::sleep(TIME_TO_SLEEP);
                    interrupt.interrupt();
                    assert_eq!(waiter.join().unwrap(), AtomicWaitResult::Interrupted);
                });
                assert_eq!(notify(&word, 1), 0);
            }
        });
    }

    #[test]
    fn interrupted_waits_end_at_once() {
        for_each_primitive(|wait, _| {
            let word = AtomicU32::new(0);
            let interrupt = WaitInterrupt::new();
            interrupt.interrupt();
            assert_eq!(wait(&word, 0, None, Some(&interrupt)), AtomicWaitResult::Interrupted);
            assert_eq!(wait(&word, 1, None, Some(&interrupt)), AtomicWaitResult::Interrupted);
        });
    }

    // An interrupt can come at any point of a wait, which is what a thread on its way to sleep is at risk from.
    #[test]
    fn interrupts_reach_waits_that_are_just_starting() {
        for_each_primitive(|wait, _| {
            for round in 0..2000 {
                let word = AtomicU32::new(0);
                let interrupt = WaitInterrupt::new();
                std::thread::scope(|scope| {
                    let waiter = scope.spawn(|| wait(&word, 0, None, Some(&interrupt)));
                    for _ in 0..(round % 200) * 20 {
                        core::hint::spin_loop();
                    }
                    interrupt.interrupt();
                    assert_eq!(waiter.join().unwrap(), AtomicWaitResult::Interrupted);
                });
            }
        });
    }

    #[test]
    fn interrupts_leave_the_other_waiters_of_a_word_alone() {
        for_each_primitive(|wait, notify| {
            let word = AtomicU32::new(0);
            let interrupted = WaitInterrupt::new();
            let other = WaitInterrupt::new();
            std::thread::scope(|scope| {
                let first = scope.spawn(|| wait(&word, 0, None, Some(&other)));
                std::thread::sleep(TIME_TO_SLEEP);
                let second = scope.spawn(|| wait(&word, 0, None, Some(&interrupted)));
                std::thread::sleep(TIME_TO_SLEEP);
                interrupted.interrupt();
                assert_eq!(second.join().unwrap(), AtomicWaitResult::Interrupted);
                assert!(!first.is_finished());
                assert_eq!(notify(&word, 2), 1);
                assert_eq!(first.join().unwrap(), AtomicWaitResult::Woken);
            });
        });
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn waiters_that_share_a_wake_bit_go_back_to_sleep_without_reporting_anything() {
        let word = AtomicU32::new(0);
        let interrupted = WaitInterrupt::new();
        let mut other = WaitInterrupt::new();
        other.wake_slot = interrupted.wake_slot;
        std::thread::scope(|scope| {
            let (victim, timed) = (
                scope.spawn(|| wait_in_kernel(&word, 0, None, Some(&other))),
                scope.spawn(|| wait_in_kernel(&word, 0, Some(TIME_TO_SLEEP * 6), Some(&other))),
            );
            std::thread::sleep(TIME_TO_SLEEP);
            let target = scope.spawn(|| wait_in_kernel(&word, 0, None, Some(&interrupted)));
            std::thread::sleep(TIME_TO_SLEEP);
            interrupted.interrupt();
            assert_eq!(target.join().unwrap(), AtomicWaitResult::Interrupted);
            std::thread::sleep(TIME_TO_SLEEP);
            assert!(!victim.is_finished());
            assert_eq!(timed.join().unwrap(), AtomicWaitResult::TimedOut);
            assert_eq!(atomic_notify(&word, 1), 1);
            assert_eq!(victim.join().unwrap(), AtomicWaitResult::Woken);
        });
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn zero_count_does_not_wake_waiters() {
        let word = AtomicU64::new(0);
        // SAFETY: The storage is aligned for AtomicU32, and neither view writes to it. Both wait widths use this
        // address as their futex key.
        let notify_word = unsafe { &*word.as_ptr().cast::<AtomicU32>() };
        for wait_word in [WaitWord::Word32(notify_word), WaitWord::Word64(&word)] {
            std::thread::scope(|scope| {
                let waiter = scope.spawn(|| atomic_wait(wait_word, 0, Some(Duration::from_millis(100)), None));
                while !waiter.is_finished() {
                    assert_eq!(atomic_notify(notify_word, 0), 0);
                    std::thread::yield_now();
                }
                assert_eq!(waiter.join().unwrap(), AtomicWaitResult::TimedOut);
            });
        }
    }

    // SharedArrayBuffers cross processes, where a waiter sleeps on a word that the notifier has at another address.
    #[cfg(target_os = "linux")]
    #[test]
    fn notifies_and_interrupts_cross_processes() {
        // SAFETY: A new mapping, which the words below live in and which is never unmapped.
        let shared = unsafe {
            libc::mmap(
                core::ptr::null_mut(),
                4096,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED | libc::MAP_ANONYMOUS,
                -1,
                0,
            )
        };
        assert_ne!(shared, libc::MAP_FAILED);
        // SAFETY: The mapping is zeroed, aligned and large enough.
        let (word, other_word) = unsafe { (&*shared.cast::<AtomicU32>(), &*shared.cast::<AtomicU32>().add(1)) };

        // SAFETY: The child only sleeps and exits, without anything that another thread could hold a lock of.
        let child = unsafe { libc::fork() };
        assert!(child >= 0);
        if child == 0 {
            let result = atomic_wait(WaitWord::Word32(word), 0, Some(Duration::from_secs(20)), None);
            // SAFETY: Ends the child without running anything of the test harness.
            unsafe { libc::_exit(i32::from(result != AtomicWaitResult::Woken)) };
        }

        let start = std::time::Instant::now();
        while atomic_notify(word, 1) == 0 {
            assert!(
                start.elapsed() < Duration::from_secs(10),
                "the child never started waiting"
            );
            std::thread::sleep(Duration::from_millis(1));
        }
        let mut status = 0;
        // SAFETY: Waits for the child of this thread.
        assert_eq!(unsafe { libc::waitpid(child, &raw mut status, 0) }, child);
        assert!(libc::WIFEXITED(status) && libc::WEXITSTATUS(status) == 0);

        // A waiter that shares a mapping with another process still has its interrupt.
        let interrupt = WaitInterrupt::new();
        std::thread::scope(|scope| {
            let waiter = scope.spawn(|| atomic_wait(WaitWord::Word32(other_word), 0, None, Some(&interrupt)));
            std::thread::sleep(TIME_TO_SLEEP);
            interrupt.interrupt();
            assert_eq!(waiter.join().unwrap(), AtomicWaitResult::Interrupted);
        });
    }
}
