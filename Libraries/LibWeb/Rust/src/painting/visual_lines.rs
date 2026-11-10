/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paint_read::PaintRead;
use crate::painting::text_fragment;

pub(crate) struct RecordVisualLine {
    pub start_offset: usize,
    pub end_offset: usize,
    pub end_offset_with_trailing_whitespace: usize,
    pub has_fragments: bool,
    pub owner: NodeSlotId,
    pub line_index: u32,
}

pub(crate) fn collect_visual_lines(layout_arena: &impl PaintRead, node_slots: &[NodeSlotId]) -> Vec<RecordVisualLine> {
    let mut lines: Vec<RecordVisualLine> = Vec::new();
    text_fragment::for_each_fragment_of_nodes(layout_arena, node_slots, |block, _, fragment| {
        let dom_start = fragment.dom_start_offset_in_node;
        let dom_end = fragment.dom_end_offset_in_node;
        let dom_end_with_trailing_whitespace = fragment.dom_end_offset_with_trailing_whitespace;
        if let Some(line) = lines.last_mut()
            && line.has_fragments
            && line.owner == block
            && line.line_index == fragment.line_index
        {
            line.start_offset = line.start_offset.min(dom_start);
            line.end_offset = line.end_offset.max(dom_end);
            line.end_offset_with_trailing_whitespace = line
                .end_offset_with_trailing_whitespace
                .max(dom_end_with_trailing_whitespace);
        } else {
            lines.push(RecordVisualLine {
                start_offset: dom_start,
                end_offset: dom_end,
                end_offset_with_trailing_whitespace: dom_end_with_trailing_whitespace,
                has_fragments: true,
                owner: block,
                line_index: fragment.line_index,
            });
        }
        true
    });
    // A preserved newline that ends a line with nothing on it gives the line no fragment.
    let mut blocks: Vec<NodeSlotId> = node_slots
        .iter()
        .filter_map(|&node| text_fragment::containing_block_paintable_of_node(layout_arena, node))
        .collect();
    blocks.dedup();
    for block in blocks {
        for (line_index, line) in layout_arena.committed_side_data(block).lines().iter().enumerate() {
            if let Some(position) = line.forced_break
                && line.fragment_count == 0
                && node_slots.contains(&position.node)
            {
                let offset = layout_arena.dom_offset_for_rendered_text_offset(
                    position.node,
                    position.rendered_offset as usize,
                    crate::layout::RenderedTextBoundary::Start,
                );
                lines.push(RecordVisualLine {
                    start_offset: offset,
                    end_offset: offset,
                    end_offset_with_trailing_whitespace: offset,
                    has_fragments: false,
                    owner: block,
                    line_index: line_index as u32,
                });
            }
        }
    }
    lines.sort_by_key(|line| line.start_offset);
    lines
}

fn fragments_of_line(
    layout_arena: &impl PaintRead,
    owner_paintable: u32,
    line_index: u32,
    node_slots: &[NodeSlotId],
) -> Vec<(NodeSlotId, u32)> {
    let Some(block) = node_slots
        .iter()
        .filter_map(|&node| text_fragment::containing_block_paintable_of_node(layout_arena, node))
        .find(|block| block.index == owner_paintable)
    else {
        return Vec::new();
    };
    let side = layout_arena.committed_side_data(block);
    let Some(line) = side.lines().get(line_index as usize) else {
        return Vec::new();
    };
    (line.first_fragment..line.first_fragment + line.fragment_count)
        .filter(|&index| node_slots.contains(&side.fragments()[index as usize].layout_node))
        .map(|index| (block, index))
        .collect()
}

pub(crate) fn caret_inline_coordinate(
    layout_arena: &impl PaintRead,
    owner_paintable: u32,
    line_index: u32,
    node_slots: &[NodeSlotId],
    offset: usize,
) -> Option<CssPixels> {
    let fragments = fragments_of_line(layout_arena, owner_paintable, line_index, node_slots);
    let mut chosen = *fragments.first()?;
    for &(block, index) in &fragments {
        let side = layout_arena.committed_side_data(block);
        let fragment = &side.fragments()[index as usize];
        let dom_start = fragment.dom_start_offset_in_node;
        let dom_end_with_trailing_whitespace = fragment.dom_end_offset_with_trailing_whitespace;
        if offset >= dom_start && offset <= dom_end_with_trailing_whitespace {
            chosen = (block, index);
            break;
        }
        if dom_start <= offset {
            chosen = (block, index);
        }
    }
    let (block, index) = chosen;
    let side = layout_arena.committed_side_data(block);
    let fragment = &side.fragments()[index as usize];
    let dom_start = fragment.dom_start_offset_in_node;
    let dom_end_with_trailing_whitespace = fragment.dom_end_offset_with_trailing_whitespace;
    let clamped_offset = offset.clamp(dom_start, dom_end_with_trailing_whitespace);
    let rect = text_fragment::range_rect(
        layout_arena,
        fragment,
        crate::painting::paintable_data::SELECTION_STATE_START_AND_END,
        clamped_offset,
        clamped_offset,
    );
    let block_position = crate::painting::paintable_geometry::absolute_line_root_position(
        layout_arena,
        block,
        fragment.line_index as usize,
    );
    let (coordinate, block_coordinate) = if text_fragment::fragment_is_horizontal(fragment) {
        (rect.x, block_position.x)
    } else {
        (rect.y, block_position.y)
    };
    // INTEROP: Chromium uses the pixel-rounded leading edge of a one-pixel caret centered on the text boundary
    //          for vertical navigation. Rounding after subtracting half a pixel floors the local text coordinate.
    Some((coordinate - block_coordinate).floor() + block_coordinate)
}

pub(crate) fn offset_closest_to_inline_coordinate(
    layout_arena: &impl PaintRead,
    owner_paintable: u32,
    line_index: u32,
    node_slots: &[NodeSlotId],
    inline_coordinate: CssPixels,
) -> Option<usize> {
    let fragments = fragments_of_line(layout_arena, owner_paintable, line_index, node_slots);
    let mut chosen = *fragments.first()?;
    for &(block, index) in &fragments {
        let side = layout_arena.committed_side_data(block);
        let fragment = &side.fragments()[index as usize];
        let rect = text_fragment::absolute_rect(layout_arena, fragment);
        let inline_start = if text_fragment::fragment_is_horizontal(fragment) {
            rect.x
        } else {
            rect.y
        };
        if inline_start <= inline_coordinate {
            chosen = (block, index);
        }
    }
    let (block, index) = chosen;
    let side = layout_arena.committed_side_data(block);
    let fragment = &side.fragments()[index as usize];
    let rect = text_fragment::absolute_rect(layout_arena, fragment);
    let mut point = crate::css::css_pixels::CssPixelPoint {
        x: rect.x + CssPixels::from_raw(rect.width.raw_value() / 2),
        y: rect.y + CssPixels::from_raw(rect.height.raw_value() / 2),
    };
    if text_fragment::fragment_is_horizontal(fragment) {
        point.x = rect.x.max(inline_coordinate);
    } else {
        point.y = rect.y.max(inline_coordinate);
    }
    Some(text_fragment::index_in_node_for_point(layout_arena, fragment, point))
}
