/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::layout::node_facts;
use crate::painting::paint_read::PaintRead;
use crate::painting::paintable_data::FragmentRecord;
use crate::painting::paintable_geometry;

pub(crate) fn containing_block_paintable_of_node(
    layout_arena: &impl PaintRead,
    node: NodeSlotId,
) -> Option<NodeSlotId> {
    if layout_arena.paintable_row_is_populated(node) {
        let block = layout_arena.paintable_data(node).containing_block;
        return layout_arena.paintable_row_is_populated(block).then_some(block);
    }
    let block = layout_arena.node_containing_block_if_live(node)?;
    layout_arena.paintable_row_is_populated(block).then_some(block)
}

pub(crate) fn containing_block_paintable(
    layout_arena: &impl PaintRead,
    fragment: &FragmentRecord,
) -> Option<NodeSlotId> {
    containing_block_paintable_of_node(layout_arena, fragment.layout_node)
}

pub(crate) fn absolute_rect(layout_arena: &impl PaintRead, fragment: &FragmentRecord) -> CssPixelRect {
    let rect = rect_in_containing_block(fragment);
    match containing_block_paintable(layout_arena, fragment) {
        Some(block) => rect.translated_by(paintable_geometry::absolute_line_root_position(
            layout_arena,
            block,
            fragment.line_index as usize,
        )),
        None => rect,
    }
}

/// The fragment's rect relative to its containing block, which is the box whose line boxes hold it.
pub(crate) fn rect_in_containing_block(fragment: &FragmentRecord) -> CssPixelRect {
    let (x, y) = fragment.offset();
    let (width, height) = fragment.size();
    CssPixelRect {
        x: x + fragment.relpos_delta.x,
        y: y + fragment.relpos_delta.y,
        width,
        height,
    }
}

pub(crate) fn absolute_line_box_rect(
    layout_arena: &impl PaintRead,
    owner: NodeSlotId,
    fragment: &FragmentRecord,
) -> CssPixelRect {
    let side = layout_arena.committed_side_data(owner);
    let lines = &side.lines();
    let Some(line) = lines.get(fragment.line_index as usize) else {
        return CssPixelRect::default();
    };
    let mut rect = CssPixelRect::from(line.rect);
    if let Some(block) = containing_block_paintable(layout_arena, fragment) {
        rect = rect.translated_by(paintable_geometry::absolute_line_root_position(
            layout_arena,
            block,
            fragment.line_index as usize,
        ));
    }
    rect
}

pub(crate) fn is_horizontal(fragment: &FragmentRecord) -> bool {
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
    layout_arena: &impl PaintRead,
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
        Some(offsets) => rect_for_selection_offsets(layout_arena, fragment, offsets, || {
            first_available_font(layout_arena, fragment)
        }),
        None => CssPixelRect::default(),
    }
}

