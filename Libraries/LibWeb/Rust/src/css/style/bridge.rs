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

use super::engine_calls::with_engine;
use super::rule_writes::RuleWrite;
use crate::render_state::DocumentHost;
use std::ffi::c_void;

use crate::abort_on_panic;
use crate::css::animated_overlay::AnimatedOverlay;
use crate::css::custom_properties::CustomPropertyRegistry;
use crate::css::host_shared::HostShared;
use crate::css::selector::SelectorListRef;
use crate::css::style_value::RetainedStyleValueData;

use super::compiler::ImplicitScopeRoot;
use super::index::FeatureValue;
use super::index::LocalFeatureKey;
use super::index::StyleAtomID;
use super::pipeline::rule_store::CascadeOrigin;
use super::pipeline::rule_store::CustomDeclaration;
use super::pipeline::rule_store::DeclarationBlockID;

use super::StyleEngine;
use super::input_types::ElementDeclarationKind;
use super::input_types::InputKey;
use super::input_types::InputValue;
use super::input_types::StateFact;
use super::input_types::TreeRelations;
use super::pipeline::rule_store::RuleID;
use super::pipeline::rule_store::SheetID;
use super::tree::StyleNodeID;
use super::tree::TreeScopeID;

mod animations;
mod container_effects;
mod native_rules;
mod records;
pub(crate) use animations::*;
pub(crate) use container_effects::*;
#[cfg(test)]
pub(crate) use native_rules::*;
pub(crate) use records::*;

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
    AffectsHitTesting = 1 << 22,
    /// The word holds the damage the engine computed with its answer.
    EngineComputed = 1 << 23,
    /// Highlight pseudo-elements, which text descendants paint, repaint.
    RepaintHighlights = 1 << 24,
    /// The white space of the element's text children collapses where it did not, or the other way round, so the white
    /// space text among them may gain or lose its box.
    WhiteSpaceCollapseChanged = 1 << 25,
    CursorChanged = 1 << 26,
}

/// The `InvalidationLevel` of a style move at which its box is painted again, the one at which it lays out again, and
/// the one at which the layout tree is built again.
pub(crate) const REPAINT_LEVEL: u32 = 1;
pub(crate) const RELAYOUT_LEVEL: u32 = 2;
pub(crate) const REBUILD_LEVEL: u32 = 3;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiAnimationInvalidation {
    pub invalidation: u32,
    pub requires_base_style_recomputation: bool,
    pub requires_layout_node_style_application: bool,
    pub requires_style_resource_update: bool,
}

/// One final style assignment.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct FfiStyleDelta {
    pub style_node: u32,
    pub old_style_record: u64,
    pub new_style_record: u64,
    pub reaction: u8,
    pub inherited_style_groups: u8,
    pub pseudo_kind: u8,
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

impl FfiStyleDelta {}

/// An engine record a demand settled.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiEngineComputedRecord {
    pub style_record: u64,
    /// The record the host holds for `style_record`, which the engine holds until the element's
    /// styles are next read or settled, or null where the demand answered none.
    pub host_record: *const c_void,
}

impl Default for FfiEngineComputedRecord {
    fn default() -> Self {
        Self {
            style_record: 0,
            host_record: std::ptr::null(),
        }
    }
}

/// One record slot per synthetic pseudo-element kind in an engine record answer.
pub const PSEUDO_RECORD_SLOTS: usize = 11;

#[derive(Default)]
pub(crate) struct FfiStyleUpdateOutput {
    answers: Vec<FfiStyleDelta>,
    reclaimed_style_atoms: Vec<super::atoms::ReclaimedStyleAtom>,
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

/// Document-wide scalar computation inputs captured at a style update boundary.
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
    /// https://drafts.csswg.org/css-viewport/#effective-zoom
    /// The effective zoom of the document element, which its font metrics above are zoomed by.
    pub root_effective_zoom: f64,
    /// The metrics of the document's initial font, which the document element's own font
    /// resolves against.
    pub initial_font_size: f64,
    pub initial_font_x_height: f64,
    pub initial_font_cap_height: f64,
    pub initial_font_zero_advance: f64,
    pub initial_line_height: f64,
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
    /// Which collection of the contexts the host lends, which the engine need not read where it holds that one.
    pub style_sheet_resource_contexts_version: u64,
    /// What `media()` conditions in `if()` read, lent for the boundary call only as the style
    /// update's media environment holds them: an `FfiMediaFeatureValue` for each media feature,
    /// and the `FfiLengthResolutionContext` lengths in a media query resolve against. The engine
    /// copies them and clears these fields before it keeps the inputs.
    pub media_feature_values: FfiHostHandle,
    pub media_feature_value_count: usize,
    pub media_length_resolution_context: FfiHostHandle,
    /// What a custom function call reads, lent for the boundary call only: an
    /// `FfiCustomFunctionEntry` for each `@function` rule of each scope. None where no scope holds
    /// an `@function` rule. The engine retains the definitions and clears these fields before it
    /// keeps the inputs.
    pub custom_functions: FfiHostHandle,
    pub custom_function_count: usize,
}

/// One `@function` rule of a scope (a `StyleScope`'s identity), or, with no function, a scope that
/// holds none: the scope a name the scope does not define is looked up in next, none for the
/// document's, the scope's tree scope, and of the rule, the interned name of its cascade layer and
/// the precedence of its cascade origin (user agent, user, then author).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiCustomFunctionEntry {
    pub function: *const c_void,
    pub scope: usize,
    pub parent_scope: usize,
    pub tree_scope: u32,
    pub layer: u32,
    pub origin: u8,
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
            root_effective_zoom: 1.0,
            root_font_metrics_depend_on_viewport_metrics: false,
            initial_font_size: 0.0,
            initial_font_x_height: 0.0,
            initial_font_cap_height: 0.0,
            initial_font_zero_advance: 0.0,
            initial_line_height: 0.0,
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
            style_sheet_resource_contexts_version: 0,
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

