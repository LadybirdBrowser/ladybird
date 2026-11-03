/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the engine did over the life of a document's render state, counted for tests to bound,
//! and what its last pass styled.
//!
//! A pass counts what it does in counts of its own, kept with the engine between the
//! transactions it restyles after, which the state takes into its counters once the pass kept the
//! records it computed. Unlike the live report, which a fork of the state starts anew, the counters
//! are the state's: a fork copies them as they are and counts its own passes in its copy, which
//! the state never sees, as the host never installs what a fork computes.

use super::super::tree::StyleNodeID;
use super::live::LiveField;
use super::projection::Projection;
use super::rules::RuleIndex;
use super::traversal::{Outcome, Statistics};

/// One of the counters of [`Counters`], in the order the host names them.
#[derive(Clone, Copy)]
#[repr(u32)]
pub(in super::super) enum StyleCounter {
    /// The passes of the engine: each restyle, of everything, of what transactions changed, or
    /// of the elements a pass before left waiting on fonts. A transaction that hints no element
    /// runs none.
    Passes,
    /// The passes that restyled every element: the one the engine starts from, and one after an
    /// input that can restyle any element, such as a stylesheet change.
    FullPasses,
    /// The elements passes styled: each once per pass that styled it.
    StyledElements,
    /// The elements passes styled differently than before in what their children compute from.
    ChangedElements,
    /// The elements whose hints asked a pass restyling what changed to restyle them, or their
    /// pseudo-elements, counted as the pass starts.
    HintedElements,
    /// The elements whose hints asked a pass restyling what changed to restyle their descendants,
    /// counted as the pass starts.
    HintedSubtrees,
    /// The elements the invalidation of transactions visited, following the selectors of the
    /// inputs they changed: each once per visit.
    InvalidationVisits,
    /// The compounds the invalidation of transactions tested against an element.
    InvalidationTests,
    /// The cold passes the engine started from: one each time it started.
    ColdPasses,
    /// The elements passes gave the record of an element they styled before with the same parent,
    /// the same inputs and the same revalidation answers, without matching them.
    SharedElements,
    /// The elements passes gave the record of an element that matched the same rules and declared
    /// the same declarations, under a parent they inherit the same from, which they matched but
    /// did not cascade or compute.
    ReusedElements,
    /// The rule indexes the engine built: one as it started, and one for each pass after a rule
    /// change.
    IndexBuilds,
    /// The entries of those indexes, summed.
    IndexedEntries,
    /// The selector entries those indexes, and the ones cold passes built, compiled: those of the
    /// programs no rule index build of the process compiled before.
    CompiledSelectors,
    /// The user agent indexes those indexes built, which no document of the process had built
    /// before: a document whose user agent rules decide as another's did shares its index.
    UserAgentIndexBuilds,
    /// The elements passes matched against the whole rule index, not only against the entries
    /// they were restyled for.
    MatchedElements,
    /// The walks of the argument of a `:has()` that passes took to answer it for an anchor.
    RelativeWalks,
}

impl StyleCounter {
    pub(in super::super) const COUNT: usize = Self::RelativeWalks as usize + 1;
}

/// What a pass counts until the state takes it, and what it styled.
#[derive(Clone, Default)]
pub(super) struct PassCounts {
    counts: [u64; StyleCounter::COUNT],
    last_pass: Option<LastPass>,
}

impl PassCounts {
    /// Note `value` of `field` of the live report, where it is one of the counters.
    pub(super) fn note(&mut self, field: LiveField, value: u64) {
        let counter = match field {
            LiveField::Passes => StyleCounter::Passes,
            LiveField::FullPasses => StyleCounter::FullPasses,
            LiveField::StyledElements => StyleCounter::StyledElements,
            LiveField::ChangedElements => StyleCounter::ChangedElements,
            LiveField::HintedElements => StyleCounter::HintedElements,
            LiveField::HintedSubtrees => StyleCounter::HintedSubtrees,
            LiveField::InvalidationVisits => StyleCounter::InvalidationVisits,
            LiveField::InvalidationTests => StyleCounter::InvalidationTests,
            _ => return,
        };
        self.counts[counter as usize] += value;
    }

