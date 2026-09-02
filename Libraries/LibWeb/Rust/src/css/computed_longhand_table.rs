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
//! Every store a C++ `ComputedStyleWorkingSet` funnel performs lands
//! here as a strong reference to the shared style value data; the C++ side
//! keeps only a lazily filled wrapper cache over these slots, minting a
//! wrapper when a caller asks for one. A sparse sidecar remembers which
//! longhands took their value from a declaration that carries style sheet
//! context, together with the declaration's cascade source slot. The C++
//! The C++ `ComputedStyleWorkingSet` creates the table and freezes it when
//! the drive completes; the frozen table is then shared
//! by reference count with every `ComputedValues` built from those
//! properties and with the style record publication that interns it.

use std::ffi::c_void;
use std::sync::Arc;

use crate::css::animated_overlay::AnimatedOverlay;
use crate::css::animated_overlay::overlay_wins;
use crate::css::property_metadata::{
    FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, property_id, property_is_inherited,
};
use crate::css::style_value::retained_value_depends_on_current_color;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};

pub(crate) const LONGHAND_COUNT: usize = (LAST_LONGHAND_PROPERTY_ID - FIRST_LONGHAND_PROPERTY_ID + 1) as usize;

pub(crate) const LONGHAND_BITMAP_BYTES: usize = LONGHAND_COUNT.div_ceil(8);

/// One sparse inheritance-dependent specified value, exposed to C++ as the
/// borrowed span behind a style's inheritance-dependent value view.
#[repr(C)]
pub struct FfiTableInheritanceDependentValue {
    pub property: u16,
    pub value: *const c_void,
}

/// The effective value of one longhand: the value data pointer together with
/// which source produced it, so the C++ side can preserve wrapper identity
/// for overlay values and stamp style sheet context only onto table mints.
#[repr(C)]
pub struct FfiEffectiveLonghandValue {
    pub value: *const c_void,
    pub source: u8,
}

pub const EFFECTIVE_LONGHAND_SOURCE_TABLE: u8 = 0;
pub const EFFECTIVE_LONGHAND_SOURCE_OVERLAY: u8 = 1;
pub const EFFECTIVE_LONGHAND_SOURCE_SPECIFIED: u8 = 2;

struct PostComputeRestoreValues {
    values: [Option<(u16, RetainedStyleValueData)>; 7],
}

pub(crate) struct RestoredPostComputeValues {
    pub properties: [u16; 7],
    pub count: usize,
}

fn bitmap_bit(bits: &[u8; LONGHAND_BITMAP_BYTES], index: usize) -> bool {
    bits[index / 8] & (1 << (index % 8)) != 0
}

fn set_bitmap_bit(bits: &mut [u8; LONGHAND_BITMAP_BYTES], index: usize, value: bool) {
    if value {
        bits[index / 8] |= 1 << (index % 8);
    } else {
        bits[index / 8] &= !(1 << (index % 8));
    }
}

