/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Answering a read of one element's style, or one of its pseudo-elements', that the host has to
//! make before the next style update takes the document's pending changes.

use super::*;

/// A read the host makes before the next style update: of an element's style, or of one of its
/// pseudo-elements'.
#[derive(Clone, Copy, Debug)]
pub(crate) enum RecordDemand {
    Element(bridge::FfiRecordDemand),
    PseudoElement(
        bridge::FfiPseudoElementRecordDemand,
        bridge::FfiDemandedPseudoElement,
        StyleAtomID,
    ),
}

impl RecordDemand {
    /// Whether the demand leaves the engine as it was, its record only for the host to read.
    pub(crate) fn is_read_only(self) -> bool {
        use bridge::{FfiPseudoElementRecordDemand as Pseudo, FfiRecordDemand as Element};
        matches!(
            self,
            Self::Element(
                Element::ElementRead | Element::ElementReadWithoutInlineStyle | Element::ElementReadAgainstParent
            ) | Self::PseudoElement(Pseudo::ReadOnly, _, _)
        )
    }
}

/// What a record demand answers.
#[derive(Clone, Copy, Debug)]
pub(crate) enum RecordDemandAnswer {
    /// The element's record with the pseudo-element records settled beside it, or the
    /// pseudo-element's record.
    Record { record: DemandedEngineRecord },
    /// The pseudo-element asked for generates no box. Its rules still declare custom properties
    /// a read sees: the environment they resolve to, or zero where no rule styles it.
    Absent { custom_property_environment: u64 },
}

/// What a record demand would read that the next transaction has yet to take.
#[derive(Clone, Copy, Debug)]
pub(crate) enum PendingDemandInput {
    /// The node is not in the tree the engine styles.
    NodeIsNotLive,
    /// The links of an element were written.
    WrittenLinks,
    /// A rule or a sheet was edited.
    StagedRules,
    /// The rules of a sheet are being replaced.
    SheetRuleReplacement,
    /// The document's environment moved.
    Environment,
    /// The node's own row changed.
    RowChange,
    /// An element the node is below in the flat tree owes a computation, for each reason that holds.
    Ancestor {
        ancestor: StyleNodeID,
        owes_element_input: bool,
        owes_row_change: bool,
        owes_deferred_input: bool,
        deferred_inputs_are_pending: bool,
    },
}

impl std::fmt::Display for PendingDemandInput {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match *self {
            Self::NodeIsNotLive => formatter.write_str("a node that is not live"),
            Self::WrittenLinks => formatter.write_str("written links"),
            Self::StagedRules => formatter.write_str("staged rules"),
            Self::SheetRuleReplacement => formatter.write_str("a sheet rule replacement"),
            Self::Environment => formatter.write_str("an environment that moved"),
            Self::RowChange => formatter.write_str("a change of its own row"),
            Self::Ancestor {
                ancestor,
                owes_element_input,
                owes_row_change,
                owes_deferred_input,
                deferred_inputs_are_pending,
            } => write!(
                formatter,
                "the ancestor {ancestor:?}, which owes an element input: {owes_element_input}, a row change: \
                 {owes_row_change}, a deferred input: {owes_deferred_input} (pending: {deferred_inputs_are_pending})"
            ),
        }
    }
}

impl RetainedState {
    /// Keep a private answer's record alive for the host, which reads it once the demand has
    /// returned. The record the target's previous demand held is let go.
    pub(in crate::css::style) fn hold_demand_record(
        &mut self,
        target: computed::ComputedStyleTarget,
        record: computed::FinalStyleRecordID,
    ) {
        self.computed_group_sets.pin_style_record(record.raw());
        if let Some(previous) = self.demand_records.insert(target, record) {
            self.computed_group_sets.unpin_style_record(previous.raw());
        }
    }

    /// Whether everything a demand for `node` reads is current: the tree, the rules and the node's
    /// own facts hold no change the next transaction has yet to take, and no ancestor the node
    /// inherits from owes a computation. A read under pending input is the host's.
    pub(in crate::css::style) fn record_demand_reads_current_inputs(
        &self,
        node: StyleNodeID,
        host: &HostState,
    ) -> bool {
        self.pending_input_of_record_demand(node, host).is_none()
    }

    /// What a demand for `node` would read that the next transaction has yet to take, or none where
    /// everything it reads is current.
    pub(in crate::css::style) fn pending_input_of_record_demand(
        &self,
        node: StyleNodeID,
        host: &HostState,
    ) -> Option<PendingDemandInput> {
        if !self.tree.is_live(node) {
            return Some(PendingDemandInput::NodeIsNotLive);
        }
        if self.projection.has_written_links() {
            return Some(PendingDemandInput::WrittenLinks);
        }
        if host.program_staging.is_dirty() {
            return Some(PendingDemandInput::StagedRules);
        }
        if host.sheet_rule_replacement.is_some() {
            return Some(PendingDemandInput::SheetRuleReplacement);
        }
        if host.element_inputs.moves_environment() {
            return Some(PendingDemandInput::Environment);
        }
        if self.projection.has_pending_change(node) {
            return Some(PendingDemandInput::RowChange);
        }
        // The nodes owing input are gathered once, so a chain of demands reads the pending inputs
        // once per demand, not once per ancestor.
        let owing: HashSet<StyleNodeID> = host
            .element_inputs
            .nodes()
            .chain(self.projection.pending_change_nodes())
            .chain(host.deferred_element_style_inputs.keys().copied())
            .collect();
        if owing.is_empty() {
            return None;
        }
        let ancestor = std::iter::successors(self.projection.flat_tree_parent(node), |&parent| {
            self.projection.flat_tree_parent(parent)
        })
        .find(|ancestor| owing.contains(ancestor))?;
        Some(PendingDemandInput::Ancestor {
            ancestor,
            owes_element_input: host.element_inputs.nodes().any(|owing| owing == ancestor),
            owes_row_change: self.projection.has_pending_change(ancestor),
            owes_deferred_input: host.deferred_element_style_inputs.contains_key(&ancestor),
            deferred_inputs_are_pending: host.deferred_element_style_inputs_are_pending,
        })
    }
}

impl StyleEngine {
    /// Answer a read of `node`'s style, or of one of its pseudo-elements', that the host makes
    /// before the next style update, from the document as it is now and without taking the
    /// document's pending transaction. The node is matched again, so no winner row an earlier
    /// batch published answers inputs that moved since. A targeted demand drives the element's
    /// record in full against the parent as it is now; a pseudo-element is settled against the
    /// element's installed record.
    ///
    /// A read-only demand leaves the published rows, the node's records and its pending inputs as
    /// they were: it reuses the node's retained match answer or matches in a private traversal, and
    /// the record it answers is held only for the host to read. Any other demand publishes what it
    /// derives as the next style update would have: the host installs the record and acknowledges
    /// it, and the node's pending inputs are answered.
    pub(in crate::css::style) fn answer_record_demand(
        &mut self,
        node: StyleNodeID,
        demand: RecordDemand,
    ) -> RecordDemandAnswer {
        // The engine answers every read. A document it styles no records of yet, one without a document
        // element or whose initial style waits on its fonts, leaves the read to the host.
        if self.retained.live_style.is_none() {
            unreachable!("the style engine answers every row");
        }
        self.answer_live_record_demand(node, demand)
    }
}

impl RetainedState {}
