/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C++/Rust boundary for StyleEngine.
//!
//! C++ emits one flat immutable transaction per style flush. The arrays use fixed-width IDs and
//! tagged records and contain no owning C++ pointers, and each typed delta kind travels in its own
//! array. The point of the shape is what it forbids: there is never one call per element or per
//! selector operator.
//!
//! No string crosses here. Selector-mentioned tags, IDs, classes, attribute names, and attribute
//! values are interned on the C++ side, where the authoritative `Utf16FlyString` payloads already
//! live, and reach Rust as `StyleAtomID` words. Interning a name is a hash lookup plus a reference
//! count bump on a string that already exists; nothing is copied, and neither side pays a UTF-16 or
//! ASCII conversion for a fact that a `u32` comparison can answer. Values too unique to intern do
//! not become atoms at all - their exact test reads the live DOM through a borrowed string view
//! that preserves the C++ side's ASCII-or-UTF-16 representation, and only after the cheaper atom
//! and name checks have already passed.
//!
//! Identity allocation is batched for the same reason as everything else: C++ owns DOM lifecycle
//! and asks for a run of `StyleNodeID` values, Rust owns the arena and the relation columns keyed by
//! them.

use std::ffi::c_void;

use crate::abort_on_panic as abort_on_boundary_panic;
use crate::css::selector::CompiledSelector;
use crate::css::selector::RustSelector;

use super::HashSet;
use super::PinnedAtoms;
use super::StyleEngine;
use super::batch_matcher::RuleMatch;
use super::cascade::CascadeOperator;
use super::compiler::ImplicitScopeRoot;
use super::compiler::NamespaceScope;
use super::compiler::ScopeChain;
use super::index::FeatureValue;
use super::index::LocalFeatureKey;
use super::index::StyleAtomID;
use super::memory::DeviceClass;
#[cfg(feature = "style-recording")]
use super::memory::MEMORY_CATEGORIES;
#[cfg(feature = "style-recording")]
use super::memory::MEMORY_CATEGORY_COUNT;
use super::memory::MemoryCategory;
use super::memory::MemoryLease;
#[cfg(feature = "style-recording")]
use super::memory::TIER3_REFUSAL_CATEGORIES;
use super::program::CascadeLayerID;
use super::program::CascadeOrigin;
use super::program::DeclarationBlockID;
use super::program::DeclaredProperty;
use super::program::RuleID;
use super::program::RuleKind;
use super::program::SheetID;
use super::program::StyleSheetObjectID;
use super::record_replay::EventKind;
use super::selector::SelectorProgram;
use super::transaction::ElementDeclarationKind;
use super::transaction::InputKey;
use super::transaction::InputValue;
use super::transaction::StateFact;
use super::transaction::TreeRelations;
use super::tree::StyleNodeID;
use super::tree::TreeScopeID;

fn abort_on_panic<F: FnOnce() -> R, R>(operation: F) -> R {
    abort_on_boundary_panic(operation)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiStyleDeltaGap {
    None,
    Materialize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u16)]
pub enum FfiStyleDeltaDamage {
    None,
    Full,
}

/// One final style assignment or a typed request to materialize it in C++.
///
/// A zero match-answer identity means the complete answer is node-contextual and cannot be shared
/// across elements. It remains in transaction scratch under the style-node identity.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct FfiStyleDelta {
    pub style_node: u32,
    pub match_answer: u32,
    pub old_style_record: u64,
    pub new_style_record: u64,
    pub damage: FfiStyleDeltaDamage,
    pub reaction: u8,
    pub inherited_style_groups: u8,
    pub pseudo_kind: u8,
    pub gap: FfiStyleDeltaGap,
}

#[derive(Default)]
pub(super) struct FfiStyleTransactionOutput {
    scoped: bool,
    style_atoms_swept: bool,
    transaction_version: u64,
    program_version: u64,
    answers: Vec<FfiStyleDelta>,
    reclaimed_style_atoms: Vec<FfiReclaimedStyleAtom>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct FfiReclaimedStyleAtom {
    pub raw: usize,
    pub atom: u32,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStyleTransactionView {
    pub transaction_version: u64,
    pub program_version: u64,
    pub answers: *const FfiStyleDelta,
    pub count: usize,
    pub reclaimed_style_atoms: *const FfiReclaimedStyleAtom,
    pub reclaimed_style_atom_count: usize,
    pub scoped: bool,
    pub style_atoms_swept: bool,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStyleNodeSlice {
    pub nodes: *const u32,
    pub count: usize,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSourceSlotAssignmentView {
    pub assignments: *const c_void,
    pub count: usize,
}

impl Default for FfiSourceSlotAssignmentView {
    fn default() -> Self {
        Self {
            assignments: std::ptr::null(),
            count: 0,
        }
    }
}

impl Default for FfiStyleNodeSlice {
    fn default() -> Self {
        Self {
            nodes: std::ptr::null(),
            count: 0,
        }
    }
}

impl Default for FfiStyleTransactionView {
    fn default() -> Self {
        Self {
            transaction_version: 0,
            program_version: 0,
            answers: std::ptr::null(),
            count: 0,
            reclaimed_style_atoms: std::ptr::null(),
            reclaimed_style_atom_count: 0,
            scoped: false,
            style_atoms_swept: false,
        }
    }
}

fn write_style_transaction_outputs(
    output: &FfiStyleTransactionOutput,
    payload: &mut super::record_replay::PayloadWriter,
) {
    payload.write_bool(output.scoped);
    payload.write_length(0);
    payload.write_length(usize::from(!output.answers.is_empty()));
    if !output.answers.is_empty() {
        payload.write_u64(output.transaction_version);
        payload.write_u64(output.program_version);
        payload.write_length(output.answers.len());
        for answer in &output.answers {
            payload.write_u32(answer.style_node);
            payload.write_u32(answer.match_answer);
            payload.write_u64(answer.old_style_record);
            payload.write_u64(answer.new_style_record);
            payload.write_u16(answer.damage as u16);
            payload.write_u8(answer.reaction);
            payload.write_u8(answer.inherited_style_groups);
            payload.write_u8(answer.pseudo_kind);
            payload.write_u8(answer.gap as u8);
        }
    }
    payload.write_bool(output.style_atoms_swept);
    payload.write_length(output.reclaimed_style_atoms.len());
    for reclaimed in &output.reclaimed_style_atoms {
        payload.write_u32(reclaimed.atom);
    }
}

/// One final base-style assignment. Zero names the absent side of an insertion or removal.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiStyleRecordDelta {
    pub old_style_record: u64,
    pub new_style_record: u64,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiInheritanceDependentValue {
    pub property: u16,
    pub value: *const c_void,
}

#[cfg(feature = "style-recording")]
#[derive(Clone, Copy, Debug, Default)]
pub struct FfiMemoryPressureSnapshot {
    pub tier3_limit: u64,
    pub tier4_limit: u64,
    pub tier3_bytes: u64,
    pub tier4_bytes: u64,
    pub tier3_refusals: u64,
    pub tier4_refusals: u64,
    pub tier3_refusal_categories: [u64; TIER3_REFUSAL_CATEGORIES.len()],
    pub tier4_refusal_categories: [u64; 2],
    pub tier3_evictions: u64,
    pub category_bytes: [u64; MEMORY_CATEGORY_COUNT],
}

/// A synchronous borrowed view of one final style record.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiStyleRecordView {
    pub payloads: *const *const c_void,
    pub base_payloads: *const *const c_void,
    pub property_importance: *const u8,
    pub property_inheritance: *const u8,
    pub inheritance_dependent_values: *const FfiInheritanceDependentValue,
    /// One borrowed style-value data pointer per longhand (null where the
    /// drive stored no value), or null/0 when the record carries no table.
    /// Always the base record's table, matching `WithAnimationsApplied::No`.
    pub longhand_values: *const *const c_void,
    pub raw_cascaded_font_size: *const c_void,
    pub animated_properties: *const c_void,
    pub payload_count: usize,
    pub property_importance_count: usize,
    pub property_inheritance_count: usize,
    pub inheritance_dependent_value_count: usize,
    pub longhand_value_count: usize,
    pub pseudo_element_styles: u64,
    pub counter_style_environment_identity: u64,
    pub animation_overlay_identity: u64,
    pub dependency_flags: u8,
    pub present: bool,
}

struct SelectorQuery {
    program: SelectorProgram,
    _atoms: PinnedAtoms,
    _memory: MemoryLease,
}

impl FfiStyleRecordView {
    fn missing() -> Self {
        Self {
            payloads: std::ptr::null(),
            base_payloads: std::ptr::null(),
            property_importance: std::ptr::null(),
            property_inheritance: std::ptr::null(),
            inheritance_dependent_values: std::ptr::null(),
            longhand_values: std::ptr::null(),
            raw_cascaded_font_size: std::ptr::null(),
            animated_properties: std::ptr::null(),
            payload_count: 0,
            property_importance_count: 0,
            property_inheritance_count: 0,
            inheritance_dependent_value_count: 0,
            longhand_value_count: 0,
            pseudo_element_styles: 0,
            counter_style_environment_identity: 0,
            animation_overlay_identity: 0,
            dependency_flags: 0,
            present: false,
        }
    }
}

/// The exact cascade comparison and the computed-group dependency closure it wakes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct FfiExactCascadePublication {
    pub computed_group_mask: u32,
    pub computed_property_word_0: u64,
    pub computed_property_word_1: u64,
    pub computed_property_word_2: u64,
    pub computed_property_word_3: u64,
    pub computed_property_word_4: u64,
    pub computed_property_word_5: u64,
    pub computed_property_closure_is_exact: bool,
    pub unchanged: bool,
}

impl FfiExactCascadePublication {
    fn missing() -> Self {
        Self {
            computed_group_mask: u32::MAX,
            computed_property_word_0: u64::MAX,
            computed_property_word_1: u64::MAX,
            computed_property_word_2: u64::MAX,
            computed_property_word_3: u64::MAX,
            computed_property_word_4: u64::MAX,
            computed_property_word_5: u64::MAX,
            computed_property_closure_is_exact: false,
            unchanged: false,
        }
    }
}

#[cfg(feature = "style-recording")]
#[derive(Clone, Copy)]
pub struct RecordedExactCascadeWinner {
    pub property: u16,
    pub value: u64,
    pub operator: CascadeOperator,
    pub animation_relevance: u32,
}

/// The tree relations of one style node on one side of a mutation. Zero means "no such relation".
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiTreeRelations {
    pub parent: u32,
    pub previous_element_sibling: u32,
    pub next_element_sibling: u32,
    pub tree_scope: u32,
    pub assigned_slot: u32,
    // Retained as zero to preserve the record-replay wire layout.
    pub reserved: u32,
}

impl FfiTreeRelations {
    fn decode(self) -> TreeRelations {
        TreeRelations {
            parent: StyleNodeID::from_raw(self.parent),
            previous_element_sibling: StyleNodeID::from_raw(self.previous_element_sibling),
            next_element_sibling: StyleNodeID::from_raw(self.next_element_sibling),
            tree_scope: TreeScopeID(self.tree_scope),
            assigned_slot: StyleNodeID::from_raw(self.assigned_slot),
        }
    }
}

/// One structural change. `old_connected` and `new_connected` distinguish insertion, removal, and
/// movement without needing three record types.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiTreeDelta {
    pub node: u32,
    pub old_connected: bool,
    pub new_connected: bool,
    pub old_relations: FfiTreeRelations,
    pub new_relations: FfiTreeRelations,
}

/// Selector-visible facts which exist when one style node first joins the tree. Variable-width
/// custom states occupy one shared atom column and are named by this row's offset and count.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiElementArrival {
    pub node: u32,
    pub namespace_atom: u32,
    pub language_atom: u32,
    pub directionality_atom: u32,
    pub custom_state_offset: u32,
    pub custom_state_count: u32,
    pub heading_level: u8,
    pub is_slot: bool,
    pub reserved: u16,
}

/// Which local fact a feature delta describes.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiFeatureKind {
    TagName = 0,
    Id = 1,
    Class = 2,
    Attribute = 3,
    /// The ASCII-lowercase folding of the element's local name, recorded only when it differs from
    /// the name itself. It is what makes a case-insensitively written type selector reach the
    /// element without every such selector having to dispatch universally.
    FoldedTagName = 4,
    /// Whether the element has no children at all. It is not a feature of any one child, which is
    /// why a text node arriving publishes it: `:empty` is about the element, and a text node
    /// changes it while connecting no element for a tree delta to be recorded from.
    Emptiness = 5,
}

/// How a feature value is represented on one side of a change.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiFeatureValueKind {
    Absent = 0,
    /// Present, but the payload is not internable; an exact test reads the live DOM.
    Present = 1,
    Atom = 2,
    /// Present, with a value that differs from the one the fact held before. Attribute values do
    /// not cross as text - an exact test reads the live DOM - but a change to one is still a
    /// change, and a delta that reported presence on both sides would cancel in the journal and
    /// invalidate nothing.
    ChangedValue = 3,
}

/// One change to a local selector feature: a tag name, an ID, one class, or one attribute.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLocalFeatureDelta {
    pub node: u32,
    pub feature_kind: FfiFeatureKind,
    /// The class atom or attribute-name atom the key is about. Unused for tag names and IDs.
    pub name_atom: u32,
    pub old_kind: FfiFeatureValueKind,
    pub old_atom: u32,
    pub new_kind: FfiFeatureValueKind,
    pub new_atom: u32,
}

// An element or document state fact. Values mirror `StateFact` one for one, so the boundary can
// publish every boolean pseudo-class the parser can produce.
include!(concat!(env!("OUT_DIR"), "/ffi_state_fact_generated.rs"));

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStateDelta {
    pub node: u32,
    pub fact: FfiStateFact,
    pub new_value: bool,
}

