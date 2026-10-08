/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Declarations of Libraries/LibGC/CAPI.h.

use core::ffi::{c_char, c_int, c_void};

use super::class::CellTypeInfo;

#[repr(C)]
pub struct GCHeap {
    _private: [u8; 0],
}

#[repr(C)]
pub struct GCVisitor {
    _private: [u8; 0],
}

#[repr(C)]
pub struct GCAllocator {
    _private: [u8; 0],
}

#[repr(C)]
pub struct GCRoot {
    _private: [u8; 0],
}

#[repr(C)]
pub struct GCWeakImpl {
    _private: [u8; 0],
}

#[repr(C)]
pub struct GCSharedMemoryViewOutsideCage {
    _private: [u8; 0],
}

pub const GC_COLLECTION_TYPE_COLLECT_GARBAGE: i32 = 0;
pub const GC_COLLECTION_TYPE_COLLECT_EVERYTHING: i32 = 1;

#[derive(Default)]
#[repr(C)]
pub struct GCLayout {
    pub cell_mark_offset: u32,
    pub cell_state_offset: u32,
    pub cell_kind_offset: u32,
    pub min_cell_size: u32,
    pub max_cell_size: u32,
    pub max_cell_alignment: u32,
    pub cell_type_info_size: u32,
    pub weak_impl_pointer_offset: u32,
    pub heap_region_offset_mask: u64,
    pub primitive_storage_cage_offset_mask: u64,
}

/// The generation of the storage's table entry in the high 32 bits and the entry's index in the low 32 bits.
pub type GCPrimitiveStorageHandle = u64;
pub const GC_PRIMITIVE_STORAGE_NULL_HANDLE: GCPrimitiveStorageHandle = 0;
pub const GC_PRIMITIVE_STORAGE_INVALID_OFFSET: usize = usize::MAX;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct GCPrimitiveStorageLayout {
    pub offset: usize,
    pub size: usize,
    pub capacity: usize,
}

pub type GCCallback = unsafe extern "C" fn(context: *mut c_void);
pub type GCGatherRootsCallback = unsafe extern "C" fn(context: *mut c_void, root_visitor: *mut GCVisitor);

