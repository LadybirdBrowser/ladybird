/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The style records elements and pseudo-elements are assigned, by identity.
//!
//! Every record is one the engine installed: its groups with their values, its flags and facts, and the payloads the
//! host renders from, in a slot of [`InstalledRecords`] that records holding the same values in the
//! same context share. Originating elements are assigned their records in a dense column, while
//! actual pseudo-element kinds use sparse rows. An element's or pseudo-element's animations compose
//! an animation overlay record over the record it is assigned, in a recyclable slot of its own.

use super::index::StyleAtomID;
use super::installed_records::{InstalledRecord, InstalledRecordID, InstalledRecords};
use super::tree::StyleNodeID;
use crate::css::computed_longhand_table::DEPENDS_ON_VIEWPORT_METRICS;
use crate::css::computed_values::computed_group_output_mask;
use crate::css::computed_values::style_group_payloads_hold_image_values;
use crate::css::host_shared::HostShared;
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::ShadowPayload;
use crate::css::style_value::retained_value_depends_on_current_color;

/// The record holds an `<image>` in a property whose images a layout node loads and observes.
/// Derived from the record's payloads; see `style_group_payloads_hold_image_values`.
pub(crate) const HOLDS_IMAGE_VALUES: u8 = 1 << 4;
/// The record is display:none, or its inheritance parent's record is in a display:none subtree.
pub(crate) const IN_DISPLAY_NONE_SUBTREE: u8 = 1 << 2;

/// The inherited style groups lead every group tuple.
pub(crate) const ENGINE_INHERITED_GROUP_COUNT: usize = crate::css::computed_values::INHERITED_SET_GROUPS.len();

/// What a view of a record with `facts` holding `payloads` says its values depend on.
pub(super) fn record_dependency_flags(
    facts: &super::pipeline::RecordFacts,
    payloads: &crate::css::computed_values::StyleGroups,
) -> u8 {
    // SAFETY: The caller holds a reference to each of the payloads.
    let holds_image_values = unsafe { style_group_payloads_hold_image_values(&payloads.pointers()) };
    view_dependency_flags(facts, holds_image_values)
}

/// What a view of a record with `facts`, whose groups hold images or not, says its values depend on.
pub(super) fn view_dependency_flags(facts: &super::pipeline::RecordFacts, holds_image_values: bool) -> u8 {
    facts.publication_dependency_flags() | (u8::from(holds_image_values) * HOLDS_IMAGE_VALUES)
}

#[derive(Clone, Copy)]
pub(crate) struct StyleRecordView<'a> {
    pub payloads: &'a crate::css::computed_values::StyleGroups,
    pub base_payloads: &'a crate::css::computed_values::StyleGroups,
    /// The base record, whose longhands the view reads.
    pub(in crate::css::style) record: &'a std::sync::Arc<super::pipeline::Record>,
    pub animated_overlay: Option<&'a crate::css::animated_overlay::AnimatedOverlay>,
    pub pseudo_element_styles: u64,
    pub counter_style_environment_identity: u64,
    pub dependency_flags: u8,
}

impl<'a> StyleRecordView<'a> {
    /// The view of `record`, a record the host holds: an installed record, or the sample of the
    /// animations over one, which reads the longhands, pseudo-elements and counter styles of the
    /// record it samples over.
    pub(crate) fn of_host_record(record: &'a std::sync::Arc<super::pipeline::Record>) -> Self {
        match record.sample_of_animations() {
            Some(sample) => Self {
                payloads: &record.payloads,
                base_payloads: &sample.base.payloads,
                record: &sample.base,
                animated_overlay: Some(&sample.animated_overlay),
                pseudo_element_styles: sample.base.pseudo_element_styles().bits(),
                counter_style_environment_identity: sample.base.rare().counter_style_environment_identity,
                dependency_flags: sample.dependency_flags,
            },
            None => Self {
                payloads: &record.payloads,
                base_payloads: &record.payloads,
                record,
                animated_overlay: None,
                pseudo_element_styles: record.pseudo_element_styles().bits(),
                counter_style_environment_identity: record.rare().counter_style_environment_identity,
                dependency_flags: record.view_dependency_flags(),
            },
        }
    }
}

/// The longhands of a view's base record, read from its group values.
#[derive(Clone, Copy)]
pub(crate) struct ViewLonghands<'a>(&'a super::pipeline::Record);

/// The longhands two records hold the very same computed values of, being in groups the records
/// share.
#[derive(Clone, Copy)]
pub(crate) struct SharedLonghands {
    /// The groups the records share, one bit per group.
    shared_groups: u32,
}

impl SharedLonghands {
    /// Whether both records hold the very same computed value of `property`.
    pub(crate) fn holds(self, property: u16) -> bool {
        let layout = crate::css::computed_values::GroupValueLayout::get();
        let longhand = crate::css::computed_values::LonghandID::new(property);
        let place = layout.place(longhand);
        // NB: The inherited bits are no group's: the records' values of them compare by value.
        layout.holds_value(longhand) && place.bit_field().is_none() && self.shared_groups & (1 << place.group()) != 0
    }

    /// The longhands whose values the records may hold differently, one bit per longhand from the
    /// first: those in a group they do not share, and those that map elsewhere or hold no value of a
    /// group.
    pub(crate) fn longhands_to_compare(self) -> [u64; crate::css::property_metadata::LONGHAND_WORD_COUNT] {
        use crate::css::property_metadata::LONGHAND_WORD_COUNT;
        let group_longhands = crate::css::computed_values::GroupValueLayout::group_longhands();
        let mut longhands = [u64::MAX; LONGHAND_WORD_COUNT];
        for (group, group_words) in group_longhands.iter().enumerate() {
            if self.shared_groups & (1 << group) != 0 {
                for (word, group_word) in longhands.iter_mut().zip(group_words) {
                    *word &= !group_word;
                }
            }
        }
        longhands
    }
}

impl<'a> ViewLonghands<'a> {
    pub(super) fn of_record(record: &'a super::pipeline::Record) -> Self {
        Self(record)
    }

    /// Reads the longhands of a record holding `value_of(property)` for each longhand, as a test does.
    #[cfg(test)]
    pub(crate) fn with_test_record<R>(
        value_of: impl Fn(u16) -> RetainedStyleValueData,
        read: impl FnOnce(ViewLonghands<'_>) -> R,
    ) -> R {
        let record = super::pipeline::Record::of_test_values(value_of);
        read(ViewLonghands::of_record(&record))
    }

    /// The longhands of the style record `record` points at.
    ///
    /// # Safety
    /// `record` must point at a style record that stays live for `'a`.
    pub(crate) unsafe fn of_raw_record(record: *const std::ffi::c_void) -> Self {
        // SAFETY: Guaranteed by the caller.
        Self::of_record(unsafe { &*record.cast::<super::pipeline::Record>() })
    }

    /// Whether these are the longhands of the record of `other`.
    pub(crate) fn same_record_as(self, other: Self) -> bool {
        std::ptr::eq(self.0, other.0)
    }

    /// The longhands whose computed values these and `other` hold as one, where both are the engine
    /// records sharing any group.
    pub(crate) fn shared_with(self, other: Self) -> Option<SharedLonghands> {
        let shared_groups = self.0.value_groups().shared_group_mask(other.0.value_groups());
        (shared_groups != 0).then_some(SharedLonghands { shared_groups })
    }

    /// Whether these are the longhands of `record`.
    pub(super) fn are_of_record(self, record: &super::pipeline::Record) -> bool {
        std::ptr::eq(self.0, record)
    }

    pub(crate) fn is_important(self, property: u16) -> bool {
        self.0.rare().table_flags.is_important(property)
    }

    /// Whether the computed value of `property` was inherited.
    pub(crate) fn is_inherited(self, property: u16) -> bool {
        self.0.rare().table_flags.is_inherited(property)
    }

    /// The handle of the computed value of `property`, where it has one.
    pub(crate) fn handle(self, property: u16) -> Option<&'a RetainedStyleValueData> {
        {
            Some(self.0.longhand(crate::css::computed_values::LonghandID::new(property)))
                .filter(|value| !value.pointer().is_null())
        }
    }

