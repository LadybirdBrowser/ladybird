/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ptr::NonNull;

use super::capi::{
    GCMarker, GCVisitor, gc_marker_pin_cell, gc_marker_visit_cell, gc_marker_visit_ephemeron_without_pinning,
    gc_marker_visit_interior_pointer, gc_marker_visit_member, gc_marker_visit_owned_values,
    gc_marker_visit_self_pointer, gc_marker_visit_values, gc_visitor_visit_cell, gc_visitor_visit_ephemeron,
    gc_visitor_visit_values,
};
use crate::layout::cell::Gc;
use crate::layout::member::{Member, NullableMember};
use crate::layout::value::Value;

/// A LibGC visitor, which marks what it visits during a collection and roots it while roots are gathered.
pub struct Visitor(NonNull<GCVisitor>);

/// Where a visitor keeps the marker it marks for, right after the vtable pointer of the C++ Cell::Visitor, which
/// check_layout() checks against GCLayout::visitor_marker_offset.
pub(crate) const VISITOR_MARKER_OFFSET: usize = size_of::<usize>();

impl Visitor {
    /// The marker of a visitor that marks for a collection, which takes the cells it visits directly.
    #[inline(always)]
    fn marker(&self) -> Option<NonNull<GCMarker>> {
        // SAFETY: The visitor is a live Cell::Visitor, which keeps a marker pointer at this offset.
        NonNull::new(unsafe {
            self.0
                .as_ptr()
                .cast::<u8>()
                .add(VISITOR_MARKER_OFFSET)
                .cast::<*mut GCMarker>()
                .read()
        })
    }

    /// # Safety
    ///
    /// `visitor` must be a visitor LibGC passed in, and outlive the returned value.
    pub unsafe fn from_raw(visitor: *mut GCVisitor) -> Self {
        Self(NonNull::new(visitor).expect("LibGC passes a visitor"))
    }

    /// The LibGC visitor, for visiting what the visitor's methods cannot name, such as an embedder's C++ cells.
    pub fn as_raw(&self) -> *mut GCVisitor {
        self.0.as_ptr()
    }

    /// Visits a cell by value, which keeps it where it is in a collection that moves cells, since the collector cannot
    /// update the copy of the pointer that the caller keeps. Edges that live in a cell should be visited through their
    /// slot instead, with visit_member(), visit_nullable_member() or visit_values().
    pub fn visit<T>(&mut self, cell: Gc<T>) {
        if let Some(marker) = self.marker() {
            // SAFETY: The marker is the visitor's, and a Gc points to a live cell.
            unsafe { gc_marker_visit_cell(marker.as_ptr(), cell.as_ptr().cast()) };
            return;
        }
        // SAFETY: The visitor is live, and a Gc points to a live cell.
        unsafe { gc_visitor_visit_cell(self.0.as_ptr(), cell.as_ptr().cast()) };
    }

    /// Visits the cell a member names. It takes the slot, which a collection that moves the cell rewrites if the slot
    /// is in a cell. A slot elsewhere keeps the cell where it is.
    pub fn visit_member<T>(&mut self, slot: &Member<T>) {
        if let Some(marker) = self.marker() {
            // SAFETY: The marker is the visitor's, and the slot holds a compressed pointer to a live cell.
            unsafe { gc_marker_visit_member(marker.as_ptr(), core::ptr::from_ref(slot).cast()) };
            return;
        }
        self.visit(slot.get());
    }

    /// Visits the cell a member names, if it is not null, like visit_member().
    pub fn visit_nullable_member<T>(&mut self, slot: &NullableMember<T>) {
        if let Some(marker) = self.marker() {
            // SAFETY: The marker is the visitor's, and the slot holds 0 or a compressed pointer to a live cell.
            unsafe { gc_marker_visit_member(marker.as_ptr(), core::ptr::from_ref(slot).cast()) };
            return;
        }
        if let Some(cell) = slot.get() {
            self.visit(cell);
        }
    }

    /// Visits `cell`, which the raw pointer in `slot` points into, past its start, as an object points into the cell
    /// that holds its property storage. A collection that moves the cell rewrites the slot if it is in a cell.
    pub fn visit_interior_pointer<T, U>(&mut self, slot: &Cell<*mut T>, cell: Gc<U>) {
        if let Some(marker) = self.marker() {
            // SAFETY: The marker is the visitor's, the slot holds a pointer, and the cell is live.
            unsafe { gc_marker_visit_interior_pointer(marker.as_ptr(), slot.as_ptr().cast(), cell.as_ptr().cast()) };
            return;
        }
        self.visit(cell);
    }

    /// Notes that the raw pointer in `slot` points into the cell that holds the slot, up to one past its end, as an
    /// object points to its inline property storage. A collection that moves the cell points the slot into the copy.
    pub fn visit_self_pointer<T>(&mut self, slot: &Cell<*mut T>) {
        if let Some(marker) = self.marker() {
            // SAFETY: The marker is the visitor's, and the slot is in the cell being visited.
            unsafe { gc_marker_visit_self_pointer(marker.as_ptr(), slot.as_ptr().cast()) };
        }
    }