/// What a resolution request's `font-size-adjust` asks of the resolver.
/// https://drafts.csswg.org/css-fonts-5/#font-size-adjust-prop
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Hash, Default, Debug)]
pub enum FontSizeAdjustKind {
    /// `none`: the resolver selects fonts at the computed font size.
    #[default]
    None,
    /// A number: the resolver scales the size it selects fonts at so that the metric of the first
    /// available font is that proportion of the computed font size.
    Number,
    /// `from-font`: the resolver measures the metric of the first available font, which is the
    /// number the value computes to, and sizes the other fonts by that number.
    FromFont,
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
    /// The computed `font-size-adjust`: its kind, the metric it normalizes (a
    /// `font_size_adjust_metric` value), and the number for `FontSizeAdjustKind::Number`. The
    /// metric and number are zero for `FontSizeAdjustKind::None`.
    pub font_size_adjust: FontSizeAdjustKind,
    pub font_size_adjust_metric: u8,
    pub font_size_adjust_value: f64,
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
    /// For a request naming `from-font`, the number it computes to: the request's metric of the
    /// first available font, divided by that font's size. Zero for other requests, and where the
    /// font has no size to divide by.
    pub font_size_adjust_from_font: f64,
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
    /// The record's groups, a `StyleGroups`, composed with its animation overlay where it carries
    /// one, which the host reads through `CSS::style_record_group`.
    pub groups: *const c_void,
    /// The style record that makes a table for each reader asking for one, which holds the
    /// payloads beneath the animation overlay where the view carries one.
    pub longhand_table_record: *const c_void,
    /// The record's animation overlay, which only the record of an element animating carries.
    pub animated_overlay: *const c_void,
    pub dependency_flags: u8,
    pub present: bool,
}

// NB: Every styled element and pseudo-element holds one of these.
const _: () = assert!(size_of::<FfiStyleRecordView>() == 32);

impl FfiStyleRecordView {
    /// The view of `record`, a record the host holds: an installed record, or the sample of the
    /// animations over one, whose view reads the longhands of the base.
    pub(super) fn of_record(record: &super::pipeline::Record) -> Self {
        match record.sample_of_animations() {
            Some(sample) => Self {
                groups: std::ptr::from_ref(&record.payloads).cast(),
                longhand_table_record: std::sync::Arc::as_ptr(&sample.base).cast(),
                animated_overlay: std::sync::Arc::as_ptr(&sample.animated_overlay).cast(),
                dependency_flags: sample.dependency_flags,
                present: true,
            },
            None => Self {
                groups: std::ptr::from_ref(&record.payloads).cast(),
                longhand_table_record: std::ptr::from_ref(record).cast(),
                animated_overlay: std::ptr::null(),
                dependency_flags: record.view_dependency_flags(),
                present: true,
            },
        }
    }

    /// The longhands of the record, the base record's where it carries an overlay, which reading
    /// does not make a style record's table for.
    ///
    /// # Safety
    ///
    /// The view must name a record that stays live for as long as the borrow.
    pub(crate) unsafe fn longhands(&self) -> Option<super::ViewLonghands<'_>> {
        // SAFETY: Guaranteed by the caller.
        unsafe { self.longhand_table_record.cast::<super::pipeline::Record>().as_ref() }
            .map(super::ViewLonghands::of_record)
    }

    /// The record's animation overlay, if it carries one.
    ///
    /// # Safety
    ///
    /// As for [`Self::longhands`].
    pub(crate) unsafe fn animated_overlay(&self) -> Option<&AnimatedOverlay> {
        // SAFETY: Guaranteed by the caller.
        unsafe { self.animated_overlay.cast::<AnimatedOverlay>().as_ref() }
    }

    /// The group payloads beneath the record's animation overlay, which are its own payloads where
    /// it carries none, or null where the view is missing.
    ///
    /// # Safety
    ///
    /// As for [`Self::longhands`].
    pub(crate) unsafe fn base_groups(&self) -> Option<&crate::css::computed_values::StyleGroups> {
        // SAFETY: Guaranteed by the caller.
        match unsafe { self.longhand_table_record.cast::<super::pipeline::Record>().as_ref() } {
            Some(record) => Some(&record.payloads),
            // SAFETY: Guaranteed by the caller.
            None => unsafe { self.groups().as_ref() },
        }
    }

    /// The record's groups, composed with its animation overlay where it carries one, or null
    /// where the view is missing.
    pub(crate) fn groups(&self) -> *const crate::css::computed_values::StyleGroups {
        self.groups.cast()
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
    pub(super) fn decode(self) -> TreeRelations {
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

/// Selector-visible facts which exist when one style node first joins the tree, all of which its
/// projection row takes in one write. The variable-width lists occupy one shared atom column, from
/// this row's offset on: its custom states, then its classes, then its attributes as name and value
/// atom pairs.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiElementArrival {
    /// The element's states, a bit per `FfiStateFact`.
    pub states: u64,
    pub node: u32,
    pub tag_atom: u32,
    /// The ASCII-lowercase folding of the tag, or the tag itself where folding leaves it as it is.
    pub folded_tag_atom: u32,
    /// The element's ID, or zero for none.
    pub id_atom: u32,
    pub namespace_atom: u32,
    pub language_atom: u32,
    pub directionality_atom: u32,
    pub custom_state_count: u32,
    pub class_count: u32,
    pub attribute_count: u32,
    pub heading_level: u8,
    pub is_slot: bool,
    /// Whether a text child keeps the element from being `:empty`.
    pub has_text_content: bool,
    /// One plus the element-backed pseudo-element kind the element stands for in its host's
    /// shadow tree, or zero.
    pub associated_pseudo_kind_plus_one: u8,
    /// The principal box the element asks for.
    pub box_kind: ElementBoxKind,
    /// The element's `ElementStyleAdjustmentFact` bits: what the box-type transformation and the
    /// element style adjustments read of the DOM. Mirrors the C++ enum.
    pub adjustment_facts: u32,
    /// The element's `ElementConstructionFact` bits: what a layout row built for it records.
    pub construction_facts: u32,
}

/// The facts of `arrival` its row takes, with its run `atoms` of the shared atom column: its custom states, then its
/// classes, then its attributes as name and value pairs.
pub(in crate::css::style) fn element_arrival_facts<'a>(
    arrival: &FfiElementArrival,
    atoms: &'a [u32],
) -> super::projection_writes::ElementArrivalFacts<'a> {
    super::projection_writes::ElementArrivalFacts {
        tag: StyleAtomID(arrival.tag_atom),
        folded_tag: StyleAtomID(arrival.folded_tag_atom),
        namespace: StyleAtomID(arrival.namespace_atom),
        id: StyleAtomID(arrival.id_atom),
        language: StyleAtomID(arrival.language_atom),
        directionality: StyleAtomID(arrival.directionality_atom),
        heading_level: arrival.heading_level,
        backing_pseudo_kind_plus_one: arrival.associated_pseudo_kind_plus_one,
        is_slot: arrival.is_slot,
        has_text_content: arrival.has_text_content,
        states: super::index::StateSet(arrival.states),
        adjustment_facts: arrival.adjustment_facts,
        construction_facts: arrival.construction_facts,
        box_kind: arrival.box_kind,
        atoms,
        custom_state_count: arrival.custom_state_count as usize,
        class_count: arrival.class_count as usize,
    }
}

