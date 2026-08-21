/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::fragment_ownership;
use crate::painting::paintable_arena::PaintableArena;
use crate::painting::paintable_data::{FfiSelectionEntry, PaintableSlotId, SELECTION_STATE_NONE};
use crate::painting::text_fragment;

#[derive(Clone, Copy, Debug)]
pub(crate) struct SelectionRange {
    pub start_offset: usize,
    pub end_offset: usize,
}

fn invalidate_block_and_ancestors(layout_arena: &LayoutNodeArena, paintables: &PaintableArena, block: PaintableSlotId) {
    let mut current = Some(block);
    while let Some(slot) = current {
        paintables.invalidate_paint_cache(layout_arena, slot);
        current = crate::painting::paint_order::paint_parent(layout_arena, paintables, slot);
    }
}

fn invalidate_self_painting_inline_box(layout_arena: &LayoutNodeArena, paintables: &PaintableArena, node: NodeSlotId) {
    if let Some(inline_box) = fragment_ownership::nearest_self_painting_inline_box(layout_arena, paintables, node) {
        paintables.invalidate_paint_cache(layout_arena, inline_box);
    }
}

fn reset_states(layout_arena: &LayoutNodeArena, paintables: &mut PaintableArena, viewport: PaintableSlotId) {
    let mut slots = Vec::new();
    crate::painting::paint_order::for_each_in_paint_subtree(layout_arena, paintables, viewport, |slot| {
        slots.push(slot);
    });
    for current in slots {
        if paintables.data_ref(current).selection_state != SELECTION_STATE_NONE {
            paintables.update_data(current, |data| data.selection_state = SELECTION_STATE_NONE);
            paintables.invalidate_paint_cache(layout_arena, current);
        }
        let mut changed_fragment_nodes: Vec<NodeSlotId> = Vec::new();
        for fragment in &mut paintables.side_mut(current).fragments {
            if fragment.selection_state != SELECTION_STATE_NONE {
                fragment.selection_state = SELECTION_STATE_NONE;
                changed_fragment_nodes.push(fragment.layout_node);
            }
        }
        if !changed_fragment_nodes.is_empty() {
            invalidate_block_and_ancestors(layout_arena, paintables, current);
            for node in changed_fragment_nodes {
                invalidate_self_painting_inline_box(layout_arena, paintables, node);
            }
        }
    }
}

pub(crate) fn apply(
    layout_arena: &LayoutNodeArena,
    paintables: &mut PaintableArena,
    viewport: PaintableSlotId,
    entries: &[FfiSelectionEntry],
    range: SelectionRange,
) {
    reset_states(layout_arena, paintables, viewport);
    for entry in entries {
        if entry.is_text_node_entry {
            let Some(block) =
                text_fragment::containing_block_paintable_of_node(layout_arena, paintables, entry.layout_node)
            else {
                continue;
            };
            let mut changed = false;
            for fragment in &mut paintables.side_mut(block).fragments {
                if fragment.layout_node == entry.layout_node && fragment.selection_state != entry.state {
                    fragment.selection_state = entry.state;
                    changed = true;
                }
            }
            if changed {
                invalidate_block_and_ancestors(layout_arena, paintables, block);
                invalidate_self_painting_inline_box(layout_arena, paintables, entry.layout_node);
            }
        } else {
            let slot = entry.paintable;
            if slot.is_invalid() || !paintables.is_live(slot) {
                continue;
            }
            if paintables.data_ref(slot).selection_state != entry.state {
                paintables.update_data(slot, |data| data.selection_state = entry.state);
                paintables.invalidate_paint_cache(layout_arena, slot);
            }
        }
    }
    paintables.selection = Some(range);
}

pub(crate) fn clear(layout_arena: &LayoutNodeArena, paintables: &mut PaintableArena, viewport: PaintableSlotId) {
    reset_states(layout_arena, paintables, viewport);
    paintables.selection = None;
}