pub struct ComputedLonghandTable {
    slots: [Option<RetainedStyleValueData>; LONGHAND_COUNT],
    /// The raw data pointer of every slot, null where the slot is empty, so a
    /// borrower can read the whole table as one span without FFI calls.
    value_view: [*const c_void; LONGHAND_COUNT],
    /// Cascade source slot for each longhand, or `-1` when its value carries
    /// no style sheet context. This is dense because every property drive
    /// overwrites the sidecar, so sparse lookup would make a full drive
    /// quadratic in the number of context-bearing declarations.
    source_slots: [i32; LONGHAND_COUNT],
    /// Whether the longhand's winning declaration was `!important`, in the
    /// byte layout of the C++ `FixedBitmap` (bit `i` is byte `i / 8`, bit
    /// `i % 8`), so whole bitmaps copy across the FFI without translation.
    important_bits: [u8; LONGHAND_BITMAP_BYTES],
    /// Whether the longhand's computed value was taken by inheritance.
    inherited_bits: [u8; LONGHAND_BITMAP_BYTES],
    /// Which longhands a drive over this table (not the styles it was seeded
    /// from) has evaluated and stored; an evaluated longhand's value always
    /// comes from the table, while an unevaluated one keeps the recorded
    /// currentcolor-dependent specified value's preference.
    evaluated_bits: [u8; LONGHAND_BITMAP_BYTES],
    /// The recorded inheritance-dependent specified values, sparse.
    inheritance_dependent: Vec<(u16, RetainedStyleValueData)>,
    /// The winning cascaded font-size retained for monospace recascades.
    raw_cascaded_font_size: Option<RetainedStyleValueData>,
    /// The borrowed view over `inheritance_dependent` handed to C++.
    inheritance_dependent_view: Vec<FfiTableInheritanceDependentValue>,
    /// Viewport dependency flags accumulated by the longhand drive.
    metadata: FfiComputedStyleMetadata,
    /// Values before automatic post-compute adjustments, retained only while
    /// animation processing may need to restore them.
    post_compute_restore_values: Option<Box<PostComputeRestoreValues>>,
    frozen: bool,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiComputedStyleMetadata {
    pub display_before_box_type_transformation: u32,
    pub pseudo_element_styles: u64,
    pub effective_color_scheme: i16,
    pub dependency_flags: u8,
    pub in_display_none_subtree: bool,
}

impl ComputedLonghandTable {
    pub(crate) fn new() -> Self {
        Self {
            slots: std::array::from_fn(|_| None),
            value_view: [std::ptr::null(); LONGHAND_COUNT],
            source_slots: [-1; LONGHAND_COUNT],
            important_bits: [0; LONGHAND_BITMAP_BYTES],
            inherited_bits: [0; LONGHAND_BITMAP_BYTES],
            evaluated_bits: [0; LONGHAND_BITMAP_BYTES],
            inheritance_dependent: Vec::new(),
            inheritance_dependent_view: Vec::new(),
            raw_cascaded_font_size: None,
            metadata: FfiComputedStyleMetadata {
                display_before_box_type_transformation: 0,
                pseudo_element_styles: 0,
                effective_color_scheme: -1,
                dependency_flags: 0,
                in_display_none_subtree: false,
            },
            post_compute_restore_values: None,
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

    pub(crate) fn set(&mut self, property_id: u16, value: RetainedStyleValueData, source_slot: i64) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        self.value_view[Self::slot_index(property_id)] = value.pointer().cast();
        self.slots[Self::slot_index(property_id)] = Some(value);
        set_bitmap_bit(&mut self.evaluated_bits, Self::slot_index(property_id), true);
        self.source_slots[Self::slot_index(property_id)] = i32::try_from(source_slot).unwrap_or(-1);
    }

    pub(crate) fn set_important(&mut self, property_id: u16, important: bool) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        set_bitmap_bit(&mut self.important_bits, Self::slot_index(property_id), important);
    }

    pub(crate) fn set_inherited(&mut self, property_id: u16, inherited: bool) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        set_bitmap_bit(&mut self.inherited_bits, Self::slot_index(property_id), inherited);
    }