    /// Note that the engine updated `index`, indexing `arrived` entries.
    pub(super) fn note_index_update(&mut self, index: &RuleIndex, arrived: usize) {
        self.counts[StyleCounter::IndexedEntries as usize] += arrived as u64;
        self.counts[StyleCounter::CompiledSelectors as usize] += index.compiled_selectors;
        self.counts[StyleCounter::UserAgentIndexBuilds as usize] += u64::from(index.built_user_agent_index);
    }

    /// Note that the engine built `index`.
    pub(super) fn note_index_build(&mut self, index: &RuleIndex) {
        self.counts[StyleCounter::IndexBuilds as usize] += 1;
        self.counts[StyleCounter::IndexedEntries as usize] += index.indexed_entries;
        self.counts[StyleCounter::CompiledSelectors as usize] += index.compiled_selectors;
        self.counts[StyleCounter::UserAgentIndexBuilds as usize] += u64::from(index.built_user_agent_index);
    }

    /// Note that a transaction took inputs, which the passes after it restyle: one that no pass
    /// follows styled nothing.
    pub(super) fn note_transaction(&mut self) {
        self.last_pass.get_or_insert_with(LastPass::default);
    }

    /// Note what a pass styled, everything where `full` is, and `changed`, the elements whose
    /// records, or pseudo-element records, are not the ones they held. A pass restyling what the
    /// one before it left waiting on fonts goes on with what that one styled.
    pub(super) fn note_pass(&mut self, full: bool, statistics: &Statistics, changed: &[StyleNodeID]) {
        self.counts[StyleCounter::SharedElements as usize] += statistics.outcomes.shared;
        self.counts[StyleCounter::ReusedElements as usize] += statistics.outcomes.reused;
        self.counts[StyleCounter::MatchedElements as usize] += statistics.outcomes.matched;
        self.counts[StyleCounter::RelativeWalks as usize] += statistics.match_counters.relative_walks;
        let last_pass = self.last_pass.get_or_insert_with(LastPass::default);
        last_pass.full |= full;
        last_pass.styled.extend_from_slice(&statistics.styled);
        last_pass.changed.extend_from_slice(changed);
    }
}

/// How many elements of a pass took the record of another, by how they found it.
#[derive(Clone, Copy, Default)]
pub(super) struct OutcomeCounts {
    /// The elements a pass reached.
    elements: u64,
    shared: u64,
    reused: u64,
    computed: u64,
    computed_directly: u64,
    /// The elements matched against the whole rule index.
    matched: u64,
}

impl OutcomeCounts {
    pub(super) fn note(&mut self, outcome: &Outcome) {
        self.elements += 1;
        match outcome {
            Outcome::Shared => self.shared += 1,
            Outcome::Reused => self.reused += 1,
            Outcome::Computed { directly } => {
                self.computed += 1;
                self.computed_directly += u64::from(*directly);
            }
            Outcome::CascadeDeclined | Outcome::ComputeDeclined => {}
        }
    }

    pub(super) fn elements(&self) -> u64 {
        self.elements
    }

    /// The elements the pass holds a record of.
    pub(super) fn styled(&self) -> u64 {
        self.shared + self.reused + self.computed
    }

    pub(super) fn shared(&self) -> u64 {
        self.shared
    }

    pub(super) fn reused(&self) -> u64 {
        self.reused
    }

    pub(super) fn computed_directly(&self) -> u64 {
        self.computed_directly
    }

    /// Note an element matched against the whole rule index.
    pub(super) fn note_matched(&mut self) {
        self.matched += 1;
    }

    pub(super) fn add(&mut self, other: &Self) {
        self.elements += other.elements;
        self.computed += other.computed;
        self.computed_directly += other.computed_directly;
        self.shared += other.shared;
        self.reused += other.reused;
        self.matched += other.matched;
    }
}

/// What the last pass of the engine styled, with the passes after it that restyled what it left
/// waiting on fonts.
#[derive(Clone, Default)]
struct LastPass {
    /// Whether it restyled every element.
    full: bool,
    /// The elements it styled.
    styled: Vec<StyleNodeID>,
    /// The elements it left holding records, or pseudo-element records, other than the ones they
    /// held before.
    changed: Vec<StyleNodeID>,
    /// Whether `styled` and `changed` are in tree order, without repeats or departed elements.
    in_tree_order: bool,
}

