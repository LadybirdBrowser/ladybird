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

pub mod batch_matcher;
pub mod bridge;
mod capacity;
pub mod cascade;
mod catalog;
mod column;
pub mod compiler;
mod computed;
#[cfg(test)]
mod differential_tests;
pub mod exact_matcher;
pub mod fast_hash;
mod flush;
mod fnv;
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
        pub fn write_u8(&mut self, _value: u8) {}
        pub fn write_u16(&mut self, _value: u16) {}
        pub fn write_u16_slice(&mut self, _value: &[u16]) {}
        pub fn write_u32(&mut self, _value: u32) {}
        pub fn write_u32_slice(&mut self, _value: &[u32]) {}
        pub fn write_u64(&mut self, _value: u64) {}
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

use catalog::*;
use column::Column;
use fast_hash::FastMap as HashMap;
use fast_hash::FastSet as HashSet;
use fast_hash::StableIterationMap;
use planning::*;
use std::cell::RefCell;
use std::collections::VecDeque;
use std::hash::Hash;
use std::hash::Hasher;
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
use index::DispatchEntryID;
use index::DispatchKey;
use index::ElementFactStore;
use index::FeatureKey;
use index::FeaturePostings;
use index::FeatureValue;
use index::PostingKey;
use index::RuleDispatch;
use index::SelectorPostingKey;
use index::StyleAtomID;
use index::StyleNodeFacts;
use input_routing::input_routes_on_key;
use input_routing::routing_keys_for_input;
use memory::BudgetInputs;
use memory::DeviceClass;
use memory::MemoryCategory;
use memory::MemoryController;
use memory::MemoryLease;
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
use program::DeclarationBlockID;
use program::DeclaredProperty;
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
use selector::SiblingSequenceGeometry;
use selector::Specificity;
use selector::SubjectPosition;
use selector::ValueStateKind;
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
use tree::StyleNodeID;
use tree::StyleNodeTree;
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
    use std::sync::OnceLock;

    static STYLE_ANSWER_PATCH: OnceLock<bool> = OnceLock::new();
    static CASCADE_WINNERS: OnceLock<bool> = OnceLock::new();
    static STYLE_PLAN_PROVENANCE: OnceLock<bool> = OnceLock::new();
    static PUBLISHED_STYLE_TRANSACTION: OnceLock<bool> = OnceLock::new();

    fn enabled(gate: &OnceLock<bool>, variable: &str) -> bool {
        *gate.get_or_init(|| std::env::var_os(variable).is_some())
    }

    /// Re-derive every patched or reused retained answer cold and compare it.
    pub(super) fn style_answer_patch(check: impl FnOnce()) {
        if enabled(&STYLE_ANSWER_PATCH, "LIBWEB_VERIFY_STYLE_ANSWER_PATCH") {
            check();
        }
    }

    /// Compare complete retained cascade winners with the legacy cascade output.
    pub(super) fn cascade_winners(check: impl FnOnce()) {
        if enabled(&CASCADE_WINNERS, "LIBWEB_VERIFY_CASCADE_WINNERS") {
            check();
        }
    }

    /// Require every scoped style transaction output to name semantic provenance.
    pub(super) fn style_plan_provenance(check: impl FnOnce()) {
        if enabled(&STYLE_PLAN_PROVENANCE, "LIBWEB_VERIFY_STYLE_PLAN_PROVENANCE") {
            check();
        }
    }

    /// Require a published style transaction to complete without another selector query.
    pub(super) fn published_style_transaction(check: impl FnOnce()) {
        if enabled(
            &PUBLISHED_STYLE_TRANSACTION,
            "LIBWEB_VERIFY_PUBLISHED_STYLE_TRANSACTION",
        ) {
            check();
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
    }
}

