/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::painting::fragment_ownership;
use crate::painting::paintable_rows::PaintableRowsRead;

pub(crate) fn paint_parent(layout_arena: &impl PaintableRowsRead, slot: NodeSlotId) -> Option<NodeSlotId> {
    if !layout_arena.paintable_row_is_populated(slot)
        || layout_arena.paintable_data(slot).kind.forms_unconnected_subtree()
    {
        return None;
    }

    let mut ancestor = layout_arena.node_parent_if_live(slot);
    while let Some(node) = ancestor {
        if layout_arena.paintable_row_is_populated(node) {
            return Some(node);
        }
        if !fragment_ownership::node_is_fragmented_inline(layout_arena, node) {
            return None;
        }
        ancestor = layout_arena.node_parent_if_live(node);
    }
    None
}

fn first_paintable_in_layout_siblings(
    layout_arena: &impl PaintableRowsRead,
    first_node: Option<NodeSlotId>,
) -> Option<NodeSlotId> {
    let mut pending_siblings = Vec::new();
    let mut current = first_node;
    while let Some(node) = current {
        let next_sibling = layout_arena.node_next_sibling_if_live(node);
        if layout_arena.paintable_row_is_populated(node) {
            if !layout_arena.paintable_data(node).kind.forms_unconnected_subtree() {
                return Some(node);
            }
        } else if fragment_ownership::node_is_fragmented_inline(layout_arena, node)
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

pub(crate) fn first_paint_child(layout_arena: &impl PaintableRowsRead, slot: NodeSlotId) -> Option<NodeSlotId> {
    if !layout_arena.paintable_row_is_populated(slot) {
        return None;
    }
    first_paintable_in_layout_siblings(layout_arena, layout_arena.node_first_child_if_live(slot))
}

pub(crate) fn next_paint_sibling(layout_arena: &impl PaintableRowsRead, slot: NodeSlotId) -> Option<NodeSlotId> {
    if !layout_arena.paintable_row_is_populated(slot)
        || layout_arena.paintable_data(slot).kind.forms_unconnected_subtree()
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
        if layout_arena.paintable_row_is_populated(parent)
            || !fragment_ownership::node_is_fragmented_inline(layout_arena, parent)
        {
            return None;
        }
        node = parent;
    }
}

pub(crate) fn for_each_paint_child(
    layout_arena: &impl PaintableRowsRead,
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
    layout_arena: &impl PaintableRowsRead,
    root: NodeSlotId,
    mut callback: impl FnMut(NodeSlotId),
) {
    fn visit(layout_arena: &impl PaintableRowsRead, slot: NodeSlotId, callback: &mut impl FnMut(NodeSlotId)) {
        callback(slot);
        for_each_paint_child(layout_arena, slot, |child| {
            visit(layout_arena, child, callback);
        });
    }

    if layout_arena.paintable_row_is_populated(root) {
        visit(layout_arena, root, &mut callback);
    }
}
