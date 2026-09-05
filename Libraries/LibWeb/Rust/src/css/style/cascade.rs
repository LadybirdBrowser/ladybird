/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Cascade priority and winner groups.
//!
//! A declaration's priority is a comparison program over stable identities, not a number baked into
//! the declaration. Components that change globally - layer topology, stylesheet order - are
//! referenced indirectly, so moving a layer or a sheet updates one token instead of rewriting every
//! declaration that lives in it.
//!
//! The components, outermost first, follow the cascade sorting order:
//! origin and importance, encapsulation context, element-attached styles, layers, specificity,
//! scope proximity, and order of appearance. Importance reverses two of them - context and layers -
//! which is why importance is folded into the key when it is built rather than checked at every
//! comparison.
//!
//! Winner groups intern the declarations which win for one style node. Thousands of elements with
//! the same cascade answer therefore share one state and retain only a compact handle. States in
//! turn share property-range groups, so changing one winner does not copy every unaffected one.

use super::capacity::ShallowCapacityBytes;
use super::capacity::capacity_bytes;
use super::column::BitColumn;
use super::column::Column;
use super::column::PagedColumn;
use super::column::PagedColumnPage;
use super::column::RemovablePagedColumnPage;
use super::fast_hash::FastMap as HashMap;
use super::fast_hash::fast_hasher;
use super::intern_table::InternIdentity;
use super::intern_table::InternTable;

use std::hash::Hash;
use std::hash::Hasher;

use crate::css::cascaded_properties::CascadeOrigin;

use super::memory::MemoryCategory;
use super::memory::MemoryController;
use super::memory::MemoryLease;
use super::partial_view::Lookup;
use super::program::CascadeLayerID;
use super::program::RuleID;
use super::selector::Specificity;
use super::sorted_merge::SortedMergeEntry;
use super::sorted_merge::merge_sorted_by;
use super::transaction::ElementDeclarationKind;
use super::transaction::ProgramVersion;
use super::tree::PseudoElementTarget;
use super::tree::StyleNodeID;

/// Where an element-sourced declaration sits relative to the rules of its context.
///
/// Element-attached styles are a cascade component in their own right, above layers: a style
/// attribute beats every layered and unlayered rule in its context, whatever layer they are in.
#[derive(Clone, Copy, Debug, Hash, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u8)]
pub enum ElementAttachment {
    /// A declaration from a stylesheet rule.
    Rule = 0,
    /// An HTML presentational hint or SVG presentation attribute mapped to declarations.
    PresentationalHint = 1,
    /// The `style` attribute.
    InlineStyle = 2,
}

/// The precedence ladder of origin and importance, lowest first.
///
/// Importance reverses the origin order, which is why this is one ladder rather than an origin plus
/// a flag: comparing an important user declaration against a normal author one is a comparison of
/// two rungs, not of two dimensions.
#[must_use]
pub fn origin_importance_rank(origin: CascadeOrigin, important: bool) -> u8 {
    match (origin, important) {
        (CascadeOrigin::UserAgent, false) => 0,
        (CascadeOrigin::User, false) => 1,
        (CascadeOrigin::AuthorPresentationalHint, false) => 2,
        (CascadeOrigin::Author, false) => 3,
        (CascadeOrigin::Animation, _) => 4,
        (CascadeOrigin::Author, true) => 5,
        (CascadeOrigin::AuthorPresentationalHint, true) => 6,
        (CascadeOrigin::User, true) => 7,
        (CascadeOrigin::UserAgent, true) => 8,
        (CascadeOrigin::Transition, _) => 9,
    }
}

/// The inputs a priority key is built from, before importance has been folded in.
#[derive(Clone, Copy)]
pub struct PriorityInputs {
    pub origin: CascadeOrigin,
    pub important: bool,
    /// Encapsulation depth: 0 for the document tree, increasing inwards.
    pub context_depth: u32,
    pub element_attachment: ElementAttachment,
    pub layer: CascadeLayerID,
    /// The layer's current rank, read from the layer order rather than copied into the rule.
    pub layer_rank: (u64, u64),
    pub specificity: Specificity,
    /// Distance from the `@scope` root; nearer wins. Zero when not scoped.
    pub scope_proximity: u32,
    /// The rule's position: its sheet's rank, then its own rank within that sheet.
    pub sheet_rank: (u64, u64),
    pub rule_rank: (u64, u64),
}

/// A total order over declarations. Greater wins.
///
/// The field order is the cascade sorting order, and `Ord` compares fields in declaration order, so
/// the comparison program is the struct layout.
#[derive(Clone, Copy, Debug, Hash, PartialEq, Eq, PartialOrd, Ord)]
pub struct CascadePriority {
    origin_importance: u8,
    /// Outer contexts win for normal declarations, inner contexts win for important ones.
    context: u32,
    element_attachment: ElementAttachment,
    /// Later layers win for normal declarations; the order is reversed for important ones.
    layer: (u64, u64),
    specificity: Specificity,
    /// Nearer scope roots win, so proximity is stored inverted.
    scope_proximity: u32,
    sheet_rank: (u64, u64),
    rule_rank: (u64, u64),
}

impl CascadePriority {
    #[must_use]
    pub fn new(inputs: PriorityInputs) -> Self {
        let reverse = |value: (u64, u64)| (u64::MAX - value.0, u64::MAX - value.1);
        Self {
            origin_importance: origin_importance_rank(inputs.origin, inputs.important),
            context: if inputs.important {
                inputs.context_depth
            } else {
                u32::MAX - inputs.context_depth
            },
            element_attachment: inputs.element_attachment,
            layer: if inputs.important {
                reverse(inputs.layer_rank)
            } else {
                inputs.layer_rank
            },
            specificity: inputs.specificity,
            scope_proximity: u32::MAX - inputs.scope_proximity,
            sheet_rank: inputs.sheet_rank,
            rule_rank: inputs.rule_rank,
        }
    }

    pub(super) fn exact_output_placeholder() -> Self {
        Self::new(PriorityInputs {
            origin: CascadeOrigin::Author,
            important: false,
            context_depth: 0,
            element_attachment: ElementAttachment::Rule,
            layer: CascadeLayerID::UNLAYERED,
            layer_rank: (0, 0),
            specificity: Specificity::default(),
            scope_proximity: 0,
            sheet_rank: (0, 0),
            rule_rank: (0, 0),
        })
    }
}

// -- Winners and stopping keys -----------------------------------------------------------------

/// A longhand property identity, matching the property metadata tables.
pub type PropertyID = u16;

const WINNER_GROUP_PROPERTY_COUNT: PropertyID = 32;

/// Canonical identity of a specified value. Two declarations that specify the same value share it,
/// however differently they were written and whichever rule they came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct SpecifiedValueID(pub u64);

define_id! {
    /// Identity of one shared continuation below a CSS-wide cascade operator. Zero means no
    /// continuation is required.
    default pub struct CascadeContinuationID(pub);
}

impl InternIdentity for CascadeContinuationID {
    fn index(self) -> usize {
        self.0 as usize - 1
    }
}

/// A CSS-wide keyword acts as a cascade operator rather than an eagerly flattened value, so its
/// dependency on parent style, origin, and layer topology stays explicit and repairable.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum CascadeOperator {
    /// An ordinary declared value.
    #[default]
    Declared,
    Inherit,
    Initial,
    Unset,
    /// Resumes below the current origin.
    Revert,
    /// Resumes below the current layer in the applicable origin and importance ordering.
    RevertLayer,
}

/// What has to be unchanged for an existing computed value to be reusable without resolving.
///
/// Declaration identity and source position are deliberately absent. They are retained for
/// diagnostics but are not semantic, which is exactly what makes a theme swap that produces
/// identical values free downstream. Transition generation and animation base changes *are*
/// semantic and do participate.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct SpecifiedWinnerKey {
    pub value: SpecifiedValueID,
    pub operator: CascadeOperator,
    /// Where a `revert` or `revert-layer` resumes. Changing the ceiling changes the meaning of the
    /// same written value.
    pub continuation: CascadeContinuationID,
    /// Animation and transition relevance for this property.
    pub animation_relevance: u32,
    /// Whether the declaration overrides an animation at this cascade level.
    pub important: bool,
}

/// The winning declaration for one property of one style node.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum WinnerSource {
    Rule(RuleID),
    Element(ElementDeclarationKind),
    ExactCascade,
}

/// The winning declaration for one property of one style node.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PropertyWinner {
    pub property: PropertyID,
    /// Retained separately from the ordering tuple because semantic state reuse can span an
    /// importance edit, while source publication still has to identify the winning declaration.
    pub important: bool,
    pub key: SpecifiedWinnerKey,
    pub priority: CascadePriority,
    /// Provenance, retained for diagnostics. Not part of any stopping key.
    pub source: WinnerSource,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum CascadeAttachment {
    StyleSheet,
    InlineStyle,
}

/// The priority stratum removed by a CSS-wide continuation.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum CascadeContinuationCeiling {
    Origin {
        origin_group: u8,
        important: bool,
    },
    Layer {
        origin: u8,
        important: bool,
        context: u32,
        layer: CascadeLayerID,
        layer_rank: (u64, u64),
        attachment: CascadeAttachment,
    },
}

/// One interned continuation and the winner reached below its ceiling.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct CascadeContinuation {
    pub ceiling: CascadeContinuationCeiling,
    pub winner: Option<PropertyWinner>,
}

/// The origin, importance, encapsulation context, attachment, and layer occupied by one contender.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CascadeStratum {
    origin: u8,
    origin_group: u8,
    important: bool,
    context: u32,
    layer: CascadeLayerID,
    layer_rank: (u64, u64),
    attachment: CascadeAttachment,
}

impl CascadeStratum {
    #[must_use]
    pub fn new(
        origin: CascadeOrigin,
        important: bool,
        context: u32,
        layer: CascadeLayerID,
        layer_rank: (u64, u64),
        attachment: CascadeAttachment,
    ) -> Self {
        let origin_group = match origin {
            CascadeOrigin::UserAgent => 0,
            CascadeOrigin::User => 1,
            CascadeOrigin::Author | CascadeOrigin::AuthorPresentationalHint => 2,
            CascadeOrigin::Animation => 3,
            CascadeOrigin::Transition => 4,
        };
        Self {
            origin: origin as u8,
            origin_group,
            important,
            context,
            layer,
            layer_rank,
            attachment,
        }
    }

    pub(crate) fn ceiling(self, operator: CascadeOperator) -> Option<CascadeContinuationCeiling> {
        match operator {
            CascadeOperator::Revert => Some(CascadeContinuationCeiling::Origin {
                origin_group: self.origin_group,
                important: self.important,
            }),
            CascadeOperator::RevertLayer => Some(CascadeContinuationCeiling::Layer {
                origin: self.origin,
                important: self.important,
                context: self.context,
                layer: self.layer,
                layer_rank: self.layer_rank,
                attachment: self.attachment,
            }),
            _ => None,
        }
    }

    pub(crate) fn is_below(self, ceiling: CascadeContinuationCeiling) -> bool {
        match ceiling {
            CascadeContinuationCeiling::Origin {
                origin_group,
                important,
            } => self.origin_group != origin_group || self.important != important,
            CascadeContinuationCeiling::Layer {
                origin,
                important: _,
                context,
                layer: _,
                layer_rank,
                attachment,
            } => {
                if self.origin != origin || self.context != context {
                    return true;
                }
                if attachment == CascadeAttachment::InlineStyle {
                    // NB: Inline styles occupy a separate cascade step, even though they share
                    //     the implicit outer layer's rank with unlayered style rules.
                    return self.attachment != CascadeAttachment::InlineStyle;
                }

                // https://drafts.csswg.org/css-cascade-5/#revert-layer
                // The cascaded value is rolled back to the earlier layer, so that the specified
                // value is calculated as if no rules were specified in the current cascade layer.
                self.layer_rank < layer_rank
            }
        }
    }
}

