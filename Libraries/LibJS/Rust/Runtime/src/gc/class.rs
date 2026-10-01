/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::{c_char, c_void};

use super::capi::GCVisitor;
use super::class_id::ClassId;
use super::visitor::{Trace, Visitor};
use crate::layout::cell::{CellHeader, CellKind, CellState};

/// Mirrors GCCellTypeInfo from Libraries/LibGC/CAPI.h, which LibGC dispatches every per-cell operation through.
#[repr(C)]
pub struct CellTypeInfo {
    pub cell_size: u32,
    pub alignment: u32,
    pub kind: u8,
    pub visit_edges: unsafe extern "C" fn(cell: *mut c_void, visitor: *mut GCVisitor),
    pub finalize: Option<unsafe extern "C" fn(cell: *mut c_void)>,
    pub destroy: Option<unsafe extern "C" fn(cell: *mut c_void)>,
    pub external_memory_size: Option<unsafe extern "C" fn(cell: *const c_void) -> usize>,
    pub class_name: Option<unsafe extern "C" fn(cell: *const c_void, length: *mut usize) -> *const c_char>,
}

/// The class of a cell, stored in the first word of every cell. It starts with the type info LibGC dispatches
/// through, so the class of a cell and the type info of the block it lives in are the same address.
#[repr(C)]
pub struct Class {
    pub type_info: CellTypeInfo,
    pub name: &'static str,
    pub id: ClassId,
}

/// LibGC hands out cells in the slots of fixed-size blocks, whose free list needs room for its own entry in each.
const MIN_CELL_SIZE: usize = 24;
const MAX_CELL_ALIGNMENT: usize = 16;

impl Class {
    pub const fn new<T: Trace>(name: &'static str, id: ClassId, kind: CellKind) -> Self {
        assert!(size_of::<T>() >= MIN_CELL_SIZE);
        assert!(size_of::<T>().is_multiple_of(8));
        assert!(align_of::<T>() <= MAX_CELL_ALIGNMENT);
        Self {
            type_info: CellTypeInfo {
                cell_size: size_of::<T>() as u32,
                alignment: align_of::<T>() as u32,
                kind: kind as u8,
                visit_edges: visit_edges::<T>,
                finalize: None,
                destroy: if core::mem::needs_drop::<T>() {
                    Some(destroy::<T>)
                } else {
                    None
                },
                external_memory_size: None,
                class_name: Some(class_name),
            },
            name,
            id,
        }
    }

    pub fn kind(&self) -> CellKind {
        // SAFETY: The kind was stored from a CellKind.
        unsafe { core::mem::transmute::<u8, CellKind>(self.type_info.kind) }
    }
}

unsafe extern "C" fn visit_edges<T: Trace>(cell: *mut c_void, visitor: *mut GCVisitor) {
    // SAFETY: LibGC passes a live cell of this class and a visitor that outlives the call.
    let (cell, mut visitor) = unsafe { (&*cell.cast::<T>(), Visitor::from_raw(visitor)) };
    cell.trace(&mut visitor);
}

unsafe extern "C" fn destroy<T>(cell: *mut c_void) {
    // SAFETY: LibGC destroys each dead cell of this class exactly once.
    unsafe { core::ptr::drop_in_place(cell.cast::<T>()) };
}

unsafe extern "C" fn class_name(cell: *const c_void, length: *mut usize) -> *const c_char {
    // SAFETY: Every Rust cell starts with its class.
    let class = unsafe { cell.cast::<&'static Class>().read() };
    // SAFETY: LibGC passes somewhere to store the length.
    unsafe { length.write(class.name.len()) };
    class.name.as_ptr().cast()
}

/// A type whose values live in the garbage-collected heap.
///
/// # Safety
///
/// The type must be #[repr(C)] and start with its cell header, either directly or through the cell it extends.
pub unsafe trait GcCell: Trace {
    const CLASS: &'static Class;
}

impl CellHeader {
    pub fn for_class(class: &'static Class) -> Self {
        Self {
            class,
            mark: false.into(),
            state: CellState::Live.into(),
            kind: class.kind(),
        }
    }
}

/// Defines the class of a cell type, named after the type and its ClassId.
macro_rules! define_cell {
    ($type:ident, $kind:ident) => {
        const _: () = {
            static CLASS: $crate::gc::class::Class = $crate::gc::class::Class::new::<$type>(
                stringify!($type),
                $crate::gc::class_id::ClassId::$type,
                $crate::layout::cell::CellKind::$kind,
            );

            // SAFETY: Checked by the asserts in Class::new and the cell's #[repr(C)] layout.
            unsafe impl $crate::gc::class::GcCell for $type {
                const CLASS: &'static $crate::gc::class::Class = &CLASS;
            }
        };
    };
}

pub(crate) use define_cell;
