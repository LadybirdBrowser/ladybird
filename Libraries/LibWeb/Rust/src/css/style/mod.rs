/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! StyleEngine: selector evaluation, cascade, and computed-value construction modelled as a
//! memory-bounded incremental computation.
//!
//! DOM structure, element state, stylesheets, CSSOM mutations, cascade topology, and environment
//! values are versioned inputs. Selectors, cascade decisions, and computed values form a shared
//! logical dependency program. A style flush discovers and propagates exact changes through that
//! program and stops as soon as a semantic value is unchanged.
//!
//! Two properties shape every module here. First, memory is a primary constraint: derived state is
//! tiered (see [`memory`]), budgeted, and - above Tier 2 - discardable at any time without affecting
//! correctness. Second, the cache-free exact matcher is part of the architecture rather than a
//! fallback bolted on: optional incremental state may accelerate matching but is never required to
//! answer a style read correctly.
//!
//! Everything between a style input and a computed value is authoritative here, on the Rust side.
//! C++ remains authoritative for DOM and CSSOM object identity, mutation semantics, document
//! lifecycle, loading order, style-observation barriers, layout, and paint - and holds no second
//! copy of the state this engine owns.
//!
//! A few modules exist that the original module sketch did not name, because the concepts they hold
//! did not belong in any of the modules it did name: [`tree`] for style node identity and the
//! relation columns, and [`order`] for cascade order maintenance.

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
pub mod batch_matcher;
pub mod bridge;
mod capacity;
pub mod cascade;
mod catalog;
mod child_reactions;
mod column;
pub mod compiler;
mod computed;
mod container_queries;
mod counter_context;
mod custom_property_cascade;
mod custom_property_environments;
#[cfg(test)]
mod differential_tests;
pub(crate) mod effect_descriptions;
pub mod engine_calls;
pub(crate) mod engine_sample;
mod environment_move;
pub mod exact_matcher;
pub(crate) mod flight_style_rows;
pub(crate) mod style_job;
pub use crate::fast_hash;
mod engine_handle;
mod flush;
mod fnv;
mod font_resolution;
pub mod identities;
pub mod impact;
pub mod index;
mod input_routing;
mod inputs;
pub(crate) use inputs::next_declaration_block_version;
pub mod instrumentation;
mod intern_table;
pub(crate) mod layout_style;
mod matching;
pub mod memory;
mod native_rules;
pub mod order;
mod ordering;
mod partial_view;
mod planning;
mod prefix;
pub mod program;
mod program_updates;
mod publication;
mod random_bases;
mod resource_contexts;
mod routing;
pub(crate) mod rule_writes;
mod sorted_merge;
mod style_invalidation;
mod transition_baselines;
mod user_agent_selectors;
pub(crate) use computed::StyleRecordLease;
pub(crate) use publication::RecordDemand;
pub(crate) use transition_baselines::{InheritedAnimatedValue, TransitionBaselines};
pub mod relative_selector;
pub mod selector;
pub mod selector_evaluation;
mod shareable;
mod shared_vector;
mod sheet_occurrences;
mod size_container_invalidation;
mod specified_value;
pub mod transaction;
mod transaction_view;
pub mod tree;
mod weak_pool;

use atoms::DocumentAtoms;
use atoms::ReclaimedStyleAtom;
use catalog::*;
use column::BitColumn;
use column::Column;
use container_queries::PublishedContainerVerdict;
use fast_hash::FastMap as HashMap;
use fast_hash::FastSet as HashSet;
use planning::*;
use smallvec::SmallVec;
use std::collections::VecDeque;
use std::hash::{Hash, Hasher};

use std::sync::Arc;
use std::sync::Mutex;

/// The attribute names whose value text a selector reads, which a document's engine shares with its host.
pub(crate) type SelectorValueTextNames = Arc<HashSet<StyleAtomID>>;

use crate::css::cascaded_properties::CascadeOrigin;
use crate::css::cascaded_properties::CascadedPropertyStore;
use crate::css::computed_values::computed_group_output_mask;
use crate::css::host_shared::{HostShared, SharedPayload};
use crate::css::selector::CompiledSelector;
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::StyleValueData;
use bridge::FfiStyleDelta as PublishedStyleDeltaRecord;
use bridge::FfiStyleDeltaDamage;
use bridge::FfiStyleDeltaGap;
use instrumentation::Counter;
use instrumentation::Counters;

use exact_matcher::ExactMatchContext;
use exact_matcher::ExactMatcher;

pub use counter_context::StyleEngine;
pub use engine_handle::StyleEngineHandle;
pub use inputs::{NaturalSize, PublishedBoxFacts, PublishedTextSource, ReplacedContentInput, TextStyleParentFacts};