/// One contender supplied to the complete ordered-choice reducer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CascadeCandidate {
    pub winner: PropertyWinner,
    /// Needed only while reducing contenders, never after the winner is retained.
    pub priority: CascadePriority,
    pub stratum: CascadeStratum,
}

/// The semantic winner rows which changed between two cascade states.
///
/// Priority and provenance are deliberately absent. They explain why a declaration won, but a
/// computed value only has to run again when the complete specified-winner key changed.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct CascadeWinnerDelta {
    properties: Vec<PropertyID>,
}

impl CascadeWinnerDelta {
    #[must_use]
    pub fn properties(&self) -> &[PropertyID] {
        &self.properties
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.properties.is_empty()
    }
}

/// One exact replacement in a retained cascade state. `None` restores the implicit winner.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PropertyWinnerUpdate {
    pub property: PropertyID,
    pub winner: Option<PropertyWinner>,
}

/// The greatest cascade candidate for each semantic output key.
///
/// Inputs may arrive in selector-dispatch order or as a complete declaration batch. An index gives
/// both plans constant-time top-1 reduction and lookup, while one deferred sort preserves ordered
/// publication without retaining any losing declaration.
pub(super) struct Top1Cascade<Key, Priority, Payload> {
    winners: Vec<Top1Winner<Key, Priority, Payload>>,
    winner_by_key: HashMap<Key, usize>,
    sorted: bool,
}

pub(super) struct Top1Winner<Key, Priority, Payload> {
    pub(super) key: Key,
    pub(super) priority: Priority,
    pub(super) payload: Payload,
}

impl<Key, Priority, Payload> Top1Cascade<Key, Priority, Payload>
where
    Key: Copy + Hash + Ord,
    Priority: Ord,
{
    #[must_use]
    pub(super) fn with_capacity(capacity: usize) -> Self {
        Self {
            winners: Vec::with_capacity(capacity),
            winner_by_key: HashMap::with_capacity_and_hasher(capacity, Default::default()),
            sorted: true,
        }
    }

    pub(super) fn clear(&mut self) {
        self.winners.clear();
        self.winner_by_key.clear();
        self.sorted = true;
    }

    /// Join one candidate into the top-1 relation. A later equal-priority row wins, matching
    /// cascade source-order tie breaking after the priority program has compared equal.
    pub(super) fn consider(&mut self, key: Key, priority: Priority, payload: Payload) {
        match self.winner_by_key.get(&key).copied() {
            Some(index) if priority >= self.winners[index].priority => {
                self.winners[index] = Top1Winner { key, priority, payload };
            }
            Some(_) => {}
            None => {
                let index = self.winners.len();
                self.winners.push(Top1Winner { key, priority, payload });
                self.winner_by_key.insert(key, index);
                self.sorted = false;
            }
        }
    }

    #[must_use]
    pub(super) fn winner(&self, key: &Key) -> Option<&Top1Winner<Key, Priority, Payload>> {
        self.winner_by_key.get(key).map(|&index| &self.winners[index])
    }

    pub(super) fn winners(&mut self) -> impl Iterator<Item = &Top1Winner<Key, Priority, Payload>> {
        if !self.sorted {
            self.winners.sort_unstable_by_key(|winner| winner.key);
            for (index, winner) in self.winners.iter().enumerate() {
                *self
                    .winner_by_key
                    .get_mut(&winner.key)
                    .expect("every top-1 winner has an index") = index;
            }
            self.sorted = true;
        }
        self.winners.iter()
    }

    #[must_use]
    pub(super) fn capacity_bytes(&self) -> usize {
        (capacity_bytes! {
            shallow [self.winners, self.winner_by_key];
            cached [];
            nested [];
            skip [self.sorted];
        }) as usize
    }
}

define_id! {
    /// Identity of an interned winner group.
    pub struct WinnerGroupID(pub);
}

impl InternIdentity for WinnerGroupID {
    fn index(self) -> usize {
        self.0 as usize
    }
}

define_id! {
    /// Identity of provenance parallel to one interned winner group.
    struct WinnerProvenanceGroupID(pub);
}

impl InternIdentity for WinnerProvenanceGroupID {
    fn index(self) -> usize {
        self.0 as usize
    }
}

define_id! {
    /// Identity of one exact priority retained by winner provenance.
    struct CascadePriorityID(pub);
}

impl InternIdentity for CascadePriorityID {
    fn index(self) -> usize {
        self.0 as usize
    }
}

define_id! {
    /// Identity of one factorized sparse cascade state.
    pub struct CascadeStateID(pub);
}

impl InternIdentity for CascadeStateID {
    fn index(self) -> usize {
        self.0 as usize
    }
}

fn content_hash(content: impl Hash) -> u64 {
    let mut hasher = fast_hasher();
    content.hash(&mut hasher);
    hasher.finish()
}

/// Whether a winner-group lookup requires topology-dependent priorities to still be current.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum WinnerPriorityCoverage {
    Retained,
    Current,
}

/// The coverage required to reuse one node's retained cascade answer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct WinnerGroupKey {
    pub(super) node: StyleNodeID,
    pub(super) pseudo: Option<PseudoElementTarget>,
    pub(super) program_version: ProgramVersion,
    pub(super) priority: WinnerPriorityCoverage,
}

impl WinnerGroupKey {
    #[must_use]
    pub fn retained(node: StyleNodeID, program_version: ProgramVersion) -> Self {
        Self {
            node,
            pseudo: None,
            program_version,
            priority: WinnerPriorityCoverage::Retained,
        }
    }

    #[must_use]
    pub fn current(node: StyleNodeID, program_version: ProgramVersion) -> Self {
        Self {
            node,
            pseudo: None,
            program_version,
            priority: WinnerPriorityCoverage::Current,
        }
    }

    #[must_use]
    pub fn current_pseudo(node: StyleNodeID, pseudo: PseudoElementTarget, program_version: ProgramVersion) -> Self {
        Self {
            node,
            pseudo: Some(pseudo),
            program_version,
            priority: WinnerPriorityCoverage::Current,
        }
    }
}

/// The exact per-node coverage gap preventing reuse of a retained winner group.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum WinnerGroupGap {
    MissingNode(StyleNodeID),
    StaleProgram {
        node: StyleNodeID,
        retained: ProgramVersion,
        required: ProgramVersion,
    },
    StalePriority(StyleNodeID),
}

/// The exact aggregate coverage gap preventing a whole-column proof.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum WinnerGroupCoverageGap {
    StaleProgram {
        retained: ProgramVersion,
        required: ProgramVersion,
    },
    InsufficientProgramRows {
        retained: usize,
        required: usize,
    },
    InsufficientPriorityRows {
        retained: usize,
        required: usize,
    },
}

/// Why an interned group token could not be restored after an answer-cache hit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum WinnerGroupTokenGap {
    StaleGeneration { retained: u64, current: u64 },
    AdmissionClosed,
}

const WINNER_RULE_PAGE_SHIFT: usize = 6;
const WINNER_RULE_PAGE_SIZE: usize = 1 << WINNER_RULE_PAGE_SHIFT;
const WINNER_RULE_NODE_LIMIT: usize = 32;

struct WinnerRuleIndexPage {
    entries: [u32; WINNER_RULE_PAGE_SIZE],
}

impl Default for WinnerRuleIndexPage {
    fn default() -> Self {
        Self {
            entries: [u32::MAX; WINNER_RULE_PAGE_SIZE],
        }
    }
}

impl PagedColumnPage for WinnerRuleIndexPage {
    type Value = u32;

    const SHIFT: usize = WINNER_RULE_PAGE_SHIFT;

    fn get(&self, index: usize) -> Option<u32> {
        (self.entries[index] != u32::MAX).then_some(self.entries[index])
    }

    fn insert(&mut self, index: usize, value: u32) {
        self.entries[index] = value;
    }
}

impl RemovablePagedColumnPage for WinnerRuleIndexPage {
    fn remove(&mut self, index: usize) -> Option<u32> {
        let previous = self.get(index)?;
        self.entries[index] = u32::MAX;
        Some(previous)
    }
}

#[derive(Clone, Copy)]
struct WinnerRuleNodeReference {
    node: StyleNodeID,
    references: u32,
}

#[derive(Clone)]
struct WinnerRuleReferenceEntry {
    rule: RuleID,
    references: u64,
    nodes: Option<Vec<WinnerRuleNodeReference>>,
}

/// A safe dense-and-sparse set of rules whose declarations currently win somewhere.
///
/// The sparse index is paged instead of reading uninitialized memory. Rule identity ranges without
/// winners allocate no page, while the dense entries keep reference-count updates cache-local.
#[derive(Default)]
struct WinnerRuleReferences {
    indices: PagedColumn<WinnerRuleIndexPage>,
    entries: Vec<WinnerRuleReferenceEntry>,
    accounted_dense_len: usize,
    accounted_dense_capacity: usize,
    posting_bytes: u64,
}

impl Clone for WinnerRuleReferences {
    fn clone(&self) -> Self {
        let mut clone = Self {
            accounted_dense_len: self.accounted_dense_len,
            accounted_dense_capacity: self.accounted_dense_capacity,
            ..Self::default()
        };
        clone.entries.reserve(self.entries.len());
        for entry in &self.entries {
            let index = u32::try_from(clone.entries.len()).expect("winner rule inventory exhausted");
            clone.entries.push(entry.clone());
            clone.indices.insert(entry.rule.0 as usize, index);
        }
        clone.posting_bytes = clone
            .entries
            .iter()
            .filter_map(|entry| entry.nodes.as_ref())
            .map(|nodes| (nodes.capacity() * size_of::<WinnerRuleNodeReference>()) as u64)
            .sum();
        clone
    }
}

impl WinnerRuleReferences {
    fn retain(&mut self, rule: RuleID) {
        self.account_for_dense_rule(rule);
        if let Some(index) = self.indices.get(rule.0 as usize) {
            self.entries[index as usize].references += 1;
            return;
        }
        let index = u32::try_from(self.entries.len()).expect("winner rule inventory exhausted");
        self.entries.push(WinnerRuleReferenceEntry {
            rule,
            references: 1,
            nodes: Some(Vec::new()),
        });
        let (previous, _) = self.indices.insert(rule.0 as usize, index);
        debug_assert!(previous.is_none());
    }

    fn release(&mut self, rule: RuleID) {
        self.account_for_dense_rule(rule);
        let index = self
            .indices
            .get(rule.0 as usize)
            .expect("a released winner rule must be retained") as usize;
        self.entries[index].references -= 1;
        if self.entries[index].references != 0 {
            return;
        }
        let released_posting_bytes = self.entries[index].nodes.as_ref().map_or(0, |nodes| {
            (nodes.capacity() * size_of::<WinnerRuleNodeReference>()) as u64
        });
        self.posting_bytes = self
            .posting_bytes
            .checked_sub(released_posting_bytes)
            .expect("winner rule node posting byte count underflow");
        self.indices.remove(rule.0 as usize);
        self.entries.swap_remove(index);
        if let Some(moved) = self.entries.get(index) {
            self.indices.insert(moved.rule.0 as usize, index as u32);
        }
    }

    fn contains(&self, rule: RuleID) -> bool {
        self.indices.get(rule.0 as usize).is_some()
    }

