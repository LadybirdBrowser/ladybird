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

use super::engine_calls::{document_host, with_engine};
use super::rule_writes::RuleWrite;
use crate::render_state::DocumentHost;
use std::ffi::c_void;

use crate::abort_on_panic;
use crate::css::animated_overlay::AnimatedOverlay;
use crate::css::computed_longhand_table::ComputedLonghandTable;
use crate::css::custom_properties::{CustomPropertyRegistry, ffi_slice};
use crate::css::host_shared::{HostShared, SharedPayload};
use crate::css::selector::CompiledSelector;
use crate::css::style_value::RetainedStyleValueData;

use super::batch_matcher::RuleMatch;

use super::compiler::ImplicitScopeRoot;
use super::compiler::NamespaceScope;
use super::compiler::ScopeChain;
use super::index::FeatureValue;
use super::index::LocalFeatureKey;
use super::index::StyleAtomID;
use super::program::CascadeOrigin;
use super::program::CustomDeclaration;
use super::program::DeclarationBlockID;

use super::program::RuleID;
use super::program::SheetID;
use super::transaction::ElementDeclarationKind;
use super::transaction::InputKey;
use super::transaction::InputValue;
use super::transaction::StateFact;
use super::transaction::TreeRelations;
use super::tree::StyleNodeID;
use super::tree::TreeScopeID;
use super::{Counters, StyleEngine, StyleEngineState};

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
    /// Highlight pseudo-elements, which text descendants paint, repaint.
    RepaintHighlights = 1 << 24,
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

/// How a row stands.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiStyleDeltaGap {
    None = 0,
    /// The engine did not settle the row: the host answers it from the element's record demand.
    Materialize = 1,
    /// The engine computed the new record itself from the moved cascade winners; C++ applies it
    /// without running a style computation.
    Computed = 2,
    /// The element needs no style: the host holds none for it in a display:none subtree, and
    /// neither it nor any element inheriting from it reads style while hidden. A read or the
    /// subtree's reveal asks for its record.
    Hidden = 4,
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

/// An engine record a demand settled and the metadata needed to install it.
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
    pub pseudo_records_present: u16,
    pub pseudo_records: [u64; PSEUDO_RECORD_SLOTS],
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
    pub pseudo_records_present: u16,
    pub pseudo_records: [u64; PSEUDO_RECORD_SLOTS],
}

/// One record slot per synthetic pseudo-element kind in an engine record answer.
pub const PSEUDO_RECORD_SLOTS: usize = 10;

#[derive(Default)]
pub(crate) struct FfiStyleTransactionOutput {
    scoped: bool,
    connected_element_count: u32,
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
    /// itself, so inputs naming none are a test's, for a document that registers nothing.
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
            connected_element_count: 0,
        }
    }
}

