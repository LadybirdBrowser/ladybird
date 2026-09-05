/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::capacity::capacity_bytes;
use super::column::EpochColumn;
use super::column::PagedColumn;
use super::column::PagedColumnPage;
use super::column::advance_epoch;
use super::program::EntryID;
use super::sorted_merge::SortedMergeEntry;
use super::sorted_merge::merge_sorted_by;
use super::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub(super) struct PendingRoute {
    pub(super) route: RouteID,
    pub(super) exact_tree_evaluation: Option<ExactTreeEvaluation>,
}

#[derive(Clone, Copy)]
pub(super) struct PendingPrefixProducer {
    pub(super) node: StyleNodeID,
    pub(super) producer: PrefixProducer,
}

#[derive(Clone, Copy)]
pub(super) enum RetainedWinnerProbe {
    Node(StyleNodeID),
    AllResident { resident_count: usize },
}

pub(super) struct RemainingPosting {
    nodes: Vec<StyleNodeID>,
    plan_generation: u64,
    pruned_nodes: Vec<StyleNodeID>,
}

#[derive(Default)]
pub(super) struct RemainingPostingDirectory {
    entries: Vec<(PostingKey, RemainingPosting)>,
}

impl RemainingPostingDirectory {
    fn entry(&mut self, key: PostingKey) -> Option<&mut RemainingPosting> {
        if let Some(index) = self.entries.iter().position(|(candidate, _)| *candidate == key) {
            return Some(&mut self.entries[index].1);
        }
        None
    }

    fn capacity_bytes(&self) -> u64 {
        (self.entries.capacity() * size_of::<(PostingKey, RemainingPosting)>()) as u64
    }

    fn insert(&mut self, key: PostingKey, posting: RemainingPosting) -> &mut RemainingPosting {
        self.entries.push((key, posting));
        &mut self.entries.last_mut().unwrap().1
    }
}

pub(super) struct ImpactPlanningWorkspace {
    pub(super) batches: HashMap<Vec<ImpactRegion>, Rc<ImpactRegionBatch>>,
    // Fact postings cannot change while one transaction is being planned, and the exact-node plan
    // only grows. Removing planned nodes here is therefore permanent for the lifetime of this
    // workspace: later routes see the posting members that can still contribute, not the same
    // already-planned prefix over and over.
    pub(super) remaining_postings: RemainingPostingDirectory,
    pub(super) memory: MemoryLease,
    pub(super) nested_memory: MemoryLease,
}