    fn retain_node(&mut self, rule: RuleID, node: StyleNodeID) {
        let index = self
            .indices
            .get(rule.0 as usize)
            .expect("a winning node must reference a retained winner rule") as usize;
        let Some(nodes) = &mut self.entries[index].nodes else {
            return;
        };
        let capacity_before = nodes.capacity();
        match nodes.binary_search_by_key(&node, |reference| reference.node) {
            Ok(index) => nodes[index].references += 1,
            Err(_) if nodes.len() == WINNER_RULE_NODE_LIMIT => self.entries[index].nodes = None,
            Err(index) => nodes.insert(index, WinnerRuleNodeReference { node, references: 1 }),
        }
        let capacity_after = self.entries[index].nodes.as_ref().map_or(0, Vec::capacity);
        self.posting_bytes = self
            .posting_bytes
            .checked_sub((capacity_before * size_of::<WinnerRuleNodeReference>()) as u64)
            .and_then(|bytes| bytes.checked_add((capacity_after * size_of::<WinnerRuleNodeReference>()) as u64))
            .expect("winner rule node posting byte count overflow");
    }

    fn release_node(&mut self, rule: RuleID, node: StyleNodeID) {
        let index = self
            .indices
            .get(rule.0 as usize)
            .expect("a released winning node must reference a retained winner rule") as usize;
        let Some(nodes) = &mut self.entries[index].nodes else {
            return;
        };
        let node_index = nodes
            .binary_search_by_key(&node, |reference| reference.node)
            .expect("a released winning node must be retained");
        nodes[node_index].references -= 1;
        if nodes[node_index].references == 0 {
            nodes.remove(node_index);
        }
    }

    fn nodes(&self, rule: RuleID) -> Option<impl Iterator<Item = StyleNodeID> + '_> {
        let index = self.indices.get(rule.0 as usize)? as usize;
        Some(
            self.entries[index]
                .nodes
                .as_ref()?
                .iter()
                .map(|reference| reference.node),
        )
    }

    fn account_for_dense_rule(&mut self, rule: RuleID) {
        let required = (rule.0 as usize)
            .checked_add(1)
            .expect("winner rule identity space exhausted");
        if required <= self.accounted_dense_len {
            return;
        }
        if required > self.accounted_dense_capacity {
            self.accounted_dense_capacity = required.max(self.accounted_dense_capacity.saturating_mul(2)).max(4);
        }
        self.accounted_dense_len = required;
    }

    #[cfg(test)]
    fn measured_posting_bytes(&self) -> u64 {
        self.entries
            .iter()
            .filter_map(|entry| entry.nodes.as_ref())
            .map(|nodes| (nodes.capacity() * size_of::<WinnerRuleNodeReference>()) as u64)
            .sum()
    }
}

impl ShallowCapacityBytes for WinnerRuleReferences {
    fn shallow_capacity_bytes(&self) -> u64 {
        // Preserve the old dense column's memory admission decisions. Those decisions affect which
        // correctness-neutral caches stay warm, so making this inventory physically smaller must
        // not give an extreme page a larger effective cache budget.
        let dense_bytes = (self.accounted_dense_capacity * size_of::<u64>()) as u64;
        dense_bytes.max(self.indices.capacity_bytes() + self.entries.shallow_capacity_bytes() + self.posting_bytes)
    }
}

/// Interned per-node winning declarations.
///
/// Sparse and shared: never one heap object per property per element. A node retains one state
/// identity, and states share property-range winner groups. The whole structure is Tier-3, so
/// evicting it changes no semantic version and a later observer reconstructs a state from the
/// node's cascade input or from the exact cold cascade.
pub struct WinnerGroups {
    states: InternTable<CascadeStateID, Vec<WinnerGroupRef>>,
    state_reference_counts: Vec<u32>,
    state_winning_rules: Vec<Vec<RuleID>>,
    groups: InternTable<WinnerGroupID, WinnerGroup>,
    provenance_groups: InternTable<WinnerProvenanceGroupID, Vec<WinnerProvenance>>,
    priorities: InternTable<CascadePriorityID, CascadePriority>,
    continuations: InternTable<CascadeContinuationID, CascadeContinuation>,
    winner_entry_count: usize,
    winner_rule_references: WinnerRuleReferences,
    column: Column<Option<(CascadeStateID, ProgramVersion)>>,
    /// The flush that published each node's row: a row published in the current flush holds the
    /// cascade of the node's current answer.
    stamps: Column<u64>,
    stamp: u64,
    pseudo_rows_by_node: Column<Vec<PseudoWinnerRow>>,
    pseudo_row_capacity_bytes: u64,
    priority_current: BitColumn,
    row_count: usize,
    priority_current_row_count: usize,
    newest_program_version: ProgramVersion,
    newest_version_row_count: usize,
    generation: u64,
    admitting: bool,
    residency: MemoryLease,
    nested_residency: MemoryLease,
    #[cfg(test)]
    group_hash_computations: usize,
}

#[derive(Clone)]
struct PseudoWinnerRow {
    pseudo: PseudoElementTarget,
    state: (CascadeStateID, ProgramVersion),
    priority_current: bool,
    /// The flush that published the row's state.
    stamp: u64,
}