    pub(crate) fn merge_driver_flags(
        &mut self,
        important_words: &[u64],
        inherited_words: &[u64],
        evaluated_words: &[u64],
    ) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        assert!(important_words.len() * 64 >= LONGHAND_COUNT);
        assert_eq!(inherited_words.len(), important_words.len());
        assert_eq!(evaluated_words.len(), important_words.len());
        for index in 0..LONGHAND_COUNT {
            if evaluated_words[index / 64] & (1 << (index % 64)) == 0 {
                continue;
            }
            set_bitmap_bit(
                &mut self.important_bits,
                index,
                important_words[index / 64] & (1 << (index % 64)) != 0,
            );
            set_bitmap_bit(
                &mut self.inherited_bits,
                index,
                inherited_words[index / 64] & (1 << (index % 64)) != 0,
            );
        }
    }

    pub(crate) fn merge_dependency_flags(
        &mut self,
        depends_on_viewport_metrics: bool,
        font_metrics_depend_on_viewport_metrics: bool,
    ) {
        self.metadata.dependency_flags |=
            u8::from(depends_on_viewport_metrics) | (u8::from(font_metrics_depend_on_viewport_metrics) << 1);
    }

    pub(crate) fn dependency_flags(&self) -> u8 {
        self.metadata.dependency_flags
    }

    pub(crate) fn publication_dependency_flags(&self) -> u8 {
        self.metadata.dependency_flags | (u8::from(self.metadata.in_display_none_subtree) << 2)
    }

    pub(crate) fn pseudo_element_styles(&self) -> u64 {
        self.metadata.pseudo_element_styles
    }

    pub(crate) fn set_effective_color_scheme(&mut self, color_scheme: i16) {
        self.metadata.effective_color_scheme = color_scheme;
    }

    pub(crate) fn set_display_before_box_type_transformation(&mut self, display: u32) {
        self.metadata.display_before_box_type_transformation = display;
    }

    pub(crate) fn set_in_display_none_subtree(&mut self, in_display_none_subtree: bool) {
        self.metadata.in_display_none_subtree = in_display_none_subtree;
    }

    pub(crate) fn display_before_box_type_transformation(&self) -> u32 {
        self.metadata.display_before_box_type_transformation
    }

    pub(crate) fn inheritance_dependent_values(&self) -> impl Iterator<Item = (u16, *const c_void)> + '_ {
        self.inheritance_dependent
            .iter()
            .map(|(property, value)| (*property, value.pointer().cast()))
    }

    pub(crate) fn retained_inheritance_dependent_values(&self) -> impl Iterator<Item = (u16, &RetainedStyleValueData)> {
        self.inheritance_dependent
            .iter()
            .map(|(property, value)| (*property, value))
    }

    pub(crate) fn raw_cascaded_font_size(&self) -> *const c_void {
        self.raw_cascaded_font_size
            .as_ref()
            .map_or(std::ptr::null(), |value| value.pointer().cast())
    }

    pub(crate) fn set_raw_cascaded_font_size(&mut self, value: Option<RetainedStyleValueData>) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        self.raw_cascaded_font_size = value;
    }

    fn copy_from(&mut self, source: &ComputedLonghandTable) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        self.slots.clone_from(&source.slots);
        self.value_view.clone_from(&source.value_view);
        self.source_slots.clone_from(&source.source_slots);
        self.important_bits = source.important_bits;
        self.inherited_bits = source.inherited_bits;
        self.evaluated_bits = source.evaluated_bits;
        self.metadata = source.metadata;
        self.inheritance_dependent.clone_from(&source.inheritance_dependent);
        self.raw_cascaded_font_size.clone_from(&source.raw_cascaded_font_size);
        self.rebuild_inheritance_dependent_view();
        self.post_compute_restore_values = None;
    }

    pub(crate) fn copied_for_drive(source: &ComputedLonghandTable) -> Self {
        let mut table = Self::new();
        table.copy_from(source);
        table.clear_seeded_state();
        table
    }

    pub(crate) fn copied_for_partial_drive(source: &ComputedLonghandTable) -> Self {
        let mut table = Self::copied_for_drive(source);
        table.inheritance_dependent.clone_from(&source.inheritance_dependent);
        table.rebuild_inheritance_dependent_view();
        table.set_in_display_none_subtree(false);
        table
    }

    pub(crate) fn with_inherited_values_from(&self, inherited_source: &ComputedLonghandTable) -> Self {
        self.with_inherited_values_and_flags_from(inherited_source, &self.important_bits, &self.inherited_bits)
    }

    pub(crate) fn with_inherited_values_and_flags_from(
        &self,
        inherited_source: &ComputedLonghandTable,
        importance: &[u8],
        inheritance: &[u8],
    ) -> Self {
        assert!(self.frozen);
        assert!(inherited_source.frozen);
        let mut table = Self::new();
        table.copy_from(self);
        for property_id in FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID {
            if !property_is_inherited(property_id) {
                continue;
            }
            let index = Self::slot_index(property_id);
            let Some(value) = inherited_source.slots[index].clone() else {
                continue;
            };
            table.set(property_id, value, -1);
        }
        table.metadata.effective_color_scheme = inherited_source.metadata.effective_color_scheme;
        let inherited_color = inherited_source
            .get(property_id::COLOR)
            .map(RetainedStyleValueData::data);
        let color_input = crate::css::color_resolution::ColorResolutionInput {
            scheme: u8::try_from(inherited_source.metadata.effective_color_scheme).ok(),
            current_color: inherited_color.and_then(|color| {
                crate::css::color_resolution::to_color(
                    color,
                    &crate::css::color_resolution::ColorResolutionInput {
                        scheme: u8::try_from(inherited_source.metadata.effective_color_scheme).ok(),
                        current_color: None,
                        current_color_value: None,
                        length: None,
                        channels: None,
                    },
                )
            }),
            current_color_value: inherited_color,
            length: None,
            channels: None,
        };
        for index in 0..table.inheritance_dependent.len() {
            let (property_id, depends_on_current_color) = {
                let (property_id, value) = &table.inheritance_dependent[index];
                (*property_id, retained_value_depends_on_current_color(value))
            };
            if !property_is_inherited(property_id) && depends_on_current_color {
                let value = table.inheritance_dependent[index].1.clone();
                let slot = Self::slot_index(property_id);
                let resolved = crate::css::color_resolution::to_color(value.data(), &color_input).map(|color| unsafe {
                    RetainedStyleValueData::from_retained_pointer(Arc::into_raw(Arc::new(
                        crate::css::color_resolution::resolved_srgb_style_value(color),
                    )))
                });
                let is_resolved = resolved.is_some();
                let replacement = resolved.unwrap_or(value);
                table.value_view[slot] = replacement.pointer().cast();
                table.slots[slot] = Some(replacement);
                set_bitmap_bit(&mut table.evaluated_bits, slot, is_resolved);
            }
        }
        table.load_flag_bitmaps(importance, inheritance);
        table.frozen = true;
        table
    }

    pub(crate) fn into_raw_shared(self) -> *const Self {
        Arc::into_raw(Arc::new(self))
    }

    fn copy_from_values(&mut self, values: &[*const c_void]) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        assert_eq!(values.len(), LONGHAND_COUNT);
        for (index, &value) in values.iter().enumerate() {
            self.slots[index] = (!value.is_null()).then(|| unsafe {
                RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(value.cast()))
            });
            self.value_view[index] = value;
        }
        self.source_slots.fill(-1);
        self.important_bits = [0; LONGHAND_BITMAP_BYTES];
        self.inherited_bits = [0; LONGHAND_BITMAP_BYTES];
        self.evaluated_bits = [0; LONGHAND_BITMAP_BYTES];
        self.metadata.dependency_flags = 0;
        self.inheritance_dependent.clear();
        self.inheritance_dependent_view.clear();
        self.raw_cascaded_font_size = None;
        self.post_compute_restore_values = None;
    }

    fn rebuild_inheritance_dependent_view(&mut self) {
        self.inheritance_dependent_view = self
            .inheritance_dependent
            .iter()
            .map(|(property, value)| FfiTableInheritanceDependentValue {
                property: *property,
                value: value.pointer().cast(),
            })
            .collect();
    }

    /// Reset the state a fresh drive must not inherit from the style its
    /// table was seeded from: the evaluated bits and the recorded
    /// inheritance-dependent specified values.
    fn clear_seeded_state(&mut self) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        self.evaluated_bits = [0; LONGHAND_BITMAP_BYTES];
        self.inheritance_dependent.clear();
        self.inheritance_dependent_view.clear();
        self.post_compute_restore_values = None;
    }

    pub(crate) fn add_inheritance_dependent_value(&mut self, property_id: u16, value: RetainedStyleValueData) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        match self
            .inheritance_dependent
            .iter_mut()
            .find(|(property, _)| *property == property_id)
        {
            Some((_, existing)) => *existing = value,
            None => self.inheritance_dependent.push((property_id, value)),
        }
        self.rebuild_inheritance_dependent_view();
    }

    pub(crate) fn append_drive_inheritance_dependent_value(&mut self, property_id: u16, value: RetainedStyleValueData) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        debug_assert!(
            self.inheritance_dependent
                .iter()
                .all(|(property, _)| *property != property_id),
            "a longhand drive must visit each property once"
        );
        self.inheritance_dependent.push((property_id, value));
    }

    pub(crate) fn finish_drive_inheritance_dependent_values(&mut self) {
        self.rebuild_inheritance_dependent_view();
    }

    pub(crate) fn remove_inheritance_dependent_value(&mut self, property_id: u16) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        let Some(index) = self
            .inheritance_dependent
            .iter()
            .position(|(property, _)| *property == property_id)
        else {
            return;
        };
        self.inheritance_dependent.swap_remove(index);
        self.rebuild_inheritance_dependent_view();
    }

    fn inheritance_dependent_value(&self, property_id: u16) -> Option<&RetainedStyleValueData> {
        self.inheritance_dependent
            .iter()
            .find(|(property, _)| *property == property_id)
            .map(|(_, value)| value)
    }

    pub(crate) fn is_important(&self, property_id: u16) -> bool {
        bitmap_bit(&self.important_bits, Self::slot_index(property_id))
    }

    pub(crate) fn is_inherited(&self, property_id: u16) -> bool {
        bitmap_bit(&self.inherited_bits, Self::slot_index(property_id))
    }

    pub(crate) fn importance_bits(&self) -> &[u8] {
        &self.important_bits
    }

    pub(crate) fn publication_sidecars(&self) -> (&[u8; LONGHAND_BITMAP_BYTES], u32, i16) {
        (
            &self.evaluated_bits,
            self.metadata.display_before_box_type_transformation,
            self.metadata.effective_color_scheme,
        )
    }

    pub(crate) fn inheritance_bits(&self) -> &[u8] {
        &self.inherited_bits
    }

    pub(crate) fn load_flag_bitmaps(&mut self, importance: &[u8], inheritance: &[u8]) {
        assert!(!self.frozen);
        assert_eq!(importance.len(), LONGHAND_BITMAP_BYTES);
        assert_eq!(inheritance.len(), LONGHAND_BITMAP_BYTES);
        self.important_bits.copy_from_slice(importance);
        self.inherited_bits.copy_from_slice(inheritance);
    }

    pub(crate) fn property_inheritance_is_standard(&self) -> bool {
        (FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID)
            .all(|property_id| self.is_inherited(property_id) == property_is_inherited(property_id))
    }

    pub(crate) fn publication_equals(&self, other: &Self) -> bool {
        let values_equal = |first: *const c_void, second: *const c_void| {
            first == second
                || (!first.is_null()
                    && !second.is_null()
                    && unsafe { crate::css::style_value::rust_style_value_equals(first.cast(), second.cast()) })
        };
        self.value_view
            .iter()
            .zip(&other.value_view)
            .all(|(&first, &second)| values_equal(first, second))
            && self.important_bits == other.important_bits
            && self.inherited_bits == other.inherited_bits
            && self.publication_sidecars() == other.publication_sidecars()
            && self.publication_dependency_flags() == other.publication_dependency_flags()
            && self.pseudo_element_styles() == other.pseudo_element_styles()
            && values_equal(self.raw_cascaded_font_size(), other.raw_cascaded_font_size())
            && self.inheritance_dependent.len() == other.inheritance_dependent.len()
            && self.inheritance_dependent.iter().all(|(property, value)| {
                other
                    .inheritance_dependent
                    .iter()
                    .find(|(other_property, _)| other_property == property)
                    .is_some_and(|(_, other_value)| values_equal(value.pointer().cast(), other_value.pointer().cast()))
            })
    }

    pub(crate) fn display_is_list_item(&self) -> bool {
        let Some(StyleValueData::Display { raw }) = self.get(property_id::DISPLAY).map(RetainedStyleValueData::data)
        else {
            return false;
        };
        crate::css::display::FfiDisplay::from_raw(*raw).is_list_item()
    }

    fn is_evaluated(&self, property_id: u16) -> bool {
        bitmap_bit(&self.evaluated_bits, Self::slot_index(property_id))
    }

    /// The effective value property() returns for one longhand: the animated
    /// overlay under the overlay read rule, then an unevaluated longhand's
    /// recorded currentcolor-dependent specified value, then the table slot.
    pub(crate) fn effective_value(
        &self,
        overlay: Option<&AnimatedOverlay>,
        property_id: u16,
        with_animations: bool,
    ) -> FfiEffectiveLonghandValue {
        if with_animations
            && let Some(overlay) = overlay
            && let Some(entry) = overlay.get(property_id)
            && overlay_wins(entry, self.is_important(property_id))
        {
            return FfiEffectiveLonghandValue {
                value: entry.value,
                source: EFFECTIVE_LONGHAND_SOURCE_OVERLAY,
            };
        }
        if !self.is_evaluated(property_id)
            && let Some(value) = self.inheritance_dependent_value(property_id)
            && retained_value_depends_on_current_color(value)
        {
            return FfiEffectiveLonghandValue {
                value: value.pointer().cast(),
                source: EFFECTIVE_LONGHAND_SOURCE_SPECIFIED,
            };
        }
        FfiEffectiveLonghandValue {
            value: self.value_view[Self::slot_index(property_id)],
            source: EFFECTIVE_LONGHAND_SOURCE_TABLE,
        }
    }

    pub(crate) fn set_post_compute_restore_values(&mut self, values: [(u16, RetainedStyleValueData); 7]) {
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        self.post_compute_restore_values = Some(Box::new(PostComputeRestoreValues {
            values: values.map(Some),
        }));
    }

    pub(crate) fn restore_post_compute_values(&mut self, only_property: Option<u16>) -> RestoredPostComputeValues {
        let mut restored = RestoredPostComputeValues {
            properties: [0; 7],
            count: 0,
        };
        assert!(
            !self.frozen,
            "the computed longhand table is immutable once its style is created"
        );
        let Some(mut restore_values) = self.post_compute_restore_values.take() else {
            return restored;
        };
        for entry in &mut restore_values.values {
            let Some((property_id, _)) = entry.as_ref() else {
                continue;
            };
            if only_property.is_some_and(|only_property| *property_id != only_property) {
                continue;
            }
            let (property_id, value) = entry.take().unwrap();
            self.set(property_id, value, -1);
            restored.properties[restored.count] = property_id;
            restored.count += 1;
        }
        if restore_values.values.iter().any(Option::is_some) {
            self.post_compute_restore_values = Some(restore_values);
        }
        restored
    }

    pub(crate) fn freeze(&mut self) {
        self.post_compute_restore_values = None;
        self.frozen = true;
    }

    /// The stored value for a longhand, when the drive stored one.
    pub(crate) fn get(&self, property_id: u16) -> Option<&RetainedStyleValueData> {
        self.slots[Self::slot_index(property_id)].as_ref()
    }

    /// The cascade source slot of the declaration a longhand's value came
    /// from, recorded only when that declaration carries style sheet context.
    pub(crate) fn source_slot(&self, property_id: u16) -> Option<u32> {
        u32::try_from(self.source_slots[Self::slot_index(property_id)]).ok()
    }

    /// One raw data pointer per longhand slot, null where the drive stored no
    /// value. The span is stable for the table's lifetime once frozen.
    pub(crate) fn value_pointers(&self) -> &[*const c_void] {
        &self.value_view
    }

    pub(crate) fn is_frozen(&self) -> bool {
        self.frozen
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_computed_longhand_table_create() -> *mut ComputedLonghandTable {
    Arc::into_raw(Arc::new(ComputedLonghandTable::new())).cast_mut()
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
    debug_assert!(unsafe { &*table }.frozen, "only frozen tables are shared");
    unsafe { Arc::increment_strong_count(table) };
    table
}

/// Releases one strong reference.
///
/// # Safety
/// `table` must be a live strong reference that is not used after this call;
/// no references into the table may outlive the final release.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_release(table: *mut ComputedLonghandTable) {
    unsafe { Arc::decrement_strong_count(table.cast_const()) };
}

