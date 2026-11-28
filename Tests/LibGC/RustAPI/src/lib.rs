/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Cells of a Rust type in a LibGC heap: allocated, traced through a member, held weakly and destroyed. TestGCRustAPI
//! runs this from a C++ test, as Rust code that allocates from a heap has to run in a process that the C++ build links.
//! The cargo test binary of the crate links no LibGC, so the crate is empty there.

#![cfg(not(test))]

#[path = "../../../../Libraries/RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../../Libraries/RustPanic.rs"]
mod rust_panic;

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use libgc_api::capi::{self, CellTypeInfo, GCVisitor};
use libgc_api::cell::{self, Allocator, RustCell};
use libgc_api::layout::{CELL_IDENTITY_HASH_STATE_OFFSET, CELL_KIND_OFFSET, CELL_STATE_OFFSET, check_layout};
use libgc_api::member::{compress, decompress, write_barrier};
use libgc_api::{MemberSlot, Trace, Visitor, WeakImpl};

/// A field that holds a test cell or null.
#[repr(transparent)]
struct TestMember(Cell<u32>);

// SAFETY: The field is a u32 that holds 0 or the compressed pointer of a live test cell.
unsafe impl MemberSlot for TestMember {
    fn slot(&self) -> *const u32 {
        self.0.as_ptr()
    }

    fn cell_address(&self) -> Option<NonNull<c_void>> {
        let compressed = self.0.get();
        if compressed == 0 {
            return None;
        }
        NonNull::new(core::ptr::with_exposed_provenance_mut(decompress(
            capi::gc_heap_region_base(),
            compressed,
        )))
    }
}

// SAFETY: Visits the cell the field names.
unsafe impl Trace for TestMember {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        visitor.visit_member(self);
    }
}

impl TestMember {
    fn set(&self, cell: NonNull<TestCell>) {
        let compressed = compress(capi::gc_heap_region_base(), cell.as_ptr().expose_provenance());
        write_barrier(self.0.as_ptr(), compressed);
        self.0.set(compressed);
    }
}

/// The header of a test cell, as LibGC reads it, after the word that holds the vtable pointer of a C++ cell.
#[repr(C)]
struct CellHeader {
    implementation_word: [u8; 8],
    identity_hash_state: u8,
    state: u8,
    kind: u8,
}

const _: () = assert!(
    core::mem::offset_of!(CellHeader, identity_hash_state) == CELL_IDENTITY_HASH_STATE_OFFSET
        && core::mem::offset_of!(CellHeader, state) == CELL_STATE_OFFSET
        && core::mem::offset_of!(CellHeader, kind) == CELL_KIND_OFFSET
);

impl CellHeader {
    const fn new() -> Self {
        Self {
            implementation_word: [0; 8],
            identity_hash_state: 0,
            state: cell::CELL_STATE_LIVE,
            kind: cell::CELL_KIND_OTHER,
        }
    }
}

// SAFETY: The header holds no cells.
unsafe impl Trace for CellHeader {
    fn trace(&self, _: &mut Visitor) {}
}

#[derive(Trace)]
#[repr(C)]
struct TestCell {
    header: CellHeader,
    next: TestMember,
    #[gc(untraced = "a count the test owns, which outlives every cell")]
    destroyed: *const Cell<u32>,
}

impl Drop for TestCell {
    fn drop(&mut self) {
        // SAFETY: The count outlives every cell.
        let destroyed = unsafe { &*self.destroyed };
        destroyed.set(destroyed.get() + 1);
    }
}

// SAFETY: The type is #[repr(C)], starts with its header and is 24 bytes.
unsafe impl RustCell for TestCell {
    const NAME: &'static str = "TestCell";
}

static TYPE_INFO: CellTypeInfo = cell::type_info_of::<TestCell>(None);

unsafe extern "C" fn gather_no_roots(_context: *mut c_void, _visitor: *mut GCVisitor) {}

#[unsafe(no_mangle)]
pub extern "C" fn libgc_api_test_cells_live_while_reachable_and_are_destroyed_once_not() {
    check_layout();
    let destroyed = Box::new(Cell::new(0));
    // SAFETY: The heap is used from this thread only, and destroyed before the allocator.
    unsafe {
        let heap = capi::gc_heap_create(gather_no_roots, core::ptr::null_mut(), false);
        capi::gc_heap_set_incremental_sweep_enabled(&*heap, false);
        let allocator = Allocator::new(&TYPE_INFO, TestCell::NAME, core::ptr::from_ref(&TYPE_INFO).cast());
        let new_cell = || {
            cell::allocate_with(heap, &allocator, |_| TestCell {
                header: CellHeader::new(),
                next: TestMember(Cell::new(0)),
                destroyed: &raw const *destroyed,
            })
        };

        let root = new_cell();
        // NB: A cell that names itself, as cells whose fields hold their own address are made.
        let reached = cell::allocate_with(heap, &allocator, |cell: NonNull<TestCell>| {
            let next = TestMember(Cell::new(0));
            next.set(cell);
            TestCell {
                header: CellHeader::new(),
                next,
                destroyed: &raw const *destroyed,
            }
        });
        assert_eq!(reached.as_ref().next.cell_address(), Some(reached.cast()));
        assert_eq!(cell::heap_of(reached), heap);
        let unreached = new_cell();
        root.as_ref().next.set(reached);
        let root_handle = capi::gc_root_create(root.as_ptr().cast());
        let weak_reached = WeakImpl::for_cell(reached.cast());
        let weak_unreached = WeakImpl::for_cell(unreached.cast());
        assert_eq!(weak_unreached.cell(), Some(unreached.cast()));

        capi::gc_heap_collect_garbage_without_scanning_the_stack(&*heap);
        assert_eq!(destroyed.get(), 1);
        assert_eq!(weak_unreached.cell(), None);
        assert_eq!(weak_reached.cell(), Some(reached.cast()));

        capi::gc_root_destroy(root_handle);
        capi::gc_heap_collect_garbage_without_scanning_the_stack(&*heap);
        assert_eq!(destroyed.get(), 3);
        assert_eq!(weak_reached.cell(), None);

        drop(weak_reached);
        drop(weak_unreached);
        capi::gc_heap_destroy(heap);
        allocator.destroy();
    }
}
