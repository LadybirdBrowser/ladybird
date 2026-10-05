/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

// The engine may move to whichever thread runs a style stage. Every host object it keeps a
// reference to says by its type whether it may be shared between threads, so one that cannot be
// fails this rather than passing as an opaque pointer.
const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<StyleEngine>();
};

impl StyleEngine {
    #[must_use]
    pub fn counters(&self) -> &Counters {
        &self.retained.counters
    }

    /// Mint `out.len()` element identities from the tree's own space, for a test.
    #[cfg(test)]
    pub fn allocate_style_nodes(&mut self, out: &mut [u32]) {
        for slot in out.iter_mut() {
            *slot = self.retained.tree.mint_test_identity(false).raw();
        }
        self.mint_style_nodes(out);
    }

    /// Mint `out.len()` text identities from the tree's own space, for a test.
    #[cfg(test)]
    pub fn allocate_text_style_nodes(&mut self, out: &mut [u32]) {
        for slot in out.iter_mut() {
            *slot = self.retained.tree.mint_test_identity(true).raw();
        }
        self.mint_style_nodes(out);
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn matches_for_cascade(
        &mut self,
        all: Vec<RuleMatch>,
        can_have_scope_duplicates: bool,
        publish_winners_for: Option<StyleNodeID>,
    ) -> Vec<RuleMatch> {
        self.matches_for_cascade_immediately(all, can_have_scope_duplicates, publish_winners_for)
    }

    /// Repair only the winner properties named by signed match changes.
    ///
    /// The retained exact answer supplies every contender, so deletion repair is a property-level
    /// upquery over rule identities rather than another selector match or whole-cascade reduction.
    #[inline]
    #[cfg(test)]
    pub(super) fn apply_cascade_winner_match_deltas(
        &mut self,
        node: StyleNodeID,
        matches: &[RuleMatch],
        deltas: &[SelectorTruthDelta],
        candidates: &mut Vec<OrderedCascadeCandidate>,
    ) -> bool {
        let mut effects = AnswerEffects::default();
        let result = self
            .retained
            .apply_cascade_winner_match_deltas(&mut effects, node, matches, deltas, candidates);
        self.install_answer_effects(effects);
        result
    }

    /// Leaves the atom sweep of the transactions taken until the next call to a later one, where `defers`.
    pub(crate) fn defer_atom_sweep(&mut self, defers: bool) {
        self.host.defers_atom_sweep = defers;
    }

    /// Apply a complete signed match delta directly to one retained exact answer.
    ///
    /// Entries with dynamic scope proximity still use the cold patch: their match row contains a
    /// value that the prefix match-set delta does not carry yet.
    #[inline]
    #[cfg(test)]
    pub(super) fn apply_retained_match_answer_deltas(
        &mut self,
        node: StyleNodeID,
        patch: &mut RetainedAnswerPatch,
        old_identity: MatchAnswerID,
        old_cascade_input: MatchAnswerID,
        deltas: &[SelectorTruthDelta],
    ) -> Option<RetainedAnswerPatchOutcome> {
        let mut effects = AnswerEffects::default();
        let result = self.retained.apply_retained_match_answer_deltas(
            &mut effects,
            node,
            patch,
            old_identity,
            old_cascade_input,
            deltas,
        );
        self.install_answer_effects(effects);
        result
    }

    /// Patch one retained exact answer by re-evaluating only the rules reached by this transaction.
    ///
    /// `None` means the optional old side was absent or exact evaluation could not complete. The
    /// caller then retires the answer and publishes an ordinary exact reaction.
    #[inline]
    #[cfg(test)]
    pub(super) fn patch_retained_match_answer(
        &mut self,
        node: StyleNodeID,
        patch: &mut RetainedAnswerPatch,
        truth_patch: SelectorTruthPatch<'_>,
    ) -> Option<RetainedAnswerPatchOutcome> {
        let mut effects = AnswerEffects::default();
        let result = self
            .retained
            .patch_retained_match_answer(&mut effects, node, patch, truth_patch);
        self.install_answer_effects(effects);
        result
    }

    /// Complete a node from a matching node's already compacted cascade input and winner rows.
    #[inline]
    #[cfg(test)]
    pub(super) fn complete_published_match_answer_from_cascade_input(
        &mut self,
        node: StyleNodeID,
        source: StyleNodeID,
        cascade_input: MatchAnswerID,
        cascade_winners_are_complete: bool,
    ) -> Option<PublishedMatchAnswer> {
        let mut effects = AnswerEffects::default();
        let result = self.retained.complete_published_match_answer_from_cascade_input(
            &mut effects,
            node,
            source,
            cascade_input,
            cascade_winners_are_complete,
        );
        self.install_answer_effects(effects);
        result
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn retained_closure_cascade_input(&self, node: StyleNodeID) -> Option<MatchAnswerID> {
        let empty = AnswerEffects::default();
        let traversal = self.retained.batch_matching_traversal.as_ref();
        let effects = traversal.map_or(&empty, |traversal| &traversal.answer_effects);
        self.retained.retained_closure_cascade_input(effects, node)
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn verify_retained_cascade_input(&mut self, node: StyleNodeID, cascade_input: MatchAnswerID) {
        let traversal = self.retained.batch_matching_traversal.take();
        let empty = AnswerEffects::default();
        let effects = traversal.as_ref().map_or(&empty, |traversal| &traversal.answer_effects);
        self.retained
            .verify_retained_cascade_input(effects, node, cascade_input);
        self.retained.batch_matching_traversal = traversal;
    }
}