impl Default for ImpactPlanningWorkspace {
    fn default() -> Self {
        Self {
            batches: HashMap::default(),
            remaining_postings: RemainingPostingDirectory::default(),
            memory: MemoryLease::new(MemoryCategory::BatchScratch),
            nested_memory: MemoryLease::new(MemoryCategory::BatchScratch),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(super) enum SetChange {
    Added,
    Removed,
}

/// Exact old/new evaluation was unavailable, so conservative planning still treats the entry as
/// changed but retained-answer maintenance must use its upquery fallback.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct ExactEntryGap;

pub(super) type ExactEntryResult = Lookup<SetChange, ExactEntryGap>;

pub(super) fn exact_entry_changed(result: ExactEntryResult) -> bool {
    !matches!(result, Lookup::KnownAbsent)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub(super) struct SelectorTruthDelta {
    pub(super) node: StyleNodeID,
    pub(super) rule: RuleID,
    pub(super) entry: EntryID,
    pub(super) change: SetChange,
    /// Activation changes the active-match join, not selector truth itself.
    pub(super) selector_truth_changed: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub(super) struct SelectorTruthRefresh {
    pub(super) node: StyleNodeID,
    pub(super) rule: Option<(RuleID, EntryID)>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub(super) struct AlreadyPlannedSelectorTruthCandidate {
    pub(super) node: StyleNodeID,
    pub(super) exact_entry: Option<(RuleID, EntryID)>,
    pub(super) exact_tree_evaluation: Option<ExactTreeEvaluation>,
}

/// A transaction batch whose common singleton form neither allocates nor sorts.
#[derive(Default)]
pub(super) enum DeltaBatch<T> {
    #[default]
    Empty,
    One(T),
    Many(Vec<T>),
}

impl<T> DeltaBatch<T> {
    pub(super) fn push(&mut self, item: T) {
        match std::mem::replace(self, Self::Empty) {
            Self::Empty => *self = Self::One(item),
            Self::One(first) => *self = Self::Many(vec![first, item]),
            Self::Many(mut items) => {
                items.push(item);
                *self = Self::Many(items);
            }
        }
    }

    #[must_use]
    pub(super) fn as_slice(&self) -> &[T] {
        match self {
            Self::Empty => &[],
            Self::One(item) => std::slice::from_ref(item),
            Self::Many(items) => items,
        }
    }

    #[must_use]
    pub(super) fn capacity_bytes(&self) -> u64 {
        match self {
            Self::Empty | Self::One(_) => 0,
            Self::Many(items) => capacity_bytes! {
                shallow [*items];
                cached [];
                nested [];
                skip [];
            },
        }
    }
}

impl<T: Ord> DeltaBatch<T> {
    pub(super) fn consolidate(&mut self) {
        if let Self::Many(items) = self {
            items.sort_unstable();
            items.dedup();
        }
    }
}

/// Exact selector changes and the exceptional requests that must refresh missing truth.
///
/// An exact change is both the retained-answer maintenance payload and its provenance. A route
/// records a refresh only when it cannot preserve that old/new pair. Nodes reached through broad
/// impact regions need no entry here because the region itself is already the refresh request.
#[derive(Default)]
pub(super) struct SelectorTruthChanges {
    pub(super) deltas: DeltaBatch<SelectorTruthDelta>,
    pub(super) refreshes: DeltaBatch<SelectorTruthRefresh>,
}

#[derive(Clone, Copy)]
pub(super) enum SelectorTruthPatch<'a> {
    Full,
    Direct(&'a [SelectorTruthDelta]),
    Refresh {
        deltas: &'a [SelectorTruthDelta],
        refreshes: &'a [SelectorTruthRefresh],
    },
    /// The node is covered by rule-attributed regions: only those rules and the node's own
    /// deltas can have moved its truth, so the patch re-derives that union instead of the
    /// transaction's whole affected set.
    Attributed {
        deltas: &'a [SelectorTruthDelta],
        refreshes: &'a [SelectorTruthRefresh],
        rules: &'a [(RuleID, EntryID)],
    },
}

impl StyleEngine {
    /// Streams the element descendants of `root` in flat-tree inheritance order without asking the
    /// DOM to rediscover slot and shadow relations already resident in the engine.
    pub(super) fn for_each_flat_tree_descendant(&mut self, root: StyleNodeID, mut visit: impl FnMut(StyleNodeID)) {
        let mut pending: Vec<_> = self.tree.flat_tree_children(root).collect();
        let mut charged_bytes = pending.capacity() * size_of::<StyleNodeID>();
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, charged_bytes as u64);

        while let Some(node) = pending.pop() {
            if node.element_index().is_some() {
                visit(node);
            }

            let previous_capacity = pending.capacity();
            pending.extend(self.tree.flat_tree_children(node));
            if pending.capacity() > previous_capacity {
                let additional_bytes = (pending.capacity() - previous_capacity) * size_of::<StyleNodeID>();
                self.memory
                    .reserve_required(MemoryCategory::BatchScratch, additional_bytes as u64);
                charged_bytes += additional_bytes;
            }
        }

        self.memory.release(MemoryCategory::BatchScratch, charged_bytes as u64);
    }

    /// Record that a route needs exact selector truth refreshed for a planned node.
    pub(super) fn record_selector_truth_refresh(&mut self, node: StyleNodeID, rule: Option<(RuleID, EntryID)>) {
        if self.selector_truth_changes_active {
            self.selector_truth_changes
                .refreshes
                .push(SelectorTruthRefresh { node, rule });
        }
    }

    pub(super) fn record_exact_selector_truth_change(
        &mut self,
        node: StyleNodeID,
        site: &RoutingSite<'_>,
        result: ExactEntryResult,
    ) {
        if !self.selector_truth_changes_active {
            return;
        }
        let Some((rule, entry)) = site.exact_entry else {
            self.record_selector_truth_refresh(node, site.refresh_rule);
            return;
        };
        match result {
            Lookup::Known(kind) => {
                self.selector_truth_changes.deltas.push(SelectorTruthDelta {
                    node,
                    rule,
                    entry,
                    change: kind,
                    selector_truth_changed: true,
                });
            }
            Lookup::KnownAbsent => {}
            Lookup::Missing(_) => self.record_selector_truth_refresh(node, Some((rule, entry))),
        }
    }

    /// Preserve the exact contribution of a route whose candidate is already in the impact set.
    ///
    /// The earlier route proves only that the node needs style work. Defer this route until all
    /// symbolic coverage is known, so a coarse region can absorb it without an entry evaluation.
    pub(super) fn record_already_planned_selector_truth(&mut self, node: StyleNodeID, site: &RoutingSite<'_>) {
        if !self.selector_truth_changes_active {
            return;
        }
        // This relation exists only to patch the node's retained exact answer. A node without
        // both sides of that retained identity will be cold-completed if it survives the plan,
        // so expanding every route which also reaches it into selector-truth scratch has no
        // consumer. StyleBench sibling routes can otherwise manufacture hundreds of thousands
        // of rows for nodes whose retained payload was never admitted.
        if !matches!(self.retained_match_answer(node), Lookup::Known(_))
            || !matches!(self.retained_match_answers.cascade_input_lookup(node), Lookup::Known(_))
        {
            return;
        }
        self.already_planned_selector_truth
            .push(AlreadyPlannedSelectorTruthCandidate {
                node,
                exact_entry: site.exact_entry,
                exact_tree_evaluation: site.exact_tree_evaluation,
            });
    }

    /// Resolve deferred exact-node routes after the plan's symbolic coverage is final.
    pub(super) fn resolve_already_planned_selector_truth(
        &mut self,
        regions: &ImpactRegions,
        coarse_cover: Option<&ImpactRegionBatch>,
    ) {
        let candidates = std::mem::take(&mut self.already_planned_selector_truth);
        let candidate_bytes = candidates.capacity_bytes();
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, candidate_bytes);
        let workspace_before = self.match_workspace.capacity_bytes();
        for candidate in candidates.as_slice() {
            if coarse_cover.is_some_and(|cover| regions.batch_contains_node(cover, candidate.node)) {
                continue;
            }
            let site = RoutingSite {
                subject: &[],
                subject_required: &[],
                position: SubjectPosition::UNBOUNDED,
                path: &[],
                waypoints: &[],
                in_flux: None,
                exact_entry: candidate.exact_entry,
                exact_tree_evaluation: candidate.exact_tree_evaluation,
                refresh_rule: None,
            };
            let result = self.candidate_changes_exact_entry(candidate.node, &site);
            self.record_exact_selector_truth_change(candidate.node, &site, result);
        }
        let workspace_after = self.match_workspace.capacity_bytes();
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, workspace_after - workspace_before);
        self.memory.release(MemoryCategory::BatchScratch, candidate_bytes);
    }
}

/// Record the rules of every dispatch entry in exactly one of two match sets.
///
/// Both sets belong to one prefix-state interner, which stores their entries in canonical order.
pub(super) fn record_match_set_difference(
    changes: &mut SelectorTruthChanges,
    active: bool,
    node: StyleNodeID,
    old_matches: &[EntryID],
    new_matches: &[EntryID],
    dispatch: &RuleDispatch,
    program: &StyleSheetProgram,
) {
    if !active {
        return;
    }
    let mut record = |entry: EntryID, kind| {
        let mut previous_rule = None;
        for candidate in dispatch.entries_for_identity(entry) {
            // Prefix truth is independent of activation. Only deciding rules belong in an
            // exact cascade answer, just as when ordinary matching consumes these identities.
            if !program.rule_can_decide(candidate.rule) {
                continue;
            }
            if previous_rule == Some(candidate.rule) {
                continue;
            }
            previous_rule = Some(candidate.rule);
            changes.deltas.push(SelectorTruthDelta {
                node,
                rule: candidate.rule,
                entry,
                change: kind,
                selector_truth_changed: true,
            });
        }
    };
    debug_assert!(old_matches.is_sorted());
    debug_assert!(new_matches.is_sorted());
    for entry in merge_sorted_by(old_matches, new_matches, Ord::cmp) {
        match entry {
            SortedMergeEntry::Left(&entry) => record(entry, SetChange::Removed),
            SortedMergeEntry::Right(&entry) => record(entry, SetChange::Added),
            SortedMergeEntry::Both(_, _) => {}
        }
    }
}

/// Difference two exact per-node match relations into signed selector truth.
pub(super) fn repaired_selector_truth_deltas(
    node: StyleNodeID,
    old_matches: &[RetainedRuleMatch],
    new_matches: &mut [RuleMatch],
    programs: &SelectorPrograms,
) -> Option<Vec<SelectorTruthDelta>> {
    const LINEAR_SEARCH_COMPARISONS: usize = 16;
    let retained_key = |entry: &RetainedRuleMatch| (entry.rule, entry.program, entry.entry);
    let current_key = |entry: &RuleMatch| (entry.rule, entry.program, entry.entry);
    let retained_is_sorted = old_matches
        .windows(2)
        .all(|pair| retained_key(&pair[0]) < retained_key(&pair[1]));
    if retained_is_sorted && old_matches.len().saturating_mul(new_matches.len()) > LINEAR_SEARCH_COMPARISONS {
        new_matches.sort_unstable_by_key(current_key);
        debug_assert!(
            !new_matches
                .windows(2)
                .any(|pair| current_key(&pair[0]) == current_key(&pair[1]))
        );
        let mut deltas = Vec::new();
        for entry in merge_sorted_by(old_matches, new_matches, |old, new| {
            retained_key(old).cmp(&current_key(new))
        }) {
            match entry {
                SortedMergeEntry::Both(old, new) => {
                    if old.scope_proximity != new.scope_proximity {
                        return None;
                    }
                }
                SortedMergeEntry::Left(old) => {
                    deltas.push(SelectorTruthDelta {
                        node,
                        rule: old.rule,
                        entry: programs.entry_id(old.program, old.entry),
                        change: SetChange::Removed,
                        selector_truth_changed: true,
                    });
                }
                SortedMergeEntry::Right(new) => {
                    deltas.push(SelectorTruthDelta {
                        node,
                        rule: new.rule,
                        entry: programs.entry_id(new.program, new.entry),
                        change: SetChange::Added,
                        selector_truth_changed: true,
                    });
                }
            }
        }
        return Some(deltas);
    }

    let mut deltas = Vec::new();
    for entry in old_matches {
        let current = new_matches
            .iter()
            .find(|candidate| current_key(candidate) == retained_key(entry));
        if current.is_some_and(|current| current.scope_proximity != entry.scope_proximity) {
            return None;
        }
        if current.is_none() {
            deltas.push(SelectorTruthDelta {
                node,
                rule: entry.rule,
                entry: programs.entry_id(entry.program, entry.entry),
                change: SetChange::Removed,
                selector_truth_changed: true,
            });
        }
    }
    for entry in new_matches {
        let retained = old_matches
            .iter()
            .any(|candidate| retained_key(candidate) == current_key(entry));
        if !retained {
            deltas.push(SelectorTruthDelta {
                node,
                rule: entry.rule,
                entry: programs.entry_id(entry.program, entry.entry),
                change: SetChange::Added,
                selector_truth_changed: true,
            });
        }
    }
    deltas.sort_unstable();
    Some(deltas)
}

impl SelectorTruthChanges {
    pub(super) fn consolidate(&mut self, counters: &mut Counters) {
        match &mut self.deltas {
            DeltaBatch::Empty => {}
            DeltaBatch::One(delta) => {
                if delta.selector_truth_changed {
                    counters.bump(match delta.change {
                        SetChange::Added => Counter::SelectorTruthAdditions,
                        SetChange::Removed => Counter::SelectorTruthRemovals,
                    });
                }
            }
            DeltaBatch::Many(deltas) => {
                deltas.sort_unstable();
                let mut read = 0;
                let mut write = 0;
                while read < deltas.len() {
                    let first = deltas[read];
                    let key = (first.node, first.rule, first.entry);
                    let mut added = false;
                    let mut removed = false;
                    let mut selector_truth_changed = false;
                    while read < deltas.len() && (deltas[read].node, deltas[read].rule, deltas[read].entry) == key {
                        match deltas[read].change {
                            SetChange::Added => added = true,
                            SetChange::Removed => removed = true,
                        }
                        selector_truth_changed |= deltas[read].selector_truth_changed;
                        read += 1;
                    }
                    if added && removed {
                        counters.bump(Counter::SelectorTruthCancellations);
                        continue;
                    }
                    let change = if added {
                        if selector_truth_changed {
                            counters.bump(Counter::SelectorTruthAdditions);
                        }
                        SetChange::Added
                    } else {
                        if selector_truth_changed {
                            counters.bump(Counter::SelectorTruthRemovals);
                        }
                        SetChange::Removed
                    };
                    deltas[write] = SelectorTruthDelta {
                        node: first.node,
                        rule: first.rule,
                        entry: first.entry,
                        change,
                        selector_truth_changed,
                    };
                    write += 1;
                }
                deltas.truncate(write);
            }
        }
        self.refreshes.consolidate();
        counters.add(Counter::SelectorTruthRefreshes, self.refreshes.as_slice().len() as u64);
    }

    pub(super) fn range_for_node<T>(
        items: &[T],
        node: StyleNodeID,
        node_of: impl Fn(&T) -> StyleNodeID,
    ) -> std::ops::Range<usize> {
        let start = items.partition_point(|item| node_of(item) < node);
        let end = start + items[start..].partition_point(|item| node_of(item) == node);
        start..end
    }

    #[must_use]
    pub(super) fn deltas_for(&self, node: StyleNodeID) -> &[SelectorTruthDelta] {
        let items = self.deltas.as_slice();
        &items[Self::range_for_node(items, node, |delta| delta.node)]
    }

    #[must_use]
    pub(super) fn refreshes_for(&self, node: StyleNodeID) -> &[SelectorTruthRefresh] {
        let items = self.refreshes.as_slice();
        &items[Self::range_for_node(items, node, |refresh| refresh.node)]
    }

    #[must_use]
    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [];
            nested [self.deltas.capacity_bytes(), self.refreshes.capacity_bytes()];
            skip [];
        }
    }
}

impl ImpactPlanningWorkspace {
    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.batches,
            ];
            cached [self.nested_memory.bytes()];
            nested [self.remaining_postings.capacity_bytes()];
            skip [self.memory];
        }
    }

    pub(super) fn insert_batch(&mut self, regions: &[ImpactRegion], batch: Rc<ImpactRegionBatch>) {
        let previous_batch_bytes = self.batches.get(regions).map_or(0, |previous| previous.storage_bytes());
        let regions = regions.to_vec();
        let region_bytes = regions.capacity() * size_of::<ImpactRegion>();
        let batch_bytes = batch.storage_bytes();
        let payload_bytes = region_bytes + batch_bytes;
        if self.batches.insert(regions, batch).is_some() {
            if batch_bytes >= previous_batch_bytes {
                self.nested_memory
                    .grow_committed((batch_bytes - previous_batch_bytes) as u64);
            } else {
                self.nested_memory
                    .shrink_committed((previous_batch_bytes - batch_bytes) as u64);
            }
        } else {
            self.nested_memory.grow_committed(payload_bytes as u64);
        }
    }

    pub(super) fn settle_memory(&mut self, memory: &mut MemoryController) {
        let nested = self.nested_memory.bytes();
        self.nested_memory.reconcile_committed(memory, nested);
        let header = self.capacity_bytes() - self.nested_memory.bytes();
        self.memory.resize_required_to(memory, header);
    }

    pub(super) fn extend_remaining_posting(
        &mut self,
        key: PostingKey,
        postings: &FeaturePostings,
        plan: &ImpactRegions,
        candidates: &mut Vec<StyleNodeID>,
        pruned_nodes: Option<&mut Vec<StyleNodeID>>,
    ) -> Result<(u64, bool, usize, usize, usize), PostingKey> {
        let bytes_before = self.capacity_bytes();
        let posting = match postings.lookup(key) {
            Lookup::Known(posting) => posting,
            Lookup::KnownAbsent => return Ok((0, false, 0, 0, 0)),
            Lookup::Missing(gap) => return Err(gap),
        };
        let was_present = self.remaining_postings.entry(key).is_some();
        let copied = if !was_present { posting.len() } else { 0 };
        let remaining = if was_present {
            self.remaining_postings.entry(key).unwrap()
        } else {
            let candidates: Vec<StyleNodeID> = posting.candidates().collect();
            self.nested_memory
                .grow_committed((candidates.capacity() * size_of::<StyleNodeID>()) as u64);
            self.remaining_postings.insert(
                key,
                RemainingPosting {
                    nodes: candidates,
                    plan_generation: u64::MAX,
                    pruned_nodes: Vec::new(),
                },
            )
        };
        let posting = &mut remaining.nodes;
        let mut inspected = 0;
        let mut pruned = 0;
        let plan_generation = plan.exact_node_generation();
        if remaining.plan_generation != plan_generation {
            let pruned_this_call = &mut remaining.pruned_nodes;
            let pruned_capacity_before = pruned_this_call.capacity();
            let previous_pruned_length = pruned_this_call.len();
            let point_removed = plan.for_each_exact_node_added_after(
                remaining.plan_generation,
                MAX_POINT_REMOVED_EXACT_NODES,
                |node| {
                    if let Ok(index) = posting.binary_search(&node) {
                        posting.remove(index);
                        pruned_this_call.push(node);
                    }
                },
            );
            if point_removed {
                pruned = pruned_this_call.len() - previous_pruned_length;
            } else {
                inspected = posting.len();
                let previous_length = posting.len();
                posting.retain(|&node| {
                    let keep = !plan.contains_node(node);
                    if !keep {
                        pruned_this_call.push(node);
                    }
                    keep
                });
                pruned = previous_length - posting.len();
            }
            remaining.plan_generation = plan_generation;
            self.nested_memory.grow_committed(
                ((pruned_this_call.capacity() - pruned_capacity_before) * size_of::<StyleNodeID>()) as u64,
            );
        }
        // Every route consuming this posting reports the full pruned history: nodes dropped for
        // an earlier route are invisible to this one too. Without a consumer the history is
        // still kept, but nothing is copied or walked.
        if let Some(pruned_nodes) = pruned_nodes {
            pruned_nodes.extend_from_slice(&remaining.pruned_nodes);
        }
        candidates.extend_from_slice(posting);
        Ok((
            self.capacity_bytes().saturating_sub(bytes_before),
            was_present,
            copied,
            inspected,
            pruned,
        ))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub(super) enum SiblingRouteKind {
    Full,
    BeyondSibling,
    Departure,
    ExactUnion,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub(super) struct PendingSiblingRoute {
    pub(super) route: RouteID,
    pub(super) kind: SiblingRouteKind,
    pub(super) exact_tree_evaluation: Option<ExactTreeEvaluation>,
}

#[derive(Clone, Copy, Default)]
pub(super) struct PrefixConvergenceOutcome {
    pub(super) sibling_routes_are_covered: bool,
    /// Whether the walk re-compared every member of every touched child sequence, so the
    /// positional truth movements of admitted entries are already exact planned regions and
    /// their deferred sequence routes need no generic narrowing.
    pub(super) positional_routes_are_covered: bool,
}

#[derive(Clone, Copy)]
pub(super) struct PendingPrefixNode {
    pub(super) node: StyleNodeID,
    pub(super) entering_deltas: PrefixEnteringDeltas,
}

pub(super) trait PendingRouteKey: Copy + Ord {
    fn route(self) -> RouteID;
}

impl PendingRouteKey for PendingRoute {
    fn route(self) -> RouteID {
        self.route
    }
}

impl PendingRouteKey for PendingSiblingRoute {
    fn route(self) -> RouteID {
        self.route
    }
}

pub(super) const PENDING_ROUTE_PAGE_SHIFT: usize = 6;
pub(super) const PENDING_ROUTE_PAGE_SIZE: usize = 1 << PENDING_ROUTE_PAGE_SHIFT;
pub(super) const NO_PENDING_ROUTE: u32 = u32::MAX;

struct SparseIndexPage {
    entries: [u32; PENDING_ROUTE_PAGE_SIZE],
}

impl Default for SparseIndexPage {
    fn default() -> Self {
        Self {
            entries: [u32::MAX; PENDING_ROUTE_PAGE_SIZE],
        }
    }
}

impl PagedColumnPage for SparseIndexPage {
    type Value = u32;

    const SHIFT: usize = PENDING_ROUTE_PAGE_SHIFT;

    fn get(&self, index: usize) -> Option<u32> {
        (self.entries[index] != u32::MAX).then_some(self.entries[index])
    }

    fn insert(&mut self, index: usize, value: u32) {
        self.entries[index] = value;
    }
}

pub(super) struct PendingRegionEntry<K> {
    pub(super) key: K,
    pub(super) regions: Vec<ImpactRegion>,
    pub(super) next_for_route: u32,
}

pub(super) struct PendingRegionTable<K> {
    pages: PagedColumn<SparseIndexPage>,
    pub(super) entries: Vec<PendingRegionEntry<K>>,
}

impl<K> Default for PendingRegionTable<K> {
    fn default() -> Self {
        Self {
            pages: PagedColumn::default(),
            entries: Vec::new(),
        }
    }
}

impl<K: PendingRouteKey> PendingRegionTable<K> {
    pub(super) fn new() -> Self {
        Self::default()
    }

    pub(super) fn head(&self, route: RouteID) -> u32 {
        let index = route.index();
        self.pages.get(index).unwrap_or(NO_PENDING_ROUTE)
    }

    pub(super) fn set_head(&mut self, route: RouteID, head: u32) {
        let index = route.index();
        self.pages.insert(index, head);
    }

    pub(super) fn regions_for(&mut self, key: K) -> &mut Vec<ImpactRegion> {
        let mut entry = self.head(key.route());
        while entry != NO_PENDING_ROUTE {
            let index = entry as usize;
            if self.entries[index].key == key {
                return &mut self.entries[index].regions;
            }
            entry = self.entries[index].next_for_route;
        }

        let next_for_route = self.head(key.route());
        let entry = u32::try_from(self.entries.len()).expect("pending route space exhausted");
        self.entries.push(PendingRegionEntry {
            key,
            regions: Vec::new(),
            next_for_route,
        });
        self.set_head(key.route(), entry);
        &mut self.entries[entry as usize].regions
    }

    pub(super) fn rebuild_index(&mut self) {
        for page in self.pages.pages_mut() {
            page.entries.fill(NO_PENDING_ROUTE);
        }
        for entry in 0..self.entries.len() {
            let route = self.entries[entry].key.route();
            let next = self.head(route);
            self.entries[entry].next_for_route = next;
            self.set_head(route, u32::try_from(entry).expect("pending route space exhausted"));
        }
    }

    pub(super) fn finish(&mut self) {
        self.entries.sort_unstable_by_key(|entry| entry.key);
        self.rebuild_index();
    }

    pub(super) fn keys(&self) -> impl Iterator<Item = K> + '_ {
        self.entries.iter().map(|entry| entry.key)
    }

    pub(super) fn values(&self) -> impl Iterator<Item = &Vec<ImpactRegion>> {
        self.entries.iter().map(|entry| &entry.regions)
    }

    pub(super) fn iter_mut(&mut self) -> impl Iterator<Item = (K, &mut Vec<ImpactRegion>)> {
        self.entries.iter_mut().map(|entry| (entry.key, &mut entry.regions))
    }

    pub(super) fn retain(&mut self, mut keep: impl FnMut(K, &mut Vec<ImpactRegion>) -> bool) {
        self.entries.retain_mut(|entry| keep(entry.key, &mut entry.regions));
        self.rebuild_index();
    }

    pub(super) fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub(super) fn len(&self) -> usize {
        self.entries.len()
    }

    pub(super) fn capacity_bytes(&self) -> usize {
        (capacity_bytes! {
            shallow [self.entries];
            cached [self.pages.capacity_bytes()];
            nested [];
            skip [];
        }) as usize
    }
}

pub(super) type PendingRoutes = PendingRegionTable<PendingRoute>;
pub(super) type PendingSiblingRoutes = PendingRegionTable<PendingSiblingRoute>;

/// What one transaction did to one child sequence.
///
/// A sequence that changed several times before anything observed it moved once, by as much as all
/// of those changes together. Measuring each change against the sequence as it ends up answers a
/// question nobody asked: two rows appended move `:last-child` two positions, so the row that
/// stopped being last is two away from the end and not one.
#[derive(Clone)]
pub(super) struct SequenceChange {
    /// The final child sequence, shared with exact evaluation and the following matching pass.
    pub(super) children: Rc<[StyleNodeID]>,
    pub(super) arrivals: u32,
    pub(super) departures: u32,
    /// The earliest and latest place in the final sequence that a change happened at.
    pub(super) span: Option<(usize, usize)>,
    /// Set when a change's place could not be found, which leaves the whole sequence moved.
    pub(super) unlocated: bool,
    /// The latest place an element that was already alive landed in. An element created here has no
    /// count to have moved, but one that moved into the place does, and nothing after it moved at
    /// all - so this is the one position on the far side of the span that a count from the end can
    /// still have crossed.
    pub(super) last_relocation: Option<usize>,
    /// The qualified types that joined or left this sequence. An `of-type` position can move only
    /// for children of one of these types, so retaining this transaction-local summary avoids
    /// routing every other type through every positional route.
    pub(super) changed_types: Vec<(StyleAtomID, StyleAtomID)>,
    /// A missing fact row must not make the type summary reject anything.
    pub(super) has_unidentified_type: bool,
    /// Newly allocated children have no old count to compare. Their subtree is already in the
    /// plan, but structural routing still needs to keep them as possible relational witnesses.
    /// Recorded unsorted with duplicates; `SequenceChanges::finish` normalizes for membership
    /// queries so a batch of k arrivals does not pay k linear scans here.
    pub(super) arriving_nodes: Vec<StyleNodeID>,
    /// Position index over `children`, built from the second located record on: one batch of k
    /// changes would otherwise pay O(k * n) placement scans against the final sequence.
    pub(super) positions: Option<HashMap<StyleNodeID, u32>>,
    pub(super) located_records: u32,
    /// Where in the final sequence each relational seam is, and which way it moved. Recorded only
    /// while some relational route is attached, and consumed once per parent by the deferred
    /// relational sequence pass. A record that could not be located carries
    /// `RELATIONAL_RECORD_UNLOCATED` and widens to the whole sequence.
    pub(super) relational_records: Vec<(u32, SequenceSide)>,
}

impl SequenceChange {
    pub(super) fn new(children: Rc<[StyleNodeID]>) -> Self {
        Self {
            children,
            arrivals: 0,
            departures: 0,
            span: None,
            unlocated: false,
            last_relocation: None,
            changed_types: Vec::new(),
            has_unidentified_type: false,
            arriving_nodes: Vec::new(),
            positions: None,
            located_records: 0,
            relational_records: Vec::new(),
        }
    }

    pub(super) fn record(
        &mut self,
        relations: TreeRelations,
        arriving: bool,
        relocated: bool,
        changed_type: Option<(StyleAtomID, StyleAtomID)>,
        changed_node: StyleNodeID,
        relational: Option<SequenceSide>,
    ) {
        match arriving {
            true => self.arrivals += 1,
            false => self.departures += 1,
        }
        match changed_type {
            Some(changed_type) if !self.changed_types.contains(&changed_type) => {
                self.changed_types.push(changed_type);
            }
            Some(_) => {}
            None => self.has_unidentified_type = true,
        }
        if arriving && !relocated {
            self.arriving_nodes.push(changed_node);
        }
        let located = self.locate(relations);
        match located {
            Some(at) => {
                self.span = Some(match self.span {
                    Some((first, last)) => (first.min(at), last.max(at)),
                    None => (at, at),
                });
                if arriving && relocated {
                    self.last_relocation = Some(match self.last_relocation {
                        Some(latest) => latest.max(at),
                        None => at,
                    });
                }
            }
            None => self.unlocated = true,
        }
        if let Some(side) = relational {
            let at = located.map_or(RELATIONAL_RECORD_UNLOCATED, |at| at as u32);
            self.relational_records.push((at, side));
        }
    }

    /// Where in the sequence as it ends up a change with these relations happened.
    ///
    /// The sibling a change names is normally still there, and the place is beside it. When it is
    /// not - because it left in this same transaction - the sibling on the other side answers, and a
    /// change at either end of the sequence needs no sibling to place it at all. Only a change
    /// between two elements that both left is unplaceable.
    pub(super) fn locate(&mut self, relations: TreeRelations) -> Option<usize> {
        self.located_records = self.located_records.saturating_add(1);
        match relations.previous_element_sibling {
            None => return Some(0),
            Some(previous) => {
                if let Some(at) = self.position_of(previous) {
                    return Some(at + 1);
                }
            }
        }
        match relations.next_element_sibling {
            None => Some(self.children.len()),
            Some(next) => self.position_of(next),
        }
    }

    /// Where one child sits in the final sequence. The first record scans, since one change is
    /// the common shape and a scan reaches exactly as far as its place; from the second record on
    /// the sequence is indexed once so a batch of k changes costs O(n + k) rather than O(k * n).
    pub(super) fn position_of(&mut self, child: StyleNodeID) -> Option<usize> {
        if self.positions.is_none() {
            if self.located_records <= 1 {
                return self.children.iter().position(|&candidate| candidate == child);
            }
            self.positions = Some(
                self.children
                    .iter()
                    .enumerate()
                    .map(|(at, &candidate)| (candidate, at as u32))
                    .collect(),
            );
        }
        self.positions.as_ref().unwrap().get(&child).map(|&at| at as usize)
    }

    pub(super) fn auxiliary_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.changed_types, self.arriving_nodes, self.relational_records];
            cached [];
            nested [self.positions.as_ref().map_or(0, |positions| capacity_bytes! {
                shallow [*positions];
                cached [];
                nested [];
                skip [];
            })];
            skip [self.children, self.arrivals, self.departures, self.span, self.unlocated, self.located_records];
        }
    }
}

