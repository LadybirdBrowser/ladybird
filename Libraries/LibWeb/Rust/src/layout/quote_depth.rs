/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::layout_node_arena::LayoutNodeArena;
use super::node_data::{
    GENERATED_FOR_AFTER, GENERATED_FOR_BACKDROP, GENERATED_FOR_BEFORE, GENERATED_FOR_MARKER, NodeKind, NodeSlotId,
};
use super::node_facts;
use crate::css::computed_value_views::QuoteTransfer;
use crate::css::style::bridge::element_adjustment_fact;
use crate::css::style::fast_hash::FastSet as HashSet;
use crate::css::style::tree::StyleNodeID;
use crate::painting::style_queries::has_style_containment;

#[derive(Clone, Copy, PartialEq, Eq)]
enum QuoteDepthStep {
    DomNodesFrom(StyleNodeID),
    AssignedNodesFrom(StyleNodeID, usize),
    TopLayerMembersFrom(usize),
    TopLayerMember(StyleNodeID),
    PseudoElement(StyleNodeID, u8),
    LeaveBox(StyleNodeID, u32),
}

pub(crate) struct QuoteDepthCursor {
    depth: u32,
    pending_steps: Vec<QuoteDepthStep>,
    document_has_top_layer: bool,
}

impl QuoteDepthCursor {
    pub(crate) fn new(arena: &LayoutNodeArena, document: StyleNodeID) -> Self {
        let mut pending_steps = vec![QuoteDepthStep::TopLayerMembersFrom(0)];
        if let Some(document_element) = arena.first_dom_child(document) {
            pending_steps.push(QuoteDepthStep::DomNodesFrom(document_element));
        }
        Self {
            depth: 0,
            pending_steps,
            document_has_top_layer: arena.top_layer_element(0).is_some(),
        }
    }

    pub(crate) fn depth_before(
        &mut self,
        arena: &LayoutNodeArena,
        element: StyleNodeID,
        generated_for: u8,
    ) -> Option<u32> {
        let target = QuoteDepthStep::PseudoElement(element, generated_for);
        while let Some(&step) = self.pending_steps.last() {
            if step == target {
                return Some(self.depth);
            }
            self.pending_steps.pop();
            self.take_step(arena, step);
        }
        None
    }

    fn take_step(&mut self, arena: &LayoutNodeArena, step: QuoteDepthStep) {
        match step {
            QuoteDepthStep::DomNodesFrom(node) => {
                let mut node = node;
                while node.text_index().is_some() {
                    let Some(next) = arena.next_dom_sibling(node) else {
                        return;
                    };
                    node = next;
                }
                if let Some(next) = arena.next_dom_sibling(node) {
                    self.pending_steps.push(QuoteDepthStep::DomNodesFrom(next));
                }
                self.enter_element_in_place(arena, node);
            }
            QuoteDepthStep::AssignedNodesFrom(slot, index) => {
                if index < arena.assigned_node_count(Some(slot)) {
                    self.pending_steps
                        .push(QuoteDepthStep::AssignedNodesFrom(slot, index + 1));
                    let node = arena.assigned_node_at(slot, index);
                    if node.text_index().is_none() {
                        self.enter_element_in_place(arena, node);
                    }
                }
            }
            QuoteDepthStep::TopLayerMembersFrom(index) => {
                let Some(member) = arena.top_layer_element(index) else {
                    return;
                };
                self.pending_steps.push(QuoteDepthStep::TopLayerMembersFrom(index + 1));
                if is_rendered_in_top_layer(arena, member) {
                    self.pending_steps.push(QuoteDepthStep::TopLayerMember(member));
                    self.pending_steps
                        .push(QuoteDepthStep::PseudoElement(member, GENERATED_FOR_BACKDROP));
                }
            }
            QuoteDepthStep::TopLayerMember(member) => self.enter_element(arena, member),
            QuoteDepthStep::PseudoElement(element, generated_for) => {
                self.depth = pseudo_element_quote_transfer(arena, element, generated_for).apply(self.depth);
            }
            QuoteDepthStep::LeaveBox(element, depth_on_entry) => {
                if self.depth != depth_on_entry
                    && node_facts::node_style_view(arena.data(arena.bound_row(element)))
                        .is_some_and(has_style_containment)
                {
                    self.depth = depth_on_entry;
                }
            }
        }
    }

