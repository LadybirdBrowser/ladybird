/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! StyleEngine: selector evaluation, cascade, and computed-value construction modelled as a
//! memory-bounded incremental computation.
//!
//! DOM structure, element state, stylesheets, CSSOM mutations, cascade topology, and environment
//! values are versioned inputs. A style flush propagates exact changes through selectors, cascade
//! decisions, and computed values, and stops as soon as a semantic value is unchanged. Derived state
//! lives as long as the node or rule it describes.
//!
//! Everything between a style input and a computed value is authoritative here, on the Rust side.
//! C++ remains authoritative for DOM and CSSOM object identity, mutation semantics, document
//! lifecycle, loading order, style-observation barriers, layout, and paint - and holds no second
//! copy of the state this engine owns.

macro_rules! define_id {
    ($(#[$attribute:meta])* $visibility:vis struct $name:ident();) => {
        $(#[$attribute])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
        $visibility struct $name(u32);
    };
    ($(#[$attribute:meta])* $visibility:vis struct $name:ident($field_visibility:vis);) => {
        $(#[$attribute])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
        $visibility struct $name($field_visibility u32);
    };
    ($(#[$attribute:meta])* default $visibility:vis struct $name:ident();) => {
        $(#[$attribute])*
        #[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
        $visibility struct $name(u32);
    };
    ($(#[$attribute:meta])* default $visibility:vis struct $name:ident($field_visibility:vis);) => {
        $(#[$attribute])*
        #[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
        $visibility struct $name($field_visibility u32);
    };
}

pub(crate) mod animations;
mod atoms;
pub(crate) mod boundary;
pub mod bridge;
pub mod cascade;
mod column;
pub mod compiler;
pub(crate) mod component_matching;
mod computed;
mod container_queries;
pub(in crate::css) mod kept_past_release;
pub(crate) mod selector_analysis;
pub(crate) use container_queries::ContainerUnitBases;
mod custom_property_cascade;
mod custom_property_environments;
mod dom_reads;
pub(crate) mod drag_selection;
pub(crate) mod effect_descriptions;
mod element_declarations;
mod element_host_facts;
mod engine;
pub mod engine_calls;
pub(crate) mod engine_sample;
#[cfg(test)]
pub mod exact_matcher;
pub(crate) mod host_atoms;
pub(crate) mod style_job;
pub(crate) use crate::fast_hash;
mod engine_handle;
mod flush;
mod font_resolution;
pub(crate) mod hover_lane;
pub mod identities;
pub mod index;
mod inputs;
mod installed_records;
pub(crate) use inputs::next_declaration_block_version;
pub mod instrumentation;
pub(crate) mod layout_style;
pub(crate) mod memory_report;
pub mod order;
mod ordering;
pub(crate) mod pointer_key;
pub(crate) mod position_try;
mod process_style;
mod projection_writes;
mod publication;
mod random_bases;
pub mod reaction_application;
mod resource_contexts;
pub(crate) mod rule_writes;
mod style_invalidation;
pub(crate) use style_invalidation::SampleBounds;
pub(crate) mod pipeline;
mod transition_baselines;
pub(crate) use atoms::{AtomLease, AtomLeases, ProcessAtoms, process_atoms};
pub(crate) use computed::{
    ENGINE_INHERITED_GROUP_COUNT, HoverFrameLease, ReleasedAnimationOverlayRecords, StyleRecordLease, StyleRecordView,
    ViewLonghands, names_an_uninstalled_record,
};
pub(crate) use process_style::ProcessStyle;
pub(crate) use publication::RecordDemand;
pub(crate) use transition_baselines::{InheritedAnimatedValue, TransitionBaselines};
pub mod input_types;
pub mod selector;
pub mod selector_evaluation;
mod size_container_invalidation;
mod specified_value;

/// Whether the host has to note what `value` reads of its element's attributes.
pub(crate) fn value_reads_attributes(value: &crate::css::style_value::StyleValueData) -> bool {
    custom_property_cascade::unresolved_reading_attributes(value).is_some()
}

pub mod tree;
mod weak_pool;

use atoms::DocumentAtoms;
use atoms::ReclaimedStyleAtom;
use column::Column;
use fast_hash::FastMap as HashMap;
use fast_hash::FastSet as HashSet;
use smallvec::SmallVec;
use std::hash::Hash;

use std::sync::Arc;
use std::sync::Mutex;

/// Attribute names a document's selectors read, which its engine shares with its host.
pub(crate) type SelectorAttributeNames = Arc<HashSet<StyleAtomID>>;

/// The style record each pseudo-element of an element holds, by its kind.
type HeldPseudoStyleRecords = crate::fork::ForkShared<HashMap<StyleNodeID, smallvec::SmallVec<[(u8, u64); 1]>>>;

/// What an element's arrival has to carry to reach the anchor of a `:has()` argument: a name whose raw identity is
/// among `names` as its tag, ID, class or attribute name, or a state among `states`. Where an argument can be reached
/// by anything else, every arrival can reach one. The engine shares it with its host, which leaves the subtrees of
/// elements in a display:none subtree out of the engine's tree for as long as nothing could observe them.
#[derive(Clone, Default)]
pub(crate) struct RelationalArrivalKeys {
    keys: HashSet<DispatchKey>,
    pub(crate) names: Vec<usize>,
    pub(crate) states: Vec<u8>,
    pub(crate) reached_by_any_arrival: bool,
}

use crate::css::cascaded_properties::CascadeOrigin;
#[cfg(test)]
use crate::css::cascaded_properties::CascadedPropertyStore;
use crate::css::computed_values::computed_group_output_mask;
use crate::css::host_shared::HostShared;
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::StyleValueData;
use bridge::FfiStyleDelta as PublishedStyleDeltaRecord;
use instrumentation::Counter;
use instrumentation::Counters;

pub use engine_handle::StyleEngineHandle;
pub use inputs::{NaturalSize, PublishedBoxFacts, PublishedTextSource, ReplacedContentInput, TextStyleParentFacts};

use crate::css::declaration_block::DeclarationEntry;
use cascade::CascadeOperator;
use cascade::CascadePriority;
use cascade::ElementAttachment;
use cascade::PriorityInputs;
use cascade::WinnerSource;
use computed::ComputedGroupSets;
use index::DispatchKey;
#[cfg(test)]
use index::FeatureValue;
use index::LocalFeatureKey;
use index::StyleAtomID;
use input_types::ElementDeclarationKind;
use input_types::ElementStyleInput;
use input_types::InputKey;
use input_types::InputValue;
use input_types::PendingElementInputs;
use input_types::StateFact;
use input_types::StyleUpdate;
use input_types::TransactionFactSide;
use input_types::TreeRelations;
use pipeline::rule_store::CascadeLayerID;
use pipeline::rule_store::CustomDeclaration;
use pipeline::rule_store::DeclarationBlockID;
use pipeline::rule_store::ProgramStaging;
use pipeline::rule_store::RuleID;
use pipeline::rule_store::RuleKind;
use pipeline::rule_store::RuleStore;
use pipeline::rule_store::RuleVersion;
use pipeline::rule_store::SheetID;
use pipeline::rule_store::SheetRuleReplacement;
use pipeline::rule_store::StyleScopeID;
use selector::Incomplete;
use selector::MatchScratch;
use selector::Specificity;
use tree::HostTree;
use tree::SegmentedNodeColumn;
use tree::StyleNodeID;
use tree::TreeScopeID;

/// Long-lived engine state: the document, its program, derived results and cross-flush
/// caches. This is the whole read side of an evaluation step; it holds no
/// host handle, no pending input intake and no borrowed FFI result storage.
#[derive(Clone)]
pub struct RetainedState {
    /// What the engine counts of its own decisions.
    counters: Counters,
    /// The elements the host shows each kind of highlight in.
    highlight_observers: pipeline::highlight_observers::HighlightObservers,
    tree: crate::fork::ForkShared<HostTree>,
    /// The depth of the deepest element the tree ever held, which the document's host reads.
    deepest_element: Arc<std::sync::atomic::AtomicU32>,
    /// Where the hover is.
    hover: hover_lane::Hover,
    rules: crate::fork::ForkShared<RuleStore>,
    /// The style data the engine shares with the other engines of the process.
    process_style: Arc<process_style::ProcessStyle>,
    /// The last declaration block version minted, by the engine or by its document's host, which mints without it.
    declaration_block_version: Arc<std::sync::atomic::AtomicU32>,
    /// Latest document-wide scalar computation facts, copied at the transaction boundary. The host
    /// publishes them with every transaction, before it asks for any row.
    document_style_computation_inputs: bridge::FfiDocumentStyleComputationInputs,
    /// The base URLs a `url()` resolves against, copied at the transaction boundary with the inputs.
    document_resource_contexts: resource_contexts::DocumentResourceContexts,
    /// The document's media features, copied from each transaction's inputs.
    document_media: custom_property_cascade::DocumentMediaSnapshot,
    /// The `@function` definitions each scope sees, retained from each transaction's inputs.
    document_functions: custom_property_cascade::DocumentFunctionSnapshot,
    /// The custom-property registry the last transaction's inputs name, which the engine holds a
    /// reference to for as long as it reads the registry: the document makes its registry anew
    /// when the registrations change.
    custom_property_registry: Option<std::sync::Arc<crate::css::custom_properties::CustomPropertyRegistry>>,
    /// Every font resolution this document has been given. An evaluation step reads it; only a
    /// host round between passes adds to it.
    font_resolution: Option<font_resolution::FontResolutionCache>,
    /// The fonts the threads of the style pass under way resolve, where they do.
    pass_fonts: Option<std::sync::Arc<font_resolution::PassFonts>>,
    /// `font-family: monospace`, the family the monospace font-size recascade resolves an
    /// ancestor's font-relative lengths against.
    monospace_font_family: RetainedStyleValueData,

    /// The shared computed-group payload tuple last published for each live element. This is the
    /// computed half of the eventual base style record; custom properties and metadata remain
    /// separate inputs until that record is complete.
    computed_group_sets: crate::fork::ForkShared<ComputedGroupSets>,
    /// How many epochs of style record views the host has open. No computed record is reclaimed while one is.
    style_record_view_epoch_depth: u32,
    /// The stores behind the custom-property environments live records are published with.
    custom_property_environments: crate::fork::ForkShared<custom_property_environments::CustomPropertyEnvironments>,
    /// The custom-property environment each element holds, for the elements that hold one. This is
    /// the only copy: the element reads its environment from here.
    element_custom_property_data: crate::fork::ForkShared<HashMap<StyleNodeID, inputs::HeldCustomPropertyEnvironment>>,
    /// The custom-property environments of each element's synthetic pseudo-elements, by
    /// pseudo-element kind, for the elements with a pseudo-element that holds one.
    pseudo_element_custom_property_data:
        crate::fork::ForkShared<HashMap<StyleNodeID, Vec<(u8, inputs::HeldCustomPropertyEnvironment)>>>,
    /// The elements whose animations' keyframes say `inherit` for a property that does not
    /// inherit, as the host last sampled them: they read their parent's values of properties that
    /// do not inherit, whether or not their records do.
    keyframes_reading_parent_reset_values: crate::fork::ForkShared<HashSet<StyleNodeID>>,
    /// What the container conditions of the rows the engine answered read of their containers,
    /// per element, taken when the host installs the element's record.
    container_effects_for_host: container_queries::ContainerEffectsForHost,
    /// What the last layout commit and scroll state say of each container's box, for the
    /// container queries that ask about it: a size container's, and a scroll-state container's.
    layout_style_snapshots: HashMap<StyleNodeID, crate::layout::style_snapshot::LayoutStyleSnapshotRow>,
    /// What the host learned about size container queries, which finds the elements a size
    /// query container's new box moves.
    size_container_queries: size_container_invalidation::SizeContainerQueryFacts,
    /// The counter-style registry each tree scope's style scope has, as one identity per scope:
    /// what a record whose `list-style-type` names a counter style the registry may define names,
    /// so that an edit to `@counter-style` moves it.
    counter_style_environment_identities: crate::fork::ForkShared<HashMap<TreeScopeID, u64>>,
    /// The style record each element holds, for the elements that hold one, as the host reports
    /// every record it installs or clears.
    /// Copied on write in chunks of its own, which a fork shares.
    held_style_records: inputs::HeldStyleRecords,
    /// The style record each pseudo-element holds, by its kind, for the elements whose pseudo-elements
    /// hold one, as the host reports every record it installs or clears.
    held_pseudo_style_records: HeldPseudoStyleRecords,
    /// The records a clock frame shows in place of the ones the host installed, which it lends the published reads
    /// for as long as it runs.
    tick_shown: engine_sample::TickShownRecords,
    /// Per shadow host, the elements of its shadow tree that stood for one of its element-backed
    /// pseudo-elements when the host published them; [`RetainedState::backing_elements`] reads the
    /// ones that still do.
    backing_elements: crate::fork::ForkShared<HashMap<StyleNodeID, SmallVec<[StyleNodeID; 2]>>>,
    /// The names of the CSS animations the host holds for each element, which the computation of
    /// its animation definitions matches them against.
    css_defined_animations: crate::fork::ForkShared<animations::CssDefinedAnimations>,
    /// The `@keyframes` each of the document's style scopes defines, as the host's rule caches
    /// resolved them, which an animation definition's keyframes are resolved from.
    animation_keyframes: crate::fork::ForkShared<animations::AnimationKeyframes>,
    /// The `@position-try` rules each of the document's style scopes defines, as the host's rule caches resolved
    /// them, which the position options of an absolutely positioned box are made from.
    position_try_rules: crate::fork::ForkShared<position_try::PositionTryRules>,
    /// The animation effects the host holds for each element, described for sampling.
    animation_effect_descriptions: crate::fork::ForkShared<effect_descriptions::AnimationEffectDescriptions>,
    /// The font metrics of the record the host holds for the document element, which a `rem` the
    /// host resolves reads, once it holds one.
    held_root_font_inputs: Option<pipeline::compute::RootFontInputs>,
    /// The random base value each random caching key has been given, for the random functions the
    /// document's styles hold.
    random_base_values: random_bases::RandomBaseValues,
    /// What each element that has replaced content gives its natural size, which layout resolves
    /// against the style of the element's box.
    replaced_content_inputs: crate::fork::ForkShared<HashMap<StyleNodeID, inputs::ReplacedContentInput>>,
    /// The before-change style each transition target's transitions are decided against for the
    /// rest of the style stabilization epoch, pinned until the epoch commits.
    transition_baselines: transition_baselines::TransitionBaselines,
    /// Whether the registrations used by this transaction differ from the preceding one. A
    /// previously substituted record must then be computed again, even when its cascade winners did
    /// not move.
    custom_property_registrations_changed: bool,
    /// The record a private demand last answered for each target it was asked about, pinned for
    /// the host to read: no node holds it.
    demand_records: crate::fork::ForkShared<HashMap<computed::ComputedStyleTarget, computed::FinalStyleRecordID>>,

    /// The places of the children of parents with many of them, which sibling-counting functions
    /// read, kept while the tree's DOM order stands.
    sibling_positions: crate::fork::ForkReset<Mutex<publication::SiblingPositionCache>>,
    /// The elements a style() container query asked as containers with a value counting their place among their
    /// siblings. A fork shares them, as too many only walk more. An element's identity used again stays here too.
    containers_counted_by_style_queries: Arc<Mutex<HashSet<StyleNodeID>>>,
    /// What the last cold pass measured.
    cold_pass_report: pipeline::Report,
    /// What the engine keeps of the document it owns, which a fork goes on keeping.
    live_style: Option<Box<pipeline::Live>>,
    /// Whether this is the state the host writes, on which alone the engine starts.
    live_style_may_start: crate::fork::ForkReset<crate::fork::IsOriginal>,
    /// How many cold passes the engine started from, on this state and on every fork of it.
    cold_passes: std::sync::Arc<std::sync::atomic::AtomicU64>,
    /// What the engine did on this state, for tests to read.
    live_counters: pipeline::counters::Counters,
    /// The threads style passes run on, which the process shares where a render state holds the
    /// engine.
    style_layout_pool: Option<std::sync::Arc<crate::style_layout_pool::StyleLayoutPool>>,
    /// The font longhand values the engine substituted, one value per property and content: the
    /// font resolver knows a family and its feature values by their values' identities.
    substituted_font_values: std::sync::Arc<std::sync::Mutex<Vec<(u16, RetainedStyleValueData)>>>,
    /// The longhand properties each element declares itself, by the kind of declaration they came
    /// from. An element-attached declaration beats every rule in its context, so the cascade needs
    /// to know which properties one covers just as it does for a rule.
    element_declarations: crate::fork::ForkShared<element_declarations::ElementDeclarationStore>,
    /// The document as the engine reads it: every element fact and link the host publishes.
    projection: pipeline::projection::Projection,
    /// The attribute names whose value text a selector reads, which the host holds a copy of between jobs.
    attribute_value_text_names: SelectorAttributeNames,
    /// The attribute names a selector tests in any way, which the host holds a copy of between jobs.
    tested_attribute_names: SelectorAttributeNames,
    attribute_value_text_requirements_version: u64,
    /// Whether a selector tests `:local-link`, which the host holds a copy of between jobs.
    selectors_test_local_link: bool,
    selectors_test_validity: bool,
    /// What an arrival has to carry to reach a `:has()` anchor, which the host holds a copy of between jobs.
    relational_arrival_keys: Arc<RelationalArrivalKeys>,
    /// The keys rules added since, which the host learns of once they are resolved.
    unresolved_relational_arrival_keys: HashSet<DispatchKey>,
    /// Moves whenever `relational_arrival_keys` does.
    relational_arrival_keys_version: u64,
    /// The node that owns each style scope, for the scopes that have one. The document's scope has
    /// no node, and a scope with no entry here is treated as the document's - which is the widest
    /// answer and therefore the safe one. Tree-scope identities are minted monotonically within one
    /// document, so the identity indexes the column directly.
    scope_roots: crate::fork::ForkShared<Column<Option<StyleNodeID>>>,
    /// The inverse of `scope_roots`. Departing ordinary elements vastly outnumber departing scope
    /// roots, so retirement must ask this index instead of scanning every historical tree scope.
    scope_by_root: crate::fork::ForkShared<SegmentedNodeColumn<TreeScopeID>>,
    /// Maps names and qualified names to process-global atoms. Selector names and DOM facts use
    /// the same owner, so a class in a stylesheet and a class on an element compare as one integer.
    ///
    /// An attribute in a namespace is published under this as well as under its local name, and a
    /// selector that names the namespace tests it. The owner retains one document reference to each
    /// global identity and releases it when this engine is destroyed.
    atoms: crate::fork::ForkShared<DocumentAtoms>,
    /// The HTML namespace when this is an HTML document, and none otherwise. Some attribute names
    /// compare their values ASCII case-insensitively on an HTML element in an HTML document.
    html_element_namespace: StyleAtomID,
    /// Whether the document matches id and class selectors ASCII case-insensitively, which a
    /// quirks-mode one does. Selectors are then compiled against the lowercase folding of the name,
    /// and the DOM side publishes the folding too, so `.item` and `ITEM` name one atom.
    fold_id_and_class_name_case: bool,
}

#[derive(Clone, Copy)]
struct DeferredElementStyleInput {
    reaction: u8,
    inherited_style_groups: u8,
}

/// Host-facing engine state: C++ ownership and input intake.
/// Never reachable from an evaluation step.
#[derive(Clone)]
pub struct HostState {
    /// The host's synchronous font resolver. A step that misses the cache suspends its element;
    /// the round outside the step calls this and the element is retried.
    font_resolver: Option<font_resolution::FontResolverHost>,
    /// The element inputs the next transaction takes beside the projection's changes.
    element_inputs: PendingElementInputs,
    /// Exact element reactions retained across rootless flushes until a style root can consume them.
    deferred_element_style_inputs: std::collections::BTreeMap<StyleNodeID, DeferredElementStyleInput>,
    /// Whether the deferred element style inputs moved since the document's host last took them.
    deferred_element_style_inputs_moved: bool,
    /// Whether the deferred element style inputs are owed to the next transaction, as opposed to
    /// held back by a flush without a document root.
    deferred_element_style_inputs_are_pending: bool,
    /// The rule edits this transaction staged, with their before and after values, until it is
    /// released.
    program_staging: ProgramStaging,
    sheet_occurrences: HashMap<TreeScopeID, pipeline::rule_store::ScopeSheetOccurrences>,
    /// The old dense rule sequence while one sheet is synchronously reparsed.
    sheet_rule_replacement: Option<SheetRuleReplacement>,
    /// Identities released at transaction settlement. The FFI keeps this batch borrowed until C++
    /// has removed its matching fly-string references and atom-keyed memo entries.
    reclaimed_style_atoms: Vec<ReclaimedStyleAtom>,
    /// Whether the transaction under way leaves its atom sweep to a later one: it runs beside the host, which may name
    /// an atom meanwhile that the sweep would reclaim before the host hears of it.
    defers_atom_sweep: bool,
    /// The records the anonymous boxes an installation styles again derive, by the record each held, its parent's and
    /// the one it takes its column properties from, while an installation runs.
    reinherited_records: Option<HashMap<layout_style::Reinheritance, u64>>,
    /// The style node identities the end of a transaction released since the job began, which the job's end pays the
    /// host to mint again.
    released_style_nodes: Vec<u32>,
    /// What the hover seal keeps between frames.
    hover_seal: pipeline::HoverSealMemo,
}

/// Mutable engine state; operations borrow their instrumentation from the boundary.
#[derive(Clone)]
pub struct StyleEngine {
    pub(super) retained: RetainedState,
    pub(super) host: HostState,
}

impl RetainedState {
    pub(crate) fn deepest_element_depth(&self) -> u32 {
        self.deepest_element.load(std::sync::atomic::Ordering::Relaxed)
    }

    pub(crate) fn started_style_layout_pool(&self) -> Option<std::sync::Arc<rayon::ThreadPool>> {
        self.style_layout_pool.as_ref()?.started()
    }

    /// The groups of default values every computation starting from nothing starts from.
    pub(crate) fn default_groups(&self) -> &crate::css::computed_values::DefaultGroups {
        &self.process_style.default_groups
    }
}

impl std::ops::Deref for StyleEngine {
    type Target = RetainedState;

    fn deref(&self) -> &Self::Target {
        &self.retained
    }
}

impl std::ops::DerefMut for StyleEngine {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.retained
    }
}

#[cfg(test)]
mod tests;

#[cfg(test)]
mod container_query_tests;