pub const FIRST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND: u8 = 11;
pub const LAST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND: u8 = 16;

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
    /// The element stands for an element-reference pseudo-element of its shadow host.
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
}

/// A set of the main-side effects of installing one element's style record, as the host's
/// `CSS::MainEffects` names them.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(transparent)]
pub struct MainEffects(u32);

impl MainEffects {
    pub(crate) const NONE: Self = Self(0);
    /// The element's animations compose over the record.
    pub(crate) const ANIMATED: Self = Self(1 << 0);
    /// The element enters or leaves display:none.
    pub(crate) const DISPLAY_NONE_FLIP: Self = Self(1 << 1);
    /// The host clears the styles of the element's descendants, as the element enters display:none, or stays
    /// display:none while what they inherit moves. The render side decided it, and notes the descendants cleared as
    /// the host takes the installation.
    pub(crate) const CLEAR_HIDDEN_DESCENDANTS: Self = Self(1 << 2);
    /// The element's visibility changes.
    pub(crate) const VISIBILITY_FLIP: Self = Self(1 << 3);
    /// The element's custom-property environment moves.
    pub(crate) const CUSTOM_PROPERTIES: Self = Self(1 << 4);
    /// The element registers or unregisters anchor names.
    pub(crate) const ANCHOR_NAMES: Self = Self(1 << 5);
    /// The root element's font metrics may move.
    pub(crate) const ROOT_FONT_METRICS: Self = Self(1 << 6);
    /// Children waiting for the element to leave display:none may be revealed.
    pub(crate) const REVEAL_WAITING: Self = Self(1 << 7);
    /// The resources an SVG graphics element references.
    pub(crate) const SVG_REFERENCES: Self = Self(1 << 8);
    /// The timelines the element declares by name change.
    pub(crate) const TIMELINES: Self = Self(1 << 9);
    /// A form control restyles its shadow tree.
    pub(crate) const FORM_CONTROL: Self = Self(1 << 10);
    /// Scroll containers re-snap, or the element's box registers as a scroll snap container or area.
    pub(crate) const SCROLL_SNAP: Self = Self(1 << 11);
    /// The layout tree is built again from the root the effects name.
    pub(crate) const REBUILD: Self = Self(1 << 12);
    /// The container queries the record read note their containers and the element's dependencies.
    pub(crate) const CONTAINER_EFFECTS: Self = Self(1 << 13);
    /// The boxes of the element or its pseudo-elements load or let go of images.
    pub(crate) const IMAGES: Self = Self(1 << 14);
    /// The element's highlight pseudo-element styles are pushed to paint.
    pub(crate) const HIGHLIGHTS: Self = Self(1 << 15);
    /// The element's box is not a plain one: it takes facts from its element as the host styles it.
    pub(crate) const HOST_BOX_FACTS: Self = Self(1 << 16);
    /// The element owes the plan of its animations, which the host applies before it composes them.
    pub(crate) const ANIMATION_PLAN: Self = Self(1 << 17);
    /// The element owes the transition step, which the host runs against the record it moves from once it applied the
    /// plan.
    pub(crate) const TRANSITION_STEP: Self = Self(1 << 18);

    pub(crate) fn with(self, other: Self, condition: bool) -> Self {
        if condition { self | other } else { self }
    }

    /// These effects, but none of `other`.
    pub(crate) fn without(self, other: Self) -> Self {
        Self(self.0 & !other.0)
    }

    /// Whether any of the effects of `other` is one of these.
    pub(crate) fn intersects(self, other: Self) -> bool {
        self.0 & other.0 != 0
    }
}

/// Where a layout tree rebuild starts, as the host's `CSS::LayoutTreeRebuildRoot` names it, ordered by how much of the
/// tree it rebuilds.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord)]
#[repr(transparent)]
pub struct LayoutTreeRebuildRoot(u8);

impl LayoutTreeRebuildRoot {
    /// Recreate ::before and ::after while retaining the principal box where its structure permits it.
    pub(crate) const PSEUDO_ELEMENTS: Self = Self(0);
    pub(crate) const SELF: Self = Self(1);
    pub(crate) const SELF_UNLESS_DOCUMENT_ELEMENT_OR_BODY: Self = Self(2);
    pub(crate) const PARENT: Self = Self(4);

    pub(crate) const fn from_raw(raw: u8) -> Self {
        Self(raw)
    }
}

impl std::ops::BitOr for MainEffects {
    type Output = Self;
    fn bitor(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
}

impl std::ops::BitOrAssign for MainEffects {
    fn bitor_assign(&mut self, other: Self) {
        self.0 |= other.0;
    }
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
    pub const IS_SVG_RESOURCE_ELEMENT: u32 = 1 << 7;
    pub const IS_SVG_GEOMETRY_ELEMENT: u32 = 1 << 8;
    pub const IS_HTML_SELECT_ELEMENT: u32 = 1 << 9;
    /// The layout algorithm a MathML element's tag name selects, as a `MathLayoutKind` in these bits.
    pub const MATHML_LAYOUT_KIND_SHIFT: u32 = 16;
    pub const MATHML_LAYOUT_KIND_MASK: u32 = 0x1f << MATHML_LAYOUT_KIND_SHIFT;
}

/// Which principal box an element asks for before its computed style has a say. The element's own
/// type and state decide this; the tree build resolves it against the computed `display` and
/// `appearance`. Mirrors the C++ `CSS::ElementBoxKind`, which lists the same kinds in the same
/// order and crosses the boundary as its underlying byte.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, Default)]
#[repr(u8)]
#[expect(dead_code, reason = "C++ constructs the variants")]
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
}