/// The position of a relational sequence record that could not be located in the final sequence,
/// which happens only for a change between two elements that both left. It widens every candidate
/// range derived from positions to the whole sequence.
pub(super) const RELATIONAL_RECORD_UNLOCATED: u32 = u32::MAX;

pub(super) const NO_SEQUENCE_CHANGE: u32 = u32::MAX;

/// What one transaction did to the child sequences it touched, keyed by dense parent identity.
///
/// Direct pages make repeated mutation-time updates constant-time. Entries stay packed and are
/// sorted once before routing, so region emission retains deterministic parent order.
#[derive(Default)]
pub(super) struct SequenceChanges {
    pages: PagedColumn<SparseIndexPage>,
    pub(super) entries: Vec<(StyleNodeID, SequenceChange)>,
}

impl SequenceChanges {
    pub(super) fn new() -> Self {
        Self::default()
    }

    pub(super) fn entry_index(&self, parent: StyleNodeID) -> Option<usize> {
        let index = parent.element_index()? as usize;
        self.pages.get(index).map(|entry| entry as usize)
    }

    pub(super) fn get_or_insert_with(
        &mut self,
        parent: StyleNodeID,
        make_change: impl FnOnce() -> SequenceChange,
    ) -> &mut SequenceChange {
        if let Some(index) = self.entry_index(parent) {
            return &mut self.entries[index].1;
        }

        let element_index = parent
            .element_index()
            .expect("only elements can own sibling sequence changes") as usize;
        let entry = u32::try_from(self.entries.len()).expect("sequence change space exhausted");
        self.pages.insert(element_index, entry);
        self.entries.push((parent, make_change()));
        &mut self.entries[entry as usize].1
    }

