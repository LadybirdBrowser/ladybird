/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Dense identities for the shared computed-group payloads of live base styles.
//!
//! `ComputedValues` already factorizes its immutable payload into independently shared groups.
//! This catalog gives each shared payload a dense identity and interns their ordered tuple. Equal
//! handles therefore prove equal computed groups without comparing a complete `ComputedValues`
//! object. The converse is intentionally not claimed yet: separately allocated payloads with equal
//! values remain distinct until value interning lands. Custom-property environments and the fixed
//! pseudo-style and environment-dependency flags are retained as separate relations. Originating
//! elements use dense columns, while actual pseudo-element kinds use sparse assignments.
//! Reconstruction metadata retains cascade provenance and sparse inheritance-dependent inputs.
//! The complete tuple is interned as the base `StyleRecordID` published for each style target.

use std::ffi::c_void;
use std::hash::Hash;
use std::hash::Hasher;
use std::num::NonZeroU32;

use super::capacity::capacity_bytes;
use super::cascade::CascadeStateID;
use super::fast_hash::FastMap as HashMap;
use super::fast_hash::fast_hasher;
use super::intern_table::InternIdentity;
use super::intern_table::InternTable;
use super::memory::MemoryCategory;
use super::memory::MemoryController;
use super::memory::MemoryLease;
use super::tree::PseudoElementKind;
use super::tree::PseudoElementTarget;
use super::tree::StyleNodeID;
use crate::css::computed_values::computed_group_output_mask;
use crate::css::computed_values::release_group_payload;
use crate::css::computed_values::replay_style_group_identity;
use crate::css::computed_values::replaying_style_groups;
use crate::css::computed_values::retain_group_payload;
use crate::css::computed_values::retained_group_payload_bytes;
use crate::css::computed_values::style_group_payloads_equal;
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::StyleValueData;
use crate::css::style_value::retained_value_depends_on_color_scheme;
use crate::css::style_value::retained_value_depends_on_current_color;
use crate::css::style_value::retained_value_may_depend_on_font_metrics;
use crate::css::style_value::rust_style_value_retain;

// The high bit is a node-local production capability carried with publication and stripped before
// the semantic fixed metadata is interned or exposed through a style-record view.
const INHERITED_GROUP_SWAP_ELIGIBLE: u8 = 1 << 3;
const COMPUTED_VALUE_DEPENDENCY_FLAGS: u8 = INHERITED_GROUP_SWAP_ELIGIBLE - 1;

define_id! { pub struct ComputedGroupID(); }

impl InternIdentity for ComputedGroupID {
    fn index(self) -> usize {
        self.0 as usize
    }
}

define_id! { pub struct ComputedGroupSetID(); }

impl InternIdentity for ComputedGroupSetID {
    fn index(self) -> usize {
        self.0 as usize
    }
}

define_id! { pub struct InheritedGroupSetID(); }

impl InternIdentity for InheritedGroupSetID {
    fn index(self) -> usize {
        self.0 as usize
    }
}

define_id! { pub struct CustomPropertyEnvironmentID(); }

impl InternIdentity for CustomPropertyEnvironmentID {
    fn index(self) -> usize {
        self.0 as usize
    }
}

define_id! { pub struct ComputedFixedMetadataID(); }

impl InternIdentity for ComputedFixedMetadataID {
    fn index(self) -> usize {
        self.0 as usize
    }
}

define_id! { pub struct ComputedReconstructionMetadataID(); }

