/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GC::PrimitiveStorage through LibGC's C interface: byte buffers inside the one cage of the process, which the
//! interpreter addresses as offsets from the cage base. Like the heap, it belongs to the thread that runs the VM, so
//! its storage never leaves that thread.

use core::num::NonZeroU64;
use std::sync::OnceLock;

use super::capi::{
    self, GC_PRIMITIVE_STORAGE_INVALID_OFFSET, GC_PRIMITIVE_STORAGE_NULL_HANDLE, GCPrimitiveStorageHandle,
    GCPrimitiveStorageLayout,
};

/// GC::PrimitiveStorage::invalid_offset, the offset of storage that does not exist.
pub const INVALID_OFFSET: usize = GC_PRIMITIVE_STORAGE_INVALID_OFFSET;

/// What PrimitiveStorage reports when the cage or the system cannot provide the memory.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OutOfMemory;

/// GC::PrimitiveStorage::ZeroFillNewBytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ZeroFillNewBytes {
    No,
    Yes,
}

impl ZeroFillNewBytes {
    fn as_bool(self) -> bool {
        self == Self::Yes
    }
}

/// gc_primitive_storage_cage_base(), the address the interpreter adds the cached offsets of typed arrays to. LibGC
/// reserves the cage on the first call, which the VM makes when it is created.
pub fn cage_base() -> usize {
    static CAGE_BASE: OnceLock<usize> = OnceLock::new();
    // SAFETY: The function has no preconditions; it reserves the cage once and then returns its address.
    *CAGE_BASE.get_or_init(|| unsafe { capi::gc_primitive_storage_cage_base() })
}

fn address_in_cage(offset: usize) -> *mut u8 {
    core::ptr::with_exposed_provenance_mut(cage_base() + offset)
}

/// Storage that the runtime allocated and frees when this is dropped. LibGC only ever changes the offset, the size
/// and the capacity of storage when its owner asks it to, and then reports them, so they are kept here with the
/// address of the first byte, where data blocks read them without calling into LibGC. Like the heap, the storage
/// belongs to the thread that runs the VM.
pub struct OwnedPrimitiveStorage {
    handle: NonZeroU64,
    layout: GCPrimitiveStorageLayout,
    data: *mut u8,
}

const LAYOUT_OF_NO_STORAGE: GCPrimitiveStorageLayout = GCPrimitiveStorageLayout {
    offset: INVALID_OFFSET,
    size: 0,
    capacity: 0,
};

impl OwnedPrimitiveStorage {
    fn from_creation(
        created: bool,
        handle: GCPrimitiveStorageHandle,
        layout: GCPrimitiveStorageLayout,
    ) -> Result<Self, OutOfMemory> {
        if !created {
            assert!(handle == GC_PRIMITIVE_STORAGE_NULL_HANDLE);
            return Err(OutOfMemory);
        }
        let mut storage = Self {
            handle: NonZeroU64::new(handle).expect("created storage has a handle"),
            layout: LAYOUT_OF_NO_STORAGE,
            data: core::ptr::null_mut(),
        };
        storage.set_layout(layout);
        Ok(storage)
    }

    fn set_layout(&mut self, layout: GCPrimitiveStorageLayout) {
        assert!(
            layout.offset != INVALID_OFFSET,
            "LibGC reports the layout of storage it created"
        );
        self.layout = layout;
        self.data = address_in_cage(layout.offset);
    }

    /// PrimitiveStorage::try_allocate(): `size` bytes, which small sizes share pages with other storage for.
    pub fn allocate(size: usize, zero_fill_new_bytes: ZeroFillNewBytes) -> Result<Self, OutOfMemory> {
        let mut handle = GC_PRIMITIVE_STORAGE_NULL_HANDLE;
        let mut layout = LAYOUT_OF_NO_STORAGE;
        // SAFETY: The handle and the layout are valid places for the results.
        let created = unsafe {
            capi::gc_primitive_storage_allocate(size, zero_fill_new_bytes.as_bool(), &raw mut handle, &raw mut layout)
        };
        Self::from_creation(created, handle, layout)
    }

    /// PrimitiveStorage::try_reserve(): `size` bytes in a reservation of `capacity` bytes of its own, which the
    /// storage can grow into without moving.
    pub fn reserve(size: usize, capacity: usize, zero_fill_new_bytes: ZeroFillNewBytes) -> Result<Self, OutOfMemory> {
        let mut handle = GC_PRIMITIVE_STORAGE_NULL_HANDLE;
        let mut layout = LAYOUT_OF_NO_STORAGE;
        // SAFETY: The handle and the layout are valid places for the results.
        let created = unsafe {
            capi::gc_primitive_storage_reserve(
                size,
                capacity,
                zero_fill_new_bytes.as_bool(),
                0,
                &raw mut handle,
                &raw mut layout,
            )
        };
        Self::from_creation(created, handle, layout)
    }

    pub fn handle(&self) -> GCPrimitiveStorageHandle {
        self.handle.get()
    }

    #[inline]
    pub fn offset(&self) -> usize {
        self.layout.offset
    }

    #[inline]
    pub fn size(&self) -> usize {
        self.layout.size
    }

    #[inline]
    pub fn capacity(&self) -> usize {
        self.layout.capacity
    }

    #[inline]
    pub fn data(&self) -> *mut u8 {
        self.data
    }

    /// PrimitiveStorage::try_resize(): changes the size, which moves the bytes to new storage only when it exceeds
    /// the capacity. Fails without changing anything.
    pub fn resize(&mut self, new_size: usize, zero_fill_new_bytes: ZeroFillNewBytes) -> Result<(), OutOfMemory> {
        let mut layout = self.layout;
        // SAFETY: The handle names storage that self owns, and the layout is a valid place for the result.
        let resized = unsafe {
            capi::gc_primitive_storage_resize(
                self.handle.get(),
                new_size,
                zero_fill_new_bytes.as_bool(),
                &raw mut layout,
            )
        };
        if !resized {
            return Err(OutOfMemory);
        }
        self.set_layout(layout);
        Ok(())
    }

    /// PrimitiveStorage::try_reserve(handle, capacity): grows the capacity to at least `new_capacity`, in a
    /// reservation of its own. Fails without changing anything.
    pub fn reserve_capacity(&mut self, new_capacity: usize) -> Result<(), OutOfMemory> {
        let mut layout = self.layout;
        // SAFETY: The handle names storage that self owns, and the layout is a valid place for the result.
        let reserved =
            unsafe { capi::gc_primitive_storage_reserve_capacity(self.handle.get(), new_capacity, &raw mut layout) };
        if !reserved {
            return Err(OutOfMemory);
        }
        self.set_layout(layout);
        Ok(())
    }
}

impl Drop for OwnedPrimitiveStorage {
    fn drop(&mut self) {
        // SAFETY: The handle names storage that self owns and that nothing refers to any more.
        unsafe { capi::gc_primitive_storage_free(self.handle.get()) };
    }
}