impl LastPass {
    /// Put the elements in the order a style traversal from `root` reaches them, leaving out the
    /// ones it does not reach any more.
    fn put_in_tree_order(&mut self, tree: &Projection, root: StyleNodeID) {
        if self.in_tree_order {
            return;
        }
        self.in_tree_order = true;
        if !tree.is_live(root) {
            self.styled.clear();
            self.changed.clear();
            return;
        }
        let order: std::collections::HashMap<StyleNodeID, usize> = tree
            .style_preorder(root)
            .enumerate()
            .map(|(position, node)| (node, position))
            .collect();
        for nodes in [&mut self.styled, &mut self.changed] {
            nodes.retain(|node| order.contains_key(node));
            nodes.sort_unstable_by_key(|node| order[node]);
            nodes.dedup();
        }
    }
}

/// What the engine did on a document's render state since the counters were last reset, what
/// its last pass styled, and how often the host composed animations over records.
#[derive(Clone, Default)]
pub(in super::super) struct Counters {
    counts: [u64; StyleCounter::COUNT],
    /// Shared with a fork until either side writes it: a fork restyles nothing until the host
    /// moves on under it.
    last_pass: crate::fork::ForkShared<LastPass>,
    /// The animation samples the host composed over the record an element installed, by whether
    /// composing them rebuilt every group of the record or only the ones they write. These are
    /// reset with the host's style invalidation counters, not with the others.
    animated_samples: [u64; 2],
}

impl Counters {
    /// Take what a pass counted, once it kept the records it computed.
    pub(super) fn take_pass(&mut self, pass: &mut PassCounts) {
        let PassCounts { counts, last_pass } = std::mem::take(pass);
        for (total, count) in self.counts.iter_mut().zip(counts) {
            *total += count;
        }
        if let Some(last_pass) = last_pass {
            self.last_pass.assign(last_pass);
        }
    }

    /// Note that the engine started from a cold pass, with `index`, the rule index it built.
    pub(super) fn note_live_start(&mut self, index: &RuleIndex) {
        self.counts[StyleCounter::ColdPasses as usize] += 1;
        self.counts[StyleCounter::IndexBuilds as usize] += 1;
        self.counts[StyleCounter::IndexedEntries as usize] += index.indexed_entries;
        self.counts[StyleCounter::CompiledSelectors as usize] += index.compiled_selectors;
        self.counts[StyleCounter::UserAgentIndexBuilds as usize] += u64::from(index.built_user_agent_index);
    }

    /// Note that a cold pass built `index`, which counts the selectors it compiled.
    pub(super) fn note_cold_pass_index(&mut self, index: &RuleIndex) {
        self.counts[StyleCounter::CompiledSelectors as usize] += index.compiled_selectors;
    }

    /// The counter of index `index` of [`StyleCounter`], or zero for none.
    pub(in super::super) fn get(&self, index: u32) -> u64 {
        self.counts.get(index as usize).copied().unwrap_or(0)
    }

    /// Zero the counters and forget the last pass.
    pub(in super::super) fn reset(&mut self) {
        self.counts = [0; StyleCounter::COUNT];
        self.last_pass.assign(LastPass::default());
    }

    /// Whether the last pass restyled every element.
    pub(in super::super) fn last_pass_is_full(&self) -> bool {
        self.last_pass.full
    }

    /// The elements the last pass styled, or the ones whose records it changed where `changed` is,
    /// in the order a style traversal from `root` reaches them.
    pub(in super::super) fn last_pass_elements(
        &mut self,
        tree: &Projection,
        root: StyleNodeID,
        changed: bool,
    ) -> &[StyleNodeID] {
        if !self.last_pass.in_tree_order {
            self.last_pass.put_in_tree_order(tree, root);
        }
        match changed {
            true => &self.last_pass.changed,
            false => &self.last_pass.styled,
        }
    }

    /// Note that the host composed an animation sample over the record an element installed,
    /// rebuilding every group of it where `rebuilt_every_group` is.
    pub(in super::super) fn note_animated_sample(&mut self, rebuilt_every_group: bool) {
        self.animated_samples[usize::from(rebuilt_every_group)] += 1;
    }

    /// How many animation samples the host composed, of those that rebuilt every group where
    /// `rebuilt_every_group` is, zeroing the count where `reset` is.
    pub(in super::super) fn animated_samples(&mut self, rebuilt_every_group: bool, reset: bool) -> u64 {
        let count = &mut self.animated_samples[usize::from(rebuilt_every_group)];
        let answer = *count;
        if reset {
            *count = 0;
        }
        answer
    }
}