impl InternIdentity for ComputedReconstructionMetadataID {
    fn index(self) -> usize {
        self.0 as usize
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct StyleRecordID(NonZeroU32);

impl InternIdentity for StyleRecordID {
    fn index(self) -> usize {
        self.raw() as usize - 1
    }
}

impl StyleRecordID {
    #[must_use]
    pub fn raw(self) -> u32 {
        self.0.get()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct ComputedFixedMetadata {
    pseudo_element_styles: u64,
    dependency_flags: u8,
    counter_style_environment_identity: u64,
}

pub(crate) struct InheritanceDependentValue {
    pub property: u16,
    pub value: RetainedStyleValueData,
}

struct ComputedReconstructionMetadata {
    property_importance: Box<[u8]>,
    property_inheritance: Box<[u8]>,
    inheritance_dependent_values: Box<[InheritanceDependentValue]>,
    inheritance_dependent_value_view: Box<[super::bridge::FfiInheritanceDependentValue]>,
    raw_cascaded_font_size: Option<RetainedStyleValueData>,
}

pub(crate) struct StyleRecordView<'a> {
    pub payloads: &'a [*const c_void],
    pub base_payloads: &'a [*const c_void],
    pub property_importance: &'a [u8],
    pub property_inheritance: &'a [u8],
    pub inheritance_dependent_values: &'a [super::bridge::FfiInheritanceDependentValue],
    pub raw_cascaded_font_size: *const c_void,
    pub animated_properties: *const c_void,
    pub pseudo_element_styles: u64,
    pub counter_style_environment_identity: u64,
    pub animation_overlay_identity: u64,
    pub dependency_flags: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct StyleRecord {
    groups: ComputedGroupSetID,
    custom_properties: CustomPropertyEnvironmentID,
    fixed_metadata: ComputedFixedMetadataID,
    reconstruction_metadata: ComputedReconstructionMetadataID,
}

struct RetainedAnimatedProperties(*const c_void);

impl RetainedAnimatedProperties {
    fn new(pointer: *const c_void) -> Self {
        assert!(!pointer.is_null(), "style-record animated properties are null");
        unsafe { ladybird_animated_properties_ref(pointer) };
        Self(pointer)
    }

    fn pointer(&self) -> *const c_void {
        self.0
    }
}

impl Drop for RetainedAnimatedProperties {
    fn drop(&mut self) {
        unsafe { ladybird_animated_properties_unref(self.0) };
    }
}

#[cfg(not(test))]
unsafe extern "C" {
    fn ladybird_animated_properties_ref(values: *const c_void);
    fn ladybird_animated_properties_unref(values: *const c_void);
}

#[cfg(test)]
unsafe fn ladybird_animated_properties_ref(_: *const c_void) {}

#[cfg(test)]
unsafe fn ladybird_animated_properties_unref(_: *const c_void) {}

pub struct ComputedReconstructionMetadataInput<'a> {
    pub property_importance: &'a [u8],
    pub property_inheritance: &'a [u8],
    pub inheritance_dependent_properties: &'a [u16],
    pub inheritance_dependent_values: &'a [*const c_void],
    pub raw_cascaded_font_size: *const c_void,
}

pub struct ComputedMetadataInput<'a> {
    pub pseudo_element_styles: u64,
    pub dependency_flags: u8,
    pub counter_style_environment_identity: u64,
    pub animation_overlay_identity: u64,
    pub animated_properties: *const c_void,
    pub animation_overlay_payloads: &'a [*const c_void],
    pub reconstruction: ComputedReconstructionMetadataInput<'a>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct FinalStyleRecordID(u64);

impl FinalStyleRecordID {
    const ANIMATION_OVERLAY_TAG: u64 = 1 << 63;

    fn base(style_record: StyleRecordID) -> Self {
        Self(style_record.raw().into())
    }

    fn animation_overlay(generation: u64) -> Self {
        assert!(generation != 0 && generation < Self::ANIMATION_OVERLAY_TAG);
        Self(Self::ANIMATION_OVERLAY_TAG | generation)
    }

    #[must_use]
    pub fn raw(self) -> u64 {
        self.0
    }

    fn base_record(self) -> Option<StyleRecordID> {
        if self.0 & Self::ANIMATION_OVERLAY_TAG != 0 {
            return None;
        }
        let raw = u32::try_from(self.0).ok()?;
        Some(StyleRecordID(NonZeroU32::new(raw)?))
    }
}

struct AnimationOverlayRecord {
    // NB: No sampled value enters a permanent interning table. The current assignment owns one
    //     reference, while detached layout and stabilization baselines can pin an old generation.
    base_style_record: StyleRecordID,
    source_identity: u64,
    final_style_record: FinalStyleRecordID,
    animated_properties: RetainedAnimatedProperties,
    payloads: Box<[*const c_void]>,
    pin_count: u64,
    is_assigned: bool,
}

impl Drop for AnimationOverlayRecord {
    fn drop(&mut self) {
        for (index, &payload) in self.payloads.iter().enumerate() {
            release_group_payload(index, payload);
        }
    }
}

struct ComputedGroup {
    index: usize,
    payload: *const c_void,
}

struct ComputedGroupSet {
    identities: Box<[ComputedGroupID]>,
    payloads: Box<[*const c_void]>,
}

#[derive(Clone, Copy)]
struct PublishedComputedInputs {
    groups: ComputedGroupSetID,
    inherited_groups: InheritedGroupSetID,
    custom_properties: CustomPropertyEnvironmentID,
    fixed_metadata: ComputedFixedMetadataID,
    reconstruction_metadata: ComputedReconstructionMetadataID,
    style_record: StyleRecordID,
    animation_overlay_slot: Option<u32>,
    cascade_state: Option<(u64, CascadeStateID)>,
}

pub struct ComputedGroupPublication {
    pub previous_style_record_identity: Option<FinalStyleRecordID>,
    pub style_record_identity: FinalStyleRecordID,
    pub new_groups: usize,
    pub canonical_output_groups_reused: usize,
    pub new_group_set: bool,
    pub new_inherited_group_set: bool,
    pub new_custom_property_environment: bool,
    pub new_computed_fixed_metadata: bool,
    pub new_computed_reconstruction_metadata: bool,
    pub new_style_record: bool,
    pub node_handle_changed: bool,
    pub inherited_node_handle_changed: bool,
    pub custom_property_environment_node_handle_changed: bool,
    pub computed_fixed_metadata_node_handle_changed: bool,
    pub computed_reconstruction_metadata_node_handle_changed: bool,
    pub style_record_node_handle_changed: bool,
    pub animation_overlay_slot_allocated: bool,
    pub animation_overlay_slot_released: bool,
    pub animation_overlay_record_updated: bool,
    pub live_animation_overlay_records: usize,
    pub is_pseudo: bool,
}

#[derive(Clone, Copy)]
struct AnimationOverlayPublication {
    slot: Option<u32>,
    final_style_record: FinalStyleRecordID,
    slot_allocated: bool,
    slot_released: bool,
    record_updated: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ComputedStyleTarget {
    node: StyleNodeID,
    pseudo_kind: u8,
}

impl ComputedStyleTarget {
    pub fn new(node: StyleNodeID, pseudo_kind: u8) -> Self {
        Self { node, pseudo_kind }
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

    #[must_use]
    pub(super) fn pseudo_element_target(self) -> Option<PseudoElementTarget> {
        self.is_pseudo()
            .then(|| PseudoElementTarget::new(PseudoElementKind(u16::from(self.pseudo_kind))))
    }
}

pub struct ComputedGroupSets {
    groups: InternTable<ComputedGroupID, ComputedGroup>,
    sets: InternTable<ComputedGroupSetID, ComputedGroupSet>,
    column: Vec<Option<ComputedGroupSetID>>,
    inherited_sets: InternTable<InheritedGroupSetID, Box<[ComputedGroupID]>>,
    inherited_column: Vec<Option<InheritedGroupSetID>>,
    custom_property_environments: InternTable<CustomPropertyEnvironmentID, u64>,
    custom_property_environment_column: Vec<Option<CustomPropertyEnvironmentID>>,
    computed_fixed_metadata: InternTable<ComputedFixedMetadataID, ComputedFixedMetadata>,
    computed_fixed_metadata_column: Vec<Option<ComputedFixedMetadataID>>,
    computed_reconstruction_metadata: InternTable<ComputedReconstructionMetadataID, ComputedReconstructionMetadata>,
    computed_reconstruction_metadata_column: Vec<Option<ComputedReconstructionMetadataID>>,
    style_records: InternTable<StyleRecordID, StyleRecord>,
    style_record_column: Vec<Option<StyleRecordID>>,
    // Recyclable animation overlays are deliberately separate from the permanent base records
    // above. Dense element assignments and sparse pseudo assignments pin at most one slot each.
    animation_overlay_slots: Vec<Option<AnimationOverlayRecord>>,
    animation_overlay_slots_by_record: HashMap<FinalStyleRecordID, u32>,
    free_animation_overlay_slots: Vec<u32>,
    animation_overlay_column: Vec<Option<u32>>,
    inherited_group_swap_eligible_column: Vec<u8>,
    live_animation_overlay_assignments: usize,
    next_animation_overlay_generation: u64,
    cascade_state_column: Vec<Option<(u64, CascadeStateID)>>,
    pseudo_cascade_state_rows: HashMap<(StyleNodeID, u8), (u64, CascadeStateID)>,
    pseudo_retained_cascade_rows: HashMap<(StyleNodeID, u8), (u64, CascadeStateID)>,
    pseudo_assignments: HashMap<(StyleNodeID, u8), PublishedComputedInputs>,
    pending_cascade_states: HashMap<(StyleNodeID, u8), (u64, CascadeStateID)>,
    pseudo_kinds_by_node: HashMap<StyleNodeID, Vec<u8>>,
    group_set_nested_memory: MemoryLease,
    reconstruction_nested_memory: MemoryLease,
    animation_overlay_nested_memory: MemoryLease,
    pseudo_assignment_nested_memory: MemoryLease,
}

impl Default for ComputedGroupSets {
    fn default() -> Self {
        Self {
            groups: InternTable::default(),
            sets: InternTable::default(),
            column: Vec::new(),
            inherited_sets: InternTable::default(),
            inherited_column: Vec::new(),
            custom_property_environments: InternTable::default(),
            custom_property_environment_column: Vec::new(),
            computed_fixed_metadata: InternTable::default(),
            computed_fixed_metadata_column: Vec::new(),
            computed_reconstruction_metadata: InternTable::default(),
            computed_reconstruction_metadata_column: Vec::new(),
            style_records: InternTable::default(),
            style_record_column: Vec::new(),
            animation_overlay_slots: Vec::new(),
            animation_overlay_slots_by_record: HashMap::default(),
            free_animation_overlay_slots: Vec::new(),
            animation_overlay_column: Vec::new(),
            inherited_group_swap_eligible_column: Vec::new(),
            live_animation_overlay_assignments: 0,
            next_animation_overlay_generation: 0,
            cascade_state_column: Vec::new(),
            pseudo_cascade_state_rows: HashMap::default(),
            pseudo_retained_cascade_rows: HashMap::default(),
            pseudo_assignments: HashMap::default(),
            pending_cascade_states: HashMap::default(),
            pseudo_kinds_by_node: HashMap::default(),
            group_set_nested_memory: MemoryLease::new(MemoryCategory::ComputedGroupSet),
            reconstruction_nested_memory: MemoryLease::new(MemoryCategory::ComputedReconstructionMetadata),
            animation_overlay_nested_memory: MemoryLease::new(MemoryCategory::AnimationOverlayRecord),
            pseudo_assignment_nested_memory: MemoryLease::new(MemoryCategory::ComputedPseudoAssignment),
        }
    }
}

impl ComputedGroupSets {
    pub(super) fn assigned_style_record(&self, node: StyleNodeID) -> Option<FinalStyleRecordID> {
        let index = node.element_index()? as usize;
        let style_record = *self.style_record_column.get(index)?.as_ref()?;
        Some(self.final_style_record(style_record, self.animation_overlay_column[index]))
    }

    fn intern_group_set(&mut self, groups: Vec<ComputedGroupID>) -> (ComputedGroupSetID, bool) {
        let hash = content_hash(&groups);
        if let Some(identity) = self.sets.find(hash, |_identity, set| set.identities.as_ref() == groups) {
            return (identity, false);
        }
        let identity =
            ComputedGroupSetID(u32::try_from(self.sets.len()).expect("computed group-set identity space exhausted"));
        let payloads = groups
            .iter()
            .map(|identity| self.groups[*identity].payload)
            .collect::<Vec<_>>()
            .into_boxed_slice();
        let identities = groups.into_boxed_slice();
        self.group_set_nested_memory
            .grow_committed((size_of_val(identities.as_ref()) + size_of_val(payloads.as_ref())) as u64);
        self.sets
            .insert(hash, identity, ComputedGroupSet { identities, payloads });
        (identity, true)
    }

    fn intern_style_record(&mut self, record: StyleRecord) -> (StyleRecordID, bool) {
        let hash = content_hash(record);
        if let Some(identity) = self
            .style_records
            .find(hash, |_identity, candidate| *candidate == record)
        {
            return (identity, false);
        }
        let raw = u32::try_from(
            self.style_records
                .len()
                .checked_add(1)
                .expect("base style-record identity space exhausted"),
        )
        .expect("base style-record identity space exhausted");
        let identity = StyleRecordID(NonZeroU32::new(raw).expect("base style-record identities are nonzero"));
        self.style_records.insert(hash, identity, record);
        (identity, true)
    }

    /// Replace a fully inheriting element's static inherited groups with its flat-tree parent's
    /// groups and publish the resulting base style record without rebuilding computed values.
    pub(super) fn replace_static_inherited_groups(
        &mut self,
        node: StyleNodeID,
        parent: StyleNodeID,
        inherited_style_groups: u8,
    ) -> Option<(FinalStyleRecordID, FinalStyleRecordID)> {
        const STATIC_INHERITED_GROUPS: u8 = (1 << 0) | (1 << 1) | (1 << 3);
        const INHERITED_GROUP_COUNT: usize = 7;
        if inherited_style_groups == 0 || inherited_style_groups & !STATIC_INHERITED_GROUPS != 0 {
            return None;
        }
        let index = node.element_index()? as usize;
        let parent_index = parent.element_index()? as usize;
        if self
            .inherited_group_swap_eligible_column
            .get(index)
            .copied()
            .unwrap_or(0)
            == 0
            || self.animation_overlay_column.get(index).copied().flatten().is_some()
            || self
                .animation_overlay_column
                .get(parent_index)
                .copied()
                .flatten()
                .is_some()
            || self.assigned_pseudo_kinds(node).next().is_some()
        {
            return None;
        }

        let old_style_record = *self.style_record_column.get(index)?.as_ref()?;
        let old_record = *self.style_records.get_index(old_style_record.raw() as usize - 1)?;
        let old_group_set = self.sets.get_index(old_record.groups.0 as usize)?;
        let parent_inherited = *self.inherited_column.get(parent_index)?.as_ref()?;
        let parent_groups = self.inherited_sets.get_index(parent_inherited.0 as usize)?;
        if parent_groups.len() != INHERITED_GROUP_COUNT || old_group_set.identities.len() < INHERITED_GROUP_COUNT {
            return None;
        }

        let mut groups = old_group_set.identities.to_vec();
        groups[..INHERITED_GROUP_COUNT].copy_from_slice(parent_groups);
        let group_set = self.intern_group_set(groups).0;
        let new_record = StyleRecord {
            groups: group_set,
            custom_properties: old_record.custom_properties,
            fixed_metadata: old_record.fixed_metadata,
            reconstruction_metadata: old_record.reconstruction_metadata,
        };
        let new_style_record = self.intern_style_record(new_record).0;
        self.column[index] = Some(group_set);
        self.inherited_column[index] = Some(parent_inherited);
        self.style_record_column[index] = Some(new_style_record);
        Some((
            FinalStyleRecordID::base(old_style_record),
            FinalStyleRecordID::base(new_style_record),
        ))
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
        base_style_record: StyleRecordID,
        source_identity: u64,
        animated_properties: *const c_void,
        payloads: &[*const c_void],
    ) -> AnimationOverlayRecord {
        assert!(payloads.iter().all(|payload| !payload.is_null()));
        for (index, &payload) in payloads.iter().enumerate() {
            retain_group_payload(index, payload);
        }
        AnimationOverlayRecord {
            base_style_record,
            source_identity,
            final_style_record: self.next_animation_overlay_record(),
            animated_properties: RetainedAnimatedProperties::new(animated_properties),
            payloads: payloads.into(),
            pin_count: 0,
            is_assigned: true,
        }
    }

    fn allocate_animation_overlay(
        &mut self,
        base_style_record: StyleRecordID,
        source_identity: u64,
        animated_properties: *const c_void,
        payloads: &[*const c_void],
    ) -> (u32, FinalStyleRecordID, bool) {
        let record =
            self.make_animation_overlay_record(base_style_record, source_identity, animated_properties, payloads);
        self.animation_overlay_nested_memory
            .grow_committed(size_of_val(record.payloads.as_ref()) as u64);
        let final_style_record = record.final_style_record;
        let (slot, slot_allocated) = if let Some(slot) = self.free_animation_overlay_slots.pop() {
            self.animation_overlay_slots[slot as usize] = Some(record);
            (slot, false)
        } else {
            let slot =
                u32::try_from(self.animation_overlay_slots.len()).expect("animation-overlay slot space exhausted");
            self.animation_overlay_slots.push(Some(record));
            (slot, true)
        };
        self.animation_overlay_slots_by_record.insert(final_style_record, slot);
        self.live_animation_overlay_assignments += 1;
        (slot, final_style_record, slot_allocated)
    }

    fn reclaim_animation_overlay_slot(&mut self, slot: u32) {
        let record = self.animation_overlay_slots[slot as usize]
            .as_ref()
            .expect("animation-overlay slot is live");
        assert!(!record.is_assigned && record.pin_count == 0);
        let final_style_record = record.final_style_record;
        let payload_bytes = size_of_val(record.payloads.as_ref()) as u64;
        self.animation_overlay_slots_by_record.remove(&final_style_record);
        self.animation_overlay_slots[slot as usize] = None;
        self.free_animation_overlay_slots.push(slot);
        self.animation_overlay_nested_memory.shrink_committed(payload_bytes);
    }

    fn release_animation_overlay_assignment(&mut self, slot: u32) {
        let record = self
            .animation_overlay_slots
            .get_mut(slot as usize)
            .and_then(Option::as_mut)
            .expect("animation-overlay slot is live");
        assert!(record.is_assigned, "animation-overlay slot has an assignment");
        record.is_assigned = false;
        self.live_animation_overlay_assignments -= 1;
        if record.pin_count == 0 {
            self.reclaim_animation_overlay_slot(slot);
        }
    }

    fn update_animation_overlay(
        &mut self,
        current_slot: Option<u32>,
        base_style_record: StyleRecordID,
        source_identity: u64,
        animated_properties: *const c_void,
        payloads: &[*const c_void],
    ) -> AnimationOverlayPublication {
        if source_identity == 0 {
            if let Some(slot) = current_slot {
                self.release_animation_overlay_assignment(slot);
            }
            return AnimationOverlayPublication {
                slot: None,
                final_style_record: FinalStyleRecordID::base(base_style_record),
                slot_allocated: false,
                slot_released: current_slot.is_some(),
                record_updated: false,
            };
        }

        if let Some(slot) = current_slot {
            let current = self.animation_overlay_slots[slot as usize]
                .as_ref()
                .expect("animation-overlay slot is live");
            if current.base_style_record == base_style_record
                && current.source_identity == source_identity
                && current.payloads.as_ref() == payloads
            {
                return AnimationOverlayPublication {
                    slot: Some(slot),
                    final_style_record: current.final_style_record,
                    slot_allocated: false,
                    slot_released: false,
                    record_updated: false,
                };
            }
            if current.pin_count == 0 {
                let old_final_style_record = current.final_style_record;
                let old_payload_bytes = size_of_val(current.payloads.as_ref()) as u64;
                let record = self.make_animation_overlay_record(
                    base_style_record,
                    source_identity,
                    animated_properties,
                    payloads,
                );
                let new_payload_bytes = size_of_val(record.payloads.as_ref()) as u64;
                if new_payload_bytes >= old_payload_bytes {
                    self.animation_overlay_nested_memory
                        .grow_committed(new_payload_bytes - old_payload_bytes);
                } else {
                    self.animation_overlay_nested_memory
                        .shrink_committed(old_payload_bytes - new_payload_bytes);
                }
                let final_style_record = record.final_style_record;
                self.animation_overlay_slots_by_record.remove(&old_final_style_record);
                self.animation_overlay_slots[slot as usize] = Some(record);
                self.animation_overlay_slots_by_record.insert(final_style_record, slot);
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

        let (slot, final_style_record, slot_allocated) =
            self.allocate_animation_overlay(base_style_record, source_identity, animated_properties, payloads);
        AnimationOverlayPublication {
            slot: Some(slot),
            final_style_record,
            slot_allocated,
            slot_released: false,
            record_updated: true,
        }
    }

    fn final_style_record(
        &self,
        base_style_record: StyleRecordID,
        animation_overlay_slot: Option<u32>,
    ) -> FinalStyleRecordID {
        animation_overlay_slot.map_or_else(
            || FinalStyleRecordID::base(base_style_record),
            |slot| {
                self.animation_overlay_slots[slot as usize]
                    .as_ref()
                    .expect("animation-overlay slot is live")
                    .final_style_record
            },
        )
    }

    pub fn publish(
        &mut self,
        target: Option<ComputedStyleTarget>,
        payloads: &[*const c_void],
        inherited_group_count: usize,
        custom_property_environment: u64,
        metadata_input: ComputedMetadataInput<'_>,
    ) -> ComputedGroupPublication {
        let ComputedMetadataInput {
            pseudo_element_styles,
            dependency_flags,
            counter_style_environment_identity,
            animation_overlay_identity,
            animated_properties,
            animation_overlay_payloads,
            reconstruction: reconstruction_metadata,
        } = metadata_input;
        let inherited_group_swap_eligible = dependency_flags & INHERITED_GROUP_SWAP_ELIGIBLE != 0;
        let dependency_flags = dependency_flags & COMPUTED_VALUE_DEPENDENCY_FLAGS;
        assert!(inherited_group_count <= payloads.len());
        let previous_group_set = target.and_then(|target| {
            let ComputedStyleTarget { node, pseudo_kind } = target;
            if target.is_pseudo() {
                self.pseudo_assignments
                    .get(&(node, pseudo_kind))
                    .map(|inputs| inputs.groups)
            } else {
                node.element_index()
                    .and_then(|index| self.column.get(index as usize))
                    .copied()
                    .flatten()
            }
        });
        let mut new_groups = 0;
        let mut canonical_output_groups_reused = 0;
        let mut groups = Vec::with_capacity(payloads.len());
        let replaying_style_groups = replaying_style_groups();
        for (index, &payload) in payloads.iter().enumerate() {
            assert!(!payload.is_null(), "computed group payload is null");
            if replaying_style_groups {
                let raw_identity = replay_style_group_identity(payload).expect("replay group identity exceeds u32");
                let identity = ComputedGroupID(raw_identity);
                match self.groups.get_index(raw_identity as usize) {
                    Some(group) => {
                        assert_eq!(group.index, index);
                        assert_eq!(group.payload, payload);
                    }
                    None => {
                        assert_eq!(raw_identity as usize, self.groups.len());
                        let identity = ComputedGroupID(raw_identity);
                        self.groups.insert(
                            content_hash((index, payload as usize)),
                            identity,
                            ComputedGroup { index, payload },
                        );
                        self.group_set_nested_memory
                            .grow_committed(retained_group_payload_bytes(index, payload) as u64);
                        new_groups += 1;
                    }
                }
                groups.push(identity);
                continue;
            }
            let key = (index, payload as usize);
            let previous_identity = previous_group_set.and_then(|set| self.sets[set].identities.get(index).copied());
            let previous_equal_identity = previous_identity
                .filter(|identity| style_group_payloads_equal(index, payload, self.groups[*identity].payload));
            let identity = match previous_equal_identity {
                Some(identity) => {
                    if self.groups[identity].payload != payload {
                        canonical_output_groups_reused += 1;
                    }
                    identity
                }
                None => match self.groups.find(content_hash(key), |_identity, group| {
                    (group.index, group.payload as usize) == key
                }) {
                    Some(identity) => identity,
                    None => {
                        retain_group_payload(index, payload);
                        let identity = ComputedGroupID(
                            u32::try_from(self.groups.len()).expect("computed group identity space exhausted"),
                        );
                        self.groups
                            .insert(content_hash(key), identity, ComputedGroup { index, payload });
                        self.group_set_nested_memory
                            .grow_committed(retained_group_payload_bytes(index, payload) as u64);
                        new_groups += 1;
                        identity
                    }
                },
            };
            groups.push(identity);
        }

        let (identity, new_group_set) = self.intern_group_set(groups);

        let inherited_groups = &self.sets[identity].identities[..inherited_group_count];
        let inherited_hash = content_hash(inherited_groups);
        let existing_inherited = self
            .inherited_sets
            .find(inherited_hash, |_identity, groups| groups.as_ref() == inherited_groups);
        let (inherited_identity, new_inherited_group_set) = match existing_inherited {
            Some(identity) => (identity, false),
            None => {
                let identity = InheritedGroupSetID(
                    u32::try_from(self.inherited_sets.len()).expect("inherited group-set identity space exhausted"),
                );
                let inherited_groups: Box<[ComputedGroupID]> = inherited_groups.into();
                self.group_set_nested_memory
                    .grow_committed(size_of_val(inherited_groups.as_ref()) as u64);
                self.inherited_sets.insert(inherited_hash, identity, inherited_groups);
                (identity, true)
            }
        };

        let custom_property_environment_hash = content_hash(custom_property_environment);
        let (custom_property_environment_identity, new_custom_property_environment) = match self
            .custom_property_environments
            .find(custom_property_environment_hash, |_identity, candidate| {
                *candidate == custom_property_environment
            }) {
            Some(identity) => (identity, false),
            None => {
                let identity = CustomPropertyEnvironmentID(
                    u32::try_from(self.custom_property_environments.len())
                        .expect("custom-property environment identity space exhausted"),
                );
                self.custom_property_environments.insert(
                    custom_property_environment_hash,
                    identity,
                    custom_property_environment,
                );
                (identity, true)
            }
        };

        let metadata = ComputedFixedMetadata {
            pseudo_element_styles,
            dependency_flags,
            counter_style_environment_identity,
        };
        let (computed_fixed_metadata_identity, new_computed_fixed_metadata) = match self
            .computed_fixed_metadata
            .find(content_hash(metadata), |_identity, candidate| *candidate == metadata)
        {
            Some(identity) => (identity, false),
            None => {
                let identity = ComputedFixedMetadataID(
                    u32::try_from(self.computed_fixed_metadata.len())
                        .expect("computed fixed-metadata identity space exhausted"),
                );
                self.computed_fixed_metadata
                    .insert(content_hash(metadata), identity, metadata);
                (identity, true)
            }
        };

        assert_eq!(
            reconstruction_metadata.inheritance_dependent_properties.len(),
            reconstruction_metadata.inheritance_dependent_values.len()
        );
        let mut inheritance_dependent_values: Vec<_> = reconstruction_metadata
            .inheritance_dependent_properties
            .iter()
            .copied()
            .zip(reconstruction_metadata.inheritance_dependent_values.iter().copied())
            .collect();
        inheritance_dependent_values.sort_unstable_by_key(|&(property, _)| property);
        let reconstruction_hash = reconstruction_metadata_hash(
            reconstruction_metadata.property_importance,
            reconstruction_metadata.property_inheritance,
            &inheritance_dependent_values,
            reconstruction_metadata.raw_cascaded_font_size,
        );
        let existing_reconstruction =
            self.computed_reconstruction_metadata
                .find(reconstruction_hash, |_identity, candidate| {
                    reconstruction_metadata_matches(candidate, &reconstruction_metadata, &inheritance_dependent_values)
                });
        let (computed_reconstruction_metadata_identity, new_computed_reconstruction_metadata) =
            match existing_reconstruction {
                Some(identity) => (identity, false),
                None => {
                    let identity = ComputedReconstructionMetadataID(
                        u32::try_from(self.computed_reconstruction_metadata.len())
                            .expect("computed reconstruction-metadata identity space exhausted"),
                    );
                    let inheritance_dependent_values: Box<[InheritanceDependentValue]> = inheritance_dependent_values
                        .into_iter()
                        .map(|(property, value)| InheritanceDependentValue {
                            property,
                            value: retain_style_value(value),
                        })
                        .collect();
                    let inheritance_dependent_value_view: Box<[super::bridge::FfiInheritanceDependentValue]> =
                        inheritance_dependent_values
                            .iter()
                            .map(|entry| super::bridge::FfiInheritanceDependentValue {
                                property: entry.property,
                                value: entry.value.pointer().cast(),
                            })
                            .collect();
                    let raw_cascaded_font_size = (!reconstruction_metadata.raw_cascaded_font_size.is_null())
                        .then(|| retain_style_value(reconstruction_metadata.raw_cascaded_font_size));
                    let metadata = ComputedReconstructionMetadata {
                        property_importance: reconstruction_metadata.property_importance.into(),
                        property_inheritance: reconstruction_metadata.property_inheritance.into(),
                        inheritance_dependent_values,
                        inheritance_dependent_value_view,
                        raw_cascaded_font_size,
                    };
                    self.reconstruction_nested_memory.grow_committed(
                        (metadata.property_importance.len()
                            + metadata.property_inheritance.len()
                            + size_of_val(metadata.inheritance_dependent_values.as_ref())
                            + size_of_val(metadata.inheritance_dependent_value_view.as_ref()))
                            as u64,
                    );
                    self.computed_reconstruction_metadata
                        .insert(reconstruction_hash, identity, metadata);
                    (identity, true)
                }
            };

        let style_record = StyleRecord {
            groups: identity,
            custom_properties: custom_property_environment_identity,
            fixed_metadata: computed_fixed_metadata_identity,
            reconstruction_metadata: computed_reconstruction_metadata_identity,
        };
        let (style_record_identity, new_style_record) = self.intern_style_record(style_record);

        let is_pseudo = target.is_some_and(ComputedStyleTarget::is_pseudo);
        let (
            node_handle_changed,
            inherited_node_handle_changed,
            custom_property_environment_node_handle_changed,
            computed_fixed_metadata_node_handle_changed,
            computed_reconstruction_metadata_node_handle_changed,
            previous_style_record_identity,
            animation_overlay_publication,
        ) = if let Some(ComputedStyleTarget { node, pseudo_kind }) = target.filter(|target| target.is_pseudo()) {
            let key = (node, pseudo_kind);
            let previous = self.pseudo_assignments.get(&key).copied();
            let previous_style_record_identity = previous
                .map(|previous| self.final_style_record(previous.style_record, previous.animation_overlay_slot));
            let animation_overlay_publication = self.update_animation_overlay(
                previous.and_then(|previous| previous.animation_overlay_slot),
                style_record_identity,
                animation_overlay_identity,
                animated_properties,
                animation_overlay_payloads,
            );
            if previous.is_none() {
                let kinds = self.pseudo_kinds_by_node.entry(node).or_default();
                let capacity_before = kinds.capacity();
                kinds.push(pseudo_kind);
                self.pseudo_assignment_nested_memory
                    .grow_committed((kinds.capacity() - capacity_before) as u64);
            }
            self.pseudo_assignments.insert(
                key,
                PublishedComputedInputs {
                    groups: identity,
                    inherited_groups: inherited_identity,
                    custom_properties: custom_property_environment_identity,
                    fixed_metadata: computed_fixed_metadata_identity,
                    reconstruction_metadata: computed_reconstruction_metadata_identity,
                    style_record: style_record_identity,
                    animation_overlay_slot: animation_overlay_publication.slot,
                    cascade_state: previous.and_then(|previous| previous.cascade_state),
                },
            );
            (
                previous.is_none_or(|previous| previous.groups != identity),
                previous.is_none_or(|previous| previous.inherited_groups != inherited_identity),
                previous.is_none_or(|previous| previous.custom_properties != custom_property_environment_identity),
                previous.is_none_or(|previous| previous.fixed_metadata != computed_fixed_metadata_identity),
                previous.is_none_or(|previous| {
                    previous.reconstruction_metadata != computed_reconstruction_metadata_identity
                }),
                previous_style_record_identity,
                animation_overlay_publication,
            )
        } else if let Some(ComputedStyleTarget { node, .. }) = target {
            let index = node.element_index().expect("only elements publish computed groups") as usize;
            if self.column.len() <= index {
                self.column.resize(index + 1, None);
                self.inherited_column.resize(index + 1, None);
                self.custom_property_environment_column.resize(index + 1, None);
                self.computed_fixed_metadata_column.resize(index + 1, None);
                self.computed_reconstruction_metadata_column.resize(index + 1, None);
                self.style_record_column.resize(index + 1, None);
                self.animation_overlay_column.resize(index + 1, None);
                self.inherited_group_swap_eligible_column.resize(index + 1, 0);
                self.cascade_state_column.resize(index + 1, None);
            }
            let previous_style_record_identity = self.style_record_column[index]
                .map(|style_record| self.final_style_record(style_record, self.animation_overlay_column[index]));
            let animation_overlay_publication = self.update_animation_overlay(
                self.animation_overlay_column[index],
                style_record_identity,
                animation_overlay_identity,
                animated_properties,
                animation_overlay_payloads,
            );
            let changed = (
                self.column[index] != Some(identity),
                self.inherited_column[index] != Some(inherited_identity),
                self.custom_property_environment_column[index] != Some(custom_property_environment_identity),
                self.computed_fixed_metadata_column[index] != Some(computed_fixed_metadata_identity),
                self.computed_reconstruction_metadata_column[index] != Some(computed_reconstruction_metadata_identity),
                previous_style_record_identity,
                animation_overlay_publication,
            );
            self.column[index] = Some(identity);
            self.inherited_column[index] = Some(inherited_identity);
            self.custom_property_environment_column[index] = Some(custom_property_environment_identity);
            self.computed_fixed_metadata_column[index] = Some(computed_fixed_metadata_identity);
            self.computed_reconstruction_metadata_column[index] = Some(computed_reconstruction_metadata_identity);
            self.style_record_column[index] = Some(style_record_identity);
            self.animation_overlay_column[index] = animation_overlay_publication.slot;
            self.inherited_group_swap_eligible_column[index] = u8::from(inherited_group_swap_eligible);
            changed
        } else {
            assert_eq!(
                animation_overlay_identity, 0,
                "unassigned style records cannot own animation overlays"
            );
            (
                false,
                false,
                false,
                false,
                false,
                None,
                AnimationOverlayPublication {
                    slot: None,
                    final_style_record: FinalStyleRecordID::base(style_record_identity),
                    slot_allocated: false,
                    slot_released: false,
                    record_updated: false,
                },
            )
        };
        let final_style_record_identity = animation_overlay_publication.final_style_record;
        let style_record_node_handle_changed =
            target.is_some() && previous_style_record_identity != Some(final_style_record_identity);
        ComputedGroupPublication {
            previous_style_record_identity,
            style_record_identity: final_style_record_identity,
            new_groups,
            canonical_output_groups_reused,
            new_group_set,
            new_inherited_group_set,
            new_custom_property_environment,
            new_computed_fixed_metadata,
            new_computed_reconstruction_metadata,
            new_style_record,
            node_handle_changed,
            inherited_node_handle_changed,
            custom_property_environment_node_handle_changed,
            computed_fixed_metadata_node_handle_changed,
            computed_reconstruction_metadata_node_handle_changed,
            style_record_node_handle_changed,
            animation_overlay_slot_allocated: animation_overlay_publication.slot_allocated,
            animation_overlay_slot_released: animation_overlay_publication.slot_released,
            animation_overlay_record_updated: animation_overlay_publication.record_updated,
            live_animation_overlay_records: self.live_animation_overlay_assignments,
            is_pseudo,
        }
    }

    pub fn set_pending_cascade_state(&mut self, target: ComputedStyleTarget, state: (u64, CascadeStateID)) {
        self.pending_cascade_states
            .insert((target.node, target.pseudo_kind), state);
    }

    pub fn take_pending_cascade_state(&mut self, target: ComputedStyleTarget) -> Option<(u64, CascadeStateID)> {
        self.pending_cascade_states.remove(&(target.node, target.pseudo_kind))
    }

    #[must_use]
    pub fn cascade_state(&self, target: ComputedStyleTarget) -> Option<(u64, CascadeStateID)> {
        if target.is_pseudo() {
            return self
                .pseudo_cascade_state_rows
                .get(&(target.node, target.pseudo_kind))
                .copied();
        }
        target
            .node
            .element_index()
            .and_then(|index| self.cascade_state_column.get(index as usize).copied().flatten())
    }

    pub fn pseudo_retained_cascade_states(
        &self,
        node: StyleNodeID,
    ) -> impl Iterator<Item = (u8, (u64, CascadeStateID))> + '_ {
        self.pseudo_retained_cascade_rows
            .iter()
            .filter_map(move |(&(row_node, pseudo_kind), &state)| (row_node == node).then_some((pseudo_kind, state)))
    }

    #[must_use]
    pub fn pseudo_retained_cascade_state(&self, node: StyleNodeID, pseudo_kind: u8) -> Option<(u64, CascadeStateID)> {
        self.pseudo_retained_cascade_rows.get(&(node, pseudo_kind)).copied()
    }

    /// The pseudo-element kinds this node holds published computed styles for.
    pub fn assigned_pseudo_kinds(&self, node: StyleNodeID) -> impl Iterator<Item = u8> + '_ {
        self.pseudo_kinds_by_node
            .get(&node)
            .into_iter()
            .flat_map(|kinds| kinds.iter().copied())
    }

    #[cfg(test)]
    pub fn record_pseudo_kind_for_test(&mut self, node: StyleNodeID, pseudo_kind: u8) {
        let kinds = self.pseudo_kinds_by_node.entry(node).or_default();
        let capacity_before = kinds.capacity();
        kinds.push(pseudo_kind);
        self.pseudo_assignment_nested_memory
            .grow_committed((kinds.capacity() - capacity_before) as u64);
    }

    fn specified_value_dependency_mask(
        &self,
        target: ComputedStyleTarget,
        depends_on_input: impl Fn(&RetainedStyleValueData) -> bool,
    ) -> Option<u32> {
        let reconstruction_metadata = if target.is_pseudo() {
            self.pseudo_assignments
                .get(&(target.node, target.pseudo_kind))
                .map(|assignment| assignment.reconstruction_metadata)
        } else {
            target
                .node
                .element_index()
                .and_then(|index| self.computed_reconstruction_metadata_column.get(index as usize))
                .copied()
                .flatten()
        }?;
        let mask = self.computed_reconstruction_metadata[reconstruction_metadata.0 as usize]
            .inheritance_dependent_values
            .iter()
            .filter(|entry| depends_on_input(&entry.value))
            // Logical aliases retain their specified values but own no output payload. Their
            // resolved physical longhands are recorded separately and name the actual group.
            .filter_map(|entry| computed_group_output_mask(entry.property))
            .fold(0, |mask, groups| mask | groups);
        Some(mask)
    }

    fn specified_value_dependency_properties(
        &self,
        target: ComputedStyleTarget,
        depends_on_input: impl Fn(&RetainedStyleValueData) -> bool,
    ) -> Option<[u64; 6]> {
        let reconstruction_metadata = if target.is_pseudo() {
            self.pseudo_assignments
                .get(&(target.node, target.pseudo_kind))
                .map(|assignment| assignment.reconstruction_metadata)
        } else {
            target
                .node
                .element_index()
                .and_then(|index| self.computed_reconstruction_metadata_column.get(index as usize))
                .copied()
                .flatten()
        }?;
        let mut properties = [0u64; 6];
        for entry in
            &self.computed_reconstruction_metadata[reconstruction_metadata.0 as usize].inheritance_dependent_values
        {
            if !depends_on_input(&entry.value) {
                continue;
            }
            let index = usize::from(
                entry
                    .property
                    .checked_sub(crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID)?,
            );
            if index >= crate::css::property_metadata::NUMBER_OF_LONGHAND_PROPERTIES {
                return None;
            }
            properties[index / 64] |= 1 << (index % 64);
        }
        Some(properties)
    }

    /// The groups whose previous specified values actually read `currentColor` while computing.
    /// Missing reconstruction coverage stays typed so a color winner change can widen safely.
    #[must_use]
    pub fn current_color_dependency_mask(&self, target: ComputedStyleTarget) -> Option<u32> {
        self.specified_value_dependency_mask(target, retained_value_depends_on_current_color)
    }

    /// The longhands whose previous specified values actually read `currentColor`.
    #[must_use]
    pub fn current_color_dependency_properties(&self, target: ComputedStyleTarget) -> Option<[u64; 6]> {
        self.specified_value_dependency_properties(target, retained_value_depends_on_current_color)
    }

    /// The groups whose previous specified values read the effective color scheme.
    #[must_use]
    pub fn color_scheme_dependency_mask(&self, target: ComputedStyleTarget) -> Option<u32> {
        self.specified_value_dependency_mask(target, retained_value_depends_on_color_scheme)
    }

    /// The longhands whose previous specified values read the effective color scheme.
    #[must_use]
    pub fn color_scheme_dependency_properties(&self, target: ComputedStyleTarget) -> Option<[u64; 6]> {
        self.specified_value_dependency_properties(target, retained_value_depends_on_color_scheme)
    }

    /// The groups whose previous specified values may read font metrics.
    #[must_use]
    pub fn font_dependency_mask(&self, target: ComputedStyleTarget) -> Option<u32> {
        self.specified_value_dependency_mask(target, retained_value_may_depend_on_font_metrics)
    }

    /// The longhands whose previous specified values may read font metrics.
    #[must_use]
    pub fn font_dependency_properties(&self, target: ComputedStyleTarget) -> Option<[u64; 6]> {
        self.specified_value_dependency_properties(target, retained_value_may_depend_on_font_metrics)
    }

    /// Bind a published base style to the complete cascade state it consumed, returning the state
    /// used by the preceding publication for the same style target.
    pub fn bind_cascade_state(
        &mut self,
        target: ComputedStyleTarget,
        cascade_state: (u64, CascadeStateID),
    ) -> Option<(u64, CascadeStateID)> {
        if target.is_pseudo() {
            let assignment = self
                .pseudo_assignments
                .get_mut(&(target.node, target.pseudo_kind))
                .expect("a pseudo style must be published before its cascade state is bound");
            assignment.cascade_state = Some(cascade_state);
            return self
                .pseudo_cascade_state_rows
                .insert((target.node, target.pseudo_kind), cascade_state);
        }
        let index = target
            .node
            .element_index()
            .expect("only elements publish computed groups") as usize;
        debug_assert!(index < self.cascade_state_column.len());
        self.cascade_state_column[index].replace(cascade_state)
    }

    /// Forget the exact cascade state behind a style published through a path that did not run the
    /// cascade, so a later comparison cannot use the state behind an older publication.
    pub fn clear_cascade_state(&mut self, target: ComputedStyleTarget) {
        if target.is_pseudo() {
            self.pseudo_cascade_state_rows
                .remove(&(target.node, target.pseudo_kind));
            self.pseudo_retained_cascade_rows
                .remove(&(target.node, target.pseudo_kind));
            if let Some(assignment) = self.pseudo_assignments.get_mut(&(target.node, target.pseudo_kind)) {
                assignment.cascade_state = None;
            }
            return;
        }
        let Some(index) = target.node.element_index().map(|index| index as usize) else {
            return;
        };
        if let Some(slot) = self.cascade_state_column.get_mut(index) {
            *slot = None;
        }
    }

    pub fn remove(&mut self, node: StyleNodeID) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        if let Some(slot) = self.column.get_mut(index) {
            *slot = None;
        }
        if let Some(slot) = self.inherited_column.get_mut(index) {
            *slot = None;
        }
        if let Some(slot) = self.custom_property_environment_column.get_mut(index) {
            *slot = None;
        }
        if let Some(slot) = self.computed_fixed_metadata_column.get_mut(index) {
            *slot = None;
        }
        if let Some(slot) = self.computed_reconstruction_metadata_column.get_mut(index) {
            *slot = None;
        }
        if let Some(slot) = self.style_record_column.get_mut(index) {
            *slot = None;
        }
        if let Some(slot) = self.animation_overlay_column.get_mut(index).and_then(Option::take) {
            self.release_animation_overlay_assignment(slot);
        }
        if let Some(slot) = self.inherited_group_swap_eligible_column.get_mut(index) {
            *slot = 0;
        }
        if let Some(slot) = self.cascade_state_column.get_mut(index) {
            *slot = None;
        }
        self.pending_cascade_states
            .retain(|(pending_node, _), _| *pending_node != node);
        self.pseudo_cascade_state_rows
            .retain(|(row_node, _), _| *row_node != node);
        self.pseudo_retained_cascade_rows
            .retain(|(row_node, _), _| *row_node != node);
        if let Some(kinds) = self.pseudo_kinds_by_node.remove(&node) {
            self.pseudo_assignment_nested_memory
                .shrink_committed(kinds.capacity() as u64);
            for pseudo_kind in kinds {
                if let Some(removed) = self.pseudo_assignments.remove(&(node, pseudo_kind))
                    && let Some(slot) = removed.animation_overlay_slot
                {
                    self.release_animation_overlay_assignment(slot);
                }
            }
        }
    }

    pub fn remove_pseudo(&mut self, node: StyleNodeID, pseudo_kind: u8) -> Option<FinalStyleRecordID> {
        let removed = self.pseudo_assignments.remove(&(node, pseudo_kind))?;
        let final_style_record = self.final_style_record(removed.style_record, removed.animation_overlay_slot);
        if let Some(slot) = removed.animation_overlay_slot {
            self.release_animation_overlay_assignment(slot);
        }
        self.pending_cascade_states.remove(&(node, pseudo_kind));
        let mut remove_node_entry = false;
        if let Some(kinds) = self.pseudo_kinds_by_node.get_mut(&node) {
            kinds.retain(|kind| *kind != pseudo_kind);
            remove_node_entry = kinds.is_empty();
        }
        if remove_node_entry {
            let kinds = self
                .pseudo_kinds_by_node
                .remove(&node)
                .expect("pseudo-kind index entry is live");
            self.pseudo_assignment_nested_memory
                .shrink_committed(kinds.capacity() as u64);
        }
        Some(final_style_record)
    }

    pub fn observe_absent_pseudo_cascade_state(&mut self, target: ComputedStyleTarget, state: (u64, CascadeStateID)) {
        debug_assert!(target.is_pseudo());
        self.pseudo_cascade_state_rows
            .insert((target.node, target.pseudo_kind), state);
    }

    pub fn observe_pseudo_retained_cascade_state(
        &mut self,
        target: ComputedStyleTarget,
        state: Option<(u64, CascadeStateID)>,
    ) {
        debug_assert!(target.is_pseudo());
        if let Some(state) = state {
            self.pseudo_retained_cascade_rows
                .insert((target.node, target.pseudo_kind), state);
        } else {
            self.pseudo_retained_cascade_rows
                .remove(&(target.node, target.pseudo_kind));
        }
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.groups,
                self.sets,
                self.column,
                self.inherited_sets,
                self.inherited_column,
            ];
            cached [self.group_set_nested_memory.bytes()];
            nested [];
            skip [];
        }
    }

    #[must_use]
    pub fn custom_property_environment_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.custom_property_environments, self.custom_property_environment_column];
            cached [];
            nested [];
            skip [];
        }
    }

    #[must_use]
    pub fn computed_fixed_metadata_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.computed_fixed_metadata,
                self.computed_fixed_metadata_column,
            ];
            cached [];
            nested [];
            skip [];
        }
    }

    #[must_use]
    pub fn computed_reconstruction_metadata_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.computed_reconstruction_metadata,
                self.computed_reconstruction_metadata_column,
            ];
            cached [self.reconstruction_nested_memory.bytes()];
            nested [];
            skip [];
        }
    }

    #[must_use]
    pub fn style_record_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.style_records,
                self.style_record_column,
                self.inherited_group_swap_eligible_column,
                self.cascade_state_column,
                self.pending_cascade_states,
            ];
            cached [];
            nested [];
            skip [];
        }
    }

    #[must_use]
    pub fn animation_overlay_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.animation_overlay_slots,
                self.animation_overlay_slots_by_record,
                self.free_animation_overlay_slots,
                self.animation_overlay_column,
            ];
            cached [self.animation_overlay_nested_memory.bytes()];
            nested [];
            skip [];
        }
    }

    #[must_use]
    pub fn live_animation_overlay_records(&self) -> usize {
        self.live_animation_overlay_assignments
    }

    pub fn style_record_payloads(&self, raw_style_record: u64) -> Option<&[*const c_void]> {
        if raw_style_record & FinalStyleRecordID::ANIMATION_OVERLAY_TAG != 0 {
            let style_record = FinalStyleRecordID(raw_style_record);
            let slot = *self.animation_overlay_slots_by_record.get(&style_record)?;
            let record = self.animation_overlay_slots[slot as usize].as_ref()?;
            return (!record.payloads.is_empty()).then_some(record.payloads.as_ref());
        }
        let raw_style_record = u32::try_from(raw_style_record).ok()?;
        let record_index = raw_style_record.checked_sub(1)? as usize;
        let record = self.style_records.get_index(record_index)?;
        Some(&self.sets[record.groups].payloads)
    }

    #[cfg(feature = "style-recording")]
    pub(crate) fn recording_group_identities(&self, raw_style_record: u64) -> Option<Vec<u32>> {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        let base_style_record = match final_style_record.base_record() {
            Some(style_record) => style_record,
            None => {
                let slot = *self.animation_overlay_slots_by_record.get(&final_style_record)?;
                self.animation_overlay_slots[slot as usize].as_ref()?.base_style_record
            }
        };
        let record = self.style_records.get_index(base_style_record.raw() as usize - 1)?;
        Some(
            self.sets[record.groups]
                .identities
                .iter()
                .map(|identity| identity.0)
                .collect(),
        )
    }

    #[cfg(feature = "style-recording")]
    pub(crate) fn recording_group_retained_bytes(&self, raw_style_record: u64) -> Option<Vec<u64>> {
        let identities = self.recording_group_identities(raw_style_record)?;
        Some(
            identities
                .into_iter()
                .map(|identity| {
                    let group = &self.groups[identity as usize];
                    retained_group_payload_bytes(group.index, group.payload) as u64
                })
                .collect(),
        )
    }

    pub(crate) fn style_record_view(&self, raw_style_record: u64) -> Option<StyleRecordView<'_>> {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        let (base_style_record, payloads, animation_overlay_identity, animated_properties) =
            if let Some(style_record) = final_style_record.base_record() {
                let record = self.style_records.get_index(style_record.raw() as usize - 1)?;
                (
                    style_record,
                    self.sets[record.groups].payloads.as_ref(),
                    0,
                    std::ptr::null(),
                )
            } else {
                let slot = *self.animation_overlay_slots_by_record.get(&final_style_record)?;
                let overlay = self.animation_overlay_slots[slot as usize].as_ref()?;
                (
                    overlay.base_style_record,
                    overlay.payloads.as_ref(),
                    overlay.source_identity,
                    overlay.animated_properties.pointer(),
                )
            };
        let record = self.style_records.get_index(base_style_record.raw() as usize - 1)?;
        let base_payloads = self.sets[record.groups].payloads.as_ref();
        let fixed_metadata = self
            .computed_fixed_metadata
            .get_index(record.fixed_metadata.0 as usize)?;
        let reconstruction_metadata = self
            .computed_reconstruction_metadata
            .get_index(record.reconstruction_metadata.0 as usize)?;
        Some(StyleRecordView {
            payloads,
            base_payloads,
            property_importance: &reconstruction_metadata.property_importance,
            property_inheritance: &reconstruction_metadata.property_inheritance,
            inheritance_dependent_values: &reconstruction_metadata.inheritance_dependent_value_view,
            raw_cascaded_font_size: reconstruction_metadata
                .raw_cascaded_font_size
                .as_ref()
                .map_or(std::ptr::null(), |value| value.pointer().cast()),
            animated_properties,
            pseudo_element_styles: fixed_metadata.pseudo_element_styles,
            counter_style_environment_identity: fixed_metadata.counter_style_environment_identity,
            animation_overlay_identity,
            dependency_flags: fixed_metadata.dependency_flags,
        })
    }

    pub fn pin_style_record(&mut self, raw_style_record: u64) {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        if final_style_record.base_record().is_some() {
            return;
        }
        let slot = *self
            .animation_overlay_slots_by_record
            .get(&final_style_record)
            .expect("animation-overlay record is live");
        let record = self.animation_overlay_slots[slot as usize]
            .as_mut()
            .expect("animation-overlay slot is live");
        record.pin_count = record
            .pin_count
            .checked_add(1)
            .expect("animation-overlay pin count overflow");
    }

    pub fn unpin_style_record(&mut self, raw_style_record: u64) {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        if final_style_record.base_record().is_some() {
            return;
        }
        let slot = *self
            .animation_overlay_slots_by_record
            .get(&final_style_record)
            .expect("animation-overlay record is live");
        let record = self.animation_overlay_slots[slot as usize]
            .as_mut()
            .expect("animation-overlay slot is live");
        record.pin_count = record
            .pin_count
            .checked_sub(1)
            .expect("animation-overlay record is pinned");
        if !record.is_assigned && record.pin_count == 0 {
            self.reclaim_animation_overlay_slot(slot);
        }
    }

    #[must_use]
    pub fn pseudo_assignment_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.pseudo_assignments,
                self.pseudo_kinds_by_node,
                self.pseudo_cascade_state_rows,
                self.pseudo_retained_cascade_rows,
            ];
            cached [self.pseudo_assignment_nested_memory.bytes()];
            nested [];
            skip [];
        }
    }

    pub fn settle_nested_memory(&mut self, memory: &mut MemoryController) {
        self.group_set_nested_memory.settle_committed(memory);
        self.reconstruction_nested_memory.settle_committed(memory);
        self.animation_overlay_nested_memory.settle_committed(memory);
        self.pseudo_assignment_nested_memory.settle_committed(memory);
    }

    #[must_use]
    pub fn group_set_header_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [self.capacity_bytes() - self.group_set_nested_memory.bytes()];
            nested [];
            skip [];
        }
    }

    #[must_use]
    pub fn reconstruction_header_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [
                self.computed_reconstruction_metadata_capacity_bytes() - self.reconstruction_nested_memory.bytes(),
            ];
            nested [];
            skip [];
        }
    }

    #[must_use]
    pub fn animation_overlay_header_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [self.animation_overlay_capacity_bytes() - self.animation_overlay_nested_memory.bytes()];
            nested [];
            skip [];
        }
    }

    #[must_use]
    pub fn pseudo_assignment_header_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [self.pseudo_assignment_capacity_bytes() - self.pseudo_assignment_nested_memory.bytes()];
            nested [];
            skip [];
        }
    }
}

