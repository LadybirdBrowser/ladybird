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
use crate::css::computed_longhand_table::LONGHAND_COUNT;
use crate::css::computed_values::computed_group_output_mask;
use crate::css::computed_values::release_group_payload;
use crate::css::computed_values::replay_style_group_identity;
use crate::css::computed_values::replaying_style_groups;
use crate::css::computed_values::retain_group_payload;
use crate::css::computed_values::retained_group_payload_bytes;
use crate::css::computed_values::style_group_payloads_equal;
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::StyleValueData;
use crate::css::style_value::retain_style_value as retain_style_value_reference;
use crate::css::style_value::retained_value_depends_on_color_scheme;
use crate::css::style_value::retained_value_depends_on_current_color;
use crate::css::style_value::retained_value_may_depend_on_font_metrics;
use crate::css::style_value::rust_style_value_equals;

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

#[repr(C)]
pub(crate) struct InheritanceDependentValue {
    pub property: u16,
    pub value: RetainedStyleValueData,
}

struct ComputedReconstructionMetadata {
    property_importance: Box<[u8]>,
    property_inheritance: Box<[u8]>,
    inheritance_dependent_values: Box<[InheritanceDependentValue]>,
    raw_cascaded_font_size: Option<RetainedStyleValueData>,
}

impl ComputedReconstructionMetadata {
    fn inheritance_dependent_value_view(&self) -> &[super::bridge::FfiInheritanceDependentValue] {
        const {
            assert!(size_of::<InheritanceDependentValue>() == size_of::<super::bridge::FfiInheritanceDependentValue>());
            assert!(
                align_of::<InheritanceDependentValue>() == align_of::<super::bridge::FfiInheritanceDependentValue>()
            );
        }
        unsafe {
            std::slice::from_raw_parts(
                self.inheritance_dependent_values.as_ptr().cast(),
                self.inheritance_dependent_values.len(),
            )
        }
    }
}

/// The interned form of one drive's computed longhand table: one retained
/// data pointer per longhand (null where the drive stored no value), which is
/// also the borrowed span the record view hands out. Provenance (the
/// source-slot sidecar) stays on the drive table and is not retained here,
/// so equal value tuples share one identity.
struct RetainedLonghandTable {
    storage: RetainedLonghandTableStorage,
}

fn longhand_value_views_equal(first: &[*const c_void], second: &[*const c_void]) -> bool {
    first
        .iter()
        .zip(second)
        .all(|(&first, &second)| first == second || unsafe { rust_style_value_equals(first.cast(), second.cast()) })
}

enum RetainedLonghandTableStorage {
    Shared(*const ComputedLonghandTable),
    Values(Box<[*const c_void]>),
}

impl RetainedLonghandTable {
    fn value_view(&self) -> &[*const c_void] {
        match &self.storage {
            RetainedLonghandTableStorage::Shared(table) => unsafe { &**table }.value_pointers(),
            RetainedLonghandTableStorage::Values(values) => values,
        }
    }
}

impl Drop for RetainedLonghandTable {
    fn drop(&mut self) {
        match &self.storage {
            RetainedLonghandTableStorage::Shared(table) => unsafe {
                crate::css::computed_longhand_table::rust_computed_longhand_table_release(table.cast_mut());
            },
            RetainedLonghandTableStorage::Values(values) => {
                for &value in values {
                    if !value.is_null() {
                        unsafe { crate::css::style_value::release_style_value(value.cast()) };
                    }
                }
            }
        }
    }
}

pub(crate) struct StyleRecordView<'a> {
    pub payloads: &'a [*const c_void],
    pub base_payloads: &'a [*const c_void],
    pub property_importance: &'a [u8],
    pub property_inheritance: &'a [u8],
    pub inheritance_dependent_values: &'a [super::bridge::FfiInheritanceDependentValue],
    /// One entry per longhand (null where the drive stored no value), or
    /// empty when the record was published without a longhand table. Always
    /// the base record's table: animation overlays store no table entries.
    pub longhand_values: &'a [*const c_void],
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
    longhand_table: Option<ComputedLonghandTableID>,
}

