/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::fragment_ownership;
use crate::painting::paintable_arena::PaintableArena;
use crate::painting::paintable_data::PaintableSlotId;

pub(crate) fn paint_parent(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    slot: PaintableSlotId,
) -> Option<PaintableSlotId> {
    if !paintables.is_live(slot) || paintables.data_ref(slot).kind.forms_unconnected_subtree() {
        return None;
    }

    let mut ancestor = layout_arena.node_parent_if_live(paintables.data_ref(slot).layout_node);
    while let Some(node) = ancestor {
        let paintable = paintables.paintable_of_node(node);
        if !paintable.is_invalid() && paintables.is_live(paintable) {
            return Some(paintable);
        }
        if !fragment_ownership::node_is_fragmented_inline(layout_arena, node) {
            return None;
        }
        ancestor = layout_arena.node_parent_if_live(node);
    }
    None
}

fn first_paintable_in_layout_siblings(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    first_node: Option<NodeSlotId>,
) -> Option<PaintableSlotId> {
    let mut pending_siblings = Vec::new();
    let mut current = first_node;
    while let Some(node) = current {
        let next_sibling = layout_arena.node_next_sibling_if_live(node);
        let paintable = paintables.paintable_of_node(node);
        if !paintable.is_invalid() && paintables.is_live(paintable) {
            if !paintables.data_ref(paintable).kind.forms_unconnected_subtree() {
                return Some(paintable);
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

pub(crate) fn first_paint_child(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    slot: PaintableSlotId,
) -> Option<PaintableSlotId> {
    if !paintables.is_live(slot) {
        return None;
    }
    first_paintable_in_layout_siblings(
        layout_arena,
        paintables,
        layout_arena.node_first_child_if_live(paintables.data_ref(slot).layout_node),
    )
}

pub(crate) fn next_paint_sibling(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    slot: PaintableSlotId,
) -> Option<PaintableSlotId> {
    if !paintables.is_live(slot) || paintables.data_ref(slot).kind.forms_unconnected_subtree() {
        return None;
    }

    let mut node = paintables.data_ref(slot).layout_node;
    loop {
        if let Some(sibling) =
            first_paintable_in_layout_siblings(layout_arena, paintables, layout_arena.node_next_sibling_if_live(node))
        {
            return Some(sibling);
        }
        let parent = layout_arena.node_parent_if_live(node)?;
        let parent_paintable = paintables.paintable_of_node(parent);
        if !parent_paintable.is_invalid() || !fragment_ownership::node_is_fragmented_inline(layout_arena, parent) {
            return None;
        }
        node = parent;
    }
}

pub(crate) fn for_each_paint_child(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    slot: PaintableSlotId,
    mut callback: impl FnMut(PaintableSlotId),
) {
    let mut child = first_paint_child(layout_arena, paintables, slot);
    while let Some(current) = child {
        child = next_paint_sibling(layout_arena, paintables, current);
        callback(current);
    }
}

pub(crate) fn for_each_in_paint_subtree(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    root: PaintableSlotId,
    mut callback: impl FnMut(PaintableSlotId),
) {
    fn visit(
        layout_arena: &LayoutNodeArena,
        paintables: &PaintableArena,
        slot: PaintableSlotId,
        callback: &mut impl FnMut(PaintableSlotId),
    ) {
        callback(slot);
        for_each_paint_child(layout_arena, paintables, slot, |child| {
            visit(layout_arena, paintables, child, callback);
        });
    }

    if paintables.is_live(root) {
        visit(layout_arena, paintables, root, &mut callback);
    }
}