/// Creates a frozen copy of `table` with every inherited longhand value taken
/// from `inherited_source`.
///
/// # Safety
/// Both pointers must name live, frozen tables.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_create_with_inherited_values(
    table: *const ComputedLonghandTable,
    inherited_source: *const ComputedLonghandTable,
) -> *mut ComputedLonghandTable {
    Arc::into_raw(Arc::new(
        unsafe { &*table }.with_inherited_values_from(unsafe { &*inherited_source }),
    ))
    .cast_mut()
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
    let value = unsafe {
        RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(data.cast()))
    };
    unsafe { &mut *table }.set(property_id, value, source_slot);
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
    unsafe { &mut *table }.copy_from(unsafe { &*source });
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
    let values = unsafe { std::slice::from_raw_parts(values, count) };
    unsafe { &mut *table }.copy_from_values(values);
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
    let table = unsafe { &*table };
    debug_assert!(table.frozen, "only frozen tables hand out their value span");
    table.value_view.as_ptr()
}

/// Whether two frozen tables have equal values and publication metadata.
///
/// # Safety
/// Both pointers must name live, frozen tables.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_tables_equal_for_publication(
    first: *const ComputedLonghandTable,
    second: *const ComputedLonghandTable,
) -> bool {
    unsafe { &*first }.publication_equals(unsafe { &*second })
}

