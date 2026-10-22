/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Linear memories. A memory reserves the address range its accesses can form up front and only commits what it has
//! grown to, so that its base never moves and accesses past its end fault into the guard region instead of needing a
//! bounds check. A store with a LibGC heap keeps its memories in LibGC's primitive storage, inside the cage that
//! array buffers live in, so that buffers can view them; other stores map their own.

use super::ranges::RangeSet;
use crate::abi::MemoryData;
use crate::types::{AddressType, MemoryType};

pub const PAGE_SIZE: u64 = 64 * 1024;
const MEMORY32_MAX_PAGES: u64 = 1 << 16;

/// The most pages a memory64 reserves, and so can grow to: 16 GiB. Its accesses are bounds checked, so it needs no
/// guard region.
const MEMORY64_MAX_PAGES: u64 = 1 << 18;

/// The bytes a memory32 memory reserves: every address (u32) plus every offset (u32), plus one page for the width of
/// the access.
pub const GUARDED_RESERVATION_SIZE: u64 = 2 * (MEMORY32_MAX_PAGES * PAGE_SIZE) + PAGE_SIZE;

/// The address ranges all memories reserve, for the fault handler to recognize accesses that ran off a memory.
static RESERVATIONS: RangeSet = RangeSet::new();

/// Whether `address` is in the reservation of a memory. Async-signal-safe, for the fault handler.
pub fn is_in_any_reservation(address: usize) -> bool {
    RESERVATIONS.contains(address)
}

/// A memory. The interpreter sees its data, which is its first field and so at its handle.
#[repr(C)]
pub struct MemoryInstance {
    pub data: MemoryData,
    pub ty: MemoryType,
    reservation: *mut u8,
    reservation_size: usize,
    pages: u64,
    maximum_pages: u64,
    backing: Backing,
}

enum Backing {
    Mapping,
    PrimitiveStorage {
        handle: u64,
        interface: super::heap::GcInterface,
    },
}

// The reservation is only accessed through the memory that owns it.
unsafe impl Send for MemoryInstance {}

#[cfg(unix)]
mod platform {
    pub unsafe fn reserve(size: usize) -> *mut u8 {
        let pointer = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                size,
                libc::PROT_NONE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_NORESERVE,
                -1,
                0,
            )
        };
        if pointer == libc::MAP_FAILED {
            std::ptr::null_mut()
        } else {
            pointer.cast()
        }
    }

    pub unsafe fn commit(address: *mut u8, size: usize) -> bool {
        unsafe { libc::mprotect(address.cast(), size, libc::PROT_READ | libc::PROT_WRITE) == 0 }
    }

    pub unsafe fn release(address: *mut u8, size: usize) {
        unsafe {
            libc::munmap(address.cast(), size);
        }
    }
}

#[cfg(windows)]
mod platform {
    use std::ffi::c_void;

    const MEM_COMMIT: u32 = 0x1000;
    const MEM_RESERVE: u32 = 0x2000;
    const MEM_RELEASE: u32 = 0x8000;
    const PAGE_NOACCESS: u32 = 0x01;
    const PAGE_READWRITE: u32 = 0x04;

    unsafe extern "system" {
        fn VirtualAlloc(address: *mut c_void, size: usize, allocation_type: u32, protect: u32) -> *mut c_void;
        fn VirtualFree(address: *mut c_void, size: usize, free_type: u32) -> i32;
    }

    pub unsafe fn reserve(size: usize) -> *mut u8 {
        unsafe { VirtualAlloc(std::ptr::null_mut(), size, MEM_RESERVE, PAGE_NOACCESS).cast() }
    }

    pub unsafe fn commit(address: *mut u8, size: usize) -> bool {
        unsafe { !VirtualAlloc(address.cast(), size, MEM_COMMIT, PAGE_READWRITE).is_null() }
    }

    pub unsafe fn release(address: *mut u8, _size: usize) {
        unsafe {
            VirtualFree(address.cast(), 0, MEM_RELEASE);
        }
    }
}

