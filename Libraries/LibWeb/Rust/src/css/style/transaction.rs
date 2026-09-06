/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Typed semantic inputs and the normalization journal that collapses them. Typing matters:
//! treating every change as a generic version bump would move work from mutation time into an
//! unnecessarily broad lazy validation later, which is exactly the amplification this engine
//! exists to avoid.

use super::fast_hash::FastMap as HashMap;

use super::capacity::capacity_bytes;
use super::index::FeatureValue;
use super::index::LocalFeatureKey;
use super::index::StyleAtomID;
use super::index::StyleNodeFacts;
use super::instrumentation::Counter;
use super::instrumentation::Counters;
use super::memory::MemoryCategory;
use super::memory::MemoryController;
use super::program::ActivationPredicateID;
use super::program::CascadeLayerID;
use super::program::DeclarationBlockID;
use super::program::RuleID;
use super::program::SelectorProgramID;
use super::program::SheetID;
use super::program::StyleScopeID;
use super::tree::StyleNodeID;
use super::tree::TreeScopeID;

/// Version of the attached stylesheet program: compiled selectors, declarations, conditions, and
/// their attachment to style scopes.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord)]
pub struct ProgramVersion(pub u64);

/// Monotonic identity of one normalized style transaction published by a document.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct StyleTransactionVersion(pub u64);

// -- Typed semantic inputs -------------------------------------------------------------------

// An element or document state fact.
//
// States are semantic facts rather than bespoke invalidation entry points, so a transition
// publishes an old and a new value and only operators depending on that state receive it.
//
// Every boolean pseudo-class the parser can produce has an entry here. That exhaustiveness is the
// point: a pseudo-class with no fact would compile to something the engine cannot route, and a
// selector that cannot be routed cannot be invalidated. Facts whose value is a parameter rather
// than a boolean - `:dir()`, `:lang()`, `:state()`, `:heading()` - are operators instead, because
// a bitset cannot carry their argument.
include!(concat!(env!("OUT_DIR"), "/style_state_fact_generated.rs"));

/// Declarations sourced from one style node rather than a stylesheet rule. Each kind keeps its
/// language-defined cascade placement: presentational hints do not masquerade as inline style, and
/// none of them enter the selector program merely because their source syntax is an attribute.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(u8)]
pub enum ElementDeclarationKind {
    /// The `style` attribute.
    InlineStyle,
    /// An HTML presentational-hint attribute.
    PresentationalHint,
    /// An SVG presentation attribute such as `fill` or `stroke`.
    SvgPresentationAttribute,
}

impl ElementDeclarationKind {
    pub const COUNT: usize = Self::SvgPresentationAttribute as usize + 1;
    pub const ALL: [Self; Self::COUNT] = [
        Self::InlineStyle,
        Self::PresentationalHint,
        Self::SvgPresentationAttribute,
    ];

    #[must_use]
    pub const fn index(self) -> usize {
        self as usize
    }
}

/// Which field of a rule an edit touched. Splitting the fields is what lets a declaration edit
/// reuse selector truth, a condition edit reuse both selector truth and declarations, and a layer
/// move reuse everything except cascade priority.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum RuleField {
    /// Whether the rule is attached to its sheet at all.
    Existence,
    Selector,
    Declarations,
    Activation,
    Layer,
    Scope,
}

/// A cascade topology axis. A change here alters priority comparisons for declarations that
/// already matched; it never invalidates selector truth.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum TopologyAxis {
    /// Layer order within one style scope. A layer name belongs to the tree scope its sheet is
    /// attached to, so declaring one in a shadow root reorders nothing outside it.
    LayerOrder(TreeScopeID),
    /// Sheet order within one style scope. Reordering sheets changes cascade priority for
    /// declarations that already matched; it never changes which elements they match.
    SheetOrder(TreeScopeID),
}

/// The tree relations of one style node, as of one side of a mutation.
///
/// This is the minimum old-side information the applicable transpose rules need. It is not a
/// traversable old DOM and does not copy unchanged descendant fields: old-side facts exist to
/// establish a complete impact region, not to reconstruct history.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TreeRelations {
    pub parent: Option<StyleNodeID>,
    pub previous_element_sibling: Option<StyleNodeID>,
    pub next_element_sibling: Option<StyleNodeID>,
    pub tree_scope: TreeScopeID,
    pub assigned_slot: Option<StyleNodeID>,
}

impl TreeRelations {
    /// Relations of a node with no parent, slot, or host: the shape a node has before insertion and
    /// after removal.
    #[must_use]
    #[cfg(test)]
    pub fn detached(tree_scope: TreeScopeID) -> Self {
        Self {
            parent: None,
            previous_element_sibling: None,
            next_element_sibling: None,
            tree_scope,
            assigned_slot: None,
        }
    }
}

macro_rules! define_input_kinds {
    ($($variant:ident => $counter:expr,)+) => {
        /// The typed kind of a semantic input. Coarsening and routing are per kind, so a child-list
        /// mutation never consults programs that depend only on an environment predicate.
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
        #[repr(usize)]
        pub enum InputKind {
            $($variant,)+
        }

        pub const INPUT_KIND_COUNT: usize = 0 $(+ { let _ = InputKind::$variant; 1 })+;

        static INPUT_KIND_VALUES: [InputKind; INPUT_KIND_COUNT] = [$(InputKind::$variant,)+];

        impl InputKind {
            #[must_use]
            pub fn index(self) -> usize {
                self as usize
            }

            #[must_use]
            pub fn counter(self) -> Option<Counter> {
                match self {
                    $(Self::$variant => $counter,)+
                }
            }
        }
    };
}

