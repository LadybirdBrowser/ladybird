/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// One clock shared by every phase; rounded cumulative endpoints make the intervals additive.
/// It owns no engine borrow, and is read only for diagnostics.
struct TransactionClock {
    started_at: std::time::Instant,
    elapsed_microseconds: u64,
    phase: Counter,
}

impl TransactionClock {
    fn new() -> Self {
        Self {
            started_at: std::time::Instant::now(),
            elapsed_microseconds: 0,
            phase: Counter::CommitMicroseconds,
        }
    }

    fn enter(&mut self, phase: Counter, counters: &Counters) {
        let elapsed = self.started_at.elapsed().as_micros().min(u128::from(u64::MAX)) as u64;
        counters.add(self.phase, elapsed - self.elapsed_microseconds);
        self.elapsed_microseconds = elapsed;
        self.phase = phase;
    }

    fn finish(mut self, counters: &Counters) {
        self.enter(Counter::TransactionRemainderMicroseconds, counters);
        counters.add(Counter::TransactionMicroseconds, self.elapsed_microseconds);
    }
}

impl RetainedState {
    /// Whether `node` keeps its style below an element that is not displayed: an SVG element that
    /// content can reference, which an SVG resource reference paints, or an element with
    /// animations, which go on running.
    pub(super) fn keeps_style_in_display_none_subtree(&self, node: StyleNodeID) -> bool {
        self.projection.adjustment_facts(node) & bridge::element_adjustment_fact::HAS_ANIMATIONS != 0
            || self.is_referenceable_in_display_none_subtree(node)
    }

    pub(super) fn is_referenceable_in_display_none_subtree(&self, node: StyleNodeID) -> bool {
        use bridge::element_adjustment_fact::IS_SVG_ELEMENT;
        use bridge::element_construction_fact::{IS_SVG_GEOMETRY_ELEMENT, IS_SVG_RESOURCE_ELEMENT};
        if self.projection.construction_facts(node) & IS_SVG_GEOMETRY_ELEMENT != 0
            && self.projection.facts(node).is_some_and(|facts| !facts.id().is_none())
        {
            return true;
        }
        let mut current = Some(node);
        while let Some(element) = current
            && self.projection.adjustment_facts(element) & IS_SVG_ELEMENT != 0
        {
            if self.projection.construction_facts(element) & IS_SVG_RESOURCE_ELEMENT != 0 {
                return true;
            }
            current = self
                .projection
                .parent(element)
                .map(|parent| self.projection.host_of(parent).unwrap_or(parent));
        }
        false
    }
}

impl StyleEngine {
    fn run_style_update_with_clock(
        &mut self,
        root: StyleNodeID,
        mut emit: impl FnMut(StyleUpdateVersion, &[PublishedStyleDeltaRecord]),
        clock: &mut TransactionClock,
    ) -> bool {
        self.reclaim_unreachable_computed_records();
        for (node, input) in std::mem::take(&mut self.host.deferred_element_style_inputs) {
            self.host.element_inputs.note_style_input(
                node,
                ElementStyleInput {
                    reaction: input.reaction,
                    inherited_style_groups: input.inherited_style_groups,
                },
            );
        }
        self.host.deferred_element_style_inputs_are_pending = false;

        let mut transaction = self.drain_transaction();
        self.apply_staged_transaction(&mut transaction);
        if let Some(mut live) = self.retained.live_style.take() {
            live.note_transaction(&self.retained, &transaction);
            self.retained.live_style = Some(live);
        }
        let process_style = self
            .retained
            .atoms
            .is_process_global()
            .then(|| Arc::clone(&self.retained.process_style));
        self.retained
            .rules
            .share_rule_storage(process_style.as_deref().map(|style| &style.rule_indexes));
        // The engine owns the document from its first transaction with a document element on,
        // and takes the records it computes.
        self.own_from_the_start(root);
        let rows = self.install_restyled(root);
        self.release_transaction_and_sweep_atoms(transaction);
        if !rows.is_empty() {
            let transaction_version = self.retained.next_style_update_version;
            self.retained.next_style_update_version = StyleUpdateVersion(
                transaction_version
                    .0
                    .checked_add(1)
                    .expect("style update version space exhausted"),
            );
            emit(transaction_version, &rows);
        }
        clock.enter(Counter::TransactionRemainderMicroseconds, &self.retained.counters);
        true
    }
}

impl StyleEngine {
    pub fn run_style_update(
        &mut self,
        root: StyleNodeID,
        emit: impl FnMut(StyleUpdateVersion, &[PublishedStyleDeltaRecord]),
    ) -> bool {
        let mut clock = TransactionClock::new();
        let scoped = self.run_style_update_with_clock(root, emit, &mut clock);
        // Include transaction-local destruction on both ordinary and early-return paths.
        clock.finish(&self.retained.counters);
        scoped
    }
}
