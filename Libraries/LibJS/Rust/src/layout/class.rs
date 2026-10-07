/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A mirror of the start of gc::class::Class, which build.rs cannot see, up to the fields an embedder reads to tell
//! the class of a cell. gc::class checks that the mirror matches.

use core::ffi::c_void;

/// Mirrors gc::class::CellTypeInfo, GCCellTypeInfo of Libraries/LibGC/CAPI.h.
#[repr(C)]
pub struct CellTypeInfoLayout {
    pub cell_size: u32,
    pub alignment: u32,
    pub kind: u8,
    pub visit_edges: *const c_void,
    pub finalize: *const c_void,
    pub destroy: *const c_void,
    pub external_memory_size: *const c_void,
    pub class_name: *const c_void,
}

/// Mirrors gc::class::Class up to the class it extends.
#[repr(C)]
pub struct ClassLayout {
    pub type_info: CellTypeInfoLayout,
    pub name: &'static str,
    pub id: u16,
    pub parent: *const ClassLayout,
}
