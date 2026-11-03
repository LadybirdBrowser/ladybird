/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Matching one element against the rule index: gather the bucket candidates its keys reach,
//! reject those whose required ancestor keys the filter lacks, and evaluate the rest.

use super::super::component_matching::EntrySelector;
use super::super::input_types::TransactionFactSide;
use super::super::selector_evaluation::StructuralRead;
use super::super::*;
use super::bloom::{AncestorFilter, SiblingFilter};
use super::extent_map::KeyHash;
use super::projection::{ElementFacts, Projection, ProjectionEvaluator};
use super::rules::{BucketSlots, BucketSource, Buckets, RuleIndex, ScopeGroupID, ScopeIndex};
use crate::css::selector::Specificity;
use smallvec::SmallVec;

/// One rule entry matching the element.
#[derive(Clone, Copy, PartialEq)]
pub(super) struct ElementMatch {
    /// The entry in the rule index.
    pub(super) entry: u32,
    pub(super) rule: RuleID,
    pub(super) specificity: Specificity,
    pub(super) scope_proximity: u32,
    /// The kind of pseudo-element the entry styles, or `ELEMENT_PSEUDO_KIND` for the element itself.
    pub(super) pseudo_kind: u8,
    /// Where the encapsulation context the entry's rule decides from stands among the element's,
    /// which its cascade weighs the match by.
    pub(super) context_depth: u32,
    /// The tree scope whose sheets the entry's rule decides from: the document for the document's
    /// sheets, and otherwise the shadow tree the entry reached the element from.
    pub(super) scope: TreeScopeID,
}

impl ElementMatch {
    pub(super) fn styles(&self, index: &RuleIndex, target: super::PseudoElementTarget) -> bool {
        self.pseudo_kind == target.kind
            && (target.kind != super::super::publication::pseudo_kind::HIGHLIGHT
                || index.entry_styles_custom_highlight(self.entry, target.highlight_name))
    }

    /// Whether the entry styles a pseudo-element.
    #[cfg(test)]
    pub(super) fn pseudo_element(&self) -> bool {
        self.pseudo_kind != super::rules::ELEMENT_PSEUDO_KIND
    }
}

/// Which of a scope's bucket families an element is matched against.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Family {
    Own,
    OwnRevalidation,
    Host,
    Slotted,
    Part,
}

impl ScopeIndex {
    fn family(&self, family: Family) -> &Buckets {
        match family {
            Family::Own => &self.own,
            Family::OwnRevalidation => &self.own_revalidation,
            Family::Host => &self.host,
            Family::Slotted => &self.slotted,
            Family::Part => &self.part,
        }
    }
}

/// How an element is reached by the entries of one bucket family of a scope.
#[derive(Clone, Copy)]
struct Reach<'a> {
    scope: TreeScopeID,
    family: Family,
    /// Where the matches of the entries of the scope's own sheets stand among the element's
    /// encapsulation contexts; the document's stand outermost.
    scope_depth: u32,
    /// The filter of the element's ancestors, for the entries of its own tree, whose ancestors it
    /// holds.
    filter: Option<&'a AncestorFilter>,
    /// The tree scope the element is exposed to as a part, for the rules of a `::part()` reaching it.
    part_exposure: Option<TreeScopeID>,
}

/// What a bucket's candidates are rejected or proven on of the element they are offered to.
#[derive(Clone, Copy)]
struct ElementKeys {
    /// The dispatch bloom bits of the keys the element carries.
    bloom: u64,
    /// Whether the element's tag is its own ASCII-lowercase form.
    tag_is_lowercase: bool,
}

/// The dispatch bloom bits of the element being matched, or every bit where its ids and classes
/// compare to the selectors' folded, which the bits the selectors carry are not.
fn element_bloom_of(facts: &ElementFacts, is_document_root: bool, folds_ids_and_classes: bool) -> u64 {
    match folds_ids_and_classes {
        true => u64::MAX,
        false => facts.dispatch_bloom(is_document_root),
    }
}

