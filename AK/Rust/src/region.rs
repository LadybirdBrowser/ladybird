/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Ranges of address space of a process's own, with memory that comes straight from the system and goes back to it.

use core::ffi::CStr;
use core::num::NonZeroUsize;
use core::ptr::NonNull;

/// What the system does for a region. Addresses and sizes are multiples of the page size.
#[cfg(all(unix, not(miri)))]
mod system {
    use core::ffi::CStr;
    use core::ptr::NonNull;

    pub fn page_size() -> usize {
        // SAFETY: sysconf() has no preconditions.
        let size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
        usize::try_from(size).expect("the page size is positive")
    }

    /// Maps `size` bytes that nothing can access, at `address` in place of what is mapped there, or anywhere if it is
    /// null.
    ///
    /// # Safety
    ///
    /// Unless `address` is null, the range must be inside a mapping from `reserve()` that nothing reads or writes.
    unsafe fn map_inaccessible(address: *mut libc::c_void, size: usize, name: &CStr) -> *mut libc::c_void {
        let fixed = if address.is_null() { 0 } else { libc::MAP_FIXED };
        // SAFETY: A new mapping at an address of the system's choosing has no preconditions, and the caller keeps the
        //         contract for a mapping in place of another.
        let mapping = unsafe {
            libc::mmap(
                address,
                size,
                libc::PROT_NONE,
                libc::MAP_PRIVATE | libc::MAP_ANON | libc::MAP_NORESERVE | fixed,
                -1,
                0,
            )
        };
        assert_ne!(mapping, libc::MAP_FAILED, "mapping address space for a column failed");
        // NB: The name tells the memory of columns apart in /proc/<pid>/smaps. A kernel that cannot name mappings
        //     fails the call, which changes nothing.
        #[cfg(target_os = "linux")]
        // SAFETY: The range is the mapping made above, and the kernel copies the name.
        unsafe {
            libc::prctl(
                libc::PR_SET_VMA,
                libc::PR_SET_VMA_ANON_NAME as libc::c_ulong,
                mapping.addr() as libc::c_ulong,
                size as libc::c_ulong,
                name.as_ptr(),
            );
        }
        #[cfg(not(target_os = "linux"))]
        let _ = name;
        mapping
    }

    /// Reserves `size` bytes of address space, which is no memory yet.
    pub fn reserve(size: usize, name: &CStr) -> NonNull<u8> {
        // SAFETY: The address is null.
        let mapping = unsafe { map_inaccessible(core::ptr::null_mut(), size, name) };
        NonNull::new(mapping.cast()).expect("no mapping is at address zero")
    }

    /// Makes a range memory that can be read and written. The memory holds zeros.
    ///
    /// # Safety
    ///
    /// The range must be inside a mapping from `reserve()`, and not be memory already.
    pub unsafe fn commit(address: NonNull<u8>, size: usize) {
        // SAFETY: The caller keeps the contract.
        let result = unsafe { libc::mprotect(address.as_ptr().cast(), size, libc::PROT_READ | libc::PROT_WRITE) };
        assert_eq!(result, 0, "the system has no memory for a column");
    }

    /// Gives the memory of a range back to the system. The range stays reserved.
    ///
    /// # Safety
    ///
    /// The range must be inside a mapping from `reserve()`, and nothing may read or write it until `commit()`.
    pub unsafe fn decommit(address: NonNull<u8>, size: usize, name: &CStr) {
        // NB: A new mapping in place of the pages drops them at once on every Unix. Advice to the kernel either leaves
        //     the pages for it to take later, or is not the same call everywhere.
        // SAFETY: The caller keeps the contract.
        unsafe { map_inaccessible(address.as_ptr().cast(), size, name) };
    }

    /// Gives the address space of a mapping back.
    ///
    /// # Safety
    ///
    /// `base` and `size` must be those of a mapping from `reserve()` that nothing uses anymore.
    pub unsafe fn release(base: NonNull<u8>, size: usize) {
        // SAFETY: The caller keeps the contract.
        let result = unsafe { libc::munmap(base.as_ptr().cast(), size) };
        assert_eq!(result, 0, "unmapping the address space of a column failed");
    }
}

/// What the system does for a region. Addresses and sizes are multiples of the page size.
#[cfg(all(windows, not(miri)))]
mod system {
    use core::ffi::{CStr, c_void};
    use core::ptr::NonNull;

    const MEM_COMMIT: u32 = 0x1000;
    const MEM_RESERVE: u32 = 0x2000;
    const MEM_DECOMMIT: u32 = 0x4000;
    const MEM_RELEASE: u32 = 0x8000;
    const PAGE_NOACCESS: u32 = 0x01;
    const PAGE_READWRITE: u32 = 0x04;

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn VirtualAlloc(address: *mut c_void, size: usize, allocation_type: u32, protect: u32) -> *mut c_void;
        fn VirtualFree(address: *mut c_void, size: usize, free_type: u32) -> i32;
    }

    pub fn page_size() -> usize {
        4096
    }

    /// Reserves `size` bytes of address space, which is no memory yet.
    pub fn reserve(size: usize, _name: &CStr) -> NonNull<u8> {
        // SAFETY: A reservation at an address of the system's choosing has no preconditions.
        let base = unsafe { VirtualAlloc(core::ptr::null_mut(), size, MEM_RESERVE, PAGE_NOACCESS) };
        NonNull::new(base.cast()).expect("reserving address space for a column failed")
    }

    /// Makes a range memory that can be read and written. The memory holds zeros.
    ///
    /// # Safety
    ///
    /// The range must be inside a reservation from `reserve()`, and not be memory already.
    pub unsafe fn commit(address: NonNull<u8>, size: usize) {
        // SAFETY: The caller keeps the contract.
        let result = unsafe { VirtualAlloc(address.as_ptr().cast(), size, MEM_COMMIT, PAGE_READWRITE) };
        assert!(!result.is_null(), "the system has no memory for a column");
    }

    /// Gives the memory of a range back to the system. The range stays reserved.
    ///
    /// # Safety
    ///
    /// The range must be inside a reservation from `reserve()`, and nothing may read or write it until `commit()`.
    pub unsafe fn decommit(address: NonNull<u8>, size: usize, _name: &CStr) {
        // SAFETY: The caller keeps the contract.
        let result = unsafe { VirtualFree(address.as_ptr().cast(), size, MEM_DECOMMIT) };
        assert_ne!(result, 0, "giving the memory of a column back failed");
    }

    /// Gives the address space of a reservation back.
    ///
    /// # Safety
    ///
    /// `base` must come from `reserve()`, and nothing may use the reservation anymore.
    pub unsafe fn release(base: NonNull<u8>, _size: usize) {
        // SAFETY: The caller keeps the contract.
        let result = unsafe { VirtualFree(base.as_ptr().cast(), 0, MEM_RELEASE) };
        assert_ne!(result, 0, "releasing the address space of a column failed");
    }
}