    /// One bit per longhand: whether its value is important.
    pub(crate) fn importance_bits(self) -> &'a [u8] {
        self.0.rare().table_flags.importance_bits()
    }

    /// The computed value of `property`, where it has one.
    pub(crate) fn value(self, property: u16) -> Option<&'a crate::css::style_value::StyleValueData> {
        self.handle(property).map(RetainedStyleValueData::data)
    }

    /// https://drafts.csswg.org/css-viewport/#effective-zoom
    pub(crate) fn effective_zoom(self) -> f32 {
        self.0.payloads.inherited_values().effective_zoom()
    }

    /// The dependency flags of the record's table, without whether it is in a `display: none`
    /// subtree.
    pub(crate) fn dependency_flags(self) -> u8 {
        self.0.facts().dependency_flags()
    }

    /// The overflow keywords before the box adjustments, where they adjusted them.
    pub(crate) fn overflow_before_adjustment(self) -> Option<[u16; 2]> {
        self.0.rare().table_flags.overflow_before_adjustment()
    }

    /// The element's cascaded font size, where it declares one.
    pub(crate) fn raw_cascaded_font_size(self) -> Option<&'a crate::css::style_value::StyleValueData> {
        self.0.rare().table_flags.raw_cascaded_font_size_value()
    }

    /// The animated value of `property` that `overlay` composes over these longhands, where it
    /// wins over the computed one.
    pub(crate) fn overlay_value(
        self,
        overlay: Option<&'a crate::css::animated_overlay::AnimatedOverlay>,
        property: u16,
    ) -> Option<&'a RetainedStyleValueData> {
        overlay?
            .get(property)
            .filter(|entry| crate::css::animated_overlay::overlay_wins(entry, self.is_important(property)))
            .map(crate::css::animated_overlay::FfiAnimatedOverlayEntry::value_handle)
    }

    /// The value of `property` as specified: its recorded specified value where the computation did
    /// not evaluate it, else its computed value.
    pub(crate) fn specified_value(self, property: u16) -> Option<&'a RetainedStyleValueData> {
        (!self.0.rare().table_flags.is_evaluated(property))
            .then(|| self.inheritance_dependent_value(property))
            .flatten()
            .or_else(|| self.handle(property))
    }

    /// Whether `property` was specified as the `currentcolor` keyword itself.
    pub(crate) fn specified_value_is_current_color(self, property: u16) -> bool {
        self.specified_value(property)
            .is_some_and(|value| crate::css::computed_longhand_table::is_current_color_keyword(value.data()))
    }

    /// Whether each layer of the specified `text-shadow` has the `currentcolor` keyword as its
    /// color, or no color, in the order the computed layers have; empty for `none`.
    pub(crate) fn text_shadow_layer_colors_are_current_color(self) -> Vec<bool> {
        use crate::css::style_value::StyleValueData;
        let Some(StyleValueData::ValueList { values, .. }) = self
            .specified_value(crate::css::property_metadata::property_id::TEXT_SHADOW)
            .map(RetainedStyleValueData::data)
        else {
            return Vec::new();
        };
        values
            .as_slice()
            .iter()
            .map(|layer| match layer.optional_data() {
                Some(StyleValueData::Shadow(shadow_payload)) => {
                    let ShadowPayload { color, .. } = &**shadow_payload;
                    color
                        .optional_data()
                        .is_none_or(crate::css::computed_longhand_table::is_current_color_keyword)
                }
                _ => false,
            })
            .collect()
    }

    /// Whether the computed `display` is a list item.
    pub(crate) fn display_is_list_item(self) -> bool {
        matches!(
            self.value(crate::css::property_metadata::property_id::DISPLAY),
            Some(crate::css::style_value::StyleValueData::Display { raw })
                if crate::css::display::FfiDisplay::from_raw(*raw).is_list_item()
        )
    }

    /// The `display` the element computed before its box type transformation.
    pub(crate) fn display_before_box_type_transformation(self) -> u32 {
        self.0.rare().value_facts.display_before_box_type_transformation()
    }

    /// The color scheme the element's colors resolved against.
    pub(crate) fn effective_color_scheme(self) -> i16 {
        self.0.effective_color_scheme()
    }

    /// The value of `property` the element's styles read, as [`Self::effective_value`] answers it
    /// with what `overlay` composes where `with_animations` asks for that, and which source it is.
    pub(crate) fn effective_value_and_source(
        self,
        overlay: Option<&'a crate::css::animated_overlay::AnimatedOverlay>,
        property: u16,
        with_animations: bool,
    ) -> (&'a RetainedStyleValueData, u8) {
        use crate::css::computed_longhand_table::{
            EFFECTIVE_LONGHAND_SOURCE_OVERLAY, EFFECTIVE_LONGHAND_SOURCE_SPECIFIED, EFFECTIVE_LONGHAND_SOURCE_TABLE,
        };
        if with_animations && let Some(value) = self.overlay_value(overlay, property) {
            return (value, EFFECTIVE_LONGHAND_SOURCE_OVERLAY);
        }
        match self.0.rare().value_facts.value_read_in_place(property) {
            Some(value) => (value, EFFECTIVE_LONGHAND_SOURCE_SPECIFIED),
            None => (
                self.0.longhand(crate::css::computed_values::LonghandID::new(property)),
                EFFECTIVE_LONGHAND_SOURCE_TABLE,
            ),
        }
    }

    /// What the computation settled beside the values, which the host starts an update of the
    /// element's style from.
    pub(crate) fn metadata(self) -> crate::css::computed_longhand_table::FfiComputedStyleMetadata {
        crate::css::computed_longhand_table::FfiComputedStyleMetadata {
            display_before_box_type_transformation: self.0.rare().value_facts.display_before_box_type_transformation(),
            effective_color_scheme: self.0.effective_color_scheme(),
            dependency_flags: self.0.facts().dependency_flags(),
            in_display_none_subtree: self.0.facts().in_display_none_subtree(),
        }
    }

    /// The value of `property` the element's styles read: what `overlay` composes over these
    /// longhands where it wins, else its recorded `currentcolor`-dependent specified value where
    /// the computation did not evaluate it, else its computed value, as
    /// a group build reads it with the animations.
    pub(crate) fn effective_value(
        self,
        overlay: Option<&'a crate::css::animated_overlay::AnimatedOverlay>,
        property: u16,
    ) -> &'a RetainedStyleValueData {
        if let Some(value) = self.overlay_value(overlay, property) {
            return value;
        }
        self.0.effective_value(property)
    }

    /// Whether the computed `animation-name` names any animation.
    pub(crate) fn declares_css_animations(self) -> bool {
        self.0.facts().declares_css_animations()
    }

    /// Whether the computed `transition-*` values make any longhand transitionable.
    pub(crate) fn declares_transitions(self) -> bool {
        self.0.facts().declares_transitions()
    }

    /// Whether `display` computed to `none` only through the custom properties the element's
    /// animations sampled.
    pub(crate) fn display_none_is_animated(self) -> bool {
        self.0.rare().reads.display_none_is_animated
    }

    /// The computed `display` with what `overlay` composes over it.
    pub(crate) fn effective_display(
        self,
        overlay: Option<&'a crate::css::animated_overlay::AnimatedOverlay>,
    ) -> crate::css::display::FfiDisplay {
        self.overlay_value(overlay, crate::css::property_metadata::property_id::DISPLAY)
            .map_or(self.0.facts().display(), crate::css::style_compute::display_of)
    }

    /// The computed `writing-mode` with what `overlay` composes over it.
    pub(crate) fn effective_writing_mode(
        self,
        overlay: Option<&'a crate::css::animated_overlay::AnimatedOverlay>,
    ) -> u8 {
        self.overlay_value(overlay, crate::css::property_metadata::property_id::WRITING_MODE)
            .map_or(
                self.0.payloads.inherited_bits().writing_mode(),
                crate::css::style_compute::writing_mode_of,
            )
    }

    /// The animated values of the properties that inherit which a parent whose animations compose
    /// `overlay` over these longhands hands down.
    pub(crate) fn inherited_overrides(
        self,
        overlay: Option<&'a crate::css::animated_overlay::AnimatedOverlay>,
    ) -> impl Iterator<Item = (u16, RetainedStyleValueData)> + 'a {
        overlay
            .map_or(&[][..], crate::css::animated_overlay::AnimatedOverlay::entries)
            .iter()
            .filter(move |entry| {
                crate::css::property_metadata::property_is_inherited(entry.property)
                    && crate::css::animated_overlay::overlay_wins(entry, self.is_important(entry.property))
            })
            .map(|entry| (entry.property, entry.clone_value()))
    }

    /// The recorded specified values of the longhands whose computed values depend on what the
    /// element inherits.
    pub(crate) fn inheritance_dependent_values(self) -> impl Iterator<Item = (u16, &'a RetainedStyleValueData)> {
        self.0.inheritance_dependent_values()
    }

    /// The recorded specified value of `property`, where its computed value depends on what the
    /// element inherits.
    pub(crate) fn inheritance_dependent_value(self, property: u16) -> Option<&'a RetainedStyleValueData> {
        self.inheritance_dependent_values()
            .find(|(held, _)| *held == property)
            .map(|(_, value)| value)
    }
}

impl<'a> StyleRecordView<'a> {
    /// The base record's longhands.
    pub(crate) fn longhands(&self) -> ViewLonghands<'a> {
        ViewLonghands::of_record(self.record)
    }

    /// The base record, held, for reading its longhands beyond the view.
    pub(super) fn retained_record(&self) -> std::sync::Arc<super::pipeline::Record> {
        self.record.clone()
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct FinalStyleRecordID(u64);

impl FinalStyleRecordID {
    /// No record: what a node holds before its first publication.
    pub(crate) const NONE: Self = Self(0);
    const ANIMATION_OVERLAY_TAG: u64 = 1 << 63;
    /// NB: Below the 53 bits a double holds, for script to read an installed record's identity
    ///     exactly through internals.
    const INSTALLED_TAG: u64 = 1 << 52;

    fn animation_overlay(generation: u64) -> Self {
        assert!(generation != 0 && generation < Self::ANIMATION_OVERLAY_TAG);
        Self(Self::ANIMATION_OVERLAY_TAG | generation)
    }

    /// The identity of a record the engine installed.
    pub(crate) fn installed(record: InstalledRecordID) -> Self {
        const _: () = assert!(InstalledRecordID::MAX_GENERATION < 1 << 20);
        Self(Self::INSTALLED_TAG | (u64::from(record.generation()) << 32) | (u64::from(record.slot()) + 1))
    }

    /// The record the engine installed that this identity names, if it names one.
    pub(crate) fn installed_record(self) -> Option<InstalledRecordID> {
        if self.0 & (Self::ANIMATION_OVERLAY_TAG | Self::INSTALLED_TAG) != Self::INSTALLED_TAG {
            return None;
        }
        let slot = (self.0 as u32).checked_sub(1)?;
        Some(InstalledRecordID::from_parts(
            slot,
            ((self.0 & !Self::INSTALLED_TAG) >> 32) as u32,
        ))
    }

    #[must_use]
    pub fn raw(self) -> u64 {
        self.0
    }

    /// The record a raw identity names; none for zero.
    pub(crate) fn from_raw(raw: u64) -> Option<Self> {
        (raw != 0).then_some(Self(raw))
    }

    /// Whether the record composes animated values over a base record: those of the animations of the element or
    /// pseudo-element that holds it.
    pub(crate) fn is_animation_overlay(self) -> bool {
        self.0 & Self::ANIMATION_OVERLAY_TAG != 0
    }
}

/// Whether the raw identity `raw` names a style record other than one the engine installed.
#[must_use]
pub(crate) fn names_an_uninstalled_record(raw: u64) -> bool {
    FinalStyleRecordID::from_raw(raw).is_some_and(|record| record.installed_record().is_none())
}

/// An immutable hover frame's hold on the style records its rows name, all of which something holds as it is taken: the
/// engine frees no record whose last holder lets go while the lease stands, until so many did that it reads the rows to
/// tell which of them they name.
pub(crate) struct HoverFrameLease {
    epoch: u32,
    nodes: crate::cow_column::ColumnSnapshot<
        crate::layout::node_data::PaintNode,
        { crate::layout::tree_shape::PUBLISHED_ROWS_PER_CHUNK },
    >,
}

/// How many records a task that sees no frame may let go of under a hover frame's lease before it reads the frame's rows:
/// as many as they are, so that reading them costs a task less than what it made them for, but at least this many.
const MIN_RECORDS_RELEASED_BEFORE_READING_LEASES: usize = 1024;

/// How few released records a reclamation leaves to a later sweep.
const MIN_RECORDS_RELEASED_BEFORE_SWEEPING: usize = 64;

/// A reader's hold on the payloads of every style record published so far: while one is held, the
/// engine frees no record's payloads, which a reader on another thread may be reading.
pub(crate) struct StyleRecordLease {
    leases: std::sync::Arc<std::sync::atomic::AtomicUsize>,
}

impl Drop for StyleRecordLease {
    fn drop(&mut self) {
        // NB: Releases the reads made under the lease to the engine that frees the payloads once it sees none held.
        self.leases.fetch_sub(1, std::sync::atomic::Ordering::Release);
    }
}

/// A fork never takes its records, so past [`Self::MAX_RECORDS`] only the fact that they overflowed is kept.
#[derive(Clone, Default)]
pub(crate) struct ReleasedAnimationOverlayRecords {
    records: Vec<FinalStyleRecordID>,
    overflowed: bool,
}

impl ReleasedAnimationOverlayRecords {
    const MAX_RECORDS: usize = 1024;

    fn add(&mut self, record: FinalStyleRecordID) {
        if self.overflowed {
            return;
        }
        if self.records.len() == Self::MAX_RECORDS {
            self.records = Vec::new();
            self.overflowed = true;
            return;
        }
        self.records.push(record);
    }

    #[cfg(test)]
    fn is_empty(&self) -> bool {
        self.records.is_empty() && !self.overflowed
    }

    pub(crate) fn records(&self) -> impl Iterator<Item = u64> + '_ {
        self.records.iter().map(|record| record.raw())
    }

    pub(crate) fn overflowed(&self) -> bool {
        self.overflowed
    }
}

#[derive(Clone)]
struct AnimationOverlayRecord {
    // NB: No sampled value enters a permanent interning table. The current assignment owns one
    //     reference, while detached layout and stabilization baselines can pin an old generation.
    /// The installed record the overlay composes over, which it keeps from being freed while it
    /// lives.
    base: InstalledRecordID,
    source_identity: u64,
    /// The record of the style the animations sample over the base: its groups are the base's but for those the
    /// animated values changed, and it reads the base's longhands beneath them and holds the animated values.
    /// NB: Shared with a fork of the render state, whose boxes name its groups as the host's do.
    /// The host holds it for the element or pseudo-element assigned the record.
    record: std::sync::Arc<super::pipeline::Record>,
    pin_count: u64,
    is_assigned: bool,
}