#[derive(Default)]
pub(super) struct MatchCounters {
    pub(super) candidates: u64,
    pub(super) bloom_rejections: u64,
    pub(super) evaluations: u64,
    pub(super) matches: u64,
    /// The walks of the argument of a `:has()` that answered it for an anchor.
    pub(super) relative_walks: u64,
}

impl MatchCounters {
    pub(super) fn add(&mut self, other: &Self) {
        self.candidates += other.candidates;
        self.bloom_rejections += other.bloom_rejections;
        self.evaluations += other.evaluations;
        self.matches += other.matches;
        self.relative_walks += other.relative_walks;
    }
}

pub(super) struct Matcher<'a> {
    index: &'a RuleIndex,
    /// The buckets elements reach entries of `index` by: its own, or those of a part of it.
    buckets: &'a RuleIndex,
    state: &'a RetainedState,
    evaluator: ProjectionEvaluator<'a>,
    projection: &'a Projection,
    /// The node stamp each entry was last offered under, so an entry in several buckets an element
    /// reaches is evaluated once.
    offered: Vec<u32>,
    stamp: u32,
    /// The filter of the element asked for last, which its candidates ask for one after another.
    last_earlier_siblings: Option<(StyleNodeID, SiblingFilter)>,

    pub(super) statistics: MatchCounters,
}

impl<'a> Matcher<'a> {
    pub(super) fn new(index: &'a RuleIndex, state: &'a RetainedState) -> Self {
        Self::reaching(index, index, state)
    }

    /// A matcher of the entries of `index` that the buckets of `buckets` hold alone.
    pub(super) fn reaching(index: &'a RuleIndex, buckets: &'a RuleIndex, state: &'a RetainedState) -> Self {
        let projection = &state.projection;
        Self {
            index,
            buckets,
            state,
            // NB: A matcher lives for one pass, over which the projection holds still, so its
            //     evaluator keeps the sibling positions and positional answers it finds for the
            //     elements after: a long sibling list costs one walk, not one per element.
            evaluator: projection.evaluator().with_owned_match_workspace(),
            projection,
            offered: vec![0; index.entries.len()],
            stamp: 0,
            last_earlier_siblings: None,

            statistics: MatchCounters::default(),
        }
    }

    /// What the matcher measured, with the walks its evaluator took.
    pub(super) fn counters(&self) -> MatchCounters {
        MatchCounters {
            relative_walks: self.evaluator.relative_walks(),
            ..self.statistics
        }
    }

    /// Append the element's matches to `out`, in index order.
    pub(super) fn match_element(
        &mut self,
        node: StyleNodeID,
        facts: &ElementFacts,
        filter: &AncestorFilter,
        out: &mut Vec<ElementMatch>,
    ) -> Result<(), Incomplete> {
        self.match_in(Family::Own, node, facts, filter, out)
    }

    /// Append the element's matches among the entries that revalidate to `out`, in index order.
    pub(super) fn match_revalidation_entries(
        &mut self,
        node: StyleNodeID,
        facts: &ElementFacts,
        filter: &AncestorFilter,
        out: &mut Vec<ElementMatch>,
    ) -> Result<(), Incomplete> {
        self.match_in(Family::OwnRevalidation, node, facts, filter, out)
    }