/// Which element-sourced declaration block changed. Each kind keeps its language-defined cascade
/// placement; a presentational hint is not inline style wearing a different name.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiElementDeclarationKind {
    InlineStyle = 0,
    PresentationalHint = 1,
    SvgPresentationAttribute = 2,
}

/// How a CSS-wide keyword participates in the cascade.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiCascadeOperator {
    Declared = 0,
    Inherit = 1,
    Initial = 2,
    Unset = 3,
    Revert = 4,
    RevertLayer = 5,
}

impl FfiCascadeOperator {
    fn decode(self) -> CascadeOperator {
        match self {
            Self::Declared => CascadeOperator::Declared,
            Self::Inherit => CascadeOperator::Inherit,
            Self::Initial => CascadeOperator::Initial,
            Self::Unset => CascadeOperator::Unset,
            Self::Revert => CascadeOperator::Revert,
            Self::RevertLayer => CascadeOperator::RevertLayer,
        }
    }
}

/// One change to a declaration block sourced from a style node. Zero means "no block".
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiElementDeclarationDelta {
    pub node: u32,
    pub kind: FfiElementDeclarationKind,
    pub old_block: u32,
    pub new_block: u32,
}

/// One exact non-selector style reaction for an element.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiElementStyleInput {
    pub style_node: u32,
    pub reaction: u8,
    pub inherited_style_groups: u8,
}

// SAFETY: the six transaction row types are pointer-free repr(C) with alignment four, and their
// enum fields are recorded only from live FFI values, so raw bytes round-trip on the capturing
// host (the RawRecord contract).
unsafe impl super::record_replay::RawRecord for FfiTreeDelta {}
unsafe impl super::record_replay::RawRecord for FfiElementArrival {}
unsafe impl super::record_replay::RawRecord for FfiLocalFeatureDelta {}
unsafe impl super::record_replay::RawRecord for FfiStateDelta {}
unsafe impl super::record_replay::RawRecord for FfiElementDeclarationDelta {}
unsafe impl super::record_replay::RawRecord for FfiElementStyleInput {}

/// One flat style input transaction. Every array is borrowed for the duration of the call.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStyleInputTransaction {
    pub tree_deltas: *const FfiTreeDelta,
    pub tree_delta_count: usize,
    pub element_arrivals: *const FfiElementArrival,
    pub element_arrival_count: usize,
    pub arrival_custom_state_atoms: *const u32,
    pub arrival_custom_state_atom_count: usize,
    pub local_feature_deltas: *const FfiLocalFeatureDelta,
    pub local_feature_delta_count: usize,
    pub state_deltas: *const FfiStateDelta,
    pub state_delta_count: usize,
    pub element_declaration_deltas: *const FfiElementDeclarationDelta,
    pub element_declaration_delta_count: usize,
    pub element_style_inputs: *const FfiElementStyleInput,
    pub element_style_input_count: usize,
}

/// Device class selecting the document's memory budget coefficients.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiDeviceClass {
    ForegroundDesktop = 0,
}

impl FfiDeviceClass {
    fn decode(self) -> DeviceClass {
        match self {
            Self::ForegroundDesktop => DeviceClass::ForegroundDesktop,
        }
    }
}

/// Cascade origin of a sheet, as the boundary names it.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiCascadeOrigin {
    Author = 0,
    AuthorPresentationalHint = 1,
    User = 2,
    UserAgent = 3,
}

impl FfiCascadeOrigin {
    fn decode(self) -> CascadeOrigin {
        match self {
            Self::Author => CascadeOrigin::Author,
            Self::AuthorPresentationalHint => CascadeOrigin::AuthorPresentationalHint,
            Self::User => CascadeOrigin::User,
            Self::UserAgent => CascadeOrigin::UserAgent,
        }
    }
}

fn decode_feature_key(delta: &FfiLocalFeatureDelta) -> LocalFeatureKey {
    match delta.feature_kind {
        FfiFeatureKind::TagName => LocalFeatureKey::TagName,
        FfiFeatureKind::FoldedTagName => LocalFeatureKey::FoldedTagName,
        FfiFeatureKind::Emptiness => LocalFeatureKey::Emptiness,
        FfiFeatureKind::Id => LocalFeatureKey::Id,
        FfiFeatureKind::Class => LocalFeatureKey::Class(StyleAtomID(delta.name_atom)),
        FfiFeatureKind::Attribute => LocalFeatureKey::Attribute(StyleAtomID(delta.name_atom)),
    }
}

fn decode_feature_value(kind: FfiFeatureValueKind, atom: u32) -> FeatureValue {
    match kind {
        FfiFeatureValueKind::Absent => FeatureValue::Absent,
        FfiFeatureValueKind::Present => FeatureValue::Present,
        FfiFeatureValueKind::Atom => FeatureValue::Atom(StyleAtomID(atom)),
        FfiFeatureValueKind::ChangedValue => FeatureValue::ChangedValue,
    }
}

fn decode_element_declaration_kind(kind: FfiElementDeclarationKind) -> ElementDeclarationKind {
    match kind {
        FfiElementDeclarationKind::InlineStyle => ElementDeclarationKind::InlineStyle,
        FfiElementDeclarationKind::PresentationalHint => ElementDeclarationKind::PresentationalHint,
        FfiElementDeclarationKind::SvgPresentationAttribute => ElementDeclarationKind::SvgPresentationAttribute,
    }
}

fn write_recording_atom_mappings(engine: &StyleEngine, payload: &mut super::record_replay::PayloadWriter) {
    let mappings = engine.recording_atom_mappings();
    enum Mapping {
        Atom { token: u64, atom: u32 },
        Qualified { namespace: u32, name: u32, atom: u32 },
    }
    let mut ordered = mappings
        .atoms
        .into_iter()
        .map(|(token, atom)| Mapping::Atom { token, atom })
        .chain(
            mappings
                .qualified_atoms
                .into_iter()
                .map(|(namespace, name, atom)| Mapping::Qualified { namespace, name, atom }),
        )
        .collect::<Vec<_>>();
    ordered.sort_unstable_by_key(|mapping| match mapping {
        Mapping::Atom { atom, .. } | Mapping::Qualified { atom, .. } => *atom,
    });
    payload.write_length(ordered.len());
    for mapping in ordered {
        match mapping {
            Mapping::Atom { token, atom } => {
                payload.write_u8(0);
                payload.write_u64(token);
                payload.write_u32(atom);
            }
            Mapping::Qualified { namespace, name, atom } => {
                payload.write_u8(1);
                payload.write_u32(namespace);
                payload.write_u32(name);
                payload.write_u32(atom);
            }
        }
    }
}

fn write_declared_properties(declared: &[DeclaredProperty], payload: &mut super::record_replay::PayloadWriter) {
    payload.write_length(declared.len());
    for property in declared {
        payload.write_u16(property.property);
        payload.write_bool(property.important);
        payload.write_u8(match property.operator {
            CascadeOperator::Declared => 0,
            CascadeOperator::Inherit => 1,
            CascadeOperator::Initial => 2,
            CascadeOperator::Unset => 3,
            CascadeOperator::Revert => 4,
            CascadeOperator::RevertLayer => 5,
        });
        payload.write_u64(property.value.0);
    }
}

fn write_exact_cascade_publication(
    publication: FfiExactCascadePublication,
    payload: &mut super::record_replay::PayloadWriter,
) {
    payload.write_u32(publication.computed_group_mask);
    payload.write_u64(publication.computed_property_word_0);
    payload.write_u64(publication.computed_property_word_1);
    payload.write_u64(publication.computed_property_word_2);
    payload.write_u64(publication.computed_property_word_3);
    payload.write_u64(publication.computed_property_word_4);
    payload.write_u64(publication.computed_property_word_5);
    payload.write_bool(publication.computed_property_closure_is_exact);
    payload.write_bool(publication.unchanged);
}

impl StyleEngine {
    fn install_ffi_retained_cascade_assignments(
        &mut self,
        assignments: Vec<crate::css::cascaded_properties::FfiSourceSlotAssignment>,
    ) -> FfiSourceSlotAssignmentView {
        let bytes =
            (assignments.capacity() * size_of::<crate::css::cascaded_properties::FfiSourceSlotAssignment>()) as u64;
        self.ffi_retained_cascade_assignments = assignments;
        self.ffi_retained_cascade_assignments_memory
            .resize_required_to(&mut self.memory, bytes);
        FfiSourceSlotAssignmentView {
            assignments: self.ffi_retained_cascade_assignments.as_ptr().cast(),
            count: self.ffi_retained_cascade_assignments.len(),
        }
    }

    fn clear_ffi_retained_cascade_assignments(&mut self) {
        self.ffi_retained_cascade_assignments = Vec::new();
        self.ffi_retained_cascade_assignments_memory.shrink_to(0);
    }

    fn install_ffi_flat_tree_descendants(&mut self, descendants: Vec<u32>) -> FfiStyleNodeSlice {
        let bytes = (descendants.capacity() * size_of::<u32>()) as u64;
        self.ffi_flat_tree_descendants = descendants;
        self.ffi_flat_tree_descendants_memory
            .resize_required_to(&mut self.memory, bytes);
        FfiStyleNodeSlice {
            nodes: self.ffi_flat_tree_descendants.as_ptr(),
            count: self.ffi_flat_tree_descendants.len(),
        }
    }

    fn clear_ffi_flat_tree_descendants(&mut self) {
        self.ffi_flat_tree_descendants = Vec::new();
        self.ffi_flat_tree_descendants_memory.shrink_to(0);
    }

    fn install_ffi_style_transaction_output(&mut self, output: FfiStyleTransactionOutput) {
        let bytes = (output.answers.capacity() * size_of::<FfiStyleDelta>()
            + output.reclaimed_style_atoms.capacity() * size_of::<FfiReclaimedStyleAtom>()) as u64;
        self.ffi_style_transaction_output = output;
        self.ffi_style_transaction_output_memory
            .resize_required_to(&mut self.memory, bytes);
    }

    pub(super) fn clear_ffi_style_transaction_output(&mut self) {
        self.ffi_style_transaction_output = FfiStyleTransactionOutput::default();
        self.ffi_style_transaction_output_memory.shrink_to(0);
    }

    /// Apply one flat transaction. Tree deltas are staged in arrival order so derived neighbour
    /// rows follow the live tree step by step, while the journal normalizes for discovery.
    pub fn apply_transaction_batch(
        &mut self,
        tree_deltas: &[FfiTreeDelta],
        arrival_columns: (&[FfiElementArrival], &[u32]),
        local_feature_deltas: &[FfiLocalFeatureDelta],
        state_deltas: &[FfiStateDelta],
        element_declaration_deltas: &[FfiElementDeclarationDelta],
        element_style_inputs: &[FfiElementStyleInput],
    ) {
        let (element_arrivals, arrival_custom_state_atoms) = arrival_columns;
        let largest_element_index = tree_deltas
            .iter()
            .filter_map(|delta| StyleNodeID::from_raw(delta.node)?.element_index())
            .max()
            .unwrap_or(0);
        let mut arriving_nodes = vec![false; largest_element_index as usize + 1];
        let mut initial_tree_was_bulk_loaded = false;
        if self.can_bulk_load_initial_tree() && !tree_deltas.is_empty() {
            let mut initial_arrivals = Vec::with_capacity(tree_deltas.len());
            let mut initial_document_root = None;
            let mut can_bulk_load_initial_tree = true;
            for delta in tree_deltas {
                let Some(node) = StyleNodeID::from_raw(delta.node) else {
                    can_bulk_load_initial_tree = false;
                    continue;
                };
                let Some(index) = node.element_index() else {
                    can_bulk_load_initial_tree = false;
                    continue;
                };
                let is_unique_arrival = !delta.old_connected && delta.new_connected && !arriving_nodes[index as usize];
                can_bulk_load_initial_tree &= is_unique_arrival;
                arriving_nodes[index as usize] = is_unique_arrival;
                if is_unique_arrival {
                    let relations = delta.new_relations.decode();
                    if relations.parent.is_none() && relations.tree_scope == TreeScopeID::DOCUMENT {
                        can_bulk_load_initial_tree &= initial_document_root.replace(node).is_none();
                    }
                    initial_arrivals.push((node, relations));
                }
            }
            if can_bulk_load_initial_tree && let Some(document_root) = initial_document_root {
                self.bulk_load_initial_tree(document_root, &initial_arrivals);
                initial_tree_was_bulk_loaded = true;
            }
        }
        if !initial_tree_was_bulk_loaded {
            self.initial_tree_batch_applied |= !tree_deltas.is_empty();
            for delta in tree_deltas {
                let Some(node) = StyleNodeID::from_raw(delta.node) else {
                    continue;
                };
                let old = delta.old_connected.then(|| delta.old_relations.decode());
                let new = delta.new_connected.then(|| delta.new_relations.decode());
                self.record_tree_delta(node, old, new);
            }
            for delta in tree_deltas {
                let Some(node) = StyleNodeID::from_raw(delta.node) else {
                    continue;
                };
                let Some(index) = node.element_index() else {
                    continue;
                };
                arriving_nodes[index as usize] = self.node_arrival_is_pending(node);
            }
        }
        let node_is_arriving = |node: StyleNodeID| {
            node.element_index()
                .and_then(|index| arriving_nodes.get(index as usize))
                .copied()
                .unwrap_or(false)
        };

        if !element_arrivals.is_empty() {
            for arrival in element_arrivals {
                let Some(node) = StyleNodeID::from_raw(arrival.node) else {
                    debug_assert!(false, "an element arrival named an invalid style node");
                    continue;
                };
                let Some(custom_state_end) = arrival.custom_state_offset.checked_add(arrival.custom_state_count) else {
                    debug_assert!(false, "an element arrival custom-state range overflowed");
                    continue;
                };
                let Ok(custom_state_range) = usize::try_from(arrival.custom_state_offset)
                    .and_then(|start| usize::try_from(custom_state_end).map(|end| start..end))
                else {
                    debug_assert!(false, "an element arrival custom-state range exceeded usize");
                    continue;
                };
                let Some(custom_states) = arrival_custom_state_atoms.get(custom_state_range) else {
                    debug_assert!(
                        false,
                        "an element arrival named custom states outside the shared atom column"
                    );
                    continue;
                };
                let custom_states = custom_states.iter().copied().map(StyleAtomID).collect::<Vec<_>>();
                self.record_element_arrival(node, arrival, &custom_states, node_is_arriving(node));
            }
            self.settle_batched_inputs();
        }

        for delta in local_feature_deltas {
            let Some(node) = StyleNodeID::from_raw(delta.node) else {
                continue;
            };
            self.record_batched_input(
                InputKey::LocalFeature(node, decode_feature_key(delta)),
                InputValue::Feature(decode_feature_value(delta.old_kind, delta.old_atom)),
                InputValue::Feature(decode_feature_value(delta.new_kind, delta.new_atom)),
                node_is_arriving(node),
            );
        }
        self.settle_batched_inputs();

        for delta in state_deltas {
            let Some(node) = StyleNodeID::from_raw(delta.node) else {
                continue;
            };
            self.record_batched_state(
                node,
                decode_state_fact(delta.fact),
                delta.new_value,
                node_is_arriving(node),
            );
        }
        self.settle_batched_inputs();

        for delta in element_declaration_deltas {
            let Some(node) = StyleNodeID::from_raw(delta.node) else {
                continue;
            };
            // Zero means the node has no declaration block of that kind on that side.
            let block = |raw: u32| (raw != 0).then_some(DeclarationBlockID(raw));
            self.record_input(
                InputKey::ElementDeclaration(node, decode_element_declaration_kind(delta.kind)),
                InputValue::ElementDeclaration(block(delta.old_block)),
                InputValue::ElementDeclaration(block(delta.new_block)),
            );
        }
        for input in element_style_inputs {
            let Some(node) = StyleNodeID::from_raw(input.style_node) else {
                continue;
            };
            self.record_input(
                InputKey::ElementStyleInput(node),
                InputValue::ElementStyleInput {
                    reaction: 0,
                    inherited_style_groups: 0,
                },
                InputValue::ElementStyleInput {
                    reaction: input.reaction,
                    inherited_style_groups: input.inherited_style_groups,
                },
            );
        }
    }
}