define_input_kinds! {
    TreeRelations => Some(Counter::TreeDeltas),
    LocalFeature => Some(Counter::LocalFeatureDeltas),
    State => Some(Counter::StateDeltas),
    ElementDeclaration => Some(Counter::ElementDeclarationDeltas),
    ElementStyleInput => None,
    Program => Some(Counter::ProgramDeltas),
    CascadeTopology => Some(Counter::CascadeTopologyDeltas),
    Environment => Some(Counter::EnvironmentDeltas),
}

/// A journal key: a style-node identity plus the typed fact or relation it describes.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum InputKey {
    TreeRelations(StyleNodeID),
    LocalFeature(StyleNodeID, LocalFeatureKey),
    State(StyleNodeID, StateFact),
    ElementDeclaration(StyleNodeID, ElementDeclarationKind),
    /// A non-selector style input owned by the element changed. This is an edge-triggered action,
    /// not retained fact state; its journal value says only whether the action is pending.
    ElementStyleInput(StyleNodeID),
    /// One sheet's attachment to one style scope.
    SheetAttachment(SheetID, TreeScopeID),
    /// Whether a sheet's rules are active at all.
    SheetActivation(SheetID),
    RuleField(RuleID, RuleField),
    /// A registration made through `CSS.registerProperty()`, which has no stylesheet rule identity.
    CustomPropertyRegistration(StyleAtomID),
    CascadeTopology(TopologyAxis),
}

pub const STYLE_REACTION_PUBLISHED_STYLE: u8 = 1 << 0;
pub const STYLE_REACTION_RECOMPUTE_STYLE: u8 = 1 << 1;
pub const STYLE_REACTION_INHERITED_STYLE: u8 = 1 << 2;
pub const STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES: u8 = 1 << 3;
pub const STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES: u8 = 1 << 4;
pub const STYLE_REACTION_ANCESTOR_BECAME_VISIBLE: u8 = 1 << 5;
pub const STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED: u8 = 1 << 6;

impl InputKey {
    #[must_use]
    pub fn kind(self) -> InputKind {
        match self {
            Self::TreeRelations(..) => InputKind::TreeRelations,
            Self::LocalFeature(..) => InputKind::LocalFeature,
            Self::State(..) => InputKind::State,
            Self::ElementDeclaration(..) => InputKind::ElementDeclaration,
            Self::ElementStyleInput(..) => InputKind::ElementStyleInput,
            Self::SheetAttachment(..)
            | Self::SheetActivation(..)
            | Self::RuleField(..)
            | Self::CustomPropertyRegistration(..) => InputKind::Program,
            Self::CascadeTopology(..) => InputKind::CascadeTopology,
        }
    }

    /// The style node this key is about, for keys that name one.
    #[must_use]
    pub fn style_node(self) -> Option<StyleNodeID> {
        match self {
            Self::TreeRelations(node)
            | Self::LocalFeature(node, _)
            | Self::State(node, _)
            | Self::ElementDeclaration(node, _)
            | Self::ElementStyleInput(node) => Some(node),
            Self::SheetAttachment(..)
            | Self::SheetActivation(..)
            | Self::RuleField(..)
            | Self::CustomPropertyRegistration(..)
            | Self::CascadeTopology(..) => None,
        }
    }
}

/// The value of a journal key on one side of a mutation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InputValue {
    /// `None` means the node is not participating in the tree on that side.
    TreeRelations(Option<TreeRelations>),
    Feature(FeatureValue),
    State(bool),
    /// `None` means no declaration block of that kind exists on that side.
    ElementDeclaration(Option<DeclarationBlockID>),
    ElementStyleInput {
        reaction: u8,
        inherited_style_groups: u8,
    },
    /// Whether a sheet is attached to a scope, whether it is enabled, or whether a rule is
    /// attached to its sheet. Position within a scope is not encoded here: an order token is an
    /// identity whose numeric label is deliberately meaningless, and a position index is not stable
    /// against other sheets moving. Reordering publishes a cascade topology change instead.
    Flag(bool),
    SelectorProgram(Option<SelectorProgramID>),
    RuleDeclarations(Option<DeclarationBlockID>),
    ActivationPredicate(Option<ActivationPredicateID>),
    Layer(CascadeLayerID),
    Scope(StyleScopeID),
    /// The version of one topology axis.
    Topology(u64),
}

impl InputValue {
    #[must_use]
    fn kind(self) -> InputKind {
        match self {
            Self::TreeRelations(_) => InputKind::TreeRelations,
            Self::Feature(_) => InputKind::LocalFeature,
            Self::State(_) => InputKind::State,
            Self::ElementDeclaration(_) => InputKind::ElementDeclaration,
            Self::ElementStyleInput { .. } => InputKind::ElementStyleInput,
            Self::Flag(_)
            | Self::SelectorProgram(_)
            | Self::RuleDeclarations(_)
            | Self::ActivationPredicate(_)
            | Self::Layer(_)
            | Self::Scope(_) => InputKind::Program,
            Self::Topology(_) => InputKind::CascadeTopology,
        }
    }
}

/// Asks exact matching to read final authoritative facts for one input kind throughout a complete
/// scope and compare them against last committed outputs. This is still an exact path that
/// emits only actual semantic changes - not a conservative restyle. A marker can represent an input
/// that is inherently scope-wide, or replace fine-grained entries which exceeded their scratch
/// journal capacity.
///
/// The marker currently covers the whole document for its input kind. Narrowing it to a proven
/// common ancestor of the coarsened keys is an optimization, not a correctness requirement: a
/// larger region is always safe, an unknown one never is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CompleteScopeMarker {
    pub kind: InputKind,
}