/// Which element the values of a replaced content input are of. See
/// `inputs::ReplacedContentInput`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
#[expect(dead_code, reason = "C++ constructs the variants")]
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
#[expect(dead_code, reason = "C++ constructs the variants")]
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
#[expect(dead_code, reason = "C++ constructs the variants")]
pub enum FfiFeatureValueKind {
    Absent = 0,
    /// Present, but the payload is not internable; an exact test reads the live DOM.
    Present = 1,
    Atom = 2,
    /// Present, with a value that differs from the one the fact held before. Attribute values do
    /// not cross as text - an exact test reads the live DOM - but a change to one is still a
    /// change, and a delta that reported presence on both sides would write the fact the
    /// projection already holds and invalidate nothing.
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
#[expect(dead_code, reason = "C++ constructs the variants")]
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

/// One exact non-selector style reaction for an element, which a test records directly.
#[cfg(test)]
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiElementStyleInput {
    pub style_node: u32,
    pub reaction: u8,
    pub inherited_style_groups: u8,
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

impl super::projection_writes::ProjectionWrites<'_> {
    /// Write the tree delta the host published as `delta`.
    pub(crate) fn record_ffi_tree_delta(&mut self, delta: &FfiTreeDelta) {
        let Some(node) = StyleNodeID::from_raw(delta.node) else {
            return;
        };
        let old = delta.old_connected.then(|| delta.old_relations.decode());
        let new = delta.new_connected.then(|| delta.new_relations.decode());
        self.record_tree_delta(node, old, new);
    }

    /// Write the arrival the host published as `arrival`, whose custom states, classes and attribute pairs come next
    /// in `atoms`, the shared atom column.
    #[cfg(test)]
    pub(crate) fn record_ffi_element_arrival(&mut self, arrival: &FfiElementArrival, atoms: &mut &[u32]) {
        let Some(node) = StyleNodeID::from_raw(arrival.node) else {
            debug_assert!(false, "an element arrival named an invalid style node");
            return;
        };
        let count =
            arrival.custom_state_count as usize + arrival.class_count as usize + 2 * arrival.attribute_count as usize;
        let Some((arrival_atoms, rest)) = atoms.split_at_checked(count) else {
            debug_assert!(false, "an element arrival named atoms outside the shared atom column");
            return;
        };
        *atoms = rest;
        self.record_element_arrival(node, &element_arrival_facts(arrival, arrival_atoms), None);
    }

    /// Write the local fact the host published as `delta`.
    pub(crate) fn record_ffi_local_feature(&mut self, delta: &FfiLocalFeatureDelta) {
        let Some(node) = StyleNodeID::from_raw(delta.node) else {
            return;
        };
        let arriving = self.projection.arrival_is_pending(node);
        self.record_feature(
            node,
            decode_feature_key(delta),
            decode_feature_value(delta.new_kind, delta.new_atom),
            arriving,
        );
    }

    /// Write the state the host published as `delta`.
    pub(crate) fn record_ffi_state(&mut self, delta: &FfiStateDelta) {
        let Some(node) = StyleNodeID::from_raw(delta.node) else {
            return;
        };
        let arriving = self.projection.arrival_is_pending(node);
        self.record_state(node, decode_state_fact(delta.fact), delta.new_value, arriving);
    }
}

fn decode_element_declaration_kind(kind: FfiElementDeclarationKind) -> ElementDeclarationKind {
    match kind {
        FfiElementDeclarationKind::InlineStyle => ElementDeclarationKind::InlineStyle,
        FfiElementDeclarationKind::PresentationalHint => ElementDeclarationKind::PresentationalHint,
        FfiElementDeclarationKind::SvgPresentationAttribute => ElementDeclarationKind::SvgPresentationAttribute,
    }
}

impl StyleEngine {
    /// Apply one flat transaction as a test's host would publish it at one barrier: its tree deltas in order, then its
    /// arrivals, local facts and states, then its declarations and style inputs.
    #[cfg(test)]
    #[allow(clippy::too_many_arguments)]
    pub fn apply_transaction_batch(
        &mut self,
        tree_deltas: &[FfiTreeDelta],
        arrival_columns: (&[FfiElementArrival], &[u32]),
        local_feature_deltas: &[FfiLocalFeatureDelta],
        state_deltas: &[FfiStateDelta],
        element_declaration_deltas: &[FfiElementDeclarationDelta],
        element_style_inputs: &[FfiElementStyleInput],
    ) {
        let (element_arrivals, mut arrival_atoms) = arrival_columns;
        self.retained.write_projection_for_test(|writes| {
            for delta in tree_deltas {
                writes.record_ffi_tree_delta(delta);
            }
            for arrival in element_arrivals {
                writes.record_ffi_element_arrival(arrival, &mut arrival_atoms);
            }
            for delta in local_feature_deltas {
                writes.record_ffi_local_feature(delta);
            }
            for delta in state_deltas {
                writes.record_ffi_state(delta);
            }
        });
        for arrival in element_arrivals {
            if let Some(node) = StyleNodeID::from_raw(arrival.node)
                && arrival.associated_pseudo_kind_plus_one != 0
            {
                self.retained.note_backing_element(node);
            }
        }
        // As the adoption of a host's publication does, the hover's chain follows the tree where it moved.
        if !tree_deltas.is_empty() {
            self.set_hover(self.hover_target());
        }
        self.record_element_declaration_deltas(element_declaration_deltas);
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

    /// Record the declaration blocks the host published elements to hold, or no longer to hold.
    pub(crate) fn record_element_declaration_deltas(
        &mut self,
        element_declaration_deltas: &[FfiElementDeclarationDelta],
    ) {
        for delta in element_declaration_deltas {
            let Some(node) = StyleNodeID::from_raw(delta.node) else {
                continue;
            };
            // Zero means the node has no declaration block of that kind on that side.
            let block = |raw: u32| (raw != 0).then(|| DeclarationBlockID::new(raw));
            // The block's contents moved even where the host's object for it did not, so what makes
            // this a change is a fresh version of the block, which is minted here rather than by the
            // host: the host says only that the node has one.
            let new_block =
                (delta.new_block != 0).then(|| DeclarationBlockID::new(self.retained.next_declaration_block_version()));
            self.record_input(
                InputKey::ElementDeclaration(node, decode_element_declaration_kind(delta.kind)),
                InputValue::ElementDeclaration(block(delta.old_block)),
                InputValue::ElementDeclaration(new_block),
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
    /// The calculations the edges of the attachment range resolve, which the animation retains, or null.
    pub range_start_calculated: *const c_void,
    pub range_end_calculated: *const c_void,
}

/// What an applied animation definition holds by value, its numbers before its bytes, which pack
/// behind them.
#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
pub struct FfiAppliedAnimationValues {
    pub duration: f64,
    pub iteration_count: f64,
    pub delay: f64,
    pub range_start_px: f64,
    pub range_start_percentage: f64,
    pub range_end_px: f64,
    pub range_end_percentage: f64,
    pub duration_is_auto: bool,
    pub direction: u8,
    pub play_state: u8,
    pub fill_mode: u8,
    pub composition: u8,
    /// An `FfiAnimationTimelineKind`.
    pub timeline_kind: u8,
    pub scroll_scroller: u8,
    pub scroll_axis: u8,
    pub range_start_name: u8,
    pub range_end_name: u8,
}

const _: () = assert!(size_of::<FfiAppliedAnimationValues>() == 72);

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
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_set_tree_scope_animation_keyframes(
    host: &DocumentHost,
    tree_scope: u32,
    name_lengths: ak::Span<'_, u32>,
    name_units: ak::Span<'_, u16>,
    keyframe_sets: ak::Span<'_, usize>,
) {
    assert_eq!(
        name_lengths.as_slice().len(),
        keyframe_sets.as_slice().len(),
        "each keyframes name has a set"
    );
    let row = super::animations::AnimationKeyframes::row(
        name_lengths.as_slice(),
        name_units.as_slice(),
        keyframe_sets.as_slice(),
    );
    let write = super::engine_calls::EngineWrite::AnimationKeyframes {
        tree_scope: TreeScopeID(tree_scope),
        row,
    };
    super::engine_calls::queue(host, write);
}

/// Replaces the `@position-try` row of one style scope: each name the scope defines, packed into one buffer of code
/// units with a length each, with the declarations of the rule that name resolves to. An empty row gives the scope's
/// row up.
///
/// # Safety
/// Each declaration pointer must be a live `SharedDeclarationBlock` the call borrows.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_tree_scope_position_try_rules(
    host: &DocumentHost,
    tree_scope: u32,
    name_lengths: ak::Span<'_, u32>,
    name_units: ak::Span<'_, u16>,
    declarations: ak::Span<'_, *const c_void>,
) {
    assert_eq!(
        name_lengths.as_slice().len(),
        declarations.as_slice().len(),
        "each @position-try name has declarations"
    );
    // SAFETY: Guaranteed by the caller.
    let row = unsafe {
        super::position_try::PositionTryRules::row(
            name_lengths.as_slice(),
            name_units.as_slice(),
            declarations.as_slice(),
        )
    };
    let write = super::engine_calls::EngineWrite::PositionTryRules {
        tree_scope: TreeScopeID(tree_scope),
        row,
    };
    super::engine_calls::queue(host, write);
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
    host: &DocumentHost,
    identity: u64,
    store: *const c_void,
    inheritable: u64,
    inheritable_store: *const c_void,
) {
    use super::custom_property_environments::retained_store;
    let write = super::engine_calls::EngineWrite::NoteCustomPropertyEnvironment {
        identity,
        // SAFETY: Guaranteed by the caller.
        store: unsafe { retained_store(store) },
        inheritable,
        // SAFETY: Guaranteed by the caller.
        inheritable_store: unsafe { retained_store(inheritable_store) },
    };
    super::engine_calls::queue(host, write);
}

/// [`style_engine_note_custom_property_environment`] on `engine`.
pub fn note_custom_property_environment(
    engine: &mut StyleEngine,
    identity: u64,
    store: Option<&std::sync::Arc<crate::css::custom_properties::CustomPropertyStore>>,
    inheritable: u64,
    inheritable_store: Option<&std::sync::Arc<crate::css::custom_properties::CustomPropertyStore>>,
) {
    engine.custom_property_environments.retain(identity, store);
    engine
        .custom_property_environments
        .note_inheritable(identity, inheritable, inheritable_store);
}

/// The environment a child inherits from one the engine resolved: itself, unless a registration
/// keeps some of its custom properties from inheriting.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_inheritable_custom_property_environment(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    identity: u64,
) -> u64 {
    with_engine(read, host, |engine| {
        engine.custom_property_environments.inheritable(identity)
    })
}

#[derive(Clone, Default)]
pub(crate) struct BoundScopeChain {
    pub(crate) roots: Vec<crate::css::selector::SelectorRef>,
    pub(crate) limits: Vec<crate::css::selector::SelectorRef>,
    /// Whether each of `limits` names `&` or `:scope`, anywhere in it.
    pub(crate) limits_name_the_root: Vec<bool>,
    pub(crate) levels: Vec<(u32, u32)>,
    pub(crate) implicit_roots: Vec<Option<ImplicitScopeRoot>>,
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
            self.roots.extend(start.selectors.owned());
        }
        if let Some(end) = end {
            self.limits.extend(end.selectors.owned());
            self.limits_name_the_root.extend(end.selectors.owned().map(|limit| {
                let tree = limit.view().to_tree();
                crate::css::selector_operations::contains_nesting(&tree)
                    || crate::css::selector_operations::contains_pseudo_class(
                        &tree,
                        crate::css::selector::PseudoClassType::Scope,
                    )
            }));
        }
    }
}

fn collect_native_custom_declarations(
    engine: &mut StyleEngine,
    custom_properties: crate::css::declaration_block::CustomProperties<'_>,
) -> Vec<(CustomDeclaration, RetainedStyleValueData)> {
    custom_properties
        .iter()
        .map(|property| {
            let name = property.name.to_fly_string();
            let atom = engine.atoms.intern_raw(name.raw());
            // SAFETY: The fly string is live for the call, and the engine takes a reference of its own.
            unsafe { engine.note_custom_property_name(atom, name.raw()) };
            let declaration = &property.declaration;
            // Custom properties retain their authored values. Their token spelling is observable
            // after substitution.
            let written = RetainedStyleValueData::from_arc(declaration.value.clone());
            let declared = CustomDeclaration::new(atom, declaration.important, declaration.operator);
            (declared, written)
        })
        .collect()
}

pub(super) fn register_element_declared_properties<'a>(
    engine: &mut StyleEngine,
    node: StyleNodeID,
    kind: FfiElementDeclarationKind,
    declarations: impl ExactSizeIterator<Item = crate::css::declaration_block::DeclarationRef<'a>>,
) {
    let declarations = super::specified_value::element_declared_properties(declarations);
    engine.set_element_declared_properties(node, decode_element_declaration_kind(kind), declarations);
    // The engine holds the declarations as they were published.
}

