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

use super::engine_calls::{document_host, sheet_writing_host, with_engine};
use super::{ReplayAtomSweep, StyleEngineHandle};
use crate::render_state::DocumentHost;
use std::ffi::c_void;

use crate::abort_on_panic as abort_on_boundary_panic;
use crate::css::custom_properties::{CustomPropertyRegistry, ffi_slice};
use crate::css::host_shared::{HostShared, SharedPayload};
use crate::css::selector::CompiledSelector;
use crate::css::style_value::RetainedStyleValueData;

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
#[cfg(feature = "style-recording")]
use super::memory::MemoryCategory;
#[cfg(feature = "style-recording")]
use super::memory::TIER3_REFUSAL_CATEGORIES;
use super::program::CascadeLayerID;
use super::program::CascadeOrigin;
use super::program::CustomDeclaration;
use super::program::DeclarationBlockID;
use super::program::DeclaredProperty;
use super::program::RuleID;
use super::program::RuleKind;
use super::program::SheetID;
use super::program::StyleSheetObjectID;
use super::record_replay::EventKind;
use super::transaction::ElementDeclarationKind;
use super::transaction::InputKey;
use super::transaction::InputValue;
use super::transaction::StateFact;
use super::transaction::TreeRelations;
use super::tree::StyleNodeID;
use super::tree::TreeScopeID;
use super::{Counters, StyleEngine, StyleEngineState};

fn abort_on_panic<F: FnOnce() -> R, R>(operation: F) -> R {
    abort_on_boundary_panic(operation)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum FfiStyleInvalidationField {
    LevelMask = 0x3,
    VisualContextShift = 2,
    RebuildRootShift = 4,
    RebuildRootMask = 0x7,
    RebuildStackingContext = 1 << 7,
    ResnapScrollContainer = 1 << 8,
    RecomputeDescendants = 1 << 9,
    InheritedGroupsShift = 10,
    InheritedGroupsMask = 0x7f,
    RepaintTextDecorations = 1 << 18,
    NonInheritedInheritanceSource = 1 << 19,
    AnyComputedValueChanged = 1 << 20,
    CacheHit = 1 << 21,
    AffectsHitTesting = 1 << 22,
    /// The word holds the damage the engine computed with its answer.
    EngineComputed = 1 << 23,
    /// Selection highlights, which text descendants paint, repaint.
    RepaintSelection = 1 << 24,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiAnimationInvalidation {
    pub invalidation: u32,
    pub changed_non_inherited_style_groups: u32,
    pub requires_base_style_recomputation: bool,
    pub requires_layout_node_style_application: bool,
    pub requires_style_resource_update: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiStyleDeltaGap {
    None,
    Materialize,
    /// The engine computed the new record itself from the moved cascade winners; C++ applies it
    /// without running a style computation.
    Computed,
    /// Retry a cold drive while applying the preorder batch, after its parent is authoritative.
    RetryAfterAncestor,
    /// The element needs no style: the host holds none for it in a display:none subtree, and
    /// neither it nor any element inheriting from it reads style while hidden. A read or the
    /// subtree's reveal asks for its record.
    Hidden,
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
    /// Substitution usage for an engine-computed element, including its pseudo-elements.
    pub uses_substitution: bool,
    /// What an engine-computed element's records read beyond their cascade, as
    /// `FfiNodeRecordReads` bits, for the host to note as its own computation notes them.
    pub record_reads: u8,
    /// The non-inherited style groups an engine-computed element read straight from its parent
    /// through an explicit `inherit`, which the host marks the parent with; all of them when
    /// `u32::MAX`.
    pub explicitly_inherited_groups: u32,
    /// What moving an element the engine settled from the old record to the new one damages, packed
    /// as an `FfiStyleInvalidationField` word. Only a word with `EngineComputed` set holds an answer;
    /// the host asks for the damage of any other move.
    pub record_damage: u32,
    /// Whether the host owes an element the engine settled the animation plan its new record
    /// decides, once it installs the record: the record moves the `animation-*` longhands
    /// declaring the element's CSS animations, or the element runs some.
    pub owes_an_animation_plan: bool,
    /// Whether the host owes an element the engine settled the transition step, against the old
    /// record, once it installs the new one.
    pub owes_a_transition_step: bool,
    /// Whether the host composes the new record of an element the engine settled before anything
    /// inherits from it: it applies the record's animation plan and samples the element's
    /// animations over it, and only then asks the engine for the element's pseudo-elements, which
    /// the row leaves out.
    pub composed_by_the_host: bool,
}

/// A retried engine record and the metadata needed to install it.
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiEngineComputedRecord {
    pub style_record: u64,
    pub uses_substitution: bool,
    /// As [`FfiStyleDelta::record_reads`].
    pub record_reads: u8,
    /// As [`FfiStyleDelta::explicitly_inherited_groups`].
    pub explicitly_inherited_groups: u32,
    /// As [`FfiStyleDelta::owes_an_animation_plan`].
    pub owes_an_animation_plan: bool,
    /// As [`FfiStyleDelta::owes_a_transition_step`].
    pub owes_a_transition_step: bool,
    /// As [`FfiStyleDelta::composed_by_the_host`].
    pub composed_by_the_host: bool,
    /// The synthetic pseudo-element kinds whose records the engine settled beside the
    /// element's, as a bit per kind; a present slot holding zero is a removal.
    pub pseudo_records_present: u8,
    pub pseudo_records: [u64; RETRY_PSEUDO_RECORD_SLOTS],
}

/// The synthetic pseudo-element records the engine settled beside an element's installed record.
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiSettledPseudoRecords {
    /// The engine settled none: one of the pseudo-elements reads a value it cannot compute, or one
    /// of their rules a container condition it cannot decide. They keep the records they hold, and
    /// nothing else here is set.
    pub refused: bool,
    /// Whether a settled pseudo-element substituted custom properties.
    pub uses_substitution: bool,
    /// As [`FfiStyleDelta::record_reads`].
    pub record_reads: u8,
    /// As [`FfiStyleDelta::explicitly_inherited_groups`].
    pub explicitly_inherited_groups: u32,
    /// The kinds whose records the engine settled, as a bit per kind; a present slot holding zero
    /// is a removal, and an absent kind keeps its record.
    pub pseudo_records_present: u8,
    pub pseudo_records: [u64; RETRY_PSEUDO_RECORD_SLOTS],
}

/// One record slot per synthetic pseudo-element kind in a retried record.
pub const RETRY_PSEUDO_RECORD_SLOTS: usize = 8;

#[derive(Default)]
pub(crate) struct FfiStyleTransactionOutput {
    scoped: bool,
    connected_element_count: u32,
    style_atoms_swept: bool,
    only_derived_child_reactions: bool,
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
    /// The transaction planned nothing but the child reactions the engine derived from the
    /// reactions C++ applied last: one more generation of the same style change, not a new one.
    pub only_derived_child_reactions: bool,
    /// The elements connected to the document as the transaction was taken.
    pub connected_element_count: u32,
}

/// A host-owned object the engine names but never follows.
///
/// The engine holds it as an integer rather than a raw pointer, because the read side an
/// evaluation step borrows has to be `Sync` and a raw pointer is neither `Send` nor `Sync` — for
/// the good reason that nothing about it says who may follow it. Only the bridge turns one back
/// into a pointer, at a host call, which never happens inside a step.
/// C++ hands one over as the address of the object it owns.
///
/// NB: The pointer that becomes a handle is exposed, and the handle becomes a pointer through
///     that exposed provenance, because the host call does dereference it (a style value is
///     retained, a registry is read). A bare address would be a pointer nothing may access.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct FfiHostHandle {
    pub address: usize,
}

impl FfiHostHandle {
    #[must_use]
    pub fn from_pointer(pointer: *const c_void) -> Self {
        Self {
            address: pointer.expose_provenance(),
        }
    }

    #[must_use]
    pub fn as_pointer(self) -> *const c_void {
        std::ptr::with_exposed_provenance(self.address)
    }

    #[must_use]
    pub fn is_none(self) -> bool {
        self.address == 0
    }
}

/// Document-wide scalar computation inputs captured at a style transaction boundary.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FfiDocumentStyleComputationInputs {
    pub in_quirks_mode: bool,
    pub viewport_width: f64,
    pub viewport_height: f64,
    pub root_font_size: f64,
    pub root_font_x_height: f64,
    pub root_font_cap_height: f64,
    pub root_font_zero_advance: f64,
    pub root_line_height: f64,
    pub root_font_metrics_depend_on_viewport_metrics: bool,
    /// The metrics of the document's initial font, which the document element's own font
    /// resolves against.
    pub initial_font_size: f64,
    pub initial_font_x_height: f64,
    pub initial_font_cap_height: f64,
    pub initial_font_zero_advance: f64,
    pub initial_font_size_raw: i32,
    pub default_font_size_raw: i32,
    pub device_pixels_per_css_pixel: f64,
    pub font_environment_generation: u64,
    /// The page's preferred color scheme and the document's supported schemes, as
    /// PreferredColorScheme codes; up to four supported schemes are carried.
    pub preferred_color_scheme: u8,
    pub has_document_supported_schemes: bool,
    pub document_supported_scheme_count: u8,
    pub document_supported_scheme_codes: [u8; 4],
    /// The document's custom-property registry, and the generation its registrations are at:
    /// what an engine-computed environment resolves registered names against.
    pub custom_property_registry: FfiHostHandle,
    pub custom_property_registration_generation: u64,
    /// What a `url()` resolves against, lent for the boundary call only: the document's base URL,
    /// and an `FfiStyleSheetResourceContextEntry` for each style sheet a rule may come from. The
    /// engine copies them and clears these fields before it keeps the inputs.
    pub document_base_url: FfiHostHandle,
    pub document_base_url_length: usize,
    pub style_sheet_resource_contexts: FfiHostHandle,
    pub style_sheet_resource_context_count: usize,
    /// What `media()` conditions in `if()` read, lent for the boundary call only as the style
    /// update's media environment holds them: an `FfiMediaFeatureValue` for each media feature,
    /// and the `FfiLengthResolutionContext` lengths in a media query resolve against. The engine
    /// copies them and clears these fields before it keeps the inputs.
    pub media_feature_values: FfiHostHandle,
    pub media_feature_value_count: usize,
    pub media_length_resolution_context: FfiHostHandle,
    /// What a custom function call reads, lent for the boundary call only: an
    /// `FfiCustomFunctionEntry` for each definition a scope sees. None where no scope holds an
    /// `@function` rule. The engine retains the definitions and clears these fields before it
    /// keeps the inputs.
    pub custom_functions: FfiHostHandle,
    pub custom_function_count: usize,
}

/// That a scope sees a custom function definition: the compiled function its name dereferences
/// to there, the scope (a `StyleScope`'s identity) whose calls see it, the scope defining it, and
/// the tree scope of the scope whose calls see it.
#[repr(C)]
pub struct FfiCustomFunctionEntry {
    pub function: *const c_void,
    pub caller_scope: usize,
    pub definition_scope: usize,
    pub tree_scope: u32,
}

/// One style sheet's resource context, keyed by the identity of its native sheet: the base URL a
/// `url()` in its rules resolves against, and whether the sheet is origin-clean.
#[repr(C)]
pub struct FfiStyleSheetResourceContextEntry {
    pub source_identity: u64,
    pub base_url: *const u8,
    pub base_url_length: usize,
    pub has_base_url: bool,
    pub origin_clean: bool,
}

impl FfiDocumentStyleComputationInputs {
    /// The document's custom-property registry. Every document publishes the one it creates with
    /// itself, so inputs naming none are a test's or a replay's, for a document that registers
    /// nothing.
    pub(crate) fn custom_property_registry(&self) -> &CustomPropertyRegistry {
        // SAFETY: A document's registry lives as long as the document, which outlives the inputs
        // it publishes.
        unsafe {
            self.custom_property_registry
                .as_pointer()
                .cast::<CustomPropertyRegistry>()
                .as_ref()
        }
        .unwrap_or_else(|| CustomPropertyRegistry::shared_empty())
    }
}

impl Default for FfiDocumentStyleComputationInputs {
    fn default() -> Self {
        Self {
            in_quirks_mode: false,
            viewport_width: 0.0,
            viewport_height: 0.0,
            root_font_size: 0.0,
            root_font_x_height: 0.0,
            root_font_cap_height: 0.0,
            root_font_zero_advance: 0.0,
            root_line_height: 0.0,
            root_font_metrics_depend_on_viewport_metrics: false,
            initial_font_size: 0.0,
            initial_font_x_height: 0.0,
            initial_font_cap_height: 0.0,
            initial_font_zero_advance: 0.0,
            initial_font_size_raw: 0,
            default_font_size_raw: 0,
            device_pixels_per_css_pixel: 0.0,
            font_environment_generation: 0,
            preferred_color_scheme: 0,
            has_document_supported_schemes: false,
            document_supported_scheme_count: 0,
            document_supported_scheme_codes: [0; 4],
            custom_property_registry: FfiHostHandle { address: 0 },
            custom_property_registration_generation: 0,
            document_base_url: FfiHostHandle { address: 0 },
            document_base_url_length: 0,
            style_sheet_resource_contexts: FfiHostHandle { address: 0 },
            style_sheet_resource_context_count: 0,
            media_feature_values: FfiHostHandle { address: 0 },
            media_feature_value_count: 0,
            media_length_resolution_context: FfiHostHandle { address: 0 },
            custom_functions: FfiHostHandle { address: 0 },
            custom_function_count: 0,
        }
    }
}

/// The computed values the font resolver reads beside the family, each at its own index in a
/// resolution request. C++ reads the request through this same enum, so the two sides cannot
/// disagree about which value is which.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum FontResolutionFeatureInput {
    FontFeatureSettings,
    FontVariationSettings,
    FontVariantCaps,
    FontVariantEastAsian,
    FontVariantEmoji,
    FontVariantLigatures,
    FontVariantNumeric,
    FontVariantPosition,
    FontVariantAlternates,
    FontKerning,
    TextRendering,
}

/// How many values a resolution request names, one per `FontResolutionFeatureInput`.
pub const FONT_RESOLUTION_FEATURE_INPUT_COUNT: usize = 11;
const _: () = assert!(FontResolutionFeatureInput::TextRendering as usize + 1 == FONT_RESOLUTION_FEATURE_INPUT_COUNT);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiFontResolutionRequest {
    pub font_family: FfiHostHandle,
    /// The computed values the resolver reads beside the family, by `FontResolutionFeatureInput`,
    /// each null when the property has its initial value. They select shaping features and
    /// variations, so two elements differing only in one of them resolve to different fonts.
    pub font_feature_values: [FfiHostHandle; FONT_RESOLUTION_FEATURE_INPUT_COUNT],
    pub font_size_raw: i32,
    pub font_slope: i32,
    pub font_weight: f64,
    pub font_width: f64,
    pub font_optical_sizing: u8,
    /// The tree scope whose `@font-feature-values` `font-variant-alternates` names features
    /// through, or the document's where the request names no `font-variant-alternates`.
    pub font_feature_values_scope: u32,
    pub font_environment_generation: u64,
}