/// One normalized input change: the pre-transaction value and the final pending value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NormalizedInput {
    pub key: InputKey,
    pub old: InputValue,
    pub new: InputValue,
}

/// One rule-side row produced by changing an input of the active-rule-match join.
///
/// Selector, attachment, and activation changes alter which selector truth joins the rule.
/// Declaration and priority changes retain that truth but alter its downstream cascade row.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ProgramJoinDelta {
    pub input: InputKey,
    pub rule: RuleID,
    pub before_program: Option<SelectorProgramID>,
    pub after_program: Option<SelectorProgramID>,
    pub before_contributes: bool,
    pub after_contributes: bool,
    pub kind: ProgramJoinDeltaKind,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProgramJoinDeltaKind {
    ActiveRuleMatch,
    Declarations,
    Priority,
}

/// The original and final declaration inventories of one edited rule.
///
/// Declaration block identities normalize the semantic input, while these property rows retain
/// the exact key range needed by incremental cascade maintenance.
#[derive(Debug)]
pub struct RuleDeclarationChange {
    pub rule: RuleID,
    pub old_properties: Vec<u16>,
    pub new_properties: Vec<u16>,
    /// Whether the custom properties the rule declares moved, which no winner shows.
    pub custom_declarations_changed: bool,
}

/// The normalized result of one transaction boundary, in deterministic key order.
#[derive(Default)]
#[must_use]
pub struct StyleTransaction {
    pub inputs: Vec<NormalizedInput>,
    pub markers: Vec<CompleteScopeMarker>,
    pub program_joins: Vec<ProgramJoinDelta>,
    pub rule_declaration_changes: Vec<RuleDeclarationChange>,
    pub program_base_version: Option<ProgramVersion>,
    pub(crate) before_facts: Option<StyleNodeFacts>,
    charged_bytes: u64,
}

impl StyleTransaction {
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.inputs.is_empty() && self.markers.is_empty()
    }

    /// Whether any marker replaced fine-grained inputs whose exact identities are unavailable.
    #[must_use]
    pub fn has_coarsened_markers(&self) -> bool {
        self.markers.iter().any(|marker| marker.kind != InputKind::Environment)
    }

    pub fn install_program_joins(&mut self, joins: Vec<ProgramJoinDelta>, memory: &mut MemoryController) {
        debug_assert!(self.program_joins.is_empty());
        let bytes = (joins.capacity() * size_of::<ProgramJoinDelta>()) as u64;
        memory.reserve_required(MemoryCategory::NormalizationJournal, bytes);
        self.charged_bytes = self
            .charged_bytes
            .checked_add(bytes)
            .expect("transaction memory charge overflow");
        self.program_joins = joins;
    }

    pub fn install_rule_declaration_changes(
        &mut self,
        changes: Vec<RuleDeclarationChange>,
        memory: &mut MemoryController,
    ) {
        debug_assert!(self.rule_declaration_changes.is_empty());
        let bytes = (changes.capacity() * size_of::<RuleDeclarationChange>()
            + changes
                .iter()
                .map(|change| (change.old_properties.capacity() + change.new_properties.capacity()) * size_of::<u16>())
                .sum::<usize>()) as u64;
        memory.reserve_required(MemoryCategory::NormalizationJournal, bytes);
        self.charged_bytes = self
            .charged_bytes
            .checked_add(bytes)
            .expect("transaction memory charge overflow");
        self.rule_declaration_changes = changes;
    }

    pub fn install_before_facts(&mut self, before_facts: StyleNodeFacts, memory: &mut MemoryController) {
        debug_assert!(self.before_facts.is_none());
        let bytes = before_facts.capacity_bytes();
        memory.reserve_required(MemoryCategory::NormalizationJournal, bytes);
        self.charged_bytes = self
            .charged_bytes
            .checked_add(bytes)
            .expect("transaction memory charge overflow");
        self.before_facts = Some(before_facts);
    }

    pub fn take_before_facts(&mut self, memory: &mut MemoryController) -> Option<StyleNodeFacts> {
        let before_facts = self.before_facts.take();
        let bytes = before_facts.as_ref().map_or(0, StyleNodeFacts::capacity_bytes);
        memory.release(MemoryCategory::NormalizationJournal, bytes);
        self.charged_bytes = self
            .charged_bytes
            .checked_sub(bytes)
            .expect("transaction fact snapshot charge underflow");
        before_facts
    }

    #[must_use]
    pub fn program_joins_for(&self, input: InputKey) -> &[ProgramJoinDelta] {
        let first = self.program_joins.partition_point(|delta| delta.input < input);
        let count = self.program_joins[first..].partition_point(|delta| delta.input == input);
        &self.program_joins[first..first + count]
    }

    /// Release the transaction's scratch charge. Called once the transaction has been consumed.
    pub fn release(self, memory: &mut MemoryController) {
        memory.release(MemoryCategory::NormalizationJournal, self.charged_bytes);
    }
}