/// Creates one document's style engine.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_create(device_class: FfiDeviceClass) -> *mut c_void {
    abort_on_panic(|| {
        let device_class = device_class.decode();
        let mut engine = Box::new(StyleEngine::new(device_class));
        engine.begin_recording(device_class);
        engine.record_boundary_call(EventKind::SetComputedGroupDependencyMasks, |payload| {
            let mapping = crate::css::computed_values::property_dependency_masks_snapshot();
            payload.write_bool(mapping.is_some());
            if let Some((first_property, masks, output_masks)) = mapping {
                payload.write_u16(first_property);
                payload.write_u32_slice(masks);
                payload.write_u32_slice(output_masks);
            }
        });
        Box::into_raw(engine).cast()
    })
}

/// Installs the C++ ownership callbacks used by live process-global raw atoms.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_install_raw_atom_callbacks(
    retain: unsafe extern "C" fn(usize),
    release: unsafe extern "C" fn(usize),
) {
    super::atoms::install_raw_atom_callbacks(retain, release);
}

/// Creates a replay engine whose atom keys are opaque capture tokens rather than live fly strings.
pub fn style_engine_create_for_replay(device_class: FfiDeviceClass) -> *mut c_void {
    abort_on_panic(|| Box::into_raw(Box::new(StyleEngine::new_for_replay(device_class.decode()))).cast())
}

/// Applies the memory policy used while producing a replay recording.
///
/// # Safety
/// `engine` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_use_recording_memory_policy(engine: *mut c_void) {
    abort_on_panic(|| {
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        engine.memory.enable_recording_policy();
    });
}

/// Accounts C++ substitution-cache capacity against this document's acceleration budget.
///
/// # Safety
/// `engine` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_resize_parsed_substitution_cache(engine: *mut c_void, bytes: u64) -> bool {
    abort_on_panic(|| {
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        engine.resize_parsed_substitution_cache(bytes)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn style_engine_verification_gate_bits() -> u8 {
    super::verification_gate_bits()
}

#[unsafe(no_mangle)]
pub extern "C" fn style_engine_recording_pointer_will_die(pointer: *const c_void) {
    super::record_replay::invalidate_pointer(pointer as usize);
}
/// # Safety
/// `engine` must be a pointer returned by `style_engine_create` and not yet destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_destroy(engine: *mut c_void) {
    abort_on_panic(|| {
        let mut engine = unsafe { Box::from_raw(engine.cast::<StyleEngine>()) };
        engine.end_recording();
    });
}

/// # Safety
/// `engine` must be live, and `out` must point at `count` writable `u32` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_allocate_style_nodes(engine: *mut c_void, out: *mut u32, count: usize) {
    abort_on_panic(|| {
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        let out = if count == 0 {
            &mut []
        } else {
            unsafe { std::slice::from_raw_parts_mut(out, count) }
        };
        engine.allocate_style_nodes(out);
        engine.record_boundary_call(EventKind::AllocateStyleNodes, |payload| payload.write_u32_slice(out));
    });
}

/// Returns the live element descendants whose inheritance path begins at `root` in the flat tree.
///
/// # Safety
/// `engine` must be live. The returned node slice remains valid until the next mutable
/// `style_engine_*` entry point or an explicit discard of the flat-tree descendants.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_flat_tree_descendants(engine: *mut c_void, root: u32) -> FfiStyleNodeSlice {
    abort_on_panic(|| {
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        engine.clear_ffi_flat_tree_descendants();
        let Some(root) = StyleNodeID::from_raw(root) else {
            return FfiStyleNodeSlice::default();
        };
        let mut descendants = Vec::new();
        engine.for_each_flat_tree_descendant(root, |node| {
            descendants.push(node.raw());
        });
        engine.record_boundary_call(EventKind::ForEachFlatTreeDescendant, |payload| {
            payload.write_u32(root.raw());
            payload.write_u32_slice(&descendants);
        });
        engine.install_ffi_flat_tree_descendants(descendants)
    })
}

/// Discards the borrowed flat-tree descendant slice.
///
/// # Safety
/// `engine` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_discard_flat_tree_descendants(engine: *mut c_void) {
    abort_on_panic(|| {
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        engine.clear_ffi_flat_tree_descendants();
    });
}
/// Applies one flat style input transaction.
///
/// # Safety
/// `engine` must be live and every array in `transaction` must point at its stated number of valid
/// records for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_apply_transaction(engine: *mut c_void, transaction: &FfiStyleInputTransaction) {
    abort_on_panic(|| {
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        // SAFETY: the caller vouches that each pointer covers its stated count for this call.
        let tree = unsafe { borrow(transaction.tree_deltas, transaction.tree_delta_count) };
        let arrivals = unsafe { borrow(transaction.element_arrivals, transaction.element_arrival_count) };
        let arrival_custom_state_atoms = unsafe {
            borrow(
                transaction.arrival_custom_state_atoms,
                transaction.arrival_custom_state_atom_count,
            )
        };
        let features = unsafe { borrow(transaction.local_feature_deltas, transaction.local_feature_delta_count) };
        let states = unsafe { borrow(transaction.state_deltas, transaction.state_delta_count) };
        let declarations = unsafe {
            borrow(
                transaction.element_declaration_deltas,
                transaction.element_declaration_delta_count,
            )
        };
        let element_style_inputs =
            unsafe { borrow(transaction.element_style_inputs, transaction.element_style_input_count) };
        engine.apply_transaction_batch(
            tree,
            (arrivals, arrival_custom_state_atoms),
            features,
            states,
            declarations,
            element_style_inputs,
        );
        engine.record_boundary_call(EventKind::ApplyTransaction, |payload| {
            write_recording_tree_deltas(tree, payload);
            payload.write_raw_slice(arrivals);
            payload.write_u32_slice(arrival_custom_state_atoms);
            payload.write_raw_slice(features);
            write_recording_state_deltas(states, payload);
            payload.write_raw_slice(declarations);
            write_recording_element_style_inputs(element_style_inputs, payload);
        });
    });
}

fn write_recording_tree_deltas(tree: &[FfiTreeDelta], payload: &mut super::record_replay::PayloadWriter) {
    payload.write_raw_rows(
        tree.len(),
        size_of::<FfiTreeDelta>(),
        align_of::<FfiTreeDelta>(),
        |payload| {
            for delta in tree {
                payload.write_native_u32(delta.node);
                payload.write_bool(delta.old_connected);
                payload.write_bool(delta.new_connected);
                payload.write_native_u16(0);
                write_recording_tree_relations(delta.old_relations, payload);
                write_recording_tree_relations(delta.new_relations, payload);
            }
        },
    );
}

fn write_recording_tree_relations(relations: FfiTreeRelations, payload: &mut super::record_replay::PayloadWriter) {
    payload.write_native_u32(relations.parent);
    payload.write_native_u32(relations.previous_element_sibling);
    payload.write_native_u32(relations.next_element_sibling);
    payload.write_native_u32(relations.tree_scope);
    payload.write_native_u32(relations.assigned_slot);
    payload.write_native_u32(relations.reserved);
}

fn write_recording_state_deltas(state_deltas: &[FfiStateDelta], payload: &mut super::record_replay::PayloadWriter) {
    payload.write_raw_rows(
        state_deltas.len(),
        size_of::<FfiStateDelta>(),
        align_of::<FfiStateDelta>(),
        |payload| {
            for delta in state_deltas {
                payload.write_native_u32(delta.node);
                payload.write_u8(delta.fact as u8);
                payload.write_bool(delta.new_value);
                payload.write_native_u16(0);
            }
        },
    );
}

fn write_recording_element_style_inputs(
    element_style_inputs: &[FfiElementStyleInput],
    payload: &mut super::record_replay::PayloadWriter,
) {
    payload.write_raw_rows(
        element_style_inputs.len(),
        size_of::<FfiElementStyleInput>(),
        align_of::<FfiElementStyleInput>(),
        |payload| {
            for input in element_style_inputs {
                payload.write_native_u32(input.style_node);
                payload.write_u8(input.reaction);
                payload.write_u8(input.inherited_style_groups);
                payload.write_native_u16(0);
            }
        },
    );
}
/// Compiles one style rule's selector list into the program.
///
/// `selectors` points at `count` `RustSelector` pointers, each owned by the C++ selector it was
/// created from and alive for this call.
///
/// # Safety
/// `engine` must be live, `sheet` must have come from `style_engine_add_sheet`, and the array must
/// hold `count` valid non-null pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_add_style_rule(
    engine: *mut c_void,
    sheet: u32,
    before_rule: u32,
    selectors: *const *const c_void,
    count: usize,
    default_namespace: u32,
    namespace_prefixes: *const u32,
    namespace_uris: *const u32,
    namespace_count: usize,
    scope_roots: *const *const c_void,
    scope_root_count: usize,
    scope_limits: *const *const c_void,
    scope_limit_count: usize,
    scope_level_root_counts: *const u32,
    scope_level_limit_counts: *const u32,
    scope_level_implicit_roots: *const u32,
    scope_level_count: usize,
) -> u32 {
    abort_on_panic(|| {
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        if sheet == 0 || count == 0 {
            engine.record_boundary_call(EventKind::AddStyleRule, |payload| {
                payload.write_u32(sheet);
                payload.write_u32(before_rule);
                payload.write_u32(0);
            });
            return 0;
        }
        let compiled = unsafe { borrow_selectors(selectors, count) };
        if compiled.is_empty() {
            engine.record_boundary_call(EventKind::AddStyleRule, |payload| {
                payload.write_u32(sheet);
                payload.write_u32(before_rule);
                payload.write_u32(0);
            });
            return 0;
        }
        let scope = unsafe { borrow_selectors(scope_roots, scope_root_count) };
        let limits = unsafe { borrow_selectors(scope_limits, scope_limit_count) };
        let levels =
            unsafe { borrow_scope_levels(scope_level_root_counts, scope_level_limit_counts, scope_level_count) };
        let implicit_roots = unsafe { borrow_implicit_scope_roots(scope_level_implicit_roots, scope_level_count) };
        let before = match before_rule {
            0 => None,
            id => Some(RuleID(id - 1)),
        };
        let namespaces =
            unsafe { borrow_namespace_scope(default_namespace, namespace_prefixes, namespace_uris, namespace_count) };
        let scope = ScopeChain {
            roots: &scope,
            limits: &limits,
            levels: &levels,
            implicit_roots: &implicit_roots,
        };
        let rule = engine.add_style_rule_in_scope(SheetID(sheet - 1), before, &compiled, namespaces, &scope);
        let result = rule.0 + 1;
        engine.record_boundary_call(EventKind::AddStyleRule, |payload| {
            payload.write_u32(sheet);
            payload.write_u32(before_rule);
            payload.write_u32(result);
            write_recording_atom_mappings(engine, payload);
            super::selector::replay::write(engine.selector_program_for_rule(rule), payload);
        });
        result
    })
}

/// Installs one already compiled semantic selector program.
///
/// # Safety
/// `engine` must be live, and the sheet and optional rule handles must belong to it.
#[cfg(feature = "style-recording")]
pub unsafe fn replay_add_style_rule(
    engine: *mut c_void,
    sheet: u32,
    before_rule: u32,
    selector_program: super::selector::SelectorProgram,
) -> u32 {
    let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
    if sheet == 0 {
        return 0;
    }
    let before = (before_rule != 0).then(|| RuleID(before_rule - 1));
    engine
        .add_replayed_style_rule(SheetID(sheet - 1), before, selector_program)
        .0
        + 1
}

