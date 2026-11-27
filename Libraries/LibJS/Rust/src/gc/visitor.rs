/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::marker::PhantomData;
use core::ptr::NonNull;

use super::capi::{
    GCMarker, GCVisitor, gc_marker_pin_cell, gc_marker_visit_cell, gc_marker_visit_ephemeron_without_pinning,
    gc_marker_visit_interior_pointer, gc_marker_visit_member, gc_marker_visit_owned_cell_pointer,
    gc_marker_visit_owned_member, gc_marker_visit_owned_values, gc_marker_visit_self_pointer, gc_marker_visit_values,
    gc_visitor_visit_cell, gc_visitor_visit_ephemeron, gc_visitor_visit_values,
};
use crate::layout::cell::Gc;
use crate::layout::member::{Member, NullableMember};
use crate::layout::value::Value;

/// A LibGC visitor, which marks what it visits during a collection and roots it while roots are gathered.
///
/// It visits slots for `'c`, the borrow of the cell it traces: the slots it takes by reference must live that long, so
/// that only slots in the cell, or in memory the cell owns, can be handed to the collector, which rewrites them when it
/// moves their cells. A copy on the stack does not live long enough, and is visited by value.
pub struct Visitor<'c> {
    /// The LibGC visitor, or None in the visitor of record_cell_slots(), which never calls into LibGC.
    raw: Option<NonNull<GCVisitor>>,
    /// The marker of a visitor that marks for a collection, and null in every other visitor.
    marker: *mut GCMarker,
    /// The marker, except while visit_owned_slots() traces, when it is null, so that the visits of slots outside it,
    /// which are almost all of them, need one check.
    plain_marker: *mut GCMarker,
    /// The slots that record_cell_slots() gathers, if it traces with this visitor, and null in every other visitor.
    recorder: *mut CellSlots,
    slots: PhantomData<&'c ()>,
}

/// What a slot of a cell holds, as record_cell_slots() finds it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SlotKind {
    /// A value, which is a cell if its tag says so.
    Value,
    /// The address of a cell, or 0.
    Cell,
    /// The compressed pointer of a cell, or 0.
    CompressedCell,
    /// A pointer into the cell itself, which a copy of the cell must not keep.
    SelfPointer,
}

/// The slots that tracing a cell visits, by their offset into the cell, for code that copies the bytes of the cell:
/// what tracing says is the only thing that tells which bytes are cells. Only the slots in the first bytes of the cell,
/// which the copy covers, are kept.
#[derive(Debug, Default)]
pub struct CellSlots {
    start: usize,
    end: usize,
    pub slots: Vec<(u32, SlotKind)>,
    /// Whether every cell the tracing visited is in a slot it named. A cell visited by value or through an interior
    /// pointer may be one that the bytes hold where no slot says so, and then nothing about the bytes is known.
    pub complete: bool,
}

/// Where a slot that a trace visits is, as far as a copy of the first bytes of the cell is concerned.
enum SlotPlace {
    /// In the bytes the copy covers, at this offset.
    InCopy(u32),
    /// In a cell, but not in the bytes the copy covers: past them in the cell, or in another cell. The collection
    /// rewrites it where it is, and the copy needs nothing.
    InHeap,
    /// Outside the heap, such as in malloc memory, where a copy of the cell's bytes may hold a pointer to it that the
    /// copy would then need to know about.
    Outside,
}

impl CellSlots {
    fn place(&self, slot: *const u8, size: usize) -> SlotPlace {
        let start = slot.addr();
        if start >= self.start && start + size <= self.end {
            SlotPlace::InCopy((start - self.start) as u32)
        } else if super::class::is_in_heap_region(start) {
            SlotPlace::InHeap
        } else {
            SlotPlace::Outside
        }
    }

    fn record(&mut self, slot: *const u8, size: usize, kind: SlotKind) {
        match self.place(slot, size) {
            SlotPlace::InCopy(offset) => self.slots.push((offset, kind)),
            SlotPlace::InHeap => {}
            // NB: Nothing tells which bytes of the cell lead to the slot, so nothing about the bytes is known.
            SlotPlace::Outside => self.complete = false,
        }
    }
}