/// Makes the table immutable; every later store aborts. Only a frozen table
/// may be shared through `rust_computed_longhand_table_retain`.
///
/// # Safety
/// `table` must be a valid, uniquely owned table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_freeze(table: *mut ComputedLonghandTable) {
    unsafe { &mut *table }.freeze();
}

/// Returns the retained raw cascaded font-size data, or null when none won.
///
/// # Safety
/// `table` must be a valid table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_raw_cascaded_font_size(
    table: *const ComputedLonghandTable,
) -> *const c_void {
    unsafe { &*table }.raw_cascaded_font_size()
}

/// Replaces the retained raw cascaded font-size data.
///
/// # Safety
/// `table` must be a valid, unfrozen, uniquely owned table. `data` must be
/// null or point at live `StyleValueData`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_set_raw_cascaded_font_size(
    table: *mut ComputedLonghandTable,
    data: *const c_void,
) {
    let value = (!data.is_null()).then(|| unsafe {
        RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(data.cast()))
    });
    unsafe { &mut *table }.set_raw_cascaded_font_size(value);
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
    match unsafe { &*table }.source_slot(property_id) {
        Some(slot) => i64::from(slot),
        None => -1,
    }
}

/// Marks a longhand's stored value `!important` (or not).
///
/// # Safety
/// `table` must be a valid, unfrozen, uniquely owned table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_set_important(
    table: *mut ComputedLonghandTable,
    property_id: u16,
    important: bool,
) {
    unsafe { &mut *table }.set_important(property_id, important);
}