/// Records the custom properties an element's inline declaration block declares.
pub(super) fn register_element_custom_properties(
    engine: &mut StyleEngine,
    node: StyleNodeID,
    custom_properties: crate::css::declaration_block::CustomProperties<'_>,
) {
    let custom_declarations = collect_native_custom_declarations(engine, custom_properties);
    engine.element_declarations.set_custom(node, custom_declarations);
}

/// Whether `declarations` can define transitions.
pub(crate) fn declares_transitions<'a>(
    declarations: impl IntoIterator<Item = crate::css::declaration_block::DeclarationRef<'a>>,
) -> bool {
    use crate::css::property_metadata::property_defines_a_css_transition;
    declarations
        .into_iter()
        .any(|declaration| property_defines_a_css_transition(declaration.property_id))
}

/// Registers an element's native inline declaration block, or clears it when null.
/// Returns whether the declarations can define transitions.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_set_element_inline_style_properties(
    host: &DocumentHost,
    node: u32,
    block: Option<&crate::css::declaration_block::DeclarationBlock>,
) -> bool {
    use crate::css::declaration_block::DeclarationBlock;
    let Some(node) = StyleNodeID::from_raw(node) else {
        return false;
    };
    let data = block.map(DeclarationBlock::data).filter(|data| !data.is_empty());
    let has_transitions = data.as_ref().is_some_and(|data| data.declares_transitions());
    if let Some(data) = &data
        && data.reads_attributes()
    {
        super::host_atoms::note_attr_names_read_by_block(host, data);
    }
    let declares_custom = data.as_ref().is_some_and(|data| !data.custom_properties().is_empty());
    let previous = host
        .engine_memo()
        .projection
        .borrow_mut()
        .write_inline_declarations(node, data.clone());
    if declares_custom || previous.is_some_and(|previous| !previous.custom_properties().is_empty()) {
        super::engine_calls::queue(
            host,
            super::engine_calls::EngineWrite::InlineCustomProperties { node, data },
        );
    }
    has_transitions
}