impl Drop for ComputedGroupSets {
    fn drop(&mut self) {
        for group in &self.groups {
            release_group_payload(group.index, group.payload);
        }
    }
}

fn content_hash(content: impl Hash) -> u64 {
    let mut hasher = fast_hasher();
    content.hash(&mut hasher);
    hasher.finish()
}

fn reconstruction_metadata_hash(
    property_importance: &[u8],
    property_inheritance: &[u8],
    inheritance_dependent_values: &[(u16, *const c_void)],
    raw_cascaded_font_size: *const c_void,
) -> u64 {
    let mut hasher = fast_hasher();
    property_importance.hash(&mut hasher);
    property_inheritance.hash(&mut hasher);
    for &(property, value) in inheritance_dependent_values {
        property.hash(&mut hasher);
        (value as usize).hash(&mut hasher);
    }
    (raw_cascaded_font_size as usize).hash(&mut hasher);
    hasher.finish()
}

fn reconstruction_metadata_matches(
    retained: &ComputedReconstructionMetadata,
    input: &ComputedReconstructionMetadataInput<'_>,
    inheritance_dependent_values: &[(u16, *const c_void)],
) -> bool {
    retained.property_importance.as_ref() == input.property_importance
        && retained.property_inheritance.as_ref() == input.property_inheritance
        && retained.inheritance_dependent_values.len() == inheritance_dependent_values.len()
        && retained
            .inheritance_dependent_values
            .iter()
            .zip(inheritance_dependent_values)
            .all(|(retained, &(property, value))| {
                retained.property == property && retained.value.pointer().cast() == value
            })
        && retained
            .raw_cascaded_font_size
            .as_ref()
            .map_or(std::ptr::null(), |value| value.pointer())
            .cast::<c_void>()
            == input.raw_cascaded_font_size
}