    /// The filter of the keys of the earlier siblings of `node`. The traversal styles siblings one
    /// after another, so this mostly adds the keys of the sibling before to the filter of the element
    /// asked for last; an element that does not follow it has its earlier siblings gathered again, up
    /// to a bound past which the filter holds every key.
    fn earlier_siblings(&mut self, node: StyleNodeID) -> SiblingFilter {
        if let Some((last, filter)) = self.last_earlier_siblings
            && last == node
        {
            return filter;
        }
        let previous = self.projection.previous_element_sibling(node);
        let projection = self.projection;
        let add_keys = |filter: &mut SiblingFilter, sibling| {
            if let Some(row) = projection.row_of(sibling) {
                for key in super::ancestor_keys_of(row) {
                    filter.insert(key);
                }
            }
        };
        let filter = match self.last_earlier_siblings {
            Some((last, filter)) if previous == Some(last) => {
                let mut filter = filter;
                add_keys(&mut filter, last);
                filter
            }
            _ => {
                const MAX_GATHERED_SIBLINGS: usize = 32;
                let mut filter = SiblingFilter::default();
                let mut sibling = previous;
                for _ in 0..MAX_GATHERED_SIBLINGS {
                    let Some(current) = sibling else {
                        break;
                    };
                    add_keys(&mut filter, current);
                    sibling = self.projection.previous_element_sibling(current);
                }
                if sibling.is_some() {
                    filter.fill();
                }
                filter
            }
        };
        self.last_earlier_siblings = Some((node, filter));
        filter
    }

    /// Whether `node`, with `facts`, matches the entry, outside the buckets and the ancestor filter.
    /// An entry whose rule does not decide in the element's own tree matches nothing.
    pub(super) fn matches_entry(&mut self, node: StyleNodeID, entry: u32) -> Result<bool, Incomplete> {
        // NB: The invalidation map of a segment goes on naming the entries that died for this index.
        if !self.index.entries.view(entry as usize).live() {
            return Ok(false);
        }
        let scope = self.projection.tree_scope(node);
        let placement = self.index.entries.placement(entry as usize);
        let applies = self.index.group_of(scope) == Some(placement.group)
            || (placement.group == ScopeGroupID::DOCUMENT
                && (self.state.rules.scope_uses_document_sheets(scope)
                    || matches!(placement.origin, CascadeOrigin::UserAgent | CascadeOrigin::User)));
        if !applies {
            return Ok(false);
        }
        // An element is rejected on the keys of its earlier siblings as a candidate of the buckets is.
        let selector = self.index.entry_selector(entry);
        if !self.index.entries.view(entry as usize).scoped()
            && let Some(key) = super::bloom::earlier_sibling_key(selector.selector)
            && !self.earlier_siblings(node).might_contain(key)
        {
            self.projection
                .note_structural_read(TransactionFactSide::After, node, StructuralRead::EarlierSiblings);
            return Ok(false);
        }
        self.enter_scope(scope, None);
        self.statistics.evaluations += 1;
        self.evaluator
            .matches_entry_selector(selector, node, self.state.document_matching())
    }

    /// The match of an unscoped entry of the own tree of `node`.
    pub(super) fn element_match(&self, node: StyleNodeID, entry: u32) -> ElementMatch {
        let indexed = self.index.entries.view(entry as usize);
        let scope = match self.index.entries.placement_of(indexed).group {
            ScopeGroupID::DOCUMENT => TreeScopeID::DOCUMENT,
            _ => self.projection.tree_scope(node),
        };
        ElementMatch {
            entry,
            rule: self.index.rule_of_view(indexed),
            specificity: indexed.specificity(),
            scope_proximity: u32::MAX,
            pseudo_kind: indexed.pseudo_kind(),
            context_depth: self.depth_of(scope),
            scope,
        }
    }

    /// How deeply `scope` is nested, which the matches of the rules of the trees an element is in
    /// stand at.
    fn depth_of(&self, scope: TreeScopeID) -> u32 {
        match scope {
            TreeScopeID::DOCUMENT => 0,
            _ => self.state.tree_scope_depth(scope),
        }
    }

    /// Evaluate the rules of `scope` from here on, for a part exposed to `part_exposure` if any.
    fn enter_scope(&mut self, scope: TreeScopeID, part_exposure: Option<TreeScopeID>) {
        let shadow_root = match scope {
            TreeScopeID::DOCUMENT => None,
            _ => self.state.scope_root(scope),
        };
        self.evaluator.set_scope_shadow_root(shadow_root);
        self.evaluator.set_part_exposure_scope(part_exposure);
    }

