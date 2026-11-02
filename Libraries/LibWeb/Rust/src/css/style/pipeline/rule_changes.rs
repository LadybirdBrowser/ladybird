/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What an update of the rule index changed, and the elements those changes can restyle: the ones
//! that matched an entry that died or moved in the cascade order, and the ones that match an entry
//! that arrived.
//!
//! NB: An element is restyled only where it matches one of those rules, or matched one, rather than
//!     wherever the features of the rules a sheet change adds or removes reach in the tree scope.

use super::super::*;
use super::bloom::AncestorFilter;
use super::matching::{ElementMatch, Matcher};
use super::records::{MatchList, RetainedRecords};
use super::rules::{BoundSegment, RuleIndex, ScopeGroupID};
use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID};
use smallvec::SmallVec;
use std::collections::hash_map::Entry;

/// The entries of the rule index an update changed.
#[derive(Default)]
pub(super) struct RuleIndexChanges {
    /// By their number, whether each entry died or moved against another in the cascade order: an
    /// element that matched one is restyled.
    before: Vec<bool>,
    /// The entries that arrived, in the buckets the index holds them in: an element that matches
    /// one now is restyled.
    arrived: RuleIndex,
    /// The entries that died, whose numbers a user agent index the document shares instead may
    /// give to other selectors.
    died: Vec<u32>,
    /// Whether the document shares another user agent index, whose entries the invalidation map
    /// takes in only as it is built again.
    pub(super) user_agent_moved: bool,
    /// How many entries arrived.
    pub(super) arrived_entries: usize,
    /// The first entry that arrived. Those after it arrived too, or pad a page of entries.
    pub(super) first_arrived_entry: Option<u32>,
    /// The tree scopes of the entries that changed, and those that moved to another group, in no
    /// order, each once.
    pub(super) scopes: Vec<TreeScopeID>,
    /// The shadow trees that stopped deciding with the entries of one group, or started deciding
    /// with those of another that was there before.
    regrouped: Vec<Regrouped>,
    /// Whether the entries that died declared what their rules declare now, and those that stayed
    /// stand as they stood, which lets what they decide be told from their rules.
    conditions_moved_alone: bool,
    /// The segments the update let go of, which alone still tell what their entries were.
    pub(super) let_go: Vec<BoundSegment>,
}

/// The list an element holding one list comes to hold as the given entries arrive, if it keeps its style.
type KeptLists<'a> = HashMap<(&'a [u32], SmallVec<[u32; 4]>), Option<MatchList>>;

/// A shadow tree whose sheets or layers moved, which decides with the entries of another group of
/// trees: the elements that matched an entry of the group it left, or that match one of the
/// group it joined, are restyled.
pub(super) struct Regrouped {
    pub(super) scope: TreeScopeID,
    pub(super) left: Option<ScopeGroupID>,
    /// The group it joined, where it held entries before; the entries of a new one arrived.
    pub(super) joined: Option<ScopeGroupID>,
}

/// An entry an update kept, with the ranks it held before it.
pub(super) struct StayedEntry {
    id: u32,
    ranks: Ranks,
}

impl StayedEntry {
    pub(super) fn of(id: u32, entries: &super::rules::IndexEntries) -> Self {
        Self {
            id,
            ranks: Ranks::of(entries, id),
        }
    }
}

/// Where an entry's layer, sheet and rule stand.
#[derive(Clone, Copy)]
struct Ranks {
    layer: u64,
    sheet: u64,
    rule: u64,
}

impl Ranks {
    fn of(entries: &super::rules::IndexEntries, id: u32) -> Self {
        Self::of_view(entries, entries.view(id as usize))
    }

    fn of_view(entries: &super::rules::IndexEntries, view: super::rules::EntryView<'_>) -> Self {
        let placement = entries.placement_of(view);
        Self {
            layer: placement.layer_rank,
            sheet: placement.sheet_rank,
            rule: view.rule_rank(),
        }
    }
}