use batch_matcher::AncestorRequirements;
use batch_matcher::AncestorRequirementsCache;
use batch_matcher::BatchMatchState;
use batch_matcher::BatchMatcher;
use batch_matcher::CountRuleMatchEmission;
use batch_matcher::RuleMatch;
use batch_matcher::RuleMatches;
use batch_matcher::append_prefix_matches;
use batch_matcher::append_retained_matches;
use batch_matcher::scope_dispatch_shape_and_rules;
use cascade::CascadeCandidate as OrderedCascadeCandidate;
use cascade::CascadeOperator;
use cascade::CascadePriority;
use cascade::CascadeStateID;
use cascade::CascadeStratum;
use cascade::ElementAttachment;
use cascade::PriorityInputs;
use cascade::PropertyWinner;
use cascade::PropertyWinnerUpdate;
use cascade::SpecifiedValueID;
use cascade::SpecifiedWinnerKey;
use cascade::Top1Cascade;
use cascade::WinnerGroupKey;
use cascade::WinnerGroups;
use cascade::WinnerSource;
use compiler::NamespaceScope;
use compiler::ScopeChain;
use compiler::SelectorCompiler;
use computed::ComputedGroupSets;
use impact::AttributionSweep;
use impact::ImpactRegion;
use impact::ImpactRegionBatch;
use impact::ImpactRegions;
use impact::Plan;
use impact::SELECTIVE_SHARE_DIVISOR;
use impact::TransactionTopology;
use impact::choose_plan;
use index::DependencyPostingKey;
use index::DispatchCandidateWorkspace;
use index::DispatchKey;
use index::ElementFactStore;
use index::FeaturePostings;
use index::FeatureValue;
use index::LocalFeatureKey;
use index::MatchingFactBatch;
use index::PostingKey;
use index::RuleDispatch;
use index::StyleAtomID;
use index::StyleNodeFacts;
use input_routing::routing_keys_for_input;
use memory::AdmissionFacts;
use memory::BudgetInputs;
use memory::MemoryCategory;
use memory::MemoryController;
use memory::MemoryLease;
use memory::TIER3_REFUSAL_CATEGORIES;
use partial_view::Lookup;
use prefix::PrefixDeltaArena;
use prefix::PrefixEnteringDeltas;
use prefix::PrefixEvaluation;
use prefix::PrefixMatchSetID;
use prefix::PrefixProducer;
use prefix::PrefixStateCache;
use prefix::PrefixStates;
use prefix::PrefixTransitionLookup;
use program::CascadeLayerID;
use program::CustomDeclaration;
use program::DeclarationBlockID;
use program::DeclaredProperty;
use program::EntryID;
use program::RuleID;
use program::RuleKind;
use program::RuleVersion;
use program::SelectorProgramID;
use program::SheetID;
use program::StyleSheetObjectID;
use program::StyleSheetProgram;
use relative_selector::RelationalWitnessGap;
use relative_selector::RelationalWitnessKey;
use relative_selector::RelationalWitnesses;
use relative_selector::RelativeAxis;
use relative_selector::RelativeQueryID;
use relative_selector::WitnessEffect;
use relative_selector::possible_anchors;
use relative_selector::possible_hosting_anchors;
use relative_selector::traversal_anchor;
use selector::Incomplete;
use selector::InverseStep;
use selector::LiveRelationalRoute;
use selector::MatchEvaluationSide;
use selector::MatchEvaluator;
use selector::MatchScratch;
use selector::NthPosition;
use selector::RelativeAnchor;
use selector::RouteID;
use selector::RoutingKey;
use selector::RoutingRegistry;
use selector::SelectorEntry;
use selector::SelectorOp;
use selector::SelectorProgram;
use selector::SelectorPrograms;
use selector::SiblingEntry;
use selector::SiblingSequenceGeometry;
use selector::Specificity;
use selector::SubjectPosition;
use specified_value::SpecifiedValues;
use transaction::ElementDeclarationKind;
use transaction::InputKey;
use transaction::InputKind;
use transaction::InputValue;
use transaction::NormalizationJournal;
use transaction::NormalizedInput;
use transaction::ProgramJoinDelta;
use transaction::ProgramJoinDeltaKind;
use transaction::ProgramVersion;
use transaction::RuleDeclarationChange;
use transaction::RuleField;
use transaction::StateFact;
use transaction::StyleTransaction;
use transaction::StyleTransactionVersion;
use transaction::TopologyAxis;
use transaction::TreeRelations;
use transaction_view::FeatureFluxColumn;
use transaction_view::PrefixFactTransition;
use transaction_view::TransactionFactSide;
use transaction_view::TransactionFactView;
use tree::SegmentedNodeColumn;
use tree::StyleNodeID;
use tree::StyleNodeTree;
use tree::TreeRelationStaging;
use tree::TreeScopeID;

/// A candidate source at most this large is always worth enumerating, whatever share of the
/// document it is. Proving membership for a handful of candidates cannot lose to streaming a
/// region whose size is unknown.
const SMALL_CANDIDATE_SOURCE: usize = 64;
// Point removal wins for a tiny plan delta. Larger deltas use one linear retain pass instead of
// repeatedly shifting the remaining posting.
const MAX_POINT_REMOVED_EXACT_NODES: usize = 64;

/// Missing prefix transitions installed after one selective traversal or during one convergence
/// pass. This bounds speculative work while letting repeated asks grow retained coverage.
const PREFIX_TRANSITION_CACHE_COMPLETION_BUDGET: usize = 32;

/// How many previous-sibling steps a retained-witness check walks before giving up and routing
/// conservatively, which keeps the check constant-time however long a sibling sequence is.
const RETAINED_WITNESS_SIBLING_STEPS: usize = 64;

/// How much of a sibling sequence the first fact batch asks for. A scan that stops early wastes at
/// most this many rows, which is cheaper than the restart a smaller window would cost.
const INITIAL_SIBLING_FACT_WINDOW: usize = 8;

mod verification {
    use super::Counters;
    use super::MatchAnswerID;
    use super::RetainedState;
    use super::RuleMatch;
    use super::StyleNodeID;
    #[cfg(test)]
    use std::cell::Cell;
    use std::sync::OnceLock;

    static STYLE_ANSWER_PATCH: OnceLock<bool> = OnceLock::new();
    static SELECTOR_TRUTH_DERIVATION: OnceLock<bool> = OnceLock::new();
    static CASCADE_WINNERS: OnceLock<bool> = OnceLock::new();
    static STYLE_PLAN_PROVENANCE: OnceLock<bool> = OnceLock::new();
    static PUBLISHED_STYLE_TRANSACTION: OnceLock<bool> = OnceLock::new();
    static PREFIX_RELATION: OnceLock<bool> = OnceLock::new();

    #[cfg(test)]
    thread_local! {
        static SELECTOR_TRUTH_DERIVATION_OVERRIDE: Cell<bool> = const { Cell::new(false) };
    }

    fn enabled(gate: &OnceLock<bool>, variable: &str) -> bool {
        *gate.get_or_init(|| std::env::var_os(variable).is_some())
    }

