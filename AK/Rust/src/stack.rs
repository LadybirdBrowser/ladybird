/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The bounds of the native stack of the calling thread.

use std::cell::Cell;

/// The lowest address of the stack of this thread, or `None` if that cannot be told.
pub fn lowest_address_of_this_thread() -> Option<usize> {
    // NB: Asking for the stack of the main thread reads /proc/self/maps on Linux.
    thread_local! {
        static LOWEST_ADDRESS: Cell<Option<usize>> = const { Cell::new(None) };
    }
    LOWEST_ADDRESS.with(|lowest_address| found_once(lowest_address, thread_stack_lowest_address))
}

/// What `cache` holds, or else what `find` finds, which `cache` keeps only if it was found: A stack's bounds never
/// change, but a lookup that failed can succeed when it's asked again.
fn found_once(cache: &Cell<Option<usize>>, find: impl FnOnce() -> Option<usize>) -> Option<usize> {
    if let Some(found) = cache.get() {
        return Some(found);
    }
    let found = find();
    cache.set(found);
    found
}

/// The address that the stack can only go `bytes` below where it is now.
pub fn limit_for_budget(bytes: usize) -> usize {
    let marker = 0u8;
    (&raw const marker as usize).saturating_sub(bytes)
}

#[cfg(target_os = "linux")]
fn thread_stack_lowest_address() -> Option<usize> {
    // SAFETY: The attributes live for the calls, and are destroyed once initialized.
    unsafe {
        let mut attributes: libc::pthread_attr_t = std::mem::zeroed();
        if libc::pthread_getattr_np(libc::pthread_self(), &raw mut attributes) != 0 {
            return None;
        }
        let mut address = std::ptr::null_mut();
        let mut size = 0;
        let result = libc::pthread_attr_getstack(&raw const attributes, &raw mut address, &raw mut size);
        libc::pthread_attr_destroy(&raw mut attributes);
        if result != 0 {
            return None;
        }
        #[cfg(target_env = "musl")]
        if let Some(lowest_address) = main_thread_stack_lowest_address(address as usize) {
            return Some(lowest_address);
        }
        Some(address as usize)
    }
}

/// For the main thread, the lowest address its stack can grow down to, or `None` on any other thread.
///
/// musl reports only the part of the main thread's stack that is mapped so far, and from below the argument and
/// environment strings at its top. But the kernel grows the stack on demand until it spans the stack size limit, from
/// the top of its mapping. The kernel puts the executable's file name at that top, under one pointer, so the top
/// follows from where the file name ends. A guard page is left below. The stack reaches at least as far as
/// `mapped_lowest_address`, the lowest address musl reports, even where an unlimited size limit is taken as less.
#[cfg(all(target_os = "linux", target_env = "musl"))]
fn main_thread_stack_lowest_address(mapped_lowest_address: usize) -> Option<usize> {
    // SAFETY: These only read the process's and the thread's IDs, the auxiliary vector and the file name it points to,
    //         the stack size limit, and the page size.
    unsafe {
        if libc::getpid() != libc::gettid() {
            return None;
        }
        let file_name = libc::getauxval(libc::AT_EXECFN) as *const libc::c_char;
        if file_name.is_null() {
            return None;
        }
        let mut limit: libc::rlimit = std::mem::zeroed();
        if libc::getrlimit(libc::RLIMIT_STACK, &raw mut limit) != 0 {
            return None;
        }
        // The same size AK::StackInfo assumes for an unlimited stack.
        let size = if limit.rlim_cur == libc::RLIM_INFINITY {
            8 * 0x10000
        } else {
            usize::try_from(limit.rlim_cur).unwrap_or(usize::MAX)
        };
        let page_size = usize::try_from(libc::sysconf(libc::_SC_PAGESIZE)).ok()?;
        let top = (file_name as usize + libc::strlen(file_name) + 1 + size_of::<usize>()).next_multiple_of(page_size);
        Some(lowest_address_within_size_limit(
            top,
            size,
            page_size,
            mapped_lowest_address,
        ))
    }
}

/// The lowest address a stack from `top` can grow down to under the size limit `size`, a guard page above it, but never
/// above `mapped_lowest_address`. A limit that reaches past address 0 leaves the stack at `mapped_lowest_address`, as it
/// does in AK::StackInfo, since an address of 0 would keep the stack checks from ever stopping a recursion.
#[cfg(any(test, all(target_os = "linux", target_env = "musl")))]
fn lowest_address_within_size_limit(top: usize, size: usize, page_size: usize, mapped_lowest_address: usize) -> usize {
    match top.checked_sub(size.saturating_sub(page_size)) {
        Some(lowest_address) => lowest_address.min(mapped_lowest_address),
        None => mapped_lowest_address,
    }
}

#[cfg(target_os = "macos")]
#[allow(clippy::unnecessary_wraps)] // The other platforms' versions can fail.
fn thread_stack_lowest_address() -> Option<usize> {
    // SAFETY: These only read the stack of the calling thread.
    unsafe {
        let thread = libc::pthread_self();
        let top = libc::pthread_get_stackaddr_np(thread) as usize;
        Some(top - libc::pthread_get_stacksize_np(thread))
    }
}

#[cfg(windows)]
#[allow(clippy::unnecessary_wraps)] // The other platforms' versions can fail.
fn thread_stack_lowest_address() -> Option<usize> {
    unsafe extern "system" {
        fn GetCurrentThreadStackLimits(low_limit: *mut usize, high_limit: *mut usize);
        fn SetThreadStackGuarantee(stack_size_in_bytes: *mut u32) -> i32;
    }
    let (mut low_limit, mut high_limit) = (0, 0);
    let mut stack_guarantee = 0;
    // SAFETY: These only read the limits of the stack of the calling thread. A guarantee of 0 asks for the current one
    //         without changing it.
    unsafe {
        GetCurrentThreadStackLimits(&raw mut low_limit, &raw mut high_limit);
        SetThreadStackGuarantee(&raw mut stack_guarantee);
    }
    // The stack overflows before reaching its low limit by the size reserved for handling that overflow.
    Some(low_limit + stack_guarantee as usize)
}

#[cfg(not(any(target_os = "linux", target_os = "macos", windows)))]
fn thread_stack_lowest_address() -> Option<usize> {
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_lookup_that_failed_is_tried_again_and_one_that_succeeded_is_kept() {
        let cache = Cell::new(None);
        assert_eq!(found_once(&cache, || None), None);
        assert_eq!(found_once(&cache, || Some(4096)), Some(4096));
        assert_eq!(
            found_once(&cache, || unreachable!("a found address is kept")),
            Some(4096)
        );
    }

    #[test]
    fn a_size_limit_that_reaches_past_address_zero_keeps_the_mapped_bound() {
        let page_size = 4096;
        let top = 0x7fff_0000_0000;
        let mapped_lowest_address = top - 33 * page_size;
        assert_eq!(
            lowest_address_within_size_limit(top, 8 * 1024 * 1024, page_size, mapped_lowest_address),
            top - 8 * 1024 * 1024 + page_size
        );
        assert_eq!(
            lowest_address_within_size_limit(top, 16 * page_size, page_size, mapped_lowest_address),
            mapped_lowest_address
        );
        assert_eq!(
            lowest_address_within_size_limit(top, top, page_size, mapped_lowest_address),
            page_size
        );
        assert_eq!(
            lowest_address_within_size_limit(top, usize::MAX, page_size, mapped_lowest_address),
            mapped_lowest_address
        );
    }
}