/// Note in `moved` the entries of `stayed` that moved against another of them in the cascade
/// order, where `entries` holds the ranks they hold now.
pub(super) fn note_moved(entries: &super::rules::IndexEntries, stayed: &[StayedEntry], moved: &mut Vec<u32>) {
    // The entries that stayed decide against one another as they did where they keep their
    // order, for declarations of either importance: of those that do not, as few as can be
    // are taken to have moved.
    for important in [false, true] {
        let mut order: Vec<&StayedEntry> = stayed
            .iter()
            .filter(|stayed| !important || entries.entry(stayed.id as usize).declares_important())
            .collect();
        let view = |stayed: &StayedEntry| entries.view(stayed.id as usize);
        let now = |stayed: &StayedEntry| cascade_key(entries, view(stayed), Ranks::of(entries, stayed.id), important);
        let before = |stayed: &StayedEntry| cascade_key(entries, view(stayed), stayed.ranks, important);
        order.sort_unstable_by_key(|stayed| (now(stayed), before(stayed)));
        let keys: Vec<_> = order.iter().map(|stayed| before(stayed)).collect();
        let kept = longest_ordered_run(&keys);
        for (position, stayed) in order.iter().enumerate() {
            // NB: An entry that moved matches the elements it matched.
            if !kept[position] {
                moved.push(stayed.id);
            }
        }
    }
}

impl RuleIndexChanges {
    /// The changes of an update of `index` that killed `died`, moved `moved` and added `added`.
    pub(super) fn new(
        index: &RuleIndex,
        died: Vec<u32>,
        moved: &[u32],
        added: &[u32],
        user_agent_moved: bool,
        regrouped: Vec<Regrouped>,
        conditions_moved_alone: bool,
    ) -> Self {
        let mut before = vec![false; index.entries.len()];
        let mut scopes: Vec<TreeScopeID> = Vec::new();
        for &id in died.iter().chain(moved) {
            before[id as usize] = true;
        }
        let mut groups: Vec<ScopeGroupID> = Vec::new();
        for &id in died.iter().chain(moved).chain(added) {
            // NB: A user agent entry that died may have no entry of its number any more.
            let group = index
                .entries
                .get(id as usize)
                .map_or(ScopeGroupID::DOCUMENT, |_| index.entries.placement(id as usize).group);
            if !groups.contains(&group) {
                groups.push(group);
            }
        }
        // NB: The trees of a group that died moved to other groups, as `regrouped` says.
        for scope in groups
            .iter()
            .flat_map(|&group| index.scopes_of(group).iter().copied())
            .chain(regrouped.iter().map(|regrouped| regrouped.scope))
        {
            if !scopes.contains(&scope) {
                scopes.push(scope);
            }
        }
        Self {
            before,
            arrived: match added.is_empty() {
                true => RuleIndex::default(),
                false => index.buckets_of(added),
            },
            died,
            user_agent_moved,
            arrived_entries: added.len(),
            first_arrived_entry: added.iter().copied().min(),
            scopes,
            regrouped,
            conditions_moved_alone,
            let_go: Vec::new(),
        }
    }

    /// Each entry as itself where it lives and did not die in this update, and none otherwise:
    /// the number of a user agent entry that died may stand for another selector now.
    pub(super) fn numbering(&self, index: &RuleIndex) -> Vec<Option<u32>> {
        let mut numbering = index.live_numbering();
        for &id in &self.died {
            numbering[id as usize] = None;
        }
        numbering
    }

    /// Whether any entry died.
    pub(super) fn any_died(&self) -> bool {
        !self.died.is_empty()
    }

    /// The entries that died.
    pub(super) fn died(&self) -> &[u32] {
        &self.died
    }

    /// Whether no entry changed.
    pub(super) fn is_empty(&self) -> bool {
        self.scopes.is_empty()
    }