impl AnimationOverlayRecord {
    /// What the animations sampled over the base.
    fn sample(&self) -> &super::pipeline::Sample {
        self.record
            .sample_of_animations()
            .expect("an animation-overlay record is a sample")
    }

    /// The record's identity.
    fn final_style_record(&self) -> FinalStyleRecordID {
        self.sample().identity
    }

    /// Whether something beside the slot holds the record: the host for the element it applied the record to, a box
    /// of a fork of the render state, or a fork's copy of the sets.
    fn is_held_elsewhere(&self) -> bool {
        std::sync::Arc::strong_count(&self.record) > 1
    }
}

mod animation_overlay_slots {
    use super::{AnimationOverlayRecord, FinalStyleRecordID};

    /// The animation-overlay records by slot. A record leaves its slot only to be retired, never dropped, so that a
    /// reader holding a style-record lease keeps reading its payloads until the owner frees the retired records.
    #[derive(Clone, Default)]
    pub(super) struct AnimationOverlaySlots {
        slots: Vec<Option<AnimationOverlayRecord>>,
        free: Vec<u32>,
        retired: Vec<AnimationOverlayRecord>,
    }

    impl AnimationOverlaySlots {
        /// The records of the sampled styles the overlays hold, retired ones included.
        pub(super) fn records(&self) -> impl Iterator<Item = &std::sync::Arc<super::super::pipeline::Record>> {
            self.slots
                .iter()
                .flatten()
                .chain(self.retired.iter())
                .map(|overlay| &overlay.record)
        }

        pub(super) fn heap_size(&self) -> usize {
            use crate::heap_size::vec_bytes;
            vec_bytes(&self.slots) + vec_bytes(&self.free) + vec_bytes(&self.retired)
        }

        pub(super) fn get(&self, slot: u32) -> Option<&AnimationOverlayRecord> {
            self.slots[slot as usize].as_ref()
        }

        pub(super) fn get_mut(&mut self, slot: u32) -> Option<&mut AnimationOverlayRecord> {
            self.slots[slot as usize].as_mut()
        }

        pub(super) fn iter(&self) -> impl Iterator<Item = &AnimationOverlayRecord> {
            self.slots.iter().flatten()
        }

        /// Puts `record` in a free slot, and answers the slot and whether it is a new one.
        pub(super) fn fill(&mut self, record: AnimationOverlayRecord) -> (u32, bool) {
            if let Some(slot) = self.free.pop() {
                self.slots[slot as usize] = Some(record);
                return (slot, false);
            }
            let slot = u32::try_from(self.slots.len()).expect("animation-overlay slot space exhausted");
            self.slots.push(Some(record));
            (slot, true)
        }

        /// Retires the record in `slot` and frees the slot, answering the record's identity.
        pub(super) fn retire(&mut self, slot: u32) -> FinalStyleRecordID {
            let record = self.slots[slot as usize]
                .take()
                .expect("animation-overlay slot is live");
            self.free.push(slot);
            self.retire_record(record)
        }

        /// Retires the record in `slot` for `record`, answering the retired record's identity.
        pub(super) fn replace(&mut self, slot: u32, record: AnimationOverlayRecord) -> FinalStyleRecordID {
            let retired = self.slots[slot as usize]
                .replace(record)
                .expect("animation-overlay slot is live");
            self.retire_record(retired)
        }

        fn retire_record(&mut self, record: AnimationOverlayRecord) -> FinalStyleRecordID {
            let final_style_record = record.final_style_record();
            self.retired.push(record);
            final_style_record
        }

        pub(super) fn retired_records(&self) -> usize {
            self.retired.len()
        }

        /// Frees the retired records, which no reader may still read.
        pub(super) fn free_retired(&mut self) {
            self.retired.clear();
        }
    }
}

#[derive(Clone, Copy)]
struct PseudoComputedRow {
    kind: u8,
    highlight_name: StyleAtomID,
    published: bool,
    /// The installed record the pseudo-element is assigned.
    installed: Option<InstalledRecordID>,
    /// The slot of the animation overlay composed over `installed`.
    installed_overlay_slot: Option<u32>,
}

/// The rows of an element's pseudo-elements, of which it rarely has more than one.
type PseudoComputedRows = smallvec::SmallVec<[PseudoComputedRow; 1]>;

/// The slot of the animation overlay composed over each element's record, by element.
#[derive(Clone, Default)]
struct AnimationOverlayColumn(crate::fork_vec::ForkVec<u32>);

impl AnimationOverlayColumn {
    fn get(&self, index: usize) -> Option<u32> {
        self.0.get(index)?.checked_sub(1)
    }

    fn set(&mut self, index: usize, slot: Option<u32>) {
        if self.0.len() <= index {
            self.0
                .resize_with_default(index.checked_add(1).expect("animation overlay column space exhausted"));
        }
        self.0[index] = slot.map_or(0, |slot| {
            slot.checked_add(1)
                .expect("animation overlay slot identity space exhausted")
        });
    }

    /// Takes the slot at `index`, leaving none.
    fn take(&mut self, index: usize) -> Option<u32> {
        let slot = self.get(index);
        if slot.is_some() {
            self.0[index] = 0;
        }
        slot
    }
}

impl PseudoComputedRow {
    fn new(target: super::pipeline::PseudoElementTarget) -> Self {
        Self {
            kind: target.kind,
            highlight_name: target.highlight_name,
            published: false,
            installed: None,
            installed_overlay_slot: None,
        }
    }

    fn is_empty(&self) -> bool {
        !self.published && self.installed.is_none()
    }

    fn target(&self) -> super::pipeline::PseudoElementTarget {
        super::pipeline::PseudoElementTarget {
            kind: self.kind,
            highlight_name: self.highlight_name,
        }
    }
}
#[derive(Clone, Copy)]
struct AnimationOverlayPublication {
    slot: Option<u32>,
    final_style_record: FinalStyleRecordID,
    slot_allocated: bool,
    slot_released: bool,
    record_updated: bool,
}

pub struct AnimationOverlayUpdate {
    pub previous_style_record: FinalStyleRecordID,
    pub style_record: FinalStyleRecordID,
    pub slot_allocated: bool,
    pub slot_released: bool,
    pub record_updated: bool,
    pub live_records: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ComputedStyleTarget {
    node: StyleNodeID,
    pseudo_kind: u8,
    highlight_name: StyleAtomID,
}

impl ComputedStyleTarget {
    pub fn new(node: StyleNodeID, pseudo_kind: u8) -> Self {
        Self {
            node,
            pseudo_kind,
            highlight_name: StyleAtomID::NONE,
        }
    }

    pub(super) fn of_pseudo_element(node: StyleNodeID, target: super::pipeline::PseudoElementTarget) -> Self {
        Self {
            node,
            pseudo_kind: target.kind,
            highlight_name: target.highlight_name,
        }
    }

    #[must_use]
    pub(super) fn pseudo_element(self) -> super::pipeline::PseudoElementTarget {
        super::pipeline::PseudoElementTarget {
            kind: self.pseudo_kind,
            highlight_name: self.highlight_name,
        }
    }

    #[must_use]
    pub(super) fn node(self) -> StyleNodeID {
        self.node
    }

    #[must_use]
    pub(super) fn pseudo_kind(self) -> u8 {
        self.pseudo_kind
    }

    #[must_use]
    pub fn is_pseudo(self) -> bool {
        self.pseudo_kind != u8::MAX
    }
}

/// The record an element or a pseudo-element is assigned beneath the composition of its animations:
/// the installed record, and the record the engine computed that it installs. The installed record's
/// identity is the element's FinalStyleRecordID where no animation overlay is composed over it.
#[derive(Clone)]
pub(crate) struct InstalledStyle {
    pub(super) id: InstalledRecordID,
    pub(super) record: std::sync::Arc<super::pipeline::Record>,
}

impl InstalledStyle {
    /// The final style record the installed record is, beneath any composition.
    pub(crate) fn final_style_record(&self) -> FinalStyleRecordID {
        FinalStyleRecordID::installed(self.id)
    }
}

impl PartialEq for InstalledStyle {
    fn eq(&self, other: &Self) -> bool {
        self.id == other.id && std::sync::Arc::ptr_eq(&self.record, &other.record)
    }
}

/// A pseudo-element's [`InstalledStyle`], with its kind.
#[derive(Clone, PartialEq)]
pub(crate) struct InstalledPseudoStyle {
    pub(super) kind: u8,
    pub(super) style: InstalledStyle,
}

/// How many elements a chunk of the installed style columns holds.
const INSTALLED_STYLE_CHUNK: usize = 64;

/// The styles installed for an element's pseudo-elements, of which it rarely has more than one.
type InstalledPseudoStyles = smallvec::SmallVec<[InstalledPseudoStyle; 1]>;

/// The styles installed for the elements and their pseudo-elements, by element slot, as one
/// installation left them: a published generation of the sets' columns, which later
/// installations do not change. Publishing one costs a reference count however many elements
/// there are, and a reader holding it keeps the records it names alive.
#[derive(Clone, Default)]
pub(crate) struct InstalledStyles {
    elements: crate::cow_column::ColumnSnapshot<Option<InstalledStyle>, INSTALLED_STYLE_CHUNK>,
    pseudo_elements:
        crate::cow_column::ColumnSnapshot<Option<std::sync::Arc<InstalledPseudoStyles>>, INSTALLED_STYLE_CHUNK>,
}

impl InstalledStyles {
    /// The style installed for the element `node`, if any.
    pub(crate) fn element(&self, node: StyleNodeID) -> Option<&InstalledStyle> {
        self.elements.get(node.element_index()? as usize)?.as_ref()
    }

    /// The styles installed for the pseudo-elements of `node`, by kind, in no particular order.
    pub(crate) fn pseudo_elements(&self, node: StyleNodeID) -> &[InstalledPseudoStyle] {
        node.element_index()
            .and_then(|index| {
                self.pseudo_elements
                    .get(index as usize)?
                    .as_deref()
                    .map(|styles| &**styles)
            })
            .unwrap_or(&[])
    }