/// Replaces one selector list with an already compiled semantic program.
///
/// # Safety
/// `engine` must be live and `rule` must name one of its style rules.
#[cfg(feature = "style-recording")]
pub unsafe fn replay_replace_style_rule_selectors(
    engine: *mut c_void,
    rule: u32,
    selector_program: super::selector::SelectorProgram,
) {
    let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
    engine.replace_replayed_style_rule_selectors(RuleID(rule - 1), selector_program);
}

/// Installs semantic declared-property identities without C++ style-value pointers.
///
/// # Safety
/// `engine` must be live and `node` must name one of its style nodes.
#[cfg(feature = "style-recording")]
pub unsafe fn replay_set_element_declared_properties(
    engine: *mut c_void,
    node: u32,
    kind: FfiElementDeclarationKind,
    declared: &[DeclaredProperty],
    declarations_are_complete: bool,
) {
    let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
    let node = StyleNodeID::from_raw(node).expect("recorded style node identities are nonzero");
    engine.set_element_declared_properties(
        node,
        decode_element_declaration_kind(kind),
        declared,
        declarations_are_complete,
    );
}

/// Installs semantic declared-property identities without C++ style-value pointers.
///
/// # Safety
/// `engine` must be live and `rule` must name one of its style rules.
#[cfg(feature = "style-recording")]
pub unsafe fn replay_set_rule_declared_properties(
    engine: *mut c_void,
    rule: u32,
    declared: &[DeclaredProperty],
    declarations_are_complete: bool,
) {
    let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
    engine.set_rule_declared_properties_with_operators(RuleID(rule - 1), declared, declarations_are_complete);
}

/// Gives an existing style rule a new selector list, keeping its identity and its position.
///
/// # Safety
/// `engine` must be live, and the selector pointers must point to live `RustSelector`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_replace_style_rule_selectors(
    engine: *mut c_void,
    rule: u32,
    selectors: *const *const c_void,
    count: usize,
    default_namespace: u32,
    namespace_prefixes: *const u32,
    namespace_uris: *const u32,
    namespace_count: usize,
    scope_roots: *const *const c_void,
    scope_root_count: usize,
    scope_limits: *const *const c_void,
    scope_limit_count: usize,
    scope_level_root_counts: *const u32,
    scope_level_limit_counts: *const u32,
    scope_level_implicit_roots: *const u32,
    scope_level_count: usize,
) {
    abort_on_panic(|| {
        if rule == 0 || count == 0 {
            return;
        }
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        let compiled = unsafe { borrow_selectors(selectors, count) };
        if compiled.is_empty() {
            return;
        }
        let scope = unsafe { borrow_selectors(scope_roots, scope_root_count) };
        let limits = unsafe { borrow_selectors(scope_limits, scope_limit_count) };
        let levels =
            unsafe { borrow_scope_levels(scope_level_root_counts, scope_level_limit_counts, scope_level_count) };
        let implicit_roots = unsafe { borrow_implicit_scope_roots(scope_level_implicit_roots, scope_level_count) };
        let namespaces =
            unsafe { borrow_namespace_scope(default_namespace, namespace_prefixes, namespace_uris, namespace_count) };
        let scope = ScopeChain {
            roots: &scope,
            limits: &limits,
            levels: &levels,
            implicit_roots: &implicit_roots,
        };
        engine.replace_style_rule_selectors(RuleID(rule - 1), &compiled, namespaces, &scope);
        engine.record_boundary_call(EventKind::ReplaceStyleRuleSelectors, |payload| {
            payload.write_u32(rule);
            write_recording_atom_mappings(engine, payload);
            super::selector::replay::write(engine.selector_program_for_rule(RuleID(rule - 1)), payload);
        });
    });
}
/// Records the shadow parts an element exposes and which host each name reaches.
///
/// # Safety
/// `engine` must be live, and `names` and `hosts` must each point to `count` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_element_parts(
    engine: *mut c_void,
    node: u32,
    names: *const u32,
    hosts: *const u32,
    count: usize,
) {
    abort_on_panic(|| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return;
        };
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        let pairs: Vec<(StyleAtomID, StyleNodeID)> = match count == 0 || names.is_null() || hosts.is_null() {
            true => Vec::new(),
            false => {
                let names = unsafe { std::slice::from_raw_parts(names, count) };
                let hosts = unsafe { std::slice::from_raw_parts(hosts, count) };
                names
                    .iter()
                    .zip(hosts.iter())
                    .filter_map(|(name, host)| StyleNodeID::from_raw(*host).map(|host| (StyleAtomID(*name), host)))
                    .collect()
            }
        };
        engine.set_element_parts(node, &pairs);
        engine.record_boundary_call(EventKind::SetElementParts, |payload| {
            payload.write_u32(node.raw());
            payload.write_length(pairs.len());
            for (name, host) in &pairs {
                payload.write_u32(name.0);
                payload.write_u32(host.raw());
            }
        });
    });
}
/// Records the element's resolved language, as its primary subtag atom.
///
/// # Safety
/// `engine` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_element_language(
    engine: *mut c_void,
    node: u32,
    language: u32,
    text: *const u16,
    text_length: usize,
) {
    abort_on_panic(|| {
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        let text = match language != 0 && !text.is_null() {
            true => unsafe { std::slice::from_raw_parts(text, text_length) },
            false => &[],
        };
        if language != 0 && !text.is_empty() {
            // A range is not a name, so `:lang()` compares against the tag itself. It is recorded
            // once per language rather than once per element.
            engine.set_element_language_text(StyleAtomID(language), text);
        }
        if let Some(node) = StyleNodeID::from_raw(node) {
            engine.set_element_language(node, StyleAtomID(language));
        }
        engine.record_boundary_call(EventKind::SetElementLanguage, |payload| {
            payload.write_u32(node);
            payload.write_u32(language);
            payload.write_u16_slice(text);
        });
    });
}

/// # Safety
/// `pointers` must be null or point to `count` live `RustSelector` pointers.
/// How many scope roots and limits each enclosing `@scope` contributed, outermost first.
///
/// Scopes nest, and an element is in scope only when it is in every one of them, so the compiler
/// needs the boundaries rather than one flat run.
unsafe fn borrow_scope_levels(roots: *const u32, limits: *const u32, count: usize) -> Vec<(u32, u32)> {
    if count == 0 || roots.is_null() || limits.is_null() {
        return Vec::new();
    }
    let roots = unsafe { std::slice::from_raw_parts(roots, count) };
    let limits = unsafe { std::slice::from_raw_parts(limits, count) };
    roots.iter().copied().zip(limits.iter().copied()).collect()
}

/// The element each `@scope` level roots at when it wrote no `<scope-start>`, zero where it wrote
/// one.
///
/// # Safety
/// `roots` must be null or point to `count` live `u32`s.
unsafe fn borrow_implicit_scope_roots(roots: *const u32, count: usize) -> Vec<Option<ImplicitScopeRoot>> {
    if count == 0 || roots.is_null() {
        return Vec::new();
    }
    unsafe { std::slice::from_raw_parts(roots, count) }
        .iter()
        .map(|&raw| match raw {
            0 => None,
            u32::MAX => Some(ImplicitScopeRoot::ContainingTree),
            node => StyleNodeID::from_raw(node).map(ImplicitScopeRoot::Node),
        })
        .collect()
}

unsafe fn borrow_selectors<'a>(pointers: *const *const c_void, count: usize) -> Vec<&'a CompiledSelector> {
    if count == 0 || pointers.is_null() {
        return Vec::new();
    }
    unsafe { std::slice::from_raw_parts(pointers, count) }
        .iter()
        .filter(|pointer| !pointer.is_null())
        .map(|pointer| unsafe { (*pointer.cast::<RustSelector>()).compiled() })
        .collect()
}

/// Compiles one selector list for the DOM query APIs.
///
/// # Safety
/// `engine` must be live, and `selectors` must point at `count` live selector handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_compile_selector_query(
    engine: *mut c_void,
    selectors: *const *const c_void,
    count: usize,
) -> *mut c_void {
    abort_on_panic(|| {
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        let selectors = unsafe { borrow_selectors(selectors, count) };
        let program = engine.compile_selector_query(&selectors);
        let mut atoms = HashSet::default();
        program.collect_atoms(&mut atoms);
        let atoms = engine.atoms.pin(atoms);
        engine.record_boundary_call(EventKind::SelectorQueryAtomMappings, |payload| {
            write_recording_atom_mappings(engine, payload);
        });
        let bytes = program
            .capacity_bytes()
            .saturating_add(std::mem::size_of::<SelectorQuery>() as u64);
        let mut memory = MemoryLease::new(MemoryCategory::SelectorQuery);
        memory.resize_required_to(&mut engine.memory, bytes);
        Box::into_raw(Box::new(SelectorQuery {
            program,
            _atoms: atoms,
            _memory: memory,
        }))
        .cast()
    })
}

/// # Safety
/// `query` must have been returned by `style_engine_compile_selector_query` and not yet destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_destroy_selector_query(query: *mut c_void) {
    abort_on_panic(|| {
        if !query.is_null() {
            drop(unsafe { Box::from_raw(query.cast::<SelectorQuery>()) });
        }
    });
}
/// Matches one resident element against an ad-hoc selector list.
///
/// Returns 0 for no match, 1 for a match, and `u8::MAX` when resident facts do not cover the query.
///
/// # Safety
/// `engine` and `query` must be live and belong to the same document.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_selector_query_matches(
    engine: *mut c_void,
    query: *const c_void,
    node: u32,
    scope_root: u32,
    shadow_root: u32,
) -> u8 {
    unsafe { selector_query_matches_impl(engine, query, node, scope_root, shadow_root, true) }
}

/// Matches one resident element in a engine that has no document root.
///
/// # Safety
/// `engine` and `query` must be live and belong to the same isolated engine.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_selector_query_matches_without_document_root(
    engine: *mut c_void,
    query: *const c_void,
    node: u32,
    scope_root: u32,
    shadow_root: u32,
) -> u8 {
    unsafe { selector_query_matches_impl(engine, query, node, scope_root, shadow_root, false) }
}

unsafe fn selector_query_matches_impl(
    engine: *mut c_void,
    query: *const c_void,
    node: u32,
    scope_root: u32,
    shadow_root: u32,
    has_document_root: bool,
) -> u8 {
    abort_on_panic(|| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return u8::MAX;
        };
        if query.is_null() {
            return u8::MAX;
        }
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        let query = unsafe { &*query.cast::<SelectorQuery>() };
        match engine.selector_query_matches(
            &query.program,
            node,
            StyleNodeID::from_raw(scope_root),
            StyleNodeID::from_raw(shadow_root),
            has_document_root,
        ) {
            Ok(matches) => u8::from(matches),
            Err(_) => u8::MAX,
        }
    })
}

/// The `@namespace` declarations of one sheet, as the boundary carries them.
///
/// A zero default means the sheet declared none, in which case an unprefixed type or universal
/// selector constrains no namespace at all.
///
/// # Safety
/// `prefixes` and `uris` must each point at `count` readable `u32` values, or be null.
unsafe fn borrow_namespace_scope(
    default_namespace: u32,
    prefixes: *const u32,
    uris: *const u32,
    count: usize,
) -> NamespaceScope {
    let by_prefix = match prefixes.is_null() || uris.is_null() {
        true => Vec::new(),
        false => {
            let prefixes = unsafe { std::slice::from_raw_parts(prefixes, count) };
            let uris = unsafe { std::slice::from_raw_parts(uris, count) };
            prefixes
                .iter()
                .zip(uris)
                .map(|(&prefix, &uri)| (StyleAtomID(prefix), StyleAtomID(uri)))
                .collect()
        }
    };
    NamespaceScope {
        default: match default_namespace {
            0 => None,
            // The empty string, which is the namespace an element in none has.
            u32::MAX => Some(StyleAtomID::NONE),
            atom => Some(StyleAtomID(atom)),
        },
        by_prefix,
    }
}
/// One concrete match, as the boundary carries it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct FfiRuleMatch {
    pub node: u32,
    /// The rule identity as the boundary numbers it: one more than the engine's, so that zero can
    /// mean no rule.
    pub rule: u32,
    /// Collision-checked identity of the rule's complete semantic declaration inventory, or zero
    /// when custom properties or another declaration kind remain on the C++ side.
    pub semantic_declaration: u32,
    /// The pseudo-element the match targets, or `u32::MAX` for the originating element itself.
    pub pseudo_element: u32,
    /// The host whose shadow tree this match decides in, or 0 for the document's own context. How
    /// deeply encapsulated a rule is orders it against the contexts around it, and the scope the
    /// match resolved through is not always the one the element is in: `:host`, `::slotted()` and
    /// `::part()` all decide for an element outside their own tree.
    pub scope_host: u32,
    /// Generational hops from the scoping root the match resolved through, or `u32::MAX` for a rule
    /// that is not scoped. A nearer root wins the proximity comparison.
    pub scope_proximity: u32,
}

unsafe fn write_rule_matches(
    engine: &mut StyleEngine,
    matches: &[RuleMatch],
    out: *mut FfiRuleMatch,
    capacity: usize,
) -> usize {
    if matches.len() > capacity {
        return matches.len();
    }
    for (index, entry) in matches.iter().enumerate() {
        unsafe {
            *out.add(index) = FfiRuleMatch {
                node: entry.node.raw(),
                rule: entry.rule.0 + 1,
                semantic_declaration: engine.program.ensure_semantic_declaration(entry.rule).0,
                pseudo_element: entry.pseudo_element.map_or(u32::MAX, |target| u32::from(target.kind.0)),
                scope_host: engine.cascade_context_host(entry.rule, entry.tree_scope),
                scope_proximity: entry.scope_proximity,
            };
        }
    }
    matches.len()
}

