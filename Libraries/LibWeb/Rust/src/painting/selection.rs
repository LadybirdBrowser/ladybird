/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::fragment_ownership;
use crate::painting::paintable_data::{FfiSelectionEntry, SELECTION_STATE_NONE};
use crate::painting::text_fragment;

#[derive(Clone, Copy, Debug)]
pub(crate) struct SelectionRange {
    pub start_offset: usize,
    pub end_offset: usize,
}

fn invalidate_block_and_ancestors(layout_arena: &LayoutNodeArena, block: NodeSlotId) {
    let mut current = Some(block);
    while let Some(slot) = current {
        layout_arena.invalidate_paint_cache(slot);
        current = crate::painting::paint_order::paint_parent(layout_arena, slot);
    }
}

fn invalidate_self_painting_inline_box(layout_arena: &LayoutNodeArena, node: NodeSlotId) {
    if let Some(inline_box) = fragment_ownership::nearest_self_painting_inline_box(layout_arena, node) {
        layout_arena.invalidate_paint_cache(inline_box);
    }
}

fn reset_states(layout_arena: &LayoutNodeArena, viewport: NodeSlotId) {
    let mut slots = Vec::new();
    crate::painting::paint_order::for_each_in_paint_subtree(layout_arena, viewport, |slot| {
        slots.push(slot);
    });
    for current in slots {
        if layout_arena.paintable_data(current).selection_state != SELECTION_STATE_NONE {
            layout_arena.update_paintable_data(current, |data| data.selection_state = SELECTION_STATE_NONE);
            layout_arena.invalidate_paint_cache(current);
        }
        let mut changed_fragment_nodes: Vec<NodeSlotId> = Vec::new();
        for fragment in &mut layout_arena.paintable_side_data_mut(current).fragments {
            if fragment.selection_state != SELECTION_STATE_NONE {
                fragment.selection_state = SELECTION_STATE_NONE;
                changed_fragment_nodes.push(fragment.layout_node);
            }
        }
        if !changed_fragment_nodes.is_empty() {
            invalidate_block_and_ancestors(layout_arena, current);
            for node in changed_fragment_nodes {
                invalidate_self_painting_inline_box(layout_arena, node);
            }
        }
    }
}

pub(crate) fn apply(layout_arena: &LayoutNodeArena, viewport: NodeSlotId, entries: &[FfiSelectionEntry]) {
    reset_states(layout_arena, viewport);
    for entry in entries {
        if entry.is_text_node_entry {
            let Some(block) = text_fragment::containing_block_paintable_of_node(layout_arena, entry.layout_node) else {
                continue;
            };
            let mut changed = false;
            for fragment in &mut layout_arena.paintable_side_data_mut(block).fragments {
                if fragment.layout_node == entry.layout_node && fragment.selection_state != entry.state {
                    fragment.selection_state = entry.state;
                    changed = true;
                }
            }
            if changed {
                invalidate_block_and_ancestors(layout_arena, block);
                invalidate_self_painting_inline_box(layout_arena, entry.layout_node);
            }
        } else {
            if !layout_arena.paintable_row_is_populated(entry.layout_node) {
                continue;
            }
            if layout_arena.paintable_data(entry.layout_node).selection_state != entry.state {
                layout_arena.update_paintable_data(entry.layout_node, |data| data.selection_state = entry.state);
                layout_arena.invalidate_paint_cache(entry.layout_node);
            }
        }
    }
}

pub(crate) fn clear(layout_arena: &LayoutNodeArena, viewport: NodeSlotId) {
    reset_states(layout_arena, viewport);
}