struct RetainedAnimatedProperties(*const c_void);

impl RetainedAnimatedProperties {
    /// Assumes ownership of one leaked C++ reference.
    unsafe fn from_leaked(pointer: *const c_void) -> Self {
        assert!(!pointer.is_null(), "style-record animated properties are null");
        Self(pointer)
    }

    fn pointer(&self) -> *const c_void {
        self.0
    }
}

impl Drop for RetainedAnimatedProperties {
    fn drop(&mut self) {
        crate::css::ffi_stats::release_animated_properties(self.0);
    }
}

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
    /// The drive's frozen computed longhand table, or null when the publisher
    /// carries none. Its values are interned as the record's longhand-table
    /// relation. The style-sheet-context sidecar stays on the drive table:
    /// its cascade source slots are only meaningful against the drive's own
    /// cascade, so folding them into interned identity would split records
    /// across otherwise identical recomputes.
    pub longhand_table: *const ComputedLonghandTable,
    pub reconstruction: ComputedReconstructionMetadataInput<'a>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct FinalStyleRecordID(u64);

impl FinalStyleRecordID {
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
    reconstruction_metadata: ComputedReconstructionMetadataID,
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
    reconstruction_metadata: Vec<u32>,
    animation_overlay_slots: Vec<u32>,
    cascade_versions: Vec<u64>,
    cascade_states: Vec<u32>,
    flags: Vec<u8>,
}

impl PublishedComputedColumns {
    const ASSIGNED: u8 = 1;
    const INHERITED_GROUP_SWAP_ELIGIBLE: u8 = 1 << 1;
    const HAS_CASCADE_STATE: u8 = 1 << 2;

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
        self.reconstruction_metadata.resize(len, 0);
        self.animation_overlay_slots.resize(len, 0);
        self.cascade_versions.resize(len, 0);
        self.cascade_states.resize(len, 0);
        self.flags.resize(len, 0);
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