/// The slots in the first `size` bytes of `cell`, which tracing the cell visits (see CellSlots).
pub fn record_cell_slots<T>(cell: Gc<T>, size: usize) -> CellSlots {
    let start = cell.as_ptr().addr();
    let mut slots = CellSlots {
        start,
        end: start + size,
        slots: Vec::new(),
        complete: true,
    };
    // NB: The visitor holds no LibGC visitor, so no trace can hand one to LibGC or to C++ visit code: it gets None
    //     from Visitor::as_raw(), which marks the recording incomplete.
    let mut visitor = Visitor {
        raw: None,
        marker: core::ptr::null_mut(),
        plain_marker: core::ptr::null_mut(),
        recorder: &raw mut slots,
        slots: PhantomData,
    };
    // SAFETY: The cell is live and of the class whose trace this is, and stays where it is during the trace.
    unsafe { (super::class::class_of(cell).trace)(cell.as_ptr().cast(), &mut visitor) };
    slots
}

/// A slot that mark_slot_in_owned_slots_mode() marks.
#[derive(Clone, Copy)]
enum OwnedModeSlot<'a> {
    Values(&'a [Value]),
    Member(*const u32),
}

/// Where a visitor keeps the marker it marks for, right after the vtable pointer of the C++ Cell::Visitor, which
/// check_layout() checks against GCLayout::visitor_marker_offset.
pub(crate) const VISITOR_MARKER_OFFSET: usize = size_of::<usize>();

impl<'c> Visitor<'c> {
    /// The marker of a visitor that marks for a collection, which takes the cells it visits directly.
    #[inline(always)]
    fn marker(&self) -> Option<NonNull<GCMarker>> {
        NonNull::new(self.marker)
    }

    /// The marker of a visitor that marks for a collection, outside visit_owned_slots().
    #[inline(always)]
    fn plain_marker(&self) -> Option<NonNull<GCMarker>> {
        NonNull::new(self.plain_marker)
    }

    /// # Safety
    ///
    /// `visitor` must be a visitor LibGC passed in, and outlive the returned value.
    pub unsafe fn from_raw(visitor: *mut GCVisitor) -> Self {
        let raw = NonNull::new(visitor).expect("LibGC passes a visitor");
        // SAFETY: The visitor is a live Cell::Visitor, which keeps a marker pointer at this offset, and does not change
        //         it while it visits.
        let marker = unsafe {
            raw.as_ptr()
                .cast::<u8>()
                .add(VISITOR_MARKER_OFFSET)
                .cast::<*mut GCMarker>()
                .read()
        };
        Self {
            raw: Some(raw),
            marker,
            plain_marker: marker,
            recorder: core::ptr::null_mut(),
            slots: PhantomData,
        }
    }

    /// The LibGC visitor, for visiting what the visitor's methods cannot name, such as an embedder's C++ cells. None
    /// while record_cell_slots() traces, which then knows nothing about the bytes of the cell, as it cannot see what
    /// the caller would visit.
    pub fn as_raw(&mut self) -> Option<NonNull<GCVisitor>> {
        if self.raw.is_none()
            && let Some(slots) = self.slot_recorder()
        {
            slots.complete = false;
        }
        self.raw
    }

    /// The slots that record_cell_slots() gathers, if it traces with this visitor.
    #[inline(always)]
    fn slot_recorder(&mut self) -> Option<&mut CellSlots> {
        // SAFETY: The slots that record_cell_slots() gathers outlive the trace, and only this visitor writes them.
        unsafe { self.recorder.as_mut() }
    }

    /// Visits a C++ cell by its address, which keeps it where it is.
    ///
    /// # Safety
    ///
    /// `cell` must be a live cell of the heap.
    pub unsafe fn visit_foreign_cell(&mut self, cell: NonNull<core::ffi::c_void>) {
        let Some(raw) = self.as_raw() else {
            return;
        };
        // SAFETY: The visitor is live, and the caller vouches for the cell.
        unsafe { gc_visitor_visit_cell(raw.as_ptr(), cell.as_ptr()) };
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
        let Some(raw) = self.as_raw() else {
            return;
        };
        // SAFETY: The visitor is live, and a Gc points to a live cell.
        unsafe { gc_visitor_visit_cell(raw.as_ptr(), cell.as_ptr().cast()) };
    }

