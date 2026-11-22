/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The virtual memory operations the collector needs: reserving zero-filled address space that is only backed by
//! memory where it is touched, committing the chunks of the heap region that C++ reserved, and handing the pages of
//! unused blocks back to the system while keeping them committed.

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
    /// The range must be all of a reservation that reserve_zeroed() returned, which nothing uses any more.
    pub unsafe fn release(address: NonNull<u8>, size: usize) {
        // SAFETY: The caller passes a whole reservation that nothing uses.
        let result = unsafe { libc::munmap(address.as_ptr().cast(), size) };
        assert_eq!(result, 0, "munmap() failed");
    }

    /// # Safety
    ///
    /// The range must be within memory that reserve_zeroed() returned.
    pub unsafe fn commit_reserved_zeroed(_address: NonNull<u8>, _size: usize) {
        // NB: Anonymous mappings are backed by zeroed pages as they are first touched.
    }

    /// # Safety
    ///
    /// The range must be reserved address space that holds nothing yet.
    pub unsafe fn commit(address: NonNull<u8>, size: usize) {
        // SAFETY: The caller guarantees the range is reserved, so mapping over it replaces nothing else.
        let mapped = unsafe {
            libc::mmap(
                address.as_ptr().cast(),
                size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_FIXED,
                MEMORY_TAG,
                0,
            )
        };
        assert!(mapped != libc::MAP_FAILED, "commit a chunk of the heap region");
    }

    /// # Safety
    ///
    /// The range must be committed memory.
    pub unsafe fn populate(address: NonNull<u8>, size: usize) {
        // NB: This only fails if the kernel is too old to know it, which leaves the faults to the first touches.
        #[cfg(any(target_os = "linux", target_os = "android"))]
        // SAFETY: The caller guarantees the range is committed memory.
        unsafe {
            libc::madvise(address.as_ptr().cast(), size, libc::MADV_POPULATE_WRITE);
        }
        #[cfg(not(any(target_os = "linux", target_os = "android")))]
        let _ = (address, size);
    }

    /// Whether the range is made of whole pages. Advice applies to whole pages, and where a page is larger than a
    /// block, as on systems with 64 KiB pages, it also holds other blocks, which may be in use.
    fn is_whole_pages(address: NonNull<u8>, size: usize) -> bool {
        static PAGE_SIZE: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
        let page_size = *PAGE_SIZE.get_or_init(|| {
            // SAFETY: Reading a system configuration value has no preconditions.
            let page_size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
            usize::try_from(page_size).expect("the page size is known")
        });
        address.as_ptr().addr().is_multiple_of(page_size) && size.is_multiple_of(page_size)
    }

    fn madvise(address: NonNull<u8>, size: usize, advice: i32, what: &str) {
        // SAFETY: Every caller passes committed memory whose contents nothing needs, or that it is about to use again.
        let result = unsafe { libc::madvise(address.as_ptr().cast(), size, advice) };
        assert_eq!(result, 0, "{what} failed");
    }

    /// # Safety
    ///
    /// The range must be committed memory whose contents nothing needs any more.
    pub unsafe fn discard(address: NonNull<u8>, size: usize) {
        // NB: Where pages hold more than one block, the pages of a free block stay as they are.
        if !is_whole_pages(address, size) {
            return;
        }
        // NB: macOS has a paired protocol for this, which its accounting of resident memory understands.
        #[cfg(target_os = "macos")]
        madvise(address, size, libc::MADV_FREE_REUSABLE, "madvise(MADV_FREE_REUSABLE)");
        // NB: MADV_FREE keeps the pages mapped until the kernel needs them, so a block reused before then takes no
        //     page fault.
        #[cfg(any(target_os = "linux", target_os = "android", target_os = "freebsd"))]
        madvise(address, size, libc::MADV_FREE, "madvise(MADV_FREE)");
        #[cfg(not(any(
            target_os = "macos",
            target_os = "linux",
            target_os = "android",
            target_os = "freebsd"
        )))]
        madvise(address, size, libc::MADV_DONTNEED, "madvise(MADV_DONTNEED)");
    }

    /// # Safety
    ///
    /// The range must be committed memory whose contents nothing needs any more.
    pub unsafe fn return_to_system(address: NonNull<u8>, size: usize) {
        // NB: As in discard().
        if !is_whole_pages(address, size) {
            return;
        }
        #[cfg(any(target_os = "linux", target_os = "android"))]
        madvise(address, size, libc::MADV_DONTNEED, "madvise(MADV_DONTNEED)");
        // SAFETY: Forwarded from the caller.
        #[cfg(not(any(target_os = "linux", target_os = "android")))]
        unsafe {
            discard(address, size);
        }
    }

    /// # Safety
    ///
    /// The range must be committed memory that was discarded, or that mark_reusable() was called on.
    pub unsafe fn reuse(address: NonNull<u8>, size: usize) {
        #[cfg(target_os = "macos")]
        madvise(address, size, libc::MADV_FREE_REUSE, "madvise(MADV_FREE_REUSE)");
        #[cfg(not(target_os = "macos"))]
        let _ = (address, size);
    }

    /// # Safety
    ///
    /// The range must be committed memory that holds nothing yet.
    pub unsafe fn mark_reusable(address: NonNull<u8>, size: usize) {
        // NB: So that reuse() pairs with this when the blocks of a new chunk are handed out.
        #[cfg(target_os = "macos")]
        madvise(address, size, libc::MADV_FREE_REUSABLE, "madvise(MADV_FREE_REUSABLE)");
        #[cfg(not(target_os = "macos"))]
        let _ = (address, size);
    }
}

