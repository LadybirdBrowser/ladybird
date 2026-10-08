/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The values of an object's named properties beyond its inline slots, or of its indexed elements.
//!
//! Objects point at the first value. The 8 bytes in front of it hold the capacity (u32) and the kind of the storage
//! (u32): normally a ValueStorage cell, and a malloc allocation for capacities too large for a cell. The interpreter
//! reads the capacity there.
//!
//! The object that owns the storage visits the values it uses and the ValueStorage cell, which visits nothing itself.
//! A cell is owned by one object at a time, and is collected once its object drops it. Storage allocation never
//! collects garbage, as with malloc, so an object may grow its storage in the middle of a change (a new shape set
//! before its storage grows), and callers may hold on to anything across the growth.

use std::alloc::{Layout, handle_alloc_error};

use crate::gc::class::{GcCell, define_cell};
use crate::gc::heap::{Heap, SizeClassAllocator};
use crate::gc::visitor::{Trace, Visitor};
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::object::INDEXED_ELEMENTS_HEADER_SIZE;
use crate::layout::value::Value;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum ValueStorageKind {
    Malloc = 0,
    Cell = 1,
}

#[repr(C)]
pub struct ValueStorage {
    header: CellHeader,
    // NB: Keeps the capacity and the kind right in front of the values.
    _padding: [u8; 5],
    capacity: u32,
    kind: ValueStorageKind,
    values: [Value; 0],
}

define_cell!(ValueStorage, Other);

// SAFETY: The object that owns the storage visits the values it uses.
unsafe impl Trace for ValueStorage {
    fn trace(&self, _: &mut Visitor) {}
}

/// Where the values start in a ValueStorage cell.
pub const VALUES_OFFSET: usize = core::mem::offset_of!(ValueStorage, values);

const _: () = {
    assert!(INDEXED_ELEMENTS_HEADER_SIZE == 2 * size_of::<u32>());
    assert!(core::mem::offset_of!(ValueStorage, capacity) == VALUES_OFFSET - INDEXED_ELEMENTS_HEADER_SIZE);
    assert!(core::mem::offset_of!(ValueStorage, kind) == VALUES_OFFSET - size_of::<u32>());
    assert!(VALUES_OFFSET == size_of::<ValueStorage>());
};

/// Larger storage is a malloc allocation.
pub const MAX_CELL_CAPACITY: u32 = 1024;

/// The capacities of the size classes of ValueStorage cells, ending in MAX_CELL_CAPACITY.
pub const SIZE_CLASS_CAPACITIES: [u32; 17] = [
    4,
    6,
    8,
    12,
    16,
    24,
    32,
    48,
    64,
    96,
    128,
    192,
    256,
    384,
    512,
    768,
    MAX_CELL_CAPACITY,
];

const SIZE_CLASS_CELL_SIZES: [u32; SIZE_CLASS_CAPACITIES.len()] = {
    let mut cell_sizes = [0; SIZE_CLASS_CAPACITIES.len()];
    let mut index = 0;
    while index < SIZE_CLASS_CAPACITIES.len() {
        cell_sizes[index] = (VALUES_OFFSET + SIZE_CLASS_CAPACITIES[index] as usize * size_of::<Value>()) as u32;
        index += 1;
    }
    cell_sizes
};

/// The size classes of ValueStorage cells in `heap`, in the order of SIZE_CLASS_CAPACITIES.
fn size_classes(heap: &Heap) -> &[SizeClassAllocator] {
    heap.size_classes(ValueStorage::CLASS, &SIZE_CLASS_CELL_SIZES)
}

/// Allocates storage for at least `capacity` values (rounded up to its size class), all set to `fill`, and returns the
/// first value. Never collects garbage.
pub fn allocate(heap: &Heap, capacity: u32, fill: Value) -> *mut Value {
    let Some(index) = SIZE_CLASS_CAPACITIES
        .iter()
        .position(|&size_class_capacity| capacity <= size_class_capacity)
    else {
        return malloc::allocate(capacity, fill);
    };
    let storage = heap.allocate_storage_with_size_class(
        size_classes(heap)[index],
        ValueStorage {
            header: CellHeader::for_class(ValueStorage::CLASS),
            _padding: [0; 5],
            capacity: SIZE_CLASS_CAPACITIES[index],
            kind: ValueStorageKind::Cell,
            values: [],
        },
        fill,
    );
    // SAFETY: The values follow the cell's header.
    unsafe { storage.as_ptr().cast::<u8>().add(VALUES_OFFSET).cast::<Value>() }
}