    /// Visits the cell a member names. It takes the slot, which a collection that moves the cell rewrites if the slot
    /// is in a cell. A slot elsewhere keeps the cell where it is.
    #[inline(always)]
    pub fn visit_member<T>(&mut self, slot: &'c Member<T>) {
        if let Some(marker) = self.plain_marker() {
            // SAFETY: The marker is the visitor's, and the slot holds a compressed pointer to a live cell.
            unsafe { gc_marker_visit_member(marker.as_ptr(), core::ptr::from_ref(slot).cast()) };
            return;
        }
        self.visit_member_outside_plain_marking(core::ptr::from_ref(slot).cast());
    }

    /// Visits the cell a member names, if it is not null, like visit_member().
    #[inline(always)]
    pub fn visit_nullable_member<T>(&mut self, slot: &'c NullableMember<T>) {
        if let Some(marker) = self.plain_marker() {
            // SAFETY: The marker is the visitor's, and the slot holds 0 or a compressed pointer to a live cell.
            unsafe { gc_marker_visit_member(marker.as_ptr(), core::ptr::from_ref(slot).cast()) };
            return;
        }
        self.visit_member_outside_plain_marking(core::ptr::from_ref(slot).cast());
    }

    /// Visits the Gc in `slot` while visit_owned_slots() traces or for a visitor that does not mark, like
    /// visit_member_outside_plain_marking().
    #[inline(never)]
    fn visit_gc_outside_plain_marking(&mut self, slot: *const usize) {
        let Some(marker) = self.marker() else {
            if let Some(slots) = self.slot_recorder() {
                slots.record(slot.cast(), size_of::<usize>(), SlotKind::Cell);
                return;
            }
            let Some(raw) = self.as_raw() else {
                return;
            };
            // SAFETY: A Gc is a pointer to the start of a live cell, and the visitor is live.
            unsafe { gc_visitor_visit_cell(raw.as_ptr(), core::ptr::without_provenance_mut(slot.read())) };
            return;
        };
        // SAFETY: The marker is the visitor's. The caller of visit_owned_slots() vouches for the memory, and the slot
        //         lives for `'c`, so it is not a copy on the stack.
        unsafe { gc_marker_visit_owned_cell_pointer(marker.as_ptr(), slot) };
    }

    /// Visits the member in `slot`, which holds 0 or a compressed pointer to a live cell, while visit_owned_slots()
    /// traces or for a visitor that does not mark. Out of line, so that the visits of members stay small enough to
    /// inline.
    #[inline(never)]
    fn visit_member_outside_plain_marking(&mut self, slot: *const u32) {
        if let Some(marker) = self.marker() {
            self.mark_slot_in_owned_slots_mode(marker, OwnedModeSlot::Member(slot));
            return;
        }
        if let Some(slots) = self.slot_recorder() {
            slots.record(slot.cast(), size_of::<u32>(), SlotKind::CompressedCell);
            return;
        }
        let Some(raw) = self.as_raw() else {
            return;
        };
        // SAFETY: See above.
        let compressed = unsafe { slot.read() };
        if compressed != 0 {
            let address = crate::layout::member::decompress_address(compressed);
            // SAFETY: The visitor is live, and the address is that of a live cell.
            unsafe { gc_visitor_visit_cell(raw.as_ptr(), core::ptr::without_provenance_mut(address)) };
        }
    }

    /// Visits `cell`, which the raw pointer in `slot` points into, past its start, as an object points into the cell
    /// that holds its property storage. A collection that moves the cell rewrites the slot if it is in a cell.
    pub fn visit_interior_pointer<T, U>(&mut self, slot: &'c Cell<*mut T>, cell: Gc<U>) {
        if let Some(marker) = self.marker() {
            // SAFETY: The marker is the visitor's, the slot holds a pointer, and the cell is live.
            unsafe { gc_marker_visit_interior_pointer(marker.as_ptr(), slot.as_ptr().cast(), cell.as_ptr().cast()) };
            return;
        }
        self.visit(cell);
    }

    /// Notes that the raw pointer in `slot` points into the cell that holds the slot, up to one past its end, as an
    /// object points to its inline property storage. A collection that moves the cell points the slot into the copy.
    pub fn visit_self_pointer<T>(&mut self, slot: &'c Cell<*mut T>) {
        if let Some(marker) = self.marker() {
            // SAFETY: The marker is the visitor's, and the slot is in the cell being visited.
            unsafe { gc_marker_visit_self_pointer(marker.as_ptr(), slot.as_ptr().cast()) };
        } else if let Some(slots) = self.slot_recorder() {
            slots.record(slot.as_ptr().cast(), size_of::<usize>(), SlotKind::SelfPointer);
        }
    }

