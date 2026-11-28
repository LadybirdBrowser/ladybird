/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What Rust knows of the layout of LibGC's types, which check_layout() compares with what gc_get_layout() reports.

use crate::capi::{self, GCLayout};

/// Where a visitor keeps the marker it marks for, right after the vtable pointer of the C++ Cell::Visitor.
pub const VISITOR_MARKER_OFFSET: usize = size_of::<usize>();

/// Where a GC::WeakImpl keeps the address of its cell, which LibGC clears once the cell is collected.
pub const WEAK_IMPL_POINTER_OFFSET: usize = 16;

/// Where a cell keeps the state of its identity hash, its State and its CellKind, after the word that holds the vtable
/// pointer of a C++ cell, or that belongs to the foreign implementation of a cell defined outside of C++.
pub const CELL_IDENTITY_HASH_STATE_OFFSET: usize = 8;
pub const CELL_STATE_OFFSET: usize = 9;
pub const CELL_KIND_OFFSET: usize = 10;

/// The size of the smallest cell LibGC allocates.
pub const MIN_CELL_SIZE: usize = 24;

/// The bits of a cell's identity hash state, see gc_cell_identity_hash().
pub const IDENTITY_HASH_HASHED: u8 = 1 << 0;
pub const IDENTITY_HASH_MOVED: u8 = 1 << 1;

/// The layout LibGC reports, once it is checked against the constants of this module.
pub fn check_layout() -> GCLayout {
    let mut layout = GCLayout::default();
    capi::gc_get_layout(&mut layout, size_of::<GCLayout>());
    let expect = |what: &str, actual: u64, expected: u64| {
        assert!(
            actual == expected,
            "LibGC's {what} is {actual}, Rust expects {expected}"
        );
    };
    expect(
        "visitor marker offset",
        layout.visitor_marker_offset.into(),
        VISITOR_MARKER_OFFSET as u64,
    );
    expect(
        "weak impl pointer offset",
        layout.weak_impl_pointer_offset.into(),
        WEAK_IMPL_POINTER_OFFSET as u64,
    );
    expect(
        "identity hash state offset",
        layout.identity_hash_state_offset.into(),
        CELL_IDENTITY_HASH_STATE_OFFSET as u64,
    );
    expect(
        "cell state offset",
        layout.cell_state_offset.into(),
        CELL_STATE_OFFSET as u64,
    );
    expect(
        "cell kind offset",
        layout.cell_kind_offset.into(),
        CELL_KIND_OFFSET as u64,
    );
    expect(
        "identity hash bits",
        u64::from(layout.identity_hash_hashed) | (u64::from(layout.identity_hash_moved) << 8),
        u64::from(IDENTITY_HASH_HASHED) | (u64::from(IDENTITY_HASH_MOVED) << 8),
    );
    expect("minimum cell size", layout.min_cell_size.into(), MIN_CELL_SIZE as u64);
    expect(
        "cell type info size",
        layout.cell_type_info_size.into(),
        size_of::<capi::CellTypeInfo>() as u64,
    );
    layout
}
