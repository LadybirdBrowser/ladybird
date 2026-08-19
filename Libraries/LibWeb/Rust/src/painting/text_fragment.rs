/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paintable_arena::PaintableArena;
use crate::painting::paintable_data::FragmentRecord;
use crate::painting::paintable_data::PaintableSlotId;
use crate::painting::paintable_geometry;

pub(crate) fn containing_block_paintable(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    fragment: &FragmentRecord,
) -> Option<PaintableSlotId> {
    let own = paintables.paintable_of_node(fragment.layout_node);
    if !own.is_invalid() && paintables.is_live(own) {
        let block = paintables.data_ref(own).containing_block;
        return (!block.is_invalid() && paintables.is_live(block)).then_some(block);
    }
    let block = layout_arena.node_containing_block_if_live(fragment.layout_node)?;
    let block_paintable = paintables.paintable_of_node(block);
    (!block_paintable.is_invalid() && paintables.is_live(block_paintable)).then_some(block_paintable)
}

pub(crate) fn absolute_rect(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    fragment: &FragmentRecord,
) -> CssPixelRect {
    let mut rect = CssPixelRect::from_location_and_size(fragment.offset.into(), fragment.size.into());
    if let Some(block) = containing_block_paintable(layout_arena, paintables, fragment) {
        rect = rect.translated_by(paintable_geometry::absolute_position(paintables, block));
    }
    rect
}

pub(crate) fn absolute_line_box_rect(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    owner: PaintableSlotId,
    fragment: &FragmentRecord,
) -> CssPixelRect {
    let lines = &paintables.side(owner).lines;
    let Some(line) = lines.get(fragment.line_index as usize) else {
        return CssPixelRect::default();
    };
    let mut rect = CssPixelRect::from(line.rect);
    if let Some(block) = containing_block_paintable(layout_arena, paintables, fragment) {
        rect = rect.translated_by(paintable_geometry::absolute_position(paintables, block));
    }
    rect
}

fn is_horizontal(fragment: &FragmentRecord) -> bool {
    fragment.writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB
}

fn primary_size(rect: CssPixelRect, horizontal: bool) -> CssPixels {
    if horizontal { rect.width } else { rect.height }
}