    /// Keeps `cell` where it is in a collection that moves cells, without keeping it alive, for a weak reference that
    /// the collector cannot update.
    pub fn pin<T: ?Sized>(&mut self, cell: Gc<T>) {
        if let Some(marker) = self.marker() {
            // SAFETY: The marker is the visitor's, and the cell is live.
            unsafe { gc_marker_pin_cell(marker.as_ptr(), cell.as_ptr().cast()) };
        } else if let Some(slots) = self.slot_recorder() {
            slots.complete = false;
        }
    }

    pub fn visit_values(&mut self, values: &'c [Value]) {
        if let Some(marker) = self.plain_marker() {
            if !values.is_empty() {
                // SAFETY: The marker is the visitor's, and Value is a NaN-boxed u64.
                unsafe { gc_marker_visit_values(marker.as_ptr(), values.as_ptr().cast(), values.len()) };
            }
            return;
        }
        if let Some(marker) = self.marker() {
            if !values.is_empty() {
                self.mark_slot_in_owned_slots_mode(marker, OwnedModeSlot::Values(values));
            }
            return;
        }
        self.visit_values_without_marker(values);
    }

    /// Visits values for a visitor that does not mark. Out of line, so that visit_values() stays small enough to
    /// inline.
    #[inline(never)]
    fn visit_values_without_marker(&mut self, values: &[Value]) {
        if let Some(slots) = self.slot_recorder() {
            for value in values {
                slots.record(core::ptr::from_ref(value).cast(), size_of::<Value>(), SlotKind::Value);
            }
            return;
        }
        let Some(raw) = self.as_raw() else {
            return;
        };
        // SAFETY: The visitor is live, and Value is a NaN-boxed u64.
        unsafe { gc_visitor_visit_values(raw.as_ptr(), values.as_ptr().cast(), values.len()) };
    }

    /// Visits values in memory that the cell being visited owns outside the heap, such as malloc storage, which a
    /// collection that moves cells rewrites where they are rather than keeping their cells where they are. Slots in the
    /// cell itself are traced in place instead (see the Trace of Gc, Member and Cell), and never visited as owned.
    ///
    /// # Safety
    ///
    /// Nothing may free, move or write the values until the collection is over: no finalizer, destructor or sweep
    /// callback may touch them.
    pub unsafe fn visit_owned_values(&mut self, values: &'c [Value]) {
        if let Some(marker) = self.marker() {
            if !values.is_empty() {
                // SAFETY: The marker is the visitor's, Value is a NaN-boxed u64, and the caller vouches for the memory.
                unsafe { gc_marker_visit_owned_values(marker.as_ptr(), values.as_ptr().cast(), values.len()) };
            }
            return;
        }
        self.visit_values_without_marker(values);
    }

    /// Traces `value`, in memory that the cell being visited owns outside the heap, such as a vector, so that the Gc,
    /// member and value slots in it are visited as owned slots: a collection that moves their cells rewrites them where
    /// they are rather than keeping the cells where they are. A cell that is visited by value still stays where it is.
    ///
    /// # Safety
    ///
    /// As for visit_owned_values(), for every slot that `value` traces.
    #[inline(never)]
    pub unsafe fn visit_owned_slots<T: Trace + ?Sized>(&mut self, value: &'c T) {
        let outer_plain_marker = self.plain_marker;
        self.plain_marker = core::ptr::null_mut();
        value.trace(self);
        self.plain_marker = outer_plain_marker;
    }

    /// Marks what `slot` holds as an owned slot while visit_owned_slots() traces. Out of line, so that the visits
    /// outside visit_owned_slots() stay small enough to inline.
    #[cold]
    #[inline(never)]
    fn mark_slot_in_owned_slots_mode(&mut self, marker: NonNull<GCMarker>, slot: OwnedModeSlot<'_>) {
        // SAFETY: The marker is the visitor's. Values are NaN-boxed u64s, and a member slot holds 0 or a compressed
        //         pointer to a live cell. The caller of visit_owned_slots() vouches for the memory, and the slot lives
        //         for `'c`, so it is not a copy on the stack.
        unsafe {
            match slot {
                OwnedModeSlot::Values(values) => {
                    gc_marker_visit_owned_values(marker.as_ptr(), values.as_ptr().cast(), values.len());
                }
                OwnedModeSlot::Member(slot) => gc_marker_visit_owned_member(marker.as_ptr(), slot),
            }
        }
    }