impl FfiFontResolutionRequest {
    /// The font size the element's own lengths resolve against: the C++ working set's
    /// `CSSPixels` value, not the computed value's double.
    #[must_use]
    pub fn font_size(&self) -> f64 {
        crate::css::css_pixels::CssPixels::from_raw(self.font_size_raw).to_double()
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FfiResolvedFont {
    /// The two host font objects a resolution names. The engine holds them as handles and hands
    /// them straight back to C++ when it publishes a record; nothing in Rust follows either.
    pub first_available_font: FfiHostHandle,
    pub font_cascade_list: FfiHostHandle,
    pub ascent: f32,
    pub descent: f32,
    pub x_height: f32,
    pub zero_advance: f32,
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
            only_derived_child_reactions: false,
            style_atoms_swept: false,
            connected_element_count: 0,
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
            payload.write_bool(answer.uses_substitution);
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
    /// The base record's borrowed table, matching `WithAnimationsApplied::No`.
    pub longhand_table: *const c_void,
    pub animated_overlay: *const c_void,
    pub payload_count: usize,
    pub pseudo_element_styles: u64,
    pub counter_style_environment_identity: u64,
    pub animation_overlay_identity: u64,
    pub dependency_flags: u8,
    pub present: bool,
}

impl FfiStyleRecordView {
    fn missing() -> Self {
        Self {
            payloads: std::ptr::null(),
            base_payloads: std::ptr::null(),
            longhand_table: std::ptr::null(),
            animated_overlay: std::ptr::null(),
            payload_count: 0,
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
    pub unchanged: bool,
    pub donor_used: bool,
}

#[cfg(feature = "style-recording")]
#[derive(Clone, Copy)]
pub struct RecordedExactCascadeWinner {
    pub property: u16,
    pub value: u64,
    pub operator: CascadeOperator,
    pub animation_relevance: u32,
    pub important: bool,
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
    /// One plus the element-backed pseudo-element kind the element stands for in its host's
    /// shadow tree, or zero.
    pub associated_pseudo_kind_plus_one: u8,
    /// The `ElementBoxKind` the element asks for, as its raw byte.
    pub box_kind: u8,
    /// The element's `ElementStyleAdjustmentFact` bits: what the box-type transformation and the
    /// element style adjustments read of the DOM. Mirrors the C++ enum.
    pub adjustment_facts: u32,
    /// The element's `ElementConstructionFact` bits: what a layout row built for it records.
    pub construction_facts: u32,
}

/// The last pseudo-element kind C++ materializes as a synthetic pseudo-element; the kinds up to
/// it are the bits a style record's pseudo-element mask carries. Mirrors the C++
/// `last_synthetic_pseudo_element`.
pub const LAST_SYNTHETIC_PSEUDO_ELEMENT_KIND: u16 = 7;
pub const FIRST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND: u8 = 8;
pub const LAST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND: u8 = 13;

/// What C++ reports about a style reaction it applied, for the engine to derive the reactions of
/// the element's children. Mirrors C++ `StyleReactionAppliedFact`.
pub mod style_reaction_applied_fact {
    pub const DID_CHANGE_CUSTOM_PROPERTIES: u32 = 1 << 0;
    pub const INVALIDATION_IS_NONE: u32 = 1 << 1;
    pub const NEEDS_LAYOUT_TREE_REBUILD: u32 = 1 << 2;
    pub const RECOMPUTE_DESCENDANT_STYLES: u32 = 1 << 3;
    pub const CHILDREN_EXPLICITLY_INHERIT: u32 = 1 << 4;
    pub const SHADOW_CHILDREN_EXPLICITLY_INHERIT: u32 = 1 << 5;
    pub const WAS_UNSTYLED: u32 = 1 << 6;
    pub const WAS_DISPLAY_NONE: u32 = 1 << 7;
    /// The applied reaction moved the element's computed display, which its children's box-type
    /// transformation reads.
    pub const DISPLAY_CHANGED: u32 = 1 << 11;
}

/// The element facts the style computation's box-type transformation and element style
/// adjustments read. Mirrors C++ `ElementStyleAdjustmentFact`.
pub mod element_adjustment_fact {
    pub const IS_BR: u32 = 1 << 0;
    pub const IS_WBR: u32 = 1 << 1;
    pub const DISALLOW_DISPLAY_CONTENTS: u32 = 1 << 2;
    pub const REWRITE_INLINE_FLOW: u32 = 1 << 3;
    pub const IS_BUTTON: u32 = 1 << 4;
    pub const FORCE_LINE_HEIGHT_NORMAL: u32 = 1 << 5;
    pub const CHECK_INPUT_LINE_HEIGHT: u32 = 1 << 6;
    pub const HIDE_AUDIO_WITHOUT_CONTROLS: u32 = 1 << 7;
    pub const IS_TABLE: u32 = 1 << 8;
    pub const FORCE_POSITION_STATIC: u32 = 1 << 9;
    pub const FORCE_SYMBOL_DISPLAY_INLINE: u32 = 1 << 10;
    pub const IS_MATHML: u32 = 1 << 11;
    pub const IS_MATHML_MTABLE: u32 = 1 << 12;
    pub const IS_MATHML_MTR: u32 = 1 << 13;
    pub const IS_MATHML_MTD: u32 = 1 << 14;
    pub const IS_TH: u32 = 1 << 15;
    pub const IS_DOCUMENT_ELEMENT: u32 = 1 << 16;
    pub const HAS_ANIMATIONS: u32 = 1 << 17;
    /// An SVG graphics element folds its own transform into its SVG container's layout, which the
    /// damage of the element's record moves reads.
    pub const IS_SVG_GRAPHICS_ELEMENT: u32 = 1 << 18;
    /// The element stands for an element-reference pseudo-element of its shadow host, whose
    /// style C++ computes and installs on it.
    pub const IS_SHADOW_HOST_PSEUDO_ELEMENT: u32 = 1 << 19;
    /// An HTML `<body>`. The first one among an HTML `<html>` root's children propagates its style
    /// to the viewport, which layout and the damage of the element's record moves read.
    pub const IS_HTML_BODY_ELEMENT: u32 = 1 << 20;
    // The element types layout tree construction branches on. An element's type is fixed when it
    // is created, so the store holds these rather than the tree builder asking the DOM for them.
    pub const IS_SVG_ELEMENT: u32 = 1 << 21;
    pub const IS_SVG_SWITCH_ELEMENT: u32 = 1 << 22;
    pub const IS_SVG_CONTAINER: u32 = 1 << 23;
    pub const REQUIRES_SVG_CONTAINER: u32 = 1 << 24;
    pub const IS_SVG_FOREIGN_OBJECT_ELEMENT: u32 = 1 << 25;
    pub const IS_SVG_MASK_ELEMENT: u32 = 1 << 26;
    pub const IS_SVG_CLIP_PATH_ELEMENT: u32 = 1 << 27;
    pub const IS_SVG_PATTERN_ELEMENT: u32 = 1 << 28;
    /// Whether the element is rendered in the top layer. Unlike the type facts above it moves
    /// during the element's lifetime, and every move is recorded where the element's flag is set.
    pub const RENDERED_IN_TOP_LAYER: u32 = 1 << 29;
    /// An HTML `<html>`, whose first `<body>` child propagates its overflow to the viewport when it
    /// is the root.
    pub const IS_HTML_HTML_ELEMENT: u32 = 1 << 30;
    /// An HTML `<frameset>`, which is the document's body in place of a `<body>`.
    pub const IS_HTML_FRAMESET_ELEMENT: u32 = 1 << 31;
    /// The facts only the layout tree build reads. No style depends on them, so a style record
    /// computed for one element is as good for another that differs only in these.
    pub const LAYOUT_TREE_FACTS: u32 = IS_SVG_ELEMENT
        | IS_SVG_SWITCH_ELEMENT
        | IS_SVG_CONTAINER
        | REQUIRES_SVG_CONTAINER
        | IS_SVG_FOREIGN_OBJECT_ELEMENT
        | IS_SVG_MASK_ELEMENT
        | IS_SVG_CLIP_PATH_ELEMENT
        | IS_SVG_PATTERN_ELEMENT
        | RENDERED_IN_TOP_LAYER
        | IS_HTML_FRAMESET_ELEMENT;
    /// The facts only the damage of a record move reads. No style depends on them either.
    pub const RECORD_DAMAGE_FACTS: u32 = IS_SVG_GRAPHICS_ELEMENT | IS_HTML_BODY_ELEMENT | IS_HTML_HTML_ELEMENT;
}

/// What a layout row records about the element it is built for at the moment it is allocated, so
/// that the tree build can read it out of the mirror rather than off the DOM node. Mirrors the C++
/// `ElementConstructionFact`.
pub mod element_construction_fact {
    pub const IS_HTML_INPUT_ELEMENT: u32 = 1 << 0;
    /// This and `IS_DOCUMENT_ELEMENT` are also `element_adjustment_fact`s. A row is built out of
    /// this word alone, so they are published into both rather than read across two.
    pub const IS_HTML_HTML_ELEMENT: u32 = 1 << 1;
    pub const IS_IN_USER_AGENT_SHADOW_TREE: u32 = 1 << 2;
    pub const USES_BUTTON_LAYOUT: u32 = 1 << 3;
    pub const IS_EDITING_HOST: u32 = 1 << 4;
    pub const IS_BODY: u32 = 1 << 5;
    pub const IS_DOCUMENT_ELEMENT: u32 = 1 << 6;
}

/// Which principal box an element asks for before its computed style has a say. The element's own
/// type and state decide this; the tree build resolves it against the computed `display` and
/// `appearance`. Mirrors the C++ `CSS::ElementBoxKind`; it crosses the boundary as its raw byte.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
#[repr(u8)]
pub enum ElementBoxKind {
    /// The computed display decides the box on its own.
    #[default]
    FromDisplay = 0,
    /// The element generates no box, whatever its display says.
    NoBox = 1,
    Break = 2,
    FieldSet = 3,
    Legend = 4,
    Audio = 5,
    Video = 6,
    Canvas = 7,
    NavigableContainerViewport = 8,
    TextArea = 9,
    Image = 10,
    SvgGraphics = 11,
    SvgSvg = 12,
    SvgText = 13,
    SvgTextPath = 14,
    SvgForeignObject = 15,
    SvgImage = 16,
    SvgGeometry = 17,
    // An input's native widget. `appearance: none` suppresses it, and then the computed display
    // decides the box like it does for any other element.
    InputButton = 18,
    InputCheckBox = 19,
    InputRadioButton = 20,
    InputRange = 21,
    InputText = 22,
}

impl ElementBoxKind {
    /// Whether `appearance: none` suppresses the kind's native widget, leaving the box to the
    /// element's computed display.
    /// https://drafts.csswg.org/css-ui/#appearance-switching
    #[must_use]
    pub(crate) fn is_suppressed_by_appearance_none(self) -> bool {
        matches!(
            self,
            Self::InputButton | Self::InputCheckBox | Self::InputRadioButton | Self::InputRange | Self::InputText
        )
    }

    /// The kind C++ sends as `raw`. The two enums list the same kinds in the same order, which
    /// `ElementBoxKind.h` asserts for the last one.
    #[must_use]
    pub(crate) fn from_raw(raw: u8) -> Self {
        match raw {
            0 => Self::FromDisplay,
            1 => Self::NoBox,
            2 => Self::Break,
            3 => Self::FieldSet,
            4 => Self::Legend,
            5 => Self::Audio,
            6 => Self::Video,
            7 => Self::Canvas,
            8 => Self::NavigableContainerViewport,
            9 => Self::TextArea,
            10 => Self::Image,
            11 => Self::SvgGraphics,
            12 => Self::SvgSvg,
            13 => Self::SvgText,
            14 => Self::SvgTextPath,
            15 => Self::SvgForeignObject,
            16 => Self::SvgImage,
            17 => Self::SvgGeometry,
            18 => Self::InputButton,
            19 => Self::InputCheckBox,
            20 => Self::InputRadioButton,
            21 => Self::InputRange,
            22 => Self::InputText,
            _ => unreachable!("C++ sent an unknown element box kind {raw}"),
        }
    }
}

/// Which element the values of a replaced content input are of. See
/// `inputs::ReplacedContentInput`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiReplacedContentInputKind {
    None = 0,
    /// A `<textarea>`: `first` is its `cols`, and `second` its `rows`.
    TextArea = 1,
    /// An `<input>` whose type makes it no text entry widget: `first` is its `size`.
    Input = 2,
    /// An `<input>` whose type makes it a text entry widget: `first` is its `size`.
    TextEntryInput = 3,
    /// A `<canvas>`: `first` is its `width`, and `second` its `height`.
    Canvas = 4,
    /// The natural size of what an element has loaded, such as a video's, in raw fixed-point CSS
    /// pixels: `first` is its width, `second` its height, and `third` and `fourth` the numerator
    /// and denominator of its aspect ratio. `present` says which of them it has.
    NaturalSize = 5,
    /// An SVG `<image>` whose image has decoded: its natural size, as `NaturalSize`.
    DecodedSvgImage = 6,
}

/// The bits of a replaced content input's `present`, for the kinds whose values can be missing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiReplacedContentInputPresent {
    First = 1 << 0,
    Second = 1 << 1,
    ThirdAndFourth = 1 << 2,
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

fn write_custom_declarations(declared: &[CustomDeclaration], payload: &mut super::record_replay::PayloadWriter) {
    payload.write_length(declared.len());
    for property in declared {
        payload.write_u32(property.name.0);
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

impl StyleEngineState {
    fn install_ffi_style_node_query(&mut self, nodes: Vec<u32>) -> FfiStyleNodeSlice {
        let bytes = (nodes.capacity() * size_of::<u32>()) as u64;
        self.host.ffi_style_node_query = nodes;
        self.host
            .ffi_style_node_query_memory
            .resize_required_to(&mut self.retained.memory, bytes);
        FfiStyleNodeSlice {
            nodes: self.host.ffi_style_node_query.as_ptr(),
            count: self.host.ffi_style_node_query.len(),
        }
    }

    fn clear_ffi_style_node_query(&mut self) {
        self.host.ffi_style_node_query = Vec::new();
        self.host.ffi_style_node_query_memory.shrink_to(0);
    }

    fn install_ffi_style_transaction_output(&mut self, output: FfiStyleTransactionOutput) {
        let bytes = (output.answers.capacity() * size_of::<FfiStyleDelta>()
            + output.reclaimed_style_atoms.capacity() * size_of::<FfiReclaimedStyleAtom>()) as u64;
        self.host.ffi_style_transaction_output = output;
        self.host
            .ffi_style_transaction_output_memory
            .resize_required_to(&mut self.retained.memory, bytes);
    }

    pub(super) fn clear_ffi_style_transaction_output(&mut self) {
        self.host.ffi_style_transaction_output = FfiStyleTransactionOutput::default();
        self.host.ffi_style_transaction_output_memory.shrink_to(0);
    }

    /// Apply one flat transaction. Tree deltas are staged in arrival order so derived neighbour
    /// rows follow the live tree step by step, while the journal normalizes for discovery.
    #[allow(clippy::too_many_arguments)]
    pub fn apply_transaction_batch(
        &mut self,
        tree_deltas: &[FfiTreeDelta],
        arrival_columns: (&[FfiElementArrival], &[u32]),
        local_feature_deltas: &[FfiLocalFeatureDelta],
        state_deltas: &[FfiStateDelta],
        element_declaration_deltas: &[FfiElementDeclarationDelta],
        element_style_inputs: &[FfiElementStyleInput],
        counters: &mut Counters,
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
                self.bulk_load_initial_tree(document_root, &initial_arrivals, counters);
                initial_tree_was_bulk_loaded = true;
            }
        }
        if !initial_tree_was_bulk_loaded {
            self.host.initial_tree_batch_applied |= !tree_deltas.is_empty();
            for delta in tree_deltas {
                let Some(node) = StyleNodeID::from_raw(delta.node) else {
                    continue;
                };
                let old = delta.old_connected.then(|| delta.old_relations.decode());
                let new = delta.new_connected.then(|| delta.new_relations.decode());
                self.record_tree_delta(node, old, new, counters);
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
                // The host can submit an element's features a batch after its arrival, where it submitted its input
                // as the element's subtree was being inserted: the arrival is pending still.
                let arriving = node_is_arriving(node) || self.node_arrival_is_pending(node);
                self.record_element_arrival(node, arrival, &custom_states, arriving, counters);
            }
            self.settle_batched_inputs(counters);
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
                counters,
            );
        }
        self.settle_batched_inputs(counters);

        for delta in state_deltas {
            let Some(node) = StyleNodeID::from_raw(delta.node) else {
                continue;
            };
            self.record_batched_state(
                node,
                decode_state_fact(delta.fact),
                delta.new_value,
                node_is_arriving(node),
                counters,
            );
        }
        self.settle_batched_inputs(counters);

        for delta in element_declaration_deltas {
            let Some(node) = StyleNodeID::from_raw(delta.node) else {
                continue;
            };
            // Zero means the node has no declaration block of that kind on that side.
            let block = |raw: u32| (raw != 0).then_some(DeclarationBlockID(raw));
            // The block's contents moved even where the host's object for it did not, so what makes
            // this a change is a fresh version of the block, which is minted here rather than by the
            // host: the host says only that the node has one.
            let new_block =
                (delta.new_block != 0).then(|| DeclarationBlockID(self.retained.next_declaration_block_version()));
            self.record_input(
                InputKey::ElementDeclaration(node, decode_element_declaration_kind(delta.kind)),
                InputValue::ElementDeclaration(block(delta.old_block)),
                InputValue::ElementDeclaration(new_block),
                counters,
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
                counters,
            );
        }
    }
}

/// Creates one document's style engine, which its render state owns.
pub(crate) fn create_document_style_engine(device_class: FfiDeviceClass) -> Box<StyleEngine> {
    // The host registers the style groups before it creates a document's render state; one a test creates has none.
    let style_groups = crate::css::computed_values::StyleGroupMasks::registered();
    let device_class = device_class.decode();
    let mut engine = Box::new(StyleEngine::new(device_class));
    engine.begin_recording(device_class);
    engine.record_boundary_call(EventKind::SetComputedGroupDependencyMasks, |payload| {
        payload.write_bool(style_groups.is_some());
        if let Some(style_groups) = style_groups {
            payload.write_u16(style_groups.first_property);
            payload.write_u32_slice(style_groups.masks);
            payload.write_u32_slice(style_groups.output_masks);
        }
    });
    engine
}

/// The definition a plan last applied to one of the host's CSS animations, published beside the
/// animation's name: what the style computation handed the host for it, field for field, so that a
/// definition just computed and one published back compare directly. A plan whose every definition
/// equals what its animation last had applied changes nothing, and the engine does not hand it
/// over.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiAppliedAnimationDefinition {
    pub values: FfiAppliedAnimationValues,
    /// The keyframe set the plan gave the animation's effect, which the animation retains.
    pub keyframe_set: *const c_void,
    /// The computed `animation-timing-function`, which the animation retains, or null where no plan
    /// has described the animation yet.
    pub timing_function: *const c_void,
}

/// What an applied animation definition holds by value.
#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
pub struct FfiAppliedAnimationValues {
    pub duration_is_auto: bool,
    pub duration: f64,
    pub iteration_count: f64,
    pub direction: u8,
    pub play_state: u8,
    pub delay: f64,
    pub fill_mode: u8,
    pub composition: u8,
    /// An `FfiAnimationTimelineKind`.
    pub timeline_kind: u8,
    pub scroll_scroller: u8,
    pub scroll_axis: u8,
}

// SAFETY: plain values and pointers, recorded from definitions the host published.
unsafe impl super::record_replay::RawRecord for FfiAppliedAnimationDefinition {}
// SAFETY: the keyframe set is only compared by address, and the timing function is immutable style
// value data the animation retains for as long as the definition is published, read only to
// compare it.
unsafe impl Send for FfiAppliedAnimationDefinition {}
unsafe impl Sync for FfiAppliedAnimationDefinition {}

/// Replaces the `@keyframes` row of one style scope: each name the scope defines, packed into one
/// buffer of code units with a length each, with the host's keyframe set for it. An empty row gives
/// the scope's row up; a shadow root's scope is named by the root's pointer identity as well.
///
/// The host publishes a scope whenever its rule cache is built, and keeps a reference to every set
/// the row names until it replaces or gives the row up. This is not a recorded boundary event: a
/// keyframe set is a host pointer, which a replayed engine could not be handed.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and each buffer must hold the
/// count it is given.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_tree_scope_animation_keyframes(
    host: *const DocumentHost,
    tree_scope: u32,
    shadow_root_identity: usize,
    name_lengths: *const u32,
    name_units: *const u16,
    name_unit_count: usize,
    keyframe_sets: *const usize,
    count: usize,
) {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        let name_lengths = unsafe { ffi_slice(name_lengths, count) };
        let name_units = unsafe { ffi_slice(name_units, name_unit_count) };
        let keyframe_sets = unsafe { ffi_slice(keyframe_sets, count) };
        engine.set_tree_scope_animation_keyframes(
            TreeScopeID(tree_scope),
            shadow_root_identity,
            name_lengths,
            name_units,
            keyframe_sets,
        );
    });
}