    /// The style installed for the pseudo-element of `kind` of `node`, if any.
    pub(crate) fn pseudo_element(&self, node: StyleNodeID, kind: u8) -> Option<&InstalledStyle> {
        self.pseudo_elements(node)
            .iter()
            .find(|pseudo| pseudo.kind == kind)
            .map(|pseudo| &pseudo.style)
    }
}

/// A copy of the sets holds its own reference to every record and overlay payload, and shares the
/// leases on its records.
#[derive(Clone)]
pub struct ComputedGroupSets {
    /// The records the engine installed.
    installed: InstalledRecords,
    /// The installed record each element is assigned, by element index.
    installed_column: crate::cow_column::CowColumn<Option<InstalledStyle>, INSTALLED_STYLE_CHUNK>,
    /// The installed records of each element's pseudo-elements, as the pseudo rows held them when
    /// the column was last published, by element index. A row is one thin pointer: most elements
    /// have none.
    installed_pseudo_column:
        crate::cow_column::CowColumn<Option<std::sync::Arc<InstalledPseudoStyles>>, INSTALLED_STYLE_CHUNK>,
    /// The elements whose pseudo rows moved since the pseudo column was last published.
    pseudo_rows_moved: crate::fork::ForkShared<Vec<StyleNodeID>>,
    /// Long-lived immutable readers pin only the records they actually read. Expired readers leave weak entries
    /// that the next lease removes, so they neither block reclamation nor need to call back into the owner.
    hover_frame_leases: std::sync::Arc<std::sync::Mutex<Vec<std::sync::Weak<HoverFrameLease>>>>,
    /// How many released records have the next sweep read the rows of the hover frames' leases.
    released_records_reading_leases: usize,
    /// How many records have to be released, as a sweep leaves them, for a reclamation to sweep them. A sweep reads
    /// every record it does not free, so a style update that releases a few sweeps only once they add up.
    released_records_sweeping: usize,
    // Recyclable animation overlays are deliberately separate from the installed records above.
    // Dense element assignments and sparse pseudo assignments pin at most one slot each.
    animation_overlay_column: AnimationOverlayColumn,
    animation_overlay_slots: crate::fork::ForkShared<animation_overlay_slots::AnimationOverlaySlots>,
    animation_overlay_slots_by_record: crate::cow_map::CowMap<FinalStyleRecordID, u32>,
    /// The slots of overlay records nothing assigns or pins but something else holds, such as the host for the element
    /// it last applied the record to, which go once nothing does.
    held_animation_overlay_slots: crate::fork::ForkShared<Vec<u32>>,
    released_animation_overlay_records: ReleasedAnimationOverlayRecords,
    /// How many leases are held, shared with every copy of the sets: a copy holds no lease of its own.
    // NB: Either copy may be the host's, whose published frames hold their leases here.
    leases: std::sync::Arc<std::sync::atomic::AtomicUsize>,
    live_animation_overlay_assignments: usize,
    next_animation_overlay_generation: u64,
    pseudo_rows_by_node: crate::cow_map::CowMap<StyleNodeID, PseudoComputedRows>,
    /// How many installed records were freed since what they named was last reclaimed.
    records_freed_since_reclamation: usize,
    next_reclamation_after: usize,
}

impl Default for ComputedGroupSets {
    fn default() -> Self {
        Self {
            installed: InstalledRecords::default(),
            installed_column: Default::default(),
            installed_pseudo_column: Default::default(),
            pseudo_rows_moved: Default::default(),
            hover_frame_leases: std::sync::Arc::default(),
            released_records_reading_leases: MIN_RECORDS_RELEASED_BEFORE_READING_LEASES,
            released_records_sweeping: MIN_RECORDS_RELEASED_BEFORE_SWEEPING,
            animation_overlay_column: AnimationOverlayColumn::default(),
            animation_overlay_slots: Default::default(),
            animation_overlay_slots_by_record: Default::default(),
            held_animation_overlay_slots: Default::default(),
            released_animation_overlay_records: ReleasedAnimationOverlayRecords::default(),
            leases: std::sync::Arc::default(),
            live_animation_overlay_assignments: 0,
            next_animation_overlay_generation: 0,
            pseudo_rows_by_node: Default::default(),
            records_freed_since_reclamation: 0,
            next_reclamation_after: 1024,
        }
    }
}

impl ComputedGroupSets {
    /// Counts the installed records and the columns naming them toward the installed records,
    /// and the records by owner.
    pub(super) fn report_memory(&self, report: &mut super::memory_report::MemoryReport) {
        use crate::heap_size::{small_vec_bytes, vec_bytes};
        self.installed.report_memory(report);
        for record in self.animation_overlay_slots.records() {
            super::pipeline::Record::report_memory(record, report);
        }
        let pseudo_rows = &self.pseudo_rows_by_node;
        report.add(
            super::memory_report::MemoryOwner::Installed,
            self.installed_column.len() * size_of::<Option<InstalledStyle>>()
                + self.installed_pseudo_column.len() * size_of::<Option<std::sync::Arc<InstalledPseudoStyles>>>()
                + vec_bytes(&self.pseudo_rows_moved)
                + self.animation_overlay_column.0.row_bytes()
                + self.animation_overlay_slots.heap_size()
                + self.animation_overlay_slots_by_record.heap_size()
                + vec_bytes(&self.held_animation_overlay_slots)
                + vec_bytes(&self.released_animation_overlay_records.records)
                + pseudo_rows.heap_size()
                + pseudo_rows.iter().map(|(_, rows)| small_vec_bytes(rows)).sum::<usize>(),
        );
    }

    pub(crate) fn lease_style_records(&self) -> StyleRecordLease {
        self.leases.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        StyleRecordLease {
            leases: std::sync::Arc::clone(&self.leases),
        }
    }

    /// A version of the records the elements are assigned, which every write of them moves.
    pub(crate) fn installed_column_version(&self) -> u64 {
        self.installed_column.version()
    }

    /// Leases the records the rows of an immutable hover frame name, none of which may be an animation overlay record,
    /// whose payloads such a lease does not keep.
    pub(crate) fn lease_hover_frame_style_records(
        &mut self,
        nodes: crate::cow_column::ColumnSnapshot<
            crate::layout::node_data::PaintNode,
            { crate::layout::tree_shape::PUBLISHED_ROWS_PER_CHUNK },
        >,
    ) -> std::sync::Arc<HoverFrameLease> {
        let lease = std::sync::Arc::new(HoverFrameLease {
            epoch: self.installed.begin_lease_epoch(),
            nodes,
        });
        let mut leases = self.hover_frame_leases.lock().expect("hover frame leases");
        leases.retain(|lease| lease.strong_count() != 0);
        leases.push(std::sync::Arc::downgrade(&lease));
        lease
    }

    /// Whether something holds the record `raw` names, where it names an installed one.
    pub(crate) fn style_record_is_held(&self, raw: u64) -> bool {
        FinalStyleRecordID(raw)
            .installed_record()
            .is_some_and(|installed| self.installed.is_held(installed))
    }

    fn style_records_are_leased(&self) -> bool {
        self.leases.load(std::sync::atomic::Ordering::Acquire) != 0
    }

    /// Frees the retired overlay records, if no lease is held, and answers whether it freed any.
    pub(super) fn free_retired_animation_overlays(&mut self) -> bool {
        if self.animation_overlay_slots.retired_records() == 0 || self.style_records_are_leased() {
            return false;
        }
        self.animation_overlay_slots.free_retired();
        true
    }

    pub(super) fn retired_animation_overlay_records(&self) -> usize {
        self.animation_overlay_slots.retired_records()
    }

    #[cfg(test)]
    fn has_released_animation_overlay_records(&self) -> bool {
        !self.released_animation_overlay_records.is_empty()
    }

    pub(super) fn take_released_animation_overlay_records(&mut self) -> ReleasedAnimationOverlayRecords {
        std::mem::take(&mut self.released_animation_overlay_records)
    }

    fn pseudo_rows(&self, node: StyleNodeID) -> &[PseudoComputedRow] {
        self.pseudo_rows_by_node.get(&node).map_or(&[], |rows| rows.as_slice())
    }

    fn pseudo_row(
        &self,
        node: StyleNodeID,
        target: super::pipeline::PseudoElementTarget,
    ) -> Option<&PseudoComputedRow> {
        self.pseudo_rows(node).iter().find(|row| row.target() == target)
    }

    fn pseudo_row_mut(
        &mut self,
        node: StyleNodeID,
        target: super::pipeline::PseudoElementTarget,
    ) -> Option<&mut PseudoComputedRow> {
        self.pseudo_rows_moved.push(node);
        self.pseudo_rows_by_node
            .get_mut(&node)
            .and_then(|rows| rows.iter_mut().find(|row| row.target() == target))
    }

    fn ensure_pseudo_row(
        &mut self,
        node: StyleNodeID,
        target: super::pipeline::PseudoElementTarget,
    ) -> &mut PseudoComputedRow {
        self.pseudo_rows_moved.push(node);
        let rows = self.pseudo_rows_by_node.entry(node).or_default();
        let row_index = rows.iter().position(|row| row.target() == target).unwrap_or_else(|| {
            rows.push(PseudoComputedRow::new(target));
            rows.len() - 1
        });
        &mut rows[row_index]
    }

    fn remove_empty_pseudo_row(&mut self, node: StyleNodeID, target: super::pipeline::PseudoElementTarget) {
        self.pseudo_rows_moved.push(node);
        let Some(rows) = self.pseudo_rows_by_node.get(&node) else {
            return;
        };
        let Some(row_index) = rows.iter().position(|row| row.target() == target && row.is_empty()) else {
            return;
        };
        let rows = self.pseudo_rows_by_node.get_mut(&node).expect("the node has rows");
        rows.remove(row_index);
        if rows.is_empty() {
            self.pseudo_rows_by_node.remove(&node);
        }
    }

    /// Take a slot for a record the engine computed, or answer the one it holds already.
    /// `dependency_flags` says what the view of a record in a new slot depends on.
    pub(super) fn install_record(
        &mut self,
        record: &std::sync::Arc<super::pipeline::Record>,
        dependency_flags: impl FnOnce() -> u8,
    ) -> FinalStyleRecordID {
        FinalStyleRecordID::installed(self.installed.install(record, dependency_flags))
    }

    /// Take a slot for a record derived for what no element is, or answer one holding the same.
    /// `dependency_flags` says what the view of a record in a new slot depends on.
    pub(super) fn install_derived_record(
        &mut self,
        record: &std::sync::Arc<super::pipeline::Record>,
        dependency_flags: impl FnOnce() -> u8,
    ) -> FinalStyleRecordID {
        FinalStyleRecordID::installed(self.installed.install_derived(record, dependency_flags))
    }

    /// The identity `record` holds already, if it holds one.
    pub(super) fn installed_record_named_by(
        &self,
        record: &std::sync::Arc<super::pipeline::Record>,
    ) -> Option<FinalStyleRecordID> {
        self.installed.named_by(record).map(FinalStyleRecordID::installed)
    }

    /// The record the engine computed that the installed `record` installs, where it is one.
    pub(super) fn installed_record(
        &self,
        record: FinalStyleRecordID,
    ) -> Option<&std::sync::Arc<super::pipeline::Record>> {
        Some(self.installed.get(record.installed_record()?)?.record())
    }

    /// The record the engine computed that `record` installs or composes animations over, where it
    /// is one.
    pub(super) fn installed_base_record(
        &self,
        record: FinalStyleRecordID,
    ) -> Option<&std::sync::Arc<super::pipeline::Record>> {
        let installed = match record.installed_record() {
            Some(installed) => installed,
            None => self.overlay_base(record)?,
        };
        Some(self.installed.get(installed)?.record())
    }

    /// Assign `node` the installed `record` in place of what it held, which it lets go of.
    pub(super) fn assign_installed_record(&mut self, node: StyleNodeID, record: FinalStyleRecordID) {
        let installed = record.installed_record().expect("an installed record");
        let index = node.element_index().expect("only elements are assigned records") as usize;
        self.installed.hold(installed);
        if let Some(slot) = self.animation_overlay_column.take(index) {
            self.release_animation_overlay_assignment(slot);
        }
        self.installed_column.grow_to(index + 1);
        let record = self
            .installed
            .get(installed)
            .expect("an assigned record is installed")
            .record()
            .clone();
        let previous = self
            .installed_column
            .replace(index, Some(InstalledStyle { id: installed, record }))
            .expect("the column holds the element");
        if let Some(previous) = previous {
            self.installed.release(previous.id);
        }
    }

    /// Assign `node`'s pseudo-element of `pseudo_kind` the installed `record` in place of what it
    /// held.
    pub(super) fn assign_installed_pseudo_record(
        &mut self,
        node: StyleNodeID,
        pseudo_kind: u8,
        record: FinalStyleRecordID,
    ) {
        self.assign_installed_pseudo_record_of(
            node,
            super::pipeline::PseudoElementTarget::of_kind(pseudo_kind),
            record,
        );
    }

