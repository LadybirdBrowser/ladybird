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
mod custom_property_cascade;
mod custom_property_environments;
#[cfg(test)]
mod differential_tests;
pub mod exact_matcher;
pub mod fast_hash;
mod flush;
mod fnv;
mod font_resolution;
pub mod impact;
pub mod index;
mod input_routing;
mod inputs;
pub mod instrumentation;
mod intern_table;
mod matching;
pub mod memory;
pub mod order;
mod ordering;
mod partial_view;
mod planning;
mod prefix;
pub mod program;
mod program_updates;
mod publication;
#[cfg(feature = "style-recording")]
pub mod record_replay;
mod routing;
mod sorted_merge;
mod style_invalidation;
#[cfg(not(feature = "style-recording"))]
pub mod record_replay {
    include!(concat!(env!("OUT_DIR"), "/style_engine_event_kind_stub_generated.rs"));

    pub(crate) fn invalidate_pointer(_pointer: usize) {}

    #[derive(Default)]
    pub struct PayloadWriter {
        _private: (),
    }

    /// Mirror of the recording build's marker trait.
    ///
    /// # Safety
    /// See the recording build's `RawRecord` for the contract; this stub records nothing.
    pub unsafe trait RawRecord: Copy {}

    impl PayloadWriter {
        pub fn write_bool(&mut self, _value: bool) {}
        pub fn write_bytes(&mut self, _value: &[u8]) {}
        pub fn write_length(&mut self, _value: usize) {}
        pub fn write_i32(&mut self, _value: i32) {}
        pub fn write_u8(&mut self, _value: u8) {}
        pub fn write_u16(&mut self, _value: u16) {}
        pub fn write_u16_slice(&mut self, _value: &[u16]) {}
        pub fn write_u32(&mut self, _value: u32) {}
        pub fn write_u32_slice(&mut self, _value: &[u32]) {}
        pub fn write_u64(&mut self, _value: u64) {}
        pub fn write_u64_slice(&mut self, _value: &[u64]) {}
        pub fn write_native_u16(&mut self, _value: u16) {}
        pub fn write_native_u32(&mut self, _value: u32) {}
        pub fn write_raw_slice<T: RawRecord>(&mut self, _values: &[T]) {}
        pub fn write_raw_rows(
            &mut self,
            _count: usize,
            _row_size: usize,
            _row_alignment: usize,
            write_rows: impl FnOnce(&mut Self),
        ) {
            write_rows(self);
        }
        pub fn as_bytes(&self) -> &[u8] {
            &[]
        }
        pub fn stable_digest(&self) -> u64 {
            0
        }
    }
}
pub mod relative_selector;
pub mod selector;
mod specified_value;
pub mod transaction;
mod transaction_view;
pub mod tree;

use atoms::DocumentAtoms;
use atoms::PinnedAtoms;
use atoms::ReclaimedStyleAtom;
use catalog::*;
use column::BitColumn;
use column::Column;
use fast_hash::FastMap as HashMap;
use fast_hash::FastSet as HashSet;
use planning::*;
use std::cell::RefCell;
use std::collections::VecDeque;
use std::hash::{Hash, Hasher};
use std::rc::Rc;

use crate::css::cascaded_properties::CascadeOrigin;
use crate::css::cascaded_properties::CascadedPropertyStore;
use crate::css::cascaded_properties::FfiCascadeBlock;
use crate::css::cascaded_properties::FfiSourceSlotAssignment;
use crate::css::computed_values::computed_group_dependency_mask;
use crate::css::computed_values::computed_group_output_mask;
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