    /// Visits the cell a member names, in memory that the cell being visited owns outside the heap, such as a box,
    /// which a collection that moves the cell rewrites where it is rather than keeping the cell where it is.
    ///
    /// # Safety
    ///
    /// As for visit_owned_values().
    pub unsafe fn visit_owned_member<T>(&mut self, slot: &'c Member<T>) {
        if let Some(marker) = self.marker() {
            // SAFETY: The marker is the visitor's, the slot holds a compressed pointer to a live cell, and the caller
            //         vouches for the memory.
            unsafe { gc_marker_visit_owned_member(marker.as_ptr(), core::ptr::from_ref(slot).cast()) };
            return;
        }
        if let Some(slots) = self.slot_recorder() {
            slots.record(
                core::ptr::from_ref(slot).cast(),
                size_of::<u32>(),
                SlotKind::CompressedCell,
            );
            return;
        }
        self.visit(slot.get());
    }

    /// Like visit_owned_member(), for a member that may be null.
    ///
    /// # Safety
    ///
    /// As for visit_owned_values().
    pub unsafe fn visit_owned_nullable_member<T>(&mut self, slot: &'c NullableMember<T>) {
        if let Some(marker) = self.marker() {
            // SAFETY: The marker is the visitor's, the slot holds 0 or a compressed pointer to a live cell, and the
            //         caller vouches for the memory.
            unsafe { gc_marker_visit_owned_member(marker.as_ptr(), core::ptr::from_ref(slot).cast()) };
            return;
        }
        if let Some(slots) = self.slot_recorder() {
            slots.record(
                core::ptr::from_ref(slot).cast(),
                size_of::<u32>(),
                SlotKind::CompressedCell,
            );
            return;
        }
        if let Some(cell) = slot.get() {
            self.visit(cell);
        }
    }

    /// Visits the cell that `slot` points to, in memory that the cell being visited owns outside the heap, which a
    /// collection that moves the cell rewrites where it is rather than keeping the cell where it is.
    ///
    /// # Safety
    ///
    /// As for visit_owned_values().
    pub unsafe fn visit_owned<T>(&mut self, slot: &'c Gc<T>) {
        if let Some(marker) = self.marker() {
            // SAFETY: The marker is the visitor's, a Gc is a pointer to a live cell, and the caller vouches for the
            //         memory.
            unsafe { gc_marker_visit_owned_cell_pointer(marker.as_ptr(), core::ptr::from_ref(slot).cast()) };
            return;
        }
        if let Some(slots) = self.slot_recorder() {
            slots.record(core::ptr::from_ref(slot).cast(), size_of::<usize>(), SlotKind::Cell);
            return;
        }
        self.visit(*slot);
    }

    /// Like visit_owned(), for a slot that may be empty.
    ///
    /// # Safety
    ///
    /// As for visit_owned_values().
    pub unsafe fn visit_owned_option<T>(&mut self, slot: &'c Option<Gc<T>>) {
        if let Some(marker) = self.marker() {
            // SAFETY: A Gc is a NonNull, so an Option of one is a pointer to a live cell or null. The marker is the
            //         visitor's, and the caller vouches for the memory.
            unsafe { gc_marker_visit_owned_cell_pointer(marker.as_ptr(), core::ptr::from_ref(slot).cast()) };
            return;
        }
        if let Some(slots) = self.slot_recorder() {
            slots.record(core::ptr::from_ref(slot).cast(), size_of::<usize>(), SlotKind::Cell);
            return;
        }
        if let Some(cell) = *slot {
            self.visit(cell);
        }
    }