/// Marks a longhand's computed value as taken by inheritance (or not).
///
/// # Safety
/// `table` must be a valid, unfrozen, uniquely owned table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_set_inherited(
    table: *mut ComputedLonghandTable,
    property_id: u16,
    inherited: bool,
) {
    unsafe { &mut *table }.set_inherited(property_id, inherited);
}

/// # Safety
/// `table` must be a valid table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_is_important(
    table: *const ComputedLonghandTable,
    property_id: u16,
) -> bool {
    unsafe { &*table }.is_important(property_id)
}

/// # Safety
/// `table` must be a valid table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_is_inherited(
    table: *const ComputedLonghandTable,
    property_id: u16,
) -> bool {
    unsafe { &*table }.is_inherited(property_id)
}

/// The importance bitmap, in the C++ `FixedBitmap` byte layout. The pointer
/// stays valid while the caller's table reference is live.
///
/// # Safety
/// `table` must be a valid table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_importance_bits(
    table: *const ComputedLonghandTable,
) -> *const u8 {
    unsafe { &*table }.important_bits.as_ptr()
}

/// The inheritance bitmap, in the C++ `FixedBitmap` byte layout.
///
/// # Safety
/// `table` must be a valid table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_inheritance_bits(
    table: *const ComputedLonghandTable,
) -> *const u8 {
    unsafe { &*table }.inherited_bits.as_ptr()
}