/// Consumes the complete answer published by the immediately preceding style transaction.
///
/// Returns `usize::MAX` when that transaction did not publish an answer for `node`, or a count
/// larger than `capacity` when nothing was written because the buffer was too small.
///
/// # Safety
/// `engine` must be live and `out` must point at `capacity` writable `FfiRuleMatch` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_consume_published_match_answer(
    engine: *mut c_void,
    node: u32,
    out: *mut FfiRuleMatch,
    capacity: usize,
) -> usize {
    abort_on_panic(|| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return usize::MAX;
        };
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        let result = engine
            .consume_published_match_answer_with(
                node,
                capacity,
                |index, node, rule, semantic_declaration, pseudo_element, scope_host, scope_proximity| unsafe {
                    *out.add(index) = FfiRuleMatch {
                        node: node.raw(),
                        rule: rule.0 + 1,
                        semantic_declaration: semantic_declaration.0,
                        pseudo_element: pseudo_element.map_or(u32::MAX, |target| u32::from(target.kind.0)),
                        scope_host,
                        scope_proximity,
                    };
                },
            )
            .unwrap_or(usize::MAX);
        engine.record_boundary_call(EventKind::ConsumePublishedMatchAnswer, |payload| {
            payload.write_u32(node.raw());
            payload.write_u64(u64::try_from(capacity).expect("match capacity exceeds u64"));
            payload.write_u64(u64::try_from(result).expect("match count exceeds u64"));
            let written = result != usize::MAX && result <= capacity;
            payload.write_bool(written);
            if written {
                let matches = unsafe { std::slice::from_raw_parts(out, result) };
                payload.write_length(matches.len());
                for entry in matches {
                    payload.write_u32(entry.node);
                    payload.write_u32(entry.rule);
                    payload.write_u32(entry.pseudo_element);
                    payload.write_u32(entry.scope_host);
                    payload.write_u32(entry.scope_proximity);
                }
            }
        });
        result
    })
}
/// Matches one element and writes its matches, in cascade order, into `out`.
///
/// The same answer the document pass gives for that element, asked one element at a time, which is
/// what a style recompute needs.
///
/// Returns the number written, `usize::MAX` when the pass could not complete, or a count larger
/// than `capacity` when nothing was written because the buffer was too small.
///
/// # Safety
/// `engine` must be live and `out` must point at `capacity` writable `FfiRuleMatch` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_match_element(
    engine: *mut c_void,
    node: u32,
    out: *mut FfiRuleMatch,
    capacity: usize,
    compact_for_cascade: bool,
) -> usize {
    abort_on_panic(|| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return usize::MAX;
        };
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        let result = match compact_for_cascade {
            true => engine.match_element_for_cascade(node),
            false => engine.match_element(node),
        };
        let Ok(matches) = result else {
            return usize::MAX;
        };
        let result = unsafe { write_rule_matches(engine, &matches, out, capacity) };
        engine.record_boundary_call(EventKind::MatchElement, |payload| {
            payload.write_u32(node.raw());
            payload.write_u64(u64::try_from(capacity).expect("match capacity exceeds u64"));
            payload.write_bool(compact_for_cascade);
            payload.write_u64(u64::try_from(result).expect("match count exceeds u64"));
            let written = result != usize::MAX && result <= capacity;
            payload.write_bool(written);
            if written {
                let matches = unsafe { std::slice::from_raw_parts(out, result) };
                payload.write_length(matches.len());
                for entry in matches {
                    payload.write_u32(entry.node);
                    payload.write_u32(entry.rule);
                    payload.write_u32(entry.pseudo_element);
                    payload.write_u32(entry.scope_host);
                    payload.write_u32(entry.scope_proximity);
                }
            }
        });
        result
    })
}
/// Records which longhand properties one of an element's own declarations covers.
///
/// # Safety
/// `engine` must be live, all arrays must have `count` readable entries, and every canonical and
/// original value must point at live Rust-owned style value data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_element_declared_properties(
    engine: *mut c_void,
    node: u32,
    kind: FfiElementDeclarationKind,
    properties: *const u16,
    important: *const bool,
    operators: *const FfiCascadeOperator,
    values: *const *const c_void,
    original_values: *const *const c_void,
    count: usize,
    declarations_are_complete: bool,
) {
    abort_on_panic(|| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return;
        };
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        let declared: Vec<DeclaredProperty> = match count == 0 {
            true => Vec::new(),
            false => {
                if properties.is_null()
                    || important.is_null()
                    || operators.is_null()
                    || values.is_null()
                    || original_values.is_null()
                {
                    return;
                }
                let properties = unsafe { std::slice::from_raw_parts(properties, count) };
                let important = unsafe { std::slice::from_raw_parts(important, count) };
                let operators = unsafe { std::slice::from_raw_parts(operators, count) };
                let values = unsafe { std::slice::from_raw_parts(values, count) };
                let original_values = unsafe { std::slice::from_raw_parts(original_values, count) };
                properties
                    .iter()
                    .copied()
                    .zip(important.iter().copied())
                    .zip(operators.iter().copied())
                    .zip(values.iter().copied().zip(original_values.iter().copied()))
                    .map(|(((property, important), operator), (value, original_value))| {
                        let specified_value = unsafe { engine.intern_specified_value(value.cast()) };
                        unsafe { engine.alias_specified_value(original_value.cast(), specified_value) };
                        DeclaredProperty {
                            property,
                            important,
                            operator: operator.decode(),
                            value: specified_value,
                        }
                    })
                    .collect()
            }
        };
        engine.set_element_declared_properties(
            node,
            decode_element_declaration_kind(kind),
            &declared,
            declarations_are_complete,
        );
        engine.record_boundary_call(EventKind::SetElementDeclaredProperties, |payload| {
            payload.write_u32(node.raw());
            payload.write_u8(kind as u8);
            payload.write_bool(declarations_are_complete);
            write_declared_properties(&declared, payload);
        });
    });
}

/// # Safety
/// `engine` and `store` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_publish_exact_cascade_state(
    engine: *mut c_void,
    node: u32,
    pseudo_kind: u8,
    store: *const c_void,
    inherited_style_groups: u8,
) -> FfiExactCascadePublication {
    abort_on_panic(|| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return FfiExactCascadePublication::missing();
        };
        if store.is_null() {
            return FfiExactCascadePublication::missing();
        }
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        let generation_snapshot =
            engine.exact_cascade_generation_snapshot(super::computed::ComputedStyleTarget::new(node, pseudo_kind));
        let (publication, winners, had_previous) = engine.publish_exact_cascade_state(
            super::computed::ComputedStyleTarget::new(node, pseudo_kind),
            unsafe { &*store.cast::<crate::css::cascaded_properties::CascadedPropertyStore>() },
            inherited_style_groups,
        );
        engine.record_boundary_call(EventKind::PublishExactCascadeState, |payload| {
            payload.write_u32(node.raw());
            payload.write_u8(pseudo_kind);
            payload.write_u8(inherited_style_groups);
            payload.write_u64(generation_snapshot.0);
            payload.write_bool(generation_snapshot.1.is_some());
            if let Some(generation) = generation_snapshot.1 {
                payload.write_u64(generation);
            }
            payload.write_length(winners.len());
            for (property, winner) in winners {
                payload.write_u16(property);
                payload.write_u64(winner.value.0);
                payload.write_u8(match winner.operator {
                    CascadeOperator::Declared => 0,
                    CascadeOperator::Inherit => 1,
                    CascadeOperator::Initial => 2,
                    CascadeOperator::Unset => 3,
                    CascadeOperator::Revert => 4,
                    CascadeOperator::RevertLayer => 5,
                });
                payload.write_u32(winner.animation_relevance);
            }
            write_exact_cascade_publication(publication, payload);
            // NB: Whether a previous cascade state was retained decides the publication's group
            //     mask, and retention differs legitimately between the recording session and a
            //     replay (memory pressure evicts). Record it so replay can compare accordingly;
            //     older captures simply end before this byte.
            payload.write_bool(had_previous);
        });
        publication
    })
}

/// Seed the ordinary cascade store from a complete retained winner relation.
///
/// # Safety
/// All pointers must be live and `blocks` must describe `block_count` entries. The returned
/// assignment slice remains valid until the next mutable `style_engine_*` entry point or an
/// explicit discard of the retained cascade assignments.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_materialize_retained_cascade_state(
    engine: *mut c_void,
    node: u32,
    pseudo_kind: u8,
    store: *mut c_void,
    blocks: *const c_void,
    block_count: usize,
) -> FfiSourceSlotAssignmentView {
    abort_on_panic(|| {
        if engine.is_null() {
            return FfiSourceSlotAssignmentView::default();
        }
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        engine.clear_ffi_retained_cascade_assignments();
        let Some(node) = StyleNodeID::from_raw(node) else {
            return FfiSourceSlotAssignmentView::default();
        };
        if store.is_null() {
            return FfiSourceSlotAssignmentView::default();
        }
        let blocks = if block_count == 0 {
            &[]
        } else {
            unsafe {
                std::slice::from_raw_parts(
                    blocks.cast::<crate::css::cascaded_properties::FfiCascadeBlock>(),
                    block_count,
                )
            }
        };
        let assignments = engine.materialize_retained_cascade_state(
            super::computed::ComputedStyleTarget::new(node, pseudo_kind),
            unsafe { &mut *store.cast::<crate::css::cascaded_properties::CascadedPropertyStore>() },
            blocks,
        );
        engine.install_ffi_retained_cascade_assignments(assignments)
    })
}

/// Discards the borrowed retained cascade source-slot assignments.
///
/// # Safety
/// `engine` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_discard_retained_cascade_assignments(engine: *mut c_void) {
    abort_on_panic(|| {
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        engine.clear_ffi_retained_cascade_assignments();
    });
}

#[cfg(feature = "style-recording")]
pub unsafe fn replay_publish_exact_cascade_state(
    engine: *mut c_void,
    node: u32,
    pseudo_kind: u8,
    winners: &[RecordedExactCascadeWinner],
    inherited_style_groups: u8,
) -> (FfiExactCascadePublication, bool) {
    let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
    let node = StyleNodeID::from_raw(node).expect("recorded style node identities are nonzero");
    let winners = winners
        .iter()
        .map(|winner| {
            (
                winner.property,
                super::cascade::SpecifiedWinnerKey {
                    value: super::cascade::SpecifiedValueID(winner.value),
                    operator: winner.operator,
                    continuation: super::cascade::CascadeContinuationID::default(),
                    animation_relevance: winner.animation_relevance,
                },
            )
        })
        .collect::<Vec<_>>();
    let (publication, had_previous) = engine.publish_exact_cascade_winners(
        super::computed::ComputedStyleTarget::new(node, pseudo_kind),
        &winners,
        inherited_style_groups,
    );
    (publication, had_previous)
}

#[cfg(feature = "style-recording")]
pub unsafe fn replay_exact_cascade_generation_snapshot(
    engine: *const c_void,
    node: u32,
    pseudo_kind: u8,
) -> (u64, Option<u64>) {
    let engine = unsafe { &*engine.cast::<StyleEngine>() };
    let node = StyleNodeID::from_raw(node).expect("recorded style node identities are nonzero");
    engine.exact_cascade_generation_snapshot(super::computed::ComputedStyleTarget::new(node, pseudo_kind))
}

#[cfg(feature = "style-recording")]
pub fn replay_style_value(token: u64, dependency_flags: u8) -> *const c_void {
    crate::css::style_value::register_replay_style_value(token, dependency_flags).cast()
}