unsafe extern "C" {
    pub fn gc_get_layout(layout: *mut GCLayout);
    pub fn gc_heap_region_base() -> usize;
    pub fn gc_primitive_storage_cage_base() -> usize;

    pub fn gc_heap_create(
        gather_roots: GCGatherRootsCallback,
        context: *mut c_void,
        become_process_default: bool,
    ) -> *mut GCHeap;
    pub fn gc_heap_destroy(heap: *mut GCHeap);
    pub fn gc_heap_collect_garbage(heap: *mut GCHeap, collection_type: i32, print_report: bool);
    pub fn gc_heap_is_collecting_everything(heap: *const GCHeap) -> bool;
    pub fn gc_heap_defer_gc(heap: *mut GCHeap);
    pub fn gc_heap_undefer_gc(heap: *mut GCHeap);
    pub fn gc_heap_should_collect_on_every_allocation(heap: *const GCHeap) -> bool;
    pub fn gc_heap_set_should_collect_on_every_allocation(heap: *mut GCHeap, should_collect: bool);
    pub fn gc_heap_set_incremental_sweep_enabled(heap: *mut GCHeap, enabled: bool);
    pub fn gc_heap_uproot_cell(heap: *mut GCHeap, cell: *mut c_void);
    pub fn gc_heap_stack_bounds(heap: *const GCHeap, base: *mut usize, top: *mut usize);
    pub fn gc_heap_enqueue_post_gc_task(heap: *mut GCHeap, callback: GCCallback, context: *mut c_void);
    pub fn gc_heap_register_sweep_callback(heap: *mut GCHeap, callback: GCCallback, context: *mut c_void);
    pub fn gc_heap_did_allocate_external_memory(heap: *mut GCHeap, size: usize);
    pub fn gc_heap_did_free_external_memory(heap: *mut GCHeap, size: usize);

    pub fn gc_allocator_create(
        type_info: *const CellTypeInfo,
        name: *const c_char,
        name_length: usize,
    ) -> *mut GCAllocator;
    pub fn gc_allocator_destroy(allocator: *mut GCAllocator);
    pub fn gc_heap_allocate_cell(heap: *mut GCHeap, allocator: *mut GCAllocator, must_mark: *mut bool) -> *mut c_void;
    pub fn gc_cell_type_info(cell: *const c_void) -> *const CellTypeInfo;

    pub fn gc_root_create(cell: *mut c_void) -> *mut GCRoot;
    pub fn gc_root_destroy(root: *mut GCRoot);
    pub fn gc_root_cell(root: *const GCRoot) -> *mut c_void;

    pub fn gc_heap_create_weak_impl(heap: *mut GCHeap, cell: *mut c_void) -> *mut GCWeakImpl;
    pub fn gc_weak_impl_null() -> *mut GCWeakImpl;
    pub fn gc_weak_impl_ref(weak_impl: *mut GCWeakImpl);
    pub fn gc_weak_impl_unref(weak_impl: *mut GCWeakImpl);

    pub fn gc_visitor_visit_cell(visitor: *mut GCVisitor, cell: *mut c_void);
    pub fn gc_visitor_visit_cells(visitor: *mut GCVisitor, cells: *const *mut c_void, count: usize);
    pub fn gc_visitor_visit_values(visitor: *mut GCVisitor, values: *const u64, count: usize);
    pub fn gc_visitor_visit_possible_values(visitor: *mut GCVisitor, data: *const u8, size: usize);

    pub fn gc_primitive_storage_allocate(
        size: usize,
        zero_fill: bool,
        out_handle: *mut GCPrimitiveStorageHandle,
        out_layout: *mut GCPrimitiveStorageLayout,
    ) -> bool;
    pub fn gc_primitive_storage_reserve(
        size: usize,
        capacity: usize,
        zero_fill: bool,
        guard_size: usize,
        out_handle: *mut GCPrimitiveStorageHandle,
        out_layout: *mut GCPrimitiveStorageLayout,
    ) -> bool;
    pub fn gc_primitive_storage_adopt_shared_fd(
        fd: c_int,
        size: usize,
        out_handle: *mut GCPrimitiveStorageHandle,
        out_layout: *mut GCPrimitiveStorageLayout,
    ) -> bool;
    pub fn gc_primitive_storage_resize(
        handle: GCPrimitiveStorageHandle,
        new_size: usize,
        zero_fill: bool,
        out_layout: *mut GCPrimitiveStorageLayout,
    ) -> bool;
    pub fn gc_primitive_storage_reserve_capacity(
        handle: GCPrimitiveStorageHandle,
        new_capacity: usize,
        out_layout: *mut GCPrimitiveStorageLayout,
    ) -> bool;
    pub fn gc_primitive_storage_resize_and_reserve(
        handle: GCPrimitiveStorageHandle,
        new_size: usize,
        new_capacity: usize,
        zero_fill: bool,
        out_layout: *mut GCPrimitiveStorageLayout,
    ) -> bool;
    pub fn gc_primitive_storage_free(handle: GCPrimitiveStorageHandle);
    pub fn gc_primitive_storage_is_valid(handle: GCPrimitiveStorageHandle) -> bool;
    pub fn gc_primitive_storage_offset(handle: GCPrimitiveStorageHandle) -> usize;
    pub fn gc_primitive_storage_size(handle: GCPrimitiveStorageHandle) -> usize;
    pub fn gc_primitive_storage_capacity(handle: GCPrimitiveStorageHandle) -> usize;
    pub fn gc_primitive_storage_committed_size(handle: GCPrimitiveStorageHandle) -> usize;
    pub fn gc_primitive_storage_data(handle: GCPrimitiveStorageHandle) -> *mut u8;

    pub fn gc_shared_memory_create(size: usize, out_fd: *mut c_int) -> bool;
    pub fn gc_shared_memory_view_outside_cage_create(fd: c_int, size: usize) -> *mut GCSharedMemoryViewOutsideCage;
    pub fn gc_shared_memory_view_outside_cage_data(view: *mut GCSharedMemoryViewOutsideCage) -> *mut u8;
    pub fn gc_shared_memory_view_outside_cage_destroy(view: *mut GCSharedMemoryViewOutsideCage);
}

// On Windows, a variable that a DLL exports is only reachable through the DLL's import table, which the reference has
// to go through.
#[cfg_attr(windows, link(name = "lagom-gc", kind = "dylib"))]
unsafe extern "C" {
    /// The base NaN-boxed cell values are relative to; zero until gc_heap_region_base() first runs.
    pub static js_heap_region_base: usize;
}