#[derive(Clone)]
struct WinnerGroup {
    winners: Vec<SemanticPropertyWinner>,
    content_hash: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct SemanticPropertyWinner {
    property: PropertyID,
    key: SpecifiedWinnerKey,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct WinnerProvenance {
    important: bool,
    source: WinnerSource,
    priority: CascadePriorityID,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct WinnerGroupRef {
    winners: WinnerGroupID,
    provenance: WinnerProvenanceGroupID,
}

impl Default for WinnerGroups {
    fn default() -> Self {
        Self {
            states: InternTable::default(),
            state_reference_counts: Vec::new(),
            state_winning_rules: Vec::new(),
            groups: InternTable::default(),
            provenance_groups: InternTable::default(),
            priorities: InternTable::default(),
            continuations: InternTable::default(),
            winner_entry_count: 0,
            winner_rule_references: WinnerRuleReferences::default(),
            column: Column::default(),
            stamps: Column::default(),
            stamp: 0,
            pseudo_rows_by_node: Column::default(),
            pseudo_row_capacity_bytes: 0,
            priority_current: BitColumn::default(),
            row_count: 0,
            priority_current_row_count: 0,
            newest_program_version: ProgramVersion::default(),
            newest_version_row_count: 0,
            generation: 0,
            admitting: true,
            residency: MemoryLease::new(MemoryCategory::CascadeWinnerGroup),
            nested_residency: MemoryLease::new(MemoryCategory::CascadeWinnerGroup),
            #[cfg(test)]
            group_hash_computations: 0,
        }
    }
}

impl WinnerGroups {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    pub(super) fn verification_copy(&self) -> Self {
        let pseudo_rows_by_node = self.pseudo_rows_by_node.clone();
        let pseudo_row_capacity_bytes = pseudo_rows_by_node
            .iter()
            .map(|rows| rows.capacity() * size_of::<PseudoWinnerRow>())
            .sum::<usize>() as u64;
        Self {
            states: self.states.clone(),
            state_reference_counts: self.state_reference_counts.clone(),
            state_winning_rules: self.state_winning_rules.clone(),
            groups: self.groups.clone(),
            provenance_groups: self.provenance_groups.clone(),
            priorities: self.priorities.clone(),
            continuations: self.continuations.clone(),
            winner_entry_count: self.winner_entry_count,
            winner_rule_references: self.winner_rule_references.clone(),
            column: self.column.clone(),
            stamps: self.stamps.clone(),
            stamp: self.stamp,
            pseudo_rows_by_node,
            pseudo_row_capacity_bytes,
            priority_current: self.priority_current.clone(),
            row_count: self.row_count,
            priority_current_row_count: self.priority_current_row_count,
            newest_program_version: self.newest_program_version,
            newest_version_row_count: self.newest_version_row_count,
            generation: self.generation,
            admitting: self.admitting,
            residency: MemoryLease::new(MemoryCategory::CascadeWinnerGroup),
            nested_residency: MemoryLease::new(MemoryCategory::CascadeWinnerGroup),
            #[cfg(test)]
            group_hash_computations: self.group_hash_computations,
        }
    }

    pub(super) fn node_rows_are_semantically_equal(
        &self,
        other: &Self,
        node: StyleNodeID,
        program_version: ProgramVersion,
    ) -> bool {
        let key = WinnerGroupKey::current(node, program_version);
        let current_rows_are_equal = match (self.token_for(key), other.token_for(key)) {
            (Lookup::Known((_, left)), Lookup::Known((_, right))) => other.semantic_delta(Some(left), right).is_empty(),
            (Lookup::Missing(_), Lookup::Missing(_)) => true,
            (Lookup::Known(_), Lookup::Missing(_)) | (Lookup::Missing(_), Lookup::Known(_)) => false,
            (Lookup::KnownAbsent, _) | (_, Lookup::KnownAbsent) => unreachable!("winner groups are sparse"),
        };
        if !current_rows_are_equal {
            return false;
        }

        let mut left: Vec<_> = self.pseudo_states(node).collect();
        let mut right: Vec<_> = other.pseudo_states(node).collect();
        left.sort_unstable_by_key(|row| row.0);
        right.sort_unstable_by_key(|row| row.0);
        left.iter().all(|&(left_pseudo, _, left_state, left_current)| {
            right
                .binary_search_by_key(&left_pseudo, |row| row.0)
                .ok()
                .is_some_and(|index| {
                    let (_, _, right_state, right_current) = right[index];
                    left_current == right_current && other.semantic_delta(Some(left_state), right_state).is_empty()
                })
        })
    }

    /// Resolve one property's ordered contenders and intern only the continuation payloads needed
    /// by `revert` and `revert-layer` operators.
    pub fn resolve_candidates(&mut self, candidates: &mut [CascadeCandidate]) -> Option<PropertyWinner> {
        // CascadePriority is the total declaration order, so equal keys are interchangeable here.
        candidates.sort_unstable_by_key(|candidate| candidate.priority);
        self.resolve_candidates_below(candidates, &mut Vec::new())
    }

    fn resolve_candidates_below(
        &mut self,
        candidates: &[CascadeCandidate],
        ceilings: &mut Vec<CascadeContinuationCeiling>,
    ) -> Option<PropertyWinner> {
        let candidate = candidates
            .iter()
            .rev()
            .copied()
            .find(|candidate| ceilings.iter().all(|&ceiling| candidate.stratum.is_below(ceiling)))?;
        let mut winner = candidate.winner;
        winner.priority = candidate.priority;
        let Some(ceiling) = candidate.stratum.ceiling(candidate.winner.key.operator) else {
            return Some(winner);
        };
        ceilings.push(ceiling);
        let continuation_winner = self.resolve_candidates_below(candidates, ceilings);
        ceilings.pop();
        let continuation = self.intern_continuation(CascadeContinuation {
            ceiling,
            winner: continuation_winner,
        });
        winner.key.continuation = continuation;
        Some(winner)
    }

    fn intern_continuation(&mut self, continuation: CascadeContinuation) -> CascadeContinuationID {
        let hash = content_hash(continuation);
        if let Some(id) = self
            .continuations
            .find(hash, |_id, candidate| *candidate == continuation)
        {
            return id;
        }
        let id = CascadeContinuationID(
            u32::try_from(self.continuations.len() + 1).expect("cascade continuation space exhausted"),
        );
        self.continuations.insert(hash, id, continuation);
        id
    }

    #[must_use]
    pub fn continuation(&self, id: CascadeContinuationID) -> Option<CascadeContinuation> {
        (id != CascadeContinuationID::default()).then(|| self.continuations[id])
    }

    /// Follow CSS-wide continuations to the declaration consumed by the legacy exact cascade.
    #[must_use]
    pub fn resolved_winner(&self, mut winner: PropertyWinner) -> Option<PropertyWinner> {
        loop {
            let Some(continuation) = self.continuation(winner.key.continuation) else {
                return Some(winner);
            };
            winner = continuation.winner?;
        }
    }

    #[must_use]
    #[cfg(test)]
    pub fn continuation_count(&self) -> usize {
        self.continuations.len()
    }

    /// Intern an already sorted state, reusing unchanged groups directly from its previous state.
    pub fn intern_sorted(&mut self, winners: &[PropertyWinner], previous: Option<CascadeStateID>) -> CascadeStateID {
        debug_assert!(winners.windows(2).all(|pair| pair[0].property < pair[1].property));
        let mut groups = Vec::new();
        let mut start = 0;
        let mut previous_group_index = 0;
        while start < winners.len() {
            let bucket = winners[start].property / WINNER_GROUP_PROPERTY_COUNT;
            let mut end = start + 1;
            while end < winners.len() && winners[end].property / WINNER_GROUP_PROPERTY_COUNT == bucket {
                end += 1;
            }
            let previous_group = previous.and_then(|previous| {
                let previous_groups = &self.states[previous];
                while previous_group_index < previous_groups.len()
                    && self.group_bucket(previous_groups[previous_group_index]) < bucket
                {
                    previous_group_index += 1;
                }
                previous_groups
                    .get(previous_group_index)
                    .copied()
                    .filter(|&group| self.group_bucket(group) == bucket)
            });
            let group = previous_group
                .filter(|&group| self.group_matches(group, &winners[start..end]))
                .unwrap_or_else(|| self.intern_group(&winners[start..end]));
            groups.push(group);
            start = end;
        }
        if let Some(previous) = previous
            && self.states[previous] == groups
        {
            return previous;
        }
        self.intern_group_ids(groups)
    }

    fn intern_group_ids(&mut self, groups: Vec<WinnerGroupRef>) -> CascadeStateID {
        let hash = content_hash(&groups);
        if let Some(id) = self.states.find(hash, |_id, candidate| *candidate == groups) {
            return id;
        }
        let id = CascadeStateID(u32::try_from(self.states.len()).expect("cascade state space exhausted"));
        let mut winning_rules = Vec::new();
        for &group in &groups {
            for mut winner in self.group_winners(group) {
                loop {
                    if let WinnerSource::Rule(rule) = winner.source {
                        winning_rules.push(rule);
                    }
                    let Some(continuation) = self.continuation(winner.key.continuation) else {
                        break;
                    };
                    let Some(next) = continuation.winner else {
                        break;
                    };
                    winner = next;
                }
            }
        }
        winning_rules.sort_unstable();
        winning_rules.dedup();
        self.nested_residency.grow_committed(
            (groups.capacity() * size_of::<WinnerGroupRef>() + winning_rules.capacity() * size_of::<RuleID>()) as u64,
        );
        self.states.insert(hash, id, groups);
        self.state_reference_counts.push(0);
        self.state_winning_rules.push(winning_rules);
        id
    }

    /// Apply exact property replacements without rebuilding unaffected winner groups.
    ///
    /// Updates are ordered and unique by property. The caller is responsible for performing any
    /// contender repair needed to produce the replacement winner.
    pub fn apply_property_updates(
        &mut self,
        previous: CascadeStateID,
        updates: &[PropertyWinnerUpdate],
    ) -> (CascadeStateID, CascadeWinnerDelta) {
        debug_assert!(updates.windows(2).all(|pair| pair[0].property < pair[1].property));
        debug_assert!(
            updates
                .iter()
                .all(|update| update.winner.is_none_or(|winner| winner.property == update.property))
        );
        if updates.is_empty() {
            return (previous, CascadeWinnerDelta::default());
        }

        let mut groups = self.states[previous].clone();
        let mut changed_properties = Vec::new();
        let mut update_start = 0;
        while update_start < updates.len() {
            let bucket = updates[update_start].property / WINNER_GROUP_PROPERTY_COUNT;
            let update_end = update_start
                + updates[update_start..]
                    .partition_point(|update| update.property / WINNER_GROUP_PROPERTY_COUNT == bucket);
            let group_index = groups.partition_point(|&group| self.group_bucket(group) < bucket);
            let old_group = groups
                .get(group_index)
                .copied()
                .filter(|&group| self.group_bucket(group) == bucket);
            let old_winners: Vec<PropertyWinner> = old_group
                .map(|group| self.group_winners(group).collect())
                .unwrap_or_default();
            let bucket_updates = &updates[update_start..update_end];
            let mut winners = Vec::with_capacity(old_winners.len() + bucket_updates.len());
            for entry in merge_sorted_by(&old_winners, bucket_updates, |old, update| {
                old.property.cmp(&update.property)
            }) {
                match entry {
                    SortedMergeEntry::Both(old, update) => match update.winner {
                        Some(winner) => {
                            if old.key != winner.key {
                                changed_properties.push(update.property);
                            }
                            winners.push(winner);
                        }
                        None => changed_properties.push(update.property),
                    },
                    SortedMergeEntry::Left(old) => {
                        winners.push(*old);
                    }
                    SortedMergeEntry::Right(update) => {
                        if let Some(winner) = update.winner {
                            winners.push(winner);
                            changed_properties.push(update.property);
                        }
                    }
                }
            }

            match (old_group, winners.is_empty()) {
                (Some(_), true) => {
                    groups.remove(group_index);
                }
                (Some(old_group), false) => {
                    let new_group = if self.group_matches(old_group, &winners) {
                        old_group
                    } else {
                        self.intern_group(&winners)
                    };
                    groups[group_index] = new_group;
                }
                (None, false) => {
                    let new_group = self.intern_group(&winners);
                    groups.insert(group_index, new_group);
                }
                (None, true) => {}
            }
            update_start = update_end;
        }

        let state = if self.states[previous] == groups {
            previous
        } else {
            self.intern_group_ids(groups)
        };
        (
            state,
            CascadeWinnerDelta {
                properties: changed_properties,
            },
        )
    }

    fn intern_group(&mut self, winners: &[PropertyWinner]) -> WinnerGroupRef {
        let semantic: Vec<_> = winners
            .iter()
            .map(|winner| SemanticPropertyWinner {
                property: winner.property,
                key: winner.key,
            })
            .collect();
        let provenance: Vec<_> = winners
            .iter()
            .map(|winner| WinnerProvenance {
                important: winner.important,
                source: winner.source,
                priority: self.intern_priority(winner.priority),
            })
            .collect();
        let hash = content_hash(&semantic);
        #[cfg(test)]
        {
            self.group_hash_computations += 1;
        }
        if let Some(id) = self.groups.find(hash, |_id, group| {
            group.content_hash == hash && group.winners == semantic
        }) {
            return WinnerGroupRef {
                winners: id,
                provenance: self.intern_provenance_group(provenance),
            };
        }
        let id = WinnerGroupID(u32::try_from(self.groups.len()).expect("winner group space exhausted"));
        self.winner_entry_count += semantic.len();
        self.nested_residency
            .grow_committed((semantic.capacity() * size_of::<SemanticPropertyWinner>()) as u64);
        self.groups.insert(
            hash,
            id,
            WinnerGroup {
                winners: semantic,
                content_hash: hash,
            },
        );
        WinnerGroupRef {
            winners: id,
            provenance: self.intern_provenance_group(provenance),
        }
    }

    fn intern_provenance_group(&mut self, provenance: Vec<WinnerProvenance>) -> WinnerProvenanceGroupID {
        let hash = content_hash(&provenance);
        if let Some(id) = self
            .provenance_groups
            .find(hash, |_id, candidate| *candidate == provenance)
        {
            return id;
        }
        let id = WinnerProvenanceGroupID(
            u32::try_from(self.provenance_groups.len()).expect("winner provenance group space exhausted"),
        );
        self.nested_residency
            .grow_committed((provenance.capacity() * size_of::<WinnerProvenance>()) as u64);
        self.provenance_groups.insert(hash, id, provenance);
        id
    }

    fn intern_priority(&mut self, priority: CascadePriority) -> CascadePriorityID {
        let hash = content_hash(priority);
        if let Some(id) = self.priorities.find(hash, |_id, candidate| *candidate == priority) {
            return id;
        }
        let id = CascadePriorityID(u32::try_from(self.priorities.len()).expect("cascade priority space exhausted"));
        self.priorities.insert(hash, id, priority);
        id
    }

    fn group_matches(&self, group: WinnerGroupRef, winners: &[PropertyWinner]) -> bool {
        self.group_winners(group).eq(winners.iter().copied())
    }

    fn group_winners(&self, group: WinnerGroupRef) -> impl Iterator<Item = PropertyWinner> + '_ {
        self.groups[group.winners]
            .winners
            .iter()
            .zip(&self.provenance_groups[group.provenance])
            .map(|(winner, provenance)| PropertyWinner {
                property: winner.property,
                important: provenance.important,
                key: winner.key,
                priority: self.priorities[provenance.priority],
                source: provenance.source,
            })
    }

    fn group_bucket(&self, group: WinnerGroupRef) -> PropertyID {
        self.groups[group.winners]
            .winners
            .first()
            .expect("a winner group is non-empty")
            .property
            / WINNER_GROUP_PROPERTY_COUNT
    }

    /// Name the flush whose publications the rows record from now on.
    pub fn begin_flush(&mut self, stamp: u64) {
        self.stamp = stamp;
    }

    /// The flush that published the node's row, when it has one.
    #[must_use]
    pub fn row_stamp(&self, node: StyleNodeID) -> Option<u64> {
        let index = node.element_index()? as usize;
        self.column.get(index)?.as_ref()?;
        Some(self.stamps.get(index).copied().unwrap_or(0))
    }

    /// The flush that published the node's row for a pseudo-element, when it has one.
    #[must_use]
    pub fn pseudo_row_stamp(&self, node: StyleNodeID, pseudo: PseudoElementTarget) -> Option<u64> {
        node.element_index()
            .and_then(|index| self.pseudo_rows_by_node.get(index as usize))
            .and_then(|rows| rows.iter().find(|row| row.pseudo == pseudo))
            .map(|row| row.stamp)
    }

    #[must_use]
    pub fn winner_in_state(&self, state: CascadeStateID, property: PropertyID) -> Option<PropertyWinner> {
        let groups = &self.states[state];
        let index = groups.partition_point(|group| {
            self.groups[group.winners]
                .winners
                .last()
                .is_some_and(|winner| winner.property < property)
        });
        let group = *groups.get(index)?;
        let winners = &self.groups[group.winners].winners;
        let provenance = &self.provenance_groups[group.provenance];
        winners
            .binary_search_by_key(&property, |winner| winner.property)
            .ok()
            .map(|index| PropertyWinner {
                property: winners[index].property,
                important: provenance[index].important,
                key: winners[index].key,
                priority: self.priorities[provenance[index].priority],
                source: provenance[index].source,
            })
    }

    pub fn winners_in_state(&self, state: CascadeStateID) -> impl Iterator<Item = PropertyWinner> + '_ {
        self.states[state].iter().flat_map(|&group| self.group_winners(group))
    }

    /// Compare the semantic winners consumed by two computed-style publications.
    ///
    /// Missing retained coverage is not the implicit cascade result. It conservatively reports
    /// every explicit current winner, but the returned property list alone cannot prove semantic
    /// equality when `previous` is `None`.
    ///
    /// Missing rows represent the implicit cascade result, so inserting or removing an explicit
    /// winner changes that property. Equal specified keys stop even when the winning rule or its
    /// priority changed.
    #[must_use]
    pub fn semantic_delta(&self, previous: Option<CascadeStateID>, current: CascadeStateID) -> CascadeWinnerDelta {
        let previous: &[WinnerGroupRef] = previous.map_or(&[], |state| &self.states[state]);
        let current = &self.states[current];
        let mut properties = Vec::new();
        for entry in merge_sorted_by(previous, current, |old, new| {
            self.group_bucket(*old).cmp(&self.group_bucket(*new))
        }) {
            match entry {
                SortedMergeEntry::Both(old, new) => {
                    if old.winners != new.winners {
                        self.append_group_semantic_delta(Some(old.winners), Some(new.winners), &mut properties);
                    }
                }
                SortedMergeEntry::Left(old) => {
                    self.append_group_semantic_delta(Some(old.winners), None, &mut properties);
                }
                SortedMergeEntry::Right(new) => {
                    self.append_group_semantic_delta(None, Some(new.winners), &mut properties);
                }
            }
        }
        CascadeWinnerDelta { properties }
    }

    /// Hash the set of properties represented by a state, deliberately leaving their values out.
    /// This identifies candidates which can seed a first computation and then be corrected by the
    /// exact semantic delta.
    pub(super) fn property_shape_hash(&self, state: CascadeStateID) -> u64 {
        let mut hasher = fast_hasher();
        for winner in self.winners_in_state(state) {
            winner.property.hash(&mut hasher);
        }
        hasher.finish()
    }

    pub(super) fn property_shapes_are_equal(&self, left: CascadeStateID, right: CascadeStateID) -> bool {
        self.winners_in_state(left)
            .map(|winner| winner.property)
            .eq(self.winners_in_state(right).map(|winner| winner.property))
    }

    fn append_group_semantic_delta(
        &self,
        previous: Option<WinnerGroupID>,
        current: Option<WinnerGroupID>,
        properties: &mut Vec<PropertyID>,
    ) {
        let previous: &[SemanticPropertyWinner] = previous.map_or(&[], |group| self.groups[group].winners.as_slice());
        let current: &[SemanticPropertyWinner] = current.map_or(&[], |group| self.groups[group].winners.as_slice());
        for entry in merge_sorted_by(previous, current, |old, new| old.property.cmp(&new.property)) {
            match entry {
                SortedMergeEntry::Both(old, new) => {
                    if old.key != new.key {
                        properties.push(old.property);
                    }
                }
                SortedMergeEntry::Left(old) => {
                    properties.push(old.property);
                }
                SortedMergeEntry::Right(new) => {
                    properties.push(new.property);
                }
            }
        }
    }

    #[must_use]
    pub fn payload_count(&self) -> usize {
        self.groups.len()
    }

    #[must_use]
    pub fn state_count(&self) -> usize {
        self.states.len()
    }

    #[must_use]
    pub fn winner_entry_count(&self) -> usize {
        self.winner_entry_count
    }

    #[must_use]
    pub fn generation(&self) -> u64 {
        self.generation
    }

    #[must_use]
    pub fn set(&mut self, node: StyleNodeID, state: CascadeStateID, program_version: ProgramVersion) -> bool {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return false;
        };
        if !self.admitting && self.column.get(index).is_none_or(Option::is_none) {
            return false;
        }
        self.column.ensure(index);
        self.stamps.ensure(index);
        self.stamps[index] = self.stamp;
        if self.column[index] == Some((state, program_version)) {
            self.set_priority_current(index, true);
            return true;
        }
        if let Some((previous, previous_version)) = self.column[index] {
            self.update_winner_rule_node_references(previous, node, false);
            self.release_state(previous);
            if previous_version == self.newest_program_version {
                self.newest_version_row_count -= 1;
            }
        } else {
            self.row_count += 1;
        }
        if program_version > self.newest_program_version {
            self.newest_program_version = program_version;
            self.newest_version_row_count = 0;
        }
        self.column[index] = Some((state, program_version));
        self.retain_state(state);
        self.update_winner_rule_node_references(state, node, true);
        if program_version == self.newest_program_version {
            self.newest_version_row_count += 1;
        }
        self.set_priority_current(index, true);
        true
    }