    pub(super) fn finish(&mut self) {
        for (_, change) in &mut self.entries {
            change.arriving_nodes.sort_unstable();
            change.arriving_nodes.dedup();
        }
        self.entries.sort_unstable_by_key(|&(parent, _)| parent);
        for page in self.pages.pages_mut() {
            page.entries.fill(NO_SEQUENCE_CHANGE);
        }
        for (entry, &(parent, _)) in self.entries.iter().enumerate() {
            let element_index = parent.element_index().unwrap() as usize;
            self.pages.insert(
                element_index,
                u32::try_from(entry).expect("sequence change space exhausted"),
            );
        }
    }

    pub(super) fn iter(&self) -> impl Iterator<Item = (StyleNodeID, &SequenceChange)> {
        self.entries.iter().map(|(parent, change)| (*parent, change))
    }

    pub(super) fn get(&self, parent: StyleNodeID) -> Option<&SequenceChange> {
        let element_index = parent.element_index()? as usize;
        let entry = self.pages.get(element_index)?;
        Some(&self.entries[entry as usize].1)
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.entries];
            cached [self.pages.capacity_bytes()];
            nested [
                self
                .entries
                .iter()
                .map(|(_, change)| change.auxiliary_capacity_bytes())
                .sum::<u64>(),
            ];
            skip [];
        }
    }
}

