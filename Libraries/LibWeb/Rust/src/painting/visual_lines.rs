/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::paintable_arena::PaintableArena;
use crate::painting::paintable_data::PaintableSlotId;
use crate::painting::{paintable_geometry, text_fragment};

pub(crate) struct RecordVisualLine {
    pub start_offset: usize,
    pub end_offset: usize,
    pub end_offset_with_trailing_whitespace: usize,
    pub has_fragments: bool,
    pub owner: PaintableSlotId,
    pub line_index: u32,
}

pub(crate) fn text_preserves_newlines(layout_arena: &LayoutNodeArena, text_node: NodeSlotId) -> bool {
    let Some(parent) = layout_arena.node_parent_if_live(text_node) else {
        return false;
    };
    let Some(parent_style) = layout_arena.node_style_if_live(parent) else {
        return false;
    };
    matches!(
        parent_style.inherited_text().white_space_collapse,
        crate::css::css_enums::white_space_collapse::PRESERVE
            | crate::css::css_enums::white_space_collapse::PRESERVE_BREAKS
    )
}

fn for_each_empty_visual_line_position(
    layout_arena: &LayoutNodeArena,
    node_slots: &[NodeSlotId],
    mut callback: impl FnMut(usize) -> bool,
) {
    let mut segments: Vec<(usize, &[u16])> = Vec::new();
    for &node in node_slots {
        if let Some(content) = layout_arena.text_content(node) {
            segments.push((content.dom_start_offset, &content.text));
        }
    }
    let Some(&(last_start, last_text)) = segments.last() else {
        return;
    };
    let total_length = last_start + last_text.len();
    let code_unit_at = |offset: usize| -> Option<u16> {
        for &(start, text) in &segments {
            if offset >= start && offset < start + text.len() {
                return Some(text[offset - start]);
            }
        }
        None
    };
    for i in 0..total_length {
        if code_unit_at(i) != Some(u16::from(b'\n')) {
            continue;
        }
        let position = i + 1;
        if (position == total_length || code_unit_at(position) == Some(u16::from(b'\n'))) && !callback(position) {
            return;
        }
    }
}

fn has_empty_visual_line_positions(layout_arena: &LayoutNodeArena, node_slots: &[NodeSlotId]) -> bool {
    let mut found = false;
    for_each_empty_visual_line_position(layout_arena, node_slots, |_| {
        found = true;
        false
    });
    found
}

