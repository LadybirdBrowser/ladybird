/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::node_painting;
use crate::painting::paint_read::PaintRead;
use crate::painting::style_queries;
use std::cmp::Ordering;

pub(crate) fn order_for_painting(arena: &impl PaintRead, slot: NodeSlotId) -> i32 {
    // Absolutely positioned children of flex/grid containers have order: 0 for painting.
    // https://drafts.csswg.org/css-display-3/#order-modified-document-order
    if style_queries::is_flex_or_grid_item(arena, slot) {
        arena.node_style_if_live(slot).map_or(0, |style| style.order())
    } else {
        0
    }
}

impl LayoutNodeArena {
    pub(crate) fn compare_order_modified_document_order(
        &self,
        mut left: NodeSlotId,
        mut right: NodeSlotId,
    ) -> Ordering {
        let document_order = self.node_pre_order_label(left).cmp(&self.node_pre_order_label(right));
        // Hoisted positioned descendants follow the order of the items containing them.
        // Walk up the parent with the later pre-order label to find the siblings whose
        // subtrees contain the boxes. An ancestor stays before its descendants.
        while left != right {
            let left_parent = self.data(left).parent.get();
            let right_parent = self.data(right).parent.get();
            if left_parent == right_parent {
                return order_for_painting(self, left)
                    .cmp(&order_for_painting(self, right))
                    .then(document_order);
            }
            if left_parent.is_invalid() || right_parent.is_invalid() {
                break;
            }
            if self.node_pre_order_label(left_parent) > self.node_pre_order_label(right_parent) {
                left = left_parent;
            } else {
                right = right_parent;
            }
        }
        document_order
    }
}

pub(crate) fn paint_parent(layout_arena: &impl PaintRead, slot: NodeSlotId) -> Option<NodeSlotId> {
    if !layout_arena.paintable_row_is_populated(slot)
        || layout_arena
            .node_kind_if_live(slot)
            .is_some_and(crate::layout::node_facts::kind_is_svg_resource_box)
    {
        return None;
    }

    let mut ancestor = layout_arena.node_parent_if_live(slot);
    while let Some(node) = ancestor {
        if layout_arena.paintable_row_is_populated(node) {
            return Some(node);
        }
        if !node_painting::is_fragmented_inline(layout_arena, node) {
            return None;
        }
        ancestor = layout_arena.node_parent_if_live(node);
    }
    None
}

fn first_paintable_in_layout_siblings(
    layout_arena: &impl PaintRead,
    first_node: Option<NodeSlotId>,
) -> Option<NodeSlotId> {
    let mut pending_siblings = Vec::new();
    let mut current = first_node;
    while let Some(node) = current {
        let next_sibling = layout_arena.node_next_sibling_if_live(node);
        if layout_arena.paintable_row_is_populated(node) {
            // A table cell whose columns all have 'visibility: collapse' is removed from the display along with them,
            // descendants included: "This value causes the entire row or column to be removed from the display".
            // https://www.w3.org/TR/CSS22/tables.html#dynamic-effects
            if !layout_arena
                .node_kind_if_live(node)
                .is_some_and(crate::layout::node_facts::kind_is_svg_resource_box)
                && !crate::painting::paintable_geometry::committed_hidden_by_collapsed_columns(layout_arena, node)
            {
                return Some(node);
            }
        } else if node_painting::is_fragmented_inline(layout_arena, node)
            && let Some(first_child) = layout_arena.node_first_child_if_live(node)
        {
            if let Some(next_sibling) = next_sibling {
                pending_siblings.push(next_sibling);
            }
            current = Some(first_child);
            continue;
        }
        current = next_sibling.or_else(|| pending_siblings.pop());
    }
    None
}

pub(crate) fn first_paint_child(layout_arena: &impl PaintRead, slot: NodeSlotId) -> Option<NodeSlotId> {
    if !layout_arena.paintable_row_is_populated(slot) {
        return None;
    }
    first_paintable_in_layout_siblings(layout_arena, layout_arena.node_first_child_if_live(slot))
}

pub(crate) fn next_paint_sibling(layout_arena: &impl PaintRead, slot: NodeSlotId) -> Option<NodeSlotId> {
    if !layout_arena.paintable_row_is_populated(slot)
        || layout_arena
            .node_kind_if_live(slot)
            .is_some_and(crate::layout::node_facts::kind_is_svg_resource_box)
    {
        return None;
    }

    let mut node = slot;
    loop {
        if let Some(sibling) =
            first_paintable_in_layout_siblings(layout_arena, layout_arena.node_next_sibling_if_live(node))
        {
            return Some(sibling);
        }
        let parent = layout_arena.node_parent_if_live(node)?;
        if layout_arena.paintable_row_is_populated(parent) || !node_painting::is_fragmented_inline(layout_arena, parent)
        {
            return None;
        }
        node = parent;
    }
}

pub(crate) fn for_each_paint_child(
    layout_arena: &impl PaintRead,
    slot: NodeSlotId,
    mut callback: impl FnMut(NodeSlotId),
) {
    let mut child = first_paint_child(layout_arena, slot);
    while let Some(current) = child {
        child = next_paint_sibling(layout_arena, current);
        callback(current);
    }
}

pub(crate) fn for_each_in_paint_subtree(
    layout_arena: &impl PaintRead,
    root: NodeSlotId,
    mut callback: impl FnMut(NodeSlotId),
) {
    fn visit(layout_arena: &impl PaintRead, slot: NodeSlotId, callback: &mut impl FnMut(NodeSlotId)) {
        callback(slot);
        for_each_paint_child(layout_arena, slot, |child| {
            visit(layout_arena, child, callback);
        });
    }

    if layout_arena.paintable_row_is_populated(root) {
        visit(layout_arena, root, &mut callback);
    }
}