/// Accumulates pending input changes and normalizes them at a transaction boundary.
///
/// Script performs many mutations before style is observed, so the raw mutation count is not the
/// default work multiplier. Adding then removing the same class produces no final delta; changing a
/// declaration three times retains only the old and final blocks; moving a subtree twice retains
/// only its original and final relationships.
///
/// Normalization never combines changes across a required observation boundary. An operation whose
/// Web-platform semantics force synchronous observation drains the journal first.
#[derive(Default)]
pub struct NormalizationJournal {
    entries: HashMap<InputKey, (InputValue, InputValue)>,
    markers: Vec<CompleteScopeMarker>,
    covered: [bool; INPUT_KIND_COUNT],
    charged_bytes: u32,
    document_capacity_limit: u32,
    #[cfg(test)]
    capacity_limit_override: Option<u64>,
}

const JOURNAL_ENTRY_BYTES: usize = size_of::<InputKey>() + 2 * size_of::<InputValue>() + 1;
const JOURNAL_ENTRIES_PER_CONNECTED_ELEMENT: u64 = 4;
const MIN_JOURNAL_CAPACITY_LIMIT: u64 = 4 * 1024 * 1024;
const MAX_JOURNAL_CAPACITY_LIMIT: u64 = 32 * 1024 * 1024;
impl NormalizationJournal {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Fine-grained keys currently pending. Kinds replaced by a complete-scope marker are not
    /// counted here, because their individual keys no longer exist.
    #[must_use]
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty() && self.markers.is_empty()
    }

    #[must_use]
    pub fn markers(&self) -> &[CompleteScopeMarker] {
        &self.markers
    }

    /// The normalized view of the fine-grained inputs which are still pending.
    pub(super) fn inputs(&self) -> impl Iterator<Item = NormalizedInput> + '_ {
        self.entries
            .iter()
            .map(|(&key, &(old, new))| NormalizedInput { key, old, new })
    }

    /// Fold a newer journal into this one without counting its already-recorded inputs a second
    /// time. This retains the original value before either journal and the final value after both.
    pub(super) fn absorb_newer(&mut self, newer: &mut Self, memory: &mut MemoryController, counters: &mut Counters) {
        debug_assert!(self.markers.is_empty());
        debug_assert!(newer.markers.is_empty());
        let newer_entries = std::mem::take(&mut newer.entries);
        memory.release(MemoryCategory::NormalizationJournal, u64::from(newer.charged_bytes));
        newer.charged_bytes = 0;
        newer.covered = [false; INPUT_KIND_COUNT];

        for (key, (old, new)) in newer_entries {
            if self.covered[key.kind().index()] {
                continue;
            }
            if let Some(entry) = self.entries.get_mut(&key) {
                if entry.0 == new {
                    self.entries.remove(&key);
                    counters.bump(Counter::JournalCancellations);
                } else {
                    entry.1 = new;
                }
                continue;
            }
            if old == new || !self.make_room_for_one(key.kind(), memory, counters) {
                continue;
            }
            self.entries.insert(key, (old, new));
            self.settle(memory);
        }
        self.settle(memory);
    }

    #[must_use]
    pub fn contains_only_element_style_inputs(&self) -> bool {
        self.markers.is_empty()
            && !self.entries.is_empty()
            && self
                .entries
                .keys()
                .all(|key| matches!(key, InputKey::ElementStyleInput(..)))
    }

    #[must_use]
    pub fn charged_bytes(&self) -> u64 {
        u64::from(self.charged_bytes)
    }

    #[inline(never)]
    fn capacity_limit(&self) -> u64 {
        #[cfg(test)]
        if let Some(limit) = self.capacity_limit_override {
            return limit;
        }
        u64::from(self.document_capacity_limit.max(MIN_JOURNAL_CAPACITY_LIMIT as u32))
    }

    pub(super) fn set_document_capacity_limit(&mut self, connected_element_count: u32) {
        let limit = (JOURNAL_ENTRY_BYTES as u64)
            .saturating_mul(JOURNAL_ENTRIES_PER_CONNECTED_ELEMENT)
            .saturating_mul(u64::from(connected_element_count))
            .clamp(MIN_JOURNAL_CAPACITY_LIMIT, MAX_JOURNAL_CAPACITY_LIMIT);
        self.document_capacity_limit = u32::try_from(limit).expect("normalization journal limit exceeds u32");
    }

    /// The pre-transaction value already journalled for `key`, if anything has been recorded for it.
    ///
    /// A caller that needs to know whether its own record will cancel has to ask before making it:
    /// cancellation means the input is back where the transaction found it, which for some inputs is
    /// not the same as nothing having happened.
    #[must_use]
    pub fn pending_old(&self, key: InputKey) -> Option<InputValue> {
        self.entries.get(&key).map(|entry| entry.0)
    }

    /// Discard a transaction whose invalidation result is already known to cover its complete scope.
    pub fn discard(&mut self, memory: &mut MemoryController) {
        self.entries.clear();
        self.entries.shrink_to_fit();
        self.markers.clear();
        self.markers.shrink_to_fit();
        self.covered = [false; INPUT_KIND_COUNT];
        memory.release(MemoryCategory::NormalizationJournal, u64::from(self.charged_bytes));
        self.charged_bytes = 0;
    }

    /// Record one input change. `old` is the value before this mutation and is kept only if this is
    /// the first record for `key`; `new` replaces any previously pending value.
    #[inline]
    pub fn record(
        &mut self,
        key: InputKey,
        old: InputValue,
        new: InputValue,
        memory: &mut MemoryController,
        counters: &mut Counters,
    ) {
        let kind = key.kind();
        assert_eq!(old.kind(), kind, "journal value does not match its key");
        assert_eq!(new.kind(), kind, "journal value does not match its key");

        counters.bump(Counter::RawMutationRecords);
        if let Some(counter) = kind.counter() {
            counters.bump(counter);
        }

        // Once a kind has been coarsened, its individual keys carry no further information: the
        // marker's region already contains them. This is what bounds journal memory against a
        // pathological script, which can force broad discovery but not unbounded retention.
        if self.covered[kind.index()] {
            return;
        }

        if let Some(entry) = self.entries.get_mut(&key) {
            if let (
                InputValue::ElementStyleInput {
                    reaction: pending_reaction,
                    inherited_style_groups: pending_inherited_style_groups,
                },
                InputValue::ElementStyleInput {
                    reaction: new_reaction,
                    inherited_style_groups: new_inherited_style_groups,
                },
            ) = (&mut entry.1, new)
            {
                *pending_reaction |= new_reaction;
                *pending_inherited_style_groups |= new_inherited_style_groups;
                return;
            }
            if entry.0 == new {
                // The pending change cancels out against the pre-transaction value.
                self.entries.remove(&key);
                counters.bump(Counter::JournalCancellations);
            } else {
                entry.1 = new;
            }
            return;
        }

        if old == new {
            return;
        }
        if !self.make_room_for_one(kind, memory, counters) {
            return;
        }
        self.entries.insert(key, (old, new));
        self.settle(memory);
    }

    /// Record one input action whose semantic scope is already the complete document.
    pub fn record_complete_scope_action(
        &mut self,
        kind: InputKind,
        memory: &mut MemoryController,
        counters: &mut Counters,
    ) {
        counters.bump(Counter::RawMutationRecords);
        if let Some(counter) = kind.counter() {
            counters.bump(counter);
        }
        self.install_marker(kind, memory);
    }

    /// Normalize and drain. The result is sorted by key so that downstream planning is
    /// deterministic regardless of mutation arrival order.
    pub fn take_transaction(&mut self, memory: &mut MemoryController, counters: &mut Counters) -> StyleTransaction {
        let mut inputs: Vec<NormalizedInput> = self
            .entries
            .drain()
            .map(|(key, (old, new))| NormalizedInput { key, old, new })
            .collect();
        inputs.sort_unstable_by_key(|input| input.key);

        // Facts are normally folded as they are recorded after an element's tree arrival. The DOM
        // publishes an element's immutable tag before connecting it, though, and other producers
        // are allowed to announce facts in either order. Once the complete transaction is visible,
        // fold both orders to the same arriving-facts key: the tree delta already puts the subtree
        // in the plan, while that one key routes the final facts to relational and sibling
        // selectors outside it.
        // Tree-relation keys are unique and already sorted by node in the normalized inputs.
        let arriving_nodes: Vec<StyleNodeID> = inputs
            .iter()
            .filter_map(|input| match (input.key, input.old, input.new) {
                (
                    InputKey::TreeRelations(node),
                    InputValue::TreeRelations(None),
                    InputValue::TreeRelations(Some(_)),
                ) => Some(node),
                _ => None,
            })
            .collect();
        if !arriving_nodes.is_empty() {
            inputs.retain(|input| {
                let Some(node) = input.key.style_node() else {
                    return true;
                };
                if arriving_nodes.binary_search(&node).is_err() {
                    return true;
                }
                match input.key {
                    InputKey::LocalFeature(_, LocalFeatureKey::ArrivingFacts) => false,
                    InputKey::LocalFeature(..) | InputKey::State(..) => {
                        counters.bump(Counter::ArrivingNodeFactsFolded);
                        false
                    }
                    _ => true,
                }
            });
            inputs.extend(arriving_nodes.into_iter().map(|node| NormalizedInput {
                key: InputKey::LocalFeature(node, LocalFeatureKey::ArrivingFacts),
                old: InputValue::Feature(FeatureValue::Absent),
                new: InputValue::Feature(FeatureValue::Present),
            }));
            inputs.sort_unstable_by_key(|input| input.key);
        }

        counters.add(Counter::NormalizedUniqueKeys, inputs.len() as u64);

        let markers = std::mem::take(&mut self.markers);
        self.covered = [false; INPUT_KIND_COUNT];
        self.entries.shrink_to_fit();

        // The scratch charge moves to the transaction, which now owns the drained storage. Both
        // sides stay accounted across the handover.
        memory.release(MemoryCategory::NormalizationJournal, u64::from(self.charged_bytes));
        self.charged_bytes = 0;
        let charged_bytes = (inputs.capacity() * size_of::<NormalizedInput>()
            + markers.capacity() * size_of::<CompleteScopeMarker>()) as u64;
        memory.reserve_required(MemoryCategory::NormalizationJournal, charged_bytes);
        self.settle(memory);

        StyleTransaction {
            inputs,
            markers,
            program_joins: Vec::new(),
            rule_declaration_changes: Vec::new(),
            program_base_version: None,
            before_facts: None,
            charged_bytes,
        }
    }

    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.entries, self.markers];
            cached [];
            nested [];
            skip [self.covered, self.charged_bytes, self.document_capacity_limit];
        }
    }

    /// Ensure one more fine-grained entry may be inserted, coarsening until it fits. Returns false
    /// when the entry must not be journalled at all, in which case its kind has been marked.
    fn make_room_for_one(&mut self, kind: InputKind, memory: &mut MemoryController, counters: &mut Counters) -> bool {
        if self.entries.len() < self.entries.capacity() {
            return true;
        }
        self.make_room_for_one_slow(kind, memory, counters)
    }

    #[cold]
    #[inline(never)]
    fn make_room_for_one_slow(
        &mut self,
        kind: InputKind,
        memory: &mut MemoryController,
        counters: &mut Counters,
    ) -> bool {
        while self.entries.len() >= self.entries.capacity() {
            let growth = (JOURNAL_ENTRY_BYTES * self.entries.capacity().max(4)) as u64;
            if u64::from(self.charged_bytes).saturating_add(growth) <= self.capacity_limit() {
                return true;
            }
            if !self.coarsen_largest_kind(memory, counters) {
                if kind == InputKind::ElementStyleInput {
                    return true;
                }
                // Nothing left to coarsen: mark this kind's scope rather than journalling it.
                if self.install_marker(kind, memory) {
                    counters.bump(Counter::CoarsenedScopeMarkers);
                }
                return false;
            }
            if self.covered[kind.index()] {
                return false;
            }
        }
        true
    }

    /// Replace every fine-grained entry of the most numerous remaining kind with one typed
    /// complete-scope marker. Coarsening the largest kind first keeps a burst of class mutations
    /// from also coarsening unrelated tree relations.
    fn coarsen_largest_kind(&mut self, memory: &mut MemoryController, counters: &mut Counters) -> bool {
        let mut counts = [0_usize; INPUT_KIND_COUNT];
        for key in self.entries.keys() {
            counts[key.kind().index()] += 1;
        }
        let Some((index, _)) = counts
            .iter()
            .enumerate()
            .filter(|&(index, &count)| {
                count > 0 && !self.covered[index] && INPUT_KIND_VALUES[index] != InputKind::ElementStyleInput
            })
            .max_by_key(|&(_, &count)| count)
        else {
            return false;
        };
        if self.install_marker(INPUT_KIND_VALUES[index], memory) {
            counters.bump(Counter::CoarsenedScopeMarkers);
        }
        true
    }

    fn install_marker(&mut self, kind: InputKind, memory: &mut MemoryController) -> bool {
        if self.covered[kind.index()] {
            return false;
        }
        self.covered[kind.index()] = true;
        self.entries.retain(|key, _| key.kind() != kind);
        self.entries.shrink_to_fit();
        self.markers.push(CompleteScopeMarker { kind });
        self.settle(memory);
        true
    }

    /// Reconcile the charge with the containers' actual capacity. Growth is already committed by
    /// the time it can be measured, so Tier 4 reports it rather than refusing it. Capacity growth
    /// is checked against the document-shaped cap before insertion.
    fn settle(&mut self, memory: &mut MemoryController) {
        let current = self.capacity_bytes();
        let charged_bytes = u64::from(self.charged_bytes);
        if current > charged_bytes {
            memory.reserve_required(MemoryCategory::NormalizationJournal, current - charged_bytes);
        } else if charged_bytes > current {
            memory.release(MemoryCategory::NormalizationJournal, charged_bytes - current);
        }
        self.charged_bytes = u32::try_from(current).expect("normalization journal charge exceeds u32");
    }
}