/// Replaces the whole importance and inheritance bitmaps, for the builder
/// seeding a fresh table from an existing style's published bitmaps.
///
/// # Safety
/// `table` must be a valid, unfrozen, uniquely owned table; each bitmap must
/// have exactly `LONGHAND_BITMAP_BYTES` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_load_flag_bitmaps(
    table: *mut ComputedLonghandTable,
    importance: *const u8,
    importance_count: usize,
    inheritance: *const u8,
    inheritance_count: usize,
) {
    let table = unsafe { &mut *table };
    table.load_flag_bitmaps(
        unsafe { std::slice::from_raw_parts(importance, importance_count) },
        unsafe { std::slice::from_raw_parts(inheritance, inheritance_count) },
    );
}

/// Resets the state a fresh drive must not inherit from the style its table
/// was seeded from: the evaluated bits and the recorded inheritance-dependent
/// specified values.
///
/// # Safety
/// `table` must be a valid, unfrozen, uniquely owned table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_clear_seeded_state(table: *mut ComputedLonghandTable) {
    unsafe { &mut *table }.clear_seeded_state();
}

/// Records (or replaces) a longhand's inheritance-dependent specified value,
/// retaining `value`.
///
/// # Safety
/// `table` must be a valid, unfrozen, uniquely owned table and `value` must
/// point at live style value data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_add_inheritance_dependent_value(
    table: *mut ComputedLonghandTable,
    property_id: u16,
    value: *const c_void,
) {
    let value = unsafe {
        RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(value.cast()))
    };
    unsafe { &mut *table }.add_inheritance_dependent_value(property_id, value);
}