use batch_matcher::AncestorRequirements;
use batch_matcher::AncestorRequirementsCache;
use batch_matcher::BatchMatchState;
use batch_matcher::BatchMatcher;
use batch_matcher::CountRuleMatchEmission;
use batch_matcher::RuleMatch;
use batch_matcher::RuleMatches;
use batch_matcher::append_prefix_matches;
use batch_matcher::append_retained_matches;
use batch_matcher::build_scope_dispatch;
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
use memory::BudgetInputs;
use memory::DeviceClass;
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
use prefix::PrefixProducerCache;
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
use program::SemanticDeclarationID;
use program::SheetID;
use program::StyleSheetObjectID;
use program::StyleSheetProgram;
use relative_selector::RelationalWitnessGap;
use relative_selector::RelationalWitnessKey;
use relative_selector::RelationalWitnesses;
use relative_selector::RelativeAxis;
use relative_selector::RelativeQueryID;
use relative_selector::possible_anchors;
use relative_selector::possible_hosting_anchors;
use relative_selector::traversal_anchor;
use selector::Incomplete;
use selector::InverseStep;
use selector::LiveRelationalRoute;
use selector::MatchEvaluationSide;
use selector::MatchEvaluationWorkspace;
use selector::MatchEvaluator;
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
    use super::MatchAnswerID;
    use super::RuleMatch;
    use super::StyleEngine;
    use super::StyleNodeID;
    #[cfg(test)]
    use std::cell::Cell;
    use std::sync::OnceLock;

    static STYLE_ANSWER_PATCH: OnceLock<bool> = OnceLock::new();
    static SELECTOR_TRUTH_DERIVATION: OnceLock<bool> = OnceLock::new();
    static CASCADE_WINNERS: OnceLock<bool> = OnceLock::new();
    static STYLE_PLAN_PROVENANCE: OnceLock<bool> = OnceLock::new();
    static PUBLISHED_STYLE_TRANSACTION: OnceLock<bool> = OnceLock::new();

    #[cfg(test)]
    thread_local! {
        static SELECTOR_TRUTH_DERIVATION_OVERRIDE: Cell<bool> = const { Cell::new(false) };
    }

    fn enabled(gate: &OnceLock<bool>, variable: &str) -> bool {
        *gate.get_or_init(|| std::env::var_os(variable).is_some())
    }

    pub(super) struct StyleAnswerVerifier<'a> {
        engine: &'a mut StyleEngine,
    }

    impl StyleAnswerVerifier<'_> {
        pub(super) fn verify_match_answer(&mut self, answer: &[RuleMatch], node: StyleNodeID, description: &str) {
            let cold = self
                .engine
                .exact_match_answer_for_verification(node)
                .expect("cold matching must answer wherever a retained answer did");
            assert_eq!(answer, cold, "{description} differs from cold matching for {node:?}");
        }

        pub(super) fn verify_cascade_answer(&mut self, answer: &[RuleMatch], node: StyleNodeID, description: &str) {
            let (cold, _) = self
                .engine
                .exact_cascade_answer_for_verification(node)
                .expect("cold matching must answer wherever a retained answer did");
            assert_eq!(answer, cold, "{description} differs from cold matching for {node:?}");
        }

        pub(super) fn verify_retained_cascade_input(&mut self, node: StyleNodeID, cascade_input: MatchAnswerID) {
            self.engine.verify_retained_cascade_input(node, cascade_input);
        }
    }

    /// Re-derive every patched or reused retained answer cold and compare it. The callback receives
    /// only the verifier capability, so it cannot publish through or otherwise mutate the engine.
    pub(super) fn style_answer_patch(engine: &mut StyleEngine, check: impl FnOnce(&mut StyleAnswerVerifier<'_>)) {
        if enabled(&STYLE_ANSWER_PATCH, "LIBWEB_VERIFY_STYLE_ANSWER_PATCH") {
            check(&mut StyleAnswerVerifier { engine });
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
    pub(super) fn cascade_winners(engine: &StyleEngine, check: impl FnOnce(&StyleEngine)) {
        if enabled(&CASCADE_WINNERS, "LIBWEB_VERIFY_CASCADE_WINNERS") {
            check(engine);
        }
    }

    /// Require every scoped style transaction output to name semantic provenance.
    pub(super) fn style_plan_provenance(engine: &StyleEngine, check: impl FnOnce(&StyleEngine)) {
        if enabled(&STYLE_PLAN_PROVENANCE, "LIBWEB_VERIFY_STYLE_PLAN_PROVENANCE") {
            check(engine);
        }
    }

    /// Require a published style transaction to complete without another selector query.
    pub(super) fn published_style_transaction(engine: &StyleEngine, check: impl FnOnce(&StyleEngine)) {
        if enabled(
            &PUBLISHED_STYLE_TRANSACTION,
            "LIBWEB_VERIFY_PUBLISHED_STYLE_TRANSACTION",
        ) {
            check(engine);
        }
    }

    pub(super) fn gate_bits() -> u8 {
        u8::from(enabled(&STYLE_ANSWER_PATCH, "LIBWEB_VERIFY_STYLE_ANSWER_PATCH"))
            | (u8::from(enabled(&CASCADE_WINNERS, "LIBWEB_VERIFY_CASCADE_WINNERS")) << 1)
            | (u8::from(enabled(&STYLE_PLAN_PROVENANCE, "LIBWEB_VERIFY_STYLE_PLAN_PROVENANCE")) << 2)
            | (u8::from(enabled(
                &PUBLISHED_STYLE_TRANSACTION,
                "LIBWEB_VERIFY_PUBLISHED_STYLE_TRANSACTION",
            )) << 3)
            | (u8::from(selector_truth_derivation_is_enabled()) << 4)
    }
}

use verification::{
    cascade_winners as verify_cascade_winners, published_style_transaction as verify_published_style_transaction,
    selector_truth_derivation_is_enabled as verify_selector_truth_derivation_is_enabled,
    style_answer_patch as verify_style_answer_patch, style_plan_provenance as verify_style_plan_provenance,
};

fn verification_gate_bits() -> u8 {
    verification::gate_bits()
}

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
    /// The element joined the sequence.
    Arrived,
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

    fn stage(&mut self, key: K, before: V, after: V) {
        if let Some(row) = self.rows.get_mut(&key) {
            row.after = after;
            if !row.dirty {
                row.dirty = true;
                self.dirty_count += 1;
            }
            return;
        }
        self.rows.insert(
            key,
            StagedFieldRow {
                before,
                after,
                dirty: true,
            },
        );
        self.touched.push(key);
        self.dirty_count += 1;
    }

    fn take_dirty(&mut self) -> Vec<(K, V)> {
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
        self.dirty_count = 0;
    }

    fn iter(&self) -> impl Iterator<Item = (&K, &V)> {
        self.touched.iter().map(|key| (key, &self.rows.get(key).unwrap().after))
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
            delta.sheets.extend(
                before
                    .iter()
                    .chain(after)
                    .copied()
                    .filter(|sheet| before.contains(sheet) != after.contains(sheet)),
            );
            delta.departed_scopes.extend(
                before
                    .iter()
                    .copied()
                    .filter(|sheet| !after.contains(sheet))
                    .map(|sheet| (sheet, scope)),
            );
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

/// One document's style engine.
struct QuerySortedCandidatesStamp {
    generation: u64,
    root: StyleNodeID,
    keys: Vec<DispatchKey>,
}

pub struct StyleEngine {
    /// The capture-local document identity, absent when record-replay is disabled.
    #[cfg(feature = "style-recording")]
    recording_id: Option<u64>,
    memory: MemoryController,
    counters: Counters,
    /// The instrumentation state to restore after C++ materializes a record for verification.
    computed_record_verification_counters: Option<Box<Counters>>,
    computed_record_verification_pins: Vec<u64>,
    tree: StyleNodeTree,
    program: StyleSheetProgram,
    journal: NormalizationJournal,
    /// Local selector facts through the latest geometry read which reused committed layout. A
    /// normal style observation merges this into `journal`; a newly introduced transition can
    /// instead consume it as the preceding style change event.
    deferred_geometry_journal: NormalizationJournal,
    flushing_deferred_geometry_journal: bool,
    /// Exact element reactions retained across rootless flushes until a style root can consume them.
    deferred_element_style_inputs: Vec<NormalizedInput>,
    /// Whether the deferred element style inputs are owed to the next transaction, as opposed to
    /// held back by a flush without a document root.
    deferred_element_style_inputs_are_pending: bool,
    /// The nodes whose deferred element style input C++ recorded and the engine did not also
    /// derive as a child reaction: what makes the next transaction a new pass of a style change
    /// rather than one more generation of the last one.
    externally_recorded_style_input_nodes: HashSet<StyleNodeID>,
    /// Whether the last transaction taken planned nothing but derived child reactions.
    last_transaction_only_derived_child_reactions: bool,
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
    /// Sheets whose rules currently have no entry points in the routing registry. A detached
    /// sheet's rules decide nothing, so routing every input past their entry points is pure cost
    /// that grows with every sheet that ever came and went.
    sheets_excluded_from_routing: BitColumn,
    /// Whether a sheet detached since the last routing shed, so the registry may hold entry
    /// points for rules that can no longer decide.
    routing_needs_detachment_sweep: bool,
    /// The old dense rule sequence while one sheet is synchronously reparsed.
    sheet_rule_replacement: Option<SheetRuleReplacement>,
    match_workspace: MatchEvaluationWorkspace,
    /// Sibling positions and relation answers shared by the candidates of one DOM selector query.
    /// A query can't mutate the tree it walks — so this is reset per-query, rather than per-candidate.
    query_match_workspace: MatchEvaluationWorkspace,
    /// Advanced when a selector query settles over a changed document — so a run of queries over
    /// an unchanged one shares a single workspace. A query that never asks a positional question
    /// pays a comparison, rather than a workspace rebuild.
    selector_query_generation: u64,
    /// The transaction version the last selector query settled at; the change detector for the
    /// generation above.
    query_settled_transaction_version: StyleTransactionVersion,
    /// Tree-ordered candidates of the last posting-driven selector query. querySelector in a loop
    /// typically asks with the same subject keys every time — so collect-and-sort pays once per
    /// document change, rather than once per query.
    query_sorted_candidates: Vec<StyleNodeID>,
    /// What `query_sorted_candidates` was computed from. Same stamp, same list.
    query_sorted_candidates_stamp: Option<QuerySortedCandidatesStamp>,
    /// node -> preorder rank under the stamped root; how candidates get their tree order without a
    /// walk per query.
    query_preorder_ranks: HashMap<StyleNodeID, u32>,
    query_preorder_ranks_stamp: Option<(u64, StyleNodeID)>,
    /// Which query `query_match_workspace` holds answers for.
    query_workspace_generation: u64,
    /// Scratch for the fact rows one exact candidate evaluation covers, reused across candidates.
    exact_covered_scratch: Vec<StyleNodeID>,
    /// Monotonic identity assigned to each non-empty normalized style transaction.
    next_style_transaction_version: StyleTransactionVersion,
    /// Latest document-wide scalar computation facts, copied at the transaction boundary.
    document_style_computation_inputs: Option<bridge::FfiDocumentStyleComputationInputs>,
    font_resolver: Option<font_resolution::FontResolver>,
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
    /// Whether the registrations used by this transaction differ from the preceding one. A
    /// previously substituted record must then be recomputed by C++, which implements registered
    /// custom properties, even when its cascade winners did not move.
    custom_property_registrations_changed: bool,
    /// Pending selections for elements, and separately for the few pseudo-elements that hold one.
    /// Both are keyed by the element so that retiring it releases every selection by key.
    pending_element_style_computation_selections: HashMap<StyleNodeID, StyleComputationSelection>,
    pending_pseudo_style_computation_selections: HashMap<StyleNodeID, Vec<(u8, StyleComputationSelection)>>,
    /// Records the engine derived for published reactions that C++ has not installed yet. Their
    /// columns already moved so descendants in the same flush build on them; the cascade state
    /// and answer consumption follow C++'s acknowledgement, and a discarded transaction reverts
    /// the columns of the ones it never installed.
    engine_computed_records_pending: Vec<publication::PendingEngineComputedRecord>,
    /// First records derived earlier, by what they were derived from, for later elements alike.
    /// Pseudo-element records the engine derived, by what they were derived from, for elements
    /// alike in that to share.
    /// Counts the style transactions taken; the winner rows record which one published them.
    flush_stamp: u64,
    /// Nodes whose style input the C++ computation has to settle: C++ recorded one, or their
    /// parent's display moved, which their box-type transformation reads.
    style_input_nodes_for_cpp: HashSet<StyleNodeID>,
    /// Elements whose parent's display moved under their record this transaction: their
    /// box-type transformation reads it, so their record is driven again in full.
    parent_inputs_moved_nodes: HashSet<StyleNodeID>,
    engine_pseudo_record_cache: HashMap<publication::PseudoCohortKey, computed::FinalStyleRecordID>,
    engine_cold_record_cache: HashMap<publication::ColdRecordKey, publication::ColdRecord>,
    engine_cold_record_donors: HashMap<publication::ColdRecordDonorKey, Vec<publication::ColdRecordDonor>>,
    /// Which winner states the engine can compute records from, decided once per state.
    engine_computable_states: HashMap<(u64, CascadeStateID, u64, u64), bool>,
    computed_group_set_memory: MemoryLease,
    custom_property_environment_memory: MemoryLease,
    computed_fixed_metadata_memory: MemoryLease,
    computed_longhand_table_memory: MemoryLease,
    style_record_memory: MemoryLease,
    animation_overlay_memory: MemoryLease,
    computed_pseudo_assignment_memory: MemoryLease,
    style_invalidation_cache: HashMap<(u64, u64, bool, bool), u32>,

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
    /// Boxed because every per-element ask takes it out of this slot and puts it back, and moving
    /// the struct moves the fact batch's two dozen vector headers with it.
    batch_matching_traversal: Option<Box<BatchMatchingTraversal>>,
    /// While a published-answer completion is running, cold matching skips cascade winner-pruning
    /// so the produced answer is exact and can enter the retained match relation. A pruned answer
    /// costs less once but cannot be retained, which forces the same region back to cold matching
    /// on every subsequent flush.
    complete_answers_exactly: bool,
    /// Distinct retained cascade states per dispatch-key posting, shared by every route-pruning
    /// proof in one routing pass. Keyed by the winner-group generation so any winner mutation
    /// invalidates naturally; cleared per transaction so the map cannot grow across flushes. A
    /// `None` entry records that the posting's coverage was incomplete, which is a `false`
    /// verdict for every asker.
    route_pruning_states: RefCell<RoutePruningStateCache>,
    /// Once Tier-3 pressure closes retained-answer admission, the rest of the completion batch
    /// stops asking for exact answers: an exact answer costs more to evaluate, and paying that
    /// premium for an answer the controller cannot retain buys nothing on any later flush.
    completion_exactness_exhausted: bool,
    /// Prefix transitions and their canonical answers have one document-lifetime owner. Matching
    /// traversals and answer patches borrow it synchronously and change its cache-owned lifecycle
    /// between scratch and retained residency without moving the payload.
    prefix_caches: Rc<RefCell<PrefixCaches>>,
    /// Test-only: force the bounded completion window regardless of headroom.
    #[cfg(test)]
    force_bounded_prefix_completion: bool,
    /// Current-side scratch an exact transaction already produced for the style consumer
    /// that immediately consumes it. Any intervening engine mutation discards it.
    prepared_batch_matching_traversal: Option<PreparedBatchMatchingTraversal>,
    /// Complete compact answers owned by the scoped style transaction which the next traversal
    /// consumes. This is required Tier-4 scratch, not a persistent inverse match relation.
    published_match_answers: PublishedMatchAnswers,
    /// Borrowed FFI result storage for the most recently published style transaction.
    ffi_style_transaction_output: bridge::FfiStyleTransactionOutput,
    ffi_style_transaction_output_memory: MemoryLease,
    /// Borrowed FFI result storage for the most recent style-node query.
    ffi_style_node_query: Vec<u32>,
    ffi_style_node_query_memory: MemoryLease,
    /// Borrowed FFI result storage for retained cascade source-slot assignments.
    ffi_retained_cascade_assignments: Vec<FfiSourceSlotAssignment>,
    ffi_retained_cascade_assignments_memory: MemoryLease,
    transaction_fact_view: Option<TransactionFactView>,
    facts: ElementFactStore,
    programs: SelectorPrograms,
    attribute_value_text_names: HashSet<StyleAtomID>,
    attribute_value_text_requirements_version: u64,
    selector_programs_need_sweep: bool,
    routing: Rc<RoutingRegistry>,
    /// Exact selector changes and refresh requests emitted by the current transaction.
    selector_truth_changes: SelectorTruthChanges,
    already_planned_selector_truth: DeltaBatch<AlreadyPlannedSelectorTruthCandidate>,
    /// Whether the current plan records attribution at all: without a patch selection nothing
    /// consumes it, so the narrowing paths skip the bookkeeping entirely.
    selector_truth_changes_active: bool,
    /// The retained witnesses of simple relational queries. Behind a `RefCell` because the cold
    /// evaluator writes them mid-evaluation while it holds shared borrows of the stores it reads.
    relational_witnesses: RefCell<RelationalWitnesses>,
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
    scope_dispatch_templates: HashMap<ScopeDispatchShape, Rc<RuleDispatch>>,
    /// One ranked dispatch for each selector topology and semantic cascade arrangement. Concrete
    /// rule identities differ between equivalent sheets, but their dense static ranks do not.
    scope_cascade_templates: HashMap<ScopeCascadeShape, Rc<RuleDispatch>>,
    /// One representative dispatch for each ancestor-key layout. Selector program growth often
    /// leaves this much smaller topology unchanged, so its document-wide summaries remain shared
    /// until selector-program sweeping bounds the set.
    ancestor_dispatch_templates: HashMap<AncestorDispatchShape, Rc<RuleDispatch>>,
    /// The shared program each concrete tree scope resolved to. Program changes clear the table,
    /// while a depth change replaces only this scope's identity. It uses the same direct tree-scope
    /// index as the root column.
    scope_program_by_scope: Column<Option<(u32, ScopeProgramID)>>,
    /// The last scope lookup. A style traversal nearly always asks consecutive elements in one
    /// scope, so the common path compares two integers and never hashes its ordered sheet set.
    held_scope_program: Option<(TreeScopeID, u32, ScopeProgramID)>,
    /// Maps names and qualified names to process-global atoms. Selector names and DOM facts use
    /// the same owner, so a class in a stylesheet and a class on an element compare as one integer.
    ///
    /// An attribute in a namespace is published under this as well as under its local name, and a
    /// selector that names the namespace tests it. The owner retains one document reference to each
    /// global identity and releases it when this engine is destroyed.
    atoms: DocumentAtoms,
    /// Identities released at transaction settlement. The FFI keeps this batch borrowed until C++
    /// has removed its matching fly-string references and atom-keyed memo entries.
    reclaimed_style_atoms: Vec<ReclaimedStyleAtom>,
    /// Whether transaction settlement performed an atom sweep, including a sweep that reclaimed
    /// no identities. Recording consumes this alongside the release batch.
    style_atoms_swept: bool,
    /// Replay reconstructs semantic engine state but not transient C++ query handles. The recorded
    /// release batch supplies their lifetime boundary while still requiring every released atom to
    /// be reclaimable from replay's complete semantic root set.
    replay_reclaimed_style_atoms: Option<Vec<StyleAtomID>>,
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

#[derive(Clone, Copy)]
pub(crate) struct StyleComputationSelection {
    pub computed_property_words: [u64; crate::css::property_metadata::LONGHAND_WORD_COUNT],
    pub computed_property_closure_is_exact: bool,
}

#[cfg(test)]
mod tests;