    pub(super) fn assign_installed_pseudo_record_of(
        &mut self,
        node: StyleNodeID,
        target: super::pipeline::PseudoElementTarget,
        record: FinalStyleRecordID,
    ) {
        let installed = record.installed_record().expect("an installed record");
        self.installed.hold(installed);
        let row = self.ensure_pseudo_row(node, target);
        row.published = true;
        let previous = row.installed.replace(installed);
        let overlay = row.installed_overlay_slot.take();
        if let Some(previous) = previous {
            self.installed.release(previous);
        }
        if let Some(slot) = overlay {
            self.release_animation_overlay_assignment(slot);
        }
    }

    /// The styles installed for every element and pseudo-element now, as a generation later
    /// installations leave as it is.
    pub(crate) fn publish_installed_styles(&mut self) -> InstalledStyles {
        let mut moved = std::mem::take(&mut self.pseudo_rows_moved);
        moved.sort_unstable();
        moved.dedup();
        for &node in moved.iter() {
            let Some(index) = node.element_index().map(|index| index as usize) else {
                continue;
            };
            let styles: InstalledPseudoStyles = self
                .pseudo_rows(node)
                .iter()
                .filter(|row| row.highlight_name.is_none())
                .filter_map(|row| {
                    let id = row.installed?;
                    Some(InstalledPseudoStyle {
                        kind: row.kind,
                        style: InstalledStyle {
                            id,
                            record: self.installed.get(id)?.record().clone(),
                        },
                    })
                })
                .collect();
            let styles = (!styles.is_empty()).then(|| std::sync::Arc::new(styles));
            if styles.is_some() {
                self.installed_pseudo_column.grow_to(index + 1);
            }
            let _ = self.installed_pseudo_column.set(index, styles);
        }
        moved.clear();
        self.pseudo_rows_moved = moved;
        InstalledStyles {
            elements: self.installed_column.publish(),
            pseudo_elements: self.installed_pseudo_column.publish(),
        }
    }

    pub(super) fn assigned_style_record(&self, node: StyleNodeID) -> Option<FinalStyleRecordID> {
        let index = node.element_index()? as usize;
        let installed = self.installed_column.get(index)?.as_ref()?.id;
        Some(self.final_style_record(installed, self.animation_overlay_column.get(index)))
    }
    pub(super) fn assigned_final_style_record(&self, target: ComputedStyleTarget) -> Option<FinalStyleRecordID> {
        if !target.is_pseudo() {
            return self.assigned_style_record(target.node);
        }
        self.pseudo_style_record(target.node, target.pseudo_kind)
    }

    /// The nodes whose element or pseudo-element style depends on viewport metrics.
    pub(super) fn viewport_dependent_nodes(&self) -> Vec<u32> {
        let installed_depends_on_viewport = |installed: InstalledRecordID| {
            self.installed
                .get(installed)
                .is_some_and(|installed| installed.dependency_flags() & DEPENDS_ON_VIEWPORT_METRICS != 0)
        };
        let mut nodes = Vec::new();
        for index in 0..self.installed_column.len() {
            if self
                .installed_column
                .get(index)
                .and_then(Option::as_ref)
                .is_some_and(|installed| installed_depends_on_viewport(installed.id))
            {
                nodes.push(u32::try_from(index).expect("computed style node identity exceeds u32"));
            }
        }
        for (&node, rows) in self.pseudo_rows_by_node.iter() {
            if rows
                .iter()
                .any(|row| row.installed.is_some_and(installed_depends_on_viewport))
            {
                nodes.push(node.raw());
            }
        }
        nodes.sort_unstable();
        nodes.dedup();
        nodes
    }

    /// The record a composition is laid over: the installed record an animation overlay record
    /// composes over, or the record itself where it composes nothing.
    pub(crate) fn underlying_style_record(&self, record: FinalStyleRecordID) -> Option<FinalStyleRecordID> {
        if record.installed_record().is_some() {
            return Some(record);
        }
        Some(FinalStyleRecordID::installed(self.overlay_base(record)?))
    }

    /// The installed record an animation overlay record composes over.
    fn overlay_base(&self, overlay: FinalStyleRecordID) -> Option<InstalledRecordID> {
        let slot = *self.animation_overlay_slots_by_record.get(&overlay)?;
        Some(self.animation_overlay_slots.get(slot)?.base)
    }

    /// The installed record a target holds beneath its composition, and the composition's slot.
    fn assigned_record_and_overlay_slot(
        &self,
        target: ComputedStyleTarget,
    ) -> Option<(InstalledRecordID, Option<u32>)> {
        if target.is_pseudo() {
            let row = self.pseudo_row(target.node, target.pseudo_element())?;
            return Some((row.installed?, row.installed_overlay_slot));
        }
        let index = target.node.element_index()? as usize;
        Some((
            self.installed_column.get(index)?.as_ref()?.id,
            self.animation_overlay_column.get(index),
        ))
    }

    /// Whether a target holds a record beneath its composition.
    pub(crate) fn holds_an_installed_record(&self, target: ComputedStyleTarget) -> bool {
        self.assigned_record_and_overlay_slot(target).is_some()
    }

    /// Whether an element holds `record`, a record the engine computed for it, beneath the
    /// composition of its animations: the record installed for it, or one installed that holds
    /// the same. An element holding no record yet has had nothing installed in place of what style
    /// the engine computed for it.
    pub(super) fn holds_record(&self, node: StyleNodeID, record: &std::sync::Arc<super::pipeline::Record>) -> bool {
        let Some(index) = node.element_index() else {
            return false;
        };
        let Some(installed) = self.installed_column.get(index as usize).and_then(Option::as_ref) else {
            return true;
        };
        std::sync::Arc::ptr_eq(&installed.record, record) || installed.record.holds_the_same_style_as(record)
    }

    /// The record the engine installed that an element holds beneath the composition of its
    /// animations, if any.
    pub(super) fn held_record(&self, node: StyleNodeID) -> Option<&std::sync::Arc<super::pipeline::Record>> {
        Some(
            &self
                .installed_column
                .get(node.element_index()? as usize)?
                .as_ref()?
                .record,
        )
    }

    /// The view of the composition of an element's animations over `record`, a record the engine
    /// computed for it, where the element holds that record beneath the composition.
    pub(super) fn composition_over_record(
        &self,
        node: StyleNodeID,
        record: &std::sync::Arc<super::pipeline::Record>,
    ) -> Option<StyleRecordView<'_>> {
        let slot = self.animation_overlay_column.get(node.element_index()? as usize)?;
        if !self.holds_record(node, record) {
            return None;
        }
        let overlay = self.animation_overlay_slots.get(slot)?;
        self.style_record_view(overlay.final_style_record().raw())
    }

    fn set_animation_overlay_slot_of(&mut self, target: ComputedStyleTarget, slot: Option<u32>) {
        if target.is_pseudo() {
            let Some(row) = self.pseudo_row_mut(target.node, target.pseudo_element()) else {
                return;
            };
            if row.installed.is_some() {
                row.installed_overlay_slot = slot;
            }
        } else if let Some(index) = target.node.element_index().map(|index| index as usize) {
            self.animation_overlay_column.set(index, slot);
        }
    }
    fn next_animation_overlay_record(&mut self) -> FinalStyleRecordID {
        self.next_animation_overlay_generation = self
            .next_animation_overlay_generation
            .checked_add(1)
            .expect("animation-overlay generation space exhausted");
        FinalStyleRecordID::animation_overlay(self.next_animation_overlay_generation)
    }

    fn make_animation_overlay_record(
        &mut self,
        base: InstalledRecordID,
        source_identity: u64,
        animated_overlay: Box<crate::css::animated_overlay::AnimatedOverlay>,
        payloads: &crate::css::computed_values::StyleGroups,
        in_display_none_subtree: Option<bool>,
    ) -> AnimationOverlayRecord {
        let final_style_record = self.next_animation_overlay_record();
        let installed = self.installed.get(base).expect("installed style record is live");
        let mut dependency_flags = installed.dependency_flags();
        // SAFETY: Published style-group payloads stay live while their records do.
        if unsafe { style_group_payloads_hold_image_values(&payloads.pointers()) } {
            dependency_flags |= HOLDS_IMAGE_VALUES;
        }
        if let Some(in_display_none_subtree) = in_display_none_subtree {
            dependency_flags = (dependency_flags & !IN_DISPLAY_NONE_SUBTREE)
                | (u8::from(in_display_none_subtree) * IN_DISPLAY_NONE_SUBTREE);
        }
        let record = std::sync::Arc::new(super::pipeline::Record::sample(
            installed.record(),
            payloads,
            std::sync::Arc::from(animated_overlay),
            dependency_flags,
            final_style_record,
        ));
        AnimationOverlayRecord {
            base,
            source_identity,
            record,
            pin_count: 0,
            is_assigned: true,
        }
    }

    fn allocate_animation_overlay(
        &mut self,
        base: InstalledRecordID,
        source_identity: u64,
        animated_overlay: Option<&crate::css::animated_overlay::AnimatedOverlay>,
        payloads: &crate::css::computed_values::StyleGroups,
        in_display_none_subtree: Option<bool>,
    ) -> (u32, FinalStyleRecordID, bool) {
        let record = self.make_animation_overlay_record(
            base,
            source_identity,
            Box::new(
                animated_overlay
                    .expect("animation overlay properties are missing")
                    .clone(),
            ),
            payloads,
            in_display_none_subtree,
        );

        let final_style_record = record.final_style_record();
        let (slot, slot_allocated) = self.animation_overlay_slots.fill(record);
        self.animation_overlay_slots_by_record.insert(final_style_record, slot);
        self.live_animation_overlay_assignments += 1;
        (slot, final_style_record, slot_allocated)
    }

    /// Reclaims `slot`, which nothing assigns or pins, unless something else holds its record: then it goes once nothing
    /// does.
    fn reclaim_animation_overlay_slot_unless_held(&mut self, slot: u32) {
        let record = self
            .animation_overlay_slots
            .get(slot)
            .expect("animation-overlay slot is live");
        if record.is_held_elsewhere() {
            self.held_animation_overlay_slots.push(slot);
            return;
        }
        self.reclaim_animation_overlay_slot(slot);
    }

    /// Reclaims the slots of the overlay records nothing assigns, pins or holds anymore.
    fn sweep_held_animation_overlays(&mut self) {
        if self.held_animation_overlay_slots.is_empty() {
            return;
        }
        let mut slots = std::mem::take(&mut self.held_animation_overlay_slots);
        slots.sort_unstable();
        slots.dedup();
        for slot in slots.iter().copied() {
            // NB: Taken for writing so that a fork sharing the slots has a copy of its own, which counts as holding the
            //     record. A slot a pin took again goes back on the list when its last pin goes.
            let Some(record) = self.animation_overlay_slots.get_mut(slot) else {
                continue;
            };
            if record.is_assigned || record.pin_count != 0 {
                continue;
            }
            if record.is_held_elsewhere() {
                self.held_animation_overlay_slots.push(slot);
            } else {
                self.reclaim_animation_overlay_slot(slot);
            }
        }
    }

    fn reclaim_animation_overlay_slot(&mut self, slot: u32) {
        let record = self
            .animation_overlay_slots
            .get(slot)
            .expect("animation-overlay slot is live");
        assert!(!record.is_assigned && record.pin_count == 0 && !record.is_held_elsewhere());
        let final_style_record = self.animation_overlay_slots.retire(slot);
        self.animation_overlay_slots_by_record.remove(&final_style_record);
        self.released_animation_overlay_records.add(final_style_record);
        self.free_retired_animation_overlays();
    }