    /// Visit each element under `roots` in the flat tree that the changes restyle, where `records`
    /// holds what the elements matched against the index before and `index` is the updated one,
    /// with the entries that arrived it matches, where those and what left its matches are all
    /// that moved of them, and none where it has to be matched anew. An element where what moved
    /// of its matches decides nothing goes to `keep` instead, with the matches it has now.
    ///
    /// NB: No other entry's answer depends on the sheets: its selector and `@scope` levels are
    ///     its own. What an entry that died matched leaves the element's matches as it is renumbered.
    pub(super) fn for_each_restyled(
        &self,
        state: &RetainedState,
        index: &RuleIndex,
        records: &RetainedRecords,
        roots: &[StyleNodeID],
        mut visit: impl FnMut(StyleNodeID, Option<&[ElementMatch]>),
        mut keep: impl FnMut(StyleNodeID, MatchList),
    ) {
        let mut matcher = Matcher::reaching(index, &self.arrived, state);
        // NB: The arrived entries are few, and each is evaluated where an element reaches its
        //     bucket, without the ancestor filter of a traversal.
        let mut filter = AncestorFilter::new();
        filter.fill();
        let mut matches = Vec::new();
        let left_any = self.before.iter().any(|&changed| changed);
        // NB: Match lists are interned, and a run of alike elements holds one of them.
        let mut last_list: Option<(&MatchList, bool)> = None;
        // NB: Entries numbered anew leave no list of them to keep.
        let keeps_styles = self.conditions_moved_alone && !index.wants_compaction();
        // NB: Elements alike hold one list, and what arrives for them is alike too.
        let mut kept_lists: KeptLists<'_> = HashMap::default();
        // NB: An element sharing no bit with these reaches no bucket of the entries that arrived,
        //     and so matches none of them. Elements compare to selectors folded where ids and
        //     classes do, which the bits are not.
        let reaching_bits = match state.document_matching().fold_ids_and_classes {
            true => u64::MAX,
            false => self.arrived.bucket_key_bits(),
        };
        for &root in roots {
            for node in state.projection.style_preorder(root) {
                let Some(facts) = state.projection.facts(node) else {
                    continue;
                };
                // An element holding no record is under one that is not displayed, or arrived and
                // is styled for it.
                if records.get(node).is_none() {
                    continue;
                }
                let mut matched_one = false;
                if left_any {
                    matched_one = match (records.matches_of(node), last_list) {
                        (Some(matched), Some((list, answer))) if std::sync::Arc::ptr_eq(matched, list) => answer,
                        (Some(matched), _) => {
                            let answer = matched.iter().any(|&entry| self.before[entry as usize]);
                            last_list = Some((matched, answer));
                            answer
                        }
                        (None, _) => {
                            visit(node, None);
                            continue;
                        }
                    };
                }
                matches.clear();
                if self.arrived_entries > 0 && facts.dispatch_bloom(true) & reaching_bits != 0 {
                    // NB: An element whose match is not known is restyled, which finds out.
                    if matcher.match_element(node, facts, &filter, &mut matches).is_err() {
                        visit(node, None);
                        continue;
                    }
                }
                if !matched_one && matches.is_empty() {
                    continue;
                }
                if let Some(matched) = records.matches_of(node).filter(|_| keeps_styles) {
                    let arrived: SmallVec<[u32; 4]> = matches.iter().map(|arrived| arrived.entry).collect();
                    let list = match kept_lists.entry((&matched[..], arrived)) {
                        Entry::Occupied(found) => found.into_mut(),
                        Entry::Vacant(vacant) => {
                            let list = self.matches_keeping_style(state, index, matched, &vacant.key().1);
                            vacant.insert(list)
                        }
                    };
                    if let Some(list) = list {
                        keep(node, list.clone());
                        continue;
                    }
                }
                visit(node, Some(&matches));
            }
        }
        for regrouped in &self.regrouped {
            Self::for_each_regrouped(regrouped, state, index, records, &filter, &mut |node| visit(node, None));
        }
    }

    /// The entries an element holding the record of `matched` matches, where those that died left
    /// it and `arrived` joined it, if its style stays as it is for all of them.
    fn matches_keeping_style(
        &self,
        state: &RetainedState,
        index: &RuleIndex,
        matched: &[u32],
        arrived: &[u32],
    ) -> Option<MatchList> {
        let stayed: SmallVec<[u32; 16]> = matched
            .iter()
            .copied()
            .filter(|&entry| !self.before[entry as usize])
            .collect();
        let left = matched.iter().copied().filter(|&entry| self.before[entry as usize]);
        if !all_lose_every_longhand(state, index, &self.let_go, left.chain(arrived.iter().copied()), &stayed) {
            return None;
        }
        let mut now: Vec<u32> = stayed.iter().chain(arrived).copied().collect();
        now.sort_unstable();
        now.dedup();
        Some(std::sync::Arc::new(now.into_boxed_slice()))
    }