/// Creates a replay engine whose atom keys are opaque capture tokens rather than live fly strings.
pub fn style_engine_create_for_replay(device_class: FfiDeviceClass) -> StyleEngineHandle {
    abort_on_panic(|| StyleEngineHandle::create(Box::new(StyleEngine::new_for_replay(device_class.decode()))))
}

/// Hands a replay engine the style groups the recording registered after creating it.
///
/// # Safety
/// `engine` must be live.
pub unsafe fn style_engine_use_registered_style_groups(engine: StyleEngineHandle) {
    let engine = unsafe { engine.get_mut() };
    engine.retained.style_groups = crate::css::computed_values::StyleGroupMasks::registered_or_none();
}

/// Keeps the store behind an environment an element holds, and what a child inherits of it,
/// `inheritable` with its store: the engine resolves the environments of an element's children
/// over it, and substitutes the element's own values under an animation overlay.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `store` and
/// `inheritable_store` null or live raw `Arc` pointers to a `CustomPropertyStore`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_note_custom_property_environment(
    host: *const DocumentHost,
    identity: u64,
    store: *const c_void,
    inheritable: u64,
    inheritable_store: *const c_void,
) {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(host, |engine| unsafe {
        note_custom_property_environment(engine, identity, store, inheritable, inheritable_store);
    });
}

/// [`style_engine_note_custom_property_environment`] on `engine`, which the style replay tool calls
/// as well.
///
/// # Safety
/// As for [`style_engine_note_custom_property_environment`], for the arguments after `engine`.
pub unsafe fn note_custom_property_environment(
    engine: &mut StyleEngine,
    identity: u64,
    store: *const c_void,
    inheritable: u64,
    inheritable_store: *const c_void,
) {
    unsafe {
        engine.custom_property_environments.retain(identity, store);
        engine
            .custom_property_environments
            .note_inheritable(identity, inheritable, inheritable_store);
    }
    engine.record_boundary_call(EventKind::NoteCustomPropertyEnvironment, |payload| {
        payload.write_u64(identity);
        payload.write_u64(inheritable);
    });
}

/// The environment a child inherits from one the engine resolved: itself, unless a registration
/// keeps some of its custom properties from inheriting.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_inheritable_custom_property_environment(
    host: *const DocumentHost,
    identity: u64,
) -> u64 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| engine.custom_property_environments.inheritable(identity))
}

/// How one animation effect's description travels across the boundary: the ranges of the flat
/// buffers that belong to it.
#[repr(C)]
pub struct FfiPublishedAnimationEffect {
    pub identity: u64,
    pub generation: u64,
    /// The effect belongs to a CSS transition, which the interpolation treats differently.
    pub is_transition: bool,
    /// The effect's keyframes come from a style sheet, which their URLs resolve against: its base
    /// URL is the range of the base URL buffer below.
    pub has_resource_context: bool,
    pub resource_context_is_origin_clean: bool,
    pub first_keyframe: u32,
    pub keyframe_count: u32,
    pub base_url_offset: u32,
    pub base_url_length: u32,
}

/// The easing function a published keyframe spells out.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum FfiPublishedEasingKind {
    Linear,
    CubicBezier,
    Steps,
}

/// One keyframe of a published effect: its offset on the scale the host keys keyframes by, its
/// easing spelled out, its composite operation and the ranges of what it declares.
#[repr(C)]
pub struct FfiPublishedAnimationKeyframe {
    pub key: i64,
    pub easing_kind: FfiPublishedEasingKind,
    pub step_position: u8,
    pub interval_count: i32,
    pub x1: f64,
    pub y1: f64,
    pub x2: f64,
    pub y2: f64,
    pub first_linear_point: u32,
    pub linear_point_count: u32,
    /// The keyframe's own easing where it still has to be substituted against the element being
    /// sampled, or null.
    pub easing_value: *const c_void,
    /// The keyframe's composite operation, with `auto` already the effect's own.
    pub composite: crate::css::animation::FfiCompositeOperation,
    pub first_declaration: u32,
    pub declaration_count: u32,
    pub first_custom_declaration: u32,
    pub custom_declaration_count: u32,
}

/// One control point of a published `linear()` easing.
#[repr(C)]
pub struct FfiPublishedLinearEasingPoint {
    pub input: f64,
    pub output: f64,
}

/// One longhand a published keyframe declares. A null value stands for the element's own value,
/// which a keyframe the host synthesized holds and which is not known until the element is
/// sampled.
#[repr(C)]
pub struct FfiPublishedAnimationDeclaration {
    pub property_id: u16,
    pub value: *const c_void,
}

/// One custom property a published keyframe declares, by the raw representation of its name. A
/// null value stands for the element's own value of it.
#[repr(C)]
pub struct FfiPublishedAnimationCustomDeclaration {
    pub name: usize,
    pub value: *const c_void,
}

/// The version of one animation effect the host is about to sample.
#[repr(C)]
pub struct FfiAnimationEffectVersion {
    pub identity: u64,
    pub generation: u64,
}

/// Describe the effects one of an element's animation lists holds, in composite order, for the
/// style engine to sample them from.
///
/// Hand-written rather than a recorded boundary event, because a keyframe declaration carries a
/// style value the host holds, which a replayed engine could not be handed.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, every buffer must hold the count
/// it is given, and every value and custom-property name the buffers name must be live for the
/// duration of the call.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn style_engine_set_element_animation_effect_descriptions(
    host: *const DocumentHost,
    node: u32,
    slot: u8,
    effects: *const FfiPublishedAnimationEffect,
    effect_count: usize,
    keyframes: *const FfiPublishedAnimationKeyframe,
    keyframe_count: usize,
    declarations: *const FfiPublishedAnimationDeclaration,
    declaration_count: usize,
    custom_declarations: *const FfiPublishedAnimationCustomDeclaration,
    custom_declaration_count: usize,
    linear_points: *const FfiPublishedLinearEasingPoint,
    linear_point_count: usize,
    base_url_bytes: *const u8,
    base_url_byte_count: usize,
) {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return;
        };
        let buffers = unsafe {
            super::effect_descriptions::PublishedEffectBuffers {
                effects: ffi_slice(effects, effect_count),
                keyframes: ffi_slice(keyframes, keyframe_count),
                declarations: ffi_slice(declarations, declaration_count),
                custom_declarations: ffi_slice(custom_declarations, custom_declaration_count),
                linear_points: ffi_slice(linear_points, linear_point_count),
                base_url_bytes: ffi_slice(base_url_bytes, base_url_byte_count),
            }
        };
        unsafe { engine.animation_effect_descriptions.set(node, slot, buffers) };
    });
}

/// Whether the engine describes one of an element's animation lists as holding exactly these
/// versions of its effects, in this order, so that the host need not describe them again.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `versions` must hold `count`
/// elements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_describes_animation_effects(
    host: *const DocumentHost,
    node: u32,
    slot: u8,
    versions: *const FfiAnimationEffectVersion,
    count: usize,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        StyleNodeID::from_raw(node).is_some_and(|node| {
            engine
                .animation_effect_descriptions
                .describe(node, slot, unsafe { ffi_slice(versions, count) })
        })
    })
}

/// Applies the memory policy used while producing a replay recording.
///
/// # Safety
/// `engine` must be live.
pub unsafe fn style_engine_use_recording_memory_policy(engine: crate::css::style::StyleEngineHandle) {
    let engine = unsafe { engine.get_mut() };
    engine.memory.enable_recording_policy();
}

#[unsafe(no_mangle)]
pub extern "C" fn style_engine_verification_gate_bits() -> u8 {
    super::verification_gate_bits()
}

/// Destroys a replay engine.
///
/// # Safety
/// `engine` must be a handle returned by `style_engine_create_for_replay` and not yet destroyed.
pub unsafe fn style_engine_destroy(engine: crate::css::style::StyleEngineHandle) {
    let mut engine = unsafe { engine.destroy() };
    engine.end_recording();
}

/// Ends the transaction the engine published last, and answers the identities its end released, for the host to mint
/// again.
pub(crate) fn end_style_transaction(engine: &mut StyleEngine) -> Vec<u32> {
    engine.clear_ffi_style_node_query();
    engine.clear_ffi_style_transaction_output();
    let released = engine.discard_style_transaction_outputs();
    engine.record_boundary_call(EventKind::DiscardStyleTransactionOutputs, |payload| {
        payload.write_u32_slice(&released);
    });
    released
}

/// Ends the transaction a replay engine published last, and answers the identities its end released.
///
/// # Safety
/// `engine` must be live.
pub unsafe fn style_engine_end_style_transaction_for_replay(engine: crate::css::style::StyleEngineHandle) -> Vec<u32> {
    end_style_transaction(unsafe { engine.get_mut() })
}

/// Returns the live element descendants whose inheritance path begins at `root` in the flat tree.
///
/// # Safety
/// `engine` must be live. The returned node slice remains valid until the next mutable
/// `style_engine_*` entry point or an explicit discard of the flat-tree descendants.
pub unsafe fn style_engine_flat_tree_descendants(
    engine: crate::css::style::StyleEngineHandle,
    root: u32,
) -> FfiStyleNodeSlice {
    let engine = unsafe { engine.get_mut() };
    engine.clear_ffi_style_node_query();
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
    engine.install_ffi_style_node_query(descendants)
}

/// Discards the borrowed flat-tree descendant slice.
///
/// # Safety
/// `engine` must be live.
pub unsafe fn style_engine_discard_flat_tree_descendants(engine: crate::css::style::StyleEngineHandle) {
    let engine = unsafe { engine.get_mut() };
    engine.clear_ffi_style_node_query();
}