/// One final base-style assignment. Zero names the absent side of an insertion or removal.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiStyleRecordDelta {
    pub old_style_record: u64,
    pub new_style_record: u64,
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
    /// The record's table, the base record's where it carries an overlay.
    ///
    /// # Safety
    ///
    /// The view must name a record of an element, which carries a table, that stays live for as long as the borrow.
    pub(crate) unsafe fn longhand_table(&self) -> &ComputedLonghandTable {
        // SAFETY: Guaranteed by the caller.
        unsafe { &*self.longhand_table.cast::<ComputedLonghandTable>() }
    }

    /// The record's animation overlay, if it carries one.
    ///
    /// # Safety
    ///
    /// As for [`Self::longhand_table`].
    pub(crate) unsafe fn animated_overlay(&self) -> Option<&AnimatedOverlay> {
        // SAFETY: Guaranteed by the caller.
        unsafe { self.animated_overlay.cast::<AnimatedOverlay>().as_ref() }
    }

    pub(crate) fn missing() -> Self {
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

/// The tree relations of one style node on one side of a mutation. Zero means "no such relation".
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiTreeRelations {
    pub parent: u32,
    pub previous_element_sibling: u32,
    pub next_element_sibling: u32,
    pub tree_scope: u32,
    pub assigned_slot: u32,
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
pub const LAST_SYNTHETIC_PSEUDO_ELEMENT_KIND: u16 = 9;
pub const FIRST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND: u8 = 10;
pub const LAST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND: u8 = 15;

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
    pub(super) fn decode(self) -> CascadeOrigin {
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

impl StyleEngineState {
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
/// the row names until it replaces or gives the row up. The row is written as the host's other
/// writes are, ahead of the next job, which is the first that resolves keyframes from it.
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
    let row = unsafe {
        super::animations::AnimationKeyframes::row(
            ffi_slice(name_lengths, count),
            ffi_slice(name_units, name_unit_count),
            ffi_slice(keyframe_sets, count),
        )
    };
    let write = super::engine_calls::EngineWrite::AnimationKeyframes {
        tree_scope: TreeScopeID(tree_scope),
        shadow_root_identity,
        row,
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { super::engine_calls::queue(host, write) };
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
    use super::custom_property_environments::RetainedCustomPropertyStore;
    // SAFETY: Guaranteed by the caller.
    let retain =
        |store: *const c_void| (!store.is_null()).then(|| unsafe { RetainedCustomPropertyStore::from_borrowed(store) });
    let write = super::engine_calls::EngineWrite::NoteCustomPropertyEnvironment {
        identity,
        store: retain(store),
        inheritable,
        inheritable_store: retain(inheritable_store),
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { super::engine_calls::queue(host, write) };
}

/// [`style_engine_note_custom_property_environment`] on `engine`.
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
}

/// The environment a child inherits from one the engine resolved: itself, unless a registration
/// keeps some of its custom properties from inheriting.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_inheritable_custom_property_environment(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    identity: u64,
) -> u64 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(read, host, |engine| {
        engine.custom_property_environments.inheritable(identity)
    })
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
/// style engine to sample them from. The host retains every value the effects name and streams
/// the description to the engine.
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
    let Some(node) = StyleNodeID::from_raw(node) else {
        return;
    };
    // SAFETY: Guaranteed by the caller.
    let effects = unsafe {
        super::effect_descriptions::PublishedEffectBuffers {
            effects: ffi_slice(effects, effect_count),
            keyframes: ffi_slice(keyframes, keyframe_count),
            declarations: ffi_slice(declarations, declaration_count),
            custom_declarations: ffi_slice(custom_declarations, custom_declaration_count),
            linear_points: ffi_slice(linear_points, linear_point_count),
            base_url_bytes: ffi_slice(base_url_bytes, base_url_byte_count),
        }
        .effects()
    };
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    host.engine_memo().described.borrow_mut().follow(node, slot, &effects);
    host.queue_change(crate::render_state::ArenaChange::Engine(
        super::engine_calls::EngineWrite::AnimationEffectDescriptions { node, slot, effects },
    ));
}

/// Whether the engine describes one of an element's animation lists as holding exactly these
/// versions of its effects, in this order, so that the host need not describe them again. Only
/// the host describes them, so it knows without asking.
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
    StyleNodeID::from_raw(node).is_some_and(|node| {
        host.engine_memo()
            .described
            .borrow()
            .describe(node, slot, unsafe { ffi_slice(versions, count) })
    })
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
    rule.0 + 1
}

/// Publishes a style rule of a user-agent sheet with the selector program the process compiled for
/// it, or answers `None` when the rule is not one or the engine compiles its own.
pub(crate) fn publish_user_agent_style_rule(
    engine: &mut StyleEngine,
    sheet: u32,
    before_rule: u32,
    rule_identity: u64,
    compiled: &[&CompiledSelector],
    namespaces: Option<&super::rule_writes::NamespaceTexts>,
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
            .add_user_agent_style_rule(sheet, before, rule_identity, compiled, namespaces)
            .0
            + 1,
    )
}

/// Replace selectors using immutable compilation inputs, preserving rule identity.
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
    read: &crate::render_state::BegunRead,
    node: u32,
    out: *mut FfiRuleMatch,
    capacity: usize,
    compact_for_cascade: bool,
) -> usize {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(read, host, |engine| unsafe {
        match_element(engine, node, out, capacity, compact_for_cascade)
    })
}

/// [`style_engine_match_element`] on `engine`.
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
    unsafe { write_rule_matches(engine, &matches, out, capacity) }
}