    pub(super) struct StyleAnswerVerifier<'a> {
        engine: &'a mut RetainedState,
        counters: &'a mut Counters,
    }

    impl StyleAnswerVerifier<'_> {
        pub(super) fn verify_match_answer(&mut self, answer: &[RuleMatch], node: StyleNodeID, description: &str) {
            let cold = self
                .engine
                .exact_match_answer_for_verification(node, self.counters)
                .expect("cold matching must answer wherever a retained answer did");
            assert_eq!(answer, cold, "{description} differs from cold matching for {node:?}");
        }

        pub(super) fn verify_cascade_answer(&mut self, answer: &[RuleMatch], node: StyleNodeID, description: &str) {
            let (cold, _) = self
                .engine
                .exact_cascade_answer_for_verification(node, self.counters)
                .expect("cold matching must answer wherever a retained answer did");
            assert_eq!(answer, cold, "{description} differs from cold matching for {node:?}");
        }

        pub(super) fn verify_retained_cascade_input(
            &mut self,
            effects: &super::AnswerEffects,
            node: StyleNodeID,
            cascade_input: MatchAnswerID,
        ) {
            self.engine
                .verify_retained_cascade_input(effects, node, cascade_input, self.counters);
        }
    }

    /// Re-derive every patched or reused retained answer cold and compare it. The callback receives
    /// only the verifier capability, so it cannot publish through or otherwise mutate the engine.
    pub(super) fn style_answer_patch(
        engine: &mut RetainedState,
        counters: &mut Counters,
        check: impl FnOnce(&mut StyleAnswerVerifier<'_>),
    ) {
        if enabled(&STYLE_ANSWER_PATCH, "LIBWEB_VERIFY_STYLE_ANSWER_PATCH") {
            check(&mut StyleAnswerVerifier { engine, counters });
        }
    }

    pub(super) fn selector_truth_derivation_is_enabled() -> bool {
        #[cfg(test)]
        if SELECTOR_TRUTH_DERIVATION_OVERRIDE.get() {
            return true;
        }
        *SELECTOR_TRUTH_DERIVATION.get_or_init(|| {
            std::env::var_os("LIBWEB_VERIFY_STYLE_ANSWER_PATCH").is_some()
                || std::env::var_os("LIBWEB_VERIFY_SELECTOR_TRUTH_DERIVATION").is_some()
        })
    }

    #[cfg(test)]
    pub(super) fn with_selector_truth_derivation_enabled<T>(run: impl FnOnce() -> T) -> T {
        struct RestoreOverride<'a> {
            value: &'a Cell<bool>,
            previous: bool,
        }

        impl Drop for RestoreOverride<'_> {
            fn drop(&mut self) {
                self.value.set(self.previous);
            }
        }

        SELECTOR_TRUTH_DERIVATION_OVERRIDE.with(|value| {
            let restore = RestoreOverride {
                previous: value.replace(true),
                value,
            };
            let result = run();
            drop(restore);
            result
        })
    }

    /// Compare complete retained cascade winners with the legacy cascade output.
    pub(super) fn cascade_winners(engine: &RetainedState, check: impl FnOnce(&RetainedState)) {
        if enabled(&CASCADE_WINNERS, "LIBWEB_VERIFY_CASCADE_WINNERS") {
            check(engine);
        }
    }

    /// Require every scoped style transaction output to name semantic provenance.
    pub(super) fn style_plan_provenance(engine: &RetainedState, check: impl FnOnce(&RetainedState)) {
        if enabled(&STYLE_PLAN_PROVENANCE, "LIBWEB_VERIFY_STYLE_PLAN_PROVENANCE") {
            check(engine);
        }
    }

    /// Require a published style transaction to complete without another `match_element()` call.
    pub(super) fn published_style_transaction(engine: &RetainedState, check: impl FnOnce(&RetainedState)) {
        if enabled(
            &PUBLISHED_STYLE_TRANSACTION,
            "LIBWEB_VERIFY_PUBLISHED_STYLE_TRANSACTION",
        ) {
            check(engine);
        }
    }

    pub(super) fn prefix_relation_is_enabled() -> bool {
        enabled(&PREFIX_RELATION, "LIBWEB_VERIFY_PREFIX_RELATION")
    }
}

use verification::{
    cascade_winners as verify_cascade_winners, published_style_transaction as verify_published_style_transaction,
    selector_truth_derivation_is_enabled as verify_selector_truth_derivation_is_enabled,
    style_answer_patch as verify_style_answer_patch, style_plan_provenance as verify_style_plan_provenance,
};

fn exact_tree_routing_is_selective(changed_nodes: usize, document_nodes: usize) -> bool {
    changed_nodes <= SMALL_CANDIDATE_SOURCE
        || changed_nodes.saturating_mul(SELECTIVE_SHARE_DIVISOR) <= document_nodes.max(1)
}

/// The posting an entry's candidates can be enumerated from, or `None` for an entry with no
/// selective rightmost feature.
/// Which region holds the anchors that can see a witness across one relative axis.
///
/// A child query's anchor is the witness's own parent, and a descendant query's is any of its
/// ancestors; naming the ancestors covers both, and covering is what a plan needs. The two subtree
/// axes reach an anchor that is a sibling of some ancestor of the witness, which no single region
/// names, so they are left to the scope.
fn anchor_region_for(axis: RelativeAxis) -> Option<fn(StyleNodeID) -> ImpactRegion> {
    match axis {
        RelativeAxis::Descendant | RelativeAxis::Child => Some(ImpactRegion::Ancestors),
        RelativeAxis::NextSibling => Some(ImpactRegion::PreviousSibling),
        RelativeAxis::FollowingSibling => Some(ImpactRegion::PrecedingSiblings),
        RelativeAxis::NextSiblingSubtree | RelativeAxis::FollowingSiblingSubtree => None,
    }
}

/// Which way a child sequence changed for one element.
#[derive(Clone, Copy, PartialEq, Eq)]
enum SequenceSide {
    /// The element joined the sequence and publishes its arriving facts.
    Arrived,
    /// An already connected element joined from another parent without republishing its facts.
    Reparented,
    /// The element left it, taking with it any witness it was.
    Departed,
    /// The element stayed in the sequence and moved within it.
    Moved,
}

fn for_each_matching_scope(
    scope: TreeScopeID,
    inner_scope: Option<TreeScopeID>,
    slotted_scopes: &[TreeScopeID],
    part_scopes: &[TreeScopeID],
    mut ask: impl FnMut(TreeScopeID, ExactMatchContext) -> Result<(), Incomplete>,
) -> Result<(), Incomplete> {
    ask(scope, ExactMatchContext::Ordinary)?;
    if let Some(inner_scope) = inner_scope {
        ask(inner_scope, ExactMatchContext::Host)?;
    }
    for &slotted_scope in slotted_scopes {
        ask(slotted_scope, ExactMatchContext::Slotted)?;
    }
    for &part_scope in part_scopes {
        ask(part_scope, ExactMatchContext::Part)?;
    }
    Ok(())
}