pub(crate) fn collect_visual_lines(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    node_slots: &[NodeSlotId],
) -> Vec<RecordVisualLine> {
    let mut lines: Vec<RecordVisualLine> = Vec::new();
    text_fragment::for_each_fragment_of_nodes(layout_arena, paintables, node_slots, |block, _, fragment| {
        let dom_start = fragment.dom_start_offset_in_node;
        let dom_end = dom_start + fragment.length_in_code_units;
        let dom_end_with_trailing_whitespace = dom_end + fragment.trailing_whitespace_length_in_code_units;
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
    if let Some(&primary) = node_slots.last()
        && text_preserves_newlines(layout_arena, primary)
    {
        for_each_empty_visual_line_position(layout_arena, node_slots, |position| {
            lines.push(RecordVisualLine {
                start_offset: position,
                end_offset: position,
                end_offset_with_trailing_whitespace: position,
                has_fragments: false,
                owner: PaintableSlotId::INVALID,
                line_index: 0,
            });
            true
        });
    }
    lines.sort_by_key(|line| line.start_offset);
    lines
}

pub(crate) struct EmptyLineCaretTarget {
    pub offset: usize,
    pub line_index: usize,
    pub rect: CssPixelRect,
}

pub(crate) fn empty_line_caret_targets(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    block: PaintableSlotId,
) -> Vec<EmptyLineCaretTarget> {
    let side = paintables.side(block);
    if side.fragments.is_empty() || side.lines.is_empty() {
        return Vec::new();
    }
    let text_layout_node = side.fragments[0].layout_node;
    if layout_arena.node_kind_if_live(text_layout_node) != Some(NodeKind::TextNode) {
        return Vec::new();
    }
    if !text_preserves_newlines(layout_arena, text_layout_node) {
        return Vec::new();
    }
    let block_node = paintables.data_ref(block).layout_node;
    let Some(block_style) = layout_arena.node_style_if_live(block_node) else {
        return Vec::new();
    };
    if block_style.writing_mode() != crate::css::css_enums::writing_mode::HORIZONTAL_TB {
        return Vec::new();
    }
    let node_slots = [text_layout_node];
    if !has_empty_visual_line_positions(layout_arena, &node_slots) {
        return Vec::new();
    }
    if side
        .fragments
        .iter()
        .any(|fragment| fragment.layout_node != text_layout_node)
    {
        return Vec::new();
    }

    let lines = collect_visual_lines(layout_arena, paintables, &node_slots);

    if lines.len() < side.lines.len() {
        return Vec::new();
    }
    for (i, line) in lines.iter().enumerate() {
        if i >= side.lines.len() {
            if line.has_fragments {
                return Vec::new();
            }
            continue;
        }
        if line.has_fragments != (side.lines[i].fragment_count != 0) {
            return Vec::new();
        }
        if line.has_fragments && line.line_index as usize != i {
            return Vec::new();
        }
    }

    let content_position = paintable_geometry::absolute_position(paintables, block);
    let content_width = paintables.data_ref(block).content_size.width;
    let mut targets = Vec::new();
    for (i, line) in lines.iter().enumerate() {
        if line.has_fragments {
            continue;
        }
        let rect = if i < side.lines.len() {
            CssPixelRect::from(side.lines[i].rect).translated_by(content_position)
        } else {
            let last = CssPixelRect::from(side.lines.last().unwrap().rect).translated_by(content_position);
            let steps = (i - (side.lines.len() - 1)) as i32;
            CssPixelRect {
                x: content_position.x,
                y: last.y + last.height + CssPixels::from_raw(last.height.raw_value() * (steps - 1)),
                width: content_width,
                height: last.height,
            }
        };
        targets.push(EmptyLineCaretTarget {
            offset: line.start_offset,
            line_index: i,
            rect,
        });
    }
    targets
}

fn fragments_of_line(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    owner_paintable: u32,
    line_index: u32,
    node_slots: &[NodeSlotId],
) -> Vec<(PaintableSlotId, u32)> {
    let mut fragments = Vec::new();
    text_fragment::for_each_fragment_of_nodes(layout_arena, paintables, node_slots, |block, index, fragment| {
        if block.index == owner_paintable && fragment.line_index == line_index {
            fragments.push((block, index));
        }
        true
    });
    fragments
}

pub(crate) fn caret_inline_coordinate(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    owner_paintable: u32,
    line_index: u32,
    node_slots: &[NodeSlotId],
    offset: usize,
) -> Option<CssPixels> {
    let fragments = fragments_of_line(layout_arena, paintables, owner_paintable, line_index, node_slots);
    let mut chosen = *fragments.first()?;
    for &(block, index) in &fragments {
        let fragment = &paintables.side(block).fragments[index as usize];
        let dom_start = fragment.dom_start_offset_in_node;
        let dom_end_with_trailing_whitespace =
            dom_start + fragment.length_in_code_units + fragment.trailing_whitespace_length_in_code_units;
        if offset >= dom_start && offset <= dom_end_with_trailing_whitespace {
            chosen = (block, index);
            break;
        }
        if dom_start <= offset {
            chosen = (block, index);
        }
    }
    let (block, index) = chosen;
    let fragment = &paintables.side(block).fragments[index as usize];
    let dom_start = fragment.dom_start_offset_in_node;
    let dom_end_with_trailing_whitespace =
        dom_start + fragment.length_in_code_units + fragment.trailing_whitespace_length_in_code_units;
    let clamped_offset = offset.clamp(dom_start, dom_end_with_trailing_whitespace);
    let rect = text_fragment::range_rect(
        layout_arena,
        paintables,
        fragment,
        crate::painting::paintable_data::SELECTION_STATE_START_AND_END,
        clamped_offset,
        clamped_offset,
    );
    Some(if text_fragment::fragment_is_horizontal(fragment) {
        rect.x
    } else {
        rect.y
    })
}

pub(crate) fn offset_closest_to_inline_coordinate(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    owner_paintable: u32,
    line_index: u32,
    node_slots: &[NodeSlotId],
    inline_coordinate: CssPixels,
) -> Option<usize> {
    let fragments = fragments_of_line(layout_arena, paintables, owner_paintable, line_index, node_slots);
    let mut chosen = *fragments.first()?;
    for &(block, index) in &fragments {
        let fragment = &paintables.side(block).fragments[index as usize];
        let rect = text_fragment::absolute_rect(layout_arena, paintables, fragment);
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
    let fragment = &paintables.side(block).fragments[index as usize];
    let rect = text_fragment::absolute_rect(layout_arena, paintables, fragment);
    let mut point = crate::css::css_pixels::CssPixelPoint {
        x: rect.x + CssPixels::from_raw(rect.width.raw_value() / 2),
        y: rect.y + CssPixels::from_raw(rect.height.raw_value() / 2),
    };
    if text_fragment::fragment_is_horizontal(fragment) {
        point.x = rect.x.max(inline_coordinate);
    } else {
        point.y = rect.y.max(inline_coordinate);
    }
    Some(text_fragment::index_in_node_for_point(
        layout_arena,
        paintables,
        fragment,
        point,
    ))
}
