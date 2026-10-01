/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ptr::NonNull;

use crate::layout_forward::Class;

/// A pointer to a garbage-collected cell. Cells are kept alive by being reachable from the heap's roots or from the
/// stack, which the collector scans conservatively, so a `Gc` needs no rooting while it lives in a local.
#[repr(transparent)]
pub struct Gc<T: ?Sized>(NonNull<T>);

impl<T: ?Sized> Gc<T> {
    /// # Safety
    ///
    /// `pointer` must point to a live cell of type `T`.
    pub const unsafe fn from_non_null(pointer: NonNull<T>) -> Self {
        Self(pointer)
    }

    pub const fn as_non_null(self) -> NonNull<T> {
        self.0
    }

    pub fn as_ptr(self) -> *mut T {
        self.0.as_ptr()
    }
}

impl<T: ?Sized> Clone for Gc<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T: ?Sized> Copy for Gc<T> {}

impl<T: ?Sized> PartialEq for Gc<T> {
    fn eq(&self, other: &Self) -> bool {
        core::ptr::addr_eq(self.0.as_ptr(), other.0.as_ptr())
    }
}

impl<T: ?Sized> Eq for Gc<T> {}

/// Mirrors GC::CellKind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum CellKind {
    Other,
    Object,
    PrimitiveString,
    Symbol,
    BigInt,
    Accessor,
}

/// Mirrors GC::Cell::State.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum CellState {
    Live,
    Dead,
}

/// The bytes LibGC shares with every cell. The first word is the vtable pointer of a C++ cell, which LibGC never
/// reads; Rust cells keep their class there. The header is packed so that a cell's own fields can use the bytes that
/// follow it, the same way C++ cells reuse the base class's tail padding.
#[repr(C, packed)]
pub struct CellHeader {
    pub class: &'static Class,
    pub mark: Cell<bool>,
    pub state: Cell<CellState>,
    pub kind: CellKind,
}

const _: () = assert!(size_of::<CellHeader>() == 11);
const _: () = assert!(align_of::<CellHeader>() == 1);