    /// Visits a value that is reachable only while `key` is. A collection that moves cells keeps both where they are.
    pub fn visit_ephemeron<T>(&mut self, key: Gc<T>, value: &'c Value) {
        let Some(raw) = self.as_raw() else {
            return;
        };
        // SAFETY: The visitor is live, a Gc points to a live cell, and Value is a NaN-boxed u64.
        unsafe {
            gc_visitor_visit_ephemeron(raw.as_ptr(), key.as_ptr().cast(), core::ptr::from_ref(value).cast());
        }
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

    /// Visits `value`, a cell that is reachable only while `key` is, without keeping either from moving.
    ///
    /// # Safety
    ///
    /// If the value survives a collection that moves cells, its holder must hand its slot to Sweep::record_slot().
    pub unsafe fn visit_ephemeron_cell_without_pinning<K, V>(&mut self, key: Gc<K>, value: Gc<V>) {
        let Some(marker) = self.marker() else {
            self.visit(value);
            return;
        };
        // SAFETY: The marker is the visitor's, and both are live cells.
        unsafe {
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
    pub unsafe fn visit_ephemeron_without_pinning<T>(&mut self, key: Gc<T>, value: &'c Value) {
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
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>);

    /// Traces a slice of values, which some types can report in a single call.
    fn trace_slice<'c>(values: &'c [Self], visitor: &mut Visitor<'c>)
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
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        (**self).trace(visitor);
    }
}

// SAFETY: Forwards to every element.
unsafe impl<T: Trace> Trace for [T] {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        T::trace_slice(self, visitor);
    }
}

// SAFETY: Forwards to every element.
unsafe impl<T: Trace, const N: usize> Trace for [T; N] {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        T::trace_slice(self, visitor);
    }
}

// SAFETY: Forwards to every element.
unsafe impl<T: Trace> Trace for Vec<T> {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        T::trace_slice(self, visitor);
    }
}

// SAFETY: Forwards to every element.
unsafe impl<T: Trace> Trace for std::collections::VecDeque<T> {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        let (front, back) = self.as_slices();
        T::trace_slice(front, visitor);
        T::trace_slice(back, visitor);
    }
}

// SAFETY: Forwards to every key and value. The default RandomState hasher holds no cells.
unsafe impl<K: Trace, V: Trace> Trace for std::collections::HashMap<K, V> {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        for (key, value) in self {
            key.trace(visitor);
            value.trace(visitor);
        }
    }
}

// SAFETY: Forwards to the value once it exists.
unsafe impl<T: Trace> Trace for core::cell::OnceCell<T> {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
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
                fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
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
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        // NB: The Gc is visited where it is, so that a collection that moves its cell rewrites it if it is in a cell.
        //     Elsewhere, such as on the stack or in malloc memory, it keeps its cell where it is.
        if let Some(marker) = visitor.plain_marker() {
            // SAFETY: The marker is the visitor's, and a Gc is a pointer to the start of a live cell.
            unsafe {
                gc_marker_visit_interior_pointer(
                    marker.as_ptr(),
                    core::ptr::from_ref(self).cast(),
                    self.as_ptr().cast(),
                );
            }
            return;
        }
        visitor.visit_gc_outside_plain_marking(core::ptr::from_ref(self).cast());
    }
}

// SAFETY: A member reaches exactly its cell.
unsafe impl<T> Trace for Member<T> {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        visitor.visit_member(self);
    }
}

// SAFETY: A nullable member reaches exactly its cell, if it has one.
unsafe impl<T> Trace for NullableMember<T> {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        visitor.visit_nullable_member(self);
    }
}

// SAFETY: Forwards to the contained value.
unsafe impl<T: Trace> Trace for Option<T> {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        if let Some(value) = self {
            value.trace(visitor);
        }
    }
}

// SAFETY: Forwards to the contained value or error.
unsafe impl<T: Trace, E: Trace> Trace for Result<T, E> {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        match self {
            Ok(value) => value.trace(visitor),
            Err(error) => error.trace(visitor),
        }
    }
}

// SAFETY: LibGC decodes the value and visits its cell, if it has one.
unsafe impl Trace for Value {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        visitor.visit_values(core::slice::from_ref(self));
    }

    fn trace_slice<'c>(values: &'c [Self], visitor: &mut Visitor<'c>) {
        visitor.visit_values(values);
    }
}

// SAFETY: Forwards to the current value, in place, so that a collection that moves cells can update a slot in it.
unsafe impl<T: Copy + Trace> Trace for Cell<T> {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        // SAFETY: Nothing writes the value while it is traced, since the mutator waits for tracing to end.
        unsafe { &*self.as_ptr() }.trace(visitor);
    }
}