impl MemoryInstance {
    pub fn new(ty: MemoryType, heap: &super::heap::Heap) -> Option<Self> {
        let (maximum_pages, capacity, guard_size) = match ty.limits.address_type {
            AddressType::I32 => {
                let maximum_pages = ty.limits.max.unwrap_or(MEMORY32_MAX_PAGES).min(MEMORY32_MAX_PAGES);
                let capacity = maximum_pages * PAGE_SIZE;
                (maximum_pages, capacity, GUARDED_RESERVATION_SIZE - capacity)
            }
            AddressType::I64 => {
                let maximum_pages = ty.limits.max.unwrap_or(MEMORY64_MAX_PAGES).min(MEMORY64_MAX_PAGES);
                // A memory that cannot grow still reserves a page, so that its base is a valid address.
                (maximum_pages, maximum_pages.max(1) * PAGE_SIZE, 0)
            }
        };
        if ty.limits.min > maximum_pages {
            return None;
        }
        let reservation_size = (capacity + guard_size) as usize;
        let (reservation, backing) = match heap.gc() {
            Some(gc) => {
                let interface = gc.interface;
                let mut handle = 0;
                let mut layout = super::heap::PrimitiveStorageLayout::default();
                let reserved = unsafe {
                    (interface.gc_primitive_storage_reserve)(
                        0,
                        capacity as usize,
                        true,
                        guard_size as usize,
                        &raw mut handle,
                        &raw mut layout,
                    )
                };
                if !reserved {
                    return None;
                }
                let data = unsafe { (interface.gc_primitive_storage_data)(handle) };
                (data, Backing::PrimitiveStorage { handle, interface })
            }
            None => {
                let reservation = unsafe { platform::reserve(reservation_size) };
                if reservation.is_null() {
                    return None;
                }
                (reservation, Backing::Mapping)
            }
        };
        let mut memory = Self {
            data: MemoryData {
                base: reservation,
                size: 0,
                address64: u64::from(ty.limits.address_type == AddressType::I64),
                shared: u64::from(ty.limits.shared),
            },
            ty,
            reservation,
            reservation_size,
            pages: 0,
            maximum_pages,
            backing,
        };
        // NB: Dropping the memory releases the reservation if anything fails from here on.
        if !RESERVATIONS.add(reservation as usize, reservation_size) {
            return None;
        }
        memory.grow(ty.limits.min)?;
        Some(memory)
    }

    pub fn pages(&self) -> u64 {
        self.pages
    }

    pub fn size(&self) -> usize {
        (self.pages * PAGE_SIZE) as usize
    }

    /// The handle of the primitive storage the memory lives in, or zero if it is a plain mapping.
    pub fn primitive_storage_handle(&self) -> u64 {
        match &self.backing {
            Backing::Mapping => 0,
            Backing::PrimitiveStorage { handle, .. } => *handle,
        }
    }

    /// Grow by `delta` pages, returning the previous size in pages.
    pub fn grow(&mut self, delta: u64) -> Option<u64> {
        let previous = self.pages;
        let new_pages = previous.checked_add(delta)?;
        if new_pages > self.maximum_pages {
            return None;
        }
        if delta != 0 {
            match &self.backing {
                Backing::Mapping => {
                    let start = unsafe { self.reservation.add(self.size()) };
                    if !unsafe { platform::commit(start, (delta * PAGE_SIZE) as usize) } {
                        return None;
                    }
                }
                Backing::PrimitiveStorage { handle, interface } => {
                    // Within the capacity, the storage stays where it is.
                    let new_size = (new_pages * PAGE_SIZE) as usize;
                    let resized = unsafe {
                        (interface.gc_primitive_storage_resize)(*handle, new_size, true, core::ptr::null_mut())
                    };
                    if !resized {
                        return None;
                    }
                }
            }
        }
        self.pages = new_pages;
        self.data.size = new_pages * PAGE_SIZE;
        Some(previous)
    }

    pub fn bytes(&self) -> &[u8] {
        unsafe { ak::slice_from_raw_parts(self.reservation, self.size()) }
    }

    pub fn bytes_mut(&mut self) -> &mut [u8] {
        unsafe { ak::slice_from_raw_parts_mut(self.reservation, self.size()) }
    }
}

impl Drop for MemoryInstance {
    fn drop(&mut self) {
        RESERVATIONS.remove(self.reservation as usize);
        match &self.backing {
            Backing::Mapping => unsafe { platform::release(self.reservation, self.reservation_size) },
            Backing::PrimitiveStorage { handle, interface } => unsafe {
                (interface.gc_primitive_storage_free)(*handle);
            },
        }
    }
}