struct StagedFieldRow<V> {
    before: V,
    after: V,
    dirty: bool,
}

/// Sparse staging for one program field. The first write freezes `before`, later writes replace
/// `after`, and `take_dirty()` applies only the last write while retaining both sides for
/// `ProgramStaging::delta()`.
struct StagedField<K, V> {
    rows: HashMap<K, StagedFieldRow<V>>,
    touched: Vec<K>,
    dirty_count: usize,
}

impl<K, V> Default for StagedField<K, V> {
    fn default() -> Self {
        Self {
            rows: HashMap::default(),
            touched: Vec::new(),
            dirty_count: 0,
        }
    }
}

impl<K: Copy + Eq + Hash + Ord, V: Clone> StagedField<K, V> {
    fn current(&self, key: K, committed: impl FnOnce() -> V) -> V {
        self.side(key, TransactionFactSide::After, committed)
    }

    fn after(&self, key: K) -> Option<&V> {
        self.rows.get(&key).map(|row| &row.after)
    }

    fn side(&self, key: K, side: TransactionFactSide, resident: impl FnOnce() -> V) -> V {
        let row = self.rows.get(&key);
        match (side, row) {
            (TransactionFactSide::Before, Some(row)) => row.before.clone(),
            (TransactionFactSide::After, Some(row)) => row.after.clone(),
            _ => resident(),
        }
    }

    fn stage(&mut self, key: K, before: impl FnOnce() -> V, after: V) {
        match self.rows.entry(key) {
            std::collections::hash_map::Entry::Occupied(mut entry) => {
                let row = entry.get_mut();
                row.after = after;
                if !row.dirty {
                    row.dirty = true;
                    self.dirty_count += 1;
                }
            }
            std::collections::hash_map::Entry::Vacant(entry) => {
                entry.insert(StagedFieldRow {
                    before: before(),
                    after,
                    dirty: true,
                });
                self.touched.push(key);
                self.dirty_count += 1;
            }
        }
    }

    fn take_dirty(&mut self) -> Vec<(K, V)> {
        if self.is_empty() {
            return Vec::new();
        }
        let mut dirty = Vec::with_capacity(self.dirty_count);
        for &key in &self.touched {
            let row = self.rows.get_mut(&key).unwrap();
            if row.dirty {
                dirty.push((key, row.after.clone()));
                row.dirty = false;
            }
        }
        self.dirty_count = 0;
        dirty.sort_unstable_by_key(|(key, _)| *key);
        dirty
    }

    fn is_empty(&self) -> bool {
        self.dirty_count == 0
    }

    fn clear(&mut self) {
        self.rows.clear();
        self.touched.clear();
        // Keep small edit buffers warm, but do not retain stylesheet-loading capacity for the
        // lifetime of every document. These rows are only needed until transaction release.
        if self.rows.capacity() * size_of::<(K, StagedFieldRow<V>)>() > 4096 {
            self.rows = HashMap::default();
        }
        if self.touched.capacity() * size_of::<K>() > 4096 {
            self.touched = Vec::new();
        }
        self.dirty_count = 0;
    }

    fn pairs(&self) -> impl Iterator<Item = (K, &V, &V)> {
        self.touched.iter().map(|&key| {
            let row = self.rows.get(&key).unwrap();
            (key, &row.before, &row.after)
        })
    }
}

#[derive(Default)]
struct ProgramStagingDelta {
    sheets: Vec<SheetID>,
    selector_programs: Vec<SelectorProgramID>,
    arriving_rules: Vec<RuleID>,
    departed_scopes: Vec<(SheetID, TreeScopeID)>,
}

/// Program transaction staging. Every field preserves its first before value and last-writer
/// after value until release; `delta()` may read those pairs after the final values are applied.
#[derive(Default)]
struct ProgramStaging {
    rule_conditions: StagedField<RuleID, bool>,
    sheet_conditions: StagedField<SheetID, bool>,
    sheet_enabled: StagedField<SheetID, bool>,
    rule_declarations: StagedField<RuleID, PendingRuleDeclarations>,
    rule_versions: StagedField<RuleID, RuleVersion>,
    rule_in_a_layer: StagedField<RuleID, bool>,
    rule_gated_by_container_query: StagedField<RuleID, bool>,
    rule_liveness: StagedField<RuleID, bool>,
    layer_orders: StagedField<TreeScopeID, HashMap<CascadeLayerID, u32>>,
    scopes_using_document_sheets: StagedField<TreeScopeID, bool>,
    sheets_in_scope: StagedField<TreeScopeID, Vec<SheetID>>,
    rule_change_is_carried_by_sheet: HashMap<SheetID, bool>,
    base_version: Option<ProgramVersion>,
    rule_declaration_changes: Vec<PendingRuleDeclarationChange>,
    rules_with_incomplete_old_declarations: Vec<RuleID>,
    sheet_rule_replacements: Column<Option<SheetRuleReplacement>>,
}

impl ProgramStaging {
    fn is_dirty(&self) -> bool {
        !self.rule_conditions.is_empty()
            || !self.sheet_conditions.is_empty()
            || !self.sheet_enabled.is_empty()
            || !self.rule_declarations.is_empty()
            || !self.rule_versions.is_empty()
            || !self.rule_in_a_layer.is_empty()
            || !self.rule_gated_by_container_query.is_empty()
            || !self.rule_liveness.is_empty()
            || !self.layer_orders.is_empty()
            || !self.scopes_using_document_sheets.is_empty()
            || !self.sheets_in_scope.is_empty()
            || self.sheet_rule_replacements.iter().any(Option::is_some)
    }