fn retain_style_value(value: *const c_void) -> RetainedStyleValueData {
    assert!(
        !value.is_null(),
        "computed reconstruction metadata contains a null style value"
    );
    let retained = unsafe { rust_style_value_retain(value.cast::<StyleValueData>()) };
    unsafe { RetainedStyleValueData::from_retained_pointer(retained) }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn metadata<'a>(
        pseudo_element_styles: u64,
        dependency_flags: u8,
        counter_style_environment_identity: u64,
        property_importance: &'a [u8],
        property_inheritance: &'a [u8],
    ) -> ComputedMetadataInput<'a> {
        ComputedMetadataInput {
            pseudo_element_styles,
            dependency_flags,
            counter_style_environment_identity,
            animation_overlay_identity: 0,
            animated_properties: std::ptr::NonNull::<c_void>::dangling().as_ptr(),
            animation_overlay_payloads: &[],
            reconstruction: ComputedReconstructionMetadataInput {
                property_importance,
                property_inheritance,
                inheritance_dependent_properties: &[],
                inheritance_dependent_values: &[],
                raw_cascaded_font_size: std::ptr::null(),
            },
        }
    }

    #[test]
    fn empty_group_sets_share_one_identity_and_publish_node_handles() {
        let mut sets = ComputedGroupSets::default();
        let first_target = ComputedStyleTarget::new(StyleNodeID::from_raw(1).unwrap(), u8::MAX);
        let second_target = ComputedStyleTarget::new(StyleNodeID::from_raw(65).unwrap(), u8::MAX);
        let first = sets.publish(Some(first_target), &[], 0, 17, metadata(3, 1, 5, &[1], &[2]));
        let second = sets.publish(Some(second_target), &[], 0, 17, metadata(3, 1, 5, &[1], &[2]));
        let unchanged = sets.publish(Some(first_target), &[], 0, 17, metadata(3, 1, 5, &[1], &[2]));

        assert_eq!(sets.bind_cascade_state(first_target, (1, CascadeStateID(3))), None);
        assert_eq!(
            sets.bind_cascade_state(first_target, (1, CascadeStateID(4))),
            Some((1, CascadeStateID(3)))
        );
        assert_eq!(sets.cascade_state(first_target), Some((1, CascadeStateID(4))));
        sets.clear_cascade_state(first_target);
        assert_eq!(sets.cascade_state(first_target), None);

        assert!(first.new_group_set);
        assert!(!first.is_pseudo);
        assert!(!second.new_group_set);
        assert!(first.node_handle_changed);
        assert!(second.node_handle_changed);
        assert!(!unchanged.node_handle_changed);
        assert!(!unchanged.inherited_node_handle_changed);
        assert!(!unchanged.custom_property_environment_node_handle_changed);
        assert!(!unchanged.computed_fixed_metadata_node_handle_changed);
        assert!(!unchanged.computed_reconstruction_metadata_node_handle_changed);
        assert!(first.new_custom_property_environment);
        assert!(!second.new_custom_property_environment);
        assert!(first.new_computed_fixed_metadata);
        assert!(!second.new_computed_fixed_metadata);
        assert!(first.new_computed_reconstruction_metadata);
        assert!(!second.new_computed_reconstruction_metadata);
        assert!(first.new_style_record);
        assert_eq!(first.previous_style_record_identity, None);
        assert!(!second.new_style_record);
        assert_eq!(second.previous_style_record_identity, None);
        assert!(!unchanged.new_style_record);
        assert_eq!(
            unchanged.previous_style_record_identity,
            Some(first.style_record_identity)
        );
        assert!(first.style_record_node_handle_changed);
        assert!(second.style_record_node_handle_changed);
        assert!(!unchanged.style_record_node_handle_changed);
        assert_eq!(first.style_record_identity, second.style_record_identity);
        assert_ne!(first.style_record_identity.raw(), 0);
        assert_eq!(first.new_groups + second.new_groups + unchanged.new_groups, 0);

        let changed_environment = sets.publish(Some(first_target), &[], 0, 18, metadata(3, 1, 5, &[1], &[2]));
        assert!(changed_environment.new_custom_property_environment);
        assert!(changed_environment.custom_property_environment_node_handle_changed);
        assert!(changed_environment.new_style_record);
        assert_eq!(
            changed_environment.previous_style_record_identity,
            Some(first.style_record_identity)
        );
        assert!(changed_environment.style_record_node_handle_changed);
        assert!(!changed_environment.node_handle_changed);

        let changed_metadata = sets.publish(Some(first_target), &[], 0, 18, metadata(7, 1, 5, &[1], &[2]));
        assert!(changed_metadata.new_computed_fixed_metadata);
        assert!(changed_metadata.computed_fixed_metadata_node_handle_changed);
        assert!(changed_metadata.new_style_record);
        assert!(changed_metadata.style_record_node_handle_changed);
        assert!(!changed_metadata.node_handle_changed);

        let changed_observer_dependency = sets.publish(Some(first_target), &[], 0, 18, metadata(7, 1, 6, &[1], &[2]));
        assert!(changed_observer_dependency.new_computed_fixed_metadata);
        assert!(changed_observer_dependency.computed_fixed_metadata_node_handle_changed);
        assert!(changed_observer_dependency.style_record_node_handle_changed);

        let mut animated_metadata = metadata(7, 1, 6, &[1], &[2]);
        animated_metadata.animation_overlay_identity = 9;
        let changed_animation_overlay = sets.publish(Some(first_target), &[], 0, 18, animated_metadata);
        assert!(!changed_animation_overlay.new_computed_fixed_metadata);
        assert!(!changed_animation_overlay.computed_fixed_metadata_node_handle_changed);
        assert!(!changed_animation_overlay.new_style_record);
        assert!(changed_animation_overlay.animation_overlay_slot_allocated);
        assert!(changed_animation_overlay.animation_overlay_record_updated);
        assert!(changed_animation_overlay.style_record_node_handle_changed);
        assert!(!changed_animation_overlay.node_handle_changed);
        let mut updated_animated_metadata = metadata(7, 1, 6, &[1], &[2]);
        updated_animated_metadata.animation_overlay_identity = 10;
        let updated_animation_overlay = sets.publish(Some(first_target), &[], 0, 18, updated_animated_metadata);
        assert!(!updated_animation_overlay.new_computed_fixed_metadata);
        assert!(!updated_animation_overlay.new_style_record);
        assert!(!updated_animation_overlay.animation_overlay_slot_allocated);
        assert!(updated_animation_overlay.animation_overlay_record_updated);
        assert_ne!(
            updated_animation_overlay.style_record_identity,
            changed_animation_overlay.style_record_identity
        );
        assert_eq!(updated_animation_overlay.live_animation_overlay_records, 1);
        assert_eq!(sets.live_animation_overlay_records(), 1);

        let released_animation_overlay = sets.publish(Some(first_target), &[], 0, 18, metadata(7, 1, 6, &[1], &[2]));
        assert!(released_animation_overlay.animation_overlay_slot_released);
        assert_eq!(released_animation_overlay.live_animation_overlay_records, 0);
        assert_eq!(
            released_animation_overlay.style_record_identity,
            changed_observer_dependency.style_record_identity
        );
        assert_eq!(sets.live_animation_overlay_records(), 0);

        let mut reused_animated_metadata = metadata(7, 1, 6, &[1], &[2]);
        reused_animated_metadata.animation_overlay_identity = 11;
        let reused_animation_overlay = sets.publish(Some(first_target), &[], 0, 18, reused_animated_metadata);
        assert!(!reused_animation_overlay.animation_overlay_slot_allocated);
        assert_eq!(reused_animation_overlay.live_animation_overlay_records, 1);

        let changed_reconstruction = sets.publish(Some(first_target), &[], 0, 18, metadata(7, 1, 6, &[3], &[2]));
        assert!(changed_reconstruction.new_computed_reconstruction_metadata);
        assert!(changed_reconstruction.computed_reconstruction_metadata_node_handle_changed);
        assert!(changed_reconstruction.new_style_record);
        assert!(changed_reconstruction.style_record_node_handle_changed);
        assert!(!changed_reconstruction.node_handle_changed);

        let pseudo_target = ComputedStyleTarget::new(StyleNodeID::from_raw(1).unwrap(), 2);
        let pseudo = sets.publish(Some(pseudo_target), &[], 0, 18, metadata(7, 1, 6, &[3], &[2]));
        let unchanged_pseudo = sets.publish(Some(pseudo_target), &[], 0, 18, metadata(7, 1, 6, &[3], &[2]));
        assert!(pseudo.is_pseudo);
        assert_eq!(pseudo.previous_style_record_identity, None);
        assert!(pseudo.node_handle_changed);
        assert!(!pseudo.new_style_record);
        assert!(pseudo.style_record_node_handle_changed);
        assert!(!unchanged_pseudo.node_handle_changed);
        assert!(!unchanged_pseudo.style_record_node_handle_changed);
        assert_eq!(
            unchanged_pseudo.previous_style_record_identity,
            Some(pseudo.style_record_identity)
        );
        sets.remove(StyleNodeID::from_raw(1).unwrap());
        let republished_pseudo = sets.publish(Some(pseudo_target), &[], 0, 18, metadata(7, 1, 6, &[3], &[2]));
        assert!(republished_pseudo.node_handle_changed);
        assert_eq!(
            sets.remove_pseudo(StyleNodeID::from_raw(1).unwrap(), 2),
            Some(republished_pseudo.style_record_identity)
        );
        assert_eq!(sets.remove_pseudo(StyleNodeID::from_raw(1).unwrap(), 2), None);
    }

    #[test]
    fn pinned_animation_overlay_survives_assignment_replacement() {
        let mut sets = ComputedGroupSets::default();
        let node = StyleNodeID::from_raw(1).unwrap();
        let target = ComputedStyleTarget::new(node, u8::MAX);
        let mut first_metadata = metadata(0, 0, 0, &[], &[]);
        first_metadata.animation_overlay_identity = 1;
        let first = sets.publish(Some(target), &[], 0, 0, first_metadata);

        sets.pin_style_record(first.style_record_identity.raw());
        let mut second_metadata = metadata(0, 0, 0, &[], &[]);
        second_metadata.animation_overlay_identity = 2;
        let second = sets.publish(Some(target), &[], 0, 0, second_metadata);

        assert_ne!(first.style_record_identity, second.style_record_identity);
        assert!(
            sets.animation_overlay_slots_by_record
                .contains_key(&first.style_record_identity)
        );
        assert!(
            sets.animation_overlay_slots_by_record
                .contains_key(&second.style_record_identity)
        );
        assert_eq!(sets.live_animation_overlay_records(), 1);

        sets.unpin_style_record(first.style_record_identity.raw());
        assert!(
            !sets
                .animation_overlay_slots_by_record
                .contains_key(&first.style_record_identity)
        );
        sets.remove(node);
        assert_eq!(sets.live_animation_overlay_records(), 0);
    }

    #[test]
    fn computed_group_set_capacity_includes_hash_table_control_bytes() {
        let mut sets = ComputedGroupSets::default();
        sets.groups.reserve(1);
        sets.sets.reserve(1);
        sets.inherited_sets.reserve(1);
        sets.custom_property_environments.reserve(1);
        sets.computed_fixed_metadata.reserve(1);
        sets.computed_reconstruction_metadata.reserve(1);
        sets.style_records.reserve(1);
        sets.pending_cascade_states.reserve(1);
        sets.animation_overlay_slots_by_record.reserve(1);
        sets.pseudo_assignments.reserve(1);
        sets.pseudo_kinds_by_node.reserve(1);
        sets.pseudo_cascade_state_rows.reserve(1);
        sets.pseudo_retained_cascade_rows.reserve(1);

        let accounted = sets.capacity_bytes()
            + sets.custom_property_environment_capacity_bytes()
            + sets.computed_fixed_metadata_capacity_bytes()
            + sets.computed_reconstruction_metadata_capacity_bytes()
            + sets.style_record_capacity_bytes()
            + sets.animation_overlay_capacity_bytes()
            + sets.pseudo_assignment_capacity_bytes();
        let expected = sets.groups.capacity_bytes() as usize
            + sets.sets.capacity_bytes() as usize
            + sets.inherited_sets.capacity_bytes() as usize
            + sets.custom_property_environments.capacity_bytes() as usize
            + sets.computed_fixed_metadata.capacity_bytes() as usize
            + sets.computed_reconstruction_metadata.capacity_bytes() as usize
            + sets.style_records.capacity_bytes() as usize
            + sets.pending_cascade_states.capacity()
                * (size_of::<(StyleNodeID, u8)>() + size_of::<(u64, CascadeStateID)>() + 1)
            + sets.animation_overlay_slots_by_record.capacity()
                * (size_of::<FinalStyleRecordID>() + size_of::<u32>() + 1)
            + sets.pseudo_assignments.capacity()
                * (size_of::<(StyleNodeID, u8)>() + size_of::<PublishedComputedInputs>() + 1)
            + sets.pseudo_kinds_by_node.capacity() * (size_of::<StyleNodeID>() + size_of::<Vec<u8>>() + 1)
            + sets.pseudo_cascade_state_rows.capacity()
                * (size_of::<(StyleNodeID, u8)>() + size_of::<(u64, CascadeStateID)>() + 1)
            + sets.pseudo_retained_cascade_rows.capacity()
                * (size_of::<(StyleNodeID, u8)>() + size_of::<(u64, CascadeStateID)>() + 1);

        assert_eq!(accounted, expected as u64);
    }
}