    /// Visit each element the shadow tree of `regrouped` reaches whose matches can have moved with
    /// the group its entries are of: one that matched an entry of the group it left, or that
    /// matches one of the group it joined.
    fn for_each_regrouped(
        regrouped: &Regrouped,
        state: &RetainedState,
        index: &RuleIndex,
        records: &RetainedRecords,
        filter: &AncestorFilter,
        visit: &mut impl FnMut(StyleNodeID),
    ) {
        let tree = &state.projection;
        // NB: A tree with no root, or a root with no host, holds no element the flat tree reaches.
        let Some(host) = state
            .scope_root(regrouped.scope)
            .and_then(|root| tree.host_of(root))
            .filter(|&host| tree.is_live(host))
        else {
            return;
        };
        let joined = regrouped
            .joined
            .map(|group| index.buckets_of(&index.live_entries_of(group)));
        let mut matcher = joined.as_ref().map(|joined| Matcher::reaching(index, joined, state));
        let mut matches = Vec::new();
        for node in tree.style_preorder(host) {
            let Some(facts) = tree.facts(node) else {
                continue;
            };
            if records.get(node).is_none() {
                continue;
            }
            let left_one = regrouped.left.is_some_and(|left| {
                records.matches_of(node).is_none_or(|matched| {
                    matched.iter().any(|&entry| {
                        index
                            .entries
                            .get(entry as usize)
                            .is_none_or(|_| index.entries.placement(entry as usize).group == left)
                    })
                })
            });
            if left_one {
                visit(node);
                continue;
            }
            let Some(matcher) = matcher.as_mut() else {
                continue;
            };
            matches.clear();
            if matcher
                .match_element(node, facts, filter, &mut matches)
                .map_or(true, |()| !matches.is_empty())
            {
                visit(node);
            }
        }
    }
}

/// How many matches and declarations a check of what moved of an element's matches reads at most. Past
/// that the check costs more than the cascade it would save, and the element is restyled.
const MOST_READ_FOR_KEEPING_A_STYLE: usize = 1 << 14;

/// Whether an element matching the entries of `stayed` cascades alike with `moved` and without them, as
/// [`loses_every_longhand`] says of each, as far as is worth reading to tell. The update of the rule
/// index let go of the segments `let_go`, if any.
pub(super) fn all_lose_every_longhand(
    state: &RetainedState,
    index: &RuleIndex,
    let_go: &[BoundSegment],
    moved: impl IntoIterator<Item = u32>,
    stayed: &[u32],
) -> bool {
    let mut reads_left = MOST_READ_FOR_KEEPING_A_STYLE;
    moved
        .into_iter()
        .all(|entry| loses_every_longhand(state, index, let_go, entry, stayed, &mut reads_left))
}

