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
//! elements use dense columns, while actual pseudo-element kinds use sparse assignments. The
//! complete tuple is interned as the base `StyleRecordID` published for each style target.

use std::ffi::c_void;
use std::hash::Hash;
use std::hash::Hasher;
use std::num::NonZeroU32;

use super::capacity::capacity_bytes;
use super::cascade::CascadeStateID;
use super::column::BitColumn;
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
use crate::css::computed_longhand_table::ComputedLonghandTable;
use crate::css::computed_values::computed_group_output_mask;
use crate::css::computed_values::release_group_payload;
use crate::css::computed_values::replay_style_group_identity;
use crate::css::computed_values::replaying_style_groups;
use crate::css::computed_values::retain_group_payload;
use crate::css::computed_values::retained_group_payload_bytes;
use crate::css::computed_values::style_group_payloads_equal;
use crate::css::computed_values::style_group_payloads_hold_image_values;
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::retained_value_depends_on_color_scheme;
use crate::css::style_value::retained_value_depends_on_current_color;
use crate::css::style_value::retained_value_may_depend_on_font_metrics;

// Bit 3 is a node-local production capability carried with publication and stripped before the
// semantic fixed metadata is interned or exposed through a style-record view.
pub(crate) const INHERITED_GROUP_SWAP_ELIGIBLE: u8 = 1 << 3;
/// The record holds an `<image>` in a property whose images a layout node loads and observes.
/// Derived from the published payloads; see `style_group_payloads_hold_image_values`.
pub(crate) const HOLDS_IMAGE_VALUES: u8 = 1 << 4;

/// The inherited style groups lead every group tuple; a node's inherited-group column names them.
pub(super) const ENGINE_INHERITED_GROUP_COUNT: usize = 7;

/// Whether a record with this table may take an inherited-group swap: every property inherits
/// the way its definition says, no marker is generated for it, and no transition runs on it.
pub(super) fn table_inherited_group_swap_eligible(table: &ComputedLonghandTable) -> bool {
    table.property_inheritance_is_standard()
        && !table.display_is_list_item()
        && crate::css::style_compute::active_transition_properties(table).is_empty()
}

/// What assembling an engine-computed record produced, for the publication counters.
pub(super) struct EngineComputedAssembly {
    pub(super) delta: (FinalStyleRecordID, FinalStyleRecordID),
    /// Rebuilt groups whose payload equalled the old one and kept its identity.
    pub(super) canonicalized_groups: u32,
    /// Whether the node's group tuple identity stayed put, so nothing propagates from it.
    pub(super) group_set_unchanged: bool,
}
const COMPUTED_VALUE_DEPENDENCY_FLAGS: u8 = (INHERITED_GROUP_SWAP_ELIGIBLE - 1) | HOLDS_IMAGE_VALUES;

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

define_id! { pub struct ComputedLonghandTableID(); }

impl InternIdentity for ComputedLonghandTableID {
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

/// The interned form of one drive's computed longhand table. Provenance in
/// the source-slot sidecar is per-drive and does not participate in identity.
struct RetainedLonghandTable {
    table: *const ComputedLonghandTable,
    /// The order-sensitive sum of the table's per-slot value hashes: the part of the table's
    /// publication hash a patched copy can adjust slot by slot instead of rehashing every value.
    slot_hash_sum: u64,
}

impl RetainedLonghandTable {
    fn value_view(&self) -> &[*const c_void] {
        unsafe { &*self.table }.value_pointers()
    }

    fn table(&self) -> &ComputedLonghandTable {
        unsafe { &*self.table }
    }
}

impl Drop for RetainedLonghandTable {
    fn drop(&mut self) {
        if !self.table.is_null() {
            unsafe {
                crate::css::computed_longhand_table::rust_computed_longhand_table_release(self.table.cast_mut());
            }
        }
    }
}

pub(crate) struct StyleRecordView<'a> {
    pub payloads: &'a [*const c_void],
    pub base_payloads: &'a [*const c_void],
    /// Always the base record's table: animation overlays store no table entries.
    pub longhand_table: *const ComputedLonghandTable,
    pub longhand_values: &'a [*const c_void],
    pub animated_overlay: *const crate::css::animated_overlay::AnimatedOverlay,
    pub pseudo_element_styles: u64,
    pub counter_style_environment_identity: u64,
    pub animation_overlay_identity: u64,
    pub dependency_flags: u8,
}