#[cfg(test)]
mod tests {
    use super::super::index::StyleAtomID;
    use super::super::memory::DeviceClass;
    use super::super::memory::Tier;
    use super::*;

    // -- Normalization journal ---------------------------------------------------------------

    struct JournalFixture {
        memory: MemoryController,
        counters: Counters,
        journal: NormalizationJournal,
    }

    impl JournalFixture {
        fn new() -> Self {
            Self {
                memory: MemoryController::new(DeviceClass::ForegroundDesktop),
                counters: Counters::new(),
                journal: NormalizationJournal::new(),
            }
        }

        fn with_journal_cap(bytes: u64) -> Self {
            let mut fixture = Self::new();
            fixture.journal.capacity_limit_override = Some(bytes);
            fixture
        }

        fn record(&mut self, key: InputKey, old: InputValue, new: InputValue) {
            self.journal.record(key, old, new, &mut self.memory, &mut self.counters);
        }

        fn class(&mut self, node: u32, class: u32, old: bool, new: bool) {
            let value = |present: bool| {
                InputValue::Feature(if present {
                    FeatureValue::Present
                } else {
                    FeatureValue::Absent
                })
            };
            self.record(
                InputKey::LocalFeature(StyleNodeID::element(node), LocalFeatureKey::Class(StyleAtomID(class))),
                value(old),
                value(new),
            );
        }

