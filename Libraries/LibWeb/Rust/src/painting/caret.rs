/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paintable_data::{FragmentRecord, SELECTION_STATE_START_AND_END};
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::text_fragment::{self, CaretMatch};

pub(crate) struct CaretRectResult {
    pub rect: CssPixelRect,
    pub style_source: NodeSlotId,
    pub owner: NodeSlotId,
    pub node: NodeSlotId,
}

fn caret_rect_in_fragment(
    layout_arena: &impl PaintableRowsRead,
    owner: NodeSlotId,
    fragment: &FragmentRecord,
    offset: usize,
) -> CaretRectResult {
    CaretRectResult {
        rect: text_fragment::range_rect(layout_arena, fragment, SELECTION_STATE_START_AND_END, offset, offset),
        style_source: text_fragment::style_source(layout_arena, fragment),
        owner,
        node: fragment.layout_node,
    }
}

pub(crate) fn caret_rect_for_position(
    layout_arena: &impl PaintableRowsRead,
    node_slots: &[NodeSlotId],
    offset: usize,
    affinity_is_downstream: bool,
) -> Option<CaretRectResult> {
    let mut fallback: Option<(NodeSlotId, u32)> = None;
    let mut direct: Option<(NodeSlotId, u32)> = None;
    text_fragment::for_each_fragment_of_nodes(layout_arena, node_slots, |block, index, fragment| {
        match text_fragment::caret_match(fragment, offset, affinity_is_downstream) {
            CaretMatch::None => true,
            CaretMatch::SoftWrapFallback => {
                if fallback.is_none() {
                    fallback = Some((block, index));
                }
                true
            }
            CaretMatch::Direct => {
                direct = Some((block, index));
                false
            }
        }
    });
    let (block, index) = direct.or(fallback)?;
    let fragment = &layout_arena.paintable_side_data(block).fragments[index as usize];
    Some(caret_rect_in_fragment(layout_arena, block, fragment, offset))
}

pub(crate) fn caret_rect_in_dom_range(
    layout_arena: &impl PaintableRowsRead,
    node_slots: &[NodeSlotId],
    offset: usize,
) -> Option<CssPixelRect> {
    let mut rect = None;
    text_fragment::for_each_fragment_of_nodes(layout_arena, node_slots, |_, _, fragment| {
        let dom_start = fragment.dom_start_offset_in_node;
        let dom_end = fragment.dom_end_offset_in_node;
        if offset < dom_start || offset > dom_end {
            return true;
        }
        rect = Some(text_fragment::range_rect(
            layout_arena,
            fragment,
            SELECTION_STATE_START_AND_END,
            offset,
            offset,
        ));
        false
    });
    rect
}