    fn enter_element_in_place(&mut self, arena: &LayoutNodeArena, element: StyleNodeID) {
        if !(self.document_has_top_layer && is_rendered_in_top_layer(arena, element)) {
            self.enter_element(arena, element);
        }
    }

    fn enter_element(&mut self, arena: &LayoutNodeArena, element: StyleNodeID) {
        let row = arena.bound_row(element);
        if !row.is_invalid() {
            self.pending_steps.push(QuoteDepthStep::LeaveBox(element, self.depth));
            self.pending_steps
                .push(QuoteDepthStep::PseudoElement(element, GENERATED_FOR_AFTER));
            if arena.data(row).kind.get() == NodeKind::ListItemBox {
                self.pending_steps
                    .push(QuoteDepthStep::PseudoElement(element, GENERATED_FOR_MARKER));
            }
        } else if arena
            .published_box_facts(Some(element))
            .is_some_and(|facts| facts.display.is_contents())
        {
            self.pending_steps
                .push(QuoteDepthStep::PseudoElement(element, GENERATED_FOR_AFTER));
        } else {
            return;
        }
        let assigned_node_count = arena.assigned_node_count(Some(element));
        if assigned_node_count != 0 {
            self.pending_steps.push(QuoteDepthStep::AssignedNodesFrom(element, 0));
        }
        let first_child = match arena.shadow_root_of(element) {
            Some(shadow_root) => arena.first_dom_child(shadow_root),
            None if assigned_node_count == 0 => arena.first_dom_child(element),
            None => None,
        };
        if let Some(first_child) = first_child {
            self.pending_steps.push(QuoteDepthStep::DomNodesFrom(first_child));
        }
        self.pending_steps
            .push(QuoteDepthStep::PseudoElement(element, GENERATED_FOR_BEFORE));
    }
}

fn is_rendered_in_top_layer(arena: &LayoutNodeArena, element: StyleNodeID) -> bool {
    arena.element_adjustment_facts(Some(element)) & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0
}

fn pseudo_element_quote_transfer(arena: &LayoutNodeArena, element: StyleNodeID, generated_for: u8) -> QuoteTransfer {
    let row = arena.bound_pseudo_element_row(element, generated_for);
    if row.is_invalid() {
        return QuoteTransfer::IDENTITY;
    }
    node_facts::node_style_view(arena.data(row)).map_or(QuoteTransfer::IDENTITY, |style| style.content_quote_transfer())
}

pub(crate) fn box_subtree_moves_quote_depth(arena: &LayoutNodeArena, root: NodeSlotId) -> bool {
    let mut generators = HashSet::default();
    let mut row = root;
    loop {
        let data = arena.data(row);
        if matches!(
            data.generated_for.get(),
            GENERATED_FOR_BEFORE | GENERATED_FOR_AFTER | GENERATED_FOR_MARKER
        ) && let Some(generator) = arena.node_style_node(row)
            && generators.insert(generator)
            && ![GENERATED_FOR_BEFORE, GENERATED_FOR_MARKER, GENERATED_FOR_AFTER]
                .into_iter()
                .fold(QuoteTransfer::IDENTITY, |transfer, generated_for| {
                    transfer.then(pseudo_element_quote_transfer(arena, generator, generated_for))
                })
                .is_identity()
        {
            return true;
        }
        let mut next = data.first_child.get();
        let mut current = row;
        while next.is_invalid() && current != root {
            next = arena.data(current).next_sibling.get();
            current = arena.data(current).parent.get();
        }
        if next.is_invalid() {
            return false;
        }
        row = next;
    }
}