/// Registers borrowed presentation hints and returns whether they can define transitions.
///
/// # Safety
/// `host` must be a live document host, on its document's thread. `properties` must borrow `count`
/// `FfiDeclaredProperty` entries whose values point at live, Arc-backed `StyleValueData` roots.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_set_element_presentational_hint_properties(
    host: &DocumentHost,
    node: u32,
    kind: FfiElementDeclarationKind,
    properties: *const c_void,
    count: usize,
) -> bool {
    use crate::css::declaration_block::{FfiDeclaredProperty, declaration_from_view};
    let Some(node) = StyleNodeID::from_raw(node) else {
        return false;
    };
    let properties = if count == 0 {
        &[]
    } else {
        // SAFETY: Guaranteed by the caller.
        unsafe { ak::slice_from_raw_parts(properties.cast::<FfiDeclaredProperty>(), count) }
    };
    // A hint may name a shorthand. Expanded as any declaration block is, it declares every longhand it decides, and so
    // do all declarations the engine is given.
    let mut hints = crate::css::declaration_block::DeclarationBlockBuilder::default();
    for property in properties {
        // SAFETY: Guaranteed by the caller.
        hints.append_in_specified_order(unsafe { declaration_from_view(property) });
    }
    let has_transitions = declares_transitions(
        hints
            .properties
            .iter()
            .map(crate::css::declaration_block::DeclaredProperty::borrowed),
    );
    super::engine_calls::queue(
        host,
        super::engine_calls::EngineWrite::PresentationalHints {
            node,
            kind,
            properties: hints.properties.into_boxed_slice(),
        },
    );
    has_transitions
}

/// Installs the record of a style the host made outside the engine, as for a layout node, holding
/// `payloads`, every computed group, with the inherited bits `inherited_bits` and the effective
/// zoom's bits `effective_zoom_bits` beside the inherited ones, and answers its identity. Nothing holds the record yet: the
/// host pins it before the engine sweeps its records again.
///
/// # Safety
/// `host` must be a live document host, on its document's thread. `payloads` must point at
/// `count` readable group payloads, which must remain live for this call. `source_record` must
/// be null for an anonymous layout style or be a style record the caller holds, which the
/// installed record derives from.
/// `custom_property_store` must be null or the live store `custom_property_environment` resolves
/// to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_install_host_style_record(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    payloads: *const *const c_void,
    count: usize,
    inherited_bits: u32,
    effective_zoom_bits: u32,
    reset_bits: u32,
    custom_property_environment: u64,
    counter_style_environment_identity: u64,
    source_record: *const c_void,
    custom_property_store: *const c_void,
) -> u64 {
    assert!(!payloads.is_null() && count == crate::css::table_group_builder::group_index::COUNT);
    let bits = crate::css::computed_value_types::StyleBits {
        inherited: crate::css::computed_value_types::InheritedSetValues {
            bits: crate::css::computed_value_types::InheritedBits(inherited_bits),
            effective_zoom_bits,
        },
        reset: crate::css::computed_value_types::ResetBits(reset_bits),
    };
    // SAFETY: Guaranteed by the caller.
    let payloads =
        &unsafe { crate::css::computed_values::StyleGroups::retained(ak::slice_from_raw_parts(payloads, count), bits) };
    // SAFETY: Guaranteed by the caller.
    let source_record = unsafe { source_record.cast::<super::pipeline::Record>().as_ref() };
    with_engine(read, host, |engine| {
        let record = match source_record {
            Some(source) => engine.install_record_derived(source, payloads, Some(counter_style_environment_identity)),
            None => engine.install_initial_derived_record(payloads, counter_style_environment_identity),
        };
        // The store is what the environment resolves to; an engine-computed environment builds on it.
        // SAFETY: Guaranteed by the caller.
        unsafe {
            engine
                .custom_property_environments
                .retain_raw(custom_property_environment, custom_property_store);
        }
        record.raw()
    })
}

/// The record of the document's own style, whose computed values are `values`, with the flags
/// and dependency flags its computation settled, which no element holds and which holds the
/// default groups until [`intern_document_style`] builds its own. Answers one reference to it.
pub(crate) fn document_style_record(
    values: crate::css::computed_values::GroupValues<'_>,
    flags: crate::css::computed_longhand_table::LonghandTableFlags,
    dependency_flags: u8,
    defaults: &crate::css::computed_values::DefaultGroups,
) -> *const c_void {
    let record = super::pipeline::Record::from_values(values, flags, dependency_flags, defaults);
    std::sync::Arc::into_raw(std::sync::Arc::new(record)).cast()
}