pub(super) struct SiblingCandidateWorkspace {
    pub(super) entry_by_route: Vec<usize>,
    pub(super) candidate_epochs: EpochColumn,
    pub(super) candidates: Vec<usize>,
    pub(super) epoch: u32,
}

impl SiblingCandidateWorkspace {
    pub(super) fn new(entries: &[SiblingEntry]) -> Self {
        let route_count = entries.iter().map(|entry| entry.route.index() + 1).max().unwrap_or(0);
        let mut entry_by_route = vec![usize::MAX; route_count];
        for (index, entry) in entries.iter().enumerate() {
            entry_by_route[entry.route.index()] = index;
        }
        Self {
            entry_by_route,
            candidate_epochs: {
                let mut column = EpochColumn::default();
                column.ensure_len(entries.len());
                column
            },
            candidates: Vec::with_capacity(entries.len()),
            epoch: 0,
        }
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.entry_by_route, self.candidate_epochs, self.candidates];
            cached [];
            nested [];
            skip [self.epoch];
        }
    }

    pub(super) fn begin(&mut self) {
        advance_epoch(&mut self.epoch, 1, &mut [&mut self.candidate_epochs]);
        self.candidates.clear();
    }

    pub(super) fn append(&mut self, route: RouteID) {
        let Some(&candidate) = self.entry_by_route.get(route.index()) else {
            return;
        };
        if candidate == usize::MAX || !self.candidate_epochs.mark(candidate, self.epoch) {
            return;
        }
        self.candidates.push(candidate);
    }
}