#[cfg(windows)]
mod platform {
    use core::ptr::NonNull;
    use windows_sys::Win32::System::Memory::{
        MEM_COMMIT, MEM_RELEASE, MEM_RESERVE, MEM_RESET, PAGE_NOACCESS, PAGE_READWRITE, VirtualAlloc, VirtualFree,
        VirtualUnlock,
    };

    pub fn reserve_zeroed(size: usize) -> Option<NonNull<u8>> {
        // NB: Windows has no lazily committed memory that does not count towards the commit limit, so this reserves
        //     only, and commit_reserved_zeroed() commits what is used.
        // SAFETY: Reserving address space that the system places anywhere touches no existing memory.
        let address = unsafe { VirtualAlloc(core::ptr::null(), size, MEM_RESERVE, PAGE_NOACCESS) };
        NonNull::new(address.cast())
    }

    /// # Safety
    ///
    /// The range must be all of a reservation that reserve_zeroed() returned, which nothing uses any more.
    pub unsafe fn release(address: NonNull<u8>, _size: usize) {
        // SAFETY: The caller passes a whole reservation that nothing uses.
        let released = unsafe { VirtualFree(address.as_ptr().cast(), 0, MEM_RELEASE) };
        assert!(released != 0, "VirtualFree(MEM_RELEASE) failed");
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

    /// # Safety
    ///
    /// The range must be reserved address space that holds nothing yet.
    pub unsafe fn commit(address: NonNull<u8>, size: usize) {
        // SAFETY: The caller guarantees the range is reserved.
        let committed = unsafe { VirtualAlloc(address.as_ptr().cast(), size, MEM_COMMIT, PAGE_READWRITE) };
        assert!(!committed.is_null(), "commit a chunk of the heap region");
    }

    /// # Safety
    ///
    /// The range must be committed memory.
    pub unsafe fn populate(_address: NonNull<u8>, _size: usize) {}

    /// # Safety
    ///
    /// The range must be committed memory whose contents nothing needs any more.
    pub unsafe fn discard(address: NonNull<u8>, size: usize) {
        // NB: DiscardVirtualMemory() validates its range with VirtualQuery(), which makes the kernel check every page in
        //     the run of identical pages that starts at the block. All chunks are committed back to back in one
        //     reservation, so that run is the whole committed heap. MEM_RESET drops the contents without that walk, and
        //     VirtualUnlock() on pages that are not locked takes them out of the working set right away.
        // SAFETY: The caller guarantees the contents are unused.
        let reset = unsafe { VirtualAlloc(address.as_ptr().cast(), size, MEM_RESET, PAGE_READWRITE) };
        assert!(!reset.is_null(), "VirtualAlloc(MEM_RESET) failed");
        // SAFETY: As above.
        unsafe { VirtualUnlock(address.as_ptr().cast(), size) };
    }

    /// # Safety
    ///
    /// The range must be committed memory whose contents nothing needs any more.
    pub unsafe fn return_to_system(address: NonNull<u8>, size: usize) {
        // SAFETY: Forwarded from the caller.
        unsafe { discard(address, size) }
    }

    /// # Safety
    ///
    /// The range must be committed memory.
    pub unsafe fn reuse(_address: NonNull<u8>, _size: usize) {}

    /// # Safety
    ///
    /// The range must be committed memory.
    pub unsafe fn mark_reusable(_address: NonNull<u8>, _size: usize) {}
}

pub fn reserve_zeroed(size: usize) -> Option<NonNull<u8>> {
    platform::reserve_zeroed(size)
}

/// Gives back all of a reservation that reserve_zeroed() returned.
///
/// # Safety
///
/// The range must be all of a reservation that reserve_zeroed() returned, which nothing uses any more.
pub unsafe fn release(address: NonNull<u8>, size: usize) {
    // SAFETY: Forwarded from the caller.
    unsafe { platform::release(address, size) }
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

/// Commits a range of address space that the heap region reserved, which reads as zero until it is written.
///
/// # Safety
///
/// The range must be reserved address space that holds nothing yet.
pub unsafe fn commit(address: NonNull<u8>, size: usize) {
    // SAFETY: Forwarded from the caller.
    unsafe { platform::commit(address, size) }
}

/// Faults the pages of a committed range in at once, where the system can do that, which costs a fraction of faulting
/// them in one by one.
///
/// # Safety
///
/// The range must be committed memory.
pub unsafe fn populate(address: NonNull<u8>, size: usize) {
    // SAFETY: Forwarded from the caller.
    unsafe { platform::populate(address, size) }
}

/// Lets the system take back the pages of a committed range when it runs short of memory. Until then, they keep their
/// contents and cost no fault when they are written again.
///
/// # Safety
///
/// The range must be committed memory whose contents nothing needs any more.
pub unsafe fn discard(address: NonNull<u8>, size: usize) {
    // SAFETY: Forwarded from the caller.
    unsafe { platform::discard(address, size) }
}

/// Gives the pages of a committed range back to the system right away, where the system can tell that apart from
/// discard().
///
/// # Safety
///
/// The range must be committed memory whose contents nothing needs any more.
pub unsafe fn return_to_system(address: NonNull<u8>, size: usize) {
    // SAFETY: Forwarded from the caller.
    unsafe { platform::return_to_system(address, size) }
}

/// Tells the system that a range that discard() or mark_reusable() was called on is in use again.
///
/// # Safety
///
/// The range must be committed memory.
pub unsafe fn reuse(address: NonNull<u8>, size: usize) {
    // SAFETY: Forwarded from the caller.
    unsafe { platform::reuse(address, size) }
}

/// Marks a freshly committed range as unused, so that reuse() pairs with it as it does with discard().
///
/// # Safety
///
/// The range must be committed memory that holds nothing yet.
pub unsafe fn mark_reusable(address: NonNull<u8>, size: usize) {
    // SAFETY: Forwarded from the caller.
    unsafe { platform::mark_reusable(address, size) }
}