    /// Whether the rules of trees other than the element's own can match it: it hosts a shadow
    /// tree, is slotted, or is a part exposed outside its tree.
    pub(super) fn is_reached_from_other_trees(&self, node: StyleNodeID) -> bool {
        self.projection.is_reached_from_other_trees(node)
    }

    /// Append the element's matches among the entries of the trees other than its own that reach
    /// it to `out`: `:host` rules of its shadow tree, `::slotted()` rules of the trees it is slotted
    /// into, and `::part()` rules of the trees it is exposed to, or only the `::slotted()` rules
    /// for `revalidation`.
    ///
    /// https://drafts.csswg.org/css-scoping-1/#shadow-cascading
    fn match_other_trees(
        &mut self,
        node: StyleNodeID,
        facts: &ElementFacts,
        revalidation: bool,
        out: &mut Vec<ElementMatch>,
    ) -> Result<(), Incomplete> {
        if !self.is_reached_from_other_trees(node) {
            return Ok(());
        }
        let state = self.state;
        let scope = self.projection.tree_scope(node);
        let inner_scope = self
            .projection
            .shadow_root_of(node)
            .and_then(|shadow_root| state.scope_by_root.get(shadow_root))
            .filter(|&inner| inner != scope);
        if let Some(inner_scope) = inner_scope
            && !revalidation
        {
            let reach = Reach {
                scope: inner_scope,
                family: Family::Host,
                scope_depth: state.encapsulation_context(node, inner_scope),
                filter: None,
                part_exposure: None,
            };
            self.match_family(reach, node, facts, out)?;
        }
        let slotted_scopes: SmallVec<[TreeScopeID; 2]> = state
            .scopes_slotted_into(node)
            .filter(|&slotted| slotted != scope && Some(slotted) != inner_scope)
            .collect();
        for &slotted_scope in &slotted_scopes {
            let reach = Reach {
                scope: slotted_scope,
                family: Family::Slotted,
                scope_depth: self.depth_of(slotted_scope),
                filter: None,
                part_exposure: None,
            };
            self.match_family(reach, node, facts, out)?;
        }
        if revalidation {
            return Ok(());
        }
        let part_scopes: SmallVec<[TreeScopeID; 2]> = state
            .part_exposure_scopes(node)
            .filter(|&exposed| exposed != scope && Some(exposed) != inner_scope && !slotted_scopes.contains(&exposed))
            .collect();
        for part_scope in part_scopes {
            let reach = Reach {
                scope: part_scope,
                family: Family::Part,
                scope_depth: self.depth_of(part_scope),
                filter: None,
                part_exposure: Some(part_scope),
            };
            self.match_family(reach, node, facts, out)?;
        }
        Ok(())
    }

    fn match_in(
        &mut self,
        family: Family,
        node: StyleNodeID,
        facts: &ElementFacts,
        filter: &AncestorFilter,
        out: &mut Vec<ElementMatch>,
    ) -> Result<(), Incomplete> {
        let start = out.len();
        let scope = self.projection.tree_scope(node);
        let reach = Reach {
            scope,
            family,
            scope_depth: self.depth_of(scope),
            filter: Some(filter),
            part_exposure: None,
        };
        let result = self
            .match_family(reach, node, facts, out)
            .and_then(|()| self.match_other_trees(node, facts, family == Family::OwnRevalidation, out));
        if result.is_err() {
            out.truncate(start);
        }
        result?;
        out[start..].sort_unstable_by_key(|matched| (matched.entry, matched.scope));
        // An entry of the document's sheets can reach an element from more than one tree. One of a
        // group of shadow trees decides from each tree of the group that reaches the element.
        let mut kept = start;
        for index in start..out.len() {
            if kept == start || (out[kept - 1].entry, out[kept - 1].scope) != (out[index].entry, out[index].scope) {
                out[kept] = out[index];
                kept += 1;
            }
        }
        out.truncate(kept);
        Ok(())
    }