impl SiblingEntry {
    pub(super) fn site<'a>(
        &self,
        routing: &'a RoutingRegistry,
        exact_tree_evaluation: Option<ExactTreeEvaluation>,
    ) -> RoutingSite<'a> {
        let point = routing.route(self.route);
        RoutingSite {
            subject: routing.subject_dispatch_of(self.route),
            subject_required: routing.subject_required_of(self.route),
            position: routing.subject_position_of(self.route),
            path: routing.path_of(self.route),
            waypoints: routing.waypoints_of(self.route),
            in_flux: None,
            exact_entry: exact_tree_evaluation.map(|_| (routing.rule_of(self.route), point.entry)),
            exact_tree_evaluation,
            refresh_rule: Some((routing.rule_of(self.route), point.entry)),
        }
    }

    /// The same site for a walk that starts where the sibling step already landed.
    ///
    /// The waypoints are the compounds the path passes through, checked by walking back from a
    /// candidate; dropping the step drops the compound it stood for. A registry that reports the two
    /// at different lengths is telling `path_meets_waypoints` not to walk at all, and that stays
    /// true of the tails.
    pub(super) fn site_beyond_the_sibling_step<'a>(
        &self,
        routing: &'a RoutingRegistry,
        exact_tree_evaluation: Option<ExactTreeEvaluation>,
    ) -> RoutingSite<'a> {
        let point = routing.route(self.route);
        let path = routing.path_of(self.route);
        let all_waypoints = routing.waypoints_of(self.route);
        let waypoints = match all_waypoints.len() == path.len() {
            true => &all_waypoints[1..],
            false => all_waypoints,
        };
        RoutingSite {
            subject: routing.subject_dispatch_of(self.route),
            subject_required: routing.subject_required_of(self.route),
            position: routing.subject_position_of(self.route),
            path: &path[1..],
            waypoints,
            in_flux: None,
            exact_entry: Some((routing.rule_of(self.route), point.entry)),
            exact_tree_evaluation,
            refresh_rule: Some((routing.rule_of(self.route), point.entry)),
        }
    }
}