    fn release_animation_overlay_assignment(&mut self, slot: u32) {
        let record = self
            .animation_overlay_slots
            .get_mut(slot)
            .expect("animation-overlay slot is live");
        assert!(record.is_assigned, "animation-overlay slot has an assignment");
        record.is_assigned = false;
        self.live_animation_overlay_assignments -= 1;
        if record.pin_count == 0 {
            self.reclaim_animation_overlay_slot_unless_held(slot);
        }
    }

    fn update_animation_overlay(
        &mut self,
        current_slot: Option<u32>,
        base: InstalledRecordID,
        source_identity: u64,
        animated_overlay: Option<&crate::css::animated_overlay::AnimatedOverlay>,
        payloads: Option<&crate::css::computed_values::StyleGroups>,
        in_display_none_subtree: Option<bool>,
    ) -> AnimationOverlayPublication {
        if source_identity == 0 {
            if let Some(slot) = current_slot {
                self.release_animation_overlay_assignment(slot);
            }
            return AnimationOverlayPublication {
                slot: None,
                final_style_record: FinalStyleRecordID::installed(base),
                slot_allocated: false,
                slot_released: current_slot.is_some(),
                record_updated: false,
            };
        }
        let payloads = payloads.expect("an animation overlay composes groups over its record");

        if let Some(slot) = current_slot {
            let current = self
                .animation_overlay_slots
                .get(slot)
                .expect("animation-overlay slot is live");
            if current.base == base
                && current.source_identity == source_identity
                && current.record.payloads.holds_the_same_groups_as(payloads)
            {
                return AnimationOverlayPublication {
                    slot: Some(slot),
                    final_style_record: current.final_style_record(),
                    slot_allocated: false,
                    slot_released: false,
                    record_updated: false,
                };
            }
            // NB: Taken for writing so that a fork sharing the slots has a copy of its own, which counts as holding the
            //     record. Both ways on from here write the slots.
            let current = self
                .animation_overlay_slots
                .get_mut(slot)
                .expect("animation-overlay slot is live");
            if current.pin_count == 0 && !current.is_held_elsewhere() {
                let record = self.make_animation_overlay_record(
                    base,
                    source_identity,
                    Box::new(
                        animated_overlay
                            .expect("animation overlay properties are missing")
                            .clone(),
                    ),
                    payloads,
                    in_display_none_subtree,
                );

                let final_style_record = record.final_style_record();
                let retired = self.animation_overlay_slots.replace(slot, record);
                self.animation_overlay_slots_by_record.remove(&retired);
                self.released_animation_overlay_records.add(retired);
                self.animation_overlay_slots_by_record.insert(final_style_record, slot);
                self.free_retired_animation_overlays();
                return AnimationOverlayPublication {
                    slot: Some(slot),
                    final_style_record,
                    slot_allocated: false,
                    slot_released: false,
                    record_updated: true,
                };
            }
            self.release_animation_overlay_assignment(slot);
        }

        let (slot, final_style_record, slot_allocated) = self.allocate_animation_overlay(
            base,
            source_identity,
            animated_overlay,
            payloads,
            in_display_none_subtree,
        );
        AnimationOverlayPublication {
            slot: Some(slot),
            final_style_record,
            slot_allocated,
            slot_released: false,
            record_updated: true,
        }
    }

    fn final_style_record(&self, base: InstalledRecordID, animation_overlay_slot: Option<u32>) -> FinalStyleRecordID {
        animation_overlay_slot.map_or_else(
            || FinalStyleRecordID::installed(base),
            |slot| {
                self.animation_overlay_slots
                    .get(slot)
                    .expect("animation-overlay slot is live")
                    .final_style_record()
            },
        )
    }

    pub fn publish_animation_overlay(
        &mut self,
        target: ComputedStyleTarget,
        source_identity: u64,
        animated_overlay: HostShared<crate::css::animated_overlay::AnimatedOverlay>,
        payloads: Option<&crate::css::computed_values::StyleGroups>,
        parent_in_display_none_subtree: bool,
    ) -> Option<AnimationOverlayUpdate> {
        let (base, current_slot) = self.assigned_record_and_overlay_slot(target)?;
        let previous_style_record = self.final_style_record(base, current_slot);
        let animated_overlay = unsafe { animated_overlay.as_ref() };
        let in_display_none_subtree =
            self.animation_overlay_in_display_none_subtree(base, animated_overlay, parent_in_display_none_subtree);
        let publication = self.update_animation_overlay(
            current_slot,
            base,
            source_identity,
            animated_overlay,
            payloads,
            in_display_none_subtree,
        );
        self.set_animation_overlay_slot_of(target, publication.slot);
        Some(AnimationOverlayUpdate {
            previous_style_record,
            style_record: publication.final_style_record,
            slot_allocated: publication.slot_allocated,
            slot_released: publication.slot_released,
            record_updated: publication.record_updated,
            live_records: self.live_animation_overlay_assignments,
        })
    }

    fn animation_overlay_in_display_none_subtree(
        &self,
        base: InstalledRecordID,
        animated_overlay: Option<&crate::css::animated_overlay::AnimatedOverlay>,
        parent_in_display_none_subtree: bool,
    ) -> Option<bool> {
        // The base computed its flag from its own display, which the sampled display overrides.
        let longhands = ViewLonghands::of_record(self.installed.get(base)?.record());
        Some(parent_in_display_none_subtree || longhands.effective_display(animated_overlay).is_none())
    }

    pub fn pseudo_style_record(&self, node: StyleNodeID, pseudo_kind: u8) -> Option<FinalStyleRecordID> {
        self.pseudo_style_record_of(node, super::pipeline::PseudoElementTarget::of_kind(pseudo_kind))
    }

    pub(super) fn pseudo_style_record_of(
        &self,
        node: StyleNodeID,
        target: super::pipeline::PseudoElementTarget,
    ) -> Option<FinalStyleRecordID> {
        let row = self.pseudo_row(node, target)?;
        Some(self.final_style_record(row.installed?, row.installed_overlay_slot))
    }