#[cfg(feature = "style-recording")]
pub unsafe fn replay_memory_pressure_snapshot(engine: *const c_void) -> FfiMemoryPressureSnapshot {
    let engine = unsafe { &*engine.cast::<StyleEngine>() };
    let memory = engine.memory();
    let tier3_refusal_categories = TIER3_REFUSAL_CATEGORIES.map(|category| memory.refusals(category));
    let tier4_refusal_categories: [u64; 2] = [MemoryCategory::NormalizationJournal, MemoryCategory::BatchScratch]
        .into_iter()
        .map(|category| memory.refusals(category))
        .collect::<Vec<_>>()
        .try_into()
        .expect("fixed memory category count");
    FfiMemoryPressureSnapshot {
        tier3_limit: memory.tier3_limit(),
        tier4_limit: memory.tier4_limit(),
        tier3_bytes: memory.bytes_in_tier(super::memory::Tier::Acceleration),
        tier4_bytes: memory.bytes_in_tier(super::memory::Tier::Scratch),
        tier3_refusals: tier3_refusal_categories.iter().sum(),
        tier4_refusals: tier4_refusal_categories.iter().sum(),
        tier3_refusal_categories,
        tier4_refusal_categories,
        tier3_evictions: engine
            .counters()
            .get(super::instrumentation::Counter::Tier3BenefitEvictions),
        category_bytes: MEMORY_CATEGORIES.map(|category| memory.bytes_in_category(category)),
    }
}
/// Publishes the immutable inputs of one element's base style and returns its old and new
/// `StyleRecordID` assignments. An equal pair means the final semantic output did not change.
///
/// # Safety
/// `engine` must be live. Every non-empty array must have its reported number of readable entries.
/// Group payloads and style values must remain live for this call. `animated_properties` must be
/// null or transfer one leaked C++ reference. `longhand_table` must be null or a live, frozen
/// `ComputedLonghandTable`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_publish_computed_groups(
    engine: *mut c_void,
    node: u32,
    pseudo_kind: u8,
    payloads: *const *const c_void,
    count: usize,
    inherited_group_count: usize,
    custom_property_environment: u64,
    pseudo_element_styles: u64,
    dependency_flags: u8,
    counter_style_environment_identity: u64,
    animation_overlay_identity: u64,
    animated_properties: *const c_void,
    animation_overlay_payloads: *const *const c_void,
    animation_overlay_payload_count: usize,
    property_importance: *const u8,
    property_importance_count: usize,
    property_inheritance: *const u8,
    property_inheritance_count: usize,
    inheritance_dependent_properties: *const u16,
    inheritance_dependent_values: *const *const c_void,
    inheritance_dependent_count: usize,
    raw_cascaded_font_size: *const c_void,
    longhand_table: *const c_void,
) -> FfiStyleRecordDelta {
    abort_on_panic(|| {
        if count != 0 && payloads.is_null() {
            return FfiStyleRecordDelta::default();
        }
        let payloads = match count {
            0 => &[],
            _ => unsafe { std::slice::from_raw_parts(payloads, count) },
        };
        if animation_overlay_payload_count != 0 && animation_overlay_payloads.is_null() {
            return FfiStyleRecordDelta::default();
        }
        let animation_overlay_payloads = match animation_overlay_payload_count {
            0 => &[],
            _ => unsafe { std::slice::from_raw_parts(animation_overlay_payloads, animation_overlay_payload_count) },
        };
        if (property_importance_count != 0 && property_importance.is_null())
            || (property_inheritance_count != 0 && property_inheritance.is_null())
            || (inheritance_dependent_count != 0
                && (inheritance_dependent_properties.is_null() || inheritance_dependent_values.is_null()))
        {
            return FfiStyleRecordDelta::default();
        }
        let property_importance = match property_importance_count {
            0 => &[],
            _ => unsafe { std::slice::from_raw_parts(property_importance, property_importance_count) },
        };
        let property_inheritance = match property_inheritance_count {
            0 => &[],
            _ => unsafe { std::slice::from_raw_parts(property_inheritance, property_inheritance_count) },
        };
        let inheritance_dependent_properties = match inheritance_dependent_count {
            0 => &[],
            _ => unsafe { std::slice::from_raw_parts(inheritance_dependent_properties, inheritance_dependent_count) },
        };
        let inheritance_dependent_values = match inheritance_dependent_count {
            0 => &[],
            _ => unsafe { std::slice::from_raw_parts(inheritance_dependent_values, inheritance_dependent_count) },
        };
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        if animation_overlay_identity != 0 && animated_properties.is_null() {
            return FfiStyleRecordDelta::default();
        }
        let metadata_input = super::computed::ComputedMetadataInput {
            pseudo_element_styles,
            dependency_flags,
            counter_style_environment_identity,
            animation_overlay_identity,
            animated_properties,
            animation_overlay_payloads,
            longhand_table: longhand_table.cast(),
            reconstruction: super::computed::ComputedReconstructionMetadataInput {
                property_importance,
                property_inheritance,
                inheritance_dependent_properties,
                inheritance_dependent_values,
                raw_cascaded_font_size,
            },
        };
        let publication = if let Some(node) = StyleNodeID::from_raw(node) {
            engine.publish_computed_groups(
                super::computed::ComputedStyleTarget::new(node, pseudo_kind),
                payloads,
                inherited_group_count,
                custom_property_environment,
                metadata_input,
            )
        } else {
            engine.intern_computed_groups(
                payloads,
                inherited_group_count,
                custom_property_environment,
                metadata_input,
            )
        };
        let result = FfiStyleRecordDelta {
            old_style_record: publication
                .previous_style_record_identity
                .map_or(0, super::computed::FinalStyleRecordID::raw),
            new_style_record: publication.style_record_identity.raw(),
        };
        engine.record_boundary_call(EventKind::PublishComputedGroups, |payload| {
            let pointer_token = |pointer: *const c_void| match pointer.is_null() {
                true => 0,
                false => engine
                    .recording_pointer_token(pointer as usize)
                    .expect("an enabled recorder must tokenize the pointer"),
            };
            payload.write_u32(node);
            payload.write_u8(pseudo_kind);
            let groups = engine
                .recording_computed_group_identities(result.new_style_record)
                .expect("a published style record must retain its computed groups");
            let retained_bytes = engine
                .recording_computed_group_retained_bytes(result.new_style_record)
                .expect("a published style record must retain its computed group sizes");
            assert_eq!(groups.len(), retained_bytes.len());
            payload.write_length(groups.len());
            for (identity, retained_bytes) in groups.into_iter().zip(retained_bytes) {
                payload.write_u32(identity);
                payload.write_u64(retained_bytes);
            }
            payload.write_length(inherited_group_count);
            payload.write_u64(custom_property_environment);
            payload.write_u64(pseudo_element_styles);
            payload.write_u8(dependency_flags);
            payload.write_u64(counter_style_environment_identity);
            payload.write_u64(animation_overlay_identity);
            payload.write_u64(pointer_token(animated_properties));
            payload.write_length(animation_overlay_payloads.len());
            for &pointer in animation_overlay_payloads {
                payload.write_u64(pointer_token(pointer));
            }
            payload.write_bytes(property_importance);
            payload.write_bytes(property_inheritance);
            payload.write_length(inheritance_dependent_properties.len());
            for (&property, &value) in inheritance_dependent_properties
                .iter()
                .zip(inheritance_dependent_values)
            {
                payload.write_u16(property);
                payload.write_u64(pointer_token(value));
                payload.write_u8(crate::css::style_value::style_value_dependency_flags(value.cast()));
            }
            payload.write_u64(pointer_token(raw_cascaded_font_size));
            payload.write_u8(match raw_cascaded_font_size.is_null() {
                true => 0,
                false => crate::css::style_value::style_value_dependency_flags(raw_cascaded_font_size.cast()),
            });
            let longhand_table = unsafe {
                longhand_table
                    .cast::<crate::css::computed_longhand_table::ComputedLonghandTable>()
                    .as_ref()
            };
            payload.write_bool(longhand_table.is_some());
            if longhand_table.is_some() {
                let (identity, canonical_values) = engine
                    .recording_computed_longhand_table(result.new_style_record)
                    .expect("a published style record must retain its longhand table");
                payload.write_u32(identity);
                let record_definition = engine.recording_first_response(2, u64::from(identity));
                payload.write_bool(record_definition);
                if record_definition {
                    let stored_values = canonical_values
                        .iter()
                        .enumerate()
                        .filter(|(_, value)| !value.is_null())
                        .collect::<Vec<_>>();
                    payload.write_length(stored_values.len());
                    for (index, &value) in stored_values {
                        payload.write_u16(crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID + index as u16);
                        payload.write_u64(pointer_token(value));
                        payload.write_u8(crate::css::style_value::style_value_dependency_flags(value.cast()));
                    }
                }
            }
            payload.write_u64(result.old_style_record);
            payload.write_u64(result.new_style_record);
        });
        result
    })
}

/// Assigns an already-interned base style record to one element or pseudo-element.
/// Returns an empty delta when recording is active so the caller can use the fully recorded
/// publication path instead.
///
/// # Safety
/// `engine` must be live and `style_record` must name a live base record from that engine.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_assign_shared_style_record(
    engine: *mut c_void,
    node: u32,
    pseudo_kind: u8,
    style_record: u64,
    inherited_group_count: usize,
    inherited_group_swap_eligible: bool,
) -> FfiStyleRecordDelta {
    abort_on_panic(|| {
        if engine.is_null() || node == 0 || style_record == 0 {
            return FfiStyleRecordDelta::default();
        }
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        if engine.recording_id().is_some() {
            return FfiStyleRecordDelta::default();
        }
        let target = super::computed::ComputedStyleTarget::new(
            StyleNodeID::from_raw(node).expect("a nonzero node must be a style node"),
            pseudo_kind,
        );
        let publication = engine.assign_shared_style_record(
            target,
            style_record,
            inherited_group_count,
            inherited_group_swap_eligible,
        );
        FfiStyleRecordDelta {
            old_style_record: publication
                .previous_style_record_identity
                .map_or(0, super::computed::FinalStyleRecordID::raw),
            new_style_record: publication.style_record_identity.raw(),
        }
    })
}

/// Returns the StyleEngine-owned group payload array for a base or live animation-overlay record.
///
/// # Safety
/// `engine` must be live for this call and for every read through the returned pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_style_record_payloads(engine: *const c_void, style_record: u64) -> *const c_void {
    abort_on_panic(|| {
        let engine = unsafe { &*engine.cast::<StyleEngine>() };
        let payloads = engine.style_record_payloads(style_record);
        let result = payloads.map_or(std::ptr::null(), |payloads| payloads.as_ptr().cast());
        let record_response = engine.recording_first_response(0, style_record);
        if !record_response {
            return result;
        }
        engine.record_boundary_call(EventKind::StyleRecordPayloads, |payload| {
            payload.write_u64(style_record);
            payload.write_bool(record_response);
            payload.write_bool(payloads.is_some());
            let Some(style_payloads) = payloads else {
                return;
            };
            let semantic_payloads = if style_record & (1 << 63) != 0 {
                style_payloads
                    .iter()
                    .map(|&pointer| {
                        engine
                            .recording_pointer_token(pointer as usize)
                            .expect("an enabled recorder must tokenize the pointer")
                    })
                    .collect::<Vec<_>>()
            } else {
                engine
                    .recording_computed_group_identities(style_record)
                    .expect("a base style record must retain its computed groups")
                    .into_iter()
                    .map(|identity| u64::from(identity) + 1)
                    .collect()
            };
            payload.write_length(semantic_payloads.len());
            for semantic_payload in semantic_payloads {
                payload.write_u64(semantic_payload);
            }
        });
        result
    })
}

/// Returns a synchronous borrowed view of a base or live animation-overlay record.
///
/// # Safety
/// `engine` must be live for this call and for every read through the returned pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_style_record_view(
    engine: *const c_void,
    style_record: u64,
) -> FfiStyleRecordView {
    abort_on_panic(|| {
        let engine = unsafe { &*engine.cast::<StyleEngine>() };
        let view = engine.style_record_view(style_record);
        let result = match &view {
            None => FfiStyleRecordView::missing(),
            Some(view) => FfiStyleRecordView {
                payloads: view.payloads.as_ptr(),
                base_payloads: view.base_payloads.as_ptr(),
                property_importance: view.property_importance.as_ptr(),
                property_inheritance: view.property_inheritance.as_ptr(),
                inheritance_dependent_values: view.inheritance_dependent_values.as_ptr(),
                longhand_values: view.longhand_values.as_ptr(),
                raw_cascaded_font_size: view.raw_cascaded_font_size,
                animated_properties: view.animated_properties,
                payload_count: view.payloads.len(),
                property_importance_count: view.property_importance.len(),
                property_inheritance_count: view.property_inheritance.len(),
                inheritance_dependent_value_count: view.inheritance_dependent_values.len(),
                longhand_value_count: view.longhand_values.len(),
                pseudo_element_styles: view.pseudo_element_styles,
                counter_style_environment_identity: view.counter_style_environment_identity,
                animation_overlay_identity: view.animation_overlay_identity,
                dependency_flags: view.dependency_flags,
                present: true,
            },
        };
        let record_response = engine.recording_first_response(1, style_record);
        if !record_response {
            return result;
        }
        engine.record_boundary_call(EventKind::StyleRecordView, |payload| {
            payload.write_u64(style_record);
            payload.write_bool(record_response);
            payload.write_bool(result.present);
            let Some(view) = view else {
                return;
            };
            let base_payloads = engine
                .recording_computed_group_identities(style_record)
                .expect("a style record view must retain its base computed groups")
                .into_iter()
                .map(|identity| u64::from(identity) + 1)
                .collect::<Vec<_>>();
            let style_payloads = match view.animation_overlay_identity {
                0 => base_payloads.clone(),
                _ => view
                    .payloads
                    .iter()
                    .map(|&pointer| {
                        engine
                            .recording_pointer_token(pointer as usize)
                            .expect("an enabled recorder must tokenize the pointer")
                    })
                    .collect(),
            };
            payload.write_length(style_payloads.len());
            for pointer in style_payloads {
                payload.write_u64(pointer);
            }
            payload.write_length(base_payloads.len());
            for pointer in base_payloads {
                payload.write_u64(pointer);
            }
            payload.write_bytes(view.property_importance);
            payload.write_bytes(view.property_inheritance);
            payload.write_length(view.inheritance_dependent_values.len());
            for entry in view.inheritance_dependent_values {
                payload.write_u16(entry.property);
                payload.write_u64(
                    engine
                        .recording_pointer_token(entry.value as usize)
                        .expect("an enabled recorder must tokenize the pointer"),
                );
            }
            payload.write_u64(match view.raw_cascaded_font_size.is_null() {
                true => 0,
                false => engine
                    .recording_pointer_token(view.raw_cascaded_font_size as usize)
                    .expect("an enabled recorder must tokenize the pointer"),
            });
            payload.write_u64(match view.animated_properties.is_null() {
                true => 0,
                false => engine
                    .recording_pointer_token(view.animated_properties as usize)
                    .expect("an enabled recorder must tokenize the pointer"),
            });
            payload.write_u64(view.pseudo_element_styles);
            payload.write_u64(view.counter_style_environment_identity);
            payload.write_u64(view.animation_overlay_identity);
            payload.write_u8(view.dependency_flags);
            payload.write_length(view.longhand_values.len());
            for &value in view.longhand_values {
                payload.write_u64(match value.is_null() {
                    true => 0,
                    false => engine
                        .recording_pointer_token(value as usize)
                        .expect("an enabled recorder must tokenize the pointer"),
                });
            }
        });
        result
    })
}
/// Removes the retained computed-input assignment for one pseudo-element kind.
///
/// # Safety
/// `engine` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_remove_computed_pseudo(
    engine: *mut c_void,
    node: u32,
    pseudo_kind: u8,
) -> FfiStyleRecordDelta {
    abort_on_panic(|| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return FfiStyleRecordDelta::default();
        };
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        let result = FfiStyleRecordDelta {
            old_style_record: engine
                .remove_computed_pseudo(node, pseudo_kind)
                .map_or(0, super::computed::FinalStyleRecordID::raw),
            new_style_record: 0,
        };
        engine.record_boundary_call(EventKind::RemoveComputedPseudo, |payload| {
            payload.write_u32(node.raw());
            payload.write_u8(pseudo_kind);
            payload.write_u64(result.old_style_record);
            payload.write_u64(result.new_style_record);
        });
        result
    })
}
/// Records which longhand properties a rule declares. All four arrays are parallel.
///
/// # Safety
/// `engine` must be live, all arrays must have `count` readable entries, and every canonical and
/// original value must point at live Rust-owned style value data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_rule_declared_properties(
    engine: *mut c_void,
    rule: u32,
    properties: *const u16,
    important: *const bool,
    operators: *const FfiCascadeOperator,
    values: *const *const c_void,
    original_values: *const *const c_void,
    count: usize,
    declarations_are_complete: bool,
) {
    abort_on_panic(|| {
        if rule == 0 {
            return;
        }
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        let declared: Vec<DeclaredProperty> = match count == 0 {
            true => Vec::new(),
            false => {
                if properties.is_null()
                    || important.is_null()
                    || operators.is_null()
                    || values.is_null()
                    || original_values.is_null()
                {
                    return;
                }
                let properties = unsafe { std::slice::from_raw_parts(properties, count) };
                let important = unsafe { std::slice::from_raw_parts(important, count) };
                let operators = unsafe { std::slice::from_raw_parts(operators, count) };
                let values = unsafe { std::slice::from_raw_parts(values, count) };
                let original_values = unsafe { std::slice::from_raw_parts(original_values, count) };
                properties
                    .iter()
                    .copied()
                    .zip(important.iter().copied())
                    .zip(operators.iter().copied())
                    .zip(values.iter().copied().zip(original_values.iter().copied()))
                    .map(|(((property, important), operator), (value, original_value))| {
                        let value = unsafe { engine.intern_specified_value(value.cast()) };
                        unsafe { engine.alias_specified_value(original_value.cast(), value) };
                        DeclaredProperty {
                            property,
                            important,
                            operator: operator.decode(),
                            value,
                        }
                    })
                    .collect()
            }
        };
        engine.set_rule_declared_properties_with_operators(RuleID(rule - 1), &declared, declarations_are_complete);
        engine.record_boundary_call(EventKind::SetRuleDeclaredProperties, |payload| {
            payload.write_u32(rule);
            payload.write_bool(declarations_are_complete);
            write_declared_properties(&declared, payload);
        });
    });
}
/// Interns one name identity and returns its document-local atom.
///
/// The caller passes the one-word identity of an interned string it holds a reference to, so the
/// identity cannot be reused while the atom is live.
///
/// # Safety
/// `engine` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_intern_atom(engine: *mut c_void, raw: usize) -> u32 {
    abort_on_panic(|| {
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        let result = engine.intern_atom(raw).0;
        let token = engine.recording_atom_pointer_token(raw);
        engine.record_boundary_call(EventKind::InternAtom, |payload| {
            payload.write_u64(token.expect("an enabled recorder must tokenize the pointer"));
            payload.write_u32(result);
        });
        result
    })
}