pub(super) fn write_recording_tree_deltas(tree: &[FfiTreeDelta], payload: &mut super::record_replay::PayloadWriter) {
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

pub(super) fn write_recording_state_deltas(
    state_deltas: &[FfiStateDelta],
    payload: &mut super::record_replay::PayloadWriter,
) {
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

pub(super) fn write_recording_element_style_inputs(
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

/// Publish already bound immutable selector inputs, after all host interning has finished.
pub(crate) fn publish_style_rule(
    engine: &mut StyleEngine,
    sheet: u32,
    before_rule: u32,
    compiled: &[&CompiledSelector],
    namespaces: NamespaceScope,
    bound_scope: &BoundScopeChain,
) -> u32 {
    if sheet == 0 || compiled.is_empty() {
        engine.record_boundary_call(EventKind::AddStyleRule, |payload| {
            payload.write_u32(sheet);
            payload.write_u32(before_rule);
            payload.write_u32(0);
        });
        return 0;
    }
    let scope: Vec<_> = bound_scope.roots.iter().map(|selector| selector.as_ref()).collect();
    let limits: Vec<_> = bound_scope.limits.iter().map(|selector| selector.as_ref()).collect();
    let before = match before_rule {
        0 => None,
        id => Some(RuleID(id - 1)),
    };
    let scope = ScopeChain {
        roots: &scope,
        limits: &limits,
        levels: &bound_scope.levels,
        implicit_roots: &bound_scope.implicit_roots,
    };
    let rule = engine.add_style_rule_in_scope(SheetID(sheet - 1), before, compiled, namespaces, &scope);
    let result = rule.0 + 1;
    engine.record_boundary_call(EventKind::AddStyleRule, |payload| {
        payload.write_u32(sheet);
        payload.write_u32(before_rule);
        payload.write_u32(result);
        write_recording_atom_mappings(engine, payload);
        super::selector::replay::write(engine.selector_program_for_rule(rule), payload);
    });
    result
}

/// Publishes a style rule of a user-agent sheet with the selector program the process compiled for
/// it, or answers `None` when the rule is not one or the engine compiles its own.
pub(crate) fn publish_user_agent_style_rule(
    engine: &mut StyleEngine,
    sheet: u32,
    before_rule: u32,
    rule_identity: u64,
    compiled: &[&CompiledSelector],
    rules: &crate::css::rule::NativeRuleList,
    bound_scope: &BoundScopeChain,
) -> Option<u32> {
    if sheet == 0 || !bound_scope.levels.is_empty() {
        return None;
    }
    let sheet = SheetID(sheet - 1);
    if engine.program.sheet_origin(sheet) != crate::css::cascaded_properties::CascadeOrigin::UserAgent
        || !engine.shares_user_agent_selector_programs()
    {
        return None;
    }
    let before = (before_rule != 0).then(|| RuleID(before_rule - 1));
    Some(
        engine
            .add_user_agent_style_rule(sheet, before, rule_identity, compiled, rules)
            .0
            + 1,
    )
}

/// Installs one already compiled semantic selector program.
///
/// # Safety
/// `engine` must be live, and the sheet and optional rule handles must belong to it.
#[cfg(feature = "style-recording")]
pub unsafe fn replay_add_style_rule(
    engine: crate::css::style::StyleEngineHandle,
    sheet: u32,
    before_rule: u32,
    selector_program: super::selector::SelectorProgram,
) -> u32 {
    let engine = unsafe { engine.get_mut() };
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
    engine: crate::css::style::StyleEngineHandle,
    rule: u32,
    selector_program: super::selector::SelectorProgram,
) {
    let engine = unsafe { engine.get_mut() };
    engine.replace_replayed_style_rule_selectors(RuleID(rule - 1), selector_program);
}

/// Installs semantic declared-property identities without C++ style-value pointers.
///
/// # Safety
/// `engine` must be live and `node` must name one of its style nodes.
#[cfg(feature = "style-recording")]
pub unsafe fn replay_set_element_declared_properties(
    engine: crate::css::style::StyleEngineHandle,
    node: u32,
    kind: FfiElementDeclarationKind,
    declared: &[DeclaredProperty],
    custom_declarations: &[CustomDeclaration],
) {
    let engine = unsafe { engine.get_mut() };
    let node = StyleNodeID::from_raw(node).expect("recorded style node identities are nonzero");
    // A replay computes no record, so no value is read: each declaration stands for its written
    // value with the guaranteed-invalid one.
    let unwritten = || RetainedStyleValueData::from_owned(crate::css::style_value::StyleValueData::GuaranteedInvalid);
    engine.set_element_declared_properties(
        node,
        decode_element_declaration_kind(kind),
        declared.iter().map(|&declared| (declared, unwritten())).collect(),
        custom_declarations
            .iter()
            .map(|&declared| (declared, unwritten()))
            .collect(),
    );
}

/// Takes the random base value a recorded draw gave, where a replay would draw one of its own.
///
/// # Safety
/// `engine` must be live.
#[cfg(feature = "style-recording")]
pub unsafe fn replay_random_base_value(
    engine: crate::css::style::StyleEngineHandle,
    node: u32,
    name: &[u16],
    element_shared: bool,
    value_bits: u64,
) {
    let engine = unsafe { engine.get_mut() };
    engine.random_base_values.set(
        StyleNodeID::from_raw(node),
        name,
        element_shared,
        f64::from_bits(value_bits),
    );
}

/// Installs semantic declared-property identities without C++ style-value pointers.
///
/// # Safety
/// `engine` must be live and `rule` must name one of its style rules.
#[cfg(feature = "style-recording")]
pub unsafe fn replay_set_rule_declared_properties(
    engine: crate::css::style::StyleEngineHandle,
    rule: u32,
    declared: &[DeclaredProperty],
    custom_declarations: &[CustomDeclaration],
) {
    let engine = unsafe { engine.get_mut() };
    engine.set_rule_declared_properties_with_written_values(
        RuleID(rule - 1),
        declared,
        Vec::new(),
        custom_declarations.to_vec(),
        Vec::new(),
    );
}

/// Replace selectors using immutable compilation inputs, preserving recording and rule identity.
pub(crate) fn publish_style_rule_selectors(
    engine: &mut StyleEngine,
    rule: u32,
    compiled: &[&CompiledSelector],
    namespaces: NamespaceScope,
    bound_scope: &BoundScopeChain,
) {
    if rule == 0 || compiled.is_empty() {
        return;
    }
    let scope: Vec<_> = bound_scope.roots.iter().map(|selector| selector.as_ref()).collect();
    let limits: Vec<_> = bound_scope.limits.iter().map(|selector| selector.as_ref()).collect();
    let scope = ScopeChain {
        roots: &scope,
        limits: &limits,
        levels: &bound_scope.levels,
        implicit_roots: &bound_scope.implicit_roots,
    };
    engine.replace_style_rule_selectors(RuleID(rule - 1), compiled, namespaces, &scope);
    engine.record_boundary_call(EventKind::ReplaceStyleRuleSelectors, |payload| {
        payload.write_u32(rule);
        write_recording_atom_mappings(engine, payload);
        super::selector::replay::write(engine.selector_program_for_rule(RuleID(rule - 1)), payload);
    });
}
#[derive(Clone, Default)]
pub(crate) struct BoundScopeChain {
    roots: Vec<std::sync::Arc<CompiledSelector>>,
    limits: Vec<std::sync::Arc<CompiledSelector>>,
    levels: Vec<(u32, u32)>,
    implicit_roots: Vec<Option<ImplicitScopeRoot>>,
}

impl BoundScopeChain {
    pub(crate) fn push(
        &mut self,
        start: Option<&crate::css::selector_parser::RustParsedSelectorList>,
        end: Option<&crate::css::selector_parser::RustParsedSelectorList>,
        implicit_root: u32,
    ) {
        self.levels.push((
            u32::try_from(start.map_or(0, |list| list.selectors.len())).expect("scope root count exceeds u32"),
            u32::try_from(end.map_or(0, |list| list.selectors.len())).expect("scope limit count exceeds u32"),
        ));
        // An explicit start that transforms to an empty list matches no roots. It must not acquire
        // the DOM root that an omitted start uses.
        let implicit_root = if start.is_none() { implicit_root } else { 0 };
        self.implicit_roots.push(match implicit_root {
            0 => None,
            u32::MAX => Some(ImplicitScopeRoot::ContainingTree),
            node => StyleNodeID::from_raw(node).map(ImplicitScopeRoot::Node),
        });
        if let Some(start) = start {
            self.roots.extend(start.selectors.iter().cloned());
        }
        if let Some(end) = end {
            self.limits.extend(end.selectors.iter().cloned());
        }
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
                semantic_declaration: if engine.program.declarations_are_complete_for(entry.rule) {
                    engine.program.ensure_semantic_declaration(entry.rule).0
                } else {
                    0
                },
                pseudo_element: entry.pseudo_element.map_or(u32::MAX, |target| u32::from(target.kind.0)),
                scope_host: engine.cascade_context_host(entry.rule, entry.tree_scope),
                scope_proximity: entry.scope_proximity,
            };
        }
    }
    matches.len()
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
/// `host` must be a live document host, on its document's thread, and `out` must point at
/// `capacity` writable `FfiRuleMatch` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_match_element(
    host: *const DocumentHost,
    node: u32,
    out: *mut FfiRuleMatch,
    capacity: usize,
    compact_for_cascade: bool,
) -> usize {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(host, |engine| unsafe {
        match_element(engine, node, out, capacity, compact_for_cascade)
    })
}

/// [`style_engine_match_element`] on `engine`, which the style replay tool calls as well.
///
/// # Safety
/// As for [`style_engine_match_element`], for the arguments after `engine`.
pub unsafe fn match_element(
    engine: &mut StyleEngine,
    node: u32,
    out: *mut FfiRuleMatch,
    capacity: usize,
    compact_for_cascade: bool,
) -> usize {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return usize::MAX;
    };
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
}

fn collect_native_custom_declarations(
    engine: &mut StyleEngine,
    custom_properties: &[crate::css::declaration_block::CustomProperty],
) -> Vec<(CustomDeclaration, RetainedStyleValueData)> {
    custom_properties
        .iter()
        .map(|property| {
            let name = property.name.to_fly_string();
            let atom = intern_native_atom(engine, name.raw());
            unsafe { note_native_custom_property_name(engine, atom, name.raw(), property.name.units()) };
            let declaration = &property.declaration;
            // Custom properties retain their authored values, without normal-property
            // canonicalization. Their token spelling is observable after substitution.
            let value = unsafe { engine.intern_specified_value(std::sync::Arc::as_ptr(&declaration.value)) };
            let written = unsafe {
                RetainedStyleValueData::from_retained_pointer(std::sync::Arc::into_raw(declaration.value.clone()))
            };
            let declared = CustomDeclaration {
                name: atom,
                important: declaration.important,
                operator: super::program_updates::declaration_operator(&declaration.value),
                value,
            };
            (declared, written)
        })
        .collect()
}

fn register_element_declared_properties(
    engine: &mut StyleEngine,
    node: StyleNodeID,
    kind: FfiElementDeclarationKind,
    declarations: &[crate::css::declaration_block::DeclaredProperty],
    custom_properties: &[crate::css::declaration_block::CustomProperty],
) -> bool {
    use crate::css::property_metadata::property_defines_a_css_transition;
    let has_transitions = declarations
        .iter()
        .any(|declaration| property_defines_a_css_transition(declaration.property_id));
    let declarations = engine.intern_element_declared_properties(declarations);
    let custom_declarations = collect_native_custom_declarations(engine, custom_properties);
    let declaration_kind = decode_element_declaration_kind(kind);
    engine.set_element_declared_properties(node, declaration_kind, declarations, custom_declarations);
    // The engine holds the declarations as they were published.
    engine.record_boundary_call(EventKind::SetElementDeclaredProperties, |payload| {
        let declared = engine.facts.element_declared_properties(node, declaration_kind);
        let custom_declared = match declaration_kind {
            ElementDeclarationKind::InlineStyle => engine.facts.element_custom_declarations(node),
            _ => &[],
        };
        payload.write_u32(node.raw());
        payload.write_u8(kind as u8);
        write_declared_properties(declared, payload);
        write_custom_declarations(custom_declared, payload);
    });
    has_transitions
}

/// Registers an element's native inline declaration block, or clears it when null.
/// Returns whether the declarations can define transitions.
///
/// # Safety
/// `host` must be a live document host, on its document's thread. A non-null `block` must borrow a
/// live `DeclarationBlock`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_element_inline_style_properties(
    host: *const DocumentHost,
    node: u32,
    block: *const c_void,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return false;
        };
        let block = unsafe { block.cast::<crate::css::declaration_block::DeclarationBlock>().as_ref() };
        let data = block.map(|block| block.data());
        register_element_declared_properties(
            engine,
            node,
            FfiElementDeclarationKind::InlineStyle,
            data.as_ref().map_or(&[], |data| data.properties.as_slice()),
            data.as_ref().map_or(&[], |data| data.custom_properties.as_slice()),
        )
    })
}

/// Registers borrowed presentation hints and returns whether they can define transitions.
///
/// # Safety
/// `host` must be a live document host, on its document's thread. `properties` must borrow `count`
/// `FfiDeclaredProperty` entries whose values point at live, Arc-backed `StyleValueData` roots.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_element_presentational_hint_properties(
    host: *const DocumentHost,
    node: u32,
    kind: FfiElementDeclarationKind,
    properties: *const c_void,
    count: usize,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        use crate::css::declaration_block::{DeclarationBlockData, FfiDeclaredProperty, declaration_from_view};
        let Some(node) = StyleNodeID::from_raw(node) else {
            return false;
        };
        let properties = if count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(properties.cast::<FfiDeclaredProperty>(), count) }
        };
        // A hint may name a shorthand. Expanded as any declaration block is, it declares every
        // longhand it decides, and so do all declarations the engine is given.
        let mut hints = DeclarationBlockData::default();
        for property in properties {
            hints.append_in_specified_order(unsafe { declaration_from_view(property) });
        }
        register_element_declared_properties(engine, node, kind, &hints.properties, &[])
    })
}

#[cfg(feature = "style-recording")]
pub unsafe fn replay_publish_exact_cascade_state(
    engine: crate::css::style::StyleEngineHandle,
    node: u32,
    pseudo_kind: u8,
    winners: &[RecordedExactCascadeWinner],
    inherited_style_groups: u8,
    donor_node: u32,
    donor_style_record: u64,
) -> (FfiExactCascadePublication, bool) {
    let engine = unsafe { engine.get_mut() };
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
                    important: winner.important,
                },
            )
        })
        .collect::<Vec<_>>();
    let (publication, had_previous) = engine.publish_exact_cascade_winners(
        super::computed::ComputedStyleTarget::new(node, pseudo_kind),
        &winners,
        inherited_style_groups,
        exact_cascade_donor(donor_node, donor_style_record),
    );
    (publication, had_previous)
}

#[cfg(feature = "style-recording")]
fn exact_cascade_donor(donor_node: u32, donor_style_record: u64) -> Option<super::publication::ExactCascadeDonor> {
    let node = StyleNodeID::from_raw(donor_node)?;
    (donor_style_record != 0).then_some(super::publication::ExactCascadeDonor {
        node,
        style_record: donor_style_record,
    })
}

#[cfg(feature = "style-recording")]
pub unsafe fn replay_exact_cascade_generation_snapshot(
    engine: crate::css::style::StyleEngineHandle,
    node: u32,
    pseudo_kind: u8,
) -> (u64, Option<u64>) {
    let engine = unsafe { engine.get() };
    let node = StyleNodeID::from_raw(node).expect("recorded style node identities are nonzero");
    engine.exact_cascade_generation_snapshot(super::computed::ComputedStyleTarget::new(node, pseudo_kind))
}

#[cfg(feature = "style-replay")]
pub fn replay_style_value(token: u64, dependency_flags: u8) -> *const c_void {
    crate::css::style_value::register_replay_style_value(token, dependency_flags).cast()
}

#[cfg(feature = "style-replay")]
pub fn replay_style_value_token(value: *const c_void) -> Option<u64> {
    crate::css::style_value::replay_style_value_token(value.cast())
}

#[cfg(feature = "style-recording")]
pub unsafe fn replay_memory_pressure_snapshot(
    engine: crate::css::style::StyleEngineHandle,
) -> FfiMemoryPressureSnapshot {
    let engine = unsafe { engine.get() };
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
/// `host` must be a live document host, on its document's thread. Every non-empty array must have
/// its reported number of readable entries. Group payloads and style values must remain live for
/// this call. `animated_overlay` must be null or point at a live Rust animation overlay.
/// `longhand_table` must be null for an anonymous layout style or point to a live, frozen
/// `ComputedLonghandTable`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_publish_computed_groups(
    host: *const DocumentHost,
    node: u32,
    pseudo_kind: u8,
    payloads: *const *const c_void,
    count: usize,
    inherited_group_count: usize,
    custom_property_environment: u64,
    inherited_group_swap_candidate: bool,
    counter_style_environment_identity: u64,
    animation_overlay_identity: u64,
    animated_overlay: *const c_void,
    animation_overlay_payloads: *const *const c_void,
    animation_overlay_payload_count: usize,
    longhand_table: *const c_void,
    custom_property_store: *const c_void,
) -> FfiStyleRecordDelta {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(host, |engine| unsafe {
        publish_computed_groups(
            engine,
            node,
            pseudo_kind,
            payloads,
            count,
            inherited_group_count,
            custom_property_environment,
            inherited_group_swap_candidate,
            counter_style_environment_identity,
            animation_overlay_identity,
            animated_overlay,
            animation_overlay_payloads,
            animation_overlay_payload_count,
            longhand_table,
            custom_property_store,
        )
    })
}