        fn element_style_input(&mut self, node: u32) {
            self.record(
                InputKey::ElementStyleInput(StyleNodeID::element(node)),
                InputValue::ElementStyleInput {
                    reaction: 0,
                    inherited_style_groups: 0,
                },
                InputValue::ElementStyleInput {
                    reaction: STYLE_REACTION_RECOMPUTE_STYLE,
                    inherited_style_groups: 0,
                },
            );
        }

        fn take(&mut self) -> StyleTransaction {
            self.journal.take_transaction(&mut self.memory, &mut self.counters)
        }
    }

    fn relations(parent: u32) -> InputValue {
        InputValue::TreeRelations(Some(TreeRelations {
            parent: Some(StyleNodeID::element(parent)),
            ..TreeRelations::detached(TreeScopeID::DOCUMENT)
        }))
    }

    #[test]
    fn adding_then_removing_the_same_class_produces_no_final_delta() {
        let mut fixture = JournalFixture::new();
        fixture.class(1, 10, false, true);
        assert_eq!(fixture.journal.len(), 1);

        fixture.class(1, 10, true, false);
        assert!(fixture.journal.is_empty());
        assert_eq!(fixture.counters.get(Counter::JournalCancellations), 1);
        assert_eq!(fixture.counters.get(Counter::RawMutationRecords), 2);

        let transaction = fixture.take();
        assert!(transaction.is_empty());
        assert_eq!(fixture.counters.get(Counter::NormalizedUniqueKeys), 0);
        transaction.release(&mut fixture.memory);
    }