/// Takes the pending style transaction and returns its versioned semantic match answers.
///
/// # Safety
/// `engine` must be live. The returned answer slice remains valid until the next mutable
/// `style_engine_*` entry point or an explicit discard of the transaction outputs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_take_style_transaction(
    engine: *mut c_void,
    root: u32,
) -> FfiStyleTransactionView {
    abort_on_panic(|| {
        let Some(root) = StyleNodeID::from_raw(root) else {
            return FfiStyleTransactionView::default();
        };
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        engine.clear_ffi_style_transaction_output();
        let mut output = FfiStyleTransactionOutput::default();
        output.scoped = engine.take_style_transaction(root, |transaction_version, program_version, answers| {
            assert!(
                output.answers.is_empty(),
                "a style transaction emitted more than one batch"
            );
            output.transaction_version = transaction_version.0;
            output.program_version = program_version.0;
            output.answers.extend_from_slice(answers);
        });
        output.reclaimed_style_atoms = std::mem::take(&mut engine.reclaimed_style_atoms)
            .into_iter()
            .map(|reclaimed| FfiReclaimedStyleAtom {
                raw: reclaimed.raw,
                atom: reclaimed.atom.0,
            })
            .collect();
        output.style_atoms_swept = std::mem::take(&mut engine.style_atoms_swept);
        if engine.recording_id().is_some() {
            engine.record_boundary_call(EventKind::StyleDeltaBatch, |payload| {
                payload.write_u32(root.raw());
                let mut outputs = super::record_replay::PayloadWriter::default();
                write_style_transaction_outputs(&output, &mut outputs);
                payload.write_bytes(outputs.as_bytes());
                payload.write_u64(outputs.stable_digest());
            });
            engine.forget_recording_atom_mappings(output.reclaimed_style_atoms.iter().map(|reclaimed| reclaimed.atom));
        }
        engine.install_ffi_style_transaction_output(output);
        let output = &engine.ffi_style_transaction_output;
        FfiStyleTransactionView {
            transaction_version: output.transaction_version,
            program_version: output.program_version,
            answers: output.answers.as_ptr(),
            count: output.answers.len(),
            reclaimed_style_atoms: output.reclaimed_style_atoms.as_ptr(),
            reclaimed_style_atom_count: output.reclaimed_style_atoms.len(),
            scoped: output.scoped,
            style_atoms_swept: output.style_atoms_swept,
        }
    })
}

/// Installs the authoritative release order recorded for the next replay transaction.
///
/// # Safety
/// `engine` must be live and `atoms` must name `count` readable atom identities.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_replay_reclaimed_style_atoms(
    engine: *mut c_void,
    atoms: *const u32,
    count: usize,
) {
    abort_on_panic(|| {
        let engine = unsafe { &mut *engine.cast::<StyleEngine>() };
        assert!(engine.replay_reclaimed_style_atoms.is_none());
        let atoms = if count == 0 {
            &[]
        } else {
            assert!(!atoms.is_null());
            unsafe { std::slice::from_raw_parts(atoms, count) }
        };
        engine.replay_reclaimed_style_atoms = Some(atoms.iter().copied().map(StyleAtomID).collect());
    });
}
/// Reads one counter by index, returning its stable name and writing its value and name length, or
/// null once the index is past the end. C++ enumerates the counters this way rather than
/// duplicating the list. The name is borrowed static UTF-8 and is not nul-terminated.
///
/// # Safety
/// `engine` must be live, and the out pointers must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_counter(
    engine: *const c_void,
    index: usize,
    out_value: *mut u64,
    out_name_length: *mut usize,
) -> *const u8 {
    abort_on_panic(|| {
        let engine = unsafe { &*engine.cast::<StyleEngine>() };
        let result = engine.counters().iter().nth(index);
        engine.record_boundary_call(EventKind::Counter, |payload| {
            payload.write_u64(u64::try_from(index).expect("counter index exceeds u64"));
            payload.write_bool(result.is_some());
            if let Some((name, value)) = result {
                payload.write_bytes(name.as_bytes());
                payload.write_u64(value);
            }
        });
        let Some((name, value)) = result else {
            return std::ptr::null();
        };
        unsafe {
            *out_value = value;
            *out_name_length = name.len();
        }
        name.as_ptr()
    })
}

/// Records a benchmark phase marker when capture is enabled.
///
/// # Safety
/// `engine` must be live, and `name` must point at `length` readable UTF-16 code units.
#[cfg(feature = "style-recording")]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_record_benchmark_marker(
    engine: *const c_void,
    name: *const c_void,
    length: usize,
    is_ascii: bool,
) {
    abort_on_panic(|| {
        let engine = unsafe { &*engine.cast::<StyleEngine>() };
        if engine.recording_id().is_none() {
            return;
        }
        let name = match is_ascii {
            true => unsafe { borrow(name.cast::<u8>(), length) }
                .iter()
                .map(|&code_unit| u16::from(code_unit))
                .collect::<Vec<_>>(),
            false => unsafe { borrow(name.cast::<u16>(), length) }.to_vec(),
        };
        engine.record_boundary_call(EventKind::BenchmarkMarker, |payload| payload.write_u16_slice(&name));
    });
}

unsafe fn borrow<'a, T>(pointer: *const T, count: usize) -> &'a [T] {
    if count == 0 {
        return &[];
    }
    assert!(!pointer.is_null(), "a non-empty delta array must not be null");
    unsafe { std::slice::from_raw_parts(pointer, count) }
}

