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

pub(crate) fn containing_block_paintable_of_node(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    node: NodeSlotId,
) -> Option<PaintableSlotId> {
    let own = paintables.paintable_of_node(node);
    if !own.is_invalid() && paintables.is_live(own) {
        let block = paintables.data_ref(own).containing_block;
        return (!block.is_invalid() && paintables.is_live(block)).then_some(block);
    }
    let block = layout_arena.node_containing_block_if_live(node)?;
    let block_paintable = paintables.paintable_of_node(block);
    (!block_paintable.is_invalid() && paintables.is_live(block_paintable)).then_some(block_paintable)
}

pub(crate) fn containing_block_paintable(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    fragment: &FragmentRecord,
) -> Option<PaintableSlotId> {
    containing_block_paintable_of_node(layout_arena, paintables, fragment.layout_node)
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

pub(crate) fn fragment_is_horizontal(fragment: &FragmentRecord) -> bool {
    is_horizontal(fragment)
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

pub(crate) fn range_rect(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    fragment: &FragmentRecord,
    selection_state: u8,
    start_offset_in_code_units: usize,
    end_offset_in_code_units: usize,
) -> CssPixelRect {
    match compute_selection_offsets(
        layout_arena,
        fragment,
        selection_state,
        start_offset_in_code_units,
        end_offset_in_code_units,
    ) {
        Some(offsets) => rect_for_selection_offsets(layout_arena, paintables, fragment, offsets, || {
            first_available_font(layout_arena, fragment)
        }),
        None => CssPixelRect::default(),
    }
}

pub(crate) fn compute_selection_offsets(
    layout_arena: &LayoutNodeArena,
    fragment: &FragmentRecord,
    selection_state: u8,
    start_offset_in_code_units: usize,
    end_offset_in_code_units: usize,
) -> Option<SelectionOffsets> {
    let length_with_trailing_whitespace =
        fragment.length_in_code_units + fragment.trailing_whitespace_length_in_code_units;
    let dom_start = fragment.dom_start_offset_in_node;
    let dom_end = fragment.dom_end_offset_with_trailing_whitespace;
    let rendered_offset_for_dom_offset = |dom_offset, boundary| {
        let rendered_offset =
            layout_arena.rendered_text_offset_for_dom_offset(fragment.layout_node, dom_offset, boundary);
        rendered_offset.clamp(
            fragment.start_offset,
            fragment.start_offset + length_with_trailing_whitespace,
        ) - fragment.start_offset
    };
    match selection_state {
        crate::painting::paintable_data::SELECTION_STATE_NONE => None,
        crate::painting::paintable_data::SELECTION_STATE_FULL => Some(SelectionOffsets {
            start: 0,
            end: length_with_trailing_whitespace,
        }),
        crate::painting::paintable_data::SELECTION_STATE_START_AND_END => selection_offsets_for_dom_range(
            layout_arena,
            fragment,
            start_offset_in_code_units,
            end_offset_in_code_units,
        ),
        crate::painting::paintable_data::SELECTION_STATE_START => {
            if dom_end < start_offset_in_code_units {
                return None;
            }
            Some(SelectionOffsets {
                start: rendered_offset_for_dom_offset(
                    start_offset_in_code_units,
                    crate::layout::RenderedTextBoundary::Start,
                ),
                end: length_with_trailing_whitespace,
            })
        }
        crate::painting::paintable_data::SELECTION_STATE_END => {
            if dom_start > end_offset_in_code_units {
                return None;
            }
            Some(SelectionOffsets {
                start: 0,
                end: rendered_offset_for_dom_offset(end_offset_in_code_units, crate::layout::RenderedTextBoundary::End),
            })
        }
        _ => unreachable!("invalid selection state"),
    }
}

pub(crate) fn for_each_fragment_of_nodes(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    node_slots: &[NodeSlotId],
    mut callback: impl FnMut(PaintableSlotId, u32, &FragmentRecord) -> bool,
) {
    for &node in node_slots {
        let Some(block) = containing_block_paintable_of_node(layout_arena, paintables, node) else {
            continue;
        };
        for (index, fragment) in paintables.side(block).fragments.iter().enumerate() {
            if fragment.layout_node != node {
                continue;
            }
            if !callback(block, index as u32, fragment) {
                return;
            }
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CaretMatch {
    None,
    Direct,
    SoftWrapFallback,
}

pub(crate) fn caret_match(fragment: &FragmentRecord, offset: usize, affinity_is_downstream: bool) -> CaretMatch {
    let dom_start = fragment.dom_start_offset_in_node;
    let dom_end_with_trailing_whitespace = fragment.dom_end_offset_with_trailing_whitespace;
    if offset < dom_start || offset > dom_end_with_trailing_whitespace {
        return CaretMatch::None;
    }
    if affinity_is_downstream && offset == dom_end_with_trailing_whitespace {
        return CaretMatch::SoftWrapFallback;
    }
    CaretMatch::Direct
}

pub(crate) fn selection_offsets_for_dom_range(
    layout_arena: &LayoutNodeArena,
    fragment: &FragmentRecord,
    start_offset_in_code_units: usize,
    end_offset_in_code_units: usize,
) -> Option<SelectionOffsets> {
    let length_with_trailing_whitespace =
        fragment.length_in_code_units + fragment.trailing_whitespace_length_in_code_units;
    let dom_start = fragment.dom_start_offset_in_node;
    let dom_end = fragment.dom_end_offset_with_trailing_whitespace;
    if dom_start > end_offset_in_code_units || dom_end < start_offset_in_code_units {
        return None;
    }
    let rendered_start = layout_arena.rendered_text_offset_for_dom_offset(
        fragment.layout_node,
        start_offset_in_code_units,
        crate::layout::RenderedTextBoundary::Start,
    );
    let rendered_end = layout_arena.rendered_text_offset_for_dom_offset(
        fragment.layout_node,
        end_offset_in_code_units,
        crate::layout::RenderedTextBoundary::End,
    );
    Some(SelectionOffsets {
        start: rendered_start.clamp(
            fragment.start_offset,
            fragment.start_offset + length_with_trailing_whitespace,
        ) - fragment.start_offset,
        end: rendered_end.clamp(
            fragment.start_offset,
            fragment.start_offset + length_with_trailing_whitespace,
        ) - fragment.start_offset,
    })
}

fn for_each_cluster_in_glyph_run(
    glyphs: &[crate::layout::FfiDrawGlyph],
    fragment_length_in_code_units: usize,
    mut callback: impl FnMut(usize, usize, f32) -> bool,
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
        if !callback(cluster_start, cluster_end, glyph.glyph_width) {
            return;
        }
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
                    return true;
                }
                if cluster_start >= end_in_text {
                    return true;
                }
                let per_unit_advance = cluster_width / (cluster_end - cluster_start) as f32;
                if cluster_start < start_in_text {
                    offset_accumulator += per_unit_advance * (start_in_text - cluster_start) as f32;
                }
                let in_sel_start = cluster_start.max(start_in_text);
                let in_sel_end = cluster_end.min(end_in_text);
                width_accumulator += per_unit_advance * (in_sel_end - in_sel_start) as f32;
                true
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

struct GraphemeEdgeTracker {
    target_width: f32,
    left_edge: usize,
    right_edge: usize,
    width_to_left_edge: f32,
    width_to_right_edge: f32,
}

impl GraphemeEdgeTracker {
    fn new(target_width: f32) -> Self {
        Self {
            target_width,
            left_edge: 0,
            right_edge: 0,
            width_to_left_edge: 0.0,
            width_to_right_edge: 0.0,
        }
    }

    fn update(&mut self, grapheme_length_in_code_units: usize, grapheme_width: f32) -> bool {
        if grapheme_width == 0.0 {
            return true;
        }
        self.right_edge += grapheme_length_in_code_units;
        self.width_to_right_edge += grapheme_width;
        if self.width_to_right_edge >= self.target_width {
            return false;
        }
        self.left_edge = self.right_edge;
        self.width_to_left_edge = self.width_to_right_edge;
        true
    }

    fn resolve(&self) -> usize {
        if (self.target_width - self.width_to_left_edge) < (self.width_to_right_edge - self.target_width) {
            return self.left_edge;
        }
        self.right_edge
    }
}

pub(crate) fn index_in_node_for_point(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    fragment: &FragmentRecord,
    position: crate::css::css_pixels::CssPixelPoint,
) -> usize {
    if !layout_arena
        .node_kind_if_live(fragment.layout_node)
        .is_some_and(crate::layout::kind_is_text)
    {
        return 0;
    }

    let rect = absolute_rect(layout_arena, paintables, fragment);
    let relative_inline_offset = if is_horizontal(fragment) {
        (position.x - rect.x).to_float()
    } else {
        (position.y - rect.y).to_float()
    };
    if relative_inline_offset < 0.0 {
        return 0;
    }

    let mut tracker = GraphemeEdgeTracker::new(relative_inline_offset);
    let mut reached_target = false;

    if let Some(run) = &fragment.glyph_run
        && let Some(content) = layout_arena.text_content(fragment.layout_node)
    {
        let segmenter = content.grapheme_segmenter();
        for_each_cluster_in_glyph_run(
            &run.glyphs,
            fragment.length_in_code_units,
            |cluster_start, cluster_end, cluster_width| {
                let per_unit_advance = cluster_width / (cluster_end - cluster_start) as f32;
                let mut grapheme_start = fragment.start_offset + cluster_start;
                let cluster_absolute_end = fragment.start_offset + cluster_end;
                while grapheme_start < cluster_absolute_end {
                    let grapheme_end = segmenter
                        .next_boundary(grapheme_start, false)
                        .unwrap_or(cluster_absolute_end)
                        .min(cluster_absolute_end);
                    if grapheme_end <= grapheme_start {
                        break;
                    }
                    let grapheme_units = grapheme_end - grapheme_start;
                    let grapheme_width = per_unit_advance * grapheme_units as f32;
                    if !tracker.update(grapheme_units, grapheme_width) {
                        reached_target = true;
                        return false;
                    }
                    grapheme_start = grapheme_end;
                }
                true
            },
        );
    }

    if !reached_target && fragment.trailing_whitespace_length_in_code_units > 0 {
        let font_raw = match &fragment.glyph_run {
            Some(run) => Some(run.font.as_raw()),
            None => first_available_font(layout_arena, fragment),
        };
        if let Some(raw) = font_raw {
            // SAFETY: The font is retained by the glyph run or by the node's style for the call.
            let space_width = unsafe { libgfx_rust::font::FontRef::from_raw(raw) }.glyph_width(' ' as u32);
            for _ in 0..fragment.trailing_whitespace_length_in_code_units {
                if !tracker.update(1, space_width) {
                    break;
                }
            }
        }
    }

    layout_arena.dom_offset_for_rendered_text_offset(
        fragment.layout_node,
        fragment.start_offset + tracker.resolve(),
        crate::layout::RenderedTextBoundary::End,
    )
}
