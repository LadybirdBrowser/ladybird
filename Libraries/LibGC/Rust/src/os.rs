/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The virtual memory operations the collector needs: reserving zero-filled address space that is only backed by
//! memory where it is touched.

use core::ptr::NonNull;

#[cfg(unix)]
mod platform {
    use core::ptr::NonNull;

    // Tags the heap's memory on macOS, as Core::System does, so that tools like vmmap show it apart from malloc.
    #[cfg(target_os = "macos")]
    const MEMORY_TAG: i32 = 241 << 24;
    #[cfg(not(target_os = "macos"))]
    const MEMORY_TAG: i32 = -1;

    pub fn reserve_zeroed(size: usize) -> Option<NonNull<u8>> {
        // SAFETY: An anonymous mapping that the kernel places anywhere touches no existing memory.
        let address = unsafe {
            libc::mmap(
                core::ptr::null_mut(),
                size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_NORESERVE,
                MEMORY_TAG,
                0,
            )
        };
        if address == libc::MAP_FAILED {
            return None;
        }
        NonNull::new(address.cast())
    }

    /// # Safety
    ///
    /// The range must be within memory that reserve_zeroed() returned.
    pub unsafe fn commit_reserved_zeroed(_address: NonNull<u8>, _size: usize) {
        // NB: Anonymous mappings are backed by zeroed pages as they are first touched.
    }
}

#[cfg(windows)]
mod platform {
    use core::ptr::NonNull;
    use windows_sys::Win32::System::Memory::{MEM_COMMIT, MEM_RESERVE, PAGE_NOACCESS, PAGE_READWRITE, VirtualAlloc};

    pub fn reserve_zeroed(size: usize) -> Option<NonNull<u8>> {
        // NB: Windows has no lazily committed memory that does not count towards the commit limit, so this reserves
        //     only, and commit_reserved_zeroed() commits what is used.
        // SAFETY: Reserving address space that the system places anywhere touches no existing memory.
        let address = unsafe { VirtualAlloc(core::ptr::null(), size, MEM_RESERVE, PAGE_NOACCESS) };
        NonNull::new(address.cast())
    }

    /// # Safety
    ///
    /// The range must be within memory that reserve_zeroed() returned.
    pub unsafe fn commit_reserved_zeroed(address: NonNull<u8>, size: usize) {
        // NB: Committing pages that are committed already leaves their contents alone.
        // SAFETY: The caller guarantees the range is reserved.
        let committed = unsafe { VirtualAlloc(address.as_ptr().cast(), size, MEM_COMMIT, PAGE_READWRITE) };
        assert!(!committed.is_null(), "commit the table of block descriptors");
    }
}

pub fn reserve_zeroed(size: usize) -> Option<NonNull<u8>> {
    platform::reserve_zeroed(size)
}

/// Backs a range of memory that reserve_zeroed() returned with memory, if it was not already.
///
/// # Safety
///
/// The range must be within memory that reserve_zeroed() returned.
pub unsafe fn commit_reserved_zeroed(address: NonNull<u8>, size: usize) {
    // SAFETY: The caller's guarantee is the platform's.
    unsafe { platform::commit_reserved_zeroed(address, size) }
}