pub(crate) fn compute_selection_offsets(
    layout_arena: &impl PaintRead,
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
        rendered_offset.clamp(fragment.start, fragment.start + length_with_trailing_whitespace) - fragment.start
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
    layout_arena: &impl PaintRead,
    node_slots: &[NodeSlotId],
    mut callback: impl FnMut(NodeSlotId, u32, &FragmentRecord) -> bool,
) {
    // OPTIMIZATION: Up to this many fragments, a scan costs less than building an index would.
    const MAX_FRAGMENTS_TO_SCAN: usize = 32;

    for &node in node_slots {
        let Some(block) = containing_block_paintable_of_node(layout_arena, node) else {
            continue;
        };
        let side_data = layout_arena.committed_side_data(block);
        let fragments = side_data.fragments();
        if fragments.len() <= MAX_FRAGMENTS_TO_SCAN {
            for (index, fragment) in fragments.iter().enumerate() {
                if fragment.layout_node == node && !callback(block, index as u32, fragment) {
                    return;
                }
            }
            continue;
        }
        for &index in side_data.fragment_indices_of(node) {
            if !callback(block, index, &fragments[index as usize]) {
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
    layout_arena: &impl PaintRead,
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
        start: rendered_start.clamp(fragment.start, fragment.start + length_with_trailing_whitespace) - fragment.start,
        end: rendered_end.clamp(fragment.start, fragment.start + length_with_trailing_whitespace) - fragment.start,
    })
}

/// Calls `callback` with each cluster of a fragment's glyph run, in glyph order: the code units it covers
/// (`cluster_start..cluster_end`) and its advance. Each glyph that covers code units starts a cluster, even one with no
/// advance (e.g. a soft hyphen's), and a glyph that covers none (another glyph of the same cluster) adds its advance to
/// the cluster before it. Stops early when `callback` returns false.
fn for_each_cluster_in_glyph_run(
    glyphs: &[libgfx_rust::text_layout::DrawGlyph],
    fragment_length_in_code_units: usize,
    mut callback: impl FnMut(usize, usize, f32) -> bool,
) {
    let mut cursor = 0usize;
    // The cluster whose glyphs are still being added up, and the advance of glyphs that came before any code unit.
    let mut pending: Option<(usize, usize, f32)> = None;
    let mut leading_advance = 0.0f32;
    for glyph in glyphs {
        if glyph.length_in_code_units() == 0 {
            match &mut pending {
                Some((_, _, width)) => *width += glyph.advance,
                None => leading_advance += glyph.advance,
            }
            continue;
        }
        let cluster_start = cursor;
        let cluster_end = (cursor + glyph.length_in_code_units()).min(fragment_length_in_code_units);
        cursor = cluster_end;
        if let Some((start, end, width)) = pending.take()
            && !callback(start, end, width)
        {
            return;
        }
        if cluster_end > cluster_start {
            pending = Some((
                cluster_start,
                cluster_end,
                glyph.advance + std::mem::take(&mut leading_advance),
            ));
        }
    }
    if let Some((start, end, width)) = pending {
        callback(start, end, width);
    }
}

fn run_has_rtl_glyphs(run: &crate::layout::inline_content::GlyphRunRecord) -> bool {
    run.glyphs.placed().any(|placed| placed.is_rtl)
}

/// The inline extents, relative to the fragment's start, of the glyphs that draw the code units `start..end`, in visual
/// order. Glyphs that are next to each other on the line share an extent; a new one starts only where a glyph of other
/// code units lies between them. A glyph that draws only part of the range contributes the part of its advance that
/// the range covers, measured from its right edge when it's right-to-left.
fn inline_extents_of_code_units(
    run: &crate::layout::inline_content::GlyphRunRecord,
    start: usize,
    end: usize,
) -> Vec<(f32, f32)> {
    // Every glyph of a cluster reports the cluster's start; the cluster's length comes from the glyph that covers it.
    let mut cluster_lengths = std::collections::HashMap::new();
    for placed in run.glyphs.placed() {
        if placed.glyph.length_in_code_units() > 0 {
            cluster_lengths.insert(placed.cluster_start as usize, placed.glyph.length_in_code_units());
        }
    }
    // Each glyph with an advance, as its left edge and the part of it the range covers, if any.
    let mut glyphs: Vec<(f32, Option<(f32, f32)>)> = Vec::new();
    for placed in run.glyphs.placed() {
        let cluster_start = placed.cluster_start as usize;
        let Some(&cluster_length) = cluster_lengths.get(&cluster_start) else {
            continue;
        };
        if placed.glyph.advance <= 0.0 {
            continue;
        }
        let left = placed.x;
        let covered_start = start.max(cluster_start);
        let covered_end = end.min(cluster_start + cluster_length);
        let covered = (covered_start < covered_end).then(|| {
            let mut from = (covered_start - cluster_start) as f32 / cluster_length as f32;
            let mut to = (covered_end - cluster_start) as f32 / cluster_length as f32;
            if placed.is_rtl {
                (from, to) = (1.0 - to, 1.0 - from);
            }
            (left + placed.glyph.advance * from, left + placed.glyph.advance * to)
        });
        glyphs.push((left, covered));
    }
    glyphs.sort_by(|a, b| a.0.total_cmp(&b.0));
    let mut extents: Vec<(f32, f32)> = Vec::new();
    let mut extending = false;
    for (_, covered) in glyphs {
        match covered {
            Some((left, right)) if extending => {
                let last = extents.last_mut().unwrap();
                last.0 = last.0.min(left);
                last.1 = last.1.max(right);
            }
            Some(extent) => {
                extents.push(extent);
                extending = true;
            }
            None => extending = false,
        }
    }
    extents
}

/// Calls `callback` with each DOM offset (in UTF-16 code units) that the horizontal `fragment` renders, from its first
/// offset through the one past its last, along with the document-absolute x coordinate where that offset's character
/// starts (or, for the last one, where the fragment's text ends). The advances come from the fragment's shaped glyph
/// run, so kerning and ligatures are reflected; a cluster's advance is split evenly among its code units.
pub(crate) fn for_each_dom_offset_x(
    layout_arena: &impl PaintRead,
    fragment: &FragmentRecord,
    mut callback: impl FnMut(usize, f32),
) {
    if !is_horizontal(fragment) {
        return;
    }
    let Some(run) = &fragment.glyph_run else {
        return;
    };
    // FIXME: Report the positions of right-to-left text. Its glyphs are in visual order, so an advance accumulated in
    //        glyph order doesn't belong to the character at the same logical offset.
    if run.glyphs.placed().any(|placed| placed.is_rtl) {
        return;
    }
    let Some(rendered_text) = layout_arena.rendered_text(fragment.layout_node) else {
        return;
    };
    let length = fragment.length_in_code_units;
    // advance_before[i] is the advance of the fragment's rendered text before its rendered offset i.
    let mut advance_before = vec![0.0f32; length + 1];
    let mut cursor = 0usize;
    for_each_cluster_in_glyph_run(&run.glyphs, length, |cluster_start, cluster_end, cluster_width| {
        let per_unit_advance = cluster_width / (cluster_end - cluster_start) as f32;
        for offset in cluster_start..cluster_end {
            advance_before[offset + 1] = advance_before[offset] + per_unit_advance;
        }
        cursor = cluster_end;
        true
    });
    // Code units past the last cluster take the end position.
    for offset in cursor..length {
        advance_before[offset + 1] = advance_before[offset];
    }
    let x = absolute_rect(layout_arena, fragment).x.to_float();
    rendered_text.for_each_rendered_text_offset_of_dom_offsets(
        fragment.dom_start_offset_in_node,
        fragment.dom_end_offset_in_node,
        crate::layout::RenderedTextBoundary::End,
        |dom_offset, rendered_offset| {
            let offset_in_fragment = rendered_offset.clamp(fragment.start, fragment.start + length) - fragment.start;
            callback(dom_offset, x + advance_before[offset_in_fragment]);
        },
    );
}

pub(crate) fn rect_for_selection_offsets(
    layout_arena: &impl PaintRead,
    fragment: &FragmentRecord,
    offsets: SelectionOffsets,
    first_available_font: impl FnOnce() -> Option<libgfx_rust::font::FontHandle>,
) -> CssPixelRect {
    let horizontal = is_horizontal(fragment);
    let mut rect = absolute_rect(layout_arena, fragment);
    let length = fragment.length_in_code_units;

    let font = match &fragment.glyph_run {
        Some(run) => Some(run.font.clone()),
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
        // Right-to-left glyphs are in visual order, so a range of code units is where their own glyphs are, not where
        // an advance added up in glyph order would put it.
        if let Some(run) = fragment
            .glyph_run
            .as_ref()
            .filter(|run| horizontal && run_has_rtl_glyphs(run))
        {
            let extents = inline_extents_of_code_units(run, start_in_text, end_in_text);
            if let (Some(first), Some(last)) = (extents.first(), extents.last()) {
                pixel_offset = CssPixels::nearest_value_for_f32(first.0);
                pixel_width = CssPixels::nearest_value_for_f32(last.1 - first.0);
            } else if start_in_text == end_in_text
                && let Some(x) = caret_inline_offset(fragment, start_in_text, true)
            {
                pixel_offset = CssPixels::nearest_value_for_f32(x);
            }
        }
    }

    if offsets.start > length || offsets.end > length {
        let space_width = match &font {
            Some(font) => CssPixels::nearest_value_for_f32(font.glyph_width(' ' as u32)),
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

    if let Some(font) = &font {
        let ascent_f = font.facts().ascent;
        let descent_f = font.facts().descent;
        if ascent_f > 0.0 || descent_f > 0.0 {
            // NB: Layout counts the font's ascent and descent in whole pixels, so its text baselines are too.
            let ascent = CssPixels::nearest_value_for_f32(ascent_f.round_ties_even());
            let descent = CssPixels::nearest_value_for_f32(descent_f.round_ties_even());
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

/// Like `rect_for_selection_offsets()`, but with one rect for each visually separate piece of the range: Code units
/// that are adjacent in the text can be apart on the line when the fragment mixes directions.
pub(crate) fn rects_for_selection_offsets(
    layout_arena: &impl PaintRead,
    fragment: &FragmentRecord,
    offsets: SelectionOffsets,
    first_available_font: impl FnOnce() -> Option<libgfx_rust::font::FontHandle>,
) -> Vec<CssPixelRect> {
    let rect = rect_for_selection_offsets(layout_arena, fragment, offsets, first_available_font);
    let length = fragment.length_in_code_units;
    let Some(run) = fragment
        .glyph_run
        .as_ref()
        .filter(|run| is_horizontal(fragment) && run_has_rtl_glyphs(run))
    else {
        return vec![rect];
    };
    let extents = inline_extents_of_code_units(run, offsets.start.min(length), offsets.end.min(length));
    if extents.len() < 2 {
        return vec![rect];
    }
    let x = absolute_rect(layout_arena, fragment).x;
    extents
        .into_iter()
        .map(|(left, right)| {
            let mut piece = rect;
            piece.x = x + CssPixels::nearest_value_for_f32(left);
            piece.width = CssPixels::nearest_value_for_f32(right - left);
            piece
        })
        .collect()
}

pub(crate) fn whole_range_rect(
    layout_arena: &impl PaintRead,
    fragment: &FragmentRecord,
    first_available_font: impl FnOnce() -> Option<libgfx_rust::font::FontHandle>,
) -> CssPixelRect {
    let offsets = SelectionOffsets {
        start: 0,
        end: fragment.length_in_code_units,
    };
    rect_for_selection_offsets(layout_arena, fragment, offsets, first_available_font)
}

pub(crate) fn is_block_level_box(layout_arena: &impl PaintRead, fragment: &FragmentRecord) -> bool {
    layout_arena
        .node_style_if_live(fragment.layout_node)
        .is_some_and(|style| style.display().is_block_outside())
}

pub(crate) fn style_source(_layout_arena: &impl PaintRead, fragment: &FragmentRecord) -> NodeSlotId {
    fragment.style_source
}

pub(crate) fn first_available_font(
    layout_arena: &impl PaintRead,
    fragment: &FragmentRecord,
) -> Option<libgfx_rust::font::FontHandle> {
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

pub(crate) fn physical_edge_offset(
    arena: &impl PaintRead,
    fragment: &FragmentRecord,
    left: bool,
) -> Option<(usize, bool)> {
    let run = fragment.glyph_run.as_ref()?;
    let placed = run
        .glyphs
        .placed()
        .filter(|placed| placed.glyph.length_in_code_units() > 0 && placed.glyph.advance > 0.0)
        .min_by(|a, b| {
            if left {
                a.x.total_cmp(&b.x)
            } else {
                (b.x + b.glyph.advance).total_cmp(&(a.x + a.glyph.advance))
            }
        })?;
    let boundary = placed.cluster_start as usize
        + if left == placed.is_rtl {
            placed.glyph.length_in_code_units()
        } else {
            0
        };
    Some((
        arena.dom_offset_for_rendered_text_offset(
            fragment.layout_node,
            fragment.start + boundary,
            crate::layout::RenderedTextBoundary::End,
        ),
        left == placed.is_rtl,
    ))
}

pub(crate) fn caret_inline_offset(fragment: &FragmentRecord, offset: usize, downstream: bool) -> Option<f32> {
    let run = fragment.glyph_run.as_ref()?;
    let mut fallback = None;
    for placed in run
        .glyphs
        .placed()
        .filter(|placed| placed.glyph.length_in_code_units() > 0)
    {
        let start = placed.cluster_start as usize;
        let length = placed.glyph.length_in_code_units();
        let end = start + length;
        if offset < start || offset > end {
            continue;
        }
        let fraction = (offset - start) as f32 / length as f32;
        let coordinate = placed.x + placed.glyph.advance * if placed.is_rtl { 1.0 - fraction } else { fraction };
        if (offset > start && offset < end) || (downstream && offset == start) || (!downstream && offset == end) {
            return Some(coordinate);
        }
        fallback = Some(coordinate);
    }
    fallback
}

pub(crate) fn index_in_node_for_point(
    layout_arena: &impl PaintRead,
    fragment: &FragmentRecord,
    position: crate::css::css_pixels::CssPixelPoint,
) -> usize {
    if !layout_arena
        .node_kind_if_live(fragment.layout_node)
        .is_some_and(node_facts::kind_is_text)
    {
        return 0;
    }

    let rect = absolute_rect(layout_arena, fragment);
    let relative_inline_offset = if is_horizontal(fragment) {
        (position.x - rect.x).to_float()
    } else {
        (position.y - rect.y).to_float()
    };
    if let Some(run) = fragment
        .glyph_run
        .as_ref()
        .filter(|run| is_horizontal(fragment) && run_has_rtl_glyphs(run))
        && let Some(offset) = rtl_offset_for_inline_position(layout_arena, fragment, run, relative_inline_offset)
    {
        return layout_arena.dom_offset_for_rendered_text_offset(
            fragment.layout_node,
            fragment.start + offset,
            crate::layout::RenderedTextBoundary::End,
        );
    }

    if relative_inline_offset < 0.0 {
        return 0;
    }

    let mut tracker = GraphemeEdgeTracker::new(relative_inline_offset);
    let mut reached_target = false;

    if let Some(run) = &fragment.glyph_run {
        layout_arena.with_grapheme_segmenter(fragment.layout_node, |segmenter| {
            for_each_cluster_in_glyph_run(
                &run.glyphs,
                fragment.length_in_code_units,
                |cluster_start, cluster_end, cluster_width| {
                    let per_unit_advance = cluster_width / (cluster_end - cluster_start) as f32;
                    let mut grapheme_start = fragment.start + cluster_start;
                    let cluster_absolute_end = fragment.start + cluster_end;
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
        });
    }

    if !reached_target && fragment.trailing_whitespace_length_in_code_units > 0 {
        let font = match &fragment.glyph_run {
            Some(run) => Some(run.font.clone()),
            None => first_available_font(layout_arena, fragment),
        };
        if let Some(font) = font {
            let space_width = font.glyph_width(' ' as u32);
            for _ in 0..fragment.trailing_whitespace_length_in_code_units {
                if !tracker.update(1, space_width) {
                    break;
                }
            }
        }
    }

    layout_arena.dom_offset_for_rendered_text_offset(
        fragment.layout_node,
        fragment.start + tracker.resolve(),
        crate::layout::RenderedTextBoundary::End,
    )
}

/// The offset within `fragment` of the grapheme boundary nearest to the inline position `position` (relative to the
/// fragment's start), found from the cluster drawn there — or the nearest one — rather than from advances added up in
/// glyph order, which right-to-left glyphs don't follow. A position past the edge where the fragment's text ends falls
/// among its trailing whitespace, a space's width at a time.
fn rtl_offset_for_inline_position(
    layout_arena: &impl PaintRead,
    fragment: &FragmentRecord,
    run: &crate::layout::inline_content::GlyphRunRecord,
    position: f32,
) -> Option<usize> {
    // Each cluster's code units and the extent of all its glyphs, including those that cover no code units.
    struct Cluster {
        start: usize,
        end: usize,
        left: f32,
        right: f32,
        is_rtl: bool,
    }
    let mut clusters: Vec<Cluster> = Vec::new();
    for placed in run.glyphs.placed() {
        let start = placed.cluster_start as usize;
        let (left, right) = (placed.x, placed.x + placed.glyph.advance.max(0.0));
        if let Some(cluster) = clusters.iter_mut().find(|cluster| cluster.start == start) {
            cluster.left = cluster.left.min(left);
            cluster.right = cluster.right.max(right);
            cluster.end = cluster.end.max(start + placed.glyph.length_in_code_units());
            continue;
        }
        clusters.push(Cluster {
            start,
            end: start + placed.glyph.length_in_code_units(),
            left,
            right,
            is_rtl: placed.is_rtl,
        });
    }
    let length = fragment.length_in_code_units;
    // A truncated fragment can keep glyphs of code units past its end.
    clusters.retain(|cluster| cluster.end > cluster.start && cluster.start < length);

    // Past the edge where the text ends, the position is in the trailing whitespace.
    if fragment.trailing_whitespace_length_in_code_units > 0
        && let Some(last) = clusters.iter().find(|cluster| cluster.end >= length)
    {
        let beyond = if last.is_rtl {
            last.left - position
        } else {
            position - last.right
        };
        let space_width = run.font.glyph_width(' ' as u32);
        if beyond > 0.0 && space_width > 0.0 {
            let spaces =
                ((beyond / space_width).round() as usize).min(fragment.trailing_whitespace_length_in_code_units);
            return Some(length + spaces);
        }
    }

    let distance = |cluster: &Cluster| {
        if position < cluster.left {
            cluster.left - position
        } else {
            (position - cluster.right).max(0.0)
        }
    };
    let cluster = clusters.iter().min_by(|a, b| distance(a).total_cmp(&distance(b)))?;
    let cluster_start = cluster.start;
    let cluster_end = cluster.end.min(length);
    let width = cluster.right - cluster.left;
    let mut fraction = if width > 0.0 {
        ((position - cluster.left) / width).clamp(0.0, 1.0)
    } else {
        0.0
    };
    if cluster.is_rtl {
        fraction = 1.0 - fraction;
    }
    let target = cluster_start as f32 + fraction * (cluster_end - cluster_start) as f32;

    let mut boundaries = vec![cluster_start];
    layout_arena.with_grapheme_segmenter(fragment.layout_node, |segmenter| {
        let mut boundary = fragment.start + cluster_start;
        while let Some(next) = segmenter.next_boundary(boundary, false) {
            if next <= boundary || next > fragment.start + cluster_end {
                break;
            }
            boundaries.push(next - fragment.start);
            boundary = next;
        }
    });
    if *boundaries.last().unwrap() != cluster_end {
        boundaries.push(cluster_end);
    }
    boundaries
        .into_iter()
        .min_by(|a, b| (*a as f32 - target).abs().total_cmp(&(*b as f32 - target).abs()))
}