    #[must_use]
    pub fn set_pseudo(
        &mut self,
        node: StyleNodeID,
        pseudo: PseudoElementTarget,
        state: CascadeStateID,
        program_version: ProgramVersion,
    ) -> bool {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return false;
        };
        let existing = self
            .pseudo_rows_by_node
            .get(index)
            .and_then(|rows| rows.iter().position(|row| row.pseudo == pseudo));
        if !self.admitting && existing.is_none() {
            return false;
        }
        self.pseudo_rows_by_node.ensure(index);
        if let Some(existing) = existing {
            let row = &mut self.pseudo_rows_by_node[index][existing];
            row.stamp = self.stamp;
            if row.state == (state, program_version) {
                row.priority_current = true;
                return true;
            }
            let previous = row.state.0;
            *row = PseudoWinnerRow {
                pseudo,
                state: (state, program_version),
                priority_current: true,
                stamp: self.stamp,
            };
            self.update_winner_rule_node_references(previous, node, false);
            self.release_state(previous);
        } else {
            let capacity_before = self.pseudo_rows_by_node[index].capacity();
            self.pseudo_rows_by_node[index].push(PseudoWinnerRow {
                pseudo,
                state: (state, program_version),
                priority_current: true,
                stamp: self.stamp,
            });
            self.pseudo_row_capacity_bytes +=
                ((self.pseudo_rows_by_node[index].capacity() - capacity_before) * size_of::<PseudoWinnerRow>()) as u64;
        }
        self.retain_state(state);
        self.update_winner_rule_node_references(state, node, true);
        true
    }

    pub fn remove(&mut self, node: StyleNodeID) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        if let Some((previous, program_version)) = self.column.get_mut(index).and_then(Option::take) {
            self.update_winner_rule_node_references(previous, node, false);
            self.release_state(previous);
            self.row_count -= 1;
            if program_version == self.newest_program_version {
                self.newest_version_row_count -= 1;
            }
        }
        self.set_priority_current(index, false);
        if let Some(rows) = self.pseudo_rows_by_node.get_mut(index) {
            let rows = std::mem::take(rows);
            self.pseudo_row_capacity_bytes -= (rows.capacity() * size_of::<PseudoWinnerRow>()) as u64;
            for row in rows {
                self.update_winner_rule_node_references(row.state.0, node, false);
                self.release_state(row.state.0);
            }
        }
    }

    /// Publish one node's interned cascade winner rows for another node with the same exact
    /// selector answer and no element declarations.
    pub(super) fn copy_node_rows(
        &mut self,
        source: StyleNodeID,
        target: StyleNodeID,
        program_version: ProgramVersion,
        memory: &mut MemoryController,
    ) -> Option<usize> {
        let (_, state) = self
            .token_for(WinnerGroupKey::current(source, program_version))
            .sparse()
            .ok()?;
        let pseudo_states: Vec<_> = self.pseudo_states(source).collect();
        let scratch_bytes = (pseudo_states.capacity()
            * size_of::<(PseudoElementTarget, ProgramVersion, CascadeStateID, bool)>())
            as u64;
        memory.reserve_required(MemoryCategory::BatchScratch, scratch_bytes);
        if !self.admitting {
            memory.release(MemoryCategory::BatchScratch, scratch_bytes);
            return None;
        }
        self.remove(target);
        assert!(self.set(target, state, program_version));
        for (pseudo, version, state, priority_current) in &pseudo_states {
            assert!(self.set_pseudo(target, *pseudo, *state, *version));
            if !priority_current {
                self.mark_pseudo_inventory_incomplete(target, *pseudo);
            }
        }
        memory.release(MemoryCategory::BatchScratch, scratch_bytes);
        Some(1 + pseudo_states.len())
    }

    fn retain_state(&mut self, state: CascadeStateID) {
        let index = state.0 as usize;
        if self.state_reference_counts[index] == 0 {
            for &rule in &self.state_winning_rules[index] {
                self.winner_rule_references.retain(rule);
            }
        }
        self.state_reference_counts[index] += 1;
    }

    fn release_state(&mut self, state: CascadeStateID) {
        let index = state.0 as usize;
        self.state_reference_counts[index] -= 1;
        if self.state_reference_counts[index] == 0 {
            for &rule in &self.state_winning_rules[index] {
                self.winner_rule_references.release(rule);
            }
        }
    }

    fn update_winner_rule_node_references(&mut self, state: CascadeStateID, node: StyleNodeID, retain: bool) {
        let winner_rule_references = &mut self.winner_rule_references;
        for &rule in &self.state_winning_rules[state.0 as usize] {
            if retain {
                winner_rule_references.retain_node(rule, node);
            } else {
                winner_rule_references.release_node(rule, node);
            }
        }
    }

    #[must_use]
    pub fn rule_is_a_winner(&self, rule: RuleID) -> bool {
        self.winner_rule_references.contains(rule)
    }

    pub fn winning_nodes(&self, rule: RuleID) -> Option<impl Iterator<Item = StyleNodeID> + '_> {
        self.winner_rule_references.nodes(rule)
    }

    fn priority_is_current(&self, index: usize) -> bool {
        self.priority_current.contains(index)
    }

    fn set_priority_current(&mut self, index: usize, current: bool) {
        if !self.priority_current.set(index, current).0 {
            return;
        }
        if current {
            self.priority_current_row_count += 1;
        } else {
            self.priority_current_row_count -= 1;
        }
    }

    /// Keep semantic winner keys while marking their topology-dependent priorities stale.
    pub fn invalidate_priorities(&mut self) {
        if self.priority_current_row_count != 0 {
            self.priority_current.clear();
            self.priority_current_row_count = 0;
        }
        for rows in self.pseudo_rows_by_node.iter_mut() {
            for row in rows {
                row.priority_current = false;
            }
        }
    }

    #[must_use]
    pub(super) fn token_for(&self, key: WinnerGroupKey) -> Lookup<(u64, CascadeStateID), WinnerGroupGap> {
        match self.lookup(key).sparse() {
            Ok(&(state, _)) => Lookup::Known((self.generation, state)),
            Err(gap) => Lookup::Missing(gap),
        }
    }

    pub(super) fn set_from_token(
        &mut self,
        node: StyleNodeID,
        generation: u64,
        state: CascadeStateID,
        program_version: ProgramVersion,
    ) -> Result<(), WinnerGroupTokenGap> {
        if generation != self.generation {
            return Err(WinnerGroupTokenGap::StaleGeneration {
                retained: generation,
                current: self.generation,
            });
        }
        self.set(node, state, program_version)
            .then_some(())
            .ok_or(WinnerGroupTokenGap::AdmissionClosed)
    }

    #[must_use]
    pub(super) fn coverage_at_least(
        &self,
        program_version: ProgramVersion,
        row_count: usize,
    ) -> Lookup<(), WinnerGroupCoverageGap> {
        if self.newest_program_version != program_version {
            return Lookup::Missing(WinnerGroupCoverageGap::StaleProgram {
                retained: self.newest_program_version,
                required: program_version,
            });
        }
        // Departed nodes lose their rows at the commit barrier and arriving nodes have no row yet.
        // Therefore the resident row count names exactly the connected elements that must already
        // have a winner row, and covering it proves there is no connected-element gap.
        if self.newest_version_row_count < row_count {
            return Lookup::Missing(WinnerGroupCoverageGap::InsufficientProgramRows {
                retained: self.newest_version_row_count,
                required: row_count,
            });
        }
        if self.priority_current_row_count < row_count {
            return Lookup::Missing(WinnerGroupCoverageGap::InsufficientPriorityRows {
                retained: self.priority_current_row_count,
                required: row_count,
            });
        }
        Lookup::Known(())
    }

    /// Start retained winner coverage for a new program version with no proven rows.
    pub fn begin_program_version(&mut self, version: ProgramVersion) {
        if version > self.newest_program_version {
            self.newest_program_version = version;
            self.newest_version_row_count = 0;
        }
    }

    /// Advance rows whose retained winners were proven unchanged by a program transaction.
    pub fn advance_program_version_where(
        &mut self,
        from: ProgramVersion,
        to: ProgramVersion,
        mut can_advance: impl FnMut(StyleNodeID) -> bool,
    ) {
        if from == to {
            return;
        }
        debug_assert!(to > from);
        self.begin_program_version(to);
        for (index, slot) in self.column.iter_mut().enumerate().skip(1) {
            let Some((_, version)) = slot else {
                continue;
            };
            if *version != from {
                continue;
            }
            let node = StyleNodeID::element(u32::try_from(index).expect("style node identity space exhausted"));
            if !can_advance(node) {
                continue;
            }
            *version = to;
            if to == self.newest_program_version {
                self.newest_version_row_count += 1;
            }
        }
        for (index, rows) in self.pseudo_rows_by_node.iter_mut().enumerate().skip(1) {
            if rows.is_empty() {
                continue;
            }
            let node = StyleNodeID::element(u32::try_from(index).expect("style node identity space exhausted"));
            for row in rows {
                if row.state.1 == from && can_advance(node) {
                    row.state.1 = to;
                }
            }
        }
    }

    pub fn active_states(&self) -> impl Iterator<Item = CascadeStateID> + '_ {
        self.states
            .iter()
            .zip(&self.state_reference_counts)
            .enumerate()
            .filter(|(_, (_, references))| **references > 0)
            .map(|(index, _)| CascadeStateID(u32::try_from(index).expect("cascade state identity space exhausted")))
    }

    pub(super) fn pseudo_states(
        &self,
        node: StyleNodeID,
    ) -> impl Iterator<Item = (PseudoElementTarget, ProgramVersion, CascadeStateID, bool)> + '_ {
        let rows = node
            .element_index()
            .and_then(|index| self.pseudo_rows_by_node.get(index as usize))
            .into_iter()
            .flatten();
        rows.map(|row| (row.pseudo, row.state.1, row.state.0, row.priority_current))
    }

    #[must_use]
    pub(super) fn pseudo_state(
        &self,
        node: StyleNodeID,
        pseudo: PseudoElementTarget,
    ) -> Option<(ProgramVersion, CascadeStateID, bool)> {
        node.element_index()
            .and_then(|index| self.pseudo_rows_by_node.get(index as usize))
            .and_then(|rows| rows.iter().find(|row| row.pseudo == pseudo))
            .map(|row| (row.state.1, row.state.0, row.priority_current))
    }

    pub(super) fn mark_pseudo_inventory_incomplete(&mut self, node: StyleNodeID, pseudo: PseudoElementTarget) {
        if let Some(row) = node
            .element_index()
            .and_then(|index| self.pseudo_rows_by_node.get_mut(index as usize))
            .and_then(|rows| rows.iter_mut().find(|row| row.pseudo == pseudo))
        {
            row.priority_current = false;
        }
    }

    /// The winning declaration for one property of one node, if a group is resident.
    #[must_use]
    pub(super) fn winner(&self, key: WinnerGroupKey, property: PropertyID) -> Lookup<PropertyWinner, WinnerGroupGap> {
        let state = match self.lookup(key).sparse() {
            Ok(&(state, _)) => state,
            Err(gap) => return Lookup::Missing(gap),
        };
        match self.winner_in_state(state, property) {
            Some(winner) => Lookup::Known(winner),
            None => Lookup::KnownAbsent,
        }
    }

    pub fn evict(&mut self) {
        self.residency.release();
        self.nested_residency.release();
        self.generation = self.generation.wrapping_add(1);
        self.states = InternTable::default();
        self.state_reference_counts = Vec::new();
        self.state_winning_rules = Vec::new();
        self.groups = InternTable::default();
        self.provenance_groups = InternTable::default();
        self.priorities = InternTable::default();
        self.continuations = InternTable::default();
        self.winner_entry_count = 0;
        self.winner_rule_references = WinnerRuleReferences::default();
        self.column = Column::default();
        self.stamps = Column::default();
        self.pseudo_rows_by_node = Column::default();
        self.pseudo_row_capacity_bytes = 0;
        self.priority_current = BitColumn::default();
        self.row_count = 0;
        self.priority_current_row_count = 0;
        self.newest_program_version = ProgramVersion::default();
        self.newest_version_row_count = 0;
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.states,
                self.groups,
                self.provenance_groups,
                self.priorities,
                self.continuations,
                self.column,
                self.pseudo_rows_by_node,
                self.priority_current,
                self.state_reference_counts,
                self.state_winning_rules,
                self.winner_rule_references,
                self.stamps,
            ];
            cached [self.nested_residency.bytes()];
            nested [self.pseudo_row_capacity_bytes];
            skip [
                self.residency,
                self.generation,
                self.winner_entry_count,
                self.row_count,
                self.priority_current_row_count,
                self.newest_program_version,
                self.newest_version_row_count,
                self.pseudo_row_capacity_bytes,
            ];
        }
    }

    pub fn settle_memory(&mut self, memory: &mut MemoryController) {
        let nested = self.nested_residency.bytes();
        self.nested_residency.reconcile_committed(memory, nested);
        let current = self.capacity_bytes() - self.nested_residency.bytes();
        self.residency.reconcile_committed(memory, current);
        memory.finish_committed_acceleration_growth(MemoryCategory::CascadeWinnerGroup);
        self.admitting = memory.is_tier3_admitting(MemoryCategory::CascadeWinnerGroup);
    }

    pub(super) fn begin_quota_period(&mut self) {
        self.admitting = true;
    }

    #[must_use]
    pub(super) fn admits_new_rows(&self) -> bool {
        self.admitting
    }
}