    #[test]
    fn repeated_element_style_actions_merge_reactions_and_inherited_groups() {
        let mut fixture = JournalFixture::new();
        let key = InputKey::ElementStyleInput(StyleNodeID::element(1));
        let empty = InputValue::ElementStyleInput {
            reaction: 0,
            inherited_style_groups: 0,
        };
        fixture.record(
            key,
            empty,
            InputValue::ElementStyleInput {
                reaction: STYLE_REACTION_PUBLISHED_STYLE,
                inherited_style_groups: 0b001,
            },
        );
        fixture.record(
            key,
            empty,
            InputValue::ElementStyleInput {
                reaction: STYLE_REACTION_RECOMPUTE_STYLE,
                inherited_style_groups: 0b100,
            },
        );

        let transaction = fixture.take();
        assert_eq!(
            transaction.inputs,
            vec![NormalizedInput {
                key,
                old: empty,
                new: InputValue::ElementStyleInput {
                    reaction: STYLE_REACTION_PUBLISHED_STYLE | STYLE_REACTION_RECOMPUTE_STYLE,
                    inherited_style_groups: 0b101,
                },
            }]
        );
        transaction.release(&mut fixture.memory);
    }

    #[test]
    fn absorbing_a_newer_journal_accepts_non_local_inputs() {
        let mut fixture = JournalFixture::new();
        fixture.class(1, 10, false, true);

        let mut newer = NormalizationJournal::new();
        newer.record(
            InputKey::ElementStyleInput(StyleNodeID::element(1)),
            InputValue::ElementStyleInput {
                reaction: 0,
                inherited_style_groups: 0,
            },
            InputValue::ElementStyleInput {
                reaction: STYLE_REACTION_RECOMPUTE_STYLE,
                inherited_style_groups: 0,
            },
            &mut fixture.memory,
            &mut fixture.counters,
        );

        fixture
            .journal
            .absorb_newer(&mut newer, &mut fixture.memory, &mut fixture.counters);
        assert!(newer.is_empty());

        let transaction = fixture.take();
        assert_eq!(transaction.inputs.len(), 2);
        assert!(
            transaction
                .inputs
                .iter()
                .any(|input| matches!(input.key, InputKey::LocalFeature(..)))
        );
        assert!(
            transaction
                .inputs
                .iter()
                .any(|input| matches!(input.key, InputKey::ElementStyleInput(..)))
        );
        transaction.release(&mut fixture.memory);
    }

    #[test]
    fn repeated_declaration_edits_retain_only_the_old_and_final_blocks() {
        let mut fixture = JournalFixture::new();
        let key = InputKey::ElementDeclaration(StyleNodeID::element(1), ElementDeclarationKind::InlineStyle);
        let block = |id: u32| InputValue::ElementDeclaration(Some(DeclarationBlockID(id)));

        fixture.record(key, block(1), block(2));
        fixture.record(key, block(2), block(3));
        fixture.record(key, block(3), block(4));

        assert_eq!(fixture.journal.len(), 1);

        let transaction = fixture.take();
        assert_eq!(
            transaction.inputs,
            vec![NormalizedInput {
                key,
                old: block(1),
                new: block(4),
            }]
        );
        transaction.release(&mut fixture.memory);
    }

    #[test]
    fn moving_a_subtree_twice_retains_only_its_original_and_final_relationships() {
        let mut fixture = JournalFixture::new();
        let key = InputKey::TreeRelations(StyleNodeID::element(5));

        fixture.record(key, relations(1), relations(2));
        fixture.record(key, relations(2), relations(3));

        let transaction = fixture.take();
        assert_eq!(transaction.inputs.len(), 1);
        assert_eq!(transaction.inputs[0].old, relations(1));
        assert_eq!(transaction.inputs[0].new, relations(3));
        transaction.release(&mut fixture.memory);
    }

    #[test]
    fn facts_published_before_and_after_an_arrival_fold_together() {
        let mut fixture = JournalFixture::new();
        let node = StyleNodeID::element(5);
        fixture.record(
            InputKey::LocalFeature(node, LocalFeatureKey::TagName),
            InputValue::Feature(FeatureValue::Absent),
            InputValue::Feature(FeatureValue::Atom(StyleAtomID(1))),
        );
        fixture.record(
            InputKey::TreeRelations(node),
            InputValue::TreeRelations(None),
            relations(1),
        );
        fixture.class(5, 10, false, true);
        fixture.record(
            InputKey::State(node, StateFact::Hover),
            InputValue::State(false),
            InputValue::State(true),
        );

        let transaction = fixture.take();
        assert_eq!(transaction.inputs.len(), 2);
        assert!(transaction.inputs.iter().any(|input| {
            input.key == InputKey::TreeRelations(node)
                && input.old == InputValue::TreeRelations(None)
                && input.new == relations(1)
        }));
        assert!(transaction.inputs.iter().any(|input| {
            input.key == InputKey::LocalFeature(node, LocalFeatureKey::ArrivingFacts)
                && input.old == InputValue::Feature(FeatureValue::Absent)
                && input.new == InputValue::Feature(FeatureValue::Present)
        }));
        assert_eq!(fixture.counters.get(Counter::ArrivingNodeFactsFolded), 3);
        transaction.release(&mut fixture.memory);
    }