    pub(super) fn custom_highlight_records(
        &self,
        node: StyleNodeID,
    ) -> impl Iterator<Item = (StyleAtomID, FinalStyleRecordID)> + '_ {
        self.pseudo_rows(node).iter().filter_map(move |row| {
            if row.highlight_name.is_none() {
                return None;
            }
            Some((
                row.highlight_name,
                self.final_style_record(row.installed?, row.installed_overlay_slot),
            ))
        })
    }

    /// Each record a pseudo-element holds, with the node it belongs to.
    pub(super) fn pseudo_style_records(&self) -> impl Iterator<Item = (StyleNodeID, FinalStyleRecordID)> + '_ {
        self.pseudo_rows_by_node.iter().flat_map(move |(&node, rows)| {
            rows.iter().filter_map(move |row| {
                Some((
                    node,
                    self.final_style_record(row.installed?, row.installed_overlay_slot),
                ))
            })
        })
    }
    /// The pseudo-element kinds this node holds published computed styles for.
    pub fn assigned_pseudo_kinds(&self, node: StyleNodeID) -> impl Iterator<Item = u8> + '_ {
        self.pseudo_rows(node)
            .iter()
            .filter_map(|row| (row.published && row.highlight_name.is_none()).then_some(row.kind))
    }

    /// The groups whose values read, from `values`, the recorded specified values of the longhands
    /// whose computed values depend on what the element inherits, one that `depends_on_input`.
    pub(super) fn specified_value_dependency_mask_of<'a>(
        values: impl Iterator<Item = (u16, &'a RetainedStyleValueData)>,
        depends_on_input: impl Fn(&RetainedStyleValueData) -> bool,
    ) -> u32 {
        values
            .filter(|(_, value)| depends_on_input(value))
            // Logical aliases retain their specified values but own no output payload. Their
            // resolved physical longhands are recorded separately and name the actual group.
            .filter_map(|(property, _)| computed_group_output_mask(property))
            .fold(0, |mask, groups| mask | groups)
    }

    /// The groups whose specified values in `longhands` actually read `currentColor` while computing.
    #[must_use]
    pub(crate) fn current_color_dependency_mask(longhands: ViewLonghands<'_>) -> u32 {
        Self::specified_value_dependency_mask_of(
            longhands.inheritance_dependent_values(),
            retained_value_depends_on_current_color,
        ) | crate::css::table_group_builder::groups_reading_current_color_through_inherited_values(|property| {
            longhands.value(property)
        })
    }

    /// Whether a raw final record identity still names a live installed record.
    pub fn final_style_record_is_live(&self, raw_style_record: u64) -> bool {
        FinalStyleRecordID(raw_style_record)
            .installed_record()
            .is_some_and(|installed| self.installed.get(installed).is_some())
    }

    /// The raw custom-property environment identity behind a node's record.
    pub fn custom_property_environment_identity(&self, node: StyleNodeID) -> Option<u64> {
        let installed = self.installed_column.get(node.element_index()? as usize)?.as_ref()?.id;
        Some(self.installed.get(installed)?.custom_property_environment())
    }

    /// Each element holding an installed record, with the custom-property environment that names.
    pub(super) fn installed_custom_property_environments(&self) -> impl Iterator<Item = (StyleNodeID, u64)> + '_ {
        (0..self.installed_column.len()).filter_map(|index| {
            let installed = self.installed_column.get(index)?.as_ref()?;
            let node = StyleNodeID::element(u32::try_from(index).ok()?);
            Some((node, installed.record.custom_property_environment()))
        })
    }

    /// Every raw custom-property environment identity a live record was installed with.
    pub fn live_custom_property_environments(&self) -> impl Iterator<Item = u64> + '_ {
        self.installed.iter().map(InstalledRecord::custom_property_environment)
    }

    /// The raw custom-property environment identity a record was installed with. An animation overlay record animates
    /// no custom property, and holds the environment of the record it composes over.
    pub fn style_record_custom_property_environment(&self, raw_style_record: u64) -> Option<u64> {
        let record = FinalStyleRecordID(raw_style_record);
        let installed = match record.installed_record() {
            Some(installed) => installed,
            None => self.overlay_base(record)?,
        };
        Some(self.installed.get(installed)?.custom_property_environment())
    }

    pub fn remove(&mut self, node: StyleNodeID) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        if let Some(installed) = self.installed_column.replace(index, None).flatten() {
            self.installed.release(installed.id);
        }
        if let Some(slot) = self.animation_overlay_column.take(index) {
            self.release_animation_overlay_assignment(slot);
        }
        self.pseudo_rows_moved.push(node);
        if let Some(rows) = self.pseudo_rows_by_node.remove(&node) {
            for row in rows {
                if let Some(installed) = row.installed {
                    self.installed.release(installed);
                }
                if let Some(slot) = row.installed_overlay_slot {
                    self.release_animation_overlay_assignment(slot);
                }
            }
        }
    }

    pub(super) fn remove_pseudo_of(
        &mut self,
        node: StyleNodeID,
        target: super::pipeline::PseudoElementTarget,
    ) -> Option<FinalStyleRecordID> {
        let row = self.pseudo_row_mut(node, target)?;
        let installed = row.installed.take()?;
        let overlay = row.installed_overlay_slot.take();
        row.published = false;
        let final_style_record = self.final_style_record(installed, overlay);
        if let Some(slot) = overlay {
            self.release_animation_overlay_assignment(slot);
        }
        self.installed.release(installed);
        self.remove_empty_pseudo_row(node, target);
        Some(final_style_record)
    }
    #[must_use]
    pub fn live_animation_overlay_records(&self) -> usize {
        self.live_animation_overlay_assignments
    }

    /// Free the installed records nothing holds, but for those an immutable hover frame may read.
    fn sweep_installed_records(&mut self) {
        if self.installed.released_count() == 0 {
            return;
        }
        let leases: Vec<_> = self
            .hover_frame_leases
            .lock()
            .expect("hover frame leases")
            .iter()
            .filter_map(std::sync::Weak::upgrade)
            .collect();
        // NB: An animation overlay keeps the installed record it composes over.
        let mut kept: super::fast_hash::FastSet<InstalledRecordID> = self
            .animation_overlay_slots
            .iter()
            .map(|overlay| overlay.base)
            .collect();
        // NB: A record let go of before a lease was taken is no record its frame names, which tells most of them with no
        //     row read, but a task that sees no next frame would keep all it lets go of.
        let reads_leases = self.installed.released_count() >= self.released_records_reading_leases;
        let oldest_lease = match reads_leases {
            true => None,
            false => leases.iter().map(|lease| lease.epoch).min(),
        };
        let mut rows_read = 0;
        for lease in leases.iter().filter(|_| reads_leases) {
            rows_read += lease.nodes.slot_capacity();
            let mut last = 0;
            for slot in 0..lease.nodes.slot_capacity() {
                let record = lease.nodes.get(slot).map_or(0, |node| node.style_record);
                if record != last {
                    last = record;
                    kept.extend(FinalStyleRecordID(record).installed_record());
                }
            }
        }
        let freed = self.installed.sweep(&kept, oldest_lease);
        if reads_leases {
            self.released_records_reading_leases =
                self.installed.released_count() + rows_read.max(MIN_RECORDS_RELEASED_BEFORE_READING_LEASES);
        }
        self.records_freed_since_reclamation = self.records_freed_since_reclamation.saturating_add(freed);
    }

    /// Frees the installed records nothing holds or leases.
    #[cfg(test)]
    pub(super) fn reclaim_unreachable(&mut self) {
        assert!(!self.style_records_are_leased(), "style records are leased");
        self.sweep_held_animation_overlays();
        self.sweep_installed_records();
    }

    /// Has the next reclamation sweep however few records were released.
    pub(super) fn sweep_at_next_reclamation(&mut self) {
        self.released_records_sweeping = 0;
    }

    /// Frees the installed records nothing holds, where nothing views or leases them, and answers
    /// whether enough were freed since the last time it did for what they named to be reclaimed.
    pub(super) fn reclaim_unreachable_if_needed(&mut self, records_are_viewed: bool) -> bool {
        self.sweep_held_animation_overlays();
        self.free_retired_animation_overlays();
        if records_are_viewed || self.style_records_are_leased() {
            return false;
        }
        if self.installed.released_count() >= self.released_records_sweeping {
            self.sweep_installed_records();
            self.released_records_sweeping =
                (self.installed.released_count() * 2).max(MIN_RECORDS_RELEASED_BEFORE_SWEEPING);
        }
        if self.records_freed_since_reclamation < self.next_reclamation_after {
            return false;
        }
        self.records_freed_since_reclamation = 0;
        self.next_reclamation_after = self.installed.iter().count().max(1024);
        true
    }
    /// The installed record `record` names, which must be live where it names one.
    fn live_installed(&self, record: FinalStyleRecordID) -> Option<&InstalledRecord> {
        let installed = record.installed_record()?;
        Some(
            self.installed
                .get(installed)
                .expect("installed style-record is not live"),
        )
    }

    pub fn style_record_payloads(&self, raw_style_record: u64) -> Option<&crate::css::computed_values::StyleGroups> {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        if let Some(installed) = self.live_installed(final_style_record) {
            return Some(&installed.record().payloads);
        }
        let slot = *self.animation_overlay_slots_by_record.get(&final_style_record)?;
        let overlay = self.animation_overlay_slots.get(slot)?;
        Some(&overlay.record.payloads)
    }

    pub fn style_record_dependency_flags(&self, raw_style_record: u64) -> Option<u8> {
        Some(self.style_record_view(raw_style_record)?.dependency_flags)
    }

    pub(crate) fn style_record_view(&self, raw_style_record: u64) -> Option<StyleRecordView<'_>> {
        self.host_record(raw_style_record).map(StyleRecordView::of_host_record)
    }

    /// The record the host holds for an element or pseudo-element assigned the live record
    /// `raw_style_record`: the installed record, or the sample of the animations over one.
    pub(crate) fn host_record(&self, raw_style_record: u64) -> Option<&std::sync::Arc<super::pipeline::Record>> {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        if let Some(installed) = self.live_installed(final_style_record) {
            return Some(installed.record());
        }
        let slot = *self.animation_overlay_slots_by_record.get(&final_style_record)?;
        Some(&self.animation_overlay_slots.get(slot)?.record)
    }

    pub fn pin_style_record(&mut self, raw_style_record: u64) {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        if let Some(installed) = final_style_record.installed_record() {
            self.installed.hold(installed);
            return;
        }
        let slot = *self
            .animation_overlay_slots_by_record
            .get(&final_style_record)
            .expect("animation-overlay record is live");
        let record = self
            .animation_overlay_slots
            .get_mut(slot)
            .expect("animation-overlay slot is live");
        record.pin_count = record
            .pin_count
            .checked_add(1)
            .expect("animation-overlay pin count overflow");
    }

    pub fn unpin_style_record(&mut self, raw_style_record: u64) {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        if let Some(installed) = final_style_record.installed_record() {
            self.installed.release(installed);
            return;
        }
        let slot = *self
            .animation_overlay_slots_by_record
            .get(&final_style_record)
            .expect("animation-overlay record is live");
        let record = self
            .animation_overlay_slots
            .get_mut(slot)
            .expect("animation-overlay slot is live");
        record.pin_count = record
            .pin_count
            .checked_sub(1)
            .expect("animation-overlay record is pinned");
        if !record.is_assigned && record.pin_count == 0 {
            self.reclaim_animation_overlay_slot_unless_held(slot);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A record of the initial values holding the default payloads.
    fn initial_record() -> std::sync::Arc<super::super::pipeline::Record> {
        std::sync::Arc::new(super::super::pipeline::Record::of_test_values(|property| {
            crate::css::style_compute::initial_value_handle(property).clone()
        }))
    }

    /// Installs a record of the initial values told apart by `pseudo_element_styles`, whose view
    /// depends on `dependency_flags`.
    fn install(sets: &mut ComputedGroupSets, pseudo_element_styles: u64, dependency_flags: u8) -> FinalStyleRecordID {
        let record = initial_record()
            .with_pseudo_element_styles(super::super::pipeline::PseudoElementStyles::from_bits(
                pseudo_element_styles,
            ))
            .with_test_dependency_flags(dependency_flags);
        sets.install_derived_record(&record, || 0)
    }

    /// Assigns `target` a record installed as [`install`] installs it.
    fn assign(
        sets: &mut ComputedGroupSets,
        target: ComputedStyleTarget,
        pseudo_element_styles: u64,
        dependency_flags: u8,
    ) -> FinalStyleRecordID {
        let record = install(sets, pseudo_element_styles, dependency_flags);
        match target.is_pseudo() {
            true => sets.assign_installed_pseudo_record(target.node, target.pseudo_kind, record),
            false => sets.assign_installed_record(target.node, record),
        }
        record
    }

    /// Composes an animation overlay of source `identity` over the record `target` holds.
    fn compose(
        sets: &mut ComputedGroupSets,
        defaults: &crate::css::computed_values::DefaultGroups,
        target: ComputedStyleTarget,
        animated_overlay: &crate::css::animated_overlay::AnimatedOverlay,
        identity: u64,
    ) -> AnimationOverlayUpdate {
        sets.publish_animation_overlay(
            target,
            identity,
            HostShared::new(std::ptr::from_ref(animated_overlay)),
            Some(defaults.groups()),
            false,
        )
        .expect("the target holds a record")
    }

    #[test]
    fn viewport_dependent_nodes_include_elements_and_pseudo_owners_once() {
        let mut sets = ComputedGroupSets::default();
        let independent = ComputedStyleTarget::new(StyleNodeID::element(1), u8::MAX);
        let viewport_dependent = ComputedStyleTarget::new(StyleNodeID::element(2), u8::MAX);
        let font_dependent_pseudo = ComputedStyleTarget::new(StyleNodeID::element(3), 1);
        let viewport_dependent_pseudo = ComputedStyleTarget::new(StyleNodeID::element(2), 2);
        let viewport_dependent_pseudo_only = ComputedStyleTarget::new(StyleNodeID::element(4), 1);
        assign(&mut sets, independent, 0, 0);
        assign(&mut sets, viewport_dependent, 1, DEPENDS_ON_VIEWPORT_METRICS);
        assign(
            &mut sets,
            font_dependent_pseudo,
            2,
            crate::css::computed_longhand_table::FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS,
        );
        assign(&mut sets, viewport_dependent_pseudo, 1, DEPENDS_ON_VIEWPORT_METRICS);
        assign(
            &mut sets,
            viewport_dependent_pseudo_only,
            1,
            DEPENDS_ON_VIEWPORT_METRICS,
        );

        assert_eq!(sets.viewport_dependent_nodes(), vec![2, 4]);
    }

    #[test]
    fn records_holding_the_same_in_one_context_share_an_identity() {
        let mut sets = ComputedGroupSets::default();
        let first = ComputedStyleTarget::new(StyleNodeID::element(1), u8::MAX);
        let second = ComputedStyleTarget::new(StyleNodeID::element(2), u8::MAX);
        let first_record = assign(&mut sets, first, 3, 0);
        assert_eq!(assign(&mut sets, second, 3, 0), first_record);
        assert_ne!(assign(&mut sets, second, 4, 0), first_record);
        assert_eq!(sets.assigned_style_record(first.node), Some(first_record));

        let pseudo = ComputedStyleTarget::new(StyleNodeID::element(1), 2);
        let pseudo_record = assign(&mut sets, pseudo, 3, 0);
        assert_eq!(pseudo_record, first_record);
        assert_eq!(sets.pseudo_style_record(pseudo.node, 2), Some(pseudo_record));
        assert_eq!(sets.assigned_pseudo_kinds(pseudo.node).collect::<Vec<_>>(), [2]);
        assert_eq!(
            sets.remove_pseudo_of(pseudo.node, super::super::pipeline::PseudoElementTarget::of_kind(2)),
            Some(pseudo_record)
        );
        assert_eq!(
            sets.remove_pseudo_of(pseudo.node, super::super::pipeline::PseudoElementTarget::of_kind(2)),
            None
        );
    }

    #[test]
    fn an_animation_overlay_composes_over_the_record_it_replaces_and_releases() {
        let mut sets = ComputedGroupSets::default();
        let node = StyleNodeID::element(1);
        let target = ComputedStyleTarget::new(node, u8::MAX);
        let record = assign(&mut sets, target, 0, 0);
        let animated_overlay = crate::css::animated_overlay::AnimatedOverlay::default();
        let defaults = crate::css::computed_values::DefaultGroups::default();

        let composed = compose(&mut sets, &defaults, target, &animated_overlay, 9);
        assert_eq!(composed.previous_style_record, record);
        assert!(composed.slot_allocated);
        assert!(composed.record_updated);
        assert!(composed.style_record.is_animation_overlay());
        assert_eq!(sets.assigned_style_record(node), Some(composed.style_record));
        assert_eq!(sets.underlying_style_record(composed.style_record), Some(record));

        let updated = compose(&mut sets, &defaults, target, &animated_overlay, 10);
        assert!(!updated.slot_allocated);
        assert!(updated.record_updated);
        assert_ne!(updated.style_record, composed.style_record);
        assert_eq!(updated.live_records, 1);

        let unchanged = compose(&mut sets, &defaults, target, &animated_overlay, 10);
        assert!(!unchanged.record_updated);
        assert_eq!(unchanged.style_record, updated.style_record);

        let released = compose(&mut sets, &defaults, target, &animated_overlay, 0);
        assert!(released.slot_released);
        assert_eq!(released.style_record, record);
        assert_eq!(released.live_records, 0);
        assert_eq!(sets.live_animation_overlay_records(), 0);
    }

    #[test]
    fn pinned_animation_overlay_survives_assignment_replacement() {
        let mut sets = ComputedGroupSets::default();
        let node = StyleNodeID::element(1);
        let target = ComputedStyleTarget::new(node, u8::MAX);
        let animated_overlay = crate::css::animated_overlay::AnimatedOverlay::default();
        let defaults = crate::css::computed_values::DefaultGroups::default();
        assign(&mut sets, target, 0, 0);
        let first = compose(&mut sets, &defaults, target, &animated_overlay, 1);

        sets.pin_style_record(first.style_record.raw());
        let second = compose(&mut sets, &defaults, target, &animated_overlay, 2);

        assert_ne!(first.style_record, second.style_record);
        assert!(sets.animation_overlay_slots_by_record.contains_key(&first.style_record));
        assert!(
            sets.animation_overlay_slots_by_record
                .contains_key(&second.style_record)
        );
        assert_eq!(sets.live_animation_overlay_records(), 1);

        sets.unpin_style_record(first.style_record.raw());
        assert!(!sets.animation_overlay_slots_by_record.contains_key(&first.style_record));
        sets.remove(node);
        assert_eq!(sets.live_animation_overlay_records(), 0);
    }

    #[test]
    fn released_animation_overlay_records_name_each_record_the_engine_stops_resolving() {
        let mut sets = ComputedGroupSets::default();
        let node = StyleNodeID::element(1);
        let target = ComputedStyleTarget::new(node, u8::MAX);
        let animated_overlay = crate::css::animated_overlay::AnimatedOverlay::default();
        let defaults = crate::css::computed_values::DefaultGroups::default();
        assign(&mut sets, target, 0, 0);
        let released = |sets: &mut ComputedGroupSets| {
            let released = sets.take_released_animation_overlay_records();
            assert!(!released.overflowed());
            released.records().collect::<Vec<_>>()
        };

        let first = compose(&mut sets, &defaults, target, &animated_overlay, 1);
        assert!(!sets.has_released_animation_overlay_records());

        let second = compose(&mut sets, &defaults, target, &animated_overlay, 2);
        assert_eq!(released(&mut sets), [first.style_record.raw()]);

        sets.pin_style_record(second.style_record.raw());
        let third = compose(&mut sets, &defaults, target, &animated_overlay, 3);
        assert_ne!(second.style_record, third.style_record);
        assert!(!sets.has_released_animation_overlay_records());
        sets.unpin_style_record(second.style_record.raw());
        assert_eq!(released(&mut sets), [second.style_record.raw()]);

        sets.remove(node);
        assert_eq!(released(&mut sets), [third.style_record.raw()]);
        assert!(!sets.has_released_animation_overlay_records());
    }

    #[test]
    fn released_animation_overlay_records_beyond_the_limit_keep_only_the_overflow() {
        let mut released = ReleasedAnimationOverlayRecords::default();
        for generation in 1..=ReleasedAnimationOverlayRecords::MAX_RECORDS as u64 {
            released.add(FinalStyleRecordID::animation_overlay(generation));
        }
        assert!(!released.overflowed());
        assert_eq!(released.records().count(), ReleasedAnimationOverlayRecords::MAX_RECORDS);

        released.add(FinalStyleRecordID::animation_overlay(
            ReleasedAnimationOverlayRecords::MAX_RECORDS as u64 + 1,
        ));
        assert!(released.overflowed());
        assert_eq!(released.records().count(), 0);
        assert!(!released.is_empty());
    }

    #[test]
    fn a_style_record_lease_keeps_a_replaced_animation_overlay_until_it_drops() {
        let mut sets = ComputedGroupSets::default();
        let node = StyleNodeID::element(1);
        let target = ComputedStyleTarget::new(node, u8::MAX);
        let animated_overlay = crate::css::animated_overlay::AnimatedOverlay::default();
        let defaults = crate::css::computed_values::DefaultGroups::default();
        assign(&mut sets, target, 0, 0);
        let first = compose(&mut sets, &defaults, target, &animated_overlay, 1);

        let lease = sets.lease_style_records();
        let second = compose(&mut sets, &defaults, target, &animated_overlay, 2);
        assert!(!second.slot_allocated);
        assert_ne!(first.style_record, second.style_record);
        assert_eq!(sets.retired_animation_overlay_records(), 1);
        sets.remove(node);
        assert_eq!(sets.retired_animation_overlay_records(), 2);

        drop(lease);
        assert!(sets.free_retired_animation_overlays());
        assert_eq!(sets.retired_animation_overlay_records(), 0);
        assert_eq!(sets.live_animation_overlay_records(), 0);
    }

    #[test]
    fn a_copy_of_the_sets_holds_no_lease_and_shares_the_leases_held() {
        let sets = ComputedGroupSets::default();
        let copy = sets.clone();
        assert!(!sets.style_records_are_leased());
        assert!(!copy.style_records_are_leased());
        let lease = copy.lease_style_records();
        assert!(sets.style_records_are_leased());
        assert!(copy.style_records_are_leased());
        drop(lease);
        assert!(!sets.style_records_are_leased());
    }

    #[test]
    fn a_selected_record_lease_declines_the_overlay_record_of_a_removed_element() {
        let mut sets = ComputedGroupSets::default();
        let node = StyleNodeID::element(1);
        let target = ComputedStyleTarget::new(node, u8::MAX);
        let animated_overlay = crate::css::animated_overlay::AnimatedOverlay::default();
        let defaults = crate::css::computed_values::DefaultGroups::default();
        let base = assign(&mut sets, target, 0, 0);
        let overlay = compose(&mut sets, &defaults, target, &animated_overlay, 1).style_record;
        // The row of the removed element pins its records until the rebuild of its parent frees the row.
        sets.pin_style_record(base.raw());
        sets.pin_style_record(overlay.raw());
        sets.remove(node);
        assert_eq!(sets.live_animation_overlay_records(), 0);
        assert_eq!(sets.retired_animation_overlay_records(), 0);
        assert!(names_an_uninstalled_record(overlay.raw()));
        assert!(!names_an_uninstalled_record(base.raw()));
        sets.unpin_style_record(overlay.raw());
        sets.unpin_style_record(base.raw());
    }

    #[test]
    fn a_hover_frame_lease_allows_records_let_go_of_before_it_to_be_reclaimed() {
        let mut sets = ComputedGroupSets::default();
        let kept = install(&mut sets, 1, 0);
        let discarded = install(&mut sets, 2, 0);
        sets.pin_style_record(kept.raw());
        let lease = sets.lease_hover_frame_style_records(Default::default());
        sets.unpin_style_record(kept.raw());
        sets.reclaim_unreachable();
        assert!(sets.final_style_record_is_live(kept.raw()));
        assert!(!sets.final_style_record_is_live(discarded.raw()));
        // A fork shares the lease, and references the records it shares, which keep their slots until it goes.
        let mut fork = sets.clone();
        drop(lease);
        fork.reclaim_unreachable();
        assert!(fork.final_style_record_is_live(kept.raw()));
        drop(fork);
        sets.reclaim_unreachable();
        assert!(!sets.final_style_record_is_live(kept.raw()));
    }

    #[test]
    fn a_hover_frame_lease_keeps_only_what_its_rows_name_once_many_records_are_let_go_of() {
        let mut sets = ComputedGroupSets::default();
        let named = install(&mut sets, 1, 0);
        sets.pin_style_record(named.raw());
        let mut rows = crate::cow_column::CowColumn::default();
        rows.grow_to(1);
        rows.set(
            0,
            crate::layout::node_data::PaintNode {
                style_record: named.raw(),
                ..Default::default()
            },
        );
        let lease = sets.lease_hover_frame_style_records(rows.publish());
        sets.unpin_style_record(named.raw());
        let unnamed: Vec<_> = (2..=MIN_RECORDS_RELEASED_BEFORE_READING_LEASES + 2)
            .map(|id| install(&mut sets, id as u64, 0))
            .collect();
        sets.reclaim_unreachable();
        assert!(sets.final_style_record_is_live(named.raw()));
        assert!(
            unnamed
                .iter()
                .all(|record| !sets.final_style_record_is_live(record.raw()))
        );
        drop(lease);
    }

    #[test]
    fn a_pinned_record_outlives_its_assignment_until_the_last_pin_releases() {
        let mut sets = ComputedGroupSets::default();
        let node = StyleNodeID::element(1);
        let record = assign(&mut sets, ComputedStyleTarget::new(node, u8::MAX), 0, 0);
        sets.pin_style_record(record.raw());
        sets.pin_style_record(record.raw());
        sets.remove(node);
        sets.reclaim_unreachable();
        assert!(sets.final_style_record_is_live(record.raw()));

        sets.unpin_style_record(record.raw());
        sets.reclaim_unreachable();
        assert!(sets.final_style_record_is_live(record.raw()));
        sets.unpin_style_record(record.raw());
        sets.reclaim_unreachable();
        assert!(!sets.final_style_record_is_live(record.raw()));
    }

    #[test]
    #[should_panic(expected = "installed style-record is not live")]
    fn a_freed_record_cannot_be_viewed() {
        let mut sets = ComputedGroupSets::default();
        let freed = install(&mut sets, 1, 0);
        sets.reclaim_unreachable();
        let replacement = install(&mut sets, 2, 0);
        assert_eq!(
            freed.installed_record().unwrap().slot(),
            replacement.installed_record().unwrap().slot()
        );
        assert_ne!(freed, replacement);
        let _ = sets.style_record_view(freed.raw());
    }

    #[test]
    fn a_record_naming_a_slot_of_a_fork_is_not_the_record_held_in_that_slot_here() {
        let mut sets = ComputedGroupSets::default();
        let shared = initial_record().with_test_dependency_flags(1);
        let mut fork = sets.clone();
        let in_fork = fork.install_record(&shared, || 0);

        let node = StyleNodeID::element(1);
        let held = assign(&mut sets, ComputedStyleTarget::new(node, u8::MAX), 0, 0);
        assert_eq!(held, in_fork, "both sides take the same free slot");
        assert!(!sets.holds_record(node, &shared));
    }

    #[test]
    fn an_installed_record_identity_names_its_slot_and_generation_alone() {
        let installed = InstalledRecordID::from_parts(41, InstalledRecordID::MAX_GENERATION);
        let record = FinalStyleRecordID::installed(installed);
        assert_eq!(record.installed_record(), Some(installed));
        assert!(!record.is_animation_overlay());
        assert!(record.raw() < 1 << 53);
        assert_eq!(FinalStyleRecordID::animation_overlay(3).installed_record(), None);
    }
}