impl WinnerGroups {
    pub(super) fn lookup(&self, key: WinnerGroupKey) -> Lookup<&(CascadeStateID, ProgramVersion), WinnerGroupGap> {
        if let Some(pseudo) = key.pseudo {
            let Some(row) = key
                .node
                .element_index()
                .and_then(|index| self.pseudo_rows_by_node.get(index as usize))
                .and_then(|rows| rows.iter().find(|row| row.pseudo == pseudo))
            else {
                return Lookup::Missing(WinnerGroupGap::MissingNode(key.node));
            };
            if row.state.1 != key.program_version {
                return Lookup::Missing(WinnerGroupGap::StaleProgram {
                    node: key.node,
                    retained: row.state.1,
                    required: key.program_version,
                });
            }
            if key.priority == WinnerPriorityCoverage::Current && !row.priority_current {
                return Lookup::Missing(WinnerGroupGap::StalePriority(key.node));
            }
            return Lookup::Known(&row.state);
        }
        let Some(index) = key.node.element_index().map(|index| index as usize) else {
            return Lookup::Missing(WinnerGroupGap::MissingNode(key.node));
        };
        let Some(row @ (_, retained_program_version)) = self.column.get(index).and_then(Option::as_ref) else {
            return Lookup::Missing(WinnerGroupGap::MissingNode(key.node));
        };
        if *retained_program_version != key.program_version {
            return Lookup::Missing(WinnerGroupGap::StaleProgram {
                node: key.node,
                retained: *retained_program_version,
                required: key.program_version,
            });
        }
        if key.priority == WinnerPriorityCoverage::Current && !self.priority_is_current(index) {
            return Lookup::Missing(WinnerGroupGap::StalePriority(key.node));
        }
        Lookup::Known(row)
    }
}

#[cfg(test)]
mod tests {
    use super::super::memory::DeviceClass;
    use super::*;

    fn memory() -> MemoryController {
        MemoryController::new(DeviceClass::ForegroundDesktop)
    }

    fn inputs(origin: CascadeOrigin, important: bool) -> PriorityInputs {
        PriorityInputs {
            origin,
            important,
            context_depth: 0,
            element_attachment: ElementAttachment::Rule,
            layer: CascadeLayerID::UNLAYERED,
            layer_rank: (100, 100),
            specificity: Specificity::default(),
            scope_proximity: 0,
            sheet_rank: (10, 10),
            rule_rank: (10, 10),
        }
    }

    #[test]
    fn the_origin_ladder_puts_importance_where_the_cascade_does() {
        let normal_ua = CascadePriority::new(inputs(CascadeOrigin::UserAgent, false));
        let normal_user = CascadePriority::new(inputs(CascadeOrigin::User, false));
        let hint = CascadePriority::new(inputs(CascadeOrigin::AuthorPresentationalHint, false));
        let normal_author = CascadePriority::new(inputs(CascadeOrigin::Author, false));
        let animation = CascadePriority::new(inputs(CascadeOrigin::Animation, false));
        let important_author = CascadePriority::new(inputs(CascadeOrigin::Author, true));
        let important_user = CascadePriority::new(inputs(CascadeOrigin::User, true));
        let important_ua = CascadePriority::new(inputs(CascadeOrigin::UserAgent, true));
        let transition = CascadePriority::new(inputs(CascadeOrigin::Transition, false));

        let ladder = [
            normal_ua,
            normal_user,
            hint,
            normal_author,
            animation,
            important_author,
            important_user,
            important_ua,
            transition,
        ];
        assert!(ladder.windows(2).all(|pair| pair[0] < pair[1]));

        // A presentational hint loses to an author rule, however specific the hint's source.
        assert!(hint < normal_author);
    }

    #[test]
    fn importance_reverses_layer_order() {
        let mut early = inputs(CascadeOrigin::Author, false);
        early.layer_rank = (10, 0);
        let mut late = inputs(CascadeOrigin::Author, false);
        late.layer_rank = (20, 0);
        assert!(
            CascadePriority::new(early) < CascadePriority::new(late),
            "later layers win"
        );

        early.important = true;
        late.important = true;
        assert!(
            CascadePriority::new(late) < CascadePriority::new(early),
            "and the order reverses for important declarations"
        );
    }

    #[test]
    fn importance_reverses_encapsulation_context() {
        let mut outer = inputs(CascadeOrigin::Author, false);
        outer.context_depth = 0;
        let mut inner = inputs(CascadeOrigin::Author, false);
        inner.context_depth = 2;
        assert!(
            CascadePriority::new(inner) < CascadePriority::new(outer),
            "outer contexts win for normal declarations"
        );

        outer.important = true;
        inner.important = true;
        assert!(
            CascadePriority::new(outer) < CascadePriority::new(inner),
            "inner contexts win for important ones"
        );
    }

    #[test]
    fn an_element_attached_style_beats_every_rule_in_its_context() {
        let mut rule = inputs(CascadeOrigin::Author, false);
        rule.layer_rank = (u64::MAX - 1, u64::MAX - 1);
        rule.specificity = Specificity {
            ids: 100,
            classes: 100,
            types: 100,
        };
        let mut inline = inputs(CascadeOrigin::Author, false);
        inline.element_attachment = ElementAttachment::InlineStyle;
        inline.layer_rank = (0, 0);

        assert!(CascadePriority::new(rule) < CascadePriority::new(inline));
    }

    #[test]
    fn specificity_then_proximity_then_source_order_break_ties() {
        let base = inputs(CascadeOrigin::Author, false);
        let mut specific = base;
        specific.specificity = Specificity {
            classes: 1,
            ..Specificity::default()
        };
        assert!(CascadePriority::new(base) < CascadePriority::new(specific));

        let mut near = base;
        near.scope_proximity = 1;
        let mut far = base;
        far.scope_proximity = 5;
        assert!(
            CascadePriority::new(far) < CascadePriority::new(near),
            "nearer scopes win"
        );

        let mut early = base;
        early.rule_rank = (10, 10);
        let mut late = base;
        late.rule_rank = (10, 20);
        assert!(CascadePriority::new(early) < CascadePriority::new(late));

        let mut early_sheet = base;
        early_sheet.sheet_rank = (1, 0);
        early_sheet.rule_rank = (u64::MAX, u64::MAX);
        let mut late_sheet = base;
        late_sheet.sheet_rank = (2, 0);
        late_sheet.rule_rank = (0, 0);
        assert!(
            CascadePriority::new(early_sheet) < CascadePriority::new(late_sheet),
            "sheet order outranks position within a sheet"
        );
    }