include!(concat!(env!("OUT_DIR"), "/style_engine_boundary_generated.rs"));

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::style::Lookup;
    use crate::css::style::instrumentation::Counter;
    use crate::css::style::memory::MemoryCategory;
    use crate::css::style::memory::Tier;
    use crate::css::style::transaction::InputKind;

    fn no_relations() -> FfiTreeRelations {
        FfiTreeRelations {
            parent: 0,
            previous_element_sibling: 0,
            next_element_sibling: 0,
            tree_scope: 0,
            assigned_slot: 0,
            reserved: 0,
        }
    }

    #[cfg(feature = "style-recording")]
    #[test]
    fn recording_transaction_rows_zero_struct_padding() {
        let relations = no_relations();
        let mut tree = std::mem::MaybeUninit::<FfiTreeDelta>::uninit();
        let tree_pointer = tree.as_mut_ptr();
        // SAFETY: every byte starts initialized and every typed field is then written with a valid
        // value before the row is assumed initialized.
        let tree = unsafe {
            tree_pointer.cast::<u8>().write_bytes(0xaa, size_of::<FfiTreeDelta>());
            std::ptr::addr_of_mut!((*tree_pointer).node).write(1);
            std::ptr::addr_of_mut!((*tree_pointer).old_connected).write(false);
            std::ptr::addr_of_mut!((*tree_pointer).new_connected).write(true);
            std::ptr::addr_of_mut!((*tree_pointer).old_relations).write(relations);
            std::ptr::addr_of_mut!((*tree_pointer).new_relations).write(relations);
            tree.assume_init()
        };
        let mut payload = crate::css::style::record_replay::PayloadWriter::default();
        write_recording_tree_deltas(&[tree], &mut payload);
        assert_eq!(&payload.as_bytes()[18..20], &[0, 0]);

        let mut state = std::mem::MaybeUninit::<FfiStateDelta>::uninit();
        let state_pointer = state.as_mut_ptr();
        // SAFETY: every byte starts initialized and every typed field is then written with a valid
        // value before the row is assumed initialized.
        let state = unsafe {
            state_pointer.cast::<u8>().write_bytes(0xaa, size_of::<FfiStateDelta>());
            std::ptr::addr_of_mut!((*state_pointer).node).write(1);
            std::ptr::addr_of_mut!((*state_pointer).fact).write(FfiStateFact::Hover);
            std::ptr::addr_of_mut!((*state_pointer).new_value).write(true);
            state.assume_init()
        };
        let mut first_payload = crate::css::style::record_replay::PayloadWriter::default();
        write_recording_state_deltas(&[state], &mut first_payload);
        assert_eq!(&first_payload.as_bytes()[18..20], &[0, 0]);

        let mut second_state = std::mem::MaybeUninit::<FfiStateDelta>::uninit();
        let second_state_pointer = second_state.as_mut_ptr();
        // SAFETY: every byte starts initialized and every typed field is then written with a valid
        // value before the row is assumed initialized.
        let second_state = unsafe {
            second_state_pointer
                .cast::<u8>()
                .write_bytes(0xbb, size_of::<FfiStateDelta>());
            std::ptr::addr_of_mut!((*second_state_pointer).node).write(1);
            std::ptr::addr_of_mut!((*second_state_pointer).fact).write(FfiStateFact::Hover);
            std::ptr::addr_of_mut!((*second_state_pointer).new_value).write(true);
            second_state.assume_init()
        };
        let mut second_payload = crate::css::style::record_replay::PayloadWriter::default();
        write_recording_state_deltas(&[second_state], &mut second_payload);
        assert_eq!(first_payload.as_bytes(), second_payload.as_bytes());

        let mut style_input = std::mem::MaybeUninit::<FfiElementStyleInput>::uninit();
        let style_input_pointer = style_input.as_mut_ptr();
        // SAFETY: every byte starts initialized and every typed field is then written with a valid
        // value before the row is assumed initialized.
        let style_input = unsafe {
            style_input_pointer
                .cast::<u8>()
                .write_bytes(0xaa, size_of::<FfiElementStyleInput>());
            std::ptr::addr_of_mut!((*style_input_pointer).style_node).write(1);
            std::ptr::addr_of_mut!((*style_input_pointer).reaction).write(2);
            std::ptr::addr_of_mut!((*style_input_pointer).inherited_style_groups).write(3);
            style_input.assume_init()
        };
        let mut payload = crate::css::style::record_replay::PayloadWriter::default();
        write_recording_element_style_inputs(&[style_input], &mut payload);
        assert_eq!(&payload.as_bytes()[18..20], &[0, 0]);
    }

    #[test]
    fn initial_tree_batch_uses_the_document_root_as_its_transaction_envelope() {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let mut nodes = [0_u32; 4];
        engine.allocate_style_nodes(&mut nodes);

        let initial_tree = [
            FfiTreeDelta {
                node: nodes[0],
                old_connected: false,
                new_connected: true,
                old_relations: no_relations(),
                new_relations: no_relations(),
            },
            FfiTreeDelta {
                node: nodes[1],
                old_connected: false,
                new_connected: true,
                old_relations: no_relations(),
                new_relations: FfiTreeRelations {
                    parent: nodes[0],
                    ..no_relations()
                },
            },
            FfiTreeDelta {
                node: nodes[2],
                old_connected: false,
                new_connected: true,
                old_relations: no_relations(),
                new_relations: FfiTreeRelations {
                    parent: nodes[1],
                    ..no_relations()
                },
            },
        ];
        engine.apply_transaction_batch(&initial_tree, (&[], &[]), &[], &[], &[], &[]);

        let root = StyleNodeID::from_raw(nodes[0]).unwrap();
        let child = StyleNodeID::from_raw(nodes[1]).unwrap();
        let grandchild = StyleNodeID::from_raw(nodes[2]).unwrap();
        assert_eq!(
            engine.tree().preorder(root).collect::<Vec<_>>(),
            vec![root, child, grandchild]
        );

        let transaction = engine.take_transaction();
        assert_eq!(transaction.inputs.len(), 2);
        assert!(transaction.inputs.iter().all(|input| matches!(
            input.key,
            InputKey::TreeRelations(node) | InputKey::LocalFeature(node, LocalFeatureKey::ArrivingFacts) if node == root
        )));
        engine.release_transaction(transaction);

        assert_eq!(engine.counters().get(Counter::InitialBulkLoads), 1);
        assert_eq!(engine.counters().get(Counter::InitialBulkTreeRows), 3);
        assert_eq!(engine.counters().get(Counter::RawMutationRecords), 3);
        assert_eq!(engine.counters().get(Counter::TreeDeltas), 3);

        let later_arrival = [FfiTreeDelta {
            node: nodes[3],
            old_connected: false,
            new_connected: true,
            old_relations: no_relations(),
            new_relations: FfiTreeRelations {
                parent: nodes[0],
                previous_element_sibling: nodes[1],
                ..no_relations()
            },
        }];
        engine.apply_transaction_batch(&later_arrival, (&[], &[]), &[], &[], &[], &[]);
        let transaction = engine.take_transaction();
        assert_eq!(transaction.inputs.len(), 3);
        let later = StyleNodeID::from_raw(nodes[3]).unwrap();
        assert!(transaction.inputs.iter().any(|input| matches!(
            input.key,
            InputKey::TreeRelations(node) if node == child
        )));
        assert_eq!(
            transaction
                .inputs
                .iter()
                .filter(|input| matches!(
                    input.key,
                    InputKey::TreeRelations(node) | InputKey::LocalFeature(node, LocalFeatureKey::ArrivingFacts) if node == later
                ))
                .count(),
            2
        );
        engine.release_transaction(transaction);
        assert_eq!(engine.counters().get(Counter::InitialBulkLoads), 1);
        assert_eq!(engine.counters().get(Counter::InitialBulkTreeRows), 3);
    }

    #[test]
    fn element_arrival_rows_install_intrinsic_facts() {
        assert_eq!(size_of::<FfiElementArrival>(), 28);
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let mut nodes = [0_u32; 2];
        engine.allocate_style_nodes(&mut nodes);
        let tree = [
            FfiTreeDelta {
                node: nodes[0],
                old_connected: false,
                new_connected: true,
                old_relations: no_relations(),
                new_relations: no_relations(),
            },
            FfiTreeDelta {
                node: nodes[1],
                old_connected: false,
                new_connected: true,
                old_relations: no_relations(),
                new_relations: FfiTreeRelations {
                    parent: nodes[0],
                    ..no_relations()
                },
            },
        ];
        let arrivals = [
            FfiElementArrival {
                node: nodes[0],
                namespace_atom: 11,
                language_atom: 12,
                directionality_atom: 13,
                custom_state_offset: 0,
                custom_state_count: 2,
                heading_level: 4,
                is_slot: true,
                reserved: 0,
            },
            FfiElementArrival {
                node: nodes[1],
                namespace_atom: 21,
                language_atom: 22,
                directionality_atom: 23,
                custom_state_offset: 2,
                custom_state_count: 1,
                heading_level: 0,
                is_slot: false,
                reserved: 0,
            },
        ];
        engine.apply_transaction_batch(&tree, (&arrivals, &[31, 32, 33]), &[], &[], &[], &[]);
        let transaction = engine.take_transaction();

        let root = StyleNodeID::from_raw(nodes[0]).unwrap();
        let child = StyleNodeID::from_raw(nodes[1]).unwrap();
        assert_eq!(engine.facts.namespace_of(root), StyleAtomID(11));
        assert_eq!(engine.facts.language_of(root), StyleAtomID(12));
        assert_eq!(engine.facts.directionality_of(root), StyleAtomID(13));
        assert_eq!(engine.facts.heading_level_of(root), 4);
        assert!(engine.facts.is_slot(root));
        assert_eq!(engine.facts.custom_states_of(root), &[StyleAtomID(31), StyleAtomID(32)]);
        assert_eq!(engine.facts.namespace_of(child), StyleAtomID(21));
        assert_eq!(engine.facts.custom_states_of(child), &[StyleAtomID(33)]);
        engine.release_transaction(transaction);
    }

    fn arrival_for(node: u32, custom_state_offset: u32, custom_state_count: u32) -> FfiElementArrival {
        FfiElementArrival {
            node,
            namespace_atom: 1,
            language_atom: 2,
            directionality_atom: 3,
            custom_state_offset,
            custom_state_count,
            heading_level: 0,
            is_slot: false,
            reserved: 0,
        }
    }

    #[test]
    #[should_panic(expected = "an element arrival named an invalid style node")]
    fn malformed_element_arrival_rejects_an_invalid_node() {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        engine.apply_transaction_batch(&[], (&[arrival_for(0, 0, 0)], &[]), &[], &[], &[], &[]);
    }

    #[test]
    #[should_panic(expected = "an element arrival custom-state range overflowed")]
    fn malformed_element_arrival_rejects_an_overflowing_custom_state_range() {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let mut nodes = [0];
        engine.allocate_style_nodes(&mut nodes);
        engine.apply_transaction_batch(&[], (&[arrival_for(nodes[0], u32::MAX, 1)], &[]), &[], &[], &[], &[]);
    }

    #[test]
    #[should_panic(expected = "an element arrival named custom states outside the shared atom column")]
    fn malformed_element_arrival_rejects_an_out_of_bounds_custom_state_range() {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let mut nodes = [0];
        engine.allocate_style_nodes(&mut nodes);
        engine.apply_transaction_batch(&[], (&[arrival_for(nodes[0], 0, 2)], &[1]), &[], &[], &[], &[]);
    }

    #[test]
    fn initial_tree_bulk_load_publishes_match_answers_before_traversal() {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let mut nodes = [0_u32; 3];
        engine.allocate_style_nodes(&mut nodes);
        let initial_tree = [
            FfiTreeDelta {
                node: nodes[0],
                old_connected: false,
                new_connected: true,
                old_relations: no_relations(),
                new_relations: no_relations(),
            },
            FfiTreeDelta {
                node: nodes[1],
                old_connected: false,
                new_connected: true,
                old_relations: no_relations(),
                new_relations: FfiTreeRelations {
                    parent: nodes[0],
                    ..no_relations()
                },
            },
            FfiTreeDelta {
                node: nodes[2],
                old_connected: false,
                new_connected: true,
                old_relations: no_relations(),
                new_relations: FfiTreeRelations {
                    parent: nodes[1],
                    ..no_relations()
                },
            },
        ];
        let initial_features = nodes.map(|node| FfiLocalFeatureDelta {
            node,
            feature_kind: FfiFeatureKind::TagName,
            name_atom: 0,
            old_kind: FfiFeatureValueKind::Absent,
            old_atom: 0,
            new_kind: FfiFeatureValueKind::Atom,
            new_atom: 1,
        });
        engine.apply_transaction_batch(&initial_tree, (&[], &[]), &initial_features, &[], &[], &[]);

        let root = StyleNodeID::from_raw(nodes[0]).unwrap();
        let mut published = Vec::new();
        let mut emission_count = 0;
        assert!(!engine.take_style_transaction(root, |_, _, answers| {
            emission_count += 1;
            published.extend_from_slice(answers);
        }));
        assert_eq!(emission_count, 1);
        assert_eq!(
            published.iter().map(|delta| delta.style_node).collect::<Vec<_>>(),
            nodes
        );
        assert!(published.iter().all(|delta| {
            delta.pseudo_kind == u8::MAX
                && delta.old_style_record == 0
                && delta.new_style_record == 0
                && delta.damage == FfiStyleDeltaDamage::None
                && delta.gap == FfiStyleDeltaGap::Materialize
        }));
        assert_eq!(engine.counters().get(Counter::InitialBulkMatchLoads), 1);
        assert_eq!(engine.counters().get(Counter::InitialBulkMatchRows), 3);
        assert_eq!(engine.counters().get(Counter::PublishedMatchAnswerRecords), 3);
        assert_eq!(engine.counters().get(Counter::PreparedMatchingBatchRowsCloned), 3);

        let upqueries = engine.counters().get(Counter::MatchAnswerUpqueries);
        assert!(engine.begin_cold_matching_batch(root));
        for raw in nodes[..2].iter().copied() {
            let node = StyleNodeID::from_raw(raw).unwrap();
            assert_eq!(engine.consume_published_match_answer(node), Some(Vec::new()));
        }
        engine.end_cold_matching_batch();
        assert!(matches!(
            engine.retained_match_answer(StyleNodeID::from_raw(nodes[0]).unwrap()),
            Lookup::Known(_)
        ));
        assert!(matches!(
            engine.retained_match_answer(StyleNodeID::from_raw(nodes[2]).unwrap()),
            Lookup::Missing(_)
        ));
        assert_eq!(engine.counters().get(Counter::MatchAnswerUpqueries), upqueries);
        assert_eq!(
            engine
                .counters()
                .get(Counter::MatchElementCallsDuringPublishedStyleTransaction),
            0
        );
    }

    #[test]
    fn one_batch_carries_every_typed_delta_kind() {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let mut nodes = [0_u32; 3];
        engine.allocate_style_nodes(&mut nodes);

        // Connect the two elements the batch below reports facts about. An arriving element folds
        // everything it publishes onto its arrival entry, so a state change is only its own input
        // once the element is already there.
        let arrival = [
            FfiTreeDelta {
                node: nodes[0],
                old_connected: false,
                new_connected: true,
                old_relations: no_relations(),
                new_relations: no_relations(),
            },
            FfiTreeDelta {
                node: nodes[1],
                old_connected: false,
                new_connected: true,
                old_relations: no_relations(),
                new_relations: FfiTreeRelations {
                    parent: nodes[0],
                    ..no_relations()
                },
            },
        ];
        engine.apply_transaction_batch(&arrival, (&[], &[]), &[], &[], &[], &[]);
        let settled = engine.take_transaction();
        engine.release_transaction(settled);

        let tree = [FfiTreeDelta {
            node: nodes[2],
            old_connected: false,
            new_connected: true,
            old_relations: no_relations(),
            new_relations: FfiTreeRelations {
                parent: nodes[0],
                previous_element_sibling: nodes[1],
                ..no_relations()
            },
        }];
        let features = [FfiLocalFeatureDelta {
            node: nodes[1],
            feature_kind: FfiFeatureKind::Class,
            name_atom: 42,
            old_kind: FfiFeatureValueKind::Absent,
            old_atom: 0,
            new_kind: FfiFeatureValueKind::Present,
            new_atom: 0,
        }];
        let states = [FfiStateDelta {
            node: nodes[1],
            fact: FfiStateFact::Hover,
            new_value: true,
        }];
        let declarations = [FfiElementDeclarationDelta {
            node: nodes[1],
            kind: FfiElementDeclarationKind::InlineStyle,
            old_block: 0,
            new_block: 7,
        }];

        let style_inputs = [FfiElementStyleInput {
            style_node: nodes[1],
            reaction: crate::css::style::transaction::STYLE_REACTION_RECOMPUTE_STYLE,
            inherited_style_groups: 0,
        }];
        engine.apply_transaction_batch(&tree, (&[], &[]), &features, &states, &declarations, &style_inputs);

        let transaction = engine.take_transaction();
        let node0 = StyleNodeID::from_raw(nodes[0]).unwrap();
        let node1 = StyleNodeID::from_raw(nodes[1]).unwrap();
        let node2 = StyleNodeID::from_raw(nodes[2]).unwrap();
        assert_eq!(engine.tree().children(node0).collect::<Vec<_>>(), vec![node1, node2]);
        let kinds: Vec<InputKind> = transaction.inputs.iter().map(|input| input.key.kind()).collect();
        assert!(kinds.contains(&InputKind::TreeRelations));
        assert!(kinds.contains(&InputKind::LocalFeature));
        assert!(kinds.contains(&InputKind::State));
        assert!(kinds.contains(&InputKind::ElementDeclaration));
        assert!(kinds.contains(&InputKind::ElementStyleInput));
        engine.release_transaction(transaction);

        assert_eq!(engine.counters().get(Counter::TreeDeltas), 4);
        assert_eq!(engine.counters().get(Counter::LocalFeatureDeltas), 1);
        assert_eq!(engine.counters().get(Counter::StateDeltas), 1);
        assert_eq!(engine.counters().get(Counter::ElementDeclarationDeltas), 1);
    }

    #[test]
    fn a_batch_of_no_deltas_costs_nothing() {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        engine.apply_transaction_batch(&[], (&[], &[]), &[], &[], &[], &[]);
        let transaction = engine.take_transaction();
        assert!(transaction.is_empty());
        engine.release_transaction(transaction);
        assert_eq!(engine.memory().bytes_in_tier(Tier::Acceleration), 0);
    }

    #[test]
    fn identities_are_minted_in_one_call_per_batch() {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let mut nodes = [0_u32; 512];
        engine.allocate_style_nodes(&mut nodes);
        assert_eq!(engine.counters().get(Counter::StyleNodesAllocated), 512);
        assert!(nodes.windows(2).all(|pair| pair[0] < pair[1]));
        assert!(engine.memory().bytes_in_category(MemoryCategory::RelationColumns) > 0);
    }
}