    /// Keeps `cell` where it is in a collection that moves cells, without keeping it alive, for a weak reference that
    /// the collector cannot update.
    pub fn pin<T: ?Sized>(&mut self, cell: Gc<T>) {
        if let Some(marker) = self.marker() {
            // SAFETY: The marker is the visitor's, and the cell is live.
            unsafe { gc_marker_pin_cell(marker.as_ptr(), cell.as_ptr().cast()) };
        }
    }

    pub fn visit_values(&mut self, values: &[Value]) {
        if let Some(marker) = self.marker() {
            if !values.is_empty() {
                // SAFETY: The marker is the visitor's, and Value is a NaN-boxed u64.
                unsafe { gc_marker_visit_values(marker.as_ptr(), values.as_ptr().cast(), values.len()) };
            }
            return;
        }
        // SAFETY: The visitor is live, and Value is a NaN-boxed u64.
        unsafe { gc_visitor_visit_values(self.0.as_ptr(), values.as_ptr().cast(), values.len()) };
    }

    /// Visits values in memory that the cell being visited owns outside the heap, such as malloc storage, which a
    /// collection that moves cells rewrites where they are rather than keeping their cells where they are.
    ///
    /// # Safety
    ///
    /// Nothing may free, move or write the values until the collection is over: no finalizer, destructor or sweep
    /// callback may touch them.
    pub unsafe fn visit_owned_values(&mut self, values: &[Value]) {
        if let Some(marker) = self.marker() {
            if !values.is_empty() {
                // SAFETY: The marker is the visitor's, Value is a NaN-boxed u64, and the caller vouches for the memory.
                unsafe { gc_marker_visit_owned_values(marker.as_ptr(), values.as_ptr().cast(), values.len()) };
            }
            return;
        }
        // SAFETY: The visitor is live, and Value is a NaN-boxed u64.
        unsafe { gc_visitor_visit_values(self.0.as_ptr(), values.as_ptr().cast(), values.len()) };
    }

    /// Visits a value that is reachable only while `key` is. A collection that moves cells keeps both where they are.
    pub fn visit_ephemeron<T>(&mut self, key: Gc<T>, value: &Value) {
        // SAFETY: The visitor is live, a Gc points to a live cell, and Value is a NaN-boxed u64.
        unsafe { gc_visitor_visit_ephemeron(self.0.as_ptr(), key.as_ptr().cast(), core::ptr::from_ref(value).cast()) };
    }

    /// Visits `value`, a cell that is reachable only while `key` is, by value: it stays where it is in a collection
    /// that moves cells, even if it dies. Visitors that do not mark see the value as reachable.
    pub fn visit_ephemeron_cell<K, V>(&mut self, key: Gc<K>, value: Gc<V>) {
        let Some(marker) = self.marker() else {
            self.visit(value);
            return;
        };
        // SAFETY: The marker is the visitor's, and both are live cells. The value is pinned, so no slot needs to be
        //         handed to the collection. The key is only looked at, and moves of it change nothing the caller
        //         holds.
        unsafe {
            gc_marker_pin_cell(marker.as_ptr(), value.as_ptr().cast());
            gc_marker_visit_ephemeron_without_pinning(marker.as_ptr(), key.as_ptr().cast(), value.as_ptr().cast());
        }
    }

    /// Visits a value that is reachable only while `key` is, such as a WeakMap entry's, without keeping either from
    /// moving.
    ///
    /// # Safety
    ///
    /// If the entry survives a collection that moves cells, its holder must hand the slots of the key and the value to
    /// Sweep::record_slot() and Sweep::record_value_slot().
    pub unsafe fn visit_ephemeron_without_pinning<T>(&mut self, key: Gc<T>, value: &Value) {
        let Some(marker) = self.marker() else {
            self.visit_ephemeron(key, value);
            return;
        };
        if value.is_cell() {
            // SAFETY: The marker is the visitor's, and both are live cells.
            unsafe {
                gc_marker_visit_ephemeron_without_pinning(
                    marker.as_ptr(),
                    key.as_ptr().cast(),
                    value.as_cell().as_ptr().cast(),
                );
            }
        }
    }
}

/// A value that can reach cells, which it reports to the visitor.
///
/// # Safety
///
/// `trace` must visit every cell the value can reach directly; a cell it misses can be collected while still in use.
pub unsafe trait Trace {
    fn trace(&self, visitor: &mut Visitor);

    /// Traces a slice of values, which some types can report in a single call.
    fn trace_slice(values: &[Self], visitor: &mut Visitor)
    where
        Self: Sized,
    {
        for value in values {
            value.trace(visitor);
        }
    }
}

/// Implements Trace for types that cannot reach any cell.
macro_rules! impl_trace_for_cell_free_types {
    ($($type:ty),* $(,)?) => {
        $(
            // SAFETY: The type holds no cells.
            unsafe impl Trace for $type {
                fn trace(&self, _: &mut Visitor) {}

                fn trace_slice(_: &[Self], _: &mut Visitor) {}
            }
        )*
    };
}