/// One live structural route used by every sequence touched by a transaction.
#[derive(Clone)]
pub(super) struct SequenceEntry {
    pub(super) route: RouteID,
    pub(super) operator: SelectorOp,
    pub(super) can_compare_exactly: bool,
}

/// Sequence routes whose entries the prefix automaton answers exactly, held back until the
/// convergence walk reports whether its exact per-member diffs subsumed them. The held entries
/// are not routed until then: `entries` is the complete live entry list, so the shared entry
/// index applies to it, and `deferred` marks the ones held back.
pub(super) struct DeferredSequenceRoutes {
    pub(super) entries: Vec<SequenceEntry>,
    pub(super) deferred: Vec<bool>,
    pub(super) memory: MemoryLease,
}

impl Default for DeferredSequenceRoutes {
    fn default() -> Self {
        Self {
            entries: Vec::new(),
            deferred: Vec::new(),
            memory: MemoryLease::new(MemoryCategory::BatchScratch),
        }
    }
}

impl DeferredSequenceRoutes {
    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.entries, self.deferred];
            cached [];
            nested [];
            skip [self.memory];
        }
    }
}

/// Which sequence entries one routing pass handles: the ones the convergence walk cannot answer
/// on the immediate pass, and the held-back ones when the walk failed to cover them.
#[derive(Clone, Copy)]
pub(super) struct SequenceEntrySelection<'a> {
    pub(super) mask: &'a [bool],
    pub(super) route_masked: bool,
}

impl SequenceEntrySelection<'_> {
    #[must_use]
    pub(super) fn selects(self, entry_index: usize) -> bool {
        self.mask[entry_index] == self.route_masked
    }
}

pub(super) struct NthSequenceEntryIndex {
    pub(super) nth: NthPosition,
    pub(super) unindexed: Vec<usize>,
    pub(super) by_origin: HashMap<DispatchKey, Vec<usize>>,
    pub(super) candidate_epochs: EpochColumn,
    pub(super) candidates: Vec<usize>,
    pub(super) epoch: u32,
}

#[derive(Default)]
pub(super) struct SequenceEntryIndex {
    pub(super) empty: Vec<usize>,
    pub(super) nth: Vec<NthSequenceEntryIndex>,
}

impl SequenceEntryIndex {
    pub(super) fn build(entries: &[SequenceEntry], routing: &RoutingRegistry) -> Self {
        let mut index = Self::default();
        let mut nth_groups = HashMap::default();
        for (entry_index, entry) in entries.iter().enumerate() {
            let SelectorOp::NthPosition(nth) = entry.operator else {
                index.empty.push(entry_index);
                continue;
            };
            let group_index = *nth_groups.entry(nth).or_insert_with(|| {
                index.nth.push(NthSequenceEntryIndex {
                    nth,
                    unindexed: Vec::new(),
                    by_origin: HashMap::default(),
                    candidate_epochs: {
                        let mut column = EpochColumn::default();
                        column.ensure_len(entries.len());
                        column
                    },
                    candidates: Vec::with_capacity(entries.len()),
                    epoch: 0,
                });
                index.nth.len() - 1
            });
            let group = &mut index.nth[group_index];
            let point = routing.route(entry.route);
            let origin = routing.origin_dispatch_of(entry.route);
            // A relative positional input is a possible witness, so its originating compound
            // cannot reject it from the final tree alone. Non-resident dispatch keys likewise
            // cannot be used to enumerate all possible origins.
            if point.anchor.is_some() || origin.is_empty() || origin.iter().any(|key| !key.has_selector_posting()) {
                group.unindexed.push(entry_index);
                continue;
            }
            for &key in origin {
                group.by_origin.entry(key).or_default().push(entry_index);
            }
        }
        index
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.empty, self.nth];
            cached [];
            nested [self
                .nth
                .iter()
                .map(|group| {
                    capacity_bytes! {
                        shallow [group.unindexed, group.candidate_epochs, group.candidates, group.by_origin];
                        cached [];
                        nested [group
                            .by_origin
                            .values()
                            .map(|entries| entries.capacity() * size_of::<usize>())
                            .sum::<usize>()];
                        skip [group.nth, group.epoch];
                    }
                })
                .sum::<u64>()];
            skip [];
        }
    }
}

impl SequenceEntry {
    pub(super) fn site<'a>(&self, routing: &'a RoutingRegistry) -> RoutingSite<'a> {
        let point = routing.route(self.route);
        RoutingSite {
            subject: routing.subject_dispatch_of(self.route),
            subject_required: routing.subject_required_of(self.route),
            position: routing.subject_position_of(self.route),
            path: routing.path_of(self.route),
            waypoints: routing.waypoints_of(self.route),
            in_flux: None,
            exact_entry: self
                .can_compare_exactly
                .then_some((routing.rule_of(self.route), point.entry)),
            refresh_rule: Some((routing.rule_of(self.route), point.entry)),
            exact_tree_evaluation: self
                .can_compare_exactly
                .then_some(ExactTreeEvaluation::BeforeSiblingRelations),
        }
    }
}

/// The routing site of one relational route, which is how its regions are narrowed.
pub(super) fn relational_route_site(routing: &RoutingRegistry, route: RouteID) -> RoutingSite<'_> {
    let point = routing.route(route);
    RoutingSite {
        subject: routing.subject_dispatch_of(route),
        subject_required: routing.subject_required_of(route),
        position: routing.subject_position_of(route),
        path: routing.path_of(route),
        waypoints: routing.waypoints_of(route),
        in_flux: None,
        exact_entry: None,
        exact_tree_evaluation: None,
        refresh_rule: Some((routing.rule_of(route), point.entry)),
    }
}