/// [`style_engine_publish_computed_groups`] on `engine`, which the style replay tool calls as well.
///
/// # Safety
/// As for [`style_engine_publish_computed_groups`], for the arguments after `engine`.
#[allow(clippy::too_many_arguments)]
pub unsafe fn publish_computed_groups(
    engine: &mut StyleEngine,
    node: u32,
    pseudo_kind: u8,
    payloads: *const *const c_void,
    count: usize,
    inherited_group_count: usize,
    custom_property_environment: u64,
    inherited_group_swap_candidate: bool,
    counter_style_environment_identity: u64,
    animation_overlay_identity: u64,
    animated_overlay: *const c_void,
    animation_overlay_payloads: *const *const c_void,
    animation_overlay_payload_count: usize,
    longhand_table: *const c_void,
    custom_property_store: *const c_void,
) -> FfiStyleRecordDelta {
    if count != 0 && payloads.is_null() {
        return FfiStyleRecordDelta::default();
    }
    let payloads = match count {
        0 => &[],
        _ => SharedPayload::from_pointer_slice(unsafe { std::slice::from_raw_parts(payloads, count) }),
    };
    if animation_overlay_payload_count != 0 && animation_overlay_payloads.is_null() {
        return FfiStyleRecordDelta::default();
    }
    let animation_overlay_payloads = match animation_overlay_payload_count {
        0 => &[],
        _ => SharedPayload::from_pointer_slice(unsafe {
            std::slice::from_raw_parts(animation_overlay_payloads, animation_overlay_payload_count)
        }),
    };
    let longhand_table = unsafe {
        longhand_table
            .cast::<crate::css::computed_longhand_table::ComputedLonghandTable>()
            .as_ref()
    };
    publish_computed_groups_from_inputs(
        engine,
        node,
        pseudo_kind,
        payloads,
        inherited_group_count,
        custom_property_environment,
        inherited_group_swap_candidate,
        counter_style_environment_identity,
        animation_overlay_identity,
        animated_overlay,
        animation_overlay_payloads,
        longhand_table,
        custom_property_store,
    )
}

// Shared by host publication and native layout-style derivation. Recording stays at the
// engine input boundary even when the producer and the style store both live in Rust.
#[allow(clippy::too_many_arguments)]
pub(crate) fn publish_computed_groups_from_inputs(
    engine: &mut StyleEngine,
    node: u32,
    pseudo_kind: u8,
    payloads: &[SharedPayload],
    inherited_group_count: usize,
    custom_property_environment: u64,
    inherited_group_swap_candidate: bool,
    counter_style_environment_identity: u64,
    animation_overlay_identity: u64,
    animated_overlay: *const c_void,
    animation_overlay_payloads: &[SharedPayload],
    longhand_table: Option<&crate::css::computed_longhand_table::ComputedLonghandTable>,
    custom_property_store: *const c_void,
) -> FfiStyleRecordDelta {
    let raw_cascaded_font_size = longhand_table.map_or(std::ptr::null(), |table| table.raw_cascaded_font_size());
    assert!(longhand_table.is_some() || (node == 0 && pseudo_kind == u8::MAX));
    let pseudo_element_styles = longhand_table.map_or(0, |table| table.pseudo_element_styles());
    let inherited_group_swap_eligible = longhand_table.is_some_and(|table| {
        inherited_group_swap_candidate && table.property_inheritance_is_standard() && !table.display_is_list_item()
    });
    let holds_image_values =
        crate::css::computed_values::style_group_payloads_hold_image_values(HostShared::as_pointer_slice(payloads))
            || crate::css::computed_values::style_group_payloads_hold_image_values(HostShared::as_pointer_slice(
                animation_overlay_payloads,
            ));
    let dependency_flags = longhand_table.map_or(0, |table| table.publication_dependency_flags())
        | (u8::from(inherited_group_swap_eligible) * super::computed::INHERITED_GROUP_SWAP_ELIGIBLE)
        | (u8::from(holds_image_values) * super::computed::HOLDS_IMAGE_VALUES);
    if animation_overlay_identity != 0 && animated_overlay.is_null() {
        return FfiStyleRecordDelta::default();
    }
    let metadata_input = super::computed::ComputedMetadataInput {
        pseudo_element_styles,
        dependency_flags,
        counter_style_environment_identity,
        animation_overlay_identity,
        animated_overlay: HostShared::new(animated_overlay).cast(),
        animation_overlay_payloads,
        longhand_table: HostShared::new(longhand_table.map_or(std::ptr::null(), std::ptr::from_ref)),
    };
    let publication = if let Some(node) = StyleNodeID::from_raw(node) {
        let target = super::computed::ComputedStyleTarget::new(node, pseudo_kind);
        engine.forget_engine_computed_record(target);
        engine.publish_computed_groups(
            target,
            payloads,
            inherited_group_count,
            custom_property_environment,
            metadata_input,
        )
    } else {
        engine.intern_computed_groups(
            if animation_overlay_identity != 0 {
                animation_overlay_payloads
            } else {
                payloads
            },
            inherited_group_count,
            custom_property_environment,
            metadata_input,
        )
    };
    // The store is what the environment resolves to; an engine-computed environment builds on it,
    // and an element alike in its custom declarations takes the environment itself.
    unsafe {
        engine
            .custom_property_environments
            .retain(custom_property_environment, custom_property_store);
    }
    if pseudo_kind == u8::MAX
        && let Some(node) = StyleNodeID::from_raw(node)
    {
        engine.remember_cpp_custom_property_environment(node, custom_property_environment);
    }
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
        payload.write_u64(u64::from(!animated_overlay.is_null()));
        payload.write_length(animation_overlay_payloads.len());
        for &pointer in animation_overlay_payloads {
            payload.write_u64(pointer_token(pointer.as_ptr()));
        }
        payload.write_bytes(longhand_table.map_or(&[], |table| table.importance_bits()));
        payload.write_bytes(longhand_table.map_or(&[], |table| table.inheritance_bits()));
        payload.write_length(longhand_table.map_or(0, |table| table.inheritance_dependent_values().count()));
        for (property, value) in longhand_table
            .into_iter()
            .flat_map(crate::css::computed_longhand_table::ComputedLonghandTable::inheritance_dependent_values)
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
                    payload.write_u64(pointer_token(value.as_ptr()));
                    payload.write_u8(crate::css::style_value::style_value_dependency_flags(
                        value.cast().as_ptr(),
                    ));
                }
            }
        }
        payload.write_u64(result.old_style_record);
        payload.write_u64(result.new_style_record);
    });
    result
}

/// Replaces only the animation overlay on an already-published target. Recording falls back to
/// `style_engine_publish_computed_groups`, which captures the complete base-style input.
///
/// # Safety
/// `host` must be a live document host, on its document's thread. `animated_overlay` must be null
/// when `animation_overlay_identity` is zero and otherwise point at a live animation overlay.
/// `payloads` must contain live group payloads for a non-empty overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_publish_animation_overlay(
    host: *const DocumentHost,
    node: u32,
    pseudo_kind: u8,
    animation_overlay_identity: u64,
    animated_overlay: *const c_void,
    payloads: *const *const c_void,
    payload_count: usize,
) -> FfiStyleRecordDelta {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return FfiStyleRecordDelta::default();
        };
        if animation_overlay_identity != 0 && animated_overlay.is_null() {
            return FfiStyleRecordDelta::default();
        }
        if payload_count != 0 && payloads.is_null() {
            return FfiStyleRecordDelta::default();
        }
        let payloads = match payload_count {
            0 => &[],
            _ => SharedPayload::from_pointer_slice(unsafe { std::slice::from_raw_parts(payloads, payload_count) }),
        };
        let Some(publication) = engine.publish_animation_overlay_impl(
            super::computed::ComputedStyleTarget::new(node, pseudo_kind),
            animation_overlay_identity,
            HostShared::new(animated_overlay).cast(),
            payloads,
        ) else {
            return FfiStyleRecordDelta::default();
        };
        FfiStyleRecordDelta {
            old_style_record: publication.previous_style_record.raw(),
            new_style_record: publication.style_record.raw(),
        }
    })
}

/// Returns the StyleEngine-owned group payload array for a base or live animation-overlay record.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and its document's style engine
/// must stay as it is for every read through the returned pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_style_record_payloads(
    host: *const DocumentHost,
    style_record: u64,
) -> *const c_void {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(host, |engine| unsafe { style_record_payloads(engine, style_record) })
}

/// [`style_engine_style_record_payloads`] on `engine`, which the style replay tool calls as well.
///
/// # Safety
/// As for [`style_engine_style_record_payloads`], for the arguments after `engine`.
pub unsafe fn style_record_payloads(engine: &mut StyleEngine, style_record: u64) -> *const c_void {
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
                        .recording_pointer_token(pointer.addr())
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
}

/// What the winners a node's records were computed from read beyond their cascade, one bit per
/// kind, as a row's `record_reads` carries them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiNodeRecordReads {
    /// An `attr()` substitution, in the element's winners, its pseudo-elements' or the custom
    /// properties it declares: an attribute change reaches the records.
    Attributes = 1 << 0,
    /// A tree-counting function, in the element's winners or its pseudo-elements', written or
    /// substituted: a change among the element's siblings reaches the records.
    SiblingPosition = 1 << 1,
    /// An `inherit()` substitution, in the element's winners, its pseudo-elements' or the custom
    /// properties either declares: a moved environment of the parent reaches the records.
    InheritFunction = 1 << 2,
    /// An `if()` substitution, in the element's winners, its pseudo-elements' or the custom
    /// properties either declares: a change of the media features or of the element's
    /// environment reaches the records.
    IfFunction = 1 << 3,
    /// A custom function call, in the element's winners, its pseudo-elements' or the custom
    /// properties either declares: a change of the media features, of the element's environment
    /// or of the `@function` definitions reaches the records.
    CustomFunction = 1 << 4,
}

/// Computes the property-dependent damage between two final style records of no element in
/// particular: their font cascades count as equal, and no SVG container or viewport reads them.
/// What moving an element's record damages is `style_engine_element_record_damage`'s.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and both style records must
/// remain pinned or assigned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_compare_style_records(
    host: *const DocumentHost,
    old_style_record: u64,
    new_style_record: u64,
) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        engine.compare_style_records(old_style_record, new_style_record, true, false, false)
    })
}

/// Computes what moving an element from one final style record to another damages, from the
/// records and the facts the engine holds of the element. The counter styles its box was built
/// with are the host's to compare.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and both style records must
/// remain pinned or assigned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_element_record_damage(
    host: *const DocumentHost,
    node: u32,
    old_style_record: u64,
    new_style_record: u64,
) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return super::style_invalidation::unreadable_record_damage();
        };
        engine.element_record_damage(node, false, old_style_record, new_style_record)
    })
}

/// Computes what moving one of an element's pseudo-elements from one final style record to another
/// damages, where either record can be zero. `counter_styles_changed` is the host's comparison of
/// the counter styles the pseudo-element's box was built with.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and every nonzero style record
/// must remain pinned or assigned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_pseudo_element_record_damage(
    host: *const DocumentHost,
    node: u32,
    pseudo_kind: u8,
    old_style_record: u64,
    new_style_record: u64,
    originating_style_record: u64,
    counter_styles_changed: bool,
) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return super::style_invalidation::unreadable_record_damage();
        };
        engine.pseudo_element_record_damage(
            node,
            pseudo_kind,
            old_style_record,
            new_style_record,
            originating_style_record,
            counter_styles_changed,
        )
    })
}

/// Returns whether a candidate animation overlay changes any effective value in a style record.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, `animated_overlay` must be live
/// for this call, and the style record must remain pinned or assigned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_animation_overlay_changed(
    host: *const DocumentHost,
    old_style_record: u64,
    animated_overlay: *const c_void,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        engine.animation_overlay_changed(old_style_record, animated_overlay.cast())
    })
}

/// What the host hands over to have an element's sampled animation overlay composed into the
/// payloads of its overlay record.
#[repr(C)]
pub struct FfiAnimationOverlayPayloadInput {
    pub style_node: u32,
    pub pseudo_kind: u8,
    /// The record the overlay was sampled on, which it composes over.
    pub style_record: u64,
    /// The longhand table the overlay was sampled over.
    pub longhand_table: *const c_void,
    pub animated_overlay: *const c_void,
    pub used_color_scheme: u8,
    pub display_before_box_type_transformation_raw: u32,
    pub callback_context: *mut c_void,
    /// Writes the animated style's platform font, a `ComputedValuesFFI::FfiFontGroupBuildInputs`;
    /// asked only where the font group is rebuilt.
    pub font_group_inputs: unsafe extern "C" fn(*mut c_void, *mut c_void),
}

/// Which groups of a composed overlay record were rebuilt over its base record.
#[repr(C)]
pub struct FfiAnimationOverlayPayloads {
    /// Whether the engine holds the record to compose over; nothing was written where it does not.
    pub present: bool,
    /// Whether the overlay named a value the groups could not be told from, or nothing at all.
    pub rebuilt_every_group: bool,
    /// The groups whose payloads the caller owns a reference to, given back with
    /// `style_engine_release_animation_overlay_payloads`.
    pub rebuilt_groups: u32,
}

/// Compose an element's sampled animation overlay into the payloads of its overlay record, one per
/// style group, written to `payloads`; see `RetainedState::build_animation_overlay_payloads`.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, `input` and everything it points
/// to must be live for the call, `input`'s table must be non-null, and `payloads` must hold
/// `payload_count` entries, one per style group.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_build_animation_overlay_payloads(
    host: *const DocumentHost,
    input: *const FfiAnimationOverlayPayloadInput,
    payloads: *mut *const c_void,
    payload_count: usize,
) -> FfiAnimationOverlayPayloads {
    use crate::css::table_group_builder::{FfiFontGroupBuildInputs, group_index};

    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    let input = unsafe { &*input };
    assert_eq!(payload_count, group_index::COUNT, "one payload per style group");
    let payloads = unsafe { &mut *payloads.cast::<[*const c_void; group_index::COUNT]>() };
    let table = unsafe {
        &*input
            .longhand_table
            .cast::<crate::css::computed_longhand_table::ComputedLonghandTable>()
    };
    let overlay = unsafe {
        input
            .animated_overlay
            .cast::<crate::css::animated_overlay::AnimatedOverlay>()
            .as_ref()
    };
    let mut build = |font: Option<&FfiFontGroupBuildInputs>| {
        with_engine(host, |engine| {
            engine.build_animation_overlay_payloads(
                StyleNodeID::from_raw(input.style_node)?,
                input.pseudo_kind,
                input.style_record,
                table,
                overlay,
                input.used_color_scheme,
                input.display_before_box_type_transformation_raw,
                font,
                payloads,
            )
        })
    };
    // Resolving the animated font may read the engine, so a build that rebuilds the font group has the
    // host resolve it between two calls of the engine.
    let rebuilt = match build(None) {
        Some(Err(super::engine_sample::NeedsHostFont)) => {
            let mut font = std::mem::MaybeUninit::<FfiFontGroupBuildInputs>::uninit();
            // SAFETY: Guaranteed by the caller. The host writes the whole font.
            let font = unsafe {
                (input.font_group_inputs)(input.callback_context, font.as_mut_ptr().cast());
                font.assume_init()
            };
            build(Some(&font))
        }
        rebuilt => rebuilt,
    };
    match rebuilt {
        Some(Ok(rebuilt)) => FfiAnimationOverlayPayloads {
            present: true,
            rebuilt_every_group: rebuilt.every_group,
            rebuilt_groups: rebuilt.groups,
        },
        _ => FfiAnimationOverlayPayloads {
            present: false,
            rebuilt_every_group: false,
            rebuilt_groups: 0,
        },
    }
}

/// Give back the references to the payloads `style_engine_build_animation_overlay_payloads`
/// rebuilt.
///
/// # Safety
/// `payloads` must hold `payload_count` entries as that build wrote them, and `rebuilt_groups` be
/// the groups it rebuilt, not given back before.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_release_animation_overlay_payloads(
    payloads: *const *const c_void,
    payload_count: usize,
    rebuilt_groups: u32,
) {
    let payloads = unsafe { std::slice::from_raw_parts(payloads, payload_count) };
    super::engine_sample::release_rebuilt_overlay_payloads(payloads, rebuilt_groups);
}