impl_trace_for_cell_free_types!(
    (),
    bool,
    char,
    u8,
    u16,
    u32,
    u64,
    usize,
    i8,
    i16,
    i32,
    i64,
    isize,
    f32,
    f64,
    &'static str,
    String,
    ak::Utf16String,
    ak::Utf16FlyString,
    num_bigint::BigInt,
    crate::layout::cell::CellHeader,
);

// SAFETY: Holds nothing.
unsafe impl<T: ?Sized> Trace for core::marker::PhantomData<T> {
    fn trace(&self, _: &mut Visitor) {}
}

// SAFETY: Forwards to the boxed value.
unsafe impl<T: Trace + ?Sized> Trace for Box<T> {
    fn trace(&self, visitor: &mut Visitor) {
        (**self).trace(visitor);
    }
}

// SAFETY: Forwards to every element.
unsafe impl<T: Trace> Trace for [T] {
    fn trace(&self, visitor: &mut Visitor) {
        T::trace_slice(self, visitor);
    }
}

// SAFETY: Forwards to every element.
unsafe impl<T: Trace, const N: usize> Trace for [T; N] {
    fn trace(&self, visitor: &mut Visitor) {
        T::trace_slice(self, visitor);
    }
}

// SAFETY: Forwards to every element.
unsafe impl<T: Trace> Trace for Vec<T> {
    fn trace(&self, visitor: &mut Visitor) {
        T::trace_slice(self, visitor);
    }
}

// SAFETY: Forwards to every element.
unsafe impl<T: Trace> Trace for std::collections::VecDeque<T> {
    fn trace(&self, visitor: &mut Visitor) {
        let (front, back) = self.as_slices();
        T::trace_slice(front, visitor);
        T::trace_slice(back, visitor);
    }
}

// SAFETY: Forwards to every key and value. The default RandomState hasher holds no cells.
unsafe impl<K: Trace, V: Trace> Trace for std::collections::HashMap<K, V> {
    fn trace(&self, visitor: &mut Visitor) {
        for (key, value) in self {
            key.trace(visitor);
            value.trace(visitor);
        }
    }
}

// SAFETY: Forwards to the value once it exists.
unsafe impl<T: Trace> Trace for core::cell::OnceCell<T> {
    fn trace(&self, visitor: &mut Visitor) {
        if let Some(value) = self.get() {
            value.trace(visitor);
        }
    }
}

macro_rules! impl_trace_for_tuples {
    ($(($($name:ident),+)),* $(,)?) => {
        $(
            // SAFETY: Forwards to every element.
            #[allow(non_snake_case)]
            unsafe impl<$($name: Trace),+> Trace for ($($name,)+) {
                fn trace(&self, visitor: &mut Visitor) {
                    let ($($name,)+) = self;
                    $($name.trace(visitor);)+
                }
            }
        )*
    };
}

impl_trace_for_tuples!(
    (A, B),
    (A, B, C),
    (A, B, C, D),
    (A, B, C, D, E),
    (A, B, C, D, E, F),
    (A, B, C, D, E, F, G),
    (A, B, C, D, E, F, G, H),
);

// SAFETY: A Gc reaches exactly its cell.
unsafe impl<T> Trace for Gc<T> {
    fn trace(&self, visitor: &mut Visitor) {
        visitor.visit(*self);
    }
}

// SAFETY: A member reaches exactly its cell.
unsafe impl<T> Trace for Member<T> {
    fn trace(&self, visitor: &mut Visitor) {
        visitor.visit_member(self);
    }
}

// SAFETY: A nullable member reaches exactly its cell, if it has one.
unsafe impl<T> Trace for NullableMember<T> {
    fn trace(&self, visitor: &mut Visitor) {
        visitor.visit_nullable_member(self);
    }
}

// SAFETY: Forwards to the contained value.
unsafe impl<T: Trace> Trace for Option<T> {
    fn trace(&self, visitor: &mut Visitor) {
        if let Some(value) = self {
            value.trace(visitor);
        }
    }
}

// SAFETY: Forwards to the contained value or error.
unsafe impl<T: Trace, E: Trace> Trace for Result<T, E> {
    fn trace(&self, visitor: &mut Visitor) {
        match self {
            Ok(value) => value.trace(visitor),
            Err(error) => error.trace(visitor),
        }
    }
}

// SAFETY: LibGC decodes the value and visits its cell, if it has one.
unsafe impl Trace for Value {
    fn trace(&self, visitor: &mut Visitor) {
        visitor.visit_values(core::slice::from_ref(self));
    }

    fn trace_slice(values: &[Self], visitor: &mut Visitor) {
        visitor.visit_values(values);
    }
}

// SAFETY: Forwards to the current value, in place, so that a collection that moves cells can update a slot in it.
unsafe impl<T: Copy + Trace> Trace for Cell<T> {
    fn trace(&self, visitor: &mut Visitor) {
        // SAFETY: Nothing writes the value while it is traced, since the mutator waits for tracing to end.
        unsafe { &*self.as_ptr() }.trace(visitor);
    }
}