/// Miri has no way to change what a page allows, so one allocation that holds zeros stands in for a region there.
#[cfg(miri)]
mod system {
    use core::ffi::CStr;
    use core::ptr::NonNull;
    use std::alloc::{Layout, alloc_zeroed, dealloc, handle_alloc_error};

    pub fn page_size() -> usize {
        4096
    }

    fn layout(size: usize) -> Layout {
        Layout::from_size_align(size, page_size()).expect("the size of a region is a multiple of its alignment")
    }

    pub fn reserve(size: usize, _name: &CStr) -> NonNull<u8> {
        // SAFETY: A region is at least a page, so the layout has a size.
        let base = unsafe { alloc_zeroed(layout(size)) };
        NonNull::new(base).unwrap_or_else(|| handle_alloc_error(layout(size)))
    }

    pub unsafe fn commit(_address: NonNull<u8>, _size: usize) {}

    pub unsafe fn decommit(_address: NonNull<u8>, _size: usize, _name: &CStr) {}

    pub unsafe fn release(base: NonNull<u8>, size: usize) {
        // SAFETY: The caller passes what `reserve()` allocated with this layout.
        unsafe { dealloc(base.as_ptr(), layout(size)) };
    }
}

/// A range of address space of which the first `committed` bytes are memory that can be read and written. The rest is
/// reserved: nothing else gets mapped there, and it takes no memory. A region takes its address space when it first
/// gets memory, so one that is never used costs the system nothing.
pub(crate) struct Region {
    /// Where the address space of the region is, once it has some.
    base: Option<NonNull<u8>>,
    reserved: usize,
    committed: usize,
    name: &'static CStr,
}

/// What a region without address space says it starts at: an address that is not null, with nothing there.
const NO_BASE: NonNull<u8> = NonNull::without_provenance(NonZeroUsize::new(4096).unwrap());

impl Region {
    /// Where the region starts, which is a multiple of 4096. A region with no memory may have nothing there.
    pub(crate) fn base(&self) -> NonNull<u8> {
        self.base.unwrap_or(NO_BASE)
    }

    /// How many bytes of address space the region has.
    pub(crate) fn reserved(&self) -> usize {
        self.reserved
    }

    /// Whether the region has taken its address space.
    #[cfg(test)]
    pub(crate) fn has_address_space(&self) -> bool {
        self.base.is_some()
    }

    /// How many bytes of the region are memory. They are its first ones.
    pub(crate) fn committed(&self) -> usize {
        self.committed
    }

    /// A region of at least `size` bytes, none of them memory yet.
    pub(crate) fn new(size: usize, name: &'static CStr) -> Self {
        let reserved = size
            .max(1)
            .checked_next_multiple_of(system::page_size())
            .expect("a region fits in the address space");
        Self {
            base: None,
            reserved,
            committed: 0,
            name,
        }
    }

    /// Makes at least the first `size` bytes memory. What was memory before keeps its contents.
    pub(crate) fn commit(&mut self, size: usize) {
        assert!(size <= self.reserved, "a region does not grow past what it reserved");
        let committed = size.next_multiple_of(system::page_size());
        if committed <= self.committed {
            return;
        }
        let base = *self
            .base
            .get_or_insert_with(|| system::reserve(self.reserved, self.name));
        // SAFETY: Both sizes are multiples of the page size and at most `reserved`, so the range is the part of the
        //         reservation from where its memory ends.
        unsafe { system::commit(base.add(self.committed), committed - self.committed) };
        self.committed = committed;
    }

    /// Gives the memory past the first `size` bytes back to the system.
    pub(crate) fn decommit_past(&mut self, size: usize) {
        if size >= self.committed {
            return;
        }
        let kept = size.next_multiple_of(system::page_size());
        if kept >= self.committed {
            return;
        }
        let base = self.base.expect("a region with memory has address space");
        // SAFETY: The range is the end of the memory of the reservation, and `committed` shrinks to leave it out, so
        //         nothing reads or writes it until commit() makes it memory again.
        unsafe { system::decommit(base.add(kept), self.committed - kept, self.name) };
        self.committed = kept;
    }
}

impl Drop for Region {
    fn drop(&mut self) {
        let Some(base) = self.base else {
            return;
        };
        // SAFETY: The reservation is this region's, which nothing can use once it is dropped.
        unsafe { system::release(base, self.reserved) };
    }
}

// SAFETY: A region owns its mapping, and no thread has a special tie to a mapping.
unsafe impl Send for Region {}

/// The size of a page of memory.
#[cfg(test)]
pub(crate) fn page_size() -> usize {
    system::page_size()
}