/// # Safety
/// `table` must be a valid, unfrozen, uniquely owned table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_remove_inheritance_dependent_value(
    table: *mut ComputedLonghandTable,
    property_id: u16,
) {
    unsafe { &mut *table }.remove_inheritance_dependent_value(property_id);
}

/// Returns the non-longhand computation state stored alongside the table.
///
/// # Safety
/// `table` must be a valid, uniquely owned table when the returned view is
/// mutated. The view remains valid for the lifetime of the table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_metadata(
    table: *mut ComputedLonghandTable,
) -> *mut FfiComputedStyleMetadata {
    &raw mut unsafe { &mut *table }.metadata
}

/// The recorded inheritance-dependent specified values as a borrowed span.
/// The span stays valid while the caller's table reference is live and no
/// further add or remove mutates the table.
///
/// # Safety
/// `table` must be a valid table and `out_count` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_inheritance_dependent_values(
    table: *const ComputedLonghandTable,
    out_count: *mut usize,
) -> *const FfiTableInheritanceDependentValue {
    let table = unsafe { &*table };
    unsafe { *out_count = table.inheritance_dependent_view.len() };
    table.inheritance_dependent_view.as_ptr()
}

/// The effective value for one longhand: the animated overlay under the
/// overlay read rule (important base values override animated but not
/// transitioned properties), then an unevaluated longhand's recorded
/// currentcolor-dependent specified value, then the stored table slot.
///
/// # Safety
/// `table` must be a valid table and `overlay` null or a valid overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_computed_longhand_table_effective_value(
    table: *const ComputedLonghandTable,
    overlay: *const AnimatedOverlay,
    property_id: u16,
    with_animations: bool,
) -> FfiEffectiveLonghandValue {
    unsafe { &*table }.effective_value(unsafe { overlay.as_ref() }, property_id, with_animations)
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

    #[test]
    fn dependency_flags_follow_the_table() {
        let mut source = ComputedLonghandTable::new();
        source.merge_dependency_flags(true, false);

        let mut copy = ComputedLonghandTable::new();
        copy.copy_from(&source);
        assert_eq!(copy.dependency_flags(), 1);

        copy.merge_dependency_flags(false, true);
        assert_eq!(copy.dependency_flags(), 3);
    }
}