use verification::{
    cascade_winners as verify_cascade_winners, published_style_transaction as verify_published_style_transaction,
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

fn posting_for_dispatch_key(key: DispatchKey) -> Option<PostingKey> {
    match key {
        DispatchKey::Part(atom) => Some(PostingKey::Selector(SelectorPostingKey::Part(atom))),
        DispatchKey::CustomState(atom) => Some(PostingKey::Selector(SelectorPostingKey::CustomState(atom))),
        DispatchKey::Id(atom) => Some(PostingKey::Selector(SelectorPostingKey::Id(atom))),
        DispatchKey::Class(atom) => Some(PostingKey::Selector(SelectorPostingKey::Class(atom))),
        DispatchKey::AttributeName(atom) => Some(PostingKey::Selector(SelectorPostingKey::AttributeName(atom))),
        DispatchKey::TagName(atom) => Some(PostingKey::Selector(SelectorPostingKey::Tag(atom))),
        DispatchKey::Directionality(atom) => Some(PostingKey::Selector(SelectorPostingKey::Directionality(atom))),
        // These have no posting to enumerate from, so a caller that needs one widens, exactly as it
        // does for the universal bucket these used to sit in.
        DispatchKey::Root | DispatchKey::State(_) | DispatchKey::Heading | DispatchKey::Universal => None,
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

struct PendingField<K, V>(HashMap<K, V>);

impl<K, V> Default for PendingField<K, V> {
    fn default() -> Self {
        Self(HashMap::default())
    }
}

impl<K: Copy + Eq + Hash, V: Clone> PendingField<K, V> {
    fn current(&self, key: K, committed: impl FnOnce() -> V) -> V {
        self.0.get(&key).cloned().unwrap_or_else(committed)
    }

    fn stage(&mut self, key: K, value: V) {
        self.0.insert(key, value);
    }

    fn take(&mut self) -> HashMap<K, V> {
        std::mem::take(&mut self.0)
    }

    fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    fn iter(&self) -> impl Iterator<Item = (&K, &V)> {
        self.0.iter()
    }
}

/// One document's style engine.
pub struct StyleEngine {
    /// The capture-local document identity, absent when record-replay is disabled.
    #[cfg(feature = "style-recording")]
    recording_id: Option<u64>,
    memory: MemoryController,
    parsed_substitution_memory: MemoryLease,
    counters: Counters,
    tree: StyleNodeTree,
    program: StyleSheetProgram,
    /// Rule identities below this bound existed at the preceding commit barrier.
    committed_rule_count: u32,
    journal: NormalizationJournal,
    /// Whether any tree input batch has crossed into the engine. A first batch consisting entirely
    /// of unique arrivals can install its final relation rows as one bulk load.
    initial_tree_batch_applied: bool,
    /// Whether that bulk load is still part of the transaction awaiting first observation.
    initial_tree_bulk_load_is_pending: bool,
    /// Final relation rows staged until the next observation boundary. Moving one node updates its
    /// affected neighbours here, so those derived changes need no separate journal ingress.
    pending_tree_rows: StableIterationMap<StyleNodeID, Option<TreeRelations>>,
    pending_first_children: HashMap<StyleNodeID, Option<StyleNodeID>>,
    pending_tree_memory: MemoryLease,
    /// Final activation flags staged until the program commit barrier.
    pending_rule_conditions: PendingField<RuleID, bool>,
    pending_sheet_conditions: PendingField<SheetID, bool>,
    pending_sheet_enabled: PendingField<SheetID, bool>,
    /// Final declaration inventories staged until the program commit barrier.
    pending_rule_declarations: HashMap<RuleID, PendingRuleDeclarations>,
    /// Final immutable rule versions staged until the program commit barrier.
    pending_rule_versions: PendingField<RuleID, RuleVersion>,
    /// Final structural rule flags staged until the program commit barrier.
    pending_rule_in_a_layer: PendingField<RuleID, bool>,
    pending_rule_gated_by_container_query: PendingField<RuleID, bool>,
    /// Final liveness for arriving and departing rules, staged until the program commit barrier.
    pending_rule_liveness: PendingField<RuleID, bool>,
    /// Final layer orders staged until the program commit barrier.
    pending_layer_orders: HashMap<TreeScopeID, HashMap<CascadeLayerID, u32>>,
    /// Scopes staged to begin including document sheets at the commit barrier.
    pending_scopes_using_document_sheets: HashSet<TreeScopeID>,
    /// Final sheet order per changed scope, staged until the program commit barrier.
    pending_sheets_in_scope: PendingField<TreeScopeID, Vec<SheetID>>,
    /// Whether each sheet's pending attachment change carries changes to its rules.
    rule_change_is_carried_by_sheet: HashMap<SheetID, bool>,
    /// The program version before the first program mutation in the pending transaction.
    pending_program_base_version: Option<ProgramVersion>,
    /// Original and final property inventories for declaration edits in the pending transaction.
    pending_rule_declaration_changes: Vec<PendingRuleDeclarationChange>,
    /// The old dense rule sequence while one sheet is synchronously reparsed.
    sheet_rule_replacement: Option<SheetRuleReplacement>,
    /// Unmatched old rule sequences retained until the transaction boundary, indexed by sheet.
    pending_sheet_rule_replacements: Column<Option<SheetRuleReplacement>>,
    /// Elements that left the tree in the transaction being assembled. Their facts are what says
    /// which selectors their departure can reach, so the rows outlive the mutation and are dropped
    /// once the transaction that carries it has been routed.
    departed: Vec<StyleNodeID>,
    match_workspace: MatchEvaluationWorkspace,
    /// Scratch for the fact rows one exact candidate evaluation covers, reused across candidates.
    exact_covered_scratch: Vec<StyleNodeID>,
    /// Monotonic identity assigned to each non-empty normalized style transaction.
    next_style_transaction_version: StyleTransactionVersion,
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
    computed_group_set_memory: MemoryLease,
    custom_property_environment_memory: MemoryLease,
    computed_fixed_metadata_memory: MemoryLease,
    computed_reconstruction_metadata_memory: MemoryLease,
    style_record_memory: MemoryLease,
    animation_overlay_memory: MemoryLease,
    computed_pseudo_assignment_memory: MemoryLease,

    /// One identity per distinct match-answer factor, retained exact factor, or cascade input.
    /// The catalog lives with the document rather than with a traversal, because a per-element ask -
    /// which is what a script reading style gets - opens no traversal, and an identity that only
    /// exists inside one answers the pages that need it least. An id is never reused for a
    /// different answer, so a consumer holding one across a flush is never told the wrong thing.
    match_answers: MatchAnswerCatalog,
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
    /// Once the Tier-3 budget refuses a retained-answer reservation, the rest of the completion
    /// batch stops asking for exact answers: an exact answer costs more to evaluate, and paying
    /// that premium for an answer the controller cannot retain buys nothing on any later flush.
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
    transaction_fact_view: Option<TransactionFactView>,
    facts: ElementFactStore,
    programs: SelectorPrograms,
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
    scope_by_root: HashMap<StyleNodeID, TreeScopeID>,
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
    /// Maps a name's one-word interned identity to its document-local atom. Selector names and DOM
    /// facts key the same table, so a class in a stylesheet and a class on an element compare as
    /// one integer.
    atoms: HashMap<usize, StyleAtomID>,
    /// A qualified name's atom, keyed by the namespace and the local name it is built from.
    ///
    /// An attribute in a namespace is published under this as well as under its local name, and a
    /// selector that names the namespace tests it. Both sides mint from the same sequence as every
    /// other atom, so a qualified name can never collide with a word.
    qualified_atoms: HashMap<(u32, u32), StyleAtomID>,
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

#[cfg(test)]
mod tests;