    #[test]
    fn a_mutation_that_changes_nothing_is_counted_but_not_journalled() {
        let mut fixture = JournalFixture::new();
        fixture.class(1, 10, true, true);
        assert!(fixture.journal.is_empty());
        assert_eq!(fixture.counters.get(Counter::RawMutationRecords), 1);
        assert_eq!(fixture.counters.get(Counter::LocalFeatureDeltas), 1);
    }

    #[test]
    fn normalized_output_is_sorted_regardless_of_arrival_order() {
        let mut fixture = JournalFixture::new();
        for node in [7_u32, 2, 9, 4] {
            fixture.class(node, 1, false, true);
        }
        let transaction = fixture.take();
        let nodes: Vec<u32> = transaction
            .inputs
            .iter()
            .map(|input| input.key.style_node().unwrap().raw())
            .collect();
        assert_eq!(nodes, vec![2, 4, 7, 9]);
        transaction.release(&mut fixture.memory);
    }

    #[test]
    fn complete_scope_actions_are_not_counted_as_journal_coarsening() {
        let mut fixture = JournalFixture::new();
        fixture.journal.record_complete_scope_action(
            InputKind::Environment,
            &mut fixture.memory,
            &mut fixture.counters,
        );

        assert_eq!(fixture.counters.get(Counter::RawMutationRecords), 1);
        assert_eq!(fixture.counters.get(Counter::EnvironmentDeltas), 1);
        assert_eq!(fixture.counters.get(Counter::CoarsenedScopeMarkers), 0);
        let transaction = fixture.take();
        assert!(!transaction.has_coarsened_markers());
        assert_eq!(
            transaction.markers,
            vec![CompleteScopeMarker {
                kind: InputKind::Environment
            }]
        );
        transaction.release(&mut fixture.memory);
    }

    #[test]
    fn journal_overflow_coarsens_the_largest_kind_into_a_typed_marker() {
        let mut fixture = JournalFixture::with_journal_cap(4096);
        fixture.record(
            InputKey::TreeRelations(StyleNodeID::element(1)),
            relations(1),
            relations(2),
        );
        fixture.record(
            InputKey::TreeRelations(StyleNodeID::element(2)),
            relations(1),
            relations(2),
        );
        for node in 10..200_u32 {
            fixture.class(node, 1, false, true);
        }

        assert!(
            fixture.counters.get(Counter::CoarsenedScopeMarkers) > 0,
            "a bounded scratch budget must force coarsening"
        );
        assert_eq!(
            fixture.journal.markers(),
            &[CompleteScopeMarker {
                kind: InputKind::LocalFeature
            }],
            "only the kind that overflowed is coarsened"
        );

        let transaction = fixture.take();
        assert!(transaction.has_coarsened_markers());
        // The tree relations keep their exact old and new values; only the class facts widened.
        assert!(
            transaction
                .inputs
                .iter()
                .all(|input| input.key.kind() == InputKind::TreeRelations)
        );
        assert_eq!(transaction.inputs.len(), 2);
        transaction.release(&mut fixture.memory);
    }

    #[test]
    fn element_style_actions_remain_exact_without_scratch_headroom() {
        let mut fixture = JournalFixture::with_journal_cap(0);
        fixture.element_style_input(10);
        fixture.element_style_input(20);

        // Reconstructible facts may still coarsen around edge-triggered actions.
        for node in 30..200 {
            fixture.class(node, 1, false, true);
        }
        fixture.element_style_input(200);

        assert_eq!(
            fixture.journal.markers(),
            &[CompleteScopeMarker {
                kind: InputKind::LocalFeature
            }]
        );
        let transaction = fixture.take();
        assert_eq!(
            transaction
                .inputs
                .iter()
                .filter(|input| input.key.kind() == InputKind::ElementStyleInput)
                .map(|input| input.key)
                .collect::<Vec<_>>(),
            vec![
                InputKey::ElementStyleInput(StyleNodeID::element(10)),
                InputKey::ElementStyleInput(StyleNodeID::element(20)),
                InputKey::ElementStyleInput(StyleNodeID::element(200)),
            ]
        );
        transaction.release(&mut fixture.memory);
    }

    #[test]
    fn a_coarsened_kind_absorbs_further_records_without_growing() {
        let mut fixture = JournalFixture::with_journal_cap(512);
        for node in 10..200_u32 {
            fixture.class(node, 1, false, true);
        }
        let bytes_after_coarsening = fixture.journal.charged_bytes();

        for node in 200..4000_u32 {
            fixture.class(node, 1, false, true);
        }
        assert_eq!(fixture.journal.charged_bytes(), bytes_after_coarsening);
        assert_eq!(fixture.journal.len(), 0);
        assert_eq!(
            fixture.counters.get(Counter::RawMutationRecords),
            3990,
            "every raw mutation is still counted"
        );
    }

    #[test]
    fn draining_hands_the_scratch_charge_over_and_releasing_gives_it_back() {
        let mut fixture = JournalFixture::new();
        for node in 1..50_u32 {
            fixture.class(node, 1, false, true);
        }
        assert!(fixture.memory.bytes_in_tier(Tier::Scratch) > 0);

        let transaction = fixture.take();
        assert_eq!(transaction.inputs.len(), 49);
        assert!(fixture.memory.bytes_in_tier(Tier::Scratch) > 0);

        transaction.release(&mut fixture.memory);
        assert_eq!(
            fixture.memory.bytes_in_tier(Tier::Scratch),
            fixture.journal.charged_bytes()
        );
    }
}