/// Computes property-dependent damage for the sparse changed values in an animation overlay.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, `animated_overlay` and every
/// group payload must be live for this call, and the style record must remain pinned or assigned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_compare_animation_overlay(
    host: *const DocumentHost,
    old_style_record: u64,
    animated_overlay: *const c_void,
    payloads: *const *const c_void,
    payload_count: usize,
    is_document_element: bool,
) -> FfiAnimationInvalidation {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        let payloads =
            SharedPayload::from_pointer_slice(unsafe { std::slice::from_raw_parts(payloads, payload_count) });
        engine.compare_animation_overlay(old_style_record, animated_overlay.cast(), payloads, is_document_element)
    })
}

/// Returns a synchronous borrowed view of a base or live animation-overlay record.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and its document's style engine
/// must stay as it is for every read through the returned pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_style_record_view(
    host: *const DocumentHost,
    style_record: u64,
) -> FfiStyleRecordView {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(host, |engine| unsafe { style_record_view(engine, style_record) })
}

/// [`style_engine_style_record_view`] on `engine`, which the style replay tool calls as well.
///
/// # Safety
/// As for [`style_engine_style_record_view`], for the arguments after `engine`.
pub unsafe fn style_record_view(engine: &mut StyleEngine, style_record: u64) -> FfiStyleRecordView {
    let view = engine.style_record_view(style_record);
    let result = match &view {
        None => FfiStyleRecordView::missing(),
        Some(view) => FfiStyleRecordView {
            payloads: SharedPayload::as_pointer_slice(view.payloads).as_ptr(),
            base_payloads: SharedPayload::as_pointer_slice(view.base_payloads).as_ptr(),
            longhand_table: view.longhand_table.cast().as_ptr(),
            animated_overlay: view.animated_overlay.cast().as_ptr(),
            payload_count: view.payloads.len(),
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
                        .recording_pointer_token(pointer.addr())
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
        let longhand_table = unsafe { view.longhand_table.as_ref() };
        payload.write_bytes(longhand_table.map_or(&[], |table| table.importance_bits()));
        payload.write_bytes(longhand_table.map_or(&[], |table| table.inheritance_bits()));
        payload.write_length(longhand_table.map_or(0, |table| table.inheritance_dependent_values().count()));
        for (property, value) in longhand_table
            .into_iter()
            .flat_map(crate::css::computed_longhand_table::ComputedLonghandTable::inheritance_dependent_values)
        {
            payload.write_u16(property);
            payload.write_u64(
                engine
                    .recording_pointer_token(value as usize)
                    .expect("an enabled recorder must tokenize the pointer"),
            );
        }
        let raw_cascaded_font_size = longhand_table.map_or(std::ptr::null(), |table| table.raw_cascaded_font_size());
        payload.write_u64(match raw_cascaded_font_size.is_null() {
            true => 0,
            false => engine
                .recording_pointer_token(raw_cascaded_font_size as usize)
                .expect("an enabled recorder must tokenize the pointer"),
        });
        payload.write_u64(u64::from(!view.animated_overlay.is_null()));
        payload.write_u64(view.pseudo_element_styles);
        payload.write_u64(view.counter_style_environment_identity);
        payload.write_u64(view.animation_overlay_identity);
        payload.write_u8(view.dependency_flags);
        payload.write_length(view.longhand_values.len());
        for &value in view.longhand_values {
            payload.write_u64(match value.is_null() {
                true => 0,
                false => engine
                    .recording_pointer_token(value.addr())
                    .expect("an enabled recorder must tokenize the pointer"),
            });
        }
    });
    result
}

/// Removes the retained computed-input assignment for one pseudo-element kind.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_remove_computed_pseudo(
    host: *const DocumentHost,
    node: u32,
    pseudo_kind: u8,
) -> FfiStyleRecordDelta {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(host, |engine| unsafe {
        remove_computed_pseudo(engine, node, pseudo_kind)
    })
}

/// [`style_engine_remove_computed_pseudo`] on `engine`, which the style replay tool calls as well.
///
/// # Safety
/// As for [`style_engine_remove_computed_pseudo`], for the arguments after `engine`.
pub unsafe fn remove_computed_pseudo(engine: &mut StyleEngine, node: u32, pseudo_kind: u8) -> FfiStyleRecordDelta {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return FfiStyleRecordDelta::default();
    };
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
}
pub(crate) fn publish_rule_declarations(
    engine: &mut StyleEngine,
    rule: u32,
    data: &crate::css::declaration_block::DeclarationBlockData,
) -> bool {
    if rule == 0 {
        return false;
    }
    let mut has_transitions = false;
    let declared = data
        .properties
        .iter()
        .map(|declaration| {
            use crate::css::property_metadata::property_defines_a_css_transition;
            has_transitions |= property_defines_a_css_transition(declaration.property_id);
            engine.intern_declared_property(declaration)
        })
        .collect::<Vec<_>>();
    let written_values: Vec<_> = data
        .properties
        .iter()
        .map(|declaration| unsafe {
            RetainedStyleValueData::from_retained_pointer(std::sync::Arc::into_raw(declaration.value.clone()))
        })
        .collect();
    let (custom_declarations, custom_written_values) =
        collect_native_custom_declarations(engine, &data.custom_properties)
            .into_iter()
            .unzip::<_, _, Vec<_>, Vec<_>>();
    engine.set_rule_declared_properties_with_written_values(
        RuleID(rule - 1),
        &declared,
        written_values,
        custom_declarations.clone(),
        custom_written_values,
    );
    engine.record_boundary_call(EventKind::SetRuleDeclaredProperties, |payload| {
        payload.write_u32(rule);
        write_declared_properties(&declared, payload);
        write_custom_declarations(&custom_declarations, payload);
    });
    has_transitions
}

/// Moves a node's record to the custom-property environment C++ refreshed it to, keeping the
/// store behind the environment, and returns the new record's identity; zero when the node holds
/// no base record to move.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `store` must be null or a
/// live raw `Arc` pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_republish_record_environment(
    host: *const DocumentHost,
    node: u32,
    environment: u64,
    store: *const c_void,
) -> u64 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return 0;
        };
        unsafe { engine.custom_property_environments.retain(environment, store) };
        let result = engine.republish_record_environment(node, environment).unwrap_or(0);
        engine.record_boundary_call(EventKind::RepublishRecordEnvironment, |payload| {
            payload.write_u32(node.raw());
            payload.write_u64(environment);
            payload.write_u64(result);
        });
        result
    })
}

/// An environment the host holds, as a custom-property environment move names it: its identity and
/// the store behind it, both zero for the empty environment.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiNamedEnvironment {
    pub identity: u64,
    pub store: *const c_void,
}

/// A move of one element's custom-property environment, as the host made it: what its style
/// resolved to before and resolves to now, and what it handed its children before and hands them
/// now, with the host's object for the latter and whether that declares custom properties of its
/// own.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiEnvironmentMove {
    pub old_base: FfiNamedEnvironment,
    pub new_base: FfiNamedEnvironment,
    pub old_inheritable: u64,
    pub new_inheritable: FfiNamedEnvironment,
    pub new_inheritable_data: *const c_void,
    pub new_inheritable_declares: bool,
}

/// What the host does for an element a custom-property environment move reached.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiEnvironmentMoveActionKind {
    /// Install `style_record`, the element's record over the moved environment, and move the
    /// environments of its element-backed pseudo-elements that held `replaced`.
    Republish,
    /// Build the custom properties the element declares again over the moved environment.
    Rebuild,
    /// Compute the element again.
    Recompute,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiEnvironmentMoveAction {
    pub kind: FfiEnvironmentMoveActionKind,
    pub node: u32,
    pub style_record: u64,
    pub replaced: u64,
}

impl From<super::environment_move::EnvironmentMoveAction> for FfiEnvironmentMoveAction {
    fn from(action: super::environment_move::EnvironmentMoveAction) -> Self {
        use super::environment_move::EnvironmentMoveAction;
        let (kind, node, style_record, replaced) = match action {
            EnvironmentMoveAction::Republish {
                node,
                style_record,
                replaced,
            } => (FfiEnvironmentMoveActionKind::Republish, node, style_record, replaced),
            EnvironmentMoveAction::Rebuild(node) => (FfiEnvironmentMoveActionKind::Rebuild, node, 0, 0),
            EnvironmentMoveAction::Recompute(node) => (FfiEnvironmentMoveActionKind::Recompute, node, 0, 0),
        };
        Self {
            kind,
            node: node.raw(),
            style_record,
            replaced,
        }
    }
}

#[repr(C)]
pub struct FfiEnvironmentMoveActions {
    pub actions: *const FfiEnvironmentMoveAction,
    pub count: usize,
}

/// Moves the custom-property environments below `origin`, whose own moved as `moved` says, and
/// answers what the host does for the elements the move reached, in flat tree preorder. The answer
/// stays valid until the engine is next called.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, the stores `moved` names must be
/// null or live raw `Arc` pointers, and `moved.new_inheritable_data` must be null or a live
/// `Web::CSS::CustomPropertyData`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_move_custom_property_environment(
    host: *const DocumentHost,
    origin: u32,
    moved: FfiEnvironmentMove,
) -> FfiEnvironmentMoveActions {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        let mut actions = std::mem::take(&mut engine.host.environment_move_actions);
        actions.clear();
        if let Some(origin) = StyleNodeID::from_raw(origin) {
            let named = |environment: FfiNamedEnvironment| super::environment_move::NamedEnvironment {
                identity: environment.identity,
                store: environment.store,
            };
            let moved = super::environment_move::EnvironmentMove {
                old_base: named(moved.old_base),
                new_base: named(moved.new_base),
                old_inheritable: moved.old_inheritable,
                new_inheritable: named(moved.new_inheritable),
                new_inheritable_data: moved.new_inheritable_data,
                new_inheritable_declares: moved.new_inheritable_declares,
            };
            unsafe { engine.move_custom_property_environment(origin, &moved, |action| actions.push(action.into())) };
        }
        let answer = FfiEnvironmentMoveActions {
            actions: actions.as_ptr(),
            count: actions.len(),
        };
        engine.host.environment_move_actions = actions;
        answer
    })
}

/// Replays a record moved to a refreshed environment.
///
/// # Safety
/// `engine` must be live.
pub unsafe fn replay_republish_record_environment(
    engine: crate::css::style::StyleEngineHandle,
    node: u32,
    environment: u64,
) -> u64 {
    let engine = unsafe { engine.get_mut() };
    StyleNodeID::from_raw(node)
        .and_then(|node| engine.republish_record_environment(node, environment))
        .unwrap_or(0)
}

/// Retry after the ancestor's style was installed, returning installation metadata together
/// with the record instead of requiring a later query of the node.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_retry_engine_record_after_ancestor(
    host: *const DocumentHost,
    node: u32,
) -> FfiEngineComputedRecord {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(host, |engine| unsafe {
        retry_engine_record_after_ancestor(engine, node)
    })
}

/// [`style_engine_retry_engine_record_after_ancestor`] on `engine`, which the style replay tool
/// calls as well.
///
/// # Safety
/// As for [`style_engine_retry_engine_record_after_ancestor`], for the arguments after `engine`.
pub unsafe fn retry_engine_record_after_ancestor(engine: &mut StyleEngine, node: u32) -> FfiEngineComputedRecord {
    abort_on_panic(|| {
        let Some(style_node) = StyleNodeID::from_raw(node) else {
            return FfiEngineComputedRecord::default();
        };
        let retried = engine.retry_engine_record_after_ancestor(style_node);
        let result = FfiEngineComputedRecord {
            style_record: retried.style_record,
            uses_substitution: retried.style_record != 0 && engine.nodes_with_substituted_records.contains(&style_node),
            record_reads: if retried.style_record != 0 {
                engine.node_record_reads(style_node)
            } else {
                0
            },
            explicitly_inherited_groups: retried.explicitly_inherited_groups,
            owes_an_animation_plan: retried.owes_an_animation_plan,
            owes_a_transition_step: retried.owes_a_transition_step,
            composed_by_the_host: retried.composed_by_the_host,
            pseudo_records_present: retried.pseudo_records_present,
            pseudo_records: retried.pseudo_records,
        };
        engine.record_boundary_call(EventKind::RetryEngineRecordAfterAncestor, |payload| {
            payload.write_u32(node);
            payload.write_u64(result.style_record);
            payload.write_bool(result.uses_substitution);
        });
        result
    })
}

/// What a read of one element's style the host makes before the next style update asks of the
/// engine; see `StyleEngineState::answer_record_demand`. A read-only demand leaves the engine as it
/// was: its record is only for the host to read. Any other is installed and acknowledged as a
/// style update's would be.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiRecordDemand {
    /// A targeted style update of the element: its record, driven in full against the parent as it
    /// is now.
    TargetedElement,
    /// A read-only read of the element's record.
    ElementRead,
    /// A read-only read of what the element computes to as though it had no inline declaration,
    /// driven in full against the parent as it is now.
    ElementReadWithoutInlineStyle,
}

/// What a read of one of an element's pseudo-elements the host makes before the next style update
/// asks of the engine, as `FfiRecordDemand` does of an element.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiPseudoElementRecordDemand {
    /// A CSSOM read of the pseudo-element, settled against its element's installed record.
    CssomRead,
    /// A read-only read of what the pseudo-element computes to, whether or not it generates a box.
    ReadOnly,
}

/// A pseudo-element a record demand may read: a synthetic one, which the engine settles beside its
/// element, or, for a read-only read, one an element in the shadow tree backs or a named view
/// transition one, which it computes from the element's rules for it. Each is numbered as its kind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiDemandedPseudoElement {
    After = 0,
    Backdrop = 1,
    Before = 2,
    FirstLetter = 3,
    FirstLine = 4,
    Marker = 5,
    Selection = 6,
    ViewTransition = 7,
    DetailsContent = 8,
    FileSelectorButton = 9,
    Placeholder = 10,
    SliderFill = 11,
    SliderThumb = 12,
    SliderTrack = 13,
    ViewTransitionGroup = 16,
    ViewTransitionImagePair = 17,
    ViewTransitionNew = 18,
    ViewTransitionOld = 19,
}

/// The answer to a record demand: the record, or that the pseudo-element read generates no box
/// with the custom-property environment its rules resolve to (zero where none styles it). A zero
/// `style_record` that is not absent leaves the read to C++.
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiRecordDemandAnswer {
    pub record: FfiEngineComputedRecord,
    pub is_absent: bool,
    pub custom_property_environment: u64,
}

/// Answer a read of one element's style, or one of its pseudo-elements', the host makes before
/// the next style update.
pub(crate) fn answer_record_demand(
    engine: &mut StyleEngine,
    style_node: StyleNodeID,
    demand: super::publication::RecordDemand,
) -> FfiRecordDemandAnswer {
    let node = style_node.raw();
    let read_only = demand.is_read_only();
    let result = match engine.answer_record_demand(style_node, demand) {
        Ok(super::publication::RecordDemandAnswer::Record {
            record,
            uses_substitution,
        }) => FfiRecordDemandAnswer {
            record: FfiEngineComputedRecord {
                style_record: record.style_record,
                uses_substitution,
                // A read-only answer is the host's to read, never to install.
                record_reads: if record.style_record != 0 && !read_only {
                    engine.node_record_reads(style_node)
                } else {
                    0
                },
                explicitly_inherited_groups: record.explicitly_inherited_groups,
                owes_an_animation_plan: record.owes_an_animation_plan,
                owes_a_transition_step: record.owes_a_transition_step,
                composed_by_the_host: record.composed_by_the_host,
                pseudo_records_present: record.pseudo_records_present,
                pseudo_records: record.pseudo_records,
            },
            is_absent: false,
            custom_property_environment: 0,
        },
        Ok(super::publication::RecordDemandAnswer::Absent {
            custom_property_environment,
        }) => FfiRecordDemandAnswer {
            record: FfiEngineComputedRecord::default(),
            is_absent: true,
            custom_property_environment,
        },
        Err(_) => FfiRecordDemandAnswer::default(),
    };
    engine.record_boundary_call(EventKind::AnswerRecordDemand, |payload| {
        payload.write_u32(node);
        // The demand's shape: an element's numbered as its kind, then a pseudo-element's after
        // them, followed by the pseudo-element read.
        match demand {
            super::publication::RecordDemand::Element(demand) => payload.write_u8(demand as u8),
            super::publication::RecordDemand::PseudoElement(demand, pseudo_element) => {
                payload.write_u8(super::publication::RecordDemand::FIRST_PSEUDO_ELEMENT_SHAPE + demand as u8);
                payload.write_u8(pseudo_element as u8);
            }
        }
        payload.write_u64(result.record.style_record);
        payload.write_bool(result.is_absent);
        payload.write_bool(result.record.uses_substitution);
        payload.write_u8(result.record.pseudo_records_present);
    });
    result
}

