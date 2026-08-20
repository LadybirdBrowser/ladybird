/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paintable_arena::PaintableArena;
use crate::painting::paintable_data::{FragmentRecord, PaintableSlotId, SELECTION_STATE_START_AND_END};
use crate::painting::text_fragment::{self, CaretMatch};

pub(crate) struct CaretRectResult {
    pub rect: CssPixelRect,
    pub style_source: NodeSlotId,
    pub owner: PaintableSlotId,
    pub node: NodeSlotId,
}

fn caret_rect_in_fragment(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    owner: PaintableSlotId,
    fragment: &FragmentRecord,
    offset: usize,
) -> CaretRectResult {
    CaretRectResult {
        rect: text_fragment::range_rect(
            layout_arena,
            paintables,
            fragment,
            SELECTION_STATE_START_AND_END,
            offset,
            offset,
        ),
        style_source: text_fragment::style_source(layout_arena, fragment),
        owner,
        node: fragment.layout_node,
    }
}

pub(crate) fn caret_rect_for_position(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    node_slots: &[NodeSlotId],
    offset: usize,
    affinity_is_downstream: bool,
) -> Option<CaretRectResult> {
    let mut fallback: Option<(PaintableSlotId, u32)> = None;
    let mut direct: Option<(PaintableSlotId, u32)> = None;
    text_fragment::for_each_fragment_of_nodes(layout_arena, paintables, node_slots, |block, index, fragment| {
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
    let fragment = &paintables.side(block).fragments[index as usize];
    Some(caret_rect_in_fragment(
        layout_arena,
        paintables,
        block,
        fragment,
        offset,
    ))
}

pub(crate) fn caret_rect_in_dom_range(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    node_slots: &[NodeSlotId],
    offset: usize,
) -> Option<CssPixelRect> {
    let mut rect = None;
    text_fragment::for_each_fragment_of_nodes(layout_arena, paintables, node_slots, |_, _, fragment| {
        let dom_start = fragment.dom_start_offset_in_node;
        let dom_end = dom_start + fragment.length_in_code_units;
        if offset < dom_start || offset > dom_end {
            return true;
        }
        rect = Some(text_fragment::range_rect(
            layout_arena,
            paintables,
            fragment,
            SELECTION_STATE_START_AND_END,
            offset,
            offset,
        ));
        false
    });
    rect
}