    fn clear(&mut self) {
        self.rule_conditions.clear();
        self.sheet_conditions.clear();
        self.sheet_enabled.clear();
        self.rule_declarations.clear();
        self.rule_versions.clear();
        self.rule_in_a_layer.clear();
        self.rule_gated_by_container_query.clear();
        self.rule_liveness.clear();
        self.layer_orders.clear();
        self.scopes_using_document_sheets.clear();
        self.sheets_in_scope.clear();
        self.rule_change_is_carried_by_sheet.clear();
        self.base_version = None;
        self.rule_declaration_changes.clear();
        self.rules_with_incomplete_old_declarations.clear();
        for index in 0..self.sheet_rule_replacements.len() {
            self.sheet_rule_replacements[index] = None;
        }
    }

    fn delta(&self) -> ProgramStagingDelta {
        let mut delta = ProgramStagingDelta::default();
        delta
            .sheets
            .extend(self.sheet_conditions.pairs().map(|(sheet, _, _)| sheet));
        delta
            .sheets
            .extend(self.sheet_enabled.pairs().map(|(sheet, _, _)| sheet));
        for (scope, before, after) in self.sheets_in_scope.pairs() {
            for &sheet in before.iter().filter(|sheet| !after.contains(sheet)) {
                delta.sheets.push(sheet);
                delta.departed_scopes.push((sheet, scope));
            }
            delta
                .sheets
                .extend(after.iter().copied().filter(|sheet| !before.contains(sheet)));
        }
        delta.sheets.sort_unstable();
        delta.sheets.dedup();
        delta.departed_scopes.sort_unstable();
        delta.departed_scopes.dedup();

        delta.selector_programs.extend(
            self.rule_versions
                .pairs()
                .filter(|(_, before, after)| before.selector_program != after.selector_program)
                .filter_map(|(_, before, _)| before.selector_program),
        );
        delta.selector_programs.sort_unstable_by_key(|program| program.0);
        delta.selector_programs.dedup();

        delta.arriving_rules.extend(
            self.rule_liveness
                .pairs()
                .filter_map(|(rule, before, after)| (!before && *after).then_some(rule)),
        );
        delta.arriving_rules.sort_unstable();
        delta.arriving_rules.dedup();
        delta
    }
}

/// One match of an answer a transaction publishes, as the custom-property cascade reads it.
#[derive(Clone, Copy)]
pub(super) struct BatchCustomPropertyMatch {
    rule: RuleID,
    tree_scope: TreeScopeID,
    specificity: Specificity,
    scope_proximity: u32,
    pseudo: Option<u16>,
}

/// Long-lived engine state: the document, its program, derived results and cross-flush
/// caches. This is the whole read side of an evaluation step; it holds no
/// host handle, no journal intake and no borrowed FFI result storage.
pub struct RetainedState {
    memory: MemoryController,
    /// The controller's Tier-3 admission facts, copied at the loop and quota boundaries that can
    /// change them. A walk reads admission from here: a step may not reach the controller, whose
    /// ledger is shared through an interior-mutable handle no worker owns a share of. The refresh
    /// points are `refresh_admission_facts`'s callers.
    admission: AdmissionFacts,
    deferred_pseudo_elements: u64,
    tree: StyleNodeTree,
    program: StyleSheetProgram,
    native_rules: native_rules::NativeRuleRegistry,
    /// The last declaration block version minted, by the engine or by its document's host, which mints without it.
    declaration_block_version: Arc<std::sync::atomic::AtomicU32>,
    /// Whether the last transaction taken planned nothing but derived child reactions.
    last_transaction_only_derived_child_reactions: bool,
    /// Sheets whose rules currently have no entry points in the routing registry. A detached
    /// sheet's rules decide nothing, so routing every input past their entry points is pure cost
    /// that grows with every sheet that ever came and went.
    sheets_excluded_from_routing: BitColumn,
    /// Whether a sheet detached since the last routing shed, so the registry may hold entry
    /// points for rules that can no longer decide.
    routing_needs_detachment_sweep: bool,
    match_workspace: MatchScratch,
    /// Scratch for the fact rows one exact candidate evaluation covers, reused across candidates.
    exact_covered_scratch: Vec<StyleNodeID>,
    cascade_compaction_scratch: ordering::CascadeCompactionWorkspace,
    cascade_compaction_scratch_memory: MemoryLease,
    /// Monotonic identity assigned to each non-empty normalized style transaction.
    next_style_transaction_version: StyleTransactionVersion,
    /// Latest document-wide scalar computation facts, copied at the transaction boundary. The host
    /// publishes them with every transaction, before it asks for any row.
    document_style_computation_inputs: bridge::FfiDocumentStyleComputationInputs,
    /// The viewport, width and height, the last flush drove records against. A record holding a
    /// value resolved against the viewport cannot stand once it moved, and the viewport is one of
    /// the document's inputs, so comparing it is what tells the engine, not the host naming every
    /// reader.
    driven_viewport: (f64, f64),
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
    /// `font-family: monospace`, the family the monospace font-size recascade resolves an
    /// ancestor's font-relative lengths against.
    monospace_font_family: RetainedStyleValueData,
    layer_topology_version: u64,
    sheet_order_version: u64,