/// Frees malloc storage. Cells are collected instead.
///
/// # Safety
///
/// `values` must be storage that allocate() returned, and that nothing uses any more.
pub unsafe fn deallocate(values: *mut Value) {
    if kind(values) == ValueStorageKind::Malloc {
        // SAFETY: Passed on from the caller.
        unsafe { malloc::deallocate(values) };
    }
}

pub fn capacity(values: *const Value) -> u32 {
    // SAFETY: allocate() put the capacity in front of the first value.
    unsafe {
        values
            .cast::<u8>()
            .sub(INDEXED_ELEMENTS_HEADER_SIZE)
            .cast::<u32>()
            .read()
    }
}

pub fn kind(values: *const Value) -> ValueStorageKind {
    // SAFETY: allocate() put the kind in front of the first value, right after the capacity.
    unsafe {
        values
            .cast::<u8>()
            .sub(size_of::<u32>())
            .cast::<ValueStorageKind>()
            .read()
    }
}

/// The cell holding the values, or None for malloc storage.
pub fn cell(values: *mut Value) -> Option<Gc<ValueStorage>> {
    if kind(values) != ValueStorageKind::Cell {
        return None;
    }
    // SAFETY: Cell storage starts VALUES_OFFSET bytes in front of the values.
    Some(unsafe { Gc::from_non_null(core::ptr::NonNull::new_unchecked(values.cast::<u8>().sub(VALUES_OFFSET)).cast()) })
}

/// The memory the storage takes outside the GC heap.
pub fn external_memory_size_of(values: *const Value) -> usize {
    if kind(values) != ValueStorageKind::Malloc {
        return 0;
    }
    malloc::allocation_size(capacity(values))
}

/// Storage beyond MAX_CELL_CAPACITY: [u32 capacity] [u32 kind] [Value 0] [Value 1] ...
mod malloc {
    use super::*;

    // Allocating through AK puts the storage in its heap partition for JS object storage, apart from every other
    // allocation, as C++ objects keep theirs. That partition belongs to the thread that first allocates from it, so
    // the standalone tests, which run each test on a thread of its own, use the global allocator instead.
    #[cfg(feature = "allocator")]
    mod raw {
        unsafe extern "C" {
            fn ladybird_js_object_storage_alloc(size: usize) -> *mut u8;
            fn ladybird_dealloc(pointer: *mut u8, alignment: usize);
        }

        pub unsafe fn allocate(layout: super::Layout) -> *mut u8 {
            // SAFETY: Any size can be allocated, and AK aligns every allocation for a Value.
            unsafe { ladybird_js_object_storage_alloc(layout.size()) }
        }

        pub unsafe fn deallocate(pointer: *mut u8, layout: super::Layout) {
            // SAFETY: The caller passes storage that allocate() returned.
            unsafe { ladybird_dealloc(pointer, layout.align()) }
        }
    }

    #[cfg(not(feature = "allocator"))]
    mod raw {
        pub use std::alloc::{alloc as allocate, dealloc as deallocate};
    }

    fn layout(capacity: u32) -> Layout {
        Layout::from_size_align(allocation_size(capacity), align_of::<Value>()).expect("the storage layout is valid")
    }

    pub fn allocation_size(capacity: u32) -> usize {
        INDEXED_ELEMENTS_HEADER_SIZE + capacity as usize * size_of::<Value>()
    }

    pub fn allocate(capacity: u32, fill: Value) -> *mut Value {
        let layout = layout(capacity);
        // SAFETY: The layout has room for at least the header.
        let raw = unsafe { raw::allocate(layout) };
        if raw.is_null() {
            handle_alloc_error(layout);
        }
        // SAFETY: The allocation starts with the header, and the values follow it.
        unsafe {
            raw.cast::<u32>().write(capacity);
            raw.add(size_of::<u32>())
                .cast::<ValueStorageKind>()
                .write(ValueStorageKind::Malloc);
            let values = raw.add(INDEXED_ELEMENTS_HEADER_SIZE).cast::<Value>();
            for index in 0..capacity as usize {
                values.add(index).write(fill);
            }
            values
        }
    }

    /// # Safety
    ///
    /// `values` must be malloc storage that allocate() returned.
    pub unsafe fn deallocate(values: *mut Value) {
        let layout = layout(capacity(values));
        // SAFETY: The header is part of the same allocation, which the caller passes.
        unsafe { raw::deallocate(values.cast::<u8>().sub(INDEXED_ELEMENTS_HEADER_SIZE), layout) };
    }
}