/// What a routed input's transpose site says about where its subjects are.
///
/// The subject compound to enumerate by, the position in a sibling sequence the subject is allowed
/// to hold, the inverse path back to it, the compounds that path has to pass through, and the one
/// element whose fact is mid-change. Every step of routing needs all of it, so it travels together.
#[derive(Clone, Copy)]
pub(super) struct RoutingSite<'a> {
    pub(super) subject: &'a [DispatchKey],
    pub(super) subject_required: &'a [DispatchKey],
    pub(super) position: SubjectPosition,
    pub(super) path: &'a [InverseStep],
    pub(super) waypoints: &'a [DispatchKey],
    pub(super) in_flux: Option<(StyleNodeID, DispatchKey)>,
    pub(super) exact_entry: Option<(RuleID, EntryID)>,
    pub(super) exact_tree_evaluation: Option<ExactTreeEvaluation>,
    /// The one rule this route can move when it cannot name an exact entry, for refresh and
    /// patch-cover attribution. A route that knows neither poisons covered answers to full
    /// re-derivation.
    pub(super) refresh_rule: Option<(RuleID, EntryID)>,
}

impl RoutingSite<'_> {
    /// The rule this route attributes its coverage to, from the exact entry when it has one.
    #[must_use]
    pub(super) fn attribution(&self) -> Option<(RuleID, EntryID)> {
        self.exact_entry.or(self.refresh_rule)
    }
}

/// The transaction-local output and retry state of one cold scope ask.
pub(super) struct BatchMatchAttempt<'a> {
    pub(super) matches: &'a mut RuleMatches,
    pub(super) requests: Option<&'a mut Vec<Incomplete>>,
    pub(super) completed: Option<&'a mut [bool]>,
    pub(super) deferred_prefix_matches: Option<&'a mut Option<PrefixMatchSetID>>,
    pub(super) answer_is_exact: Option<&'a mut bool>,
    pub(super) cascade_only: bool,
}

pub(super) struct BatchMatchRetry<'a> {
    pub(super) requests: Option<&'a mut Vec<Incomplete>>,
    pub(super) completed: Option<&'a mut Vec<bool>>,
    pub(super) deferred_prefix_matches: Option<&'a mut Option<PrefixMatchSetID>>,
    pub(super) answer_is_exact: Option<&'a mut bool>,
    pub(super) cascade_only: bool,
}

#[derive(Clone, Copy)]
pub(super) struct TreeRoutingMode<'a> {
    pub(super) use_exact: bool,
    pub(super) has_before_sibling_relations: bool,
    pub(super) transaction_inputs: &'a [NormalizedInput],
}

impl TreeRoutingMode<'_> {
    pub(super) fn tree_position_changed(self, node: StyleNodeID) -> bool {
        let Ok(index) = self
            .transaction_inputs
            .binary_search_by_key(&InputKey::TreeRelations(node), |input| input.key)
        else {
            return false;
        };
        let relations = |value| match value {
            InputValue::TreeRelations(relations) => relations,
            _ => None,
        };
        let old = relations(self.transaction_inputs[index].old);
        let new = relations(self.transaction_inputs[index].new);
        old.and_then(|relations| relations.parent) != new.and_then(|relations| relations.parent)
            || old.and_then(|relations| relations.next_element_sibling)
                != new.and_then(|relations| relations.next_element_sibling)
    }
}

pub(super) struct ProgramRoutingContext<'a> {
    pub(super) resident_nodes: Option<&'a [StyleNodeID]>,
    pub(super) winner_program_version: Option<ProgramVersion>,
    pub(super) document_root: StyleNodeID,
    pub(super) attachment_scopes: Option<&'a [TreeScopeID]>,
    pub(super) removed_rules_requiring_refresh: &'a mut Vec<RuleID>,
}

/// Which exact tree comparison a routed candidate can use.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
#[repr(usize)]
pub(super) enum ExactTreeEvaluation {
    Arrival,
    MonotonicArrival,
    MonotonicDeparture,
    BeforeSiblingRelations,
}

impl ExactTreeEvaluation {
    #[allow(clippy::too_many_arguments)]
    pub(super) fn candidate_changes(
        self,
        tree: &StyleNodeTree,
        facts: &StyleNodeFacts,
        program: (&SelectorProgram, &selector::SelectorEntry),
        node: StyleNodeID,
        old_matches: Option<bool>,
        transaction_fact_view: Option<&TransactionFactView>,
        match_workspace: &MatchEvaluationWorkspace,
        counters: &mut Counters,
    ) -> Result<ExactEntryResult, Incomplete> {
        let (compiled, entry) = program;
        // The new side reads the authoritative tree, which every candidate of this transaction
        // shares. The workspace's current tree side was reset when the transaction began, so the
        // sibling positions it memoizes were all measured in this topology, and a sequence that
        // several positional entries ask about is counted once rather than once per candidate.
        let evaluate_new = |counters: &mut Counters| {
            MatchEvaluator::new(tree, facts)
                .with_match_workspace(match_workspace, MatchEvaluationSide::Current)
                .indexing_stepped_positions_only()
                .matches_entry_without_program_caches(compiled, entry, node, counters)
        };
        let evaluate_old = |view: &TransactionFactView, counters: &mut Counters| match view.is_present(
            tree,
            TransactionFactSide::Before,
            node,
        ) {
            false => Ok(false),
            true => MatchEvaluator::new(tree, facts)
                .with_transaction_fact_view(view, TransactionFactSide::Before)
                .with_match_workspace(match_workspace, MatchEvaluationSide::OldTree)
                .matches_entry_without_program_caches(compiled, entry, node, counters),
        };
        match self {
            Self::Arrival => match evaluate_new(counters)? {
                true => Ok(Lookup::Known(SetChange::Added)),
                false => Ok(Lookup::KnownAbsent),
            },
            Self::MonotonicArrival | Self::MonotonicDeparture | Self::BeforeSiblingRelations => {
                let old = match old_matches {
                    Some(old) => old,
                    None => {
                        let Some(view) = transaction_fact_view.filter(|view| view.before_sibling_relations_available)
                        else {
                            return Ok(Lookup::Missing(ExactEntryGap));
                        };
                        evaluate_old(view, counters)?
                    }
                };
                let new = evaluate_new(counters)?;
                let change = match (old, new) {
                    (false, true) => Lookup::Known(SetChange::Added),
                    (true, false) => Lookup::Known(SetChange::Removed),
                    _ => Lookup::KnownAbsent,
                };
                Ok(change)
            }
        }
    }
}

pub(super) fn mark_element_visited(visited: &mut Vec<bool>, node: StyleNodeID) -> bool {
    let index = node.element_index().expect("prefix convergence visits only elements") as usize;
    if visited.len() <= index {
        visited.resize(index + 1, false);
    }
    if visited[index] {
        return false;
    }
    visited[index] = true;
    true
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(super) struct PrefixAnswerKey {
    pub(super) prefix_contribution: MatchAnswerID,
    pub(super) non_prefix_matches: MatchAnswerID,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct PrefixContributionKey {
    pub(super) program: ScopeProgramID,
    pub(super) matches: PrefixMatchSetID,
}