impl StyleRecordView<'_> {
    pub(crate) fn longhand_table_for_partial_drive(&self) -> ComputedLonghandTable {
        let source = unsafe {
            self.longhand_table
                .as_ref()
                .expect("a retained style record must carry a longhand table")
        };
        ComputedLonghandTable::copied_for_partial_drive(source)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct StyleRecord {
    groups: ComputedGroupSetID,
    inherited_groups: InheritedGroupSetID,
    custom_properties: CustomPropertyEnvironmentID,
    fixed_metadata: ComputedFixedMetadataID,
    longhand_table: Option<ComputedLonghandTableID>,
}

impl Hash for StyleRecord {
    fn hash<H: Hasher>(&self, state: &mut H) {
        // The inherited subset is derived from the groups, so it contributes no useful entropy.
        self.groups.hash(state);
        self.custom_properties.hash(state);
        self.fixed_metadata.hash(state);
        self.longhand_table.hash(state);
    }
}

pub struct ComputedMetadataInput<'a> {
    pub pseudo_element_styles: u64,
    pub dependency_flags: u8,
    pub counter_style_environment_identity: u64,
    pub animation_overlay_identity: u64,
    pub animated_overlay: *const crate::css::animated_overlay::AnimatedOverlay,
    pub animation_overlay_payloads: &'a [*const c_void],
    /// The drive's frozen computed longhand table, or null when the publisher
    /// carries none. Its values are interned as the record's longhand-table
    /// relation. The style-sheet-context sidecar stays on the drive table:
    /// its cascade source slots are only meaningful against the drive's own
    /// cascade, so folding them into interned identity would split records
    /// across otherwise identical recomputes.
    pub longhand_table: *const ComputedLonghandTable,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct FinalStyleRecordID(u64);

impl FinalStyleRecordID {
    /// No record: what a node holds before its first publication.
    pub(crate) const NONE: Self = Self(0);
    const ANIMATION_OVERLAY_TAG: u64 = 1 << 63;
    const MAX_BASE_GENERATION: u32 = (1 << 31) - 1;

    fn base(style_record: StyleRecordID, generation: u32) -> Self {
        assert!(
            generation <= Self::MAX_BASE_GENERATION,
            "base style-record generation space exhausted"
        );
        Self((u64::from(generation) << 32) | u64::from(style_record.raw()))
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
        if self.0 as u32 == 0 {
            return None;
        }
        Some(StyleRecordID(NonZeroU32::new(self.0 as u32)?))
    }

    fn base_generation(self) -> u32 {
        (self.0 >> 32) as u32
    }
}

struct AnimationOverlayRecord {
    // NB: No sampled value enters a permanent interning table. The current assignment owns one
    //     reference, while detached layout and stabilization baselines can pin an old generation.
    base_style_record: StyleRecordID,
    source_identity: u64,
    final_style_record: FinalStyleRecordID,
    animated_overlay: Box<crate::css::animated_overlay::AnimatedOverlay>,
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
    identity_hash: u64,
    payloads: Box<[*const c_void]>,
    canonical_longhand_table: Option<ComputedLonghandTableID>,
}

#[derive(Clone, Copy)]
struct PublishedComputedInputs {
    groups: ComputedGroupSetID,
    inherited_groups: InheritedGroupSetID,
    custom_properties: CustomPropertyEnvironmentID,
    fixed_metadata: ComputedFixedMetadataID,
    style_record: StyleRecordID,
    animation_overlay_slot: Option<u32>,
}

#[derive(Clone, Copy)]
struct PseudoComputedRow {
    kind: u8,
    flags: u8,
    assignment: Option<PublishedComputedInputs>,
    cascade_versions: [u64; 3],
    cascade_states: [CascadeStateID; 3],
}

#[derive(Default)]
struct PublishedComputedColumns {
    groups: Vec<u32>,
    inherited_groups: Vec<u32>,
    custom_properties: Vec<u32>,
    fixed_metadata: Vec<u32>,
    animation_overlay_slots: Vec<u32>,
    cascade_versions: Vec<u64>,
    cascade_states: Vec<u32>,
    flags: Vec<u8>,
    /// The element facts the style computation's adjustments read; see
    /// `bridge::element_adjustment_fact`.
    adjustment_facts: Vec<u32>,
}

impl PublishedComputedColumns {
    const ASSIGNED: u8 = 1;
    const INHERITED_GROUP_SWAP_ELIGIBLE: u8 = 1 << 1;
    const HAS_CASCADE_STATE: u8 = 1 << 2;
    /// The node's last published match answer declared past its winners.
    const INCOMPLETE_ANSWER: u8 = 1 << 3;

    fn ensure(&mut self, index: usize) {
        if self.flags.len() > index {
            return;
        }
        let len = index
            .checked_add(1)
            .expect("computed publication column space exhausted");
        self.groups.resize(len, 0);
        self.inherited_groups.resize(len, 0);
        self.custom_properties.resize(len, 0);
        self.fixed_metadata.resize(len, 0);
        self.animation_overlay_slots.resize(len, 0);
        self.cascade_versions.resize(len, 0);
        self.cascade_states.resize(len, 0);
        self.flags.resize(len, 0);
        self.adjustment_facts.resize(len, 0);
    }

    fn is_assigned(&self, index: usize) -> bool {
        self.flags.get(index).is_some_and(|flags| flags & Self::ASSIGNED != 0)
    }

    fn groups(&self, index: usize) -> Option<ComputedGroupSetID> {
        self.is_assigned(index).then(|| ComputedGroupSetID(self.groups[index]))
    }

    fn inherited_groups(&self, index: usize) -> Option<InheritedGroupSetID> {
        self.is_assigned(index)
            .then(|| InheritedGroupSetID(self.inherited_groups[index]))
    }

    fn custom_properties(&self, index: usize) -> Option<CustomPropertyEnvironmentID> {
        self.is_assigned(index)
            .then(|| CustomPropertyEnvironmentID(self.custom_properties[index]))
    }

    fn fixed_metadata(&self, index: usize) -> Option<ComputedFixedMetadataID> {
        self.is_assigned(index)
            .then(|| ComputedFixedMetadataID(self.fixed_metadata[index]))
    }

    fn animation_overlay_slot(&self, index: usize) -> Option<u32> {
        let encoded = *self.animation_overlay_slots.get(index)?;
        encoded.checked_sub(1)
    }

    fn set_animation_overlay_slot(&mut self, index: usize, slot: Option<u32>) {
        self.animation_overlay_slots[index] = slot.map_or(0, |slot| {
            slot.checked_add(1)
                .expect("animation overlay slot identity space exhausted")
        });
    }

    fn inherited_group_swap_eligible(&self, index: usize) -> bool {
        self.flags
            .get(index)
            .is_some_and(|flags| flags & Self::INHERITED_GROUP_SWAP_ELIGIBLE != 0)
    }

    fn answer_is_incomplete(&self, index: usize) -> bool {
        self.flags
            .get(index)
            .is_some_and(|flags| flags & Self::INCOMPLETE_ANSWER != 0)
    }

    fn set_answer_incomplete(&mut self, index: usize, incomplete: bool) {
        self.ensure(index);
        if incomplete {
            self.flags[index] |= Self::INCOMPLETE_ANSWER;
        } else {
            self.flags[index] &= !Self::INCOMPLETE_ANSWER;
        }
    }

    fn set_inherited_group_swap_eligible(&mut self, index: usize, eligible: bool) {
        if let Some(flags) = self.flags.get_mut(index) {
            if eligible {
                *flags |= Self::INHERITED_GROUP_SWAP_ELIGIBLE;
            } else {
                *flags &= !Self::INHERITED_GROUP_SWAP_ELIGIBLE;
            }
        }
    }

    fn cascade_state(&self, index: usize) -> Option<(u64, CascadeStateID)> {
        self.flags
            .get(index)
            .is_some_and(|flags| flags & Self::HAS_CASCADE_STATE != 0)
            .then(|| (self.cascade_versions[index], CascadeStateID(self.cascade_states[index])))
    }

    fn replace_cascade_state(
        &mut self,
        index: usize,
        state: Option<(u64, CascadeStateID)>,
    ) -> Option<(u64, CascadeStateID)> {
        let previous = self.cascade_state(index);
        if let Some((version, state)) = state {
            self.cascade_versions[index] = version;
            self.cascade_states[index] = state.0;
            self.flags[index] |= Self::HAS_CASCADE_STATE;
        } else if let Some(flags) = self.flags.get_mut(index) {
            *flags &= !Self::HAS_CASCADE_STATE;
        }
        previous
    }

    fn publish(&mut self, index: usize, inputs: PublishedComputedInputs, inherited_group_swap_eligible: bool) {
        self.ensure(index);
        self.groups[index] = inputs.groups.0;
        self.inherited_groups[index] = inputs.inherited_groups.0;
        self.custom_properties[index] = inputs.custom_properties.0;
        self.fixed_metadata[index] = inputs.fixed_metadata.0;
        self.set_animation_overlay_slot(index, inputs.animation_overlay_slot);
        self.flags[index] = (self.flags[index] & (Self::HAS_CASCADE_STATE | Self::INCOMPLETE_ANSWER))
            | Self::ASSIGNED
            | if inherited_group_swap_eligible {
                Self::INHERITED_GROUP_SWAP_ELIGIBLE
            } else {
                0
            };
    }

    fn remove(&mut self, index: usize) -> Option<u32> {
        let overlay = self.animation_overlay_slot(index);
        if let Some(flags) = self.flags.get_mut(index) {
            *flags = 0;
            self.animation_overlay_slots[index] = 0;
        }
        overlay
    }
}

struct ComputedReachability {
    groups: Vec<bool>,
    sets: Vec<bool>,
    inherited_sets: Vec<bool>,
    custom_property_environments: Vec<bool>,
    fixed_metadata: Vec<bool>,
    longhand_tables: Vec<bool>,
    style_records: Vec<bool>,
}

impl ComputedReachability {
    fn mark<Identity: InternIdentity>(marks: &mut [bool], identity: Identity) {
        marks[identity.index()] = true;
    }
}

#[derive(Clone, Copy)]
pub(super) struct ComputedGroupRetention {
    pub retained: usize,
    pub reachable: usize,
}

impl PseudoComputedRow {
    const PUBLISHED: u8 = 1;
    const CURRENT_CASCADE: usize = 0;
    const RETAINED_CASCADE: usize = 1;
    const PENDING_CASCADE: usize = 2;

    fn new(kind: u8) -> Self {
        Self {
            kind,
            flags: 0,
            assignment: None,
            cascade_versions: [0; 3],
            cascade_states: [CascadeStateID(0); 3],
        }
    }

    fn is_published(&self) -> bool {
        self.flags & Self::PUBLISHED != 0
    }

    fn set_published(&mut self, published: bool) {
        self.flags = (self.flags & !Self::PUBLISHED) | if published { Self::PUBLISHED } else { 0 };
    }

    fn cascade_state(&self, index: usize) -> Option<(u64, CascadeStateID)> {
        (self.flags & (1 << (index + 1)) != 0).then_some((self.cascade_versions[index], self.cascade_states[index]))
    }

    fn replace_cascade_state(
        &mut self,
        index: usize,
        state: Option<(u64, CascadeStateID)>,
    ) -> Option<(u64, CascadeStateID)> {
        let previous = self.cascade_state(index);
        if let Some((version, state)) = state {
            self.cascade_versions[index] = version;
            self.cascade_states[index] = state;
            self.flags |= 1 << (index + 1);
        } else {
            self.flags &= !(1 << (index + 1));
        }
        previous
    }

    fn is_empty(&self) -> bool {
        self.flags == 0 && self.assignment.is_none()
    }
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
    pub new_style_record: bool,
    pub node_handle_changed: bool,
    pub inherited_node_handle_changed: bool,
    pub custom_property_environment_node_handle_changed: bool,
    pub computed_fixed_metadata_node_handle_changed: bool,
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
    inherited_sets: InternTable<InheritedGroupSetID, Box<[ComputedGroupID]>>,
    custom_property_environments: InternTable<CustomPropertyEnvironmentID, u64>,
    computed_fixed_metadata: InternTable<ComputedFixedMetadataID, ComputedFixedMetadata>,
    computed_longhand_tables: InternTable<ComputedLonghandTableID, RetainedLonghandTable>,
    style_records: InternTable<StyleRecordID, StyleRecord>,
    style_record_liveness: BitColumn,
    style_record_generations: Vec<u32>,
    style_record_column: Vec<Option<StyleRecordID>>,
    base_style_record_pins: HashMap<StyleRecordID, u64>,
    columns: PublishedComputedColumns,
    // Recyclable animation overlays are deliberately separate from the permanent base records
    // above. Dense element assignments and sparse pseudo assignments pin at most one slot each.
    animation_overlay_slots: Vec<Option<AnimationOverlayRecord>>,
    animation_overlay_slots_by_record: HashMap<FinalStyleRecordID, u32>,
    free_animation_overlay_slots: Vec<u32>,
    live_animation_overlay_assignments: usize,
    next_animation_overlay_generation: u64,
    pending_cascade_states: HashMap<StyleNodeID, (u64, CascadeStateID)>,
    pseudo_rows_by_node: HashMap<StyleNodeID, Box<[PseudoComputedRow]>>,
    group_set_nested_memory: MemoryLease,
    longhand_table_nested_memory: MemoryLease,
    animation_overlay_nested_memory: MemoryLease,
    pseudo_assignment_nested_memory: MemoryLease,
    style_records_interned_since_reclamation: usize,
    next_reclamation_after: usize,
    style_record_view_epoch_depth: u32,
}

impl Default for ComputedGroupSets {
    fn default() -> Self {
        Self {
            groups: InternTable::default(),
            sets: InternTable::default(),
            inherited_sets: InternTable::default(),
            custom_property_environments: InternTable::default(),
            computed_fixed_metadata: InternTable::default(),
            computed_longhand_tables: InternTable::default(),
            style_records: InternTable::default(),
            style_record_liveness: BitColumn::default(),
            style_record_generations: Vec::new(),
            style_record_column: Vec::new(),
            base_style_record_pins: HashMap::default(),
            columns: PublishedComputedColumns::default(),
            animation_overlay_slots: Vec::new(),
            animation_overlay_slots_by_record: HashMap::default(),
            free_animation_overlay_slots: Vec::new(),
            live_animation_overlay_assignments: 0,
            next_animation_overlay_generation: 0,
            pending_cascade_states: HashMap::default(),
            pseudo_rows_by_node: HashMap::default(),
            group_set_nested_memory: MemoryLease::new(MemoryCategory::ComputedGroupSet),
            longhand_table_nested_memory: MemoryLease::new(MemoryCategory::ComputedLonghandTable),
            animation_overlay_nested_memory: MemoryLease::new(MemoryCategory::AnimationOverlayRecord),
            pseudo_assignment_nested_memory: MemoryLease::new(MemoryCategory::ComputedPseudoAssignment),
            style_records_interned_since_reclamation: 0,
            next_reclamation_after: 1024,
            style_record_view_epoch_depth: 0,
        }
    }
}

impl ComputedGroupSets {
    pub(crate) fn begin_style_record_view_epoch(&mut self) {
        self.style_record_view_epoch_depth = self
            .style_record_view_epoch_depth
            .checked_add(1)
            .expect("style-record view epoch depth overflow");
    }

    pub(crate) fn end_style_record_view_epoch(&mut self) {
        assert!(
            self.style_record_view_epoch_depth > 0,
            "style-record view epoch underflow"
        );
        self.style_record_view_epoch_depth -= 1;
    }

    fn group_identity(&self, index: usize, payload: *const c_void) -> ComputedGroupID {
        let key = (index, payload as usize);
        self.groups
            .find(content_hash(key), |_identity, group| {
                (group.index, group.payload as usize) == key
            })
            .expect("computed group-set payload names a live group")
    }

    fn group_identities(&self, set: ComputedGroupSetID) -> Vec<ComputedGroupID> {
        self.sets[set]
            .payloads
            .iter()
            .copied()
            .enumerate()
            .map(|(index, payload)| self.group_identity(index, payload))
            .collect()
    }

    fn pseudo_rows(&self, node: StyleNodeID) -> &[PseudoComputedRow] {
        self.pseudo_rows_by_node.get(&node).map_or(&[], Box::as_ref)
    }

    fn pseudo_row(&self, node: StyleNodeID, kind: u8) -> Option<&PseudoComputedRow> {
        self.pseudo_rows(node).iter().find(|row| row.kind == kind)
    }

    fn pseudo_row_mut(&mut self, node: StyleNodeID, kind: u8) -> Option<&mut PseudoComputedRow> {
        self.pseudo_rows_by_node
            .get_mut(&node)
            .and_then(|rows| rows.iter_mut().find(|row| row.kind == kind))
    }

    fn ensure_pseudo_row(&mut self, node: StyleNodeID, kind: u8) -> &mut PseudoComputedRow {
        let row_index = self
            .pseudo_rows_by_node
            .get(&node)
            .and_then(|rows| rows.iter().position(|row| row.kind == kind));
        let row_index = row_index.unwrap_or_else(|| {
            let mut rows = self
                .pseudo_rows_by_node
                .remove(&node)
                .map_or_else(Vec::new, |rows| rows.into_vec());
            rows.push(PseudoComputedRow::new(kind));
            self.pseudo_assignment_nested_memory
                .grow_committed(size_of::<PseudoComputedRow>() as u64);
            let row_index = rows.len() - 1;
            self.pseudo_rows_by_node.insert(node, rows.into_boxed_slice());
            row_index
        });
        &mut self
            .pseudo_rows_by_node
            .get_mut(&node)
            .expect("pseudo row entry is live")[row_index]
    }

    fn remove_empty_pseudo_row(&mut self, node: StyleNodeID, kind: u8) {
        let Some(row_index) = self
            .pseudo_rows_by_node
            .get(&node)
            .and_then(|rows| rows.iter().position(|row| row.kind == kind && row.is_empty()))
        else {
            return;
        };
        let mut rows = self
            .pseudo_rows_by_node
            .remove(&node)
            .expect("pseudo row entry is live")
            .into_vec();
        rows.remove(row_index);
        self.pseudo_assignment_nested_memory
            .shrink_committed(size_of::<PseudoComputedRow>() as u64);
        if !rows.is_empty() {
            self.pseudo_rows_by_node.insert(node, rows.into_boxed_slice());
        }
    }

    pub(super) fn assigned_style_record(&self, node: StyleNodeID) -> Option<FinalStyleRecordID> {
        let index = node.element_index()? as usize;
        let style_record = *self.style_record_column.get(index)?.as_ref()?;
        Some(self.final_style_record(style_record, self.columns.animation_overlay_slot(index)))
    }

    pub(super) fn viewport_dependent_nodes(&self) -> Vec<u32> {
        let depends_on_viewport = |fixed_metadata: ComputedFixedMetadataID| {
            self.computed_fixed_metadata.get(fixed_metadata).dependency_flags & 1 != 0
        };
        let mut nodes = Vec::new();
        for index in 1..self.columns.flags.len() {
            if self.columns.fixed_metadata(index).is_some_and(depends_on_viewport) {
                nodes.push(u32::try_from(index).expect("computed style node identity exceeds u32"));
            }
        }
        for (&node, rows) in &self.pseudo_rows_by_node {
            if rows.iter().any(|row| {
                row.assignment
                    .is_some_and(|assignment| depends_on_viewport(assignment.fixed_metadata))
            }) {
                nodes.push(node.raw());
            }
        }
        nodes.sort_unstable();
        nodes.dedup();
        nodes
    }

    fn intern_group_set(&mut self, groups: &[ComputedGroupID]) -> (ComputedGroupSetID, bool) {
        let hash = content_hash(groups);
        if let Some(identity) = self.sets.find(hash, |_identity, set| {
            set.payloads.len() == groups.len()
                && set
                    .payloads
                    .iter()
                    .zip(groups)
                    .all(|(&payload, &identity)| payload == self.groups[identity].payload)
        }) {
            return (identity, false);
        }
        let identity = self.sets.take_free_identity().unwrap_or_else(|| {
            ComputedGroupSetID(u32::try_from(self.sets.len()).expect("computed group-set identity space exhausted"))
        });
        let payloads = groups
            .iter()
            .map(|identity| self.groups[*identity].payload)
            .collect::<Vec<_>>()
            .into_boxed_slice();
        self.group_set_nested_memory
            .grow_committed(size_of_val(payloads.as_ref()) as u64);
        self.sets.insert(
            hash,
            identity,
            ComputedGroupSet {
                identity_hash: hash,
                payloads,
                canonical_longhand_table: None,
            },
        );
        (identity, true)
    }

    fn intern_inherited_group_set(&mut self, groups: &[ComputedGroupID]) -> (InheritedGroupSetID, bool) {
        let hash = content_hash(groups);
        if let Some(identity) = self
            .inherited_sets
            .find(hash, |_identity, candidate| candidate.as_ref() == groups)
        {
            return (identity, false);
        }
        let identity = self.inherited_sets.take_free_identity().unwrap_or_else(|| {
            InheritedGroupSetID(
                u32::try_from(self.inherited_sets.len()).expect("inherited group-set identity space exhausted"),
            )
        });
        let groups: Box<[ComputedGroupID]> = groups.into();
        self.group_set_nested_memory
            .grow_committed(size_of_val(groups.as_ref()) as u64);
        self.inherited_sets.insert(hash, identity, groups);
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
        let identity = self.style_records.take_free_identity().unwrap_or_else(|| {
            let raw = u32::try_from(
                self.style_records
                    .len()
                    .checked_add(1)
                    .expect("base style-record identity space exhausted"),
            )
            .expect("base style-record identity space exhausted");
            StyleRecordID(NonZeroU32::new(raw).expect("base style-record identities are nonzero"))
        });
        if identity.index() == self.style_record_generations.len() {
            self.style_record_generations.push(0);
        } else {
            let generation = &mut self.style_record_generations[identity.index()];
            *generation = generation
                .checked_add(1)
                .filter(|&generation| generation <= FinalStyleRecordID::MAX_BASE_GENERATION)
                .expect("base style-record generation space exhausted");
        }
        self.style_records.insert(hash, identity, record);
        let (changed, _) = self.style_record_liveness.set(identity.index(), true);
        assert!(changed, "new base style-record identity must not already be live");
        self.style_records_interned_since_reclamation = self
            .style_records_interned_since_reclamation
            .checked_add(1)
            .expect("style-record reclamation growth count overflow");
        (identity, true)
    }

    fn style_record_is_live(&self, identity: StyleRecordID) -> bool {
        self.style_record_liveness.contains(identity.index())
    }

    fn style_record_generation_is_live(&self, identity: StyleRecordID, generation: u32) -> bool {
        self.style_record_is_live(identity) && self.style_record_generations.get(identity.index()) == Some(&generation)
    }

    fn final_base_style_record(&self, identity: StyleRecordID) -> FinalStyleRecordID {
        FinalStyleRecordID::base(identity, self.style_record_generations[identity.index()])
    }

    /// Interns one frozen computed longhand table. Values and publication
    /// metadata participate in identity; the per-drive provenance sidecar
    /// does not.
    fn intern_longhand_table(
        &mut self,
        table: &ComputedLonghandTable,
        previous: Option<ComputedLonghandTableID>,
        canonical: Option<ComputedLonghandTableID>,
    ) -> ComputedLonghandTableID {
        debug_assert!(table.is_frozen(), "only frozen longhand tables are published");
        for candidate in [previous, canonical].into_iter().flatten() {
            if self.computed_longhand_tables[candidate]
                .table()
                .publication_equals(table)
            {
                return candidate;
            }
        }
        self.intern_longhand_table_with_slot_hash_sum(table, longhand_table_slot_hash_sum(table))
    }

    /// Intern a frozen table whose per-slot hash sum the caller already knows, from the table it
    /// was patched from.
    fn intern_longhand_table_with_slot_hash_sum(
        &mut self,
        table: &ComputedLonghandTable,
        slot_hash_sum: u64,
    ) -> ComputedLonghandTableID {
        debug_assert!(table.is_frozen(), "only frozen longhand tables are published");
        debug_assert_eq!(slot_hash_sum, longhand_table_slot_hash_sum(table));
        let hash = longhand_table_hash_with_slot_hash_sum(table, slot_hash_sum);
        if let Some(identity) = self
            .computed_longhand_tables
            .find(hash, |_identity, candidate| candidate.table().publication_equals(table))
        {
            return identity;
        }
        let identity = self.computed_longhand_tables.take_free_identity().unwrap_or_else(|| {
            ComputedLonghandTableID(
                u32::try_from(self.computed_longhand_tables.len())
                    .expect("computed longhand-table identity space exhausted"),
            )
        });
        let retained = unsafe { crate::css::computed_longhand_table::rust_computed_longhand_table_retain(table) };
        self.longhand_table_nested_memory
            .grow_committed(size_of_val(table.value_pointers()) as u64);
        self.computed_longhand_tables.insert(
            hash,
            identity,
            RetainedLonghandTable {
                table: retained,
                slot_hash_sum,
            },
        );
        identity
    }

    /// Replace a fully inheriting element's engine-resolvable inherited groups with its flat-tree
    /// parent's groups and publish the resulting base style record without rebuilding computed values.
    pub(super) fn replace_engine_resolvable_inherited_groups(
        &mut self,
        node: StyleNodeID,
        parent: StyleNodeID,
        inherited_style_groups: u8,
    ) -> Option<(FinalStyleRecordID, FinalStyleRecordID)> {
        const STATIC_INHERITED_GROUPS: u8 = (1 << 0) | (1 << 1) | (1 << 3);
        const INHERITED_UI_GROUP: u8 = 1 << 2;
        const INHERITED_TEXT_GROUP: u8 = 1 << 4;
        const FONT_GROUP: u8 = 1 << 6;
        const ENGINE_RESOLVABLE_INHERITED_GROUPS: u8 =
            STATIC_INHERITED_GROUPS | INHERITED_UI_GROUP | INHERITED_TEXT_GROUP | FONT_GROUP;
        const INHERITED_GROUP_COUNT: usize = 7;
        const INHERITED_GROUP_MASK: u32 = (1 << INHERITED_GROUP_COUNT) - 1;
        const CURRENT_COLOR_REBUILDABLE_GROUPS: u32 = (1
            << crate::css::computed_value_types::STYLE_GROUP_INDEX_SVG_RESET)
            | (1 << crate::css::computed_value_types::STYLE_GROUP_INDEX_EFFECTS)
            | (1 << crate::css::computed_value_types::STYLE_GROUP_INDEX_TEXT_RESET)
            | (1 << crate::css::computed_value_types::STYLE_GROUP_INDEX_BACKGROUND)
            | (1 << crate::css::computed_value_types::STYLE_GROUP_INDEX_BORDER)
            | (1 << crate::css::computed_value_types::STYLE_GROUP_INDEX_MISC_RESET);
        if inherited_style_groups == 0 || inherited_style_groups & !ENGINE_RESOLVABLE_INHERITED_GROUPS != 0 {
            return None;
        }
        let target = ComputedStyleTarget::new(node, u8::MAX);
        let current_color_dependencies = if inherited_style_groups & INHERITED_TEXT_GROUP != 0 {
            self.current_color_dependency_mask(target)?
        } else {
            0
        };
        if current_color_dependencies & (1 << crate::css::computed_value_types::STYLE_GROUP_INDEX_SVG_RESET) != 0 {
            return None;
        }
        if current_color_dependencies & !(INHERITED_GROUP_MASK | CURRENT_COLOR_REBUILDABLE_GROUPS) != 0 {
            return None;
        }
        if inherited_style_groups & INHERITED_UI_GROUP != 0
            && self
                .color_scheme_dependency_mask(target)
                .is_none_or(|dependencies| dependencies & !INHERITED_GROUP_MASK != 0)
        {
            return None;
        }
        if inherited_style_groups & FONT_GROUP != 0
            && self
                .font_dependency_mask(target)
                .is_none_or(|dependencies| dependencies & !INHERITED_GROUP_MASK != 0)
        {
            return None;
        }
        let index = node.element_index()? as usize;
        let parent_index = parent.element_index()? as usize;
        // A child inherits the parent's animated values, which C++ composes over the record.
        if !self.columns.inherited_group_swap_eligible(index)
            || self.columns.animation_overlay_slot(index).is_some()
            || self.node_has_animation_overlay(parent)
            || self.adjustment_facts(parent) & super::bridge::element_adjustment_fact::HAS_ANIMATIONS != 0
            || self.assigned_pseudo_kinds(node).next().is_some()
        {
            return None;
        }

        let old_style_record = *self.style_record_column.get(index)?.as_ref()?;
        let old_record = *self.style_records.get_index(old_style_record.index())?;
        // An element animating, or declaring transitions a moved inherited value may start,
        // composes its style in C++.
        if self.adjustment_facts(node) & super::bridge::element_adjustment_fact::HAS_ANIMATIONS != 0
            || old_record
                .longhand_table
                .and_then(|table| self.computed_longhand_tables.get_index(table.index()))
                .is_none_or(|retained| {
                    !crate::css::style_compute::active_transition_properties(retained.table()).is_empty()
                })
        {
            return None;
        }
        let old_group_set = self.sets.get_index(old_record.groups.0 as usize)?;
        let old_payloads = old_group_set.payloads.to_vec();
        let parent_inherited = self.columns.inherited_groups(parent_index)?;
        let parent_groups = self.inherited_sets.get_index(parent_inherited.0 as usize)?.to_vec();
        if parent_groups.len() != INHERITED_GROUP_COUNT || old_group_set.payloads.len() < INHERITED_GROUP_COUNT {
            return None;
        }

        if current_color_dependencies & !INHERITED_GROUP_MASK != 0 {
            let inherited_box = unsafe {
                &*old_payloads[crate::css::computed_value_types::STYLE_GROUP_INDEX_INHERITED_BOX]
                    .cast::<crate::css::computed_values::InheritedBoxValues>()
            };
            let dependencies = self.current_color_dependency_properties(target)?;
            for property in crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
                ..=crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID
            {
                let slot = usize::from(property - crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID);
                if dependencies[slot / 64] & (1 << (slot % 64)) == 0
                    || crate::css::property_metadata::property_is_inherited(property)
                {
                    continue;
                }
                let physical_property = if crate::css::property_metadata::longhand_is_logical_alias(property) {
                    crate::css::style_compute::map_logical_alias_to_physical(
                        property,
                        inherited_box.writing_mode,
                        inherited_box.direction,
                    )
                } else {
                    property
                };
                if !matches!(
                    physical_property,
                    crate::css::property_metadata::property_id::BACKDROP_FILTER
                        | crate::css::property_metadata::property_id::BACKGROUND_COLOR
                        | crate::css::property_metadata::property_id::BORDER_BOTTOM_COLOR
                        | crate::css::property_metadata::property_id::BORDER_LEFT_COLOR
                        | crate::css::property_metadata::property_id::BORDER_RIGHT_COLOR
                        | crate::css::property_metadata::property_id::BORDER_TOP_COLOR
                        | crate::css::property_metadata::property_id::BOX_SHADOW
                        | crate::css::property_metadata::property_id::COLUMN_RULE_COLOR
                        | crate::css::property_metadata::property_id::FILTER
                        | crate::css::property_metadata::property_id::FLOOD_COLOR
                        | crate::css::property_metadata::property_id::OUTLINE_COLOR
                        | crate::css::property_metadata::property_id::STOP_COLOR
                        | crate::css::property_metadata::property_id::TEXT_DECORATION_COLOR
                ) {
                    return None;
                }
            }
        }

        let parent_style_record = self.style_record_column.get(parent_index).copied().flatten();
        let parent_table = parent_style_record
            .and_then(|record| self.style_records.get_index(record.index()))
            .and_then(|record| record.longhand_table);
        if parent_table.is_some_and(|table| {
            !crate::css::style_compute::active_transition_properties(self.computed_longhand_tables[table].table())
                .is_empty()
        }) {
            return None;
        }
        let swapped_table = match (old_record.longhand_table, parent_table) {
            (Some(old_table), Some(parent_table)) => Some(
                self.computed_longhand_tables[old_table]
                    .table()
                    .with_inherited_values_from(self.computed_longhand_tables[parent_table].table())
                    .into_raw_shared(),
            ),
            (None, _) if current_color_dependencies & !INHERITED_GROUP_MASK == 0 => None,
            _ => return None,
        };

        let mut groups = self.group_identities(old_record.groups);
        groups[..INHERITED_GROUP_COUNT].copy_from_slice(&parent_groups);
        if let Some(table) = swapped_table
            && current_color_dependencies & !INHERITED_GROUP_MASK != 0
        {
            let inherited_text = unsafe {
                &*self.groups[parent_groups[crate::css::computed_value_types::STYLE_GROUP_INDEX_INHERITED_TEXT]]
                    .payload
                    .cast::<crate::css::computed_value_types::InheritedTextValues>()
            };
            let inherited_ui = unsafe {
                &*self.groups[parent_groups[crate::css::computed_value_types::STYLE_GROUP_INDEX_INHERITED_UI]]
                    .payload
                    .cast::<crate::css::computed_value_types::InheritedUIValues>()
            };
            for group in INHERITED_GROUP_COUNT..groups.len() {
                if current_color_dependencies & (1 << group) == 0 {
                    continue;
                }
                let payload = unsafe {
                    crate::css::table_group_builder::rebuild_group_for_inherited_current_color(
                        &*table,
                        group,
                        old_payloads[group],
                        inherited_text.color,
                        inherited_ui.color_scheme,
                    )
                }
                .expect("a supported currentcolor group rebuilds from its computed table");
                let identity = if payload == old_payloads[group] {
                    groups[group]
                } else if let Some(identity) = self
                    .groups
                    .find(content_hash((group, payload as usize)), |_identity, candidate| {
                        candidate.index == group && candidate.payload == payload
                    })
                {
                    identity
                } else {
                    retain_group_payload(group, payload);
                    let identity = self.groups.take_free_identity().unwrap_or_else(|| {
                        ComputedGroupID(
                            u32::try_from(self.groups.len()).expect("computed group identity space exhausted"),
                        )
                    });
                    self.groups.insert(
                        content_hash((group, payload as usize)),
                        identity,
                        ComputedGroup { index: group, payload },
                    );
                    self.group_set_nested_memory
                        .grow_committed(retained_group_payload_bytes(group, payload) as u64);
                    identity
                };
                release_group_payload(group, payload);
                groups[group] = identity;
            }
        }
        let group_set = self.intern_group_set(&groups).0;
        // The swap is only taken for a fully inheriting element, so every
        // inherited-by-default longhand's value is the parent's; the swapped
        // table keeps the record a complete inheritance source for a child.
        let longhand_table = swapped_table.map(|table| {
            let identity = self.intern_longhand_table(unsafe { &*table }, old_record.longhand_table, None);
            unsafe {
                crate::css::computed_longhand_table::rust_computed_longhand_table_release(table.cast_mut());
            }
            identity
        });
        let new_record = StyleRecord {
            groups: group_set,
            inherited_groups: parent_inherited,
            custom_properties: old_record.custom_properties,
            fixed_metadata: old_record.fixed_metadata,
            longhand_table,
        };
        let new_style_record = self.intern_style_record(new_record).0;
        self.columns.groups[index] = group_set.0;
        self.columns.inherited_groups[index] = parent_inherited.0;
        self.style_record_column[index] = Some(new_style_record);
        Some((
            self.final_base_style_record(old_style_record),
            self.final_base_style_record(new_style_record),
        ))
    }

    /// Assemble a new base record for `node` from a longhand table the engine drove itself. The
    /// table began as a copy of the node's current one, so its hash adjusts slot by slot; the
    /// groups `groups_to_rebuild` names are rebuilt from it, and the record's fixed metadata
    /// follows the table's dependency flags. The font group is never rebuilt here: it needs
    /// platform font resources the table does not hold.
    ///
    /// This decides only whether the record can be assembled here: the node must hold a retained
    /// longhand table, must not carry an animation overlay, and must have published under the
    /// same eligibility the inherited-group swap needs. Its pseudo-elements' records are
    /// untouched: they inherit from the element, and the caller has proven that nothing they
    /// inherit moved.
    #[allow(clippy::arc_with_non_send_sync, clippy::too_many_arguments)]
    pub(super) fn replace_engine_computed_table(
        &mut self,
        node: StyleNodeID,
        mut table: ComputedLonghandTable,
        groups_to_rebuild: u32,
        length: &crate::css::style_compute::FfiLengthResolutionContext,
        font: Option<&crate::css::table_group_builder::FfiFontGroupBuildInputs>,
        parent_in_display_none_subtree: bool,
    ) -> Option<EngineComputedAssembly> {
        use crate::css::computed_value_types::STYLE_GROUP_INDEX_FONT;
        if groups_to_rebuild == 0 || (groups_to_rebuild & (1 << STYLE_GROUP_INDEX_FONT) != 0 && font.is_none()) {
            return None;
        }
        let index = node.element_index()? as usize;
        if self.columns.animation_overlay_slot(index).is_some() {
            return None;
        }
        let old_style_record = *self.style_record_column.get(index)?.as_ref()?;
        let old_record = *self.style_records.get_index(old_style_record.index())?;
        let old_table = old_record.longhand_table?;
        let old_group_set = self.sets.get_index(old_record.groups.0 as usize)?;
        let old_payloads = old_group_set.payloads.to_vec();
        if groups_to_rebuild >> old_payloads.len() != 0
            || old_payloads.len() <= crate::css::computed_value_types::STYLE_GROUP_INDEX_INHERITED_UI
        {
            return None;
        }
        let used_color_scheme = unsafe {
            let inherited_ui = &*old_payloads[crate::css::computed_value_types::STYLE_GROUP_INDEX_INHERITED_UI]
                .cast::<crate::css::computed_value_types::InheritedUIValues>();
            inherited_ui.color_scheme
        };
        // The builders take the element's own color from the caller, so it is resolved from the
        // driven table before any group reads it.
        let current_color =
            crate::css::table_group_builder::own_color_from_table(&table, used_color_scheme, Some(length))?;
        for property in [
            crate::css::property_metadata::property_id::STOP_COLOR,
            crate::css::property_metadata::property_id::FLOOD_COLOR,
        ] {
            let specified = table
                .retained_inheritance_dependent_values()
                .find(|(candidate, value)| *candidate == property && retained_value_depends_on_current_color(value))
                .map(|(_, value)| value.clone_retained());
            let Some(specified) = specified else {
                continue;
            };
            let input = crate::css::color_resolution::ColorResolutionInput {
                scheme: Some(used_color_scheme),
                current_color: Some(crate::css::color_resolution::Rgba {
                    r: (current_color >> 16) as u8,
                    g: (current_color >> 8) as u8,
                    b: current_color as u8,
                    a: (current_color >> 24) as u8,
                }),
                current_color_value: None,
                length: Some(length),
                channels: None,
            };
            let resolved = crate::css::color_resolution::to_color(specified.data(), &input)?;
            let resolved = unsafe {
                RetainedStyleValueData::from_retained_pointer(std::sync::Arc::into_raw(std::sync::Arc::new(
                    crate::css::color_resolution::resolved_srgb_style_value(resolved),
                )))
            };
            let source_slot = table.source_slot(property).map_or(-1, i64::from);
            table.set(property, resolved, source_slot);
        }
        let (table, slot_hash_sum) = {
            let retained = &self.computed_longhand_tables[old_table];
            let source = retained.table();
            let mut slot_hash_sum = retained.slot_hash_sum;
            let old_values = source.value_pointers();
            let new_values = table.value_pointers();
            for slot in 0..old_values.len() {
                if old_values[slot] != new_values[slot] {
                    slot_hash_sum = slot_hash_sum
                        .wrapping_sub(longhand_slot_hash(slot, old_values[slot]))
                        .wrapping_add(longhand_slot_hash(slot, new_values[slot]));
                }
            }
            // The flag is the parent's and the driven display's, the way a fresh computation
            // sets it, not what the old table held.
            let display_is_none = crate::css::style_compute::effective_display(&table, None).is_none();
            table.set_in_display_none_subtree(parent_in_display_none_subtree || display_is_none);
            table.freeze();
            (table.into_raw_shared(), slot_hash_sum)
        };
        let release_table = |table: *const ComputedLonghandTable| unsafe {
            crate::css::computed_longhand_table::rust_computed_longhand_table_release(table.cast_mut());
        };

        let mut groups = self.group_identities(old_record.groups);
        let mut canonicalized_groups = 0_u32;
        for group in 0..groups.len() {
            if groups_to_rebuild & (1 << group) == 0 {
                continue;
            }
            let payload = if group == STYLE_GROUP_INDEX_FONT {
                unsafe {
                    crate::css::table_group_builder::rebuild_font_group_from_table(
                        &*table,
                        font.expect("a font group rebuild carries the resolved font"),
                        old_payloads[group],
                    )
                }
            } else {
                unsafe {
                    crate::css::table_group_builder::rebuild_group_from_table(
                        &*table,
                        group,
                        old_payloads[group],
                        current_color,
                        used_color_scheme,
                        Some(length),
                    )
                }
            };
            let Some(payload) = payload else {
                release_table(table);
                return None;
            };
            // An equal payload keeps the old identity, as a C++ build adopts its parent's and
            // predecessor's identical payloads.
            let identity = if payload == old_payloads[group]
                || style_group_payloads_equal(group, old_payloads[group], payload)
            {
                canonicalized_groups += 1;
                groups[group]
            } else if let Some(identity) = self
                .groups
                .find(content_hash((group, payload as usize)), |_identity, candidate| {
                    candidate.index == group && candidate.payload == payload
                })
            {
                identity
            } else {
                retain_group_payload(group, payload);
                let identity = self.groups.take_free_identity().unwrap_or_else(|| {
                    ComputedGroupID(u32::try_from(self.groups.len()).expect("computed group identity space exhausted"))
                });
                self.groups.insert(
                    content_hash((group, payload as usize)),
                    identity,
                    ComputedGroup { index: group, payload },
                );
                self.group_set_nested_memory
                    .grow_committed(retained_group_payload_bytes(group, payload) as u64);
                identity
            };
            release_group_payload(group, payload);
            groups[group] = identity;
        }
        let group_set = self.intern_group_set(&groups).0;
        let holds_image_values = self
            .sets
            .get_index(group_set.0 as usize)
            .is_some_and(|set| style_group_payloads_hold_image_values(&set.payloads));
        let old_metadata = self.computed_fixed_metadata[old_record.fixed_metadata];
        let swap_eligible = table_inherited_group_swap_eligible(unsafe { &*table });
        let dependency_flags =
            unsafe { &*table }.publication_dependency_flags() | (u8::from(holds_image_values) * HOLDS_IMAGE_VALUES);
        let fixed_metadata = if dependency_flags == old_metadata.dependency_flags {
            old_record.fixed_metadata
        } else {
            self.intern_fixed_metadata(ComputedFixedMetadata {
                dependency_flags,
                ..old_metadata
            })
        };
        let longhand_table = self.intern_longhand_table_with_slot_hash_sum(unsafe { &*table }, slot_hash_sum);
        release_table(table);
        let inherited_identity = self
            .intern_inherited_group_set(&groups[..ENGINE_INHERITED_GROUP_COUNT])
            .0;
        let new_record = StyleRecord {
            groups: group_set,
            inherited_groups: inherited_identity,
            custom_properties: old_record.custom_properties,
            fixed_metadata,
            longhand_table: Some(longhand_table),
        };
        let new_style_record = self.intern_style_record(new_record).0;
        // Descendant swaps read the node's inherited groups from their own column.
        self.columns.groups[index] = group_set.0;
        self.columns.inherited_groups[index] = inherited_identity.0;
        self.columns.set_inherited_group_swap_eligible(index, swap_eligible);
        self.style_record_column[index] = Some(new_style_record);
        Some(EngineComputedAssembly {
            delta: (
                self.final_base_style_record(old_style_record),
                self.final_base_style_record(new_style_record),
            ),
            canonicalized_groups,
            group_set_unchanged: group_set == old_record.groups,
        })
    }

    /// Put a node back on the record it held before an engine derivation C++ never installed,
    /// unless a publication has moved it on since.
    pub(super) fn revert_engine_computed_record(
        &mut self,
        node: StyleNodeID,
        derived_style_record: FinalStyleRecordID,
        previous_style_record: FinalStyleRecordID,
    ) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        let Some(current) = self.style_record_column.get(index).copied().flatten() else {
            return;
        };
        if self.final_base_style_record(current) != derived_style_record {
            return;
        }
        // A first record never installed leaves the node the way it was: unassigned.
        if previous_style_record == FinalStyleRecordID::NONE {
            self.remove(node);
            return;
        }
        let Some(previous_base) = previous_style_record.base_record() else {
            return;
        };
        let Some(record) = self.style_records.get_index(previous_base.index()).copied() else {
            return;
        };
        let groups = self.group_identities(record.groups);
        let inherited_identity = self
            .intern_inherited_group_set(&groups[..ENGINE_INHERITED_GROUP_COUNT])
            .0;
        self.columns.groups[index] = record.groups.0;
        self.columns.inherited_groups[index] = inherited_identity.0;
        self.style_record_column[index] = Some(previous_base);
    }

    fn intern_fixed_metadata(&mut self, metadata: ComputedFixedMetadata) -> ComputedFixedMetadataID {
        if let Some(identity) = self
            .computed_fixed_metadata
            .find(content_hash(metadata), |_identity, candidate| *candidate == metadata)
        {
            return identity;
        }
        let identity = self.computed_fixed_metadata.take_free_identity().unwrap_or_else(|| {
            ComputedFixedMetadataID(
                u32::try_from(self.computed_fixed_metadata.len())
                    .expect("computed fixed-metadata identity space exhausted"),
            )
        });
        self.computed_fixed_metadata
            .insert(content_hash(metadata), identity, metadata);
        identity
    }

    /// Assign a node the record another node of its cohort already derived this flush: the same
    /// old record moved to the same new winner state produces the same new record, so only the
    /// node's columns move.
    pub(super) fn assign_engine_computed_record(
        &mut self,
        node: StyleNodeID,
        old_style_record: FinalStyleRecordID,
        new_style_record: FinalStyleRecordID,
    ) -> Option<(FinalStyleRecordID, FinalStyleRecordID)> {
        let index = node.element_index()? as usize;
        if self.columns.animation_overlay_slot(index).is_some() {
            return None;
        }
        let current = *self.style_record_column.get(index)?.as_ref()?;
        if self.final_base_style_record(current) != old_style_record {
            return None;
        }
        let new_base_record = new_style_record.base_record()?;
        let new_record = *self.style_records.get_index(new_base_record.index())?;
        let swap_eligible = new_record
            .longhand_table
            .and_then(|table| self.computed_longhand_tables.get_index(table.index()))
            .is_some_and(|retained| table_inherited_group_swap_eligible(retained.table()));
        let groups = self.group_identities(new_record.groups);
        let inherited_identity = self
            .intern_inherited_group_set(&groups[..ENGINE_INHERITED_GROUP_COUNT])
            .0;
        self.columns.groups[index] = new_record.groups.0;
        self.columns.inherited_groups[index] = inherited_identity.0;
        self.columns.set_inherited_group_swap_eligible(index, swap_eligible);
        self.style_record_column[index] = Some(new_base_record);
        Some((old_style_record, new_style_record))
    }

    /// The identity of a record's inherited groups, the way a node assigned it would hold them.
    pub(super) fn style_record_inherited_groups_identity(&mut self, style_record: FinalStyleRecordID) -> Option<u32> {
        let base_record = style_record.base_record()?;
        let record = *self.style_records.get_index(base_record.index())?;
        let groups = self.group_identities(record.groups);
        Some(
            self.intern_inherited_group_set(&groups[..ENGINE_INHERITED_GROUP_COUNT])
                .0
                .0,
        )
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
        animated_overlay: Box<crate::css::animated_overlay::AnimatedOverlay>,
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
            animated_overlay,
            payloads: payloads.into(),
            pin_count: 0,
            is_assigned: true,
        }
    }

    fn allocate_animation_overlay(
        &mut self,
        base_style_record: StyleRecordID,
        source_identity: u64,
        animated_overlay: &mut Option<Box<crate::css::animated_overlay::AnimatedOverlay>>,
        payloads: &[*const c_void],
    ) -> (u32, FinalStyleRecordID, bool) {
        let record = self.make_animation_overlay_record(
            base_style_record,
            source_identity,
            animated_overlay
                .take()
                .expect("animation overlay properties are missing"),
            payloads,
        );
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
        animated_overlay: &mut Option<Box<crate::css::animated_overlay::AnimatedOverlay>>,
        payloads: &[*const c_void],
    ) -> AnimationOverlayPublication {
        if source_identity == 0 {
            if let Some(slot) = current_slot {
                self.release_animation_overlay_assignment(slot);
            }
            return AnimationOverlayPublication {
                slot: None,
                final_style_record: self.final_base_style_record(base_style_record),
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
                    animated_overlay
                        .take()
                        .expect("animation overlay properties are missing"),
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
            self.allocate_animation_overlay(base_style_record, source_identity, animated_overlay, payloads);
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
            || self.final_base_style_record(base_style_record),
            |slot| {
                self.animation_overlay_slots[slot as usize]
                    .as_ref()
                    .expect("animation-overlay slot is live")
                    .final_style_record
            },
        )
    }

    pub fn publish_animation_overlay(
        &mut self,
        target: ComputedStyleTarget,
        source_identity: u64,
        animated_overlay: *const crate::css::animated_overlay::AnimatedOverlay,
        payloads: &[*const c_void],
    ) -> Option<AnimationOverlayUpdate> {
        let (base_style_record, current_slot) = if target.is_pseudo() {
            let assignment = self.pseudo_row(target.node, target.pseudo_kind)?.assignment?;
            (assignment.style_record, assignment.animation_overlay_slot)
        } else {
            let index = target.node.element_index()? as usize;
            (
                *self.style_record_column.get(index)?.as_ref()?,
                self.columns.animation_overlay_slot(index),
            )
        };
        let previous_style_record = self.final_style_record(base_style_record, current_slot);
        let mut animated_overlay =
            (!animated_overlay.is_null()).then(|| Box::new(unsafe { &*animated_overlay }.clone()));
        let publication = self.update_animation_overlay(
            current_slot,
            base_style_record,
            source_identity,
            &mut animated_overlay,
            payloads,
        );
        if target.is_pseudo() {
            self.ensure_pseudo_row(target.node, target.pseudo_kind)
                .assignment
                .as_mut()?
                .animation_overlay_slot = publication.slot;
        } else {
            self.columns
                .set_animation_overlay_slot(target.node.element_index()? as usize, publication.slot);
        }
        Some(AnimationOverlayUpdate {
            previous_style_record,
            style_record: publication.final_style_record,
            slot_allocated: publication.slot_allocated,
            slot_released: publication.slot_released,
            record_updated: publication.record_updated,
            live_records: self.live_animation_overlay_assignments,
        })
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
            animated_overlay,
            animation_overlay_payloads,
            longhand_table,
        } = metadata_input;
        let mut animated_overlay = if animated_overlay.is_null() {
            None
        } else {
            Some(Box::new(unsafe { &*animated_overlay }.clone()))
        };
        let longhand_table = unsafe { longhand_table.as_ref() };
        let inherited_group_swap_eligible = dependency_flags & INHERITED_GROUP_SWAP_ELIGIBLE != 0;
        let dependency_flags = (dependency_flags & COMPUTED_VALUE_DEPENDENCY_FLAGS)
            | longhand_table.map_or(0, ComputedLonghandTable::dependency_flags);
        assert!(inherited_group_count <= payloads.len());
        let previous_group_set = target.and_then(|target| {
            let ComputedStyleTarget { node, pseudo_kind } = target;
            if target.is_pseudo() {
                self.pseudo_row(node, pseudo_kind)
                    .and_then(|row| row.assignment)
                    .map(|inputs| inputs.groups)
            } else {
                node.element_index()
                    .and_then(|index| self.columns.groups(index as usize))
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
            let previous_identity = previous_group_set
                .and_then(|set| self.sets[set].payloads.get(index).copied())
                .map(|payload| self.group_identity(index, payload));
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
                        let identity = self.groups.take_free_identity().unwrap_or_else(|| {
                            ComputedGroupID(
                                u32::try_from(self.groups.len()).expect("computed group identity space exhausted"),
                            )
                        });
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

        let (identity, new_group_set) = self.intern_group_set(&groups);

        let (inherited_identity, new_inherited_group_set) =
            self.intern_inherited_group_set(&groups[..inherited_group_count]);

        let custom_property_environment_hash = content_hash(custom_property_environment);
        let (custom_property_environment_identity, new_custom_property_environment) = match self
            .custom_property_environments
            .find(custom_property_environment_hash, |_identity, candidate| {
                *candidate == custom_property_environment
            }) {
            Some(identity) => (identity, false),
            None => {
                let identity = self
                    .custom_property_environments
                    .take_free_identity()
                    .unwrap_or_else(|| {
                        CustomPropertyEnvironmentID(
                            u32::try_from(self.custom_property_environments.len())
                                .expect("custom-property environment identity space exhausted"),
                        )
                    });
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
                let identity = self.computed_fixed_metadata.take_free_identity().unwrap_or_else(|| {
                    ComputedFixedMetadataID(
                        u32::try_from(self.computed_fixed_metadata.len())
                            .expect("computed fixed-metadata identity space exhausted"),
                    )
                });
                self.computed_fixed_metadata
                    .insert(content_hash(metadata), identity, metadata);
                (identity, true)
            }
        };

        // NB: A recompute allocates fresh value data for many longhands even when nothing changed,
        //     so interning by pointer content alone would mint a new table identity - and a new
        //     style record - on every recompute. The target's previous table is offered as a
        //     canonical candidate and reused when every slot holds an equal value, mirroring how
        //     output group payloads canonicalize against the previous group set above.
        let previous_longhand_table = target.and_then(|target| {
            let ComputedStyleTarget { node, pseudo_kind } = target;
            let style_record = if target.is_pseudo() {
                self.pseudo_row(node, pseudo_kind)
                    .and_then(|row| row.assignment)
                    .map(|inputs| inputs.style_record)
            } else {
                node.element_index()
                    .and_then(|index| self.style_record_column.get(index as usize))
                    .copied()
                    .flatten()
            }?;
            self.style_records.get_index(style_record.index())?.longhand_table
        });
        let canonical_longhand_table = self.sets[identity].canonical_longhand_table;
        let longhand_table_identity = longhand_table
            .map(|table| self.intern_longhand_table(table, previous_longhand_table, canonical_longhand_table));
        if self.sets[identity].canonical_longhand_table.is_none() {
            self.sets[identity].canonical_longhand_table = longhand_table_identity;
        }

        let style_record = StyleRecord {
            groups: identity,
            inherited_groups: inherited_identity,
            custom_properties: custom_property_environment_identity,
            fixed_metadata: computed_fixed_metadata_identity,
            longhand_table: longhand_table_identity,
        };
        let (style_record_identity, new_style_record) = self.intern_style_record(style_record);

        let is_pseudo = target.is_some_and(ComputedStyleTarget::is_pseudo);
        let (
            node_handle_changed,
            inherited_node_handle_changed,
            custom_property_environment_node_handle_changed,
            computed_fixed_metadata_node_handle_changed,
            previous_style_record_identity,
            animation_overlay_publication,
        ) = if let Some(ComputedStyleTarget { node, pseudo_kind }) = target.filter(|target| target.is_pseudo()) {
            let previous = self.pseudo_row(node, pseudo_kind).and_then(|row| row.assignment);
            let previous_style_record_identity = previous
                .map(|previous| self.final_style_record(previous.style_record, previous.animation_overlay_slot));
            let animation_overlay_publication = self.update_animation_overlay(
                previous.and_then(|previous| previous.animation_overlay_slot),
                style_record_identity,
                animation_overlay_identity,
                &mut animated_overlay,
                animation_overlay_payloads,
            );
            let row = self.ensure_pseudo_row(node, pseudo_kind);
            row.set_published(true);
            row.assignment = Some(PublishedComputedInputs {
                groups: identity,
                inherited_groups: inherited_identity,
                custom_properties: custom_property_environment_identity,
                fixed_metadata: computed_fixed_metadata_identity,
                style_record: style_record_identity,
                animation_overlay_slot: animation_overlay_publication.slot,
            });
            (
                previous.is_none_or(|previous| previous.groups != identity),
                previous.is_none_or(|previous| previous.inherited_groups != inherited_identity),
                previous.is_none_or(|previous| previous.custom_properties != custom_property_environment_identity),
                previous.is_none_or(|previous| previous.fixed_metadata != computed_fixed_metadata_identity),
                previous_style_record_identity,
                animation_overlay_publication,
            )
        } else if let Some(ComputedStyleTarget { node, .. }) = target {
            let index = node.element_index().expect("only elements publish computed groups") as usize;
            self.columns.ensure(index);
            if self.style_record_column.len() <= index {
                self.style_record_column.resize(index + 1, None);
            }
            let previous_style_record_identity = self.style_record_column[index]
                .map(|style_record| self.final_style_record(style_record, self.columns.animation_overlay_slot(index)));
            let animation_overlay_publication = self.update_animation_overlay(
                self.columns.animation_overlay_slot(index),
                style_record_identity,
                animation_overlay_identity,
                &mut animated_overlay,
                animation_overlay_payloads,
            );
            let changed = (
                self.columns.groups(index) != Some(identity),
                self.columns.inherited_groups(index) != Some(inherited_identity),
                self.columns.custom_properties(index) != Some(custom_property_environment_identity),
                self.columns.fixed_metadata(index) != Some(computed_fixed_metadata_identity),
                previous_style_record_identity,
                animation_overlay_publication,
            );
            self.columns.publish(
                index,
                PublishedComputedInputs {
                    groups: identity,
                    inherited_groups: inherited_identity,
                    custom_properties: custom_property_environment_identity,
                    fixed_metadata: computed_fixed_metadata_identity,
                    style_record: style_record_identity,
                    animation_overlay_slot: animation_overlay_publication.slot,
                },
                inherited_group_swap_eligible,
            );
            self.style_record_column[index] = Some(style_record_identity);
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
                None,
                AnimationOverlayPublication {
                    slot: None,
                    final_style_record: self.final_base_style_record(style_record_identity),
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
            new_style_record,
            node_handle_changed,
            inherited_node_handle_changed,
            custom_property_environment_node_handle_changed,
            computed_fixed_metadata_node_handle_changed,
            style_record_node_handle_changed,
            animation_overlay_slot_allocated: animation_overlay_publication.slot_allocated,
            animation_overlay_slot_released: animation_overlay_publication.slot_released,
            animation_overlay_record_updated: animation_overlay_publication.record_updated,
            live_animation_overlay_records: self.live_animation_overlay_assignments,
            is_pseudo,
        }
    }

    pub fn assign_shared_style_record(
        &mut self,
        target: ComputedStyleTarget,
        raw_style_record: u64,
        inherited_group_count: usize,
        inherited_group_swap_eligible: bool,
    ) -> Option<ComputedGroupPublication> {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        let requested_style_record_identity = final_style_record.base_record()?;
        assert!(
            self.style_record_generation_is_live(requested_style_record_identity, final_style_record.base_generation()),
            "base style-record is not live"
        );
        let previous_base_style_record_identity = if target.is_pseudo() {
            self.pseudo_row(target.node, target.pseudo_kind)
                .and_then(|row| row.assignment)
                .map(|assignment| assignment.style_record)
        } else {
            target
                .node
                .element_index()
                .and_then(|index| self.style_record_column.get(index as usize))
                .copied()
                .flatten()
        };
        // A regular publication canonicalizes every freshly produced group and longhand table
        // against the target's previous record. A shared record was canonicalized against its
        // donor instead, so preserve the target's identity when both records carry equal values.
        // This keeps a no-op recompute from looking like a layout-affecting style change.
        let style_record_identity = previous_base_style_record_identity
            .filter(|&previous| self.style_records_equal_by_value(previous, requested_style_record_identity))
            .unwrap_or(requested_style_record_identity);
        let record = *self.style_records.get_index(style_record_identity.index())?;
        // The inherited subset is immutable along with the complete style record. Sharing a
        // record must not reconstruct its group identities and intern the same subset again.
        let inherited_identity = record.inherited_groups;
        assert_eq!(self.inherited_sets[inherited_identity].len(), inherited_group_count);

        let is_pseudo = target.is_pseudo();
        let (
            node_handle_changed,
            inherited_node_handle_changed,
            custom_property_environment_node_handle_changed,
            computed_fixed_metadata_node_handle_changed,
            previous_style_record_identity,
            animation_overlay_publication,
        ) = if target.is_pseudo() {
            let previous = self
                .pseudo_row(target.node, target.pseudo_kind)
                .and_then(|row| row.assignment);
            let previous_style_record_identity = previous
                .map(|previous| self.final_style_record(previous.style_record, previous.animation_overlay_slot));
            let mut animated_overlay = None;
            let animation_overlay_publication = self.update_animation_overlay(
                previous.and_then(|previous| previous.animation_overlay_slot),
                style_record_identity,
                0,
                &mut animated_overlay,
                &[],
            );
            let row = self.ensure_pseudo_row(target.node, target.pseudo_kind);
            row.set_published(true);
            row.assignment = Some(PublishedComputedInputs {
                groups: record.groups,
                inherited_groups: inherited_identity,
                custom_properties: record.custom_properties,
                fixed_metadata: record.fixed_metadata,
                style_record: style_record_identity,
                animation_overlay_slot: None,
            });
            (
                previous.is_none_or(|previous| previous.groups != record.groups),
                previous.is_none_or(|previous| previous.inherited_groups != inherited_identity),
                previous.is_none_or(|previous| previous.custom_properties != record.custom_properties),
                previous.is_none_or(|previous| previous.fixed_metadata != record.fixed_metadata),
                previous_style_record_identity,
                animation_overlay_publication,
            )
        } else {
            let index = target
                .node
                .element_index()
                .expect("only elements publish computed groups") as usize;
            self.columns.ensure(index);
            if self.style_record_column.len() <= index {
                self.style_record_column.resize(index + 1, None);
            }
            let previous_style_record_identity = self.style_record_column[index]
                .map(|style_record| self.final_style_record(style_record, self.columns.animation_overlay_slot(index)));
            let mut animated_overlay = None;
            let animation_overlay_publication = self.update_animation_overlay(
                self.columns.animation_overlay_slot(index),
                style_record_identity,
                0,
                &mut animated_overlay,
                &[],
            );
            let changed = (
                self.columns.groups(index) != Some(record.groups),
                self.columns.inherited_groups(index) != Some(inherited_identity),
                self.columns.custom_properties(index) != Some(record.custom_properties),
                self.columns.fixed_metadata(index) != Some(record.fixed_metadata),
                previous_style_record_identity,
                animation_overlay_publication,
            );
            self.columns.publish(
                index,
                PublishedComputedInputs {
                    groups: record.groups,
                    inherited_groups: inherited_identity,
                    custom_properties: record.custom_properties,
                    fixed_metadata: record.fixed_metadata,
                    style_record: style_record_identity,
                    animation_overlay_slot: None,
                },
                inherited_group_swap_eligible,
            );
            self.style_record_column[index] = Some(style_record_identity);
            changed
        };

        Some(ComputedGroupPublication {
            previous_style_record_identity,
            style_record_identity: animation_overlay_publication.final_style_record,
            new_groups: 0,
            canonical_output_groups_reused: 0,
            new_group_set: false,
            new_inherited_group_set: false,
            new_custom_property_environment: false,
            new_computed_fixed_metadata: false,
            new_style_record: false,
            node_handle_changed,
            inherited_node_handle_changed,
            custom_property_environment_node_handle_changed,
            computed_fixed_metadata_node_handle_changed,
            style_record_node_handle_changed: previous_style_record_identity
                != Some(animation_overlay_publication.final_style_record),
            animation_overlay_slot_allocated: false,
            animation_overlay_slot_released: animation_overlay_publication.slot_released,
            animation_overlay_record_updated: false,
            live_animation_overlay_records: self.live_animation_overlay_assignments,
            is_pseudo,
        })
    }

    fn style_records_equal_by_value(&self, first: StyleRecordID, second: StyleRecordID) -> bool {
        if first == second {
            return true;
        }
        let Some(first) = self.style_records.get_index(first.index()) else {
            return false;
        };
        let Some(second) = self.style_records.get_index(second.index()) else {
            return false;
        };
        if first.custom_properties != second.custom_properties
            || first.fixed_metadata != second.fixed_metadata
            || self.inherited_sets[first.inherited_groups].len() != self.inherited_sets[second.inherited_groups].len()
        {
            return false;
        }
        let first_groups = &self.sets[first.groups].payloads;
        let second_groups = &self.sets[second.groups].payloads;
        if first_groups.len() != second_groups.len()
            || first_groups
                .iter()
                .zip(second_groups)
                .enumerate()
                .any(|(index, (&first, &second))| first != second && !style_group_payloads_equal(index, first, second))
        {
            return false;
        }
        match (first.longhand_table, second.longhand_table) {
            (None, None) => true,
            (Some(first), Some(second)) if first == second => true,
            (Some(first), Some(second)) => self.computed_longhand_tables[first]
                .table()
                .publication_equals(self.computed_longhand_tables[second].table()),
            _ => false,
        }
    }

    pub fn set_pending_cascade_state(&mut self, target: ComputedStyleTarget, state: (u64, CascadeStateID)) {
        if !target.is_pseudo() {
            self.pending_cascade_states.insert(target.node, state);
            return;
        }
        self.ensure_pseudo_row(target.node, target.pseudo_kind)
            .replace_cascade_state(PseudoComputedRow::PENDING_CASCADE, Some(state));
    }

    pub fn take_pending_cascade_state(&mut self, target: ComputedStyleTarget) -> Option<(u64, CascadeStateID)> {
        if !target.is_pseudo() {
            return self.pending_cascade_states.remove(&target.node);
        }
        let state = self
            .pseudo_row_mut(target.node, target.pseudo_kind)
            .and_then(|row| row.replace_cascade_state(PseudoComputedRow::PENDING_CASCADE, None));
        self.remove_empty_pseudo_row(target.node, target.pseudo_kind);
        state
    }

    #[must_use]
    pub fn cascade_state(&self, target: ComputedStyleTarget) -> Option<(u64, CascadeStateID)> {
        if target.is_pseudo() {
            return self
                .pseudo_row(target.node, target.pseudo_kind)?
                .cascade_state(PseudoComputedRow::CURRENT_CASCADE);
        }
        target
            .node
            .element_index()
            .and_then(|index| self.columns.cascade_state(index as usize))
    }

    pub fn pseudo_retained_cascade_states(
        &self,
        node: StyleNodeID,
    ) -> impl Iterator<Item = (u8, (u64, CascadeStateID))> + '_ {
        self.pseudo_rows(node).iter().filter_map(|row| {
            row.cascade_state(PseudoComputedRow::RETAINED_CASCADE)
                .map(|state| (row.kind, state))
        })
    }

    #[must_use]
    pub fn pseudo_retained_cascade_state(&self, node: StyleNodeID, pseudo_kind: u8) -> Option<(u64, CascadeStateID)> {
        self.pseudo_row(node, pseudo_kind)?
            .cascade_state(PseudoComputedRow::RETAINED_CASCADE)
    }

    /// Whether every inherited group the node holds outside `own_groups` is the parent's very
    /// group: the record was computed against this parent's inherited style, and nothing under
    /// the parent's change is left to inherit.
    #[must_use]
    pub fn inherited_groups_follow_parent(
        &self,
        node: StyleNodeID,
        parent: StyleNodeID,
        own_groups: u32,
    ) -> Option<bool> {
        let node_set = self.columns.groups(node.element_index()? as usize)?;
        let parent_set = self.columns.groups(parent.element_index()? as usize)?;
        if node_set == parent_set {
            return Some(true);
        }
        let node_groups = self.group_identities(node_set);
        let parent_groups = self.group_identities(parent_set);
        // C++ and Rust can intern equal payloads under different group identities. Inheritance
        // follows the parent's value in that case just as it does after group reclamation.
        Some((0..ENGINE_INHERITED_GROUP_COUNT).all(|group| {
            if own_groups & (1 << group) != 0 || node_groups[group] == parent_groups[group] {
                return true;
            }
            let node_payload = self.groups[node_groups[group]].payload;
            let parent_payload = self.groups[parent_groups[group]].payload;
            node_payload == parent_payload
                || crate::css::computed_values::style_group_payloads_equal(group, node_payload, parent_payload)
        }))
    }

    /// Whether a record's inherited groups, past the ones in `own_groups` that its own
    /// declarations rebuild, are the node's own inherited groups: what a record derived under the
    /// node as its parent must hold, whatever identities the two were keyed by when the record
    /// was derived.
    #[must_use]
    pub fn style_record_inherits_from_node(&self, raw_style_record: u64, node: StyleNodeID, own_groups: u32) -> bool {
        let Some(record) = FinalStyleRecordID(raw_style_record)
            .base_record()
            .and_then(|base| self.style_records.get_index(base.index()).copied())
        else {
            return false;
        };
        let Some(node_set) = node
            .element_index()
            .and_then(|index| self.columns.groups(index as usize))
        else {
            return false;
        };
        // Reclamation interns an equal payload under a new identity, so the groups compare by
        // content past their identities.
        let record_groups = self.group_identities(record.groups);
        let node_groups = self.group_identities(node_set);
        (0..ENGINE_INHERITED_GROUP_COUNT).all(|group| {
            if own_groups & (1 << group) != 0 {
                return true;
            }
            let record_payload = self.groups[record_groups[group]].payload;
            let node_payload = self.groups[node_groups[group]].payload;
            record_payload == node_payload
                || crate::css::computed_values::style_group_payloads_equal(group, record_payload, node_payload)
        })
    }

    /// The record a pseudo-element of the node holds, with its animation overlay if any.
    #[must_use]
    pub fn pseudo_style_record(&self, node: StyleNodeID, pseudo_kind: u8) -> Option<FinalStyleRecordID> {
        let assignment = self.pseudo_row(node, pseudo_kind)?.assignment.as_ref()?;
        Some(self.final_style_record(assignment.style_record, assignment.animation_overlay_slot))
    }

    /// The pseudo-element kinds this node holds published computed styles for.
    pub fn assigned_pseudo_kinds(&self, node: StyleNodeID) -> impl Iterator<Item = u8> + '_ {
        self.pseudo_rows(node)
            .iter()
            .filter_map(|row| row.is_published().then_some(row.kind))
    }

    #[cfg(test)]
    pub fn record_pseudo_kind_for_test(&mut self, node: StyleNodeID, pseudo_kind: u8) {
        self.ensure_pseudo_row(node, pseudo_kind).set_published(true);
    }

    fn longhand_table_for_target(&self, target: ComputedStyleTarget) -> Option<&ComputedLonghandTable> {
        let style_record = if target.is_pseudo() {
            self.pseudo_row(target.node, target.pseudo_kind)
                .and_then(|row| row.assignment)
                .map(|assignment| assignment.style_record)
        } else {
            target
                .node
                .element_index()
                .and_then(|index| self.style_record_column.get(index as usize))
                .copied()
                .flatten()
        }?;
        let table = self.style_records.get_index(style_record.index())?.longhand_table?;
        Some(self.computed_longhand_tables[table].table())
    }

    fn specified_value_dependency_mask(
        &self,
        target: ComputedStyleTarget,
        depends_on_input: impl Fn(&RetainedStyleValueData) -> bool,
    ) -> Option<u32> {
        let mask = self
            .longhand_table_for_target(target)?
            .retained_inheritance_dependent_values()
            .filter(|(_, value)| depends_on_input(value))
            // Logical aliases retain their specified values but own no output payload. Their
            // resolved physical longhands are recorded separately and name the actual group.
            .filter_map(|(property, _)| computed_group_output_mask(property))
            .fold(0, |mask, groups| mask | groups);
        Some(mask)
    }

    fn specified_value_dependency_properties(
        &self,
        target: ComputedStyleTarget,
        depends_on_input: impl Fn(&RetainedStyleValueData) -> bool,
    ) -> Option<[u64; 6]> {
        let mut properties = [0u64; 6];
        for (property, value) in self
            .longhand_table_for_target(target)?
            .retained_inheritance_dependent_values()
        {
            if !depends_on_input(value) {
                continue;
            }
            let index = usize::from(property.checked_sub(crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID)?);
            if index >= crate::css::property_metadata::NUMBER_OF_LONGHAND_PROPERTIES {
                return None;
            }
            properties[index / 64] |= 1 << (index % 64);
        }
        Some(properties)
    }

    /// The groups whose previous specified values actually read `currentColor` while computing.
    /// Missing dependency coverage stays typed so a color winner change can widen safely.
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

    /// Whether a raw final record identity still names a live base record.
    pub fn final_style_record_is_live(&self, raw_style_record: u64) -> bool {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        final_style_record
            .base_record()
            .is_some_and(|record| self.style_record_generation_is_live(record, final_style_record.base_generation()))
    }

    /// The raw custom-property environment identity behind a node's record.
    pub fn custom_property_environment_identity(&self, node: StyleNodeID) -> Option<u64> {
        let index = node.element_index()? as usize;
        let identity = self.columns.custom_properties(index)?;
        Some(self.custom_property_environments[identity])
    }

    /// The raw custom-property environment identity a record was published with.
    pub fn style_record_custom_property_environment(&self, raw_style_record: u64) -> Option<u64> {
        let record = self
            .style_records
            .get_index(FinalStyleRecordID(raw_style_record).base_record()?.index())?;
        Some(self.custom_property_environments[record.custom_properties])
    }

    pub fn set_adjustment_facts(&mut self, node: StyleNodeID, facts: u32) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        self.columns.ensure(index);
        self.columns.adjustment_facts[index] = facts;
    }

    /// The identity of the inherited groups a node's published style carries.
    pub(super) fn node_inherited_groups_identity(&self, node: StyleNodeID) -> Option<u32> {
        let index = node.element_index()? as usize;
        self.columns.inherited_groups(index).map(|identity| identity.0)
    }

    /// Whether the node's assignment may take an inherited-group swap.
    /// Whether the node's last published match answer declared past its winners (custom
    /// properties, `all`): a record computed from it is no function of a winner state.
    pub(super) fn node_answer_is_incomplete(&self, node: StyleNodeID) -> bool {
        node.element_index()
            .is_some_and(|index| self.columns.answer_is_incomplete(index as usize))
    }

    pub(super) fn set_node_answer_incomplete(&mut self, node: StyleNodeID, incomplete: bool) {
        if let Some(index) = node.element_index() {
            self.columns.set_answer_incomplete(index as usize, incomplete);
        }
    }

    pub(super) fn node_has_animation_overlay(&self, node: StyleNodeID) -> bool {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return false;
        };
        self.columns.animation_overlay_slot(index).is_some()
            || self
                .assigned_style_record(node)
                .and_then(|record| self.style_record_view(record.raw()))
                .is_some_and(|view| !view.animated_overlay.is_null())
    }

    pub(super) fn node_inherited_group_swap_eligible(&self, node: StyleNodeID) -> bool {
        node.element_index()
            .is_some_and(|index| self.columns.inherited_group_swap_eligible(index as usize))
    }

    pub fn adjustment_facts(&self, node: StyleNodeID) -> u32 {
        node.element_index()
            .and_then(|index| self.columns.adjustment_facts.get(index as usize))
            .copied()
            .unwrap_or(0)
    }

    pub fn bind_cascade_state(
        &mut self,
        target: ComputedStyleTarget,
        cascade_state: (u64, CascadeStateID),
    ) -> Option<(u64, CascadeStateID)> {
        if target.is_pseudo() {
            let row = self.ensure_pseudo_row(target.node, target.pseudo_kind);
            assert!(
                row.assignment.is_some(),
                "a pseudo style must be published before its cascade state is bound"
            );
            return row.replace_cascade_state(PseudoComputedRow::CURRENT_CASCADE, Some(cascade_state));
        }
        let index = target
            .node
            .element_index()
            .expect("only elements publish computed groups") as usize;
        debug_assert!(index < self.columns.flags.len());
        self.columns.replace_cascade_state(index, Some(cascade_state))
    }

    /// Forget the exact cascade state behind a style published through a path that did not run the
    /// cascade, so a later comparison cannot use the state behind an older publication.
    pub fn clear_cascade_state(&mut self, target: ComputedStyleTarget) {
        if target.is_pseudo() {
            if let Some(row) = self.pseudo_row_mut(target.node, target.pseudo_kind) {
                row.replace_cascade_state(PseudoComputedRow::CURRENT_CASCADE, None);
                row.replace_cascade_state(PseudoComputedRow::RETAINED_CASCADE, None);
            }
            self.remove_empty_pseudo_row(target.node, target.pseudo_kind);
            return;
        }
        let Some(index) = target.node.element_index().map(|index| index as usize) else {
            return;
        };
        self.columns.replace_cascade_state(index, None);
    }

    pub fn remove(&mut self, node: StyleNodeID) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        if let Some(slot) = self.style_record_column.get_mut(index) {
            *slot = None;
        }
        if let Some(slot) = self.columns.remove(index) {
            self.release_animation_overlay_assignment(slot);
        }
        self.pending_cascade_states.remove(&node);
        if let Some(rows) = self.pseudo_rows_by_node.remove(&node) {
            self.pseudo_assignment_nested_memory
                .shrink_committed(size_of_val(rows.as_ref()) as u64);
            for row in rows.into_vec() {
                if let Some(slot) = row.assignment.and_then(|assignment| assignment.animation_overlay_slot) {
                    self.release_animation_overlay_assignment(slot);
                }
            }
        }
    }

    pub fn remove_pseudo(&mut self, node: StyleNodeID, pseudo_kind: u8) -> Option<FinalStyleRecordID> {
        let row = self.pseudo_row_mut(node, pseudo_kind)?;
        let removed = row.assignment.take()?;
        row.set_published(false);
        row.replace_cascade_state(PseudoComputedRow::PENDING_CASCADE, None);
        let final_style_record = self.final_style_record(removed.style_record, removed.animation_overlay_slot);
        if let Some(slot) = removed.animation_overlay_slot {
            self.release_animation_overlay_assignment(slot);
        }
        self.remove_empty_pseudo_row(node, pseudo_kind);
        Some(final_style_record)
    }

    pub fn observe_absent_pseudo_cascade_state(&mut self, target: ComputedStyleTarget, state: (u64, CascadeStateID)) {
        debug_assert!(target.is_pseudo());
        self.ensure_pseudo_row(target.node, target.pseudo_kind)
            .replace_cascade_state(PseudoComputedRow::CURRENT_CASCADE, Some(state));
    }

    pub fn observe_pseudo_retained_cascade_state(
        &mut self,
        target: ComputedStyleTarget,
        state: Option<(u64, CascadeStateID)>,
    ) {
        debug_assert!(target.is_pseudo());
        self.ensure_pseudo_row(target.node, target.pseudo_kind)
            .replace_cascade_state(PseudoComputedRow::RETAINED_CASCADE, state);
        self.remove_empty_pseudo_row(target.node, target.pseudo_kind);
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.groups,
                self.sets,
                self.columns.groups,
                self.inherited_sets,
                self.columns.inherited_groups,
            ];
            cached [self.group_set_nested_memory.bytes()];
            nested [];
            skip [];
        }
    }

    #[must_use]
    pub fn custom_property_environment_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.custom_property_environments, self.columns.custom_properties, self.columns.adjustment_facts];
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
                self.columns.fixed_metadata,
            ];
            cached [];
            nested [];
            skip [];
        }
    }

    #[must_use]
    pub fn computed_longhand_table_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.computed_longhand_tables];
            cached [self.longhand_table_nested_memory.bytes()];
            nested [];
            skip [];
        }
    }

    #[must_use]
    pub fn style_record_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.style_records,
                self.style_record_liveness,
                self.style_record_generations,
                self.style_record_column,
                self.base_style_record_pins,
                self.columns.cascade_versions,
                self.columns.cascade_states,
                self.columns.flags,
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
                self.columns.animation_overlay_slots,
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

    fn reachability(&self) -> ComputedReachability {
        let mut reachable = ComputedReachability {
            groups: vec![false; self.groups.len()],
            sets: vec![false; self.sets.len()],
            inherited_sets: vec![false; self.inherited_sets.len()],
            custom_property_environments: vec![false; self.custom_property_environments.len()],
            fixed_metadata: vec![false; self.computed_fixed_metadata.len()],
            longhand_tables: vec![false; self.computed_longhand_tables.len()],
            style_records: vec![false; self.style_records.len()],
        };

        {
            let mut mark_style_record = |identity: StyleRecordID| {
                ComputedReachability::mark(&mut reachable.style_records, identity);
                let record = self.style_records.get(identity);
                ComputedReachability::mark(&mut reachable.sets, record.groups);
                ComputedReachability::mark(&mut reachable.inherited_sets, record.inherited_groups);
                ComputedReachability::mark(&mut reachable.custom_property_environments, record.custom_properties);
                ComputedReachability::mark(&mut reachable.fixed_metadata, record.fixed_metadata);
                if let Some(longhand_table) = record.longhand_table {
                    ComputedReachability::mark(&mut reachable.longhand_tables, longhand_table);
                }
            };
            for &identity in self.style_record_column.iter().flatten() {
                mark_style_record(identity);
            }
            for rows in self.pseudo_rows_by_node.values() {
                for assignment in rows.iter().filter_map(|row| row.assignment) {
                    mark_style_record(assignment.style_record);
                }
            }
            for overlay in self.animation_overlay_slots.iter().flatten() {
                mark_style_record(overlay.base_style_record);
            }
            for &identity in self.base_style_record_pins.keys() {
                mark_style_record(identity);
            }
        }

        for index in 0..self.columns.flags.len() {
            if !self.columns.is_assigned(index) {
                continue;
            }
            ComputedReachability::mark(&mut reachable.sets, self.columns.groups(index).unwrap());
            ComputedReachability::mark(
                &mut reachable.inherited_sets,
                self.columns.inherited_groups(index).unwrap(),
            );
            ComputedReachability::mark(
                &mut reachable.custom_property_environments,
                self.columns.custom_properties(index).unwrap(),
            );
            ComputedReachability::mark(
                &mut reachable.fixed_metadata,
                self.columns.fixed_metadata(index).unwrap(),
            );
        }
        for rows in self.pseudo_rows_by_node.values() {
            for assignment in rows.iter().filter_map(|row| row.assignment) {
                ComputedReachability::mark(&mut reachable.sets, assignment.groups);
                ComputedReachability::mark(&mut reachable.inherited_sets, assignment.inherited_groups);
                ComputedReachability::mark(
                    &mut reachable.custom_property_environments,
                    assignment.custom_properties,
                );
                ComputedReachability::mark(&mut reachable.fixed_metadata, assignment.fixed_metadata);
            }
        }
        for (index, is_reachable) in reachable.sets.iter().copied().enumerate() {
            if !is_reachable {
                continue;
            }
            for (group_index, &payload) in self.sets[index].payloads.iter().enumerate() {
                ComputedReachability::mark(&mut reachable.groups, self.group_identity(group_index, payload));
            }
        }
        for (index, is_reachable) in reachable.inherited_sets.iter().copied().enumerate() {
            if !is_reachable {
                continue;
            }
            for &group in &self.inherited_sets[index] {
                ComputedReachability::mark(&mut reachable.groups, group);
            }
        }
        reachable
    }

    pub(super) fn reclaim_unreachable(&mut self) -> ComputedGroupRetention {
        let reachable = self.reachability();
        let retention = ComputedGroupRetention {
            retained: self.groups.live_identities().count(),
            reachable: reachable.groups.iter().filter(|&&reachable| reachable).count(),
        };
        if replaying_style_groups() {
            return retention;
        }

        let mut unreachable_style_records = self
            .style_records
            .live_identities()
            .filter(|identity| !reachable.style_records[identity.index()])
            .collect::<Vec<_>>();
        unreachable_style_records.sort_unstable_by_key(|identity| std::cmp::Reverse(identity.index()));
        for identity in unreachable_style_records {
            let record = *self.style_records.get(identity);
            if self.style_record_generations[identity.index()] == FinalStyleRecordID::MAX_BASE_GENERATION {
                self.style_records.remove_identity(content_hash(record), identity);
            } else {
                self.style_records.retire_identity(content_hash(record), identity);
            }
            let (changed, _) = self.style_record_liveness.set(identity.index(), false);
            assert!(changed, "retired base style-record identity must be live");
        }
        for identity in self.sets.live_identities().collect::<Vec<_>>() {
            if reachable.sets[identity.index()] {
                if let Some(longhand_table) = self.sets[identity].canonical_longhand_table
                    && !reachable.longhand_tables[longhand_table.index()]
                {
                    self.sets[identity].canonical_longhand_table = None;
                }
                continue;
            }
            let set = std::mem::replace(
                self.sets.get_mut(identity),
                ComputedGroupSet {
                    identity_hash: 0,
                    payloads: Box::default(),
                    canonical_longhand_table: None,
                },
            );
            self.group_set_nested_memory
                .shrink_committed(size_of_val(set.payloads.as_ref()) as u64);
            self.sets.retire_identity(set.identity_hash, identity);
        }
        for identity in self.inherited_sets.live_identities().collect::<Vec<_>>() {
            if reachable.inherited_sets[identity.index()] {
                continue;
            }
            let groups = std::mem::take(self.inherited_sets.get_mut(identity));
            self.group_set_nested_memory
                .shrink_committed(size_of_val(groups.as_ref()) as u64);
            self.inherited_sets.retire_identity(content_hash(&groups), identity);
        }
        for identity in self.custom_property_environments.live_identities().collect::<Vec<_>>() {
            if reachable.custom_property_environments[identity.index()] {
                continue;
            }
            let environment = *self.custom_property_environments.get(identity);
            self.custom_property_environments
                .retire_identity(content_hash(environment), identity);
        }
        for identity in self.computed_fixed_metadata.live_identities().collect::<Vec<_>>() {
            if reachable.fixed_metadata[identity.index()] {
                continue;
            }
            let metadata = *self.computed_fixed_metadata.get(identity);
            self.computed_fixed_metadata
                .retire_identity(content_hash(metadata), identity);
        }
        for identity in self.computed_longhand_tables.live_identities().collect::<Vec<_>>() {
            if reachable.longhand_tables[identity.index()] {
                continue;
            }
            let retained = &self.computed_longhand_tables[identity];
            let hash = longhand_table_hash_with_slot_hash_sum(retained.table(), retained.slot_hash_sum);
            let table = std::mem::replace(
                self.computed_longhand_tables.get_mut(identity),
                RetainedLonghandTable {
                    table: std::ptr::null(),
                    slot_hash_sum: 0,
                },
            );
            self.longhand_table_nested_memory
                .shrink_committed(size_of_val(table.value_view()) as u64);
            self.computed_longhand_tables.retire_identity(hash, identity);
        }
        for identity in self.groups.live_identities().collect::<Vec<_>>() {
            if reachable.groups[identity.index()] {
                continue;
            }
            let group = std::mem::replace(
                self.groups.get_mut(identity),
                ComputedGroup {
                    index: usize::MAX,
                    payload: std::ptr::null(),
                },
            );
            self.groups
                .retire_identity(content_hash((group.index, group.payload as usize)), identity);
            self.group_set_nested_memory
                .shrink_committed(retained_group_payload_bytes(group.index, group.payload) as u64);
            release_group_payload(group.index, group.payload);
        }
        retention
    }

    pub(super) fn reclaim_unreachable_if_needed(&mut self) -> Option<ComputedGroupRetention> {
        if self.style_record_view_epoch_depth != 0 {
            return None;
        }
        if self.style_records_interned_since_reclamation < self.next_reclamation_after {
            return None;
        }
        self.style_records_interned_since_reclamation = 0;
        let retention = self.reclaim_unreachable();
        self.next_reclamation_after = self.style_records.live_identities().count().max(1024);
        Some(retention)
    }

    pub fn style_record_payloads(&self, raw_style_record: u64) -> Option<&[*const c_void]> {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        if raw_style_record & FinalStyleRecordID::ANIMATION_OVERLAY_TAG != 0 {
            let style_record = final_style_record;
            let slot = *self.animation_overlay_slots_by_record.get(&style_record)?;
            let record = self.animation_overlay_slots[slot as usize].as_ref()?;
            return (!record.payloads.is_empty()).then_some(record.payloads.as_ref());
        }
        let style_record = final_style_record.base_record()?;
        assert!(
            self.style_record_generation_is_live(style_record, final_style_record.base_generation()),
            "base style-record is not live"
        );
        let record = self.style_records.get_index(style_record.index())?;
        Some(&self.sets[record.groups].payloads)
    }

    pub fn style_record_dependency_flags(&self, raw_style_record: u64) -> Option<u8> {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        let base_style_record = if let Some(style_record) = final_style_record.base_record() {
            assert!(
                self.style_record_generation_is_live(style_record, final_style_record.base_generation()),
                "base style-record is not live"
            );
            style_record
        } else {
            let slot = *self.animation_overlay_slots_by_record.get(&final_style_record)?;
            self.animation_overlay_slots[slot as usize].as_ref()?.base_style_record
        };
        assert!(
            self.style_record_is_live(base_style_record),
            "base style-record is not live"
        );
        let record = self.style_records.get_index(base_style_record.index())?;
        Some(self.computed_fixed_metadata.get(record.fixed_metadata).dependency_flags)
    }

    #[cfg(feature = "style-recording")]
    pub(crate) fn recording_group_identities(&self, raw_style_record: u64) -> Option<Vec<u32>> {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        let base_style_record = match final_style_record.base_record() {
            Some(style_record) => {
                assert!(
                    self.style_record_generation_is_live(style_record, final_style_record.base_generation()),
                    "base style-record is not live"
                );
                style_record
            }
            None => {
                let slot = *self.animation_overlay_slots_by_record.get(&final_style_record)?;
                self.animation_overlay_slots[slot as usize].as_ref()?.base_style_record
            }
        };
        assert!(
            self.style_record_is_live(base_style_record),
            "base style-record is not live"
        );
        let record = self.style_records.get_index(base_style_record.index())?;
        Some(
            self.group_identities(record.groups)
                .into_iter()
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

    #[cfg(feature = "style-recording")]
    pub(crate) fn recording_longhand_table(&self, raw_style_record: u64) -> Option<(u32, &[*const c_void])> {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        let base_style_record = match final_style_record.base_record() {
            Some(style_record) => {
                assert!(
                    self.style_record_generation_is_live(style_record, final_style_record.base_generation()),
                    "base style-record is not live"
                );
                style_record
            }
            None => {
                let slot = *self.animation_overlay_slots_by_record.get(&final_style_record)?;
                self.animation_overlay_slots[slot as usize].as_ref()?.base_style_record
            }
        };
        assert!(
            self.style_record_is_live(base_style_record),
            "base style-record is not live"
        );
        let identity = self
            .style_records
            .get_index(base_style_record.index())?
            .longhand_table?;
        Some((
            identity.0,
            self.computed_longhand_tables
                .get_index(identity.0 as usize)?
                .value_view(),
        ))
    }

    pub(crate) fn style_record_view(&self, raw_style_record: u64) -> Option<StyleRecordView<'_>> {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        let (base_style_record, payloads, animation_overlay_identity, animated_overlay) =
            if let Some(style_record) = final_style_record.base_record() {
                assert!(
                    self.style_record_generation_is_live(style_record, final_style_record.base_generation()),
                    "base style-record is not live"
                );
                let record = self.style_records.get_index(style_record.index())?;
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
                    std::ptr::from_ref(overlay.animated_overlay.as_ref()),
                )
            };
        assert!(
            self.style_record_is_live(base_style_record),
            "base style-record is not live"
        );
        let record = self.style_records.get_index(base_style_record.index())?;
        let base_payloads = self.sets[record.groups].payloads.as_ref();
        let fixed_metadata = self
            .computed_fixed_metadata
            .get_index(record.fixed_metadata.0 as usize)?;
        let retained_longhand_table = record
            .longhand_table
            .and_then(|identity| self.computed_longhand_tables.get_index(identity.0 as usize));
        let longhand_table = retained_longhand_table.map_or(std::ptr::null(), |table| table.table);
        let longhand_values = retained_longhand_table.map_or(&[][..], RetainedLonghandTable::value_view);
        Some(StyleRecordView {
            payloads,
            base_payloads,
            longhand_table,
            longhand_values,
            animated_overlay,
            pseudo_element_styles: fixed_metadata.pseudo_element_styles,
            counter_style_environment_identity: fixed_metadata.counter_style_environment_identity,
            animation_overlay_identity,
            dependency_flags: fixed_metadata.dependency_flags,
        })
    }

    pub fn pin_style_record(&mut self, raw_style_record: u64) {
        let final_style_record = FinalStyleRecordID(raw_style_record);
        if let Some(style_record) = final_style_record.base_record() {
            assert!(
                self.style_record_generation_is_live(style_record, final_style_record.base_generation()),
                "base style-record is not live"
            );
            let pin_count = self.base_style_record_pins.entry(style_record).or_default();
            *pin_count = pin_count.checked_add(1).expect("base style-record pin count overflow");
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
        if let Some(style_record) = final_style_record.base_record() {
            assert!(
                self.style_record_generation_is_live(style_record, final_style_record.base_generation()),
                "base style-record is not live"
            );
            let pin_count = self
                .base_style_record_pins
                .get_mut(&style_record)
                .expect("base style-record is pinned");
            *pin_count = pin_count.checked_sub(1).expect("base style-record is pinned");
            if *pin_count == 0 {
                self.base_style_record_pins.remove(&style_record);
            }
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
            shallow [self.pseudo_rows_by_node];
            cached [self.pseudo_assignment_nested_memory.bytes()];
            nested [];
            skip [];
        }
    }

    pub fn settle_nested_memory(&mut self, memory: &mut MemoryController) {
        let group_set = self.group_set_nested_memory.bytes();
        self.group_set_nested_memory.reconcile_committed(memory, group_set);
        let longhand_tables = self.longhand_table_nested_memory.bytes();
        self.longhand_table_nested_memory
            .reconcile_committed(memory, longhand_tables);
        let animation_overlay = self.animation_overlay_nested_memory.bytes();
        self.animation_overlay_nested_memory
            .reconcile_committed(memory, animation_overlay);
        let pseudo_assignment = self.pseudo_assignment_nested_memory.bytes();
        self.pseudo_assignment_nested_memory
            .reconcile_committed(memory, pseudo_assignment);
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
    pub fn longhand_table_header_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [
                self.computed_longhand_table_capacity_bytes() - self.longhand_table_nested_memory.bytes(),
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
        for identity in self.groups.live_identities() {
            let group = self.groups.get(identity);
            release_group_payload(group.index, group.payload);
        }
    }
}

fn content_hash(content: impl Hash) -> u64 {
    let mut hasher = fast_hasher();
    content.hash(&mut hasher);
    hasher.finish()
}

/// One slot's contribution to a table's slot hash sum: the value's content hash mixed with the
/// slot, so the sum is order-sensitive while any slot's share can be subtracted and replaced.
fn longhand_slot_hash(slot: usize, value: *const c_void) -> u64 {
    let content = unsafe { crate::css::style_value::style_value_content_hash(value.cast()) };
    (content ^ (slot as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15))
        .wrapping_mul(0x2545_F491_4F6C_DD1D)
        .rotate_left((slot as u32) & 63)
}

fn longhand_table_slot_hash_sum(table: &ComputedLonghandTable) -> u64 {
    table
        .value_pointers()
        .iter()
        .enumerate()
        .fold(0_u64, |sum, (slot, &value)| {
            sum.wrapping_add(longhand_slot_hash(slot, value))
        })
}

#[cfg(test)]
fn longhand_table_hash(table: &ComputedLonghandTable) -> u64 {
    longhand_table_hash_with_slot_hash_sum(table, longhand_table_slot_hash_sum(table))
}

fn longhand_table_hash_with_slot_hash_sum(table: &ComputedLonghandTable, slot_hash_sum: u64) -> u64 {
    let mut hasher = fast_hasher();
    slot_hash_sum.hash(&mut hasher);
    table.evaluated_bits().hash(&mut hasher);
    table.importance_bits().hash(&mut hasher);
    table.inheritance_bits().hash(&mut hasher);
    table.publication_sidecars().hash(&mut hasher);
    table.publication_dependency_flags().hash(&mut hasher);
    table.pseudo_element_styles().hash(&mut hasher);
    unsafe { crate::css::style_value::style_value_content_hash(table.raw_cascaded_font_size().cast()) }
        .hash(&mut hasher);
    let mut inheritance_dependent = table.inheritance_dependent_values().collect::<Vec<_>>();
    inheritance_dependent.sort_unstable_by_key(|(property, _)| *property);
    for (property, value) in inheritance_dependent {
        property.hash(&mut hasher);
        unsafe { crate::css::style_value::style_value_content_hash(value.cast()) }.hash(&mut hasher);
    }
    hasher.finish()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn metadata(
        pseudo_element_styles: u64,
        dependency_flags: u8,
        counter_style_environment_identity: u64,
    ) -> ComputedMetadataInput<'static> {
        ComputedMetadataInput {
            pseudo_element_styles,
            dependency_flags,
            counter_style_environment_identity,
            animation_overlay_identity: 0,
            animated_overlay: std::ptr::null(),
            animation_overlay_payloads: &[],
            longhand_table: std::ptr::null(),
        }
    }

    #[test]
    fn viewport_dependent_nodes_include_elements_and_pseudo_owners_once() {
        let mut sets = ComputedGroupSets::default();
        let independent = ComputedStyleTarget::new(StyleNodeID::element(1), u8::MAX);
        let viewport_dependent = ComputedStyleTarget::new(StyleNodeID::element(2), u8::MAX);
        let font_dependent_pseudo = ComputedStyleTarget::new(StyleNodeID::element(3), 1);
        let viewport_dependent_pseudo = ComputedStyleTarget::new(StyleNodeID::element(2), 2);
        let viewport_dependent_pseudo_only = ComputedStyleTarget::new(StyleNodeID::element(4), 1);
        sets.publish(Some(independent), &[], 0, 0, metadata(0, 0, 0));
        sets.publish(Some(viewport_dependent), &[], 0, 0, metadata(0, 1, 0));
        sets.publish(Some(font_dependent_pseudo), &[], 0, 0, metadata(0, 2, 0));
        sets.publish(Some(viewport_dependent_pseudo), &[], 0, 0, metadata(0, 1, 0));
        sets.publish(Some(viewport_dependent_pseudo_only), &[], 0, 0, metadata(0, 1, 0));

        assert_eq!(sets.viewport_dependent_nodes(), vec![2, 4]);
    }

    #[test]
    fn equal_longhand_tables_have_equal_hashes() {
        let mut first = ComputedLonghandTable::new();
        let mut second = ComputedLonghandTable::new();
        for table in [&mut first, &mut second] {
            table.set(
                crate::css::property_metadata::property_id::OPACITY,
                crate::css::style_value::RetainedStyleValueData::from_owned(
                    crate::css::style_value::StyleValueData::Number { value: 0.5 },
                ),
                -1,
            );
        }
        first.append_drive_inheritance_dependent_value(
            crate::css::property_metadata::property_id::COLOR,
            crate::css::style_value::RetainedStyleValueData::from_owned(
                crate::css::style_value::StyleValueData::Keyword {
                    keyword: crate::css::style_compute::keyword::CURRENTCOLOR,
                },
            ),
        );
        first.append_drive_inheritance_dependent_value(
            crate::css::property_metadata::property_id::BACKGROUND_COLOR,
            crate::css::style_value::RetainedStyleValueData::from_owned(
                crate::css::style_value::StyleValueData::Keyword {
                    keyword: crate::css::style_compute::keyword::CURRENTCOLOR,
                },
            ),
        );
        second.append_drive_inheritance_dependent_value(
            crate::css::property_metadata::property_id::BACKGROUND_COLOR,
            crate::css::style_value::RetainedStyleValueData::from_owned(
                crate::css::style_value::StyleValueData::Keyword {
                    keyword: crate::css::style_compute::keyword::CURRENTCOLOR,
                },
            ),
        );
        second.append_drive_inheritance_dependent_value(
            crate::css::property_metadata::property_id::COLOR,
            crate::css::style_value::RetainedStyleValueData::from_owned(
                crate::css::style_value::StyleValueData::Keyword {
                    keyword: crate::css::style_compute::keyword::CURRENTCOLOR,
                },
            ),
        );
        first.finish_drive_inheritance_dependent_values();
        second.finish_drive_inheritance_dependent_values();

        assert!(first.publication_equals(&second));
        assert_eq!(longhand_table_hash(&first), longhand_table_hash(&second));
    }

    #[test]
    fn empty_group_sets_share_one_identity_and_publish_node_handles() {
        let mut sets = ComputedGroupSets::default();
        let first_target = ComputedStyleTarget::new(StyleNodeID::from_raw(1).unwrap(), u8::MAX);
        let second_target = ComputedStyleTarget::new(StyleNodeID::from_raw(65).unwrap(), u8::MAX);
        let first = sets.publish(Some(first_target), &[], 0, 17, metadata(3, 1, 5));
        let second = sets.publish(Some(second_target), &[], 0, 17, metadata(3, 1, 5));
        let unchanged = sets.publish(Some(first_target), &[], 0, 17, metadata(3, 1, 5));

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
        assert!(first.new_custom_property_environment);
        assert!(!second.new_custom_property_environment);
        assert!(first.new_computed_fixed_metadata);
        assert!(!second.new_computed_fixed_metadata);
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

        let changed_environment = sets.publish(Some(first_target), &[], 0, 18, metadata(3, 1, 5));
        assert!(changed_environment.new_custom_property_environment);
        assert!(changed_environment.custom_property_environment_node_handle_changed);
        assert!(changed_environment.new_style_record);
        assert_eq!(
            changed_environment.previous_style_record_identity,
            Some(first.style_record_identity)
        );
        assert!(changed_environment.style_record_node_handle_changed);
        assert!(!changed_environment.node_handle_changed);

        let changed_metadata = sets.publish(Some(first_target), &[], 0, 18, metadata(7, 1, 5));
        assert!(changed_metadata.new_computed_fixed_metadata);
        assert!(changed_metadata.computed_fixed_metadata_node_handle_changed);
        assert!(changed_metadata.new_style_record);
        assert!(changed_metadata.style_record_node_handle_changed);
        assert!(!changed_metadata.node_handle_changed);

        let changed_observer_dependency = sets.publish(Some(first_target), &[], 0, 18, metadata(7, 1, 6));
        assert!(changed_observer_dependency.new_computed_fixed_metadata);
        assert!(changed_observer_dependency.computed_fixed_metadata_node_handle_changed);
        assert!(changed_observer_dependency.style_record_node_handle_changed);

        let animated_overlay = crate::css::animated_overlay::AnimatedOverlay::default();
        let mut animated_metadata = metadata(7, 1, 6);
        animated_metadata.animation_overlay_identity = 9;
        animated_metadata.animated_overlay = std::ptr::from_ref(&animated_overlay);
        let changed_animation_overlay = sets.publish(Some(first_target), &[], 0, 18, animated_metadata);
        assert!(!changed_animation_overlay.new_computed_fixed_metadata);
        assert!(!changed_animation_overlay.computed_fixed_metadata_node_handle_changed);
        assert!(!changed_animation_overlay.new_style_record);
        assert!(changed_animation_overlay.animation_overlay_slot_allocated);
        assert!(changed_animation_overlay.animation_overlay_record_updated);
        assert!(changed_animation_overlay.style_record_node_handle_changed);
        assert!(!changed_animation_overlay.node_handle_changed);
        let mut updated_animated_metadata = metadata(7, 1, 6);
        updated_animated_metadata.animation_overlay_identity = 10;
        updated_animated_metadata.animated_overlay = std::ptr::from_ref(&animated_overlay);
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

        let released_animation_overlay = sets.publish(Some(first_target), &[], 0, 18, metadata(7, 1, 6));
        assert!(released_animation_overlay.animation_overlay_slot_released);
        assert_eq!(released_animation_overlay.live_animation_overlay_records, 0);
        assert_eq!(
            released_animation_overlay.style_record_identity,
            changed_observer_dependency.style_record_identity
        );
        assert_eq!(sets.live_animation_overlay_records(), 0);

        let mut reused_animated_metadata = metadata(7, 1, 6);
        reused_animated_metadata.animation_overlay_identity = 11;
        reused_animated_metadata.animated_overlay = std::ptr::from_ref(&animated_overlay);
        let reused_animation_overlay = sets.publish(Some(first_target), &[], 0, 18, reused_animated_metadata);
        assert!(!reused_animation_overlay.animation_overlay_slot_allocated);
        assert_eq!(reused_animation_overlay.live_animation_overlay_records, 1);

        let pseudo_target = ComputedStyleTarget::new(StyleNodeID::from_raw(1).unwrap(), 2);
        let pseudo = sets.publish(Some(pseudo_target), &[], 0, 18, metadata(7, 1, 6));
        let unchanged_pseudo = sets.publish(Some(pseudo_target), &[], 0, 18, metadata(7, 1, 6));
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
        let republished_pseudo = sets.publish(Some(pseudo_target), &[], 0, 18, metadata(7, 1, 6));
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
        let animated_overlay = crate::css::animated_overlay::AnimatedOverlay::default();
        let mut first_metadata = metadata(0, 0, 0);
        first_metadata.animation_overlay_identity = 1;
        first_metadata.animated_overlay = std::ptr::from_ref(&animated_overlay);
        let first = sets.publish(Some(target), &[], 0, 0, first_metadata);

        sets.pin_style_record(first.style_record_identity.raw());
        let mut second_metadata = metadata(0, 0, 0);
        second_metadata.animation_overlay_identity = 2;
        second_metadata.animated_overlay = std::ptr::from_ref(&animated_overlay);
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
    fn base_style_record_pins_are_counted_until_the_last_view_releases() {
        let mut sets = ComputedGroupSets::default();
        let publication = sets.publish(None, &[], 0, 0, metadata(0, 0, 0));
        let style_record = publication.style_record_identity;
        let base_style_record = style_record.base_record().unwrap();

        sets.pin_style_record(style_record.raw());
        sets.pin_style_record(style_record.raw());
        assert_eq!(sets.base_style_record_pins.len(), 1);
        assert_eq!(sets.base_style_record_pins[&base_style_record], 2);

        sets.unpin_style_record(style_record.raw());
        assert_eq!(sets.base_style_record_pins[&base_style_record], 1);
        sets.unpin_style_record(style_record.raw());
        assert!(sets.base_style_record_pins.is_empty());
    }

    #[test]
    fn pinned_style_record_retains_its_inherited_group_set_for_sharing() {
        let mut sets = ComputedGroupSets::default();
        let donor = sets.publish(None, &[], 0, 1, metadata(0, 0, 0));
        let record = donor.style_record_identity.raw();
        sets.pin_style_record(record);
        sets.reclaim_unreachable();
        assert_eq!(sets.inherited_sets.live_len(), 1);

        let node = StyleNodeID::element(1);
        for kind in [u8::MAX, 1, 2] {
            let target = ComputedStyleTarget::new(node, kind);
            let shared = sets.assign_shared_style_record(target, record, 0, false).unwrap();
            assert_eq!(shared.style_record_identity, donor.style_record_identity);
            assert!(!shared.new_inherited_group_set);
            assert!(shared.inherited_node_handle_changed);
            let unchanged = sets.assign_shared_style_record(target, record, 0, false).unwrap();
            assert!(!unchanged.inherited_node_handle_changed);
        }
        sets.unpin_style_record(record);
        sets.reclaim_unreachable();
        assert_eq!(sets.inherited_sets.live_len(), 1);
        sets.remove(node);
        sets.reclaim_unreachable();
        assert_eq!(sets.inherited_sets.live_len(), 0);
    }

    #[test]
    fn computed_record_reclamation_preserves_roots_while_reusing_record_slots() {
        let mut sets = ComputedGroupSets::default();
        let node = StyleNodeID::element(1);
        let target = ComputedStyleTarget::new(node, u8::MAX);
        let pinned = sets.publish(Some(target), &[], 0, 1, metadata(0, 0, 0));
        sets.pin_style_record(pinned.style_record_identity.raw());

        for environment in 2..128 {
            sets.publish(Some(target), &[], 0, environment, metadata(0, 0, 0));
        }
        let current = sets.assigned_style_record(node).unwrap().base_record().unwrap();
        let dense_record_count = sets.style_records.len();
        let retention = sets.reclaim_unreachable();

        assert_eq!(retention.retained, retention.reachable);
        assert_eq!(sets.style_records.live_len(), 2);
        assert!(sets.style_records.live_identities().any(|identity| identity == current));
        assert!(
            sets.style_records
                .live_identities()
                .any(|identity| identity == pinned.style_record_identity.base_record().unwrap())
        );

        for environment in 128..253 {
            sets.publish(Some(target), &[], 0, environment, metadata(0, 0, 0));
        }
        assert_eq!(sets.style_records.len(), dense_record_count);

        sets.unpin_style_record(pinned.style_record_identity.raw());
        sets.reclaim_unreachable();
        assert_eq!(sets.style_records.live_len(), 1);
    }

    #[test]
    fn computed_record_reclamation_reuses_the_lowest_identity_first() {
        let mut sets = ComputedGroupSets::default();
        let first = sets.publish(None, &[], 0, 1, metadata(0, 0, 0));
        sets.publish(None, &[], 0, 2, metadata(0, 0, 0));
        sets.publish(None, &[], 0, 3, metadata(0, 0, 0));
        sets.reclaim_unreachable();

        let replacement = sets.publish(None, &[], 0, 4, metadata(0, 0, 0));
        assert_eq!(
            replacement.style_record_identity.base_record(),
            first.style_record_identity.base_record()
        );
    }

    #[test]
    fn computed_record_reclamation_retires_an_exhausted_identity() {
        let mut sets = ComputedGroupSets::default();
        let exhausted = sets.publish(None, &[], 0, 1, metadata(0, 0, 0));
        let exhausted = exhausted.style_record_identity.base_record().unwrap();
        sets.style_record_generations[exhausted.index()] = FinalStyleRecordID::MAX_BASE_GENERATION;
        sets.reclaim_unreachable();

        let replacement = sets.publish(None, &[], 0, 2, metadata(0, 0, 0));
        assert_ne!(replacement.style_record_identity.base_record(), Some(exhausted));
    }

    #[test]
    #[should_panic(expected = "base style-record is not live")]
    fn a_retired_base_style_record_cannot_be_viewed() {
        let mut sets = ComputedGroupSets::default();
        let publication = sets.publish(None, &[], 0, 1, metadata(0, 0, 0));
        let retired_final = publication.style_record_identity;
        let retired = retired_final.base_record().unwrap();
        sets.reclaim_unreachable();
        let replacement = sets.publish(None, &[], 0, 2, metadata(0, 0, 0));
        let replacement_final = replacement.style_record_identity;
        let replacement = replacement_final.base_record().unwrap();
        assert_eq!(retired.index(), replacement.index());
        assert_ne!(retired_final.base_generation(), replacement_final.base_generation());
        let _ = sets.style_record_view(publication.style_record_identity.raw());
    }

    #[test]
    #[should_panic(expected = "base style-record is not live")]
    fn a_retired_base_style_record_cannot_be_pinned() {
        let mut sets = ComputedGroupSets::default();
        let publication = sets.publish(None, &[], 0, 1, metadata(0, 0, 0));
        let retired_final = publication.style_record_identity;
        let retired = retired_final.base_record().unwrap();
        sets.reclaim_unreachable();
        let replacement = sets.publish(None, &[], 0, 2, metadata(0, 0, 0));
        let replacement_final = replacement.style_record_identity;
        let replacement = replacement_final.base_record().unwrap();
        assert_eq!(retired.index(), replacement.index());
        assert_ne!(retired_final.base_generation(), replacement_final.base_generation());
        sets.pin_style_record(publication.style_record_identity.raw());
    }

    #[test]
    fn computed_group_set_capacity_includes_hash_table_control_bytes() {
        let mut sets = ComputedGroupSets::default();
        sets.groups.reserve(1);
        sets.sets.reserve(1);
        sets.inherited_sets.reserve(1);
        sets.custom_property_environments.reserve(1);
        sets.computed_fixed_metadata.reserve(1);
        sets.computed_longhand_tables.reserve(1);
        sets.style_records.reserve(1);
        sets.pending_cascade_states.reserve(1);
        sets.animation_overlay_slots_by_record.reserve(1);
        sets.pseudo_rows_by_node.reserve(1);
        sets.pseudo_rows_by_node.insert(
            StyleNodeID::element(1),
            vec![PseudoComputedRow::new(1)].into_boxed_slice(),
        );
        sets.pseudo_assignment_nested_memory
            .grow_committed(size_of::<PseudoComputedRow>() as u64);

        let accounted = sets.capacity_bytes()
            + sets.custom_property_environment_capacity_bytes()
            + sets.computed_fixed_metadata_capacity_bytes()
            + sets.computed_longhand_table_capacity_bytes()
            + sets.style_record_capacity_bytes()
            + sets.animation_overlay_capacity_bytes()
            + sets.pseudo_assignment_capacity_bytes();
        let expected = sets.groups.capacity_bytes() as usize
            + sets.sets.capacity_bytes() as usize
            + sets.inherited_sets.capacity_bytes() as usize
            + sets.custom_property_environments.capacity_bytes() as usize
            + sets.computed_fixed_metadata.capacity_bytes() as usize
            + sets.computed_longhand_tables.capacity_bytes() as usize
            + sets.style_records.capacity_bytes() as usize
            + sets.style_record_generations.capacity() * size_of::<u32>()
            + sets.pending_cascade_states.capacity()
                * (size_of::<StyleNodeID>() + size_of::<(u64, CascadeStateID)>() + 1)
            + sets.animation_overlay_slots_by_record.capacity()
                * (size_of::<FinalStyleRecordID>() + size_of::<u32>() + 1)
            + sets.pseudo_rows_by_node.capacity()
                * (size_of::<StyleNodeID>() + size_of::<Box<[PseudoComputedRow]>>() + 1)
            + size_of_val(sets.pseudo_rows_by_node[&StyleNodeID::element(1)].as_ref());

        assert_eq!(accounted, expected as u64);
    }
}
