/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The bounds of the native stack of the calling thread.

/// The lowest address of the stack of this thread, or `None` if that cannot be told.
pub fn lowest_address_of_this_thread() -> Option<usize> {
    // NB: Asking for the stack of the main thread reads /proc/self/maps on Linux.
    thread_local! {
        static LOWEST_ADDRESS: Option<usize> = thread_stack_lowest_address();
    }
    LOWEST_ADDRESS.with(|lowest_address| *lowest_address)
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
        (result == 0).then_some(address as usize)
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