    /// Canonical specified values referenced by dense rule and winner rows.
    specified_values: SpecifiedValues,
    /// The winning stylesheet declarations last observed for each element, interned by their
    /// property-wise answer. Element identities are dense, so the column is directly indexed.
    winner_groups: WinnerGroups,
    /// The shared computed-group payload tuple last published for each live element. This is the
    /// computed half of the eventual base style record; custom properties and metadata remain
    /// separate inputs until that record is complete.
    computed_group_sets: ComputedGroupSets,
    /// The stores behind the custom-property environments live records are published with.
    custom_property_environments: custom_property_environments::CustomPropertyEnvironments,
    /// The nodes whose engine-computed record substituted a custom property into a winner: what
    /// C++ notes as reading custom properties when it installs the record.
    nodes_with_substituted_records: HashSet<StyleNodeID>,
    /// What the custom declarations of an element, or of its pseudo-elements, read beyond the
    /// environment they resolve over, as `FfiNodeRecordReads` bits, for the elements whose
    /// declarations read anything: what the host notes beside their records.
    custom_declaration_reads: HashMap<StyleNodeID, u8>,
    /// Which of a node's records read its place among its siblings, written or produced by a
    /// substitution: a bit for its own record and one per synthetic pseudo-element kind, which
    /// each record the engine derives, or admits from C++, sets or clears as it is installed. A
    /// change among the node's siblings drives a record that reads it again in full, and C++ notes
    /// the node as reading it when it installs the engine's records.
    nodes_with_tree_counting_records: HashMap<StyleNodeID, u16>,
    /// The nodes whose last winner store, for the element or for one of its pseudo-elements (a
    /// bit each, see `rolled_back_bit`), rolled a property back below a revert keyword a
    /// substitution produced: what it rolled back to is a declaration the winners do not name, so
    /// the node's records are its alone and are computed again whatever its winners say.
    nodes_with_rolled_back_records: HashMap<StyleNodeID, u64>,
    /// The nodes whose last winner store, for the element or for one of its pseudo-elements,
    /// substituted a value that resolves against the element: a `random()` it draws or a
    /// container-relative length. No record cache keys on the element, so its records are its
    /// alone.
    nodes_with_element_relative_substitutions: HashMap<StyleNodeID, publication::ElementRelativeSubstitutions>,
    /// The custom-property environment each element holds, for the elements that hold one. This is
    /// the only copy: the element reads its environment from here.
    element_custom_property_data: HashMap<StyleNodeID, inputs::HeldCustomPropertyEnvironment>,
    /// The custom-property environments of each element's synthetic pseudo-elements, by
    /// pseudo-element kind, for the elements with a pseudo-element that holds one.
    pseudo_element_custom_property_data: HashMap<StyleNodeID, Vec<(u8, inputs::HeldCustomPropertyEnvironment)>>,
    /// The elements whose style reads their custom-property environment other than through `var()`,
    /// which a moved environment computes again.
    environment_move_recompute_nodes: HashSet<StyleNodeID>,
    /// What the container conditions of the rows the engine answered read of their containers,
    /// per element, taken when the host installs the element's record.
    container_effects_for_host: container_queries::ContainerEffectsForHost,
    /// Each node's gated rules and whether their conditions held for their targets when its
    /// winners were published, `None` where the engine could not decide them: the winners hold a
    /// gated rule's declarations exactly where it held, and an undecided one leaves the node to
    /// the host.
    published_container_verdicts: HashMap<StyleNodeID, Vec<PublishedContainerVerdict>>,
    /// Nodes whose winners were published while an ancestor's answer was moving in the same
    /// transaction, so their gated rules' conditions could not be decided when they were.
    container_gates_unheld: HashSet<StyleNodeID>,
    /// What moved under the records of the elements the transaction being recorded owes a style
    /// input, which no winner of theirs shows: their containers, or their parent's display.
    row_inputs_moved: flush::RowInputsMoved,
    /// Whether, and as what, each element's published record makes it a query container.
    container_query_inputs: tree::ContainerQueryInputColumns,
    /// What the last layout commit and scroll state say of each container's box, for the
    /// container queries that ask about it: a size container's, and a scroll-state container's.
    layout_style_snapshots: HashMap<StyleNodeID, crate::layout::style_snapshot::LayoutStyleSnapshotRow>,
    /// What the host learned about size container queries, which finds the elements a size
    /// query container's new box moves.
    size_container_queries: size_container_invalidation::SizeContainerQueryFacts,
    /// The counter-style registry each tree scope's style scope has, as one identity per scope:
    /// what a record whose `list-style-type` names a counter style the registry may define names,
    /// so that an edit to `@counter-style` moves it.
    counter_style_environment_identities: HashMap<TreeScopeID, u64>,
    /// The style record each element holds, for the elements that hold one, as the host reports
    /// every record it installs or clears.
    held_style_records: HashMap<StyleNodeID, u64>,
    /// Per shadow host, the elements of its shadow tree that stood for one of its element-backed
    /// pseudo-elements when the host published them; [`RetainedState::backing_elements`] reads the
    /// ones that still do.
    backing_elements: HashMap<StyleNodeID, SmallVec<[StyleNodeID; 2]>>,
    /// The elements and shadow roots the host marked as having a child that explicitly inherits
    /// a non-inherited property: a move of the node's non-inherited groups reaches its children.
    children_explicitly_inherit_marks: HashSet<StyleNodeID>,
    /// The names of the CSS animations the host holds for each element, which the computation of
    /// its animation definitions matches them against.
    css_defined_animations: animations::CssDefinedAnimations,
    /// The `@keyframes` each of the document's style scopes defines, as the host's rule caches
    /// resolved them, which an animation definition's keyframes are resolved from.
    animation_keyframes: animations::AnimationKeyframes,
    /// The animation effects the host holds for each element, described for sampling.
    animation_effect_descriptions: effect_descriptions::AnimationEffectDescriptions,
    /// The font metrics of the record the host holds for the document element, which a `rem` the
    /// host resolves reads, once it holds one.
    held_root_font_inputs: Option<publication::RootFontInputs>,
    /// The random base value each random caching key has been given, for the random functions the
    /// document's styles hold.
    random_base_values: random_bases::RandomBaseValues,
    /// What each element that has replaced content gives its natural size, which layout resolves
    /// against the style of the element's box.
    replaced_content_inputs: HashMap<StyleNodeID, inputs::ReplacedContentInput>,
    /// The computed style groups each longhand reaches, which the host registers before it creates
    /// the engine.
    style_groups: &'static crate::css::computed_values::StyleGroupMasks,
    /// The before-change style each transition target's transitions are decided against for the
    /// rest of the style stabilization epoch, pinned until the epoch commits.
    transition_baselines: transition_baselines::TransitionBaselines,
    /// Whether the registrations used by this transaction differ from the preceding one. A
    /// previously substituted record must then be recomputed by C++, which implements registered
    /// custom properties, even when its cascade winners did not move.
    custom_property_registrations_changed: bool,
    /// Records the engine derived for published reactions that C++ has not installed yet. Their
    /// columns already moved so descendants in the same flush build on them; the cascade state
    /// and answer consumption follow C++'s acknowledgement, and a discarded transaction reverts
    /// the columns of the ones it never installed. Group records by node so acknowledging or
    /// abandoning one element visits only its own record and pseudo-elements.
    engine_computed_records_pending: HashMap<StyleNodeID, SmallVec<[publication::PendingEngineComputedRecord; 1]>>,
    /// The record a private demand last answered for each target it was asked about, pinned for
    /// the host to read: no node holds it.
    demand_records: HashMap<computed::ComputedStyleTarget, computed::FinalStyleRecordID>,
    /// First records derived earlier, by what they were derived from, for later elements alike.
    /// Pseudo-element records the engine derived, by what they were derived from, for elements
    /// alike in that to share.
    /// Counts the style transactions taken; the winner rows record which one published them.
    flush_stamp: u64,
    engine_pseudo_record_cache: HashMap<publication::PseudoCohortKey, computed::FinalStyleRecordID>,
    /// Whether the answer the current transaction publishes for each node has winners complete
    /// but for custom properties, read for the record loop: the answers are installed after it.
    batch_answers_complete_but_for_custom_properties: HashMap<StyleNodeID, bool>,
    /// Beside them, the matches in each answer that declare custom properties, read the same way:
    /// what the node's custom-property cascade runs over while its answer is not installed.
    batch_custom_property_matches: HashMap<StyleNodeID, Vec<BatchCustomPropertyMatch>>,
    /// Beside them, each published host's matches for the element-backed pseudo-elements, which
    /// the elements backing them cascade from while the host's answer is not installed.
    batch_backing_pseudo_matches: HashMap<StyleNodeID, Vec<RuleMatch>>,
    engine_cold_record_cache: HashMap<publication::ColdRecordKey, publication::ColdRecord>,
    engine_cold_record_donors: HashMap<publication::ColdRecordDonorKey, Vec<publication::ColdRecordDonor>>,
    computed_group_set_memory: MemoryLease,
    custom_property_environment_memory: MemoryLease,
    computed_fixed_metadata_memory: MemoryLease,
    computed_longhand_table_memory: MemoryLease,
    style_record_memory: MemoryLease,
    animation_overlay_memory: MemoryLease,
    computed_pseudo_assignment_memory: MemoryLease,
    style_invalidation_cache: HashMap<(u64, u64, bool, bool, bool), u32>,

