/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::painting::fragment_ownership;
use crate::painting::paintable_data::{FfiSelectionEntry, SELECTION_STATE_NONE};
use crate::painting::paintable_rows::PaintableRowsMut;
use crate::painting::text_fragment;

#[derive(Debug)]
pub(crate) struct SelectionRange {
    pub start_offset: usize,
    pub end_offset: usize,
    pub text_states: std::collections::HashMap<NodeSlotId, u8>,
}

fn invalidate_text_node(layout_arena: &PaintableRowsMut<'_>, node: NodeSlotId) {
    let mut current = text_fragment::containing_block_paintable_of_node(layout_arena, node);
    while let Some(slot) = current {
        layout_arena.invalidate_paint_cache(slot);
        current = crate::painting::paint_order::paint_parent(layout_arena, slot);
    }
    if let Some(inline_box) = fragment_ownership::nearest_self_painting_inline_box(layout_arena, node) {
        layout_arena.invalidate_paint_cache(inline_box);
    }
}

pub(crate) fn clear(layout_arena: &mut PaintableRowsMut<'_>, viewport: NodeSlotId) {
    let previous = layout_arena.paint_state().borrow_mut().selection.take();
    if let Some(previous) = previous {
        for node in previous.text_states.keys() {
            invalidate_text_node(layout_arena, *node);
        }
    }
    let mut slots = Vec::new();
    crate::painting::paint_order::for_each_in_paint_subtree(layout_arena, viewport, |slot| {
        slots.push(slot);
    });
    for current in slots {
        if layout_arena.paintable_data(current).selection_state != SELECTION_STATE_NONE {
            layout_arena.paintable_data_mut(current).selection_state = SELECTION_STATE_NONE;
            layout_arena.invalidate_paint_cache(current);
        }
    }
}

pub(crate) fn apply(
    layout_arena: &mut PaintableRowsMut<'_>,
    viewport: NodeSlotId,
    entries: &[FfiSelectionEntry],
) -> std::collections::HashMap<NodeSlotId, u8> {
    clear(layout_arena, viewport);
    let mut text_states = std::collections::HashMap::new();
    for entry in entries {
        if entry.is_text_node_entry {
            for &node in layout_arena.text_fragments(entry.layout_node).as_slice() {
                text_states.insert(node, entry.state);
                invalidate_text_node(layout_arena, node);
            }
        } else {
            if !layout_arena.paintable_row_is_populated(entry.layout_node) {
                continue;
            }
            if layout_arena.paintable_data(entry.layout_node).selection_state != entry.state {
                layout_arena.paintable_data_mut(entry.layout_node).selection_state = entry.state;
                layout_arena.invalidate_paint_cache(entry.layout_node);
            }
        }
    }
    text_states
}