/// [`answer_record_demand`] of an element's style for a replay, which holds its engine itself.
///
/// # Safety
/// `engine` must be live.
pub unsafe fn style_engine_answer_record_demand_for_replay(
    engine: crate::css::style::StyleEngineHandle,
    node: u32,
    demand: FfiRecordDemand,
) -> FfiRecordDemandAnswer {
    let engine = unsafe { engine.get_mut() };
    StyleNodeID::from_raw(node).map_or_else(Default::default, |node| {
        answer_record_demand(engine, node, super::publication::RecordDemand::Element(demand))
    })
}

/// [`answer_record_demand`] of one of an element's pseudo-elements for a replay, which holds its
/// engine itself.
///
/// # Safety
/// `engine` must be live.
pub unsafe fn style_engine_answer_pseudo_element_record_demand_for_replay(
    engine: crate::css::style::StyleEngineHandle,
    node: u32,
    demand: FfiPseudoElementRecordDemand,
    pseudo_element: FfiDemandedPseudoElement,
) -> FfiRecordDemandAnswer {
    let engine = unsafe { engine.get_mut() };
    StyleNodeID::from_raw(node).map_or_else(Default::default, |node| {
        answer_record_demand(
            engine,
            node,
            super::publication::RecordDemand::PseudoElement(demand, pseudo_element),
        )
    })
}

/// The record of an element no rule reaches, computed from its presentational hints and its
/// inline style alone over the initial values, its custom properties among them; see
/// `declared_only_record`. `subject` is the
/// document's style node. Returns a pinned record the host unpins, or zero where the engine
/// leaves the computation to C++.
///
/// # Safety
/// `host` must be a live document host, on its document's thread. `hints` must borrow `hint_count`
/// `FfiDeclaredProperty` entries whose values point at live, Arc-backed `StyleValueData` roots. A
/// non-null `inline_block` must borrow a live `DeclarationBlock`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_declared_only_record(
    host: *const DocumentHost,
    subject: u32,
    facts: u32,
    hint_kind: FfiElementDeclarationKind,
    hints: *const c_void,
    hint_count: usize,
    inline_block: *const c_void,
) -> u64 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        use crate::css::declaration_block::{DeclarationBlock, FfiDeclaredProperty, declaration_from_view};
        abort_on_panic(|| {
            // A record no row holds is no event a replay could reproduce.
            let Some(subject) = StyleNodeID::from_raw(subject).filter(|_| engine.recording_id().is_none()) else {
                return 0;
            };
            let hints = if hint_count == 0 {
                &[]
            } else {
                unsafe { std::slice::from_raw_parts(hints.cast::<FfiDeclaredProperty>(), hint_count) }
            };
            let hints = hints
                .iter()
                .map(|hint| unsafe { declaration_from_view(hint) })
                .collect::<Vec<_>>();
            let inline_data = unsafe { inline_block.cast::<DeclarationBlock>().as_ref() }.map(DeclarationBlock::data);
            let hint_kind = decode_element_declaration_kind(hint_kind);
            let declarations = hints
                .iter()
                .map(|hint| (hint_kind, hint))
                .chain(inline_data.iter().flat_map(|data| {
                    data.properties
                        .iter()
                        .map(|declaration| (ElementDeclarationKind::InlineStyle, declaration))
                }))
                .collect::<Vec<_>>();
            let custom_declarations = collect_native_custom_declarations(
                engine,
                inline_data
                    .as_ref()
                    .map_or(&[], |data| data.custom_properties.as_slice()),
            );
            engine
                .declared_only_record(subject, facts, &declarations, &custom_declarations)
                .map_or(0, super::computed::FinalStyleRecordID::raw)
        })
    })
}

/// Settle the synthetic pseudo-element records of an element whose record the host just installed.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_settle_pseudo_records_after_host_record(
    host: *const DocumentHost,
    node: u32,
    old_is_list_item: bool,
) -> FfiSettledPseudoRecords {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(host, |engine| unsafe {
        settle_pseudo_records_after_host_record(engine, node, old_is_list_item)
    })
}

/// [`style_engine_settle_pseudo_records_after_host_record`] on `engine`, which the style replay
/// tool calls as well.
///
/// # Safety
/// As for [`style_engine_settle_pseudo_records_after_host_record`], for the arguments after `engine`.
pub unsafe fn settle_pseudo_records_after_host_record(
    engine: &mut StyleEngine,
    node: u32,
    old_is_list_item: bool,
) -> FfiSettledPseudoRecords {
    abort_on_panic(|| {
        let Some(style_node) = StyleNodeID::from_raw(node) else {
            return FfiSettledPseudoRecords::default();
        };
        let mut result = engine.settle_pseudo_records_after_host_record(style_node, old_is_list_item);
        if !result.refused {
            result.record_reads = engine.node_record_reads(style_node);
        }
        engine.record_boundary_call(EventKind::SettlePseudoRecordsAfterHostRecord, |payload| {
            payload.write_u32(node);
            payload.write_bool(old_is_list_item);
            payload.write_bool(result.refused);
            payload.write_u8(result.pseudo_records_present);
        });
        result
    })
}

/// The store of an environment the engine resolved, with one strong reference transferred to the
/// caller, and the environment it was resolved over; null for an environment C++ published.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `parent` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_borrow_engine_custom_property_environment(
    host: *const DocumentHost,
    identity: u64,
    parent: *mut u64,
) -> *const c_void {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        let Some((store, parent_identity)) = engine.custom_property_environments.engine_environment(identity) else {
            return std::ptr::null();
        };
        unsafe {
            std::sync::Arc::increment_strong_count(store.cast::<crate::css::custom_properties::CustomPropertyStore>());
            *parent = parent_identity;
        }
        store
    })
}

/// Records what a custom property's name atom spells, and the fly string it is. The fly string is
/// retained and never recorded: a replay has no strings, and names its entries by atom alone.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, `raw` must be a live
/// `AK::Utf16FlyString` raw representation, and `text` must name `length` code units.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_note_custom_property_name(
    host: *const DocumentHost,
    name: u32,
    raw: usize,
    text: *const u16,
    length: usize,
) {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        if name == 0 || (length != 0 && text.is_null()) {
            return;
        }
        let text = match length {
            0 => &[],
            _ => unsafe { std::slice::from_raw_parts(text, length) },
        };
        unsafe { note_native_custom_property_name(engine, StyleAtomID(name), raw, text) };
    });
}

// The raw identity must remain a live Utf16FlyString for the native engine to retain it.
unsafe fn note_native_custom_property_name(engine: &mut StyleEngine, name: StyleAtomID, raw: usize, text: &[u16]) {
    unsafe { engine.note_custom_property_name(name, raw, text) };
    engine.record_boundary_call(EventKind::NoteCustomPropertyName, |payload| {
        payload.write_u32(name.0);
        payload.write_u16_slice(text);
    });
}

/// Replays a recorded custom-property name without a fly string behind it.
///
/// # Safety
/// `engine` must be live.
pub unsafe fn replay_note_custom_property_name(engine: crate::css::style::StyleEngineHandle, name: u32, text: &[u16]) {
    let engine = unsafe { engine.get_mut() };
    unsafe { engine.note_custom_property_name(StyleAtomID(name), 0, text) };
}

#[repr(C)]
pub struct FfiNativeRuleTarget {
    pub identity: u64,
    pub declaration_version: u32,
    pub source_identity: u64,
    pub declarations: *const c_void,
    pub layer_name: *const u16,
    pub layer_name_length: usize,
    pub has_container_conditions: bool,
    pub origin: FfiCascadeOrigin,
}

/// Issue a new identity for changed declaration contents, independently of CSSOM wrappers.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_next_declaration_block_version(host: *const DocumentHost) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| engine.next_declaration_block_version())
}

/// Publish a native declaration edit through its owning rule and return whether it declares
/// transitions. The host is notified before publishing, without an engine or graph borrow.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `rule` live. The callback
/// must not mutate the native rule graph, and must remain valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_native_rule_declarations_changed(
    host: *const DocumentHost,
    rule: *const c_void,
    context: *mut c_void,
    notify: unsafe extern "C" fn(*mut c_void, u32),
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { sheet_writing_host(host) };
    // SAFETY: Guaranteed by the caller.
    let rule = unsafe { &*rule.cast::<crate::css::rule::NativeRule>() };
    let Some(id) = with_engine(host, |engine| native_rule_declaration_owner(engine, rule)) else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { notify(context, id.0 + 1) };
    with_engine(host, |engine| publish_native_rule_declarations(engine, rule, id))
}

/// The engine's id of the rule that owns `rule`'s declarations, if it has one.
pub(crate) fn native_rule_declaration_owner(
    engine: &StyleEngine,
    rule: &crate::css::rule::NativeRule,
) -> Option<RuleID> {
    engine.native_rule_id(rule.declaration_owner_identity()?)
}

/// Publishes the declarations of `rule`, whose owner is the engine's rule `id`, and answers
/// whether they declare transitions.
pub(crate) fn publish_native_rule_declarations(
    engine: &mut StyleEngine,
    rule: &crate::css::rule::NativeRule,
    id: RuleID,
) -> bool {
    let declarations = rule.cascade_declarations();
    engine.native_rules.targets.get_mut(&id).unwrap().declarations = declarations.clone();
    let version = engine.next_declaration_block_version();
    operations::record_rule_declarations_changed(engine, id.0 + 1, version);
    declarations.is_some_and(|declarations| publish_rule_declarations(engine, id.0 + 1, &declarations))
}

/// Find the next compiled rule after an inserted native subtree, without creating CSSOM objects.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `sheet` a live native
/// allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_native_rule_successor(
    host: *const DocumentHost,
    sheet: *const c_void,
    identity: u64,
) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { sheet_writing_host(host) };
    with_engine(host, |engine| {
        let sheet = unsafe { &*sheet.cast::<crate::css::style_sheet::NativeStyleSheet>() };
        crate::css::rule::mutation::successor(sheet, identity, |identity| {
            engine.native_rules.identities.get(&identity).map_or(0, |id| id.0 + 1)
        })
    })
}

/// Retire a native subtree, with host callbacks only for document and cascade-cache notifications.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and the sheet, rule, callbacks,
/// and any non-null detached import must be live. Native rules must belong to Arc allocations. No
/// graph or engine borrow spans a host callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_remove_native_rule(
    host: *const DocumentHost,
    sheet: *const c_void,
    rule: *const c_void,
    detached_import: *const c_void,
    _sheet_id: u32,
    context: *mut c_void,
    begin: unsafe extern "C" fn(*mut c_void, bool, bool),
    notify: unsafe extern "C" fn(*mut c_void, u32, bool),
) {
    use crate::css::rule::{NativeRule, NativeRuleType, mutation, read::RuleRef};
    use crate::css::style_sheet::NativeStyleSheet;
    let removed = unsafe {
        mutation::removed_rules(
            &*rule.cast::<NativeRule>(),
            &*sheet.cast::<NativeStyleSheet>(),
            detached_import.cast::<NativeStyleSheet>().as_ref(),
        )
    };
    let changes_environment =
        RuleRef::Materialized(unsafe { &*rule.cast::<NativeRule>() }).change_needs_style_environment_bump();
    let has_counter_style = removed
        .iter()
        .any(|rule| RuleRef::Materialized(rule).rule_type() == NativeRuleType::CounterStyle);
    unsafe { begin(context, changes_environment, has_counter_style) };
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { sheet_writing_host(host) };
    for rule in removed {
        let declares_layer = mutation::declares_layer(&rule);
        let identity = RuleRef::Materialized(&rule).identity();
        let id = with_engine(host, |engine| engine.native_rule_id(identity));
        unsafe { notify(context, id.map_or(0, |id| id.0 + 1), declares_layer) };
        if let Some(id) = id {
            with_engine(host, |engine| operations::remove_rule(engine, id.0 + 1));
        }
    }
}

/// Read a rule's native cascade data without a CSSOM facade.
///
/// # Safety
/// `host` must be a live document host, on its document's thread. On success the caller owns
/// declarations and must release it with rust_declaration_data_release. Layer text is borrowed
/// until the next rule mutation; callers must copy any text needed across such a mutation. Source
/// identity never retains a native sheet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_native_rule_target(
    host: *const DocumentHost,
    rule: u32,
    result: &mut FfiNativeRuleTarget,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { sheet_writing_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(host, |engine| unsafe { native_rule_target(engine, rule, result) })
}

/// [`style_engine_native_rule_target`] on `engine`, which the style replay tool calls as well.
///
/// # Safety
/// As for [`style_engine_native_rule_target`], for the arguments after `engine`.
pub unsafe fn native_rule_target(engine: &mut StyleEngine, rule: u32, result: &mut FfiNativeRuleTarget) -> bool {
    let Some(target) = rule
        .checked_sub(1)
        .and_then(|id| engine.native_rules.targets.get(&RuleID(id)))
    else {
        return false;
    };
    let Some(declarations) = target.declarations() else {
        return false;
    };
    let origin = engine.program.sheet_origin(engine.program.rule_sheet(RuleID(rule - 1)));
    *result = FfiNativeRuleTarget {
        identity: target.identity.get(),
        declaration_version: engine
            .current_rule_version(RuleID(rule - 1))
            .declaration_block
            .map_or(0, |version| version.0),
        source_identity: target.source_identity,
        declarations: std::sync::Arc::into_raw(declarations.clone()).cast(),
        layer_name: target.layer_name().as_ptr(),
        layer_name_length: target.layer_name().len(),
        has_container_conditions: !target.containers().is_empty(),
        origin: match origin {
            CascadeOrigin::Author => FfiCascadeOrigin::Author,
            CascadeOrigin::AuthorPresentationalHint => FfiCascadeOrigin::AuthorPresentationalHint,
            CascadeOrigin::User => FfiCascadeOrigin::User,
            CascadeOrigin::UserAgent => FfiCascadeOrigin::UserAgent,
            CascadeOrigin::Animation | CascadeOrigin::Transition => {
                unreachable!("stylesheets cannot have an animation origin")
            }
        },
    };
    true
}

/// What a container condition the engine evaluated read of a container or its subject, which the
/// host records as it records its own evaluation's.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiContainerEffectKind {
    /// The node is a container a size or scroll-state query asks about.
    SizeContainerUsage,
    /// The node is a container a style query asks about.
    StyleContainerUsage,
    /// The node is a scroll-state container a query asks about, whose state is snapshotted.
    ScrollStateContainerUsage,
    /// The node has no box yet, and the query is evaluated again after layout.
    NeedsEvaluationAfterLayout,
    /// The subject's style resolved a viewport-relative length in the query.
    SubjectViewportDependency,
    /// A container-relative length resolved against the node, which has no box yet, and is
    /// resolved again after layout, a partial relayout included.
    UnitsNeedEvaluationAfterLayout,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiContainerEffect {
    pub node: u32,
    pub kind: FfiContainerEffectKind,
}

/// What the container conditions of a row the engine answered read of its containers, besides the
/// effects: whether the row's style depends on a size or scroll-state query or a style query, and
/// the custom properties its style queries read, null or the host's to take with
/// `rust_style_query_dependencies_take`.
#[repr(C)]
pub struct FfiContainerEffects {
    pub depends_on_size: bool,
    pub depends_on_style: bool,
    pub style_query_references: *mut c_void,
}