    /// One identity per distinct match-answer factor, retained exact factor, or cascade input.
    /// The catalog lives with the document rather than with a traversal, because a per-element ask -
    /// which is what a script reading style gets - opens no traversal, and an identity that only
    /// exists inside one answers the pages that need it least. An id is never reused for a
    /// different answer, so a consumer holding one across a flush is never told the wrong thing.
    match_answers: MatchAnswerCatalog,
    selector_truth_sets: SelectorTruthSetCatalog,
    retained_match_answers: RetainedMatchAnswers,
    retained_selector_incidences: RetainedSelectorIncidences,
    /// Whether the current transaction changes activation without changing selector inputs.
    selector_incidence_is_current: bool,
    /// Read-through facts shared by one synchronous style traversal. This is Tier-4 scratch, not
    /// retained matching state. A broad traversal begins with a complete batch; a selective one
    /// promotes only after repeated local packing clears its rebuild-cost hysteresis.
    /// The transaction or host-call adapter owns the box while matching borrows its scratch.
    /// Moving the box at those boundaries leaves the fact batch's vector headers in place.
    batch_matching_traversal: Option<Box<BatchMatchingTraversal>>,
    /// Distinct retained cascade states per dispatch-key posting, shared by every route-pruning
    /// proof in one routing pass. Keyed by the winner-group generation so any winner mutation
    /// invalidates naturally; cleared per transaction so the map cannot grow across flushes. A
    /// `None` entry records that the posting's coverage was incomplete, which is a `false`
    /// verdict for every asker.
    route_pruning_states: Mutex<RoutePruningStateCache>,
    /// Once Tier-3 pressure closes retained-answer admission, the rest of the completion batch
    /// stops asking for exact answers: an exact answer costs more to evaluate, and paying that
    /// premium for an answer the controller cannot retain buys nothing on any later flush.
    completion_exactness: CompletionExactness,
    /// Prefix transitions and their canonical answers have one document-lifetime owner. Matching
    /// traversals and answer patches borrow it synchronously and change its cache-owned lifecycle
    /// between scratch and retained residency without moving the payload.
    prefix_caches: std::sync::Arc<SharedPrefixCaches>,
    /// Test-only: force the bounded completion window regardless of headroom.
    #[cfg(test)]
    force_bounded_prefix_completion: bool,
    /// Current-side scratch an exact transaction already produced for the style consumer
    /// that immediately consumes it. Any intervening engine mutation discards it.
    prepared_batch_matching_traversal: Option<PreparedBatchMatchingTraversal>,
    /// Complete compact answers owned by the scoped style transaction which the next traversal
    /// consumes. This is required Tier-4 scratch, not a persistent inverse match relation.
    published_match_answers: PublishedMatchAnswers,
    transaction_fact_view: Option<TransactionFactView>,
    facts: ElementFactStore,
    programs: SelectorPrograms,
    /// The attribute names whose value text a selector reads, which the host holds a copy of between jobs.
    attribute_value_text_names: SelectorValueTextNames,
    attribute_value_text_requirements_version: u64,
    selector_programs_need_sweep: bool,
    routing: Arc<RoutingRegistry>,
    /// Exact selector changes and refresh requests emitted by the current transaction.
    selector_truth_changes: SelectorTruthChanges,
    already_planned_selector_truth: DeltaBatch<AlreadyPlannedSelectorTruthCandidate>,
    /// Whether the current plan records attribution at all: without a patch selection nothing
    /// consumes it, so the narrowing paths skip the bookkeeping entirely.
    selector_truth_changes_active: bool,
    /// Previous completed matching outcomes, read by routing and installed at matching boundaries.
    relational_witnesses: RelationalWitnesses,
    pending_witness_effects: Vec<WitnessEffect>,
    witness_effect_scratch: MemoryLease,
    relational_witness_residency: MemoryLease,
    /// The node that owns each style scope, for the scopes that have one. The document's scope has
    /// no node, and a scope with no entry here is treated as the document's - which is the widest
    /// answer and therefore the safe one. Tree-scope identities are minted monotonically within one
    /// document, so the identity indexes the column directly.
    scope_roots: Column<Option<StyleNodeID>>,
    /// The inverse of `scope_roots`. Departing ordinary elements vastly outnumber departing scope
    /// roots, so retirement must ask this index instead of scanning every historical tree scope.
    scope_by_root: SegmentedNodeColumn<TreeScopeID>,
    /// The immutable selector dispatch of each distinct effective sheet set and encapsulation
    /// depth. Concrete scopes retain only its dense identity.
    scope_programs: intern_table::InternTable<ScopeProgramID, Option<ScopeProgram>>,
    vacant_scope_programs: Vec<ScopeProgramID>,
    /// One representative dispatch for each live selector topology. Ordinary program changes keep
    /// these templates because their topology contains no concrete rule identity; selector-program
    /// sweeping drops templates whose selector programs are no longer live.
    scope_dispatch_templates: HashMap<ScopeDispatchShape, Arc<RuleDispatch>>,
    /// One ranked dispatch for each selector topology and semantic cascade arrangement. Concrete
    /// rule identities differ between equivalent sheets, but their dense static ranks do not.
    scope_cascade_templates: HashMap<ScopeCascadeShape, Arc<RuleDispatch>>,
    /// One ancestor table for each key layout. Selector program growth often leaves this layout
    /// unchanged. Keep only the table so sharing it cannot retain an obsolete selector dispatch.
    ancestor_dispatch_templates: HashMap<AncestorDispatchShape, Arc<index::AncestorDispatchTopology>>,
    /// The shared program each concrete tree scope resolved to. Program changes clear the table,
    /// while a depth change replaces only this scope's identity. It uses the same direct tree-scope
    /// index as the root column.
    scope_program_by_scope: Column<Option<(u32, ScopeProgramID)>>,
    /// Maps names and qualified names to process-global atoms. Selector names and DOM facts use
    /// the same owner, so a class in a stylesheet and a class on an element compare as one integer.
    ///
    /// An attribute in a namespace is published under this as well as under its local name, and a
    /// selector that names the namespace tests it. The owner retains one document reference to each
    /// global identity and releases it when this engine is destroyed.
    atoms: DocumentAtoms,
    /// The HTML namespace when this is an HTML document, and none otherwise. Some attribute names
    /// compare their values ASCII case-insensitively on an HTML element in an HTML document.
    html_element_namespace: StyleAtomID,
    /// Whether the document matches id and class selectors ASCII case-insensitively, which a
    /// quirks-mode one does. Selectors are then compiled against the lowercase folding of the name,
    /// and the DOM side publishes the folding too, so `.item` and `ITEM` name one atom.
    fold_id_and_class_name_case: bool,
    #[cfg(test)]
    diagnostic_plan_capture: Option<DiagnosticPlanCapture>,
}

