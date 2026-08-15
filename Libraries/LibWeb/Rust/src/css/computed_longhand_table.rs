/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Like the style values it retains, the table is shared across the FFI boundary but remains
// confined to the thread owning the C++ style objects.
#![allow(clippy::arc_with_non_send_sync)]

//! The computed longhand table: the source of truth for one style drive's
//! computed values, one slot per longhand.
//!
//! Every store a C++ `ComputedProperties::Builder` funnel performs lands
//! here as a strong reference to the shared style value data; the C++ side
//! keeps only a lazily filled wrapper cache over these slots, minting a
//! wrapper when a caller asks for one. A sparse sidecar remembers which
//! longhands took their value from a declaration that carries style sheet
//! context, together with the declaration's cascade source slot. The C++
//! `ComputedProperties::Data` creates the table and freezes it when the
//! drive's `ComputedProperties` is created; the frozen table is then shared
//! by reference count with every `ComputedValues` built from those
//! properties and with the style record publication that interns it.

use std::ffi::c_void;
use std::sync::Arc;

use crate::abort_on_panic;
use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID};
use crate::css::style_value::RetainedStyleValueData;

pub(crate) const LONGHAND_COUNT: usize = (LAST_LONGHAND_PROPERTY_ID - FIRST_LONGHAND_PROPERTY_ID + 1) as usize;

pub struct ComputedLonghandTable {
    slots: Vec<Option<RetainedStyleValueData>>,
    /// The raw data pointer of every slot, null where the slot is empty, so a
    /// borrower can read the whole table as one span without FFI calls.
    value_view: Vec<*const c_void>,
    /// `(longhand property id, cascade source slot)` for longhands whose
    /// stored value came from a declaration carrying style sheet context.
    source_slots: Vec<(u16, u32)>,
    frozen: bool,
}

impl ComputedLonghandTable {
    fn new() -> Self {
        let mut slots = Vec::new();
        slots.resize_with(LONGHAND_COUNT, || None);
        Self {
            slots,
            value_view: vec![std::ptr::null(); LONGHAND_COUNT],
            source_slots: Vec::new(),
            frozen: false,
        }
    }

    fn slot_index(property_id: u16) -> usize {
        assert!(
            (FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property_id),
            "computed longhand table indices are longhand property identifiers"
        );
        (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize
    }

    fn set(&mut self, property_id: u16, value: RetainedStyleValueData, source_slot: i64) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        self.value_view[Self::slot_index(property_id)] = value.pointer().cast();
        self.slots[Self::slot_index(property_id)] = Some(value);
        let existing = self
            .source_slots
            .iter()
            .position(|(longhand, _)| *longhand == property_id);
        match (existing, u32::try_from(source_slot)) {
            (Some(index), Ok(slot)) => self.source_slots[index] = (property_id, slot),
            (Some(index), Err(_)) => {
                self.source_slots.swap_remove(index);
            }
            (None, Ok(slot)) => self.source_slots.push((property_id, slot)),
            (None, Err(_)) => {}
        }
    }