fn secondary_size(rect: CssPixelRect, horizontal: bool) -> CssPixels {
    if horizontal { rect.height } else { rect.width }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SelectionOffsets {
    pub start: usize,
    pub end: usize,
}

pub(crate) fn selection_offsets_for_dom_range(
    fragment: &FragmentRecord,
    start_offset_in_code_units: usize,
    end_offset_in_code_units: usize,
) -> Option<SelectionOffsets> {
    let length_with_trailing_whitespace =
        fragment.length_in_code_units + fragment.trailing_whitespace_length_in_code_units;
    let dom_start = fragment.dom_start_offset_in_node;
    let dom_end = dom_start + length_with_trailing_whitespace;
    if dom_start > end_offset_in_code_units || dom_end < start_offset_in_code_units {
        return None;
    }
    Some(SelectionOffsets {
        start: start_offset_in_code_units - start_offset_in_code_units.min(dom_start),
        end: (end_offset_in_code_units - dom_start).min(length_with_trailing_whitespace),
    })
}

fn for_each_cluster_in_glyph_run(
    glyphs: &[crate::layout::FfiDrawGlyph],
    fragment_length_in_code_units: usize,
    mut callback: impl FnMut(usize, usize, f32),
) {
    let mut cursor = 0usize;
    for glyph in glyphs {
        if glyph.glyph_width == 0.0 {
            continue;
        }
        let cluster_start = cursor;
        let cluster_end = (cursor + glyph.length_in_code_units).min(fragment_length_in_code_units);
        cursor = cluster_end;
        if cluster_end <= cluster_start {
            continue;
        }
        callback(cluster_start, cluster_end, glyph.glyph_width);
    }
}

pub(crate) fn rect_for_selection_offsets(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    fragment: &FragmentRecord,
    offsets: SelectionOffsets,
    first_available_font: impl FnOnce() -> Option<*const std::ffi::c_void>,
) -> CssPixelRect {
    let horizontal = is_horizontal(fragment);
    let mut rect = absolute_rect(layout_arena, paintables, fragment);
    let length = fragment.length_in_code_units;

    let font_raw = match &fragment.glyph_run {
        Some(run) => Some(run.font.as_raw()),
        None => first_available_font(),
    };

    let start_in_text = offsets.start.min(length);
    let end_in_text = offsets.end.min(length);

    let mut pixel_offset;
    let mut pixel_width;
    if start_in_text == 0 && end_in_text == length && length > 0 {
        pixel_offset = CssPixels::from_raw(0);
        pixel_width = primary_size(rect, horizontal);
    } else {
        let mut offset_accumulator = 0.0f32;
        let mut width_accumulator = 0.0f32;
        if let Some(run) = &fragment.glyph_run {
            for_each_cluster_in_glyph_run(&run.glyphs, length, |cluster_start, cluster_end, cluster_width| {
                if cluster_end <= start_in_text {
                    offset_accumulator += cluster_width;
                    return;
                }
                if cluster_start >= end_in_text {
                    return;
                }
                let per_unit_advance = cluster_width / (cluster_end - cluster_start) as f32;
                if cluster_start < start_in_text {
                    offset_accumulator += per_unit_advance * (start_in_text - cluster_start) as f32;
                }
                let in_sel_start = cluster_start.max(start_in_text);
                let in_sel_end = cluster_end.min(end_in_text);
                width_accumulator += per_unit_advance * (in_sel_end - in_sel_start) as f32;
            });
        }
        pixel_offset = CssPixels::nearest_value_for_f32(offset_accumulator);
        pixel_width = CssPixels::nearest_value_for_f32(width_accumulator);
    }

    if offsets.start > length || offsets.end > length {
        let space_width = match font_raw {
            // SAFETY: The font is retained by the glyph run or by the node's style for the call.
            Some(raw) => CssPixels::nearest_value_for_f32(
                unsafe { libgfx_rust::font::FontRef::from_raw(raw) }.glyph_width(' ' as u32),
            ),
            None => CssPixels::from_raw(0),
        };
        let trailing_units_before_start = offsets.start.saturating_sub(length);
        let trailing_units_before_end = offsets.end.saturating_sub(length);
        pixel_offset += space_width * trailing_units_before_start;
        pixel_width += space_width * (trailing_units_before_end - trailing_units_before_start);
    }

    if offsets.start == offsets.end {
        pixel_width = CssPixels::from_integer(1);
    }

    if horizontal {
        rect.x += pixel_offset;
        rect.width = pixel_width;
    } else {
        rect.y += pixel_offset;
        rect.height = pixel_width;
    }

    if let Some(font_raw) = font_raw {
        // SAFETY: The font is retained by the glyph run or by the node's style for the duration
        // of this call.
        let font = unsafe { libgfx_rust::font::FontRef::from_raw(font_raw) };
        let (ascent_f, descent_f) = font.pixel_metrics_ascent_descent();
        if ascent_f > 0.0 || descent_f > 0.0 {
            let ascent = CssPixels::nearest_value_for_f32(ascent_f);
            let descent = CssPixels::nearest_value_for_f32(descent_f);
            let zero = CssPixels::from_raw(0);
            let overflow_top = (ascent - fragment.baseline).max(zero);
            let overflow_bottom = (descent - secondary_size(rect, horizontal) + fragment.baseline).max(zero);
            if horizontal {
                rect.inflate(overflow_top, zero, overflow_bottom, zero);
            } else {
                rect.inflate(zero, overflow_bottom, zero, overflow_top);
            }
        }
    }
    rect
}

pub(crate) fn whole_range_rect(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    fragment: &FragmentRecord,
    first_available_font: impl FnOnce() -> Option<*const std::ffi::c_void>,
) -> CssPixelRect {
    let offsets = SelectionOffsets {
        start: 0,
        end: fragment.length_in_code_units,
    };
    rect_for_selection_offsets(layout_arena, paintables, fragment, offsets, first_available_font)
}

pub(crate) fn is_block_level_box(layout_arena: &LayoutNodeArena, fragment: &FragmentRecord) -> bool {
    layout_arena
        .node_style_if_live(fragment.layout_node)
        .is_some_and(|style| style.display().is_block_outside())
}

pub(crate) fn style_source(layout_arena: &LayoutNodeArena, fragment: &FragmentRecord) -> NodeSlotId {
    if layout_arena.node_style_if_live(fragment.layout_node).is_some() {
        return fragment.layout_node;
    }
    layout_arena
        .node_parent_if_live(fragment.layout_node)
        .unwrap_or(NodeSlotId::INVALID)
}

pub(crate) fn first_available_font(
    layout_arena: &LayoutNodeArena,
    fragment: &FragmentRecord,
) -> Option<*const std::ffi::c_void> {
    let source = style_source(layout_arena, fragment);
    layout_arena
        .node_style_if_live(source)
        .map(|style| style.first_available_font())
}