    fn winner_key(value: u64) -> SpecifiedWinnerKey {
        SpecifiedWinnerKey {
            value: SpecifiedValueID(value),
            operator: CascadeOperator::Declared,
            continuation: CascadeContinuationID::default(),
            animation_relevance: 0,
            important: false,
        }
    }

    fn winner(property: PropertyID, value: u64, rule: u32) -> PropertyWinner {
        PropertyWinner {
            property,
            important: false,
            key: winner_key(value),
            priority: CascadePriority::new(inputs(CascadeOrigin::Author, false)),
            source: WinnerSource::Rule(RuleID(rule)),
        }
    }

    fn candidate(
        value: u64,
        rule: u32,
        operator: CascadeOperator,
        origin: CascadeOrigin,
        important: bool,
        layer: CascadeLayerID,
        layer_rank: u64,
    ) -> CascadeCandidate {
        let mut priority_inputs = inputs(origin, important);
        priority_inputs.layer = layer;
        priority_inputs.layer_rank = (layer_rank, 0);
        priority_inputs.rule_rank = (u64::from(rule), 0);
        let mut winner = winner(1, value, rule);
        winner.key.operator = operator;
        let priority = CascadePriority::new(priority_inputs);
        winner.priority = priority;
        CascadeCandidate {
            winner,
            priority,
            stratum: CascadeStratum::new(
                origin,
                important,
                0,
                layer,
                (layer_rank, 0),
                CascadeAttachment::StyleSheet,
            ),
        }
    }

    #[test]
    fn author_revert_continues_to_the_user_agent_origin() {
        let mut groups = WinnerGroups::new();
        let mut candidates = [
            candidate(
                10,
                1,
                CascadeOperator::Declared,
                CascadeOrigin::UserAgent,
                false,
                CascadeLayerID::UNLAYERED,
                0,
            ),
            candidate(
                20,
                2,
                CascadeOperator::Declared,
                CascadeOrigin::Author,
                false,
                CascadeLayerID::UNLAYERED,
                0,
            ),
            candidate(
                30,
                3,
                CascadeOperator::Revert,
                CascadeOrigin::Author,
                false,
                CascadeLayerID::UNLAYERED,
                0,
            ),
        ];

        let winner = groups.resolve_candidates(&mut candidates).unwrap();
        assert_eq!(winner.key.operator, CascadeOperator::Revert);
        assert_eq!(groups.resolved_winner(winner).unwrap().key.value, SpecifiedValueID(10));
        assert_eq!(groups.continuation_count(), 1);

        let same = groups.resolve_candidates(&mut candidates).unwrap();
        assert_eq!(same.key.continuation, winner.key.continuation);
        assert_eq!(
            groups.continuation_count(),
            1,
            "equal cascade states share their continuation"
        );
    }

    #[test]
    fn revert_layer_continues_across_three_normal_layers() {
        let mut groups = WinnerGroups::new();
        let mut candidates = [
            candidate(
                10,
                1,
                CascadeOperator::Declared,
                CascadeOrigin::Author,
                false,
                CascadeLayerID(1),
                1,
            ),
            candidate(
                20,
                2,
                CascadeOperator::Declared,
                CascadeOrigin::Author,
                false,
                CascadeLayerID(2),
                2,
            ),
            candidate(
                30,
                3,
                CascadeOperator::RevertLayer,
                CascadeOrigin::Author,
                false,
                CascadeLayerID(3),
                3,
            ),
        ];

        let winner = groups.resolve_candidates(&mut candidates).unwrap();
        assert_eq!(groups.resolved_winner(winner).unwrap().key.value, SpecifiedValueID(20));
    }

    #[test]
    fn revert_layer_continues_from_the_implicit_outer_layer() {
        let mut groups = WinnerGroups::new();
        let mut candidates = [
            candidate(
                10,
                1,
                CascadeOperator::Declared,
                CascadeOrigin::Author,
                false,
                CascadeLayerID(1),
                1,
            ),
            candidate(
                20,
                2,
                CascadeOperator::RevertLayer,
                CascadeOrigin::Author,
                false,
                CascadeLayerID::UNLAYERED,
                2,
            ),
        ];

        let winner = groups.resolve_candidates(&mut candidates).unwrap();
        assert_eq!(groups.resolved_winner(winner).unwrap().key.value, SpecifiedValueID(10));
    }

    #[test]
    fn important_revert_layer_follows_reversed_layer_order() {
        let mut groups = WinnerGroups::new();
        let mut candidates = [
            candidate(
                20,
                2,
                CascadeOperator::Declared,
                CascadeOrigin::Author,
                false,
                CascadeLayerID(1),
                1,
            ),
            candidate(
                10,
                1,
                CascadeOperator::RevertLayer,
                CascadeOrigin::Author,
                true,
                CascadeLayerID(2),
                2,
            ),
            candidate(
                30,
                3,
                CascadeOperator::Declared,
                CascadeOrigin::Author,
                true,
                CascadeLayerID(3),
                3,
            ),
        ];

        let winner = groups.resolve_candidates(&mut candidates).unwrap();
        assert_eq!(winner.source, WinnerSource::Rule(RuleID(1)));
        assert_eq!(groups.resolved_winner(winner).unwrap().key.value, SpecifiedValueID(20));
    }

    #[test]
    fn top_1_cascade_keeps_one_greatest_row_per_output() {
        let mut top_1 = Top1Cascade::with_capacity(4);
        top_1.consider(2_u16, 5_u32, "first");
        top_1.consider(1, 7, "other property");
        top_1.consider(2, 3, "loser");
        top_1.consider(2, 5, "later tie");

        assert_eq!(top_1.winner(&1).map(|winner| winner.payload), Some("other property"));
        assert_eq!(top_1.winner(&2).map(|winner| winner.payload), Some("later tie"));
        assert!(top_1.winner(&3).is_none());
        assert_eq!(top_1.winners().map(|winner| winner.key).collect::<Vec<_>>(), vec![1, 2]);

        let capacity_bytes = top_1.capacity_bytes();
        top_1.clear();
        assert_eq!(top_1.capacity_bytes(), capacity_bytes);
        assert!(top_1.winners().next().is_none());
        top_1.consider(3, 9, "reused");
        assert_eq!(top_1.winner(&3).map(|winner| winner.payload), Some("reused"));
    }

    #[test]
    fn a_cascade_operator_is_part_of_the_specified_key() {
        let declared = winner_key(1);
        let inherited = SpecifiedWinnerKey {
            operator: CascadeOperator::Inherit,
            ..declared
        };
        assert_ne!(declared, inherited);

        // Two reverts differing only in where they resume are different specified winners.
        let revert = SpecifiedWinnerKey {
            operator: CascadeOperator::RevertLayer,
            continuation: CascadeContinuationID(1),
            ..declared
        };
        let deeper = SpecifiedWinnerKey {
            continuation: CascadeContinuationID(2),
            ..revert
        };
        assert_ne!(revert, deeper);
    }

    #[test]
    fn importance_is_part_of_the_specified_key() {
        let declared = winner_key(1);
        let important = SpecifiedWinnerKey {
            important: true,
            ..declared
        };
        assert_ne!(declared, important);
    }

    #[test]
    fn semantic_winner_deltas_ignore_provenance_and_report_exact_properties() {
        let mut groups = WinnerGroups::new();
        let before = groups.intern_sorted(&[winner(1, 10, 1), winner(3, 30, 1), winner(5, 50, 1)], None);
        let semantic_group_count = groups.payload_count();
        let same_values = groups.intern_sorted(&[winner(1, 10, 2), winner(3, 30, 2), winner(5, 50, 2)], None);
        assert_ne!(before, same_values);
        assert_eq!(groups.payload_count(), semantic_group_count);
        assert!(groups.semantic_delta(Some(before), same_values).is_empty());

        let after = groups.intern_sorted(
            &[winner(1, 11, 2), winner(2, 20, 2), winner(5, 50, 2), winner(7, 70, 2)],
            None,
        );
        assert_eq!(groups.semantic_delta(Some(before), after).properties(), &[1, 2, 3, 7]);
        assert_eq!(groups.semantic_delta(None, after).properties(), &[1, 2, 5, 7]);
    }

    #[test]
    fn winner_provenance_retains_exact_priority_without_semantic_identity() {
        let mut groups = WinnerGroups::new();
        let mut before_winner = winner(1, 10, 1);
        let mut before_inputs = inputs(CascadeOrigin::Author, false);
        before_inputs.rule_rank = (1, 0);
        before_winner.priority = CascadePriority::new(before_inputs);
        let before = groups.intern_sorted(&[before_winner], None);
        let semantic_group_count = groups.payload_count();

        let mut after_winner = before_winner;
        let mut after_inputs = before_inputs;
        after_inputs.rule_rank = (2, 0);
        after_winner.priority = CascadePriority::new(after_inputs);
        let after = groups.intern_sorted(&[after_winner], None);

        assert_ne!(before, after);
        assert_eq!(groups.payload_count(), semantic_group_count);
        assert!(groups.semantic_delta(Some(before), after).is_empty());
        assert_eq!(
            groups.winner_in_state(before, 1).unwrap().priority,
            before_winner.priority
        );
        assert_eq!(
            groups.winner_in_state(after, 1).unwrap().priority,
            after_winner.priority
        );
    }

    #[test]
    fn importance_is_weighed_per_declaration_not_per_rule() {
        // Two author rules, the less specific one marking its declaration important. Ordering the
        // rules once by specificity would let the more specific normal declaration win; weighing
        // each declaration with its own importance puts the important one on a higher rung.
        let mut specific = inputs(CascadeOrigin::Author, false);
        specific.specificity = Specificity {
            ids: 1,
            ..Specificity::default()
        };
        specific.rule_rank = (20, 20);

        let mut important = inputs(CascadeOrigin::Author, true);
        important.specificity = Specificity {
            classes: 1,
            ..Specificity::default()
        };
        important.rule_rank = (10, 10);

        assert!(
            CascadePriority::new(specific) < CascadePriority::new(important),
            "an important author declaration outranks a more specific normal one"
        );

        // The same rule's other declaration, written without `!important`, does not.
        let mut normal = important;
        normal.important = false;
        assert!(
            CascadePriority::new(normal) < CascadePriority::new(specific),
            "and the same rule's normal declaration still loses to the more specific one"
        );
    }

    #[test]
    fn equal_winner_sets_share_one_state() {
        let mut groups = WinnerGroups::new();
        let first = groups.intern_sorted(&[winner(1, 5, 2), winner(2, 1, 1)], None);
        let second = groups.intern_sorted(&[winner(1, 5, 2), winner(2, 1, 1)], None);
        assert_eq!(first, second);
        assert_eq!(groups.payload_count(), 1);
        assert_eq!(groups.winners_in_state(first).next().unwrap().property, 1);

        let different = groups.intern_sorted(&[winner(1, 5, 2)], None);
        assert_ne!(first, different);
    }

    #[test]
    fn cascade_states_share_unmodified_property_groups() {
        let mut groups = WinnerGroups::new();
        let first = groups.intern_sorted(&[winner(1, 1, 1), winner(33, 2, 2)], None);
        let first_group_ids = groups.states[first.0 as usize].clone();
        let group_hash_computations = groups.group_hash_computations;
        let unchanged = groups.intern_sorted(&[winner(1, 1, 1), winner(33, 2, 2)], Some(first));

        assert_eq!(unchanged, first);
        assert_eq!(groups.group_hash_computations, group_hash_computations);

        let second = groups.intern_sorted(&[winner(1, 1, 1), winner(33, 3, 3)], Some(first));

        assert_ne!(first, second);
        assert_eq!(groups.state_count(), 2);
        assert_eq!(groups.payload_count(), 3);
        assert_eq!(groups.winner_entry_count(), 3);
        assert_eq!(groups.states[second.0 as usize][0], first_group_ids[0]);
        assert_eq!(groups.group_hash_computations, group_hash_computations + 1);
        assert_eq!(groups.winner_in_state(first, 1), groups.winner_in_state(second, 1));
        assert_ne!(groups.winner_in_state(first, 33), groups.winner_in_state(second, 33));
    }