    fn copy_from(&mut self, source: &ComputedLonghandTable) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        self.slots.clone_from(&source.slots);
        self.value_view.clone_from(&source.value_view);
        self.source_slots.clone_from(&source.source_slots);
    }

    fn copy_from_values(&mut self, values: &[*const c_void]) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        assert_eq!(values.len(), LONGHAND_COUNT);
        for (index, &value) in values.iter().enumerate() {
            self.slots[index] = (!value.is_null()).then(|| unsafe {
                RetainedStyleValueData::from_retained_pointer(crate::css::style_value::rust_style_value_retain(
                    value.cast(),
                ))
            });
            self.value_view[index] = value;
        }
        self.source_slots.clear();
    }

    fn freeze(&mut self) {
        self.frozen = true;
    }

    /// The stored value for a longhand, when the drive stored one.
    pub(crate) fn get(&self, property_id: u16) -> Option<&RetainedStyleValueData> {
        self.slots[Self::slot_index(property_id)].as_ref()
    }

    /// The cascade source slot of the declaration a longhand's value came
    /// from, recorded only when that declaration carries style sheet context.
    pub(crate) fn source_slot(&self, property_id: u16) -> Option<u32> {
        self.source_slots
            .iter()
            .find(|(longhand, _)| *longhand == property_id)
            .map(|(_, slot)| *slot)
    }

    /// One raw data pointer per longhand slot, null where the drive stored no
    /// value. The span is stable for the table's lifetime once frozen.
    pub(crate) fn value_pointers(&self) -> &[*const c_void] {
        &self.value_view
    }

    /// The complete style-sheet-context sidecar, in insertion order.
    pub(crate) fn source_slot_entries(&self) -> &[(u16, u32)] {
        &self.source_slots
    }

    pub(crate) fn is_frozen(&self) -> bool {
        self.frozen
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_computed_longhand_table_create() -> *mut ComputedLonghandTable {
    abort_on_panic(|| Arc::into_raw(Arc::new(ComputedLonghandTable::new())).cast_mut())
}

/// Takes one additional strong reference to a frozen table.
///
/// # Safety
/// `table` must be a live strong reference; a table may only be shared once
/// it is frozen, because the mutating calls require unique ownership.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_retain(
    table: *const ComputedLonghandTable,
) -> *const ComputedLonghandTable {
    abort_on_panic(|| {
        debug_assert!(unsafe { &*table }.frozen, "only frozen tables are shared");
        unsafe { Arc::increment_strong_count(table) };
        table
    })
}

/// Releases one strong reference.
///
/// # Safety
/// `table` must be a live strong reference that is not used after this call;
/// no references into the table may outlive the final release.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_release(table: *mut ComputedLonghandTable) {
    abort_on_panic(|| unsafe { Arc::decrement_strong_count(table.cast_const()) });
}

/// Stores one computed longhand, retaining `data`. `source_slot` is the
/// winning declaration's cascade source slot when its value carries style
/// sheet context, and -1 otherwise; storing without a slot clears any slot a
/// previous store recorded for the longhand.
///
/// # Safety
/// `table` must be a valid, unfrozen, uniquely owned table and `data` must
/// point at live `StyleValueData`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_set(
    table: *mut ComputedLonghandTable,
    property_id: u16,
    data: *const c_void,
    source_slot: i64,
) {
    abort_on_panic(|| {
        let value = unsafe {
            RetainedStyleValueData::from_retained_pointer(crate::css::style_value::rust_style_value_retain(data.cast()))
        };
        unsafe { &mut *table }.set(property_id, value, source_slot);
    });
}

/// Replaces the table's contents with `source`'s, for the C++ builder that
/// starts from an existing style's property array.
///
/// # Safety
/// `table` must be a valid, unfrozen, uniquely owned table and `source` a
/// valid table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_copy_from(
    table: *mut ComputedLonghandTable,
    source: *const ComputedLonghandTable,
) {
    abort_on_panic(|| unsafe { &mut *table }.copy_from(unsafe { &*source }));
}

/// Replaces the table's values with a complete borrowed value span, retaining
/// every non-null entry. The style-sheet-context sidecar is cleared: a span
/// borrowed from a published style record carries no usable cascade slots.
///
/// # Safety
/// `table` must be a valid, unfrozen, uniquely owned table and `values` must
/// have exactly one readable entry per longhand, each null or pointing at
/// live `StyleValueData`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_copy_from_values(
    table: *mut ComputedLonghandTable,
    values: *const *const c_void,
    count: usize,
) {
    abort_on_panic(|| {
        let values = unsafe { std::slice::from_raw_parts(values, count) };
        unsafe { &mut *table }.copy_from_values(values);
    });
}

/// Returns the table's value span: one borrowed data pointer per longhand,
/// null where the drive stored no value. The span stays valid while the
/// caller's table reference is live and the table stays frozen.
///
/// # Safety
/// `table` must be a valid, frozen table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_values(
    table: *const ComputedLonghandTable,
) -> *const *const c_void {
    abort_on_panic(|| {
        let table = unsafe { &*table };
        debug_assert!(table.frozen, "only frozen tables hand out their value span");
        table.value_view.as_ptr()
    })
}

/// Makes the table immutable; every later store aborts. Only a frozen table
/// may be shared through `rust_computed_longhand_table_retain`.
///
/// # Safety
/// `table` must be a valid, uniquely owned table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_freeze(table: *mut ComputedLonghandTable) {
    abort_on_panic(|| unsafe { &mut *table }.freeze());
}

