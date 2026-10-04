/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GC::PrimitiveStorage through LibGC's C interface: byte buffers inside the one cage of the process, which the
//! interpreter addresses as offsets from the cage base. Like the heap, it belongs to the thread that runs the VM, so
//! its storage never leaves that thread.

use core::marker::PhantomData;
use core::num::NonZeroU64;
use std::os::fd::{AsRawFd, BorrowedFd, FromRawFd, OwnedFd};
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

    /// PrimitiveStorage::try_adopt_shared_fd(): maps the first `size` bytes of a shared memory object into the cage.
    /// The mapping keeps the memory alive by itself. Such storage must never be resized, since that would replace the
    /// shared mapping with private memory.
    pub fn adopt_shared_memory(shared_memory: BorrowedFd<'_>, size: usize) -> Result<Self, OutOfMemory> {
        let mut handle = GC_PRIMITIVE_STORAGE_NULL_HANDLE;
        let mut layout = LAYOUT_OF_NO_STORAGE;
        // SAFETY: The descriptor is open for the duration of the call, which maps it without taking ownership, and the
        //         handle and the layout are valid places for the results.
        let created = unsafe {
            capi::gc_primitive_storage_adopt_shared_fd(
                shared_memory.as_raw_fd(),
                size,
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

/// Storage that an embedder owns and may resize or free at any time, so every query goes to LibGC, which reports a
/// handle that names no storage as having no bytes at the invalid offset.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ForeignPrimitiveStorage {
    handle: GCPrimitiveStorageHandle,
    _owned_by_the_thread_of_the_vm: PhantomData<*mut u8>,
}

impl ForeignPrimitiveStorage {
    pub fn new(handle: GCPrimitiveStorageHandle) -> Self {
        Self {
            handle,
            _owned_by_the_thread_of_the_vm: PhantomData,
        }
    }

    pub fn handle(self) -> GCPrimitiveStorageHandle {
        self.handle
    }

    pub fn is_valid(self) -> bool {
        // SAFETY: Any handle may be queried.
        unsafe { capi::gc_primitive_storage_is_valid(self.handle) }
    }

    pub fn offset(self) -> usize {
        // SAFETY: Any handle may be queried.
        unsafe { capi::gc_primitive_storage_offset(self.handle) }
    }

    pub fn size(self) -> usize {
        // SAFETY: Any handle may be queried.
        unsafe { capi::gc_primitive_storage_size(self.handle) }
    }

    pub fn capacity(self) -> usize {
        // SAFETY: Any handle may be queried.
        unsafe { capi::gc_primitive_storage_capacity(self.handle) }
    }

    /// The first byte, or null if the handle names no storage.
    pub fn data(self) -> *mut u8 {
        match self.offset() {
            INVALID_OFFSET => core::ptr::null_mut(),
            offset => address_in_cage(offset),
        }
    }
}

/// gc_shared_memory_create(): a zero-filled shared memory object of `size` bytes that other processes can map, sealed
/// against resizing where the platform can seal it. The descriptor closes when the result is dropped.
pub fn create_shared_memory(size: usize) -> Result<OwnedFd, OutOfMemory> {
    let mut descriptor = -1;
    // SAFETY: The descriptor is a valid place for the result.
    if !unsafe { capi::gc_shared_memory_create(size, &raw mut descriptor) } {
        return Err(OutOfMemory);
    }
    // SAFETY: On success, the caller owns the new descriptor.
    Ok(unsafe { OwnedFd::from_raw_fd(descriptor) })
}