/// Takes what the container conditions of the row the host is installing for `node` read of its
/// containers, for the host to record as it records its own evaluation's. Each effect is handed to
/// `record` with no engine borrow held, so recording may call the engine.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_take_container_effects(
    host: *const DocumentHost,
    node: u32,
    context: *mut c_void,
    record: unsafe extern "C" fn(*mut c_void, FfiContainerEffect),
) -> FfiContainerEffects {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    let verdict = StyleNodeID::from_raw(node)
        .and_then(|node| with_engine(host, |engine| engine.take_container_effects_for_host(node)))
        .unwrap_or_default();
    for (node, kind) in verdict.effects {
        unsafe { record(context, FfiContainerEffect { node: node.raw(), kind }) };
    }
    FfiContainerEffects {
        depends_on_size: verdict.depends_on_size,
        depends_on_style: verdict.depends_on_style,
        style_query_references: verdict
            .style_query_references
            .map_or(std::ptr::null_mut(), |references| Box::into_raw(references).cast()),
    }
}

/// Interns one name identity and returns its document-local atom.
///
/// The caller passes the one-word identity of an interned string it holds a reference to, so the
/// identity cannot be reused while the atom is live.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_intern_atom(host: *const DocumentHost, raw: usize) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(host, |engine| unsafe { intern_atom(engine, raw) })
}

/// [`style_engine_intern_atom`] on `engine`, which the style replay tool calls as well.
///
/// # Safety
/// As for [`style_engine_intern_atom`], for the arguments after `engine`.
pub unsafe fn intern_atom(engine: &mut StyleEngine, raw: usize) -> u32 {
    let result = engine.intern_atom(raw);
    record_interned_atom(engine, raw, result);
    result.0
}

pub(crate) fn intern_native_text(engine: &mut StyleEngine, units: &[u16]) -> StyleAtomID {
    let name = ak::Utf16FlyString::from_utf16(units);
    intern_native_atom(engine, name.raw_identity())
}

pub(crate) fn intern_native_atom(engine: &mut StyleEngine, raw: usize) -> StyleAtomID {
    let result = engine.atoms.intern_raw(raw);
    record_interned_atom(engine, raw, result);
    result
}

fn record_interned_atom(engine: &mut StyleEngine, raw: usize, atom: StyleAtomID) {
    let token = engine.recording_atom_pointer_token(raw);
    engine.record_boundary_call(EventKind::InternAtom, |payload| {
        payload.write_u64(token.expect("an enabled recorder must tokenize the pointer"));
        payload.write_u32(atom.0);
    });
}

/// Takes the pending style transaction under `root` and answers its versioned semantic match answers.
///
/// # Safety
/// The host lends the buffers `computation_inputs` names for this call.
pub(crate) unsafe fn take_style_transaction(
    engine: &mut StyleEngine,
    root: StyleNodeID,
    mut computation_inputs: FfiDocumentStyleComputationInputs,
) -> FfiStyleTransactionOutput {
    // SAFETY: Guaranteed by the caller.
    unsafe { engine.document_resource_contexts.take_in(&mut computation_inputs) };
    // SAFETY: The host lends the media environment the inputs name for this call.
    unsafe { engine.document_media.take_in(&mut computation_inputs) };
    // SAFETY: The host lends the custom functions the inputs name for this call.
    unsafe { engine.take_in_document_functions(&mut computation_inputs) };
    engine.custom_property_registrations_changed = engine
        .document_style_computation_inputs
        .custom_property_registration_generation
        != computation_inputs.custom_property_registration_generation;
    if engine.custom_property_registrations_changed {
        engine.custom_property_environments.registrations_changed();
    }
    if engine.document_style_computation_inputs != computation_inputs {
        // Persistent records are derived from every document computation input, not only the
        // font generation carried in their keys. None reads a resource context, so a move of those
        // keeps them (`ReadsNoResourceContexts`).
        engine.engine_cold_record_cache.clear();
        engine.engine_cold_record_donors.clear();
        engine.engine_pseudo_record_cache.clear();
    }
    // SAFETY: The inputs name the document's live registry, or none.
    engine.custom_property_registry = unsafe {
        crate::css::custom_properties::retain_custom_property_registry(
            computation_inputs.custom_property_registry.as_pointer(),
        )
    };
    engine.document_style_computation_inputs = computation_inputs;
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
    output.reclaimed_style_atoms = std::mem::take(&mut engine.host.reclaimed_style_atoms)
        .into_iter()
        .map(|reclaimed| FfiReclaimedStyleAtom {
            raw: reclaimed.raw,
            atom: reclaimed.atom.0,
        })
        .collect();
    output.style_atoms_swept = std::mem::take(&mut engine.host.style_atoms_swept);
    output.only_derived_child_reactions = engine.take_only_derived_child_reactions();
    if engine.recording_id().is_some() {
        engine.record_boundary_call(EventKind::StyleDeltaBatch, |payload| {
            payload.write_u32(root.raw());
            payload.write_u64(computation_inputs.viewport_width.to_bits());
            payload.write_u64(computation_inputs.viewport_height.to_bits());
            payload.write_u64(computation_inputs.root_font_size.to_bits());
            payload.write_u64(computation_inputs.root_font_x_height.to_bits());
            payload.write_u64(computation_inputs.root_font_cap_height.to_bits());
            payload.write_u64(computation_inputs.root_font_zero_advance.to_bits());
            payload.write_u64(computation_inputs.root_line_height.to_bits());
            payload.write_bool(computation_inputs.root_font_metrics_depend_on_viewport_metrics);
            payload.write_u64(computation_inputs.initial_font_size.to_bits());
            payload.write_u64(computation_inputs.initial_font_x_height.to_bits());
            payload.write_u64(computation_inputs.initial_font_cap_height.to_bits());
            payload.write_u64(computation_inputs.initial_font_zero_advance.to_bits());
            payload.write_i32(computation_inputs.initial_font_size_raw);
            payload.write_i32(computation_inputs.default_font_size_raw);
            payload.write_u64(computation_inputs.device_pixels_per_css_pixel.to_bits());
            payload.write_u64(computation_inputs.font_environment_generation);
            payload.write_u8(computation_inputs.preferred_color_scheme);
            payload.write_bool(computation_inputs.has_document_supported_schemes);
            payload.write_u8(computation_inputs.document_supported_scheme_count);
            for code in computation_inputs.document_supported_scheme_codes {
                payload.write_u8(code);
            }
            let custom_property_registry_is_engine_usable =
                !computation_inputs.custom_property_registry().has_registrations();
            payload.write_bool(custom_property_registry_is_engine_usable);
            payload.write_u64(computation_inputs.custom_property_registration_generation);
            payload.write_bool(computation_inputs.in_quirks_mode);
            let mut outputs = super::record_replay::PayloadWriter::default();
            write_style_transaction_outputs(&output, &mut outputs);
            payload.write_bytes(outputs.as_bytes());
            payload.write_u64(outputs.stable_digest());
        });
        engine.forget_recording_atom_mappings(output.reclaimed_style_atoms.iter().map(|reclaimed| reclaimed.atom));
    }
    output.connected_element_count = engine.connected_element_count();
    output
}

impl FfiStyleTransactionOutput {
    /// The output as C++ reads it, for as long as the output stays where it is.
    pub(crate) fn view(&self) -> FfiStyleTransactionView {
        FfiStyleTransactionView {
            transaction_version: self.transaction_version,
            program_version: self.program_version,
            answers: self.answers.as_ptr(),
            count: self.answers.len(),
            reclaimed_style_atoms: self.reclaimed_style_atoms.as_ptr(),
            reclaimed_style_atom_count: self.reclaimed_style_atoms.len(),
            scoped: self.scoped,
            only_derived_child_reactions: self.only_derived_child_reactions,
            style_atoms_swept: self.style_atoms_swept,
            connected_element_count: self.connected_element_count,
        }
    }
}

/// Takes the pending style transaction of a replay engine, which keeps its output until it is discarded.
///
/// # Safety
/// `engine` must be live, and the host must lend the buffers `computation_inputs` names for this call.
pub unsafe fn style_engine_take_style_transaction_for_replay(
    engine: crate::css::style::StyleEngineHandle,
    root: u32,
    computation_inputs: FfiDocumentStyleComputationInputs,
) -> FfiStyleTransactionView {
    let Some(root) = StyleNodeID::from_raw(root) else {
        return FfiStyleTransactionView::default();
    };
    let engine = unsafe { engine.get_mut() };
    // SAFETY: Guaranteed by the caller.
    let output = unsafe { take_style_transaction(engine, root, computation_inputs) };
    engine.install_ffi_style_transaction_output(output);
    engine.host.ffi_style_transaction_output.view()
}

/// Orders a completed reaction batch for direct application in C++.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `deltas` must name `count`
/// writable entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_sort_style_deltas_for_direct_application(
    host: *const DocumentHost,
    deltas: *mut FfiStyleDelta,
    count: usize,
) {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        if count == 0 {
            return;
        }
        assert!(!deltas.is_null(), "a non-empty delta span must have storage");
        let deltas = unsafe { std::slice::from_raw_parts_mut(deltas, count) };
        // An element's own delta leads the pseudo-element deltas settled beside it.
        let pseudo_rank = |delta: &FfiStyleDelta| {
            if delta.pseudo_kind == u8::MAX {
                0
            } else {
                1 + u16::from(delta.pseudo_kind)
            }
        };
        // Small batches cost less to compare directly. A large batch names its dependency
        // order once instead of walking both ancestor chains in every sort comparison.
        if deltas.len() > 32 {
            let ranks = engine.tree.style_reaction_order_ranks(
                deltas
                    .iter()
                    .map(|delta| StyleNodeID::from_raw(delta.style_node).expect("a style delta must name an element")),
            );
            deltas.sort_unstable_by_key(|delta| {
                let node = StyleNodeID::from_raw(delta.style_node).unwrap();
                (ranks[&node], pseudo_rank(delta))
            });
            return;
        }
        deltas.sort_unstable_by(|first, second| {
            let first_node = StyleNodeID::from_raw(first.style_node).expect("a style delta must name an element");
            let second_node = StyleNodeID::from_raw(second.style_node).expect("a style delta must name an element");
            engine
                .tree
                .compare_style_reaction_order(first_node, second_node)
                .then_with(|| pseudo_rank(first).cmp(&pseudo_rank(second)))
        });
    });
}

/// Installs the atom sweep recorded for the next replay transaction: none unless `swept`, else the
/// authoritative release order.
///
/// # Safety
/// `engine` must be live and `atoms` must name `count` readable atom identities.
pub unsafe fn style_engine_set_replay_atom_sweep(
    engine: crate::css::style::StyleEngineHandle,
    swept: bool,
    atoms: *const u32,
    count: usize,
) {
    let engine = unsafe { engine.get_mut() };
    assert!(engine.host.replay_atom_sweep.is_none());
    assert!(swept || count == 0);
    let atoms = if count == 0 {
        &[]
    } else {
        assert!(!atoms.is_null());
        unsafe { std::slice::from_raw_parts(atoms, count) }
    };
    engine.host.replay_atom_sweep = Some(if swept {
        ReplayAtomSweep::Reclaim(atoms.iter().copied().map(StyleAtomID).collect())
    } else {
        ReplayAtomSweep::Skip
    });
}

/// Reads one counter by index, returning its stable name and writing its value and name length, or
/// null once the index is past the end. C++ enumerates the counters this way rather than
/// duplicating the list. The name is borrowed static UTF-8 and is not nul-terminated.
///
/// # Safety
/// `engine` must be live, and the out pointers must be writable.
pub unsafe fn style_engine_counter_for_replay(
    engine: crate::css::style::StyleEngineHandle,
    index: usize,
    out_value: *mut u64,
    out_name_length: *mut usize,
) -> *const u8 {
    let engine = unsafe { engine.get() };
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
}

/// Records a benchmark phase marker when capture is enabled.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `name` must point at `length`
/// readable UTF-16 code units.
#[cfg(feature = "style-recording")]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_record_benchmark_marker(
    host: *const DocumentHost,
    name: *const c_void,
    length: usize,
    is_ascii: bool,
) {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(host, |engine| {
        if engine.recording_id().is_none() {
            return;
        }
        engine.record_boundary_call(EventKind::BenchmarkMarker, |payload| {
            if is_ascii {
                let name = unsafe { borrow(name.cast::<u8>(), length) };
                payload.write_length(name.len());
                for &code_unit in name {
                    payload.write_u16(u16::from(code_unit));
                }
            } else {
                payload.write_u16_slice(unsafe { borrow(name.cast::<u16>(), length) });
            }
        });
    })
}

pub(super) unsafe fn borrow<'a, T>(pointer: *const T, count: usize) -> &'a [T] {
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
    fn preallocated_siblings_keep_their_disconnected_before_rows() {
        for split_batches in [false, true] {
            let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
            let mut nodes = [0_u32; 4];
            engine.allocate_style_nodes(&mut nodes);
            let root = StyleNodeID::from_raw(nodes[0]).unwrap();
            engine.apply_transaction_batch(
                &[FfiTreeDelta {
                    node: nodes[0],
                    old_connected: false,
                    new_connected: true,
                    old_relations: no_relations(),
                    new_relations: no_relations(),
                }],
                (&[], &[]),
                &[],
                &[],
                &[],
                &[],
            );
            let transaction = engine.take_transaction();
            engine.release_transaction(transaction);

            let arrivals: Vec<_> = (1..nodes.len())
                .map(|index| FfiTreeDelta {
                    node: nodes[index],
                    old_connected: false,
                    new_connected: true,
                    old_relations: no_relations(),
                    new_relations: FfiTreeRelations {
                        parent: nodes[0],
                        previous_element_sibling: if index == 1 { 0 } else { nodes[index - 1] },
                        next_element_sibling: nodes.get(index + 1).copied().unwrap_or(0),
                        ..no_relations()
                    },
                })
                .collect();
            let chunk_size = if split_batches { 1 } else { arrivals.len() };
            for chunk in arrivals.chunks(chunk_size) {
                engine.apply_transaction_batch(chunk, (&[], &[]), &[], &[], &[], &[]);
                // Applying the staged tree between batches must not lose a row's before side.
                engine.apply_staged_tree_deltas();
            }
            let transaction = engine.take_transaction();
            for raw in &nodes[1..] {
                let node = StyleNodeID::from_raw(*raw).unwrap();
                let input = transaction
                    .inputs
                    .iter()
                    .find(|input| input.key == InputKey::TreeRelations(node))
                    .unwrap();
                assert_eq!(input.old, InputValue::TreeRelations(None));
                assert!(
                    transaction
                        .inputs
                        .iter()
                        .any(|input| input.key == InputKey::LocalFeature(node, LocalFeatureKey::ArrivingFacts))
                );
            }
            assert_eq!(
                engine.tree().preorder(root).collect::<Vec<_>>(),
                nodes.map(|raw| StyleNodeID::from_raw(raw).unwrap())
            );
            engine.release_transaction(transaction);
        }
    }

    #[test]
    fn element_arrival_rows_install_intrinsic_facts() {
        assert_eq!(size_of::<FfiElementArrival>(), 36);
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
                adjustment_facts: 0,
                construction_facts: 0,
                custom_state_offset: 0,
                custom_state_count: 2,
                heading_level: 4,
                is_slot: true,
                associated_pseudo_kind_plus_one: 0,
                box_kind: 0,
            },
            FfiElementArrival {
                node: nodes[1],
                namespace_atom: 21,
                language_atom: 22,
                directionality_atom: 23,
                adjustment_facts: 0,
                construction_facts: 0,
                custom_state_offset: 2,
                custom_state_count: 1,
                heading_level: 0,
                is_slot: false,
                associated_pseudo_kind_plus_one: 0,
                box_kind: 0,
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
            adjustment_facts: 0,
            construction_facts: 0,
            custom_state_offset,
            custom_state_count,
            heading_level: 0,
            is_slot: false,
            associated_pseudo_kind_plus_one: 0,
            box_kind: 0,
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
        }));
        assert_eq!(
            published.iter().map(|delta| delta.gap).collect::<Vec<_>>(),
            [
                FfiStyleDeltaGap::Materialize,
                FfiStyleDeltaGap::RetryAfterAncestor,
                FfiStyleDeltaGap::RetryAfterAncestor,
            ]
        );
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