/// Host-facing engine state: C++ ownership and journal intake.
/// Never reachable from an evaluation step.
pub struct HostState {
    /// The style pass the host is installing wave by wave, between two of its waves.
    suspended_style_pass: Option<flush::StylePass>,
    /// The host's synchronous font resolver. A step that misses the cache returns `NeedsInput`;
    /// the round outside the step calls this and the node is retried.
    font_resolver: Option<font_resolution::FontResolverHost>,
    journal: NormalizationJournal,
    /// Local selector facts through the latest geometry read which reused committed layout. A
    /// normal style observation merges this into `journal`; a newly introduced transition can
    /// instead consume it as the preceding style change event.
    deferred_geometry_journal: NormalizationJournal,
    flushing_deferred_geometry_journal: bool,
    /// Exact element reactions retained across rootless flushes until a style root can consume them.
    deferred_element_style_inputs: Vec<NormalizedInput>,
    /// Whether the deferred element style inputs moved since the document's host last took them.
    deferred_element_style_inputs_moved: bool,
    /// Whether the deferred element style inputs are owed to the next transaction, as opposed to
    /// held back by a flush without a document root.
    deferred_element_style_inputs_are_pending: bool,
    /// What the last custom-property environment move answered the host, kept until the next one.
    environment_move_actions: Vec<bridge::FfiEnvironmentMoveAction>,
    deferred_element_style_input_memory: MemoryLease,
    /// Whether any tree input batch has crossed into the engine. A first batch consisting entirely
    /// of unique arrivals can install its final relation rows as one bulk load.
    initial_tree_batch_applied: bool,
    /// Whether that bulk load is still part of the transaction awaiting first observation.
    initial_tree_bulk_load_is_pending: bool,
    /// Final relation rows staged until the next observation boundary. Moving one node updates its
    /// affected neighbours here, so those derived changes need no separate journal ingress.
    tree_staging: TreeRelationStaging,
    tree_staging_memory: MemoryLease,
    /// Program-family before/after rows retained until the transaction is released.
    program_staging: ProgramStaging,
    sheet_occurrences: HashMap<TreeScopeID, sheet_occurrences::ScopeSheetOccurrences>,
    sheet_occurrence_storage_bytes: u64,
    sheet_occurrence_memory: MemoryLease,
    /// The old dense rule sequence while one sheet is synchronously reparsed.
    sheet_rule_replacement: Option<SheetRuleReplacement>,
    /// Identities released at transaction settlement. The FFI keeps this batch borrowed until C++
    /// has removed its matching fly-string references and atom-keyed memo entries.
    reclaimed_style_atoms: Vec<ReclaimedStyleAtom>,
    /// Whether the transaction under way leaves its atom sweep to a later one: it runs beside the host, which may name
    /// an atom meanwhile that the sweep would reclaim before the host hears of it.
    defers_atom_sweep: bool,
}

/// Mutable engine state; operations borrow their instrumentation from the boundary.
pub struct StyleEngineState {
    pub(super) retained: RetainedState,
    pub(super) host: HostState,
}

impl std::ops::Deref for StyleEngineState {
    type Target = RetainedState;

    fn deref(&self) -> &Self::Target {
        &self.retained
    }
}

impl std::ops::DerefMut for StyleEngineState {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.retained
    }
}

#[cfg(test)]
mod tests;