    fn reconstruction_metadata(&self, index: usize) -> Option<ComputedReconstructionMetadataID> {
        self.is_assigned(index)
            .then(|| ComputedReconstructionMetadataID(self.reconstruction_metadata[index]))
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
        self.reconstruction_metadata[index] = inputs.reconstruction_metadata.0;
        self.set_animation_overlay_slot(index, inputs.animation_overlay_slot);
        self.flags[index] = (self.flags[index] & Self::HAS_CASCADE_STATE)
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
    reconstruction_metadata: Vec<bool>,
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
    inherited_sets: InternTable<InheritedGroupSetID, Box<[ComputedGroupID]>>,
    custom_property_environments: InternTable<CustomPropertyEnvironmentID, u64>,
    computed_fixed_metadata: InternTable<ComputedFixedMetadataID, ComputedFixedMetadata>,
    computed_reconstruction_metadata: InternTable<ComputedReconstructionMetadataID, ComputedReconstructionMetadata>,
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
    reconstruction_nested_memory: MemoryLease,
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
            computed_reconstruction_metadata: InternTable::default(),
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
            reconstruction_nested_memory: MemoryLease::new(MemoryCategory::ComputedReconstructionMetadata),
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

    /// Interns the values of one drive's frozen computed longhand table, so
    /// equal value tuples share one identity and one retained copy. A
    /// value-equal previous table keeps its identity even when the fresh
    /// drive re-allocated equal values. The table's provenance sidecar is
    /// deliberately not part of the identity, nor retained here at all: its
    /// cascade source slots are per-drive data.
    fn intern_longhand_table(
        &mut self,
        table: &ComputedLonghandTable,
        previous: Option<ComputedLonghandTableID>,
        canonical: Option<ComputedLonghandTableID>,
    ) -> ComputedLonghandTableID {
        debug_assert!(table.is_frozen(), "only frozen longhand tables are published");
        let values = table.value_pointers();
        for candidate in [previous, canonical].into_iter().flatten() {
            let candidate_values = self.computed_longhand_tables[candidate].value_view();
            if longhand_value_views_equal(candidate_values, values) {
                return candidate;
            }
        }
        let hash = longhand_table_hash(values);
        if let Some(identity) = self.computed_longhand_tables.find(hash, |_identity, candidate| {
            longhand_value_views_equal(candidate.value_view(), values)
        }) {
            return identity;
        }
        let identity = self.computed_longhand_tables.take_free_identity().unwrap_or_else(|| {
            ComputedLonghandTableID(
                u32::try_from(self.computed_longhand_tables.len())
                    .expect("computed longhand-table identity space exhausted"),
            )
        });
        let retained = unsafe { crate::css::computed_longhand_table::rust_computed_longhand_table_retain(table) };
        self.reconstruction_nested_memory
            .grow_committed(size_of_val(values) as u64);
        self.computed_longhand_tables.insert(
            hash,
            identity,
            RetainedLonghandTable {
                storage: RetainedLonghandTableStorage::Shared(retained),
            },
        );
        identity
    }

    fn intern_longhand_values(
        &mut self,
        values: &[*const c_void],
        previous: Option<ComputedLonghandTableID>,
    ) -> ComputedLonghandTableID {
        assert_eq!(values.len(), LONGHAND_COUNT);
        if let Some(previous) = previous {
            let previous_values = self.computed_longhand_tables[previous].value_view();
            if longhand_value_views_equal(previous_values, values) {
                return previous;
            }
        }
        let hash = longhand_table_hash(values);
        if let Some(identity) = self.computed_longhand_tables.find(hash, |_identity, candidate| {
            longhand_value_views_equal(candidate.value_view(), values)
        }) {
            return identity;
        }
        let identity = self.computed_longhand_tables.take_free_identity().unwrap_or_else(|| {
            ComputedLonghandTableID(
                u32::try_from(self.computed_longhand_tables.len())
                    .expect("computed longhand-table identity space exhausted"),
            )
        });
        let value_view: Box<[*const c_void]> = values
            .iter()
            .map(|&value| match value.is_null() {
                true => value,
                false => unsafe { retain_style_value_reference(value.cast()) }.cast(),
            })
            .collect();
        self.reconstruction_nested_memory
            .grow_committed(size_of_val(value_view.as_ref()) as u64);
        self.computed_longhand_tables.insert(
            hash,
            identity,
            RetainedLonghandTable {
                storage: RetainedLonghandTableStorage::Values(value_view),
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
        const ENGINE_RESOLVABLE_INHERITED_GROUPS: u8 =
            STATIC_INHERITED_GROUPS | INHERITED_UI_GROUP | INHERITED_TEXT_GROUP;
        const INHERITED_GROUP_COUNT: usize = 7;
        const INHERITED_GROUP_MASK: u32 = (1 << INHERITED_GROUP_COUNT) - 1;
        if inherited_style_groups == 0 || inherited_style_groups & !ENGINE_RESOLVABLE_INHERITED_GROUPS != 0 {
            return None;
        }
        let target = ComputedStyleTarget::new(node, u8::MAX);
        if inherited_style_groups & INHERITED_TEXT_GROUP != 0
            && self
                .current_color_dependency_mask(target)
                .is_none_or(|dependencies| dependencies & !INHERITED_GROUP_MASK != 0)
        {
            return None;
        }
        if inherited_style_groups & INHERITED_UI_GROUP != 0
            && self
                .color_scheme_dependency_mask(target)
                .is_none_or(|dependencies| dependencies & !INHERITED_GROUP_MASK != 0)
        {
            return None;
        }
        let index = node.element_index()? as usize;
        let parent_index = parent.element_index()? as usize;
        if !self.columns.inherited_group_swap_eligible(index)
            || self.columns.animation_overlay_slot(index).is_some()
            || self.columns.animation_overlay_slot(parent_index).is_some()
            || self.assigned_pseudo_kinds(node).next().is_some()
        {
            return None;
        }

        let old_style_record = *self.style_record_column.get(index)?.as_ref()?;
        let old_record = *self.style_records.get_index(old_style_record.index())?;
        let old_group_set = self.sets.get_index(old_record.groups.0 as usize)?;
        let parent_inherited = self.columns.inherited_groups(parent_index)?;
        let parent_groups = self.inherited_sets.get_index(parent_inherited.0 as usize)?;
        if parent_groups.len() != INHERITED_GROUP_COUNT || old_group_set.payloads.len() < INHERITED_GROUP_COUNT {
            return None;
        }

        let mut groups = self.group_identities(old_record.groups);
        groups[..INHERITED_GROUP_COUNT].copy_from_slice(parent_groups);
        let group_set = self.intern_group_set(&groups).0;
        // The swap is only taken for a fully inheriting element, so every
        // inherited-by-default longhand's value is the parent's; the swapped
        // record's table is the old one with those slots replaced by the
        // parent's, keeping the record a complete inheritance source for a
        // child's drive. Records without tables on either side (none remain
        // in practice) publish without one.
        let parent_style_record = self.style_record_column.get(parent_index).copied().flatten();
        let parent_table = parent_style_record
            .and_then(|record| self.style_records.get_index(record.index()))
            .and_then(|record| record.longhand_table);
        let longhand_table = match (old_record.longhand_table, parent_table) {
            (Some(old_table), Some(parent_table)) => {
                use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, property_is_inherited};
                let mut values = self.computed_longhand_tables[old_table].value_view().to_vec();
                let parent_values = self.computed_longhand_tables[parent_table].value_view();
                for (index, value) in values.iter_mut().enumerate() {
                    if property_is_inherited(FIRST_LONGHAND_PROPERTY_ID + index as u16) {
                        *value = parent_values[index];
                    }
                }
                Some(self.intern_longhand_values(&values, Some(old_table)))
            }
            _ => None,
        };
        let new_record = StyleRecord {
            groups: group_set,
            custom_properties: old_record.custom_properties,
            fixed_metadata: old_record.fixed_metadata,
            reconstruction_metadata: old_record.reconstruction_metadata,
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
        animated_properties: RetainedAnimatedProperties,
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
            animated_properties,
            payloads: payloads.into(),
            pin_count: 0,
            is_assigned: true,
        }
    }

    fn allocate_animation_overlay(
        &mut self,
        base_style_record: StyleRecordID,
        source_identity: u64,
        animated_properties: &mut Option<RetainedAnimatedProperties>,
        payloads: &[*const c_void],
    ) -> (u32, FinalStyleRecordID, bool) {
        let record = self.make_animation_overlay_record(
            base_style_record,
            source_identity,
            animated_properties
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
        animated_properties: &mut Option<RetainedAnimatedProperties>,
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
                    animated_properties
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
            || self.final_base_style_record(base_style_record),
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
            longhand_table,
            reconstruction: reconstruction_metadata,
        } = metadata_input;
        let mut animated_properties = if animated_properties.is_null() {
            None
        } else {
            Some(unsafe { RetainedAnimatedProperties::from_leaked(animated_properties) })
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

        assert_eq!(
            reconstruction_metadata.inheritance_dependent_properties.len(),
            reconstruction_metadata.inheritance_dependent_values.len()
        );
        let mut inheritance_dependent_values: Vec<_> = reconstruction_metadata
            .inheritance_dependent_properties
            .iter()
            .copied()
            .zip(reconstruction_metadata.inheritance_dependent_values.iter().copied())
            .map(|(property, value)| (property, value, 1u8))
            .collect();
        if let Some(longhand_table) = longhand_table {
            inheritance_dependent_values.extend(
                longhand_table
                    .inheritance_dependent_values()
                    .map(|(property, value)| (property, value, 0u8)),
            );
        }
        inheritance_dependent_values.sort_unstable_by_key(|&(property, _, source_rank)| (property, source_rank));
        inheritance_dependent_values.dedup_by_key(|(property, _, _)| *property);
        let inheritance_dependent_values: Vec<_> = inheritance_dependent_values
            .into_iter()
            .map(|(property, value, _)| (property, value))
            .collect();
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
                    let identity = self
                        .computed_reconstruction_metadata
                        .take_free_identity()
                        .unwrap_or_else(|| {
                            ComputedReconstructionMetadataID(
                                u32::try_from(self.computed_reconstruction_metadata.len())
                                    .expect("computed reconstruction-metadata identity space exhausted"),
                            )
                        });
                    let inheritance_dependent_values: Box<[InheritanceDependentValue]> = inheritance_dependent_values
                        .into_iter()
                        .map(|(property, value)| InheritanceDependentValue {
                            property,
                            value: retain_style_value(value),
                        })
                        .collect();
                    let raw_cascaded_font_size = (!reconstruction_metadata.raw_cascaded_font_size.is_null())
                        .then(|| retain_style_value(reconstruction_metadata.raw_cascaded_font_size));
                    let metadata = ComputedReconstructionMetadata {
                        property_importance: reconstruction_metadata.property_importance.into(),
                        property_inheritance: reconstruction_metadata.property_inheritance.into(),
                        inheritance_dependent_values,
                        raw_cascaded_font_size,
                    };
                    self.reconstruction_nested_memory.grow_committed(
                        (metadata.property_importance.len()
                            + metadata.property_inheritance.len()
                            + size_of_val(metadata.inheritance_dependent_values.as_ref()))
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
            longhand_table: longhand_table_identity,
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
            let previous = self.pseudo_row(node, pseudo_kind).and_then(|row| row.assignment);
            let previous_style_record_identity = previous
                .map(|previous| self.final_style_record(previous.style_record, previous.animation_overlay_slot));
            let animation_overlay_publication = self.update_animation_overlay(
                previous.and_then(|previous| previous.animation_overlay_slot),
                style_record_identity,
                animation_overlay_identity,
                &mut animated_properties,
                animation_overlay_payloads,
            );
            let row = self.ensure_pseudo_row(node, pseudo_kind);
            row.set_published(true);
            row.assignment = Some(PublishedComputedInputs {
                groups: identity,
                inherited_groups: inherited_identity,
                custom_properties: custom_property_environment_identity,
                fixed_metadata: computed_fixed_metadata_identity,
                reconstruction_metadata: computed_reconstruction_metadata_identity,
                style_record: style_record_identity,
                animation_overlay_slot: animation_overlay_publication.slot,
            });
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
                &mut animated_properties,
                animation_overlay_payloads,
            );
            let changed = (
                self.columns.groups(index) != Some(identity),
                self.columns.inherited_groups(index) != Some(inherited_identity),
                self.columns.custom_properties(index) != Some(custom_property_environment_identity),
                self.columns.fixed_metadata(index) != Some(computed_fixed_metadata_identity),
                self.columns.reconstruction_metadata(index) != Some(computed_reconstruction_metadata_identity),
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
                    reconstruction_metadata: computed_reconstruction_metadata_identity,
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
        let group_identities = self.group_identities(record.groups);
        let inherited_groups = &group_identities[..inherited_group_count];
        let (inherited_identity, new_inherited_group_set) = self.intern_inherited_group_set(inherited_groups);

        let is_pseudo = target.is_pseudo();
        let (
            node_handle_changed,
            inherited_node_handle_changed,
            custom_property_environment_node_handle_changed,
            computed_fixed_metadata_node_handle_changed,
            computed_reconstruction_metadata_node_handle_changed,
            previous_style_record_identity,
            animation_overlay_publication,
        ) = if target.is_pseudo() {
            let previous = self
                .pseudo_row(target.node, target.pseudo_kind)
                .and_then(|row| row.assignment);
            let previous_style_record_identity = previous
                .map(|previous| self.final_style_record(previous.style_record, previous.animation_overlay_slot));
            let mut animated_properties = None;
            let animation_overlay_publication = self.update_animation_overlay(
                previous.and_then(|previous| previous.animation_overlay_slot),
                style_record_identity,
                0,
                &mut animated_properties,
                &[],
            );
            let row = self.ensure_pseudo_row(target.node, target.pseudo_kind);
            row.set_published(true);
            row.assignment = Some(PublishedComputedInputs {
                groups: record.groups,
                inherited_groups: inherited_identity,
                custom_properties: record.custom_properties,
                fixed_metadata: record.fixed_metadata,
                reconstruction_metadata: record.reconstruction_metadata,
                style_record: style_record_identity,
                animation_overlay_slot: None,
            });
            (
                previous.is_none_or(|previous| previous.groups != record.groups),
                previous.is_none_or(|previous| previous.inherited_groups != inherited_identity),
                previous.is_none_or(|previous| previous.custom_properties != record.custom_properties),
                previous.is_none_or(|previous| previous.fixed_metadata != record.fixed_metadata),
                previous.is_none_or(|previous| previous.reconstruction_metadata != record.reconstruction_metadata),
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
            let mut animated_properties = None;
            let animation_overlay_publication = self.update_animation_overlay(
                self.columns.animation_overlay_slot(index),
                style_record_identity,
                0,
                &mut animated_properties,
                &[],
            );
            let changed = (
                self.columns.groups(index) != Some(record.groups),
                self.columns.inherited_groups(index) != Some(inherited_identity),
                self.columns.custom_properties(index) != Some(record.custom_properties),
                self.columns.fixed_metadata(index) != Some(record.fixed_metadata),
                self.columns.reconstruction_metadata(index) != Some(record.reconstruction_metadata),
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
                    reconstruction_metadata: record.reconstruction_metadata,
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
            new_inherited_group_set,
            new_custom_property_environment: false,
            new_computed_fixed_metadata: false,
            new_computed_reconstruction_metadata: false,
            new_style_record: false,
            node_handle_changed,
            inherited_node_handle_changed,
            custom_property_environment_node_handle_changed,
            computed_fixed_metadata_node_handle_changed,
            computed_reconstruction_metadata_node_handle_changed,
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
            || first.reconstruction_metadata != second.reconstruction_metadata
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
                .value_view()
                .iter()
                .zip(self.computed_longhand_tables[second].value_view())
                .all(|(&first, &second)| {
                    first == second || unsafe { rust_style_value_equals(first.cast(), second.cast()) }
                }),
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

    fn specified_value_dependency_mask(
        &self,
        target: ComputedStyleTarget,
        depends_on_input: impl Fn(&RetainedStyleValueData) -> bool,
    ) -> Option<u32> {
        let reconstruction_metadata = if target.is_pseudo() {
            self.pseudo_row(target.node, target.pseudo_kind)
                .and_then(|row| row.assignment)
                .map(|assignment| assignment.reconstruction_metadata)
        } else {
            target
                .node
                .element_index()
                .and_then(|index| self.columns.reconstruction_metadata(index as usize))
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
            self.pseudo_row(target.node, target.pseudo_kind)
                .and_then(|row| row.assignment)
                .map(|assignment| assignment.reconstruction_metadata)
        } else {
            target
                .node
                .element_index()
                .and_then(|index| self.columns.reconstruction_metadata(index as usize))
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
            shallow [self.custom_property_environments, self.columns.custom_properties];
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
    pub fn computed_reconstruction_metadata_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.computed_reconstruction_metadata,
                self.columns.reconstruction_metadata,
                self.computed_longhand_tables,
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
            reconstruction_metadata: vec![false; self.computed_reconstruction_metadata.len()],
            longhand_tables: vec![false; self.computed_longhand_tables.len()],
            style_records: vec![false; self.style_records.len()],
        };

        {
            let mut mark_style_record = |identity: StyleRecordID| {
                ComputedReachability::mark(&mut reachable.style_records, identity);
                let record = self.style_records.get(identity);
                ComputedReachability::mark(&mut reachable.sets, record.groups);
                ComputedReachability::mark(&mut reachable.custom_property_environments, record.custom_properties);
                ComputedReachability::mark(&mut reachable.fixed_metadata, record.fixed_metadata);
                ComputedReachability::mark(&mut reachable.reconstruction_metadata, record.reconstruction_metadata);
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
            ComputedReachability::mark(
                &mut reachable.reconstruction_metadata,
                self.columns.reconstruction_metadata(index).unwrap(),
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
                ComputedReachability::mark(
                    &mut reachable.reconstruction_metadata,
                    assignment.reconstruction_metadata,
                );
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
        for identity in self
            .computed_reconstruction_metadata
            .live_identities()
            .collect::<Vec<_>>()
        {
            if reachable.reconstruction_metadata[identity.index()] {
                continue;
            }
            let metadata = self.computed_reconstruction_metadata.get(identity);
            let inheritance_dependent_values = metadata
                .inheritance_dependent_values
                .iter()
                .map(|entry| (entry.property, entry.value.pointer().cast()))
                .collect::<Vec<_>>();
            let hash = reconstruction_metadata_hash(
                &metadata.property_importance,
                &metadata.property_inheritance,
                &inheritance_dependent_values,
                metadata
                    .raw_cascaded_font_size
                    .as_ref()
                    .map_or(std::ptr::null(), |value| value.pointer().cast()),
            );
            let metadata = std::mem::replace(
                self.computed_reconstruction_metadata.get_mut(identity),
                ComputedReconstructionMetadata {
                    property_importance: Box::default(),
                    property_inheritance: Box::default(),
                    inheritance_dependent_values: Box::default(),
                    raw_cascaded_font_size: None,
                },
            );
            self.reconstruction_nested_memory.shrink_committed(
                (metadata.property_importance.len()
                    + metadata.property_inheritance.len()
                    + size_of_val(metadata.inheritance_dependent_values.as_ref())) as u64,
            );
            self.computed_reconstruction_metadata.retire_identity(hash, identity);
        }
        for identity in self.computed_longhand_tables.live_identities().collect::<Vec<_>>() {
            if reachable.longhand_tables[identity.index()] {
                continue;
            }
            let hash = longhand_table_hash(self.computed_longhand_tables[identity].value_view());
            let table = std::mem::replace(
                self.computed_longhand_tables.get_mut(identity),
                RetainedLonghandTable {
                    storage: RetainedLonghandTableStorage::Values(Box::default()),
                },
            );
            self.reconstruction_nested_memory
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
        let (base_style_record, payloads, animation_overlay_identity, animated_properties) =
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
                    overlay.animated_properties.pointer(),
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
        let reconstruction_metadata = self
            .computed_reconstruction_metadata
            .get_index(record.reconstruction_metadata.0 as usize)?;
        let longhand_values = record
            .longhand_table
            .and_then(|identity| self.computed_longhand_tables.get_index(identity.0 as usize))
            .map_or(&[][..], RetainedLonghandTable::value_view);
        Some(StyleRecordView {
            payloads,
            base_payloads,
            property_importance: &reconstruction_metadata.property_importance,
            property_inheritance: &reconstruction_metadata.property_inheritance,
            inheritance_dependent_values: reconstruction_metadata.inheritance_dependent_value_view(),
            longhand_values,
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
        let reconstruction = self.reconstruction_nested_memory.bytes();
        self.reconstruction_nested_memory
            .reconcile_committed(memory, reconstruction);
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

fn longhand_table_hash(values: &[*const c_void]) -> u64 {
    let mut hasher = fast_hasher();
    for &value in values {
        (value as usize).hash(&mut hasher);
    }
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
    let retained = unsafe { retain_style_value_reference(value.cast::<StyleValueData>()) };
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
            longhand_table: std::ptr::null(),
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
    fn viewport_dependent_nodes_include_elements_and_pseudo_owners_once() {
        let mut sets = ComputedGroupSets::default();
        let independent = ComputedStyleTarget::new(StyleNodeID::element(1), u8::MAX);
        let viewport_dependent = ComputedStyleTarget::new(StyleNodeID::element(2), u8::MAX);
        let font_dependent_pseudo = ComputedStyleTarget::new(StyleNodeID::element(3), 1);
        let viewport_dependent_pseudo = ComputedStyleTarget::new(StyleNodeID::element(2), 2);
        let viewport_dependent_pseudo_only = ComputedStyleTarget::new(StyleNodeID::element(4), 1);
        sets.publish(Some(independent), &[], 0, 0, metadata(0, 0, 0, &[], &[]));
        sets.publish(Some(viewport_dependent), &[], 0, 0, metadata(0, 1, 0, &[], &[]));
        sets.publish(Some(font_dependent_pseudo), &[], 0, 0, metadata(0, 2, 0, &[], &[]));
        sets.publish(Some(viewport_dependent_pseudo), &[], 0, 0, metadata(0, 1, 0, &[], &[]));
        sets.publish(
            Some(viewport_dependent_pseudo_only),
            &[],
            0,
            0,
            metadata(0, 1, 0, &[], &[]),
        );

        assert_eq!(sets.viewport_dependent_nodes(), vec![2, 4]);
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
    fn base_style_record_pins_are_counted_until_the_last_view_releases() {
        let mut sets = ComputedGroupSets::default();
        let publication = sets.publish(None, &[], 0, 0, metadata(0, 0, 0, &[], &[]));
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
    fn computed_record_reclamation_preserves_roots_while_reusing_record_slots() {
        let mut sets = ComputedGroupSets::default();
        let node = StyleNodeID::element(1);
        let target = ComputedStyleTarget::new(node, u8::MAX);
        let pinned = sets.publish(Some(target), &[], 0, 1, metadata(0, 0, 0, &[], &[]));
        sets.pin_style_record(pinned.style_record_identity.raw());

        for environment in 2..128 {
            sets.publish(Some(target), &[], 0, environment, metadata(0, 0, 0, &[], &[]));
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
            sets.publish(Some(target), &[], 0, environment, metadata(0, 0, 0, &[], &[]));
        }
        assert_eq!(sets.style_records.len(), dense_record_count);

        sets.unpin_style_record(pinned.style_record_identity.raw());
        sets.reclaim_unreachable();
        assert_eq!(sets.style_records.live_len(), 1);
    }

    #[test]
    fn computed_record_reclamation_reuses_the_lowest_identity_first() {
        let mut sets = ComputedGroupSets::default();
        let first = sets.publish(None, &[], 0, 1, metadata(0, 0, 0, &[], &[]));
        sets.publish(None, &[], 0, 2, metadata(0, 0, 0, &[], &[]));
        sets.publish(None, &[], 0, 3, metadata(0, 0, 0, &[], &[]));
        sets.reclaim_unreachable();

        let replacement = sets.publish(None, &[], 0, 4, metadata(0, 0, 0, &[], &[]));
        assert_eq!(
            replacement.style_record_identity.base_record(),
            first.style_record_identity.base_record()
        );
    }

    #[test]
    fn computed_record_reclamation_retires_an_exhausted_identity() {
        let mut sets = ComputedGroupSets::default();
        let exhausted = sets.publish(None, &[], 0, 1, metadata(0, 0, 0, &[], &[]));
        let exhausted = exhausted.style_record_identity.base_record().unwrap();
        sets.style_record_generations[exhausted.index()] = FinalStyleRecordID::MAX_BASE_GENERATION;
        sets.reclaim_unreachable();

        let replacement = sets.publish(None, &[], 0, 2, metadata(0, 0, 0, &[], &[]));
        assert_ne!(replacement.style_record_identity.base_record(), Some(exhausted));
    }

    #[test]
    #[should_panic(expected = "base style-record is not live")]
    fn a_retired_base_style_record_cannot_be_viewed() {
        let mut sets = ComputedGroupSets::default();
        let publication = sets.publish(None, &[], 0, 1, metadata(0, 0, 0, &[], &[]));
        let retired_final = publication.style_record_identity;
        let retired = retired_final.base_record().unwrap();
        sets.reclaim_unreachable();
        let replacement = sets.publish(None, &[], 0, 2, metadata(0, 0, 0, &[], &[]));
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
        let publication = sets.publish(None, &[], 0, 1, metadata(0, 0, 0, &[], &[]));
        let retired_final = publication.style_record_identity;
        let retired = retired_final.base_record().unwrap();
        sets.reclaim_unreachable();
        let replacement = sets.publish(None, &[], 0, 2, metadata(0, 0, 0, &[], &[]));
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
        sets.computed_reconstruction_metadata.reserve(1);
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