    /// Append the element's matches among the entries of one bucket family of `scope` to `out`:
    /// those of the scope's own sheets, and of the document's sheets that decide in it.
    fn match_family(
        &mut self,
        reach: Reach<'_>,
        node: StyleNodeID,
        facts: &ElementFacts,
        out: &mut Vec<ElementMatch>,
    ) -> Result<(), Incomplete> {
        let (scope, family) = (reach.scope, reach.family);
        let index = self.buckets;
        // https://drafts.csswg.org/css-scoping-1/#shadow-cascading
        // NB: A shadow tree decides with the document's user agent and user sheets, and with its
        //     author sheets as well where it is built from the document's styles.
        let mut sources: SmallVec<[BucketSource<'_>; 8]> = SmallVec::new();
        match scope {
            TreeScopeID::DOCUMENT => sources.extend(index.bucket_sources(ScopeGroupID::DOCUMENT, false)),
            _ => {
                if let Some(group) = index.group_of(scope) {
                    sources.extend(index.bucket_sources(group, false));
                }
                // NB: A tree built from the document's styles stands in for what they were written for, whose host they do
                //     not name.
                let non_author = family == Family::Host || !self.state.rules.scope_uses_document_sheets(scope);
                sources.extend(index.bucket_sources(ScopeGroupID::DOCUMENT, non_author));
            }
        }
        sources.extend(index.user_agent_buckets());
        if sources.is_empty() {
            return Ok(());
        }
        // NB: An entry is offered once for each tree that reaches the element: the shadow trees of
        //     one group share their entries.
        self.stamp = self.stamp.wrapping_add(1);
        if self.stamp == 0 {
            self.offered.fill(0);
            self.stamp = 1;
        }
        self.enter_scope(scope, reach.part_exposure);
        let projection = self.projection;
        let is_document_root = projection.parent(node).is_none();
        let keys = ElementKeys {
            bloom: element_bloom_of(
                facts,
                is_document_root,
                self.state.document_matching().fold_ids_and_classes,
            ),
            // NB: A tag that is its own ASCII-lowercase form has no folding of its own.
            tag_is_lowercase: facts.folded_tag().is_none(),
        };
        // NB: Each source is asked for the same keys, which are read from the element and hashed once.
        let mut probes: SmallVec<[(DispatchKey, KeyHash); 16]> = SmallVec::new();
        projection.for_each_dispatch_probe(facts, is_document_root, |key, _| {
            probes.push((key, Buckets::hash_of(&key)));
        });
        for source in sources {
            let buckets = source.buckets.family(family);
            if buckets.is_empty() {
                continue;
            }
            for (key, hash) in &probes {
                if let Some(bucket) = buckets.get_hashed(key, *hash) {
                    self.offer_all(bucket, source, node, reach, keys, out)?;
                }
            }
        }
        Ok(())
    }

    /// Offer the entries of `bucket` to the element, as `reach` reaches it.
    fn offer_all(
        &mut self,
        bucket: BucketSlots<'_>,
        source: BucketSource<'_>,
        node: StyleNodeID,
        reach: Reach<'_>,
        keys: ElementKeys,
        out: &mut Vec<ElementMatch>,
    ) -> Result<(), Incomplete> {
        let Reach {
            scope,
            family: _,
            scope_depth,
            filter,
            part_exposure: _,
        } = reach;
        let dead_in = source.binding_with_dead_entries();
        for bucket_entry in bucket {
            self.statistics.candidates += 1;
            // The subject compound's ids and classes, which the element's bits tell it lacks, first, as
            // they reject most candidates a bucket of one of its keys holds.
            if bucket_entry.subject_bloom() & !keys.bloom != 0 {
                self.statistics.bloom_rejections += 1;
                continue;
            }
            if filter.is_some_and(|filter| !filter.might_contain_all(&bucket_entry.ancestor_hashes())) {
                self.statistics.bloom_rejections += 1;
                continue;
            }
            let entry = source.first + bucket_entry.entry();
            if dead_in.is_some_and(|bound| bound.is_dead(entry)) {
                continue;
            }
            if bucket_entry.in_other_buckets() {
                let offered = &mut self.offered[entry as usize];
                if *offered == self.stamp {
                    continue;
                }
                *offered = self.stamp;
            }
            // An element of the entry's own tree is rejected on the keys of its earlier siblings as on those of its
            // ancestors.
            if filter.is_some()
                && let Some(key) = bucket_entry.sibling_key()
                && !self.earlier_siblings(node).might_contain(key)
            {
                // NB: What the element's earlier siblings carry rejected it, which an element arriving among them
                //     can move.
                self.projection
                    .note_structural_read(TransactionFactSide::After, node, StructuralRead::EarlierSiblings);
                self.statistics.bloom_rejections += 1;
                continue;
            }
            // NB: An entry its bucket's key proves matches without its selector being read, and
            //     matches unscoped.
            let scope_proximity = match bucket_entry.covered(keys.tag_is_lowercase) {
                true => u32::MAX,
                false => {
                    let selector = match bucket_entry.scoped() {
                        true => self.index.entry_selector(entry),
                        false => EntrySelector::unscoped(source.space.resolve(bucket_entry.selector())),
                    };
                    self.statistics.evaluations += 1;
                    let matches = match bucket_entry.subject_covered(keys.tag_is_lowercase) {
                        true => {
                            self.evaluator
                                .matches_left_of_subject(selector, node, self.state.document_matching())?
                        }
                        false => {
                            self.evaluator
                                .matches_entry_selector(selector, node, self.state.document_matching())?
                        }
                    };
                    if !matches {
                        continue;
                    }
                    self.evaluator.scope_proximity_of_match(node)
                }
            };
            // NB: The entry is read once, in place from the segment holding it where the buckets are one's.
            let matched = source.matched(self.index, entry);
            let (group, rule) = (matched.group, matched.rule);
            // A document sheet the scope holds too decides from the scope.
            if self.index.group_of(scope) != Some(group)
                && self
                    .state
                    .rules
                    .attachment_in_scope(self.state.rules.rule_sheet(rule), scope)
                    .is_some()
            {
                continue;
            }
            self.statistics.matches += 1;
            out.push(ElementMatch {
                entry,
                rule,
                specificity: matched.specificity,
                scope_proximity,
                pseudo_kind: matched.pseudo_kind,
                context_depth: match group {
                    ScopeGroupID::DOCUMENT => 0,
                    _ => scope_depth,
                },
                scope: match group {
                    ScopeGroupID::DOCUMENT => TreeScopeID::DOCUMENT,
                    _ => scope,
                },
            });
        }
        Ok(())
    }
}

impl crate::css::style::RetainedState {
    /// Every scope a `::part()` rule can address this element from, nearest host first.
    ///
    /// One per level of `exportparts` forwarding: the tree each forwarding host stands in holds
    /// rules that can name the element under the name that host exposed it by. Duplicates are
    /// dropped because two levels can stand in one tree.
    pub(in crate::css::style) fn part_exposure_scopes(
        &self,
        node: StyleNodeID,
    ) -> impl Iterator<Item = TreeScopeID> + '_ {
        let mut seen: Vec<TreeScopeID> = Vec::new();
        self.projection
            .part_hosts_of(node)
            .iter()
            .map(|&(_, host)| self.projection.tree_scope(host))
            .filter(move |scope| match seen.contains(scope) {
                true => false,
                false => {
                    seen.push(*scope);
                    true
                }
            })
    }
}