/// Returns a borrowed pointer to the longhand's stored style value data, or
/// null when the drive stored none.
///
/// # Safety
/// `table` must be a valid table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_get(
    table: *const ComputedLonghandTable,
    property_id: u16,
) -> *const c_void {
    abort_on_panic(|| match unsafe { &*table }.get(property_id) {
        Some(value) => value.pointer().cast(),
        None => std::ptr::null(),
    })
}

/// Returns the longhand's recorded cascade source slot, or -1 when its value
/// did not come from a declaration carrying style sheet context.
///
/// # Safety
/// `table` must be a valid table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_source_slot(
    table: *const ComputedLonghandTable,
    property_id: u16,
) -> i64 {
    abort_on_panic(|| match unsafe { &*table }.source_slot(property_id) {
        Some(slot) => i64::from(slot),
        None => -1,
    })
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::*;
    use crate::css::style_value::StyleValueData;

    fn retained_number(value: f64) -> RetainedStyleValueData {
        unsafe {
            RetainedStyleValueData::from_retained_pointer(Arc::into_raw(Arc::new(StyleValueData::Number { value })))
        }
    }

    #[test]
    fn set_retains_and_release_drops() {
        let value = Arc::new(StyleValueData::Number { value: 42.0 });
        let weak_value = Arc::downgrade(&value);
        let retained_value = unsafe { RetainedStyleValueData::from_retained_pointer(Arc::into_raw(value)) };

        let mut table = ComputedLonghandTable::new();
        table.set(FIRST_LONGHAND_PROPERTY_ID, retained_value, -1);
        assert!(table.get(FIRST_LONGHAND_PROPERTY_ID).is_some());
        assert_eq!(table.source_slot(FIRST_LONGHAND_PROPERTY_ID), None);
        assert!(weak_value.upgrade().is_some());

        drop(table);
        assert!(weak_value.upgrade().is_none());
    }

    #[test]
    fn source_slot_sidecar_follows_overwrites() {
        let mut table = ComputedLonghandTable::new();
        table.set(FIRST_LONGHAND_PROPERTY_ID, retained_number(1.0), 7);
        assert_eq!(table.source_slot(FIRST_LONGHAND_PROPERTY_ID), Some(7));

        table.set(FIRST_LONGHAND_PROPERTY_ID, retained_number(2.0), 9);
        assert_eq!(table.source_slot(FIRST_LONGHAND_PROPERTY_ID), Some(9));

        table.set(FIRST_LONGHAND_PROPERTY_ID, retained_number(3.0), -1);
        assert_eq!(table.source_slot(FIRST_LONGHAND_PROPERTY_ID), None);
    }

    #[test]
    fn copy_from_values_retains_entries_and_clears_sidecar() {
        let mut source = ComputedLonghandTable::new();
        source.set(FIRST_LONGHAND_PROPERTY_ID, retained_number(1.0), 3);
        let source_pointer = source.get(FIRST_LONGHAND_PROPERTY_ID).unwrap().pointer();

        let mut copy = ComputedLonghandTable::new();
        let source_values = source.value_pointers().to_vec();
        copy.copy_from_values(&source_values);
        drop(source);

        assert_eq!(copy.get(FIRST_LONGHAND_PROPERTY_ID).unwrap().pointer(), source_pointer);
        assert_eq!(copy.value_pointers()[0], source_pointer.cast());
        assert_eq!(copy.source_slot(FIRST_LONGHAND_PROPERTY_ID), None);
    }

    #[test]
    fn copy_from_replicates_slots_and_sidecar() {
        let mut source = ComputedLonghandTable::new();
        source.set(FIRST_LONGHAND_PROPERTY_ID, retained_number(1.0), 3);
        let source_pointer = source.get(FIRST_LONGHAND_PROPERTY_ID).unwrap().pointer();

        let mut copy = ComputedLonghandTable::new();
        copy.copy_from(&source);
        drop(source);

        assert_eq!(copy.get(FIRST_LONGHAND_PROPERTY_ID).unwrap().pointer(), source_pointer);
        assert_eq!(copy.source_slot(FIRST_LONGHAND_PROPERTY_ID), Some(3));
    }
}