/// Whether an element matching the entries of `stayed` cascades alike with `entry` and without it:
/// each longhand it declares goes to an entry of `stayed` above it, and it declares nothing else.
/// Answers no once it has read all of `reads_left`.
///
/// NB: A `revert` or `revert-layer` that rolls back past the entry above, as declared or substituted,
///     rolls back past this one in the layer they share too, which then decides nothing either.
fn loses_every_longhand(
    state: &RetainedState,
    index: &RuleIndex,
    let_go: &[BoundSegment],
    entry: u32,
    stayed: &[u32],
    reads_left: &mut usize,
) -> bool {
    let entries = &index.entries;
    let Some(view) = entries.try_view_before(let_go, entry as usize) else {
        return false;
    };
    // NB: The entries that apply under container conditions leave what they read in the record, and
    //     may not apply. Those of a pseudo-element decide for it alone.
    let holds_by_its_rule =
        |view: super::rules::EntryView<'_>| !view.pseudo_element() && !view.scoped() && !view.gated();
    let placement = entries.placement_of(view);
    if !holds_by_its_rule(view)
        || view.declares_custom_properties()
        || view.declares_important()
        || placement.group != ScopeGroupID::DOCUMENT
    {
        return false;
    }
    let rule = index.rule_of_view(view);
    *reads_left = reads_left.saturating_sub(stayed.len());
    // NB: The entries of a rule decide by the greatest specificity among them.
    if stayed
        .iter()
        .any(|&other| index.rule_of_view(entries.view(other as usize)) == rule)
    {
        return false;
    }
    let declared = state.rules.declared_properties_of(rule);
    let priority = cascade_key(entries, view, Ranks::of_view(entries, view), false);
    !declared.is_empty()
        && declared.iter().all(|declaration| {
            (FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&declaration.property())
                // NB: What decides an element mostly comes late in the cascade.
                && stayed.iter().rev().any(|&other| {
                    if *reads_left == 0 {
                        return false;
                    }
                    *reads_left -= 1;
                    let other_view = entries.view(other as usize);
                    let other_placement = entries.placement_of(other_view);
                    if !holds_by_its_rule(other_view)
                        || other_placement.group != placement.group
                        || other_placement.origin != placement.origin
                        || other_placement.layer_rank != placement.layer_rank
                        || cascade_key(entries, other_view, Ranks::of_view(entries, other_view), false) <= priority
                    {
                        return false;
                    }
                    let above = state.rules.declared_properties_of(index.rule_of_view(other_view));
                    *reads_left = reads_left.saturating_sub(above.len());
                    above
                        .iter()
                        .any(|above| above.property() == declaration.property() && !above.important())
                })
        })
}

/// Where the declarations of `entry` of the given importance stand in the cascade order against
/// those of the other entries of its tree scope. Entries of different scopes reach an element
/// from different encapsulation contexts, which the tree orders, or from different origins.
fn cascade_key(
    entries: &super::rules::IndexEntries,
    entry: super::rules::EntryView<'_>,
    ranks: Ranks,
    important: bool,
) -> (ScopeGroupID, CascadePriority) {
    let placement = entries.placement_of(entry);
    (
        placement.group,
        CascadePriority::new(PriorityInputs {
            origin: placement.origin,
            important,
            context_depth: 0,
            element_attachment: ElementAttachment::Rule,
            layer_rank: ranks.layer,
            specificity: entry.specificity(),
            scope_proximity: u32::MAX,
            sheet_rank: ranks.sheet,
            rule_rank: ranks.rule,
        }),
    )
}

/// Whether each element of `keys` is in a longest run of them that never decreases.
fn longest_ordered_run<T: Ord>(keys: &[T]) -> Vec<bool> {
    // The index of the element ending the least-keyed run of each length, and the element before
    // each one in its run.
    let mut ends: Vec<usize> = Vec::new();
    let mut previous: Vec<Option<usize>> = Vec::with_capacity(keys.len());
    for (index, key) in keys.iter().enumerate() {
        let length = ends.partition_point(|&end| keys[end] <= *key);
        previous.push(length.checked_sub(1).map(|shorter| ends[shorter]));
        match ends.get_mut(length) {
            Some(end) => *end = index,
            None => ends.push(index),
        }
    }
    let mut kept = vec![false; keys.len()];
    for index in std::iter::successors(ends.last().copied(), |&index| previous[index]) {
        kept[index] = true;
    }
    kept
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_longest_ordered_run_keeps_the_most_elements_in_order() {
        assert_eq!(longest_ordered_run::<u32>(&[]), Vec::<bool>::new());
        assert_eq!(longest_ordered_run(&[1, 2, 3]), [true, true, true]);
        assert_eq!(longest_ordered_run(&[2, 1]).iter().filter(|&&kept| kept).count(), 1);
        assert_eq!(longest_ordered_run(&[1, 5, 2, 3, 4]), [true, false, true, true, true]);
        assert_eq!(longest_ordered_run(&[1, 1, 2]), [true, true, true]);
    }
}