/// Builds every style group of the document's own style from the longhands of `record`, which
/// [`document_style_record`] made, against the used color-scheme and `color` they computed, and
/// installs the record no element holds. Answers the record.
///
/// # Safety
/// `record` must be a record `document_style_record` made, which the caller holds. `font` must
/// borrow platform font resources that stay live across the call.
pub(crate) unsafe fn intern_document_style(
    engine: &mut StyleEngine,
    record: *const c_void,
    font: &crate::css::table_group_builder::FfiFontGroupBuildInputs,
    length: &crate::css::style_compute::FfiLengthResolutionContext,
) -> u64 {
    use crate::css::table_group_builder::{
        LonghandSource, assemble_group_from_source, assembly_color_inputs_from_source,
    };
    // SAFETY: Guaranteed by the caller.
    let record = unsafe { &*record.cast::<super::pipeline::Record>() };
    let source = LonghandSource::Longhands {
        longhands: super::ViewLonghands::of_record(record),
        animated_overlay: None,
    };
    let color_inputs = assembly_color_inputs_from_source(&source, length);
    // SAFETY: Each build hands over a reference to a payload of its group.
    let payloads = unsafe {
        crate::css::computed_values::StyleGroups::adopt(
            std::array::from_fn(|group| {
                assemble_group_from_source(
                    &source,
                    group,
                    Some(font),
                    crate::css::computed_values::GroupShares::new(engine.default_groups(), group, None),
                    color_inputs,
                    length,
                )
            }),
            source.style_bits(),
        )
    };
    let record = engine.install_record_derived(record, &payloads, None);
    record.raw()
}

/// What the winners a node's records were computed from read beyond their cascade, one bit per
/// kind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiNodeRecordReads {
    /// An `attr()` substitution, in the element's winners, its pseudo-elements' or the custom
    /// properties it declares: an attribute change reaches the records.
    Attributes = 1 << 0,
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

/// Interns the UTF-16 name `units` in `engine` as a native style atom.
pub(crate) fn intern_native_text(engine: &mut StyleEngine, units: &[u16]) -> StyleAtomID {
    let name = ak::Utf16FlyString::from_utf16(units);
    engine.atoms.intern_raw(name.raw_identity())
}

/// Takes the pending style update under `root` and answers its rows.
///
/// # Safety
/// The host lends the buffers `computation_inputs` names for this call.
pub(crate) unsafe fn run_style_update(
    engine: &mut StyleEngine,
    root: StyleNodeID,
    mut computation_inputs: FfiDocumentStyleComputationInputs,
) -> FfiStyleUpdateOutput {
    // SAFETY: Guaranteed by the caller.
    unsafe { engine.document_resource_contexts.take_in(&mut computation_inputs) };
    // SAFETY: The host lends the media environment the inputs name for this call.
    unsafe { engine.document_media.take_in(&mut computation_inputs) };
    // What a call from each scope reaches is decided under the media the inputs moved first.
    let mut functions = std::mem::take(&mut engine.document_functions);
    // SAFETY: The host lends the custom functions the inputs name for this call.
    unsafe {
        functions.take_in(&mut computation_inputs, &engine.document_media, |scope, layer| {
            engine.layer_index(scope, layer)
        });
    }
    engine.document_functions = functions;
    engine.custom_property_registrations_changed = engine
        .document_style_computation_inputs
        .custom_property_registration_generation
        != computation_inputs.custom_property_registration_generation;
    if engine.custom_property_registrations_changed {
        let previous_registry = engine.custom_property_registry.clone();
        let previous_registry = previous_registry
            .as_deref()
            .unwrap_or_else(|| CustomPropertyRegistry::shared_empty());
        engine
            .animation_effect_descriptions
            .registrations_changed(previous_registry, computation_inputs.custom_property_registry());
        engine
            .host
            .program_staging
            .note_registry_moved(previous_registry, computation_inputs.custom_property_registry());
    }
    // SAFETY: The inputs name the document's live registry, or none.
    engine.custom_property_registry = unsafe {
        crate::css::custom_properties::retain_custom_property_registry(
            computation_inputs.custom_property_registry.as_pointer(),
        )
    };
    engine.document_style_computation_inputs = computation_inputs;
    let mut output = FfiStyleUpdateOutput::default();
    engine.run_style_update(root, |answers| {
        assert!(output.answers.is_empty(), "a style update emitted more than one batch");
        output.answers.extend_from_slice(answers);
    });
    output.reclaimed_style_atoms = std::mem::take(&mut engine.host.reclaimed_style_atoms);
    output
}

impl FfiStyleUpdateOutput {
    /// The rows the transaction answered.
    pub(crate) fn answers(&self) -> &[FfiStyleDelta] {
        &self.answers
    }

    /// The atoms the engine reclaimed as it took the transaction.
    pub(crate) fn reclaimed_style_atoms(&self) -> &[super::atoms::ReclaimedStyleAtom] {
        &self.reclaimed_style_atoms
    }
}