    #[test]
    fn exact_property_updates_touch_only_their_winner_groups() {
        let mut groups = WinnerGroups::new();
        let before = groups.intern_sorted(&[winner(1, 10, 1), winner(2, 20, 2), winner(33, 30, 3)], None);
        let before_groups = groups.states[before.0 as usize].clone();
        let (after, delta) = groups.apply_property_updates(
            before,
            &[
                PropertyWinnerUpdate {
                    property: 2,
                    winner: Some(winner(2, 20, 4)),
                },
                PropertyWinnerUpdate {
                    property: 33,
                    winner: None,
                },
                PropertyWinnerUpdate {
                    property: 34,
                    winner: Some(winner(34, 40, 5)),
                },
            ],
        );

        assert_ne!(after, before);
        assert_eq!(delta.properties(), &[33, 34]);
        assert_eq!(groups.states[after.0 as usize].len(), 2);
        assert_eq!(groups.winner_in_state(after, 1), groups.winner_in_state(before, 1));
        assert_eq!(
            groups.winner_in_state(after, 2).unwrap().source,
            WinnerSource::Rule(RuleID(4))
        );
        assert!(groups.winner_in_state(after, 33).is_none());
        assert_eq!(groups.winner_in_state(after, 34).unwrap().key, winner_key(40));
        assert_ne!(groups.states[after.0 as usize][0], before_groups[0]);
        assert_ne!(groups.states[after.0 as usize][1], before_groups[1]);

        let payload_count = groups.payload_count();
        let (unchanged, delta) = groups.apply_property_updates(
            after,
            &[PropertyWinnerUpdate {
                property: 34,
                winner: groups.winner_in_state(after, 34),
            }],
        );
        assert_eq!(unchanged, after);
        assert!(delta.is_empty());
        assert_eq!(groups.payload_count(), payload_count);
    }

    #[test]
    fn active_winner_rules_are_indexed_across_shared_groups() {
        let mut groups = WinnerGroups::new();
        let first = groups.intern_sorted(&[winner(1, 1, 3), winner(2, 2, 5)], None);
        let second = groups.intern_sorted(&[winner(1, 3, 5)], None);
        let sparse = groups.intern_sorted(&[winner(1, 4, 100_000)], None);
        let first_node = StyleNodeID::element(1);
        let second_node = StyleNodeID::element(2);
        let sparse_node = StyleNodeID::element(3);

        assert!(groups.set(first_node, first, ProgramVersion(1)));
        assert!(groups.set(second_node, first, ProgramVersion(1)));
        assert!(groups.rule_is_a_winner(RuleID(3)));
        assert!(groups.rule_is_a_winner(RuleID(5)));
        assert!(groups.set(sparse_node, sparse, ProgramVersion(1)));
        assert!(groups.rule_is_a_winner(RuleID(100_000)));
        assert_eq!(groups.winner_rule_references.entries.len(), 3);
        assert_ne!(groups.winner_rule_references.posting_bytes, 0);
        assert_eq!(
            groups.winner_rule_references.posting_bytes,
            groups.winner_rule_references.measured_posting_bytes()
        );
        let cloned_references = groups.winner_rule_references.clone();
        assert_eq!(
            cloned_references.posting_bytes,
            cloned_references.measured_posting_bytes()
        );

        assert!(groups.set(first_node, second, ProgramVersion(1)));
        groups.remove(second_node);
        assert!(!groups.rule_is_a_winner(RuleID(3)));
        assert!(groups.rule_is_a_winner(RuleID(5)));

        groups.remove(first_node);
        assert!(!groups.rule_is_a_winner(RuleID(5)));
        groups.remove(sparse_node);
        assert!(!groups.rule_is_a_winner(RuleID(100_000)));
        assert!(groups.winner_rule_references.entries.is_empty());
        assert_eq!(groups.winner_rule_references.posting_bytes, 0);
    }

    #[test]
    fn closed_winner_admission_preserves_existing_rows_without_adding_new_ones() {
        let mut memory = memory();
        memory.set_tier3_limit_for_test(0);
        memory.begin_tier3_quota_period();
        let mut groups = WinnerGroups::new();
        let state = groups.intern_sorted(&[winner(1, 1, 3)], None);
        let resident = StyleNodeID::element(1);
        assert!(groups.set(resident, state, ProgramVersion(1)));
        groups.settle_memory(&mut memory);
        assert!(!memory.is_tier3_admitting(MemoryCategory::CascadeWinnerGroup));

        let refused = StyleNodeID::element(2);
        assert!(!groups.set(refused, state, ProgramVersion(1)));
        assert!(matches!(
            groups.lookup(WinnerGroupKey::current(resident, ProgramVersion(1))),
            Lookup::Known(_)
        ));
        assert!(matches!(
            groups.lookup(WinnerGroupKey::current(refused, ProgramVersion(1))),
            Lookup::Missing(WinnerGroupGap::MissingNode(node)) if node == refused
        ));
    }

    #[test]
    fn broad_winner_rule_node_postings_fall_back_to_the_rule_count() {
        let mut groups = WinnerGroups::new();
        let state = groups.intern_sorted(&[winner(1, 1, 3)], None);
        for index in 1..=WINNER_RULE_NODE_LIMIT + 1 {
            assert!(groups.set(
                StyleNodeID::element(u32::try_from(index).unwrap()),
                state,
                ProgramVersion(1),
            ));
        }

        assert!(groups.rule_is_a_winner(RuleID(3)));
        assert!(groups.winning_nodes(RuleID(3)).is_none());
        assert_eq!(groups.winner_rule_references.posting_bytes, 0);

        for index in 1..=WINNER_RULE_NODE_LIMIT + 1 {
            groups.remove(StyleNodeID::element(u32::try_from(index).unwrap()));
        }
        assert!(!groups.rule_is_a_winner(RuleID(3)));
    }

    #[test]
    fn pseudo_cascade_states_are_sparse_and_independent_from_the_element_column() {
        let mut groups = WinnerGroups::new();
        let node = StyleNodeID::element(1);
        let before = PseudoElementTarget::new(super::super::tree::PseudoElementKind(1));
        let after = PseudoElementTarget::new(super::super::tree::PseudoElementKind(2));
        let element_state = groups.intern_sorted(&[winner(1, 10, 1)], None);
        let before_state = groups.intern_sorted(&[winner(1, 20, 2)], None);
        let after_state = groups.intern_sorted(&[winner(1, 30, 3)], None);

        assert!(groups.set(node, element_state, ProgramVersion(1)));
        assert!(groups.set_pseudo(node, before, before_state, ProgramVersion(1)));
        assert!(groups.set_pseudo(node, after, after_state, ProgramVersion(1)));

        assert!(matches!(
            groups.winner(WinnerGroupKey::current(node, ProgramVersion(1)), 1),
            Lookup::Known(winner) if winner.key == winner_key(10)
        ));
        assert!(matches!(
            groups.winner(WinnerGroupKey::current_pseudo(node, before, ProgramVersion(1)), 1),
            Lookup::Known(winner) if winner.key == winner_key(20)
        ));
        assert!(matches!(
            groups.winner(WinnerGroupKey::current_pseudo(node, after, ProgramVersion(1)), 1),
            Lookup::Known(winner) if winner.key == winner_key(30)
        ));

        groups.invalidate_priorities();
        assert!(matches!(
            groups.lookup(WinnerGroupKey::current_pseudo(node, before, ProgramVersion(1))),
            Lookup::Missing(WinnerGroupGap::StalePriority(stale_node)) if stale_node == node
        ));
        assert!(matches!(
            groups.lookup(WinnerGroupKey {
                node,
                pseudo: Some(before),
                program_version: ProgramVersion(1),
                priority: WinnerPriorityCoverage::Retained,
            }),
            Lookup::Known(_)
        ));

        groups.remove(node);
        assert!(matches!(
            groups.lookup(WinnerGroupKey {
                node,
                pseudo: Some(before),
                program_version: ProgramVersion(1),
                priority: WinnerPriorityCoverage::Retained,
            }),
            Lookup::Missing(WinnerGroupGap::MissingNode(missing)) if missing == node
        ));
        assert_eq!(groups.state_reference_counts[element_state.0 as usize], 0);
        assert_eq!(groups.state_reference_counts[before_state.0 as usize], 0);
        assert_eq!(groups.state_reference_counts[after_state.0 as usize], 0);
    }

    #[test]
    fn a_cascade_state_column_is_evictable_without_semantic_effect() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut groups = WinnerGroups::new();
        let node = StyleNodeID::element(1);
        let current = WinnerGroupKey::current(node, ProgramVersion(1));
        assert!(matches!(
            groups.lookup(current),
            Lookup::Missing(WinnerGroupGap::MissingNode(missing)) if missing == node
        ));

        let group = groups.intern_sorted(&[winner(1, 1, 1)], None);
        for index in 1..1000_u32 {
            assert!(groups.set(StyleNodeID::element(index), group, ProgramVersion(1)));
        }
        groups.settle_memory(&mut memory);
        assert!(memory.bytes_in_category(MemoryCategory::CascadeWinnerGroup) > 0);
        assert!(matches!(
            groups.coverage_at_least(ProgramVersion(1), 999),
            Lookup::Known(())
        ));
        assert!(matches!(
            groups.coverage_at_least(ProgramVersion(2), 999),
            Lookup::Missing(WinnerGroupCoverageGap::StaleProgram {
                retained: ProgramVersion(1),
                required: ProgramVersion(2),
            })
        ));
        assert!(matches!(groups.winner(current, 1), Lookup::Known(_)));
        assert!(matches!(groups.winner(current, 2), Lookup::KnownAbsent));
        assert!(matches!(
            groups.lookup(WinnerGroupKey::current(node, ProgramVersion(2))),
            Lookup::Missing(WinnerGroupGap::StaleProgram {
                node: stale_node,
                retained: ProgramVersion(1),
                required: ProgramVersion(2),
            }) if stale_node == node
        ));
        let Lookup::Known((generation, token_group)) = groups.token_for(current) else {
            panic!("expected a retained group token");
        };
        groups.invalidate_priorities();
        assert!(matches!(
            groups.coverage_at_least(ProgramVersion(1), 999),
            Lookup::Missing(WinnerGroupCoverageGap::InsufficientPriorityRows {
                retained: 0,
                required: 999,
            })
        ));
        assert!(matches!(
            groups.lookup(current),
            Lookup::Missing(WinnerGroupGap::StalePriority(stale_node)) if stale_node == node
        ));
        assert!(matches!(
            groups.lookup(WinnerGroupKey::retained(node, ProgramVersion(1))),
            Lookup::Known(_)
        ));

        groups.evict();
        assert_eq!(memory.bytes_in_category(MemoryCategory::CascadeWinnerGroup), 0);
        assert_eq!(groups.capacity_bytes(), 0);
        assert!(matches!(
            groups.winner(current, 1),
            Lookup::Missing(WinnerGroupGap::MissingNode(missing)) if missing == node
        ));
        assert!(matches!(
            groups.set_from_token(node, generation, token_group, ProgramVersion(1)),
            Err(WinnerGroupTokenGap::StaleGeneration {
                retained,
                current,
            }) if retained == generation && current == generation.wrapping_add(1)
        ));
    }
}