fn collect_native_custom_declarations(
    engine: &mut StyleEngine,
    custom_properties: &[crate::css::declaration_block::CustomProperty],
) -> Vec<(CustomDeclaration, RetainedStyleValueData)> {
    custom_properties
        .iter()
        .map(|property| {
            let name = property.name.to_fly_string();
            let atom = engine.atoms.intern_raw(name.raw());
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

pub(super) fn register_element_declared_properties(
    engine: &mut StyleEngine,
    node: StyleNodeID,
    kind: FfiElementDeclarationKind,
    declarations: &[crate::css::declaration_block::DeclaredProperty],
    custom_properties: &[crate::css::declaration_block::CustomProperty],
) {
    let declarations = engine.intern_element_declared_properties(declarations);
    let custom_declarations = collect_native_custom_declarations(engine, custom_properties);
    let declaration_kind = decode_element_declaration_kind(kind);
    engine.set_element_declared_properties(node, declaration_kind, declarations, custom_declarations);
    // The engine holds the declarations as they were published.
}

/// Whether `declarations` can define transitions.
pub(crate) fn declares_transitions(declarations: &[crate::css::declaration_block::DeclaredProperty]) -> bool {
    use crate::css::property_metadata::property_defines_a_css_transition;
    declarations
        .iter()
        .any(|declaration| property_defines_a_css_transition(declaration.property_id))
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
    use crate::css::declaration_block::DeclarationBlock;
    let Some(node) = StyleNodeID::from_raw(node) else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    let data = unsafe { block.cast::<DeclarationBlock>().as_ref() }.map(DeclarationBlock::data);
    let has_transitions = data.as_ref().is_some_and(|data| declares_transitions(&data.properties));
    // SAFETY: Guaranteed by the caller.
    unsafe { super::engine_calls::queue(host, super::engine_calls::EngineWrite::InlineStyle { node, data }) };
    has_transitions
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
    use crate::css::declaration_block::{DeclarationBlockData, FfiDeclaredProperty, declaration_from_view};
    let Some(node) = StyleNodeID::from_raw(node) else {
        return false;
    };
    let properties = if count == 0 {
        &[]
    } else {
        // SAFETY: Guaranteed by the caller.
        unsafe { std::slice::from_raw_parts(properties.cast::<FfiDeclaredProperty>(), count) }
    };
    // A hint may name a shorthand. Expanded as any declaration block is, it declares every longhand it decides, and so
    // do all declarations the engine is given.
    let mut hints = DeclarationBlockData::default();
    for property in properties {
        // SAFETY: Guaranteed by the caller.
        hints.append_in_specified_order(unsafe { declaration_from_view(property) });
    }
    let has_transitions = declares_transitions(&hints.properties);
    // SAFETY: Guaranteed by the caller.
    unsafe {
        super::engine_calls::queue(
            host,
            super::engine_calls::EngineWrite::PresentationalHints {
                node,
                kind,
                properties: hints.properties.into_boxed_slice(),
            },
        );
    }
    has_transitions
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
    read: &crate::render_state::BegunRead,
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
    with_engine(read, host, |engine| unsafe {
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

/// [`style_engine_publish_computed_groups`] on `engine`.
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

// Shared by host publication and native layout-style derivation.
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
    FfiStyleRecordDelta {
        old_style_record: publication
            .previous_style_record_identity
            .map_or(0, super::computed::FinalStyleRecordID::raw),
        new_style_record: publication.style_record_identity.raw(),
    }
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
    read: &crate::render_state::BegunRead,
    node: u32,
    old_style_record: u64,
    new_style_record: u64,
) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(read, host, |engine| {
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
    read: &crate::render_state::BegunRead,
    node: u32,
    pseudo_kind: u8,
    old_style_record: u64,
    new_style_record: u64,
    originating_style_record: u64,
    counter_styles_changed: bool,
) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(read, host, |engine| {
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

/// Whether a candidate animation overlay changes any effective value of the record `installed` views, in place of the
/// record's own overlay. The host reads it from the record it installed, as the engine would.
///
/// # Safety
/// `installed` must view a record the host holds live, and `animated_overlay` must be null or live, for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_animation_overlay_changed(
    installed: &FfiStyleRecordView,
    animated_overlay: *const c_void,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let (table, old_overlay, new_overlay) = unsafe {
        (
            installed.longhand_table(),
            installed.animated_overlay(),
            animated_overlay.cast::<AnimatedOverlay>().as_ref(),
        )
    };
    super::style_invalidation::animation_overlay_changed(table, old_overlay, new_overlay)
}

/// What the host hands over to have an element's sampled animation overlay composed over the record it installed and
/// published.
#[repr(C)]
pub struct FfiAnimationOverlayPublicationInput {
    pub style_node: u32,
    pub pseudo_kind: u8,
    /// The record the element installed, which the overlay was sampled on and composes over.
    pub style_record: u64,
    /// The longhand table the overlay was sampled over.
    pub longhand_table: *const c_void,
    pub animated_overlay: *const c_void,
    /// The identity of the overlay, or 0 where it animates nothing, which releases the target's overlay record.
    pub animation_overlay_identity: u64,
    pub used_color_scheme: u8,
    pub display_before_box_type_transformation_raw: u32,
    pub is_document_element: bool,
    /// What a target the engine holds no overlay slot for is published again whole with, beside the installed record's
    /// base: the number of inherited style groups, and the custom-property environment the target holds, with its
    /// store.
    pub inherited_group_count: usize,
    pub custom_property_environment: u64,
    pub custom_property_store: *const c_void,
    pub callback_context: *mut c_void,
    /// Writes the animated style's platform font, a `ComputedValuesFFI::FfiFontGroupBuildInputs`;
    /// asked only where the font group is rebuilt.
    pub font_group_inputs: unsafe extern "C" fn(*mut c_void, *mut c_void),
}

/// What publishing an element's sampled animation overlay answers: what replacing the installed record with it damages,
/// the record the target held and the one it holds now, with the view of the latter, which is missing where the engine
/// holds no record the overlay was sampled on.
#[repr(C)]
pub struct FfiAnimationOverlayPublication {
    pub invalidation: FfiAnimationInvalidation,
    pub publication: FfiStyleRecordDelta,
    pub view: FfiStyleRecordView,
    /// Whether the overlay named a value the groups could not be told from, or nothing at all.
    pub rebuilt_every_group: bool,
}

impl FfiAnimationOverlayPublication {
    fn missing() -> Self {
        Self {
            invalidation: FfiAnimationInvalidation::default(),
            publication: FfiStyleRecordDelta::default(),
            view: FfiStyleRecordView::missing(),
            rebuilt_every_group: false,
        }
    }
}

/// Composes an element's sampled animation overlay over the record it installed, rebuilding only the groups the overlay
/// writes (see `RetainedState::build_animation_overlay_payloads`), compares it with that record, and publishes it as
/// the target's record, in one call of the engine. A second call is made only where the overlay rebuilds the font group: resolving the animated font may
/// read the engine, so the host resolves it between the two. The host keeps the view of the record it answers, which it
/// installs next.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and everything `input` points to must be live for
/// the call, its table non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_publish_sampled_animation_overlay(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    input: &FfiAnimationOverlayPublicationInput,
) -> FfiAnimationOverlayPublication {
    use crate::css::table_group_builder::FfiFontGroupBuildInputs;

    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    let publish = |font: Option<&FfiFontGroupBuildInputs>| {
        // SAFETY: Guaranteed by the caller.
        with_engine(read, host, |engine| unsafe {
            publish_sampled_animation_overlay(engine, input, font)
        })
    };
    let published = match publish(None) {
        Err(super::engine_sample::NeedsHostFont) => {
            let mut font = std::mem::MaybeUninit::<FfiFontGroupBuildInputs>::uninit();
            // SAFETY: Guaranteed by the caller. The host writes the whole font.
            let font = unsafe {
                (input.font_group_inputs)(input.callback_context, font.as_mut_ptr().cast());
                font.assume_init()
            };
            publish(Some(&font))
        }
        published => published,
    };
    let Ok(published) = published else {
        return FfiAnimationOverlayPublication::missing();
    };
    if published.view.present {
        host.engine_memo()
            .views
            .set(published.publication.new_style_record, published.view);
    }
    published
}

/// [`style_engine_publish_sampled_animation_overlay`]'s one call of `engine`, which answers `Err` where it needs the
/// animated font, `font`, it was not given.
///
/// # Safety
/// As for [`style_engine_publish_sampled_animation_overlay`].
unsafe fn publish_sampled_animation_overlay(
    engine: &mut StyleEngine,
    input: &FfiAnimationOverlayPublicationInput,
    font: Option<&crate::css::table_group_builder::FfiFontGroupBuildInputs>,
) -> Result<FfiAnimationOverlayPublication, super::engine_sample::NeedsHostFont> {
    use crate::css::table_group_builder::group_index;

    let Some(node) = StyleNodeID::from_raw(input.style_node) else {
        return Ok(FfiAnimationOverlayPublication::missing());
    };
    // SAFETY: Guaranteed by the caller.
    let (table, overlay) = unsafe {
        (
            &*input.longhand_table.cast::<ComputedLonghandTable>(),
            input.animated_overlay.cast::<AnimatedOverlay>().as_ref(),
        )
    };
    let mut payloads = [std::ptr::null(); group_index::COUNT];
    let Some(rebuilt) = engine.build_animation_overlay_payloads(
        node,
        input.pseudo_kind,
        input.style_record,
        table,
        overlay,
        input.used_color_scheme,
        input.display_before_box_type_transformation_raw,
        font,
        &mut payloads,
    ) else {
        return Ok(FfiAnimationOverlayPublication::missing());
    };
    let rebuilt = rebuilt?;
    // Compared before the publication, which may release the installed record: nothing else keeps a pseudo-element's.
    let invalidation = engine.compare_animation_overlay(
        input.style_record,
        input.animated_overlay.cast(),
        SharedPayload::from_pointer_slice(&payloads),
        input.is_document_element,
    );
    let (animated_overlay, overlay_payloads) = match input.animation_overlay_identity {
        0 => (std::ptr::null(), &[][..]),
        _ => (input.animated_overlay, &payloads[..]),
    };
    let publication = match engine.publish_animation_overlay_impl(
        super::computed::ComputedStyleTarget::new(node, input.pseudo_kind),
        input.animation_overlay_identity,
        HostShared::new(animated_overlay).cast(),
        SharedPayload::from_pointer_slice(overlay_payloads),
    ) {
        Some(publication) => FfiStyleRecordDelta {
            old_style_record: publication.previous_style_record.raw(),
            new_style_record: publication.style_record.raw(),
        },
        // A pseudo-element the engine holds no assignment for owns no overlay slot, so the record is published again
        // whole, with the overlay over the same base.
        None => {
            // SAFETY: The base record stays live for the call.
            let base = unsafe { style_record_view(engine, input.style_record) };
            // SAFETY: Guaranteed by the caller, and the base's arrays are live.
            unsafe {
                publish_computed_groups(
                    engine,
                    input.style_node,
                    input.pseudo_kind,
                    base.base_payloads,
                    base.payload_count,
                    input.inherited_group_count,
                    input.custom_property_environment,
                    false,
                    base.counter_style_environment_identity,
                    input.animation_overlay_identity,
                    animated_overlay,
                    overlay_payloads.as_ptr(),
                    overlay_payloads.len(),
                    base.longhand_table,
                    input.custom_property_store,
                )
            }
        }
    };
    super::engine_sample::release_rebuilt_overlay_payloads(&payloads, rebuilt.groups);
    Ok(FfiAnimationOverlayPublication {
        invalidation,
        publication,
        // SAFETY: The published record is live.
        view: unsafe { style_record_view(engine, publication.new_style_record) },
        rebuilt_every_group: rebuilt.every_group,
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
    read: &crate::render_state::BegunRead,
    style_record: u64,
) -> FfiStyleRecordView {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    if let Some(view) = super::engine_calls::known_style_record_view(host, style_record) {
        return view;
    }
    // SAFETY: Guaranteed by the caller.
    let view = with_engine(read, host, |engine| unsafe { style_record_view(engine, style_record) });
    if view.present {
        host.engine_memo().views.set(style_record, view);
    }
    view
}

/// [`style_engine_style_record_view`] on `engine`.
///
/// # Safety
/// As for [`style_engine_style_record_view`], for the arguments after `engine`.
pub unsafe fn style_record_view(engine: &mut StyleEngine, style_record: u64) -> FfiStyleRecordView {
    let view = engine.style_record_view(style_record);
    match &view {
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
    }
}

/// Removes the retained computed-input assignment for one pseudo-element kind.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_remove_computed_pseudo(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    node: u32,
    pseudo_kind: u8,
) -> FfiStyleRecordDelta {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(read, host, |engine| unsafe {
        remove_computed_pseudo(engine, node, pseudo_kind)
    })
}

/// [`style_engine_remove_computed_pseudo`] on `engine`.
///
/// # Safety
/// As for [`style_engine_remove_computed_pseudo`], for the arguments after `engine`.
pub unsafe fn remove_computed_pseudo(engine: &mut StyleEngine, node: u32, pseudo_kind: u8) -> FfiStyleRecordDelta {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return FfiStyleRecordDelta::default();
    };
    FfiStyleRecordDelta {
        old_style_record: engine
            .remove_computed_pseudo(node, pseudo_kind)
            .map_or(0, super::computed::FinalStyleRecordID::raw),
        new_style_record: 0,
    }
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
        custom_declarations,
        custom_written_values,
    );
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
    read: &crate::render_state::BegunRead,
    node: u32,
    environment: u64,
    store: *const c_void,
) -> u64 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(read, host, |engine| {
        let Some(node) = StyleNodeID::from_raw(node) else {
            return 0;
        };
        unsafe { engine.custom_property_environments.retain(environment, store) };
        engine.republish_record_environment(node, environment).unwrap_or(0)
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

/// A move of one element's custom-property environment, as the host made it: what it handed its
/// children before and hands them now, with the host's object for the latter and whether that
/// declares custom properties of its own.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiEnvironmentMove {
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
    read: &crate::render_state::BegunRead,
    origin: u32,
    moved: FfiEnvironmentMove,
) -> FfiEnvironmentMoveActions {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    let (answer, moved) = with_engine(read, host, |engine| {
        let mut actions = std::mem::take(&mut engine.host.environment_move_actions);
        actions.clear();
        if let Some(origin) = StyleNodeID::from_raw(origin) {
            let named = |environment: FfiNamedEnvironment| super::environment_move::NamedEnvironment {
                identity: environment.identity,
                store: environment.store,
            };
            let moved = super::environment_move::EnvironmentMove {
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
        // Each element the move left an environment takes the record republished over it.
        let mut moved = Vec::new();
        for action in &actions {
            let Some(node) =
                StyleNodeID::from_raw(action.node).filter(|_| action.kind == FfiEnvironmentMoveActionKind::Republish)
            else {
                continue;
            };
            moved.push((
                node,
                None,
                engine.element_custom_property_data(node).expose_provenance(),
            ));
            moved.extend(
                engine
                    .pseudo_element_custom_property_environments(node)
                    .map(|(pseudo, data)| (node, Some(pseudo), data.expose_provenance())),
            );
        }
        engine.host.environment_move_actions = actions;
        (answer, moved)
    });
    host.engine_memo().held.borrow_mut().follow_moved(&moved);
    answer
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
    /// A read-only read of the element's record, driven in full against the parent as it is now,
    /// which the reader assigned a record of its own.
    ElementReadAgainstParent,
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
    SearchText = 6,
    SearchTextCurrent = 7,
    Selection = 8,
    ViewTransition = 9,
    DetailsContent = 10,
    FileSelectorButton = 11,
    Placeholder = 12,
    SliderFill = 13,
    SliderThumb = 14,
    SliderTrack = 15,
    ViewTransitionGroup = 18,
    ViewTransitionImagePair = 19,
    ViewTransitionNew = 20,
    ViewTransitionOld = 21,
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
    let read_only = demand.is_read_only();
    match engine.answer_record_demand(style_node, demand) {
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
    }
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
    read: &crate::render_state::BegunRead,
    subject: u32,
    facts: u32,
    hint_kind: FfiElementDeclarationKind,
    hints: *const c_void,
    hint_count: usize,
    inline_block: *const c_void,
) -> u64 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(read, host, |engine| {
        use crate::css::declaration_block::{DeclarationBlock, FfiDeclaredProperty, declaration_from_view};
        abort_on_panic(|| {
            let Some(subject) = StyleNodeID::from_raw(subject) else {
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
    read: &crate::render_state::BegunRead,
    node: u32,
    old_is_list_item: bool,
) -> FfiSettledPseudoRecords {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(read, host, |engine| unsafe {
        settle_pseudo_records_after_host_record(engine, node, old_is_list_item)
    })
}

/// [`style_engine_settle_pseudo_records_after_host_record`] on `engine`.
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
    read: &crate::render_state::BegunRead,
    identity: u64,
    parent: *mut u64,
) -> *const c_void {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(read, host, |engine| {
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

// The raw identity must remain a live Utf16FlyString for the native engine to retain it.
pub(super) unsafe fn note_native_custom_property_name(
    engine: &mut StyleEngine,
    name: StyleAtomID,
    raw: usize,
    text: &[u16],
) {
    unsafe { engine.note_custom_property_name(name, raw, text) };
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
    unsafe { document_host(host) }.next_declaration_block_version()
}

/// Queues a native declaration edit for the rule that owns the declarations, where the host published that rule to the
/// sheet `sheet`, and answers whether they declare transitions. The host is told first, with no engine borrowed.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `rule` live. The callback
/// must not mutate the native rule graph, and must remain valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_native_rule_declarations_changed(
    host: *const DocumentHost,
    sheet: u32,
    rule: *const c_void,
    context: *mut c_void,
    notify: unsafe extern "C" fn(*mut c_void),
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    let rule = unsafe { &*rule.cast::<crate::css::rule::NativeRule>() };
    let Some(identity) = rule
        .declaration_owner_identity()
        .filter(|&identity| host.published_rules().contains(sheet, identity))
    else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { notify(context) };
    let declarations = rule.cascade_declarations();
    let transitions = declarations
        .as_ref()
        .is_some_and(|declarations| declares_transitions(&declarations.properties));
    host.write_rules(RuleWrite::RuleDeclarations { identity, declarations });
    transitions
}

/// The native identity of the first rule after an inserted native subtree that the host published to the sheet
/// `sheet`, or 0 for none, without creating CSSOM objects.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `native` a live native sheet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_native_rule_successor(
    host: *const DocumentHost,
    sheet: u32,
    native: *const c_void,
    identity: u64,
) -> u64 {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    let native = unsafe { &*native.cast::<crate::css::style_sheet::NativeStyleSheet>() };
    host.published_rules().successor(sheet, native, identity)
}

/// Retires a native subtree from the sheet `sheet`, with host callbacks only for document and cascade-cache
/// notifications, and queues the removal of the rules the host published there.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and the native sheet, rule, callbacks, and any
/// non-null detached import must be live. Native rules must belong to Rc allocations. No graph borrow spans a host
/// callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_remove_native_rule(
    host: *const DocumentHost,
    native: *const c_void,
    rule: *const c_void,
    detached_import: *const c_void,
    sheet: u32,
    context: *mut c_void,
    begin: unsafe extern "C" fn(*mut c_void, bool, bool),
    notify: unsafe extern "C" fn(*mut c_void, bool),
) {
    use crate::css::rule::{NativeRule, NativeRuleType, mutation, read::RuleRef};
    use crate::css::style_sheet::NativeStyleSheet;
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    let removed = unsafe {
        mutation::removed_rules(
            &*rule.cast::<NativeRule>(),
            &*native.cast::<NativeStyleSheet>(),
            detached_import.cast::<NativeStyleSheet>().as_ref(),
        )
    };
    let changes_environment =
        RuleRef::Materialized(unsafe { &*rule.cast::<NativeRule>() }).change_needs_style_environment_bump();
    let has_counter_style = removed
        .iter()
        .any(|rule| RuleRef::Materialized(rule).rule_type() == NativeRuleType::CounterStyle);
    unsafe { begin(context, changes_environment, has_counter_style) };
    let mut published = Vec::new();
    for rule in removed {
        unsafe { notify(context, mutation::declares_layer(&rule)) };
        let identity = RuleRef::Materialized(&rule).identity();
        if host.published_rules().remove(sheet, identity) {
            published.push(identity);
        }
    }
    if !published.is_empty() {
        host.write_rules(RuleWrite::RemoveRules(published.into()));
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
    read: &crate::render_state::BegunRead,
    rule: u32,
    result: &mut FfiNativeRuleTarget,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    // SAFETY: Guaranteed by the caller.
    with_engine(read, host, |engine| unsafe { native_rule_target(engine, rule, result) })
}

/// [`style_engine_native_rule_target`] on `engine`.
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
    read: &crate::render_state::BegunRead,
    node: u32,
    context: *mut c_void,
    record: unsafe extern "C" fn(*mut c_void, FfiContainerEffect),
) -> FfiContainerEffects {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    let verdict = StyleNodeID::from_raw(node)
        .filter(|_| host.container_effects_may_be_held())
        .and_then(|node| with_engine(read, host, |engine| engine.take_container_effects_for_host(node)))
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

/// Interns the UTF-16 name `units` in `engine` as a native style atom.
pub(crate) fn intern_native_text(engine: &mut StyleEngine, units: &[u16]) -> StyleAtomID {
    let name = ak::Utf16FlyString::from_utf16(units);
    engine.atoms.intern_raw(name.raw_identity())
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
    output.only_derived_child_reactions = engine.take_only_derived_child_reactions();
    // The host applies the rows in this order, so the transaction answers them in it.
    sort_style_deltas_for_direct_application(engine, &mut output.answers);
    output.connected_element_count = engine.connected_element_count();
    output
}

impl FfiStyleTransactionOutput {
    /// The rows the transaction answered.
    pub(crate) fn answers(&self) -> &[FfiStyleDelta] {
        &self.answers
    }

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
            connected_element_count: self.connected_element_count,
        }
    }
}

/// Orders a completed reaction batch for direct application in C++.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `deltas` must name `count`
/// writable entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_sort_style_deltas_for_direct_application(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    deltas: *mut FfiStyleDelta,
    count: usize,
) {
    // A single delta is in order already, which the host knows without asking.
    if count < 2 {
        return;
    }
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    with_engine(read, host, |engine| {
        assert!(!deltas.is_null(), "a non-empty delta span must have storage");
        // SAFETY: Guaranteed by the caller.
        sort_style_deltas_for_direct_application(engine, unsafe { std::slice::from_raw_parts_mut(deltas, count) });
    });
}

/// Orders `deltas` for direct application in C++: each inheritance branch contiguously in preorder, an element's own
/// delta ahead of the pseudo-element deltas settled beside it.
fn sort_style_deltas_for_direct_application(engine: &StyleEngine, deltas: &mut [FfiStyleDelta]) {
    let pseudo_rank = |delta: &FfiStyleDelta| {
        if delta.pseudo_kind == u8::MAX {
            0
        } else {
            1 + u16::from(delta.pseudo_kind)
        }
    };
    let node =
        |delta: &FfiStyleDelta| StyleNodeID::from_raw(delta.style_node).expect("a style delta must name an element");
    // Small batches cost less to compare directly. A large batch names its dependency order once instead of walking
    // both ancestor chains in every sort comparison.
    if deltas.len() > 32 {
        let ranks = engine.tree.style_reaction_order_ranks(deltas.iter().map(node));
        deltas.sort_unstable_by_key(|delta| (ranks[&node(delta)], pseudo_rank(delta)));
        return;
    }
    deltas.sort_unstable_by(|first, second| {
        engine
            .tree
            .compare_style_reaction_order(node(first), node(second))
            .then_with(|| pseudo_rank(first).cmp(&pseudo_rank(second)))
    });
}

pub(super) unsafe fn borrow<'a, T>(pointer: *const T, count: usize) -> &'a [T] {
    if count == 0 {
        return &[];
    }
    assert!(!pointer.is_null(), "a non-empty delta array must not be null");
    unsafe { std::slice::from_raw_parts(pointer, count) }
}

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
        }
    }

    #[test]
    fn initial_tree_batch_uses_the_document_root_as_its_transaction_envelope() {
        let mut engine = StyleEngine::new();
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
            let mut engine = StyleEngine::new();
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
        let mut engine = StyleEngine::new();
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
        let mut engine = StyleEngine::new();
        engine.apply_transaction_batch(&[], (&[arrival_for(0, 0, 0)], &[]), &[], &[], &[], &[]);
    }

    #[test]
    #[should_panic(expected = "an element arrival custom-state range overflowed")]
    fn malformed_element_arrival_rejects_an_overflowing_custom_state_range() {
        let mut engine = StyleEngine::new();
        let mut nodes = [0];
        engine.allocate_style_nodes(&mut nodes);
        engine.apply_transaction_batch(&[], (&[arrival_for(nodes[0], u32::MAX, 1)], &[]), &[], &[], &[], &[]);
    }

    #[test]
    #[should_panic(expected = "an element arrival named custom states outside the shared atom column")]
    fn malformed_element_arrival_rejects_an_out_of_bounds_custom_state_range() {
        let mut engine = StyleEngine::new();
        let mut nodes = [0];
        engine.allocate_style_nodes(&mut nodes);
        engine.apply_transaction_batch(&[], (&[arrival_for(nodes[0], 0, 2)], &[1]), &[], &[], &[], &[]);
    }

    #[test]
    fn initial_tree_bulk_load_publishes_match_answers_before_traversal() {
        let mut engine = StyleEngine::new();
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
        // The engine of a test computes no records, so each row waits for the host to install the
        // one above it, in a wave of its own.
        for _ in 0..nodes.len() {
            assert!(!engine.take_style_transaction(root, |_, _, answers| {
                emission_count += 1;
                published.extend_from_slice(answers);
            }));
        }
        assert!(!engine.has_pending_transaction());
        assert_eq!(emission_count, 3);
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
            [FfiStyleDeltaGap::Materialize; 3]
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
        let mut engine = StyleEngine::new();
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
        let mut engine = StyleEngine::new();
        engine.apply_transaction_batch(&[], (&[], &[]), &[], &[], &[], &[]);
        let transaction = engine.take_transaction();
        assert!(transaction.is_empty());
        engine.release_transaction(transaction);
        assert_eq!(engine.memory().bytes_in_tier(Tier::Acceleration), 0);
    }

    #[test]
    fn identities_are_minted_in_one_call_per_batch() {
        let mut engine = StyleEngine::new();
        let mut nodes = [0_u32; 512];
        engine.allocate_style_nodes(&mut nodes);
        assert_eq!(engine.counters().get(Counter::StyleNodesAllocated), 512);
        assert!(nodes.windows(2).all(|pair| pair[0] < pair[1]));
        assert!(engine.memory().bytes_in_category(MemoryCategory::RelationColumns) > 0);
    }
}