pub(crate) unsafe fn borrow<'a, T>(pointer: *const T, count: usize) -> &'a [T] {
    if count == 0 {
        return &[];
    }
    assert!(!pointer.is_null(), "a non-empty delta array must not be null");
    unsafe { ak::slice_from_raw_parts(pointer, count) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::style::index::AttributeFact;
    use crate::css::style::instrumentation::Counter;
    use crate::css::style::pipeline::projection::ElementChange;

    fn arrived(engine: &StyleEngine, node: StyleNodeID) -> bool {
        engine
            .projection
            .changes()
            .iter()
            .any(|&(changed, change)| changed == node && matches!(change, ElementChange::Relations(None, Some(_))))
    }

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

        let transaction = engine.take_transaction();
        assert_eq!(
            engine.projection.preorder(root).collect::<Vec<_>>(),
            vec![root, child, grandchild]
        );
        assert!(transaction.has_no_element_inputs());
        assert!([root, child, grandchild].into_iter().all(|node| arrived(&engine, node)));
        engine.release_transaction(transaction);

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
        let later = StyleNodeID::from_raw(nodes[3]).unwrap();
        assert_eq!(
            engine
                .projection
                .changes()
                .iter()
                .map(|&(node, _)| node)
                .collect::<Vec<_>>(),
            vec![child, later]
        );
        assert!(arrived(&engine, later));
        engine.release_transaction(transaction);
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
                // Settling the tree between batches must not lose a row's before side.
                engine.settle_tree_at_barrier();
            }
            let transaction = engine.take_transaction();
            for raw in &nodes[1..] {
                assert!(arrived(&engine, StyleNodeID::from_raw(*raw).unwrap()));
            }
            assert_eq!(
                engine.projection.preorder(root).collect::<Vec<_>>(),
                nodes.map(|raw| StyleNodeID::from_raw(raw).unwrap())
            );
            engine.release_transaction(transaction);
        }
    }

    #[test]
    fn element_arrival_rows_install_intrinsic_facts() {
        assert_eq!(size_of::<FfiElementArrival>(), 64);
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
                tag_atom: 41,
                folded_tag_atom: 42,
                namespace_atom: 11,
                language_atom: 12,
                directionality_atom: 13,
                custom_state_count: 2,
                class_count: 2,
                attribute_count: 1,
                heading_level: 4,
                is_slot: true,
                states: 1 << StateFact::Checked as u32,
                ..arrival_for(0, 0)
            },
            FfiElementArrival {
                node: nodes[1],
                tag_atom: 51,
                folded_tag_atom: 51,
                namespace_atom: 21,
                language_atom: 22,
                directionality_atom: 23,
                custom_state_count: 1,
                has_text_content: true,
                ..arrival_for(0, 0)
            },
        ];
        engine.apply_transaction_batch(&tree, (&arrivals, &[31, 32, 62, 61, 71, 72, 33]), &[], &[], &[], &[]);
        let transaction = engine.take_transaction();

        let root = StyleNodeID::from_raw(nodes[0]).unwrap();
        let child = StyleNodeID::from_raw(nodes[1]).unwrap();
        let root_facts = engine.projection.facts(root).unwrap();
        assert_eq!(root_facts.namespace(), StyleAtomID(11));
        assert_eq!(root_facts.language(), StyleAtomID(12));
        assert_eq!(root_facts.directionality(), StyleAtomID(13));
        assert_eq!(root_facts.heading_level(), 4);
        assert!(root_facts.is_slot());
        assert_eq!(root_facts.custom_states(), &[StyleAtomID(31), StyleAtomID(32)]);
        assert_eq!(root_facts.tag(), StyleAtomID(41));
        assert_eq!(root_facts.folded_tag(), StyleAtomID(42));
        assert_eq!(root_facts.classes(), &[StyleAtomID(61), StyleAtomID(62)]);
        assert_eq!(
            root_facts.attributes(),
            &[AttributeFact {
                name: StyleAtomID(71),
                value: StyleAtomID(72),
            }]
        );
        assert!(root_facts.states().contains(StateFact::Checked));
        assert!(!root_facts.has_text_content());
        let child_facts = engine.projection.facts(child).unwrap();
        assert_eq!(child_facts.namespace(), StyleAtomID(21));
        assert_eq!(child_facts.custom_states(), &[StyleAtomID(33)]);
        assert_eq!(child_facts.folded_tag(), StyleAtomID::NONE);
        assert!(child_facts.classes().is_empty());
        assert!(child_facts.has_text_content());
        engine.release_transaction(transaction);
    }

    fn arrival_for(node: u32, custom_state_count: u32) -> FfiElementArrival {
        FfiElementArrival {
            states: 0,
            node,
            tag_atom: 4,
            folded_tag_atom: 4,
            id_atom: 0,
            namespace_atom: 1,
            language_atom: 2,
            directionality_atom: 3,
            custom_state_count,
            class_count: 0,
            attribute_count: 0,
            adjustment_facts: 0,
            construction_facts: 0,
            heading_level: 0,
            is_slot: false,
            has_text_content: false,
            associated_pseudo_kind_plus_one: 0,
            box_kind: ElementBoxKind::FromDisplay,
        }
    }

    #[test]
    #[should_panic(expected = "an element arrival named an invalid style node")]
    fn malformed_element_arrival_rejects_an_invalid_node() {
        let mut engine = StyleEngine::new();
        engine.apply_transaction_batch(&[], (&[arrival_for(0, 0)], &[]), &[], &[], &[], &[]);
    }

    #[test]
    #[should_panic(expected = "an element arrival named atoms outside the shared atom column")]
    fn malformed_element_arrival_rejects_an_out_of_bounds_atom_range() {
        let mut engine = StyleEngine::new();
        let mut nodes = [0];
        engine.allocate_style_nodes(&mut nodes);
        let arrival = FfiElementArrival {
            attribute_count: 1,
            ..arrival_for(nodes[0], 0)
        };
        engine.apply_transaction_batch(&[], (&[arrival], &[1]), &[], &[], &[], &[]);
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
            reaction: crate::css::style::input_types::STYLE_REACTION_RECOMPUTE_STYLE,
            inherited_style_groups: 0,
        }];
        engine.apply_transaction_batch(&tree, (&[], &[]), &features, &states, &declarations, &style_inputs);

        let transaction = engine.take_transaction();
        let node0 = StyleNodeID::from_raw(nodes[0]).unwrap();
        let node1 = StyleNodeID::from_raw(nodes[1]).unwrap();
        let node2 = StyleNodeID::from_raw(nodes[2]).unwrap();
        assert_eq!(
            engine.projection.children(node0).collect::<Vec<_>>(),
            vec![node1, node2]
        );
        let changes = engine.projection.changes();
        assert!(
            changes
                .iter()
                .any(|(_, change)| matches!(change, ElementChange::Relations(..)))
        );
        assert!(
            changes
                .iter()
                .any(|(_, change)| matches!(change, ElementChange::Feature(_) | ElementChange::ArrivingFacts))
        );
        assert!(
            changes
                .iter()
                .any(|(_, change)| matches!(change, ElementChange::State(_) | ElementChange::ArrivingFacts))
        );
        assert_eq!(transaction.declarations, vec![node1]);
        assert_eq!(
            transaction
                .style_inputs
                .iter()
                .map(|&(node, _)| node)
                .collect::<Vec<_>>(),
            vec![node1]
        );
        engine.release_transaction(transaction);

        assert_eq!(engine.counters().get(Counter::TreeDeltas), 4);
        assert_eq!(engine.counters().get(Counter::LocalFeatureDeltas), 1);
        assert_eq!(engine.counters().get(Counter::StateDeltas), 1);
        assert_eq!(engine.counters().get(Counter::ElementDeclarationDeltas), 1);
    }

    #[test]
    fn identities_are_minted_in_one_call_per_batch() {
        let mut engine = StyleEngine::new();
        let mut nodes = [0_u32; 512];
        engine.allocate_style_nodes(&mut nodes);
        assert_eq!(engine.counters().get(Counter::StyleNodesAllocated), 512);
        assert!(nodes.windows(2).all(|pair| pair[0] < pair[1]));
    }
}
