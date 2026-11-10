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
        Some(offsets) => rect_for_selection_offsets(layout_arena, fragment, offsets),
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
        fragment.length_in_code_units + fragment.trimmed_trailing_whitespace.length_in_code_units;
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
        fragment.length_in_code_units + fragment.trimmed_trailing_whitespace.length_in_code_units;
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

/// Calls `callback` with each DOM offset (in UTF-16 code units) that the horizontal `fragment` renders, from its first
/// offset through the one past its last, along with the document-absolute x coordinate of the caret in front of that
/// offset's character (or, for the last one, where the fragment's text ends), read from the fragment's shaped glyphs.
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
    let Some(rendered_text) = layout_arena.rendered_text(fragment.layout_node) else {
        return;
    };
    let geometry = run.cluster_geometry();
    let length = fragment.length_in_code_units;
    let x = absolute_rect(layout_arena, fragment).x.to_float();
    rendered_text.for_each_rendered_text_offset_of_dom_offsets(
        fragment.dom_start_offset_in_node,
        fragment.dom_end_offset_in_node,
        crate::layout::RenderedTextBoundary::End,
        |dom_offset, rendered_offset| {
            let offset_in_fragment = rendered_offset.clamp(fragment.start, fragment.start + length) - fragment.start;
            let inline_x = geometry
                .x_for_offset(offset_in_fragment, true)
                .unwrap_or_else(|| geometry.end_x(length));
            callback(dom_offset, x + inline_x);
        },
    );
}

pub(crate) fn rect_for_selection_offsets(
    layout_arena: &impl PaintRead,
    fragment: &FragmentRecord,
    offsets: SelectionOffsets,
) -> CssPixelRect {
    let horizontal = is_horizontal(fragment);
    let mut rect = absolute_rect(layout_arena, fragment);
    let length = fragment.length_in_code_units;

    let start_in_text = offsets.start.min(length);
    let end_in_text = offsets.end.min(length);

    let mut pixel_offset;
    let mut pixel_width;
    if start_in_text == 0 && end_in_text == length && length > 0 {
        pixel_offset = CssPixels::from_raw(0);
        pixel_width = primary_size(rect, horizontal);
    } else {
        let (left, right) = fragment.glyph_run.as_ref().map_or((0.0, 0.0), |run| {
            run.cluster_geometry().range_extent(start_in_text, end_in_text, length)
        });
        pixel_offset = CssPixels::nearest_value_for_f32(left);
        pixel_width = CssPixels::nearest_value_for_f32(right - left);
    }

    let trimmed = fragment.trimmed_trailing_whitespace;
    if trimmed.length_in_code_units > 0 {
        let trimmed_advance_of = |offset: usize| {
            let units = offset.saturating_sub(length).min(trimmed.length_in_code_units);
            CssPixels::from_raw(
                (i64::from(trimmed.inline_size.raw_value()) * units as i64 / trimmed.length_in_code_units as i64)
                    as i32,
            )
        };
        pixel_offset += trimmed_advance_of(offsets.start);
        pixel_width += trimmed_advance_of(offsets.end) - trimmed_advance_of(offsets.start);
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

    if let Some(run) = &fragment.glyph_run {
        let zero = CssPixels::from_raw(0);
        let overflow_top = (run.ascent - fragment.baseline).max(zero);
        let overflow_bottom = (run.descent - secondary_size(rect, horizontal) + fragment.baseline).max(zero);
        if horizontal {
            rect.inflate(overflow_top, zero, overflow_bottom, zero);
        } else {
            rect.inflate(zero, overflow_bottom, zero, overflow_top);
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
) -> Vec<CssPixelRect> {
    let rect = rect_for_selection_offsets(layout_arena, fragment, offsets);
    let length = fragment.length_in_code_units;
    let extents = fragment.glyph_run.as_ref().map_or_else(Vec::new, |run| {
        run.cluster_geometry()
            .range_extents(offsets.start.min(length), offsets.end.min(length), length)
    });
    if extents.len() < 2 {
        return vec![rect];
    }
    let horizontal = is_horizontal(fragment);
    let origin = absolute_rect(layout_arena, fragment);
    extents
        .into_iter()
        .map(|(start, end)| {
            let mut piece = rect;
            let offset = CssPixels::nearest_value_for_f32(start);
            let size = CssPixels::nearest_value_for_f32(end - start);
            if horizontal {
                piece.x = origin.x + offset;
                piece.width = size;
            } else {
                piece.y = origin.y + offset;
                piece.height = size;
            }
            piece
        })
        .collect()
}

pub(crate) fn is_block_level_box(layout_arena: &impl PaintRead, fragment: &FragmentRecord) -> bool {
    layout_arena
        .node_style_if_live(fragment.layout_node)
        .is_some_and(|style| style.display().is_block_outside())
}

pub(crate) fn style_source(_layout_arena: &impl PaintRead, fragment: &FragmentRecord) -> NodeSlotId {
    fragment.style_source
}

pub(crate) fn physical_edge_offset(
    arena: &impl PaintRead,
    fragment: &FragmentRecord,
    left: bool,
) -> Option<(usize, bool)> {
    let (boundary, at_cluster_end) = fragment.glyph_run.as_ref()?.cluster_geometry().edge_offset(left)?;
    Some((
        arena.dom_offset_for_rendered_text_offset(
            fragment.layout_node,
            fragment.start + boundary,
            crate::layout::RenderedTextBoundary::End,
        ),
        at_cluster_end,
    ))
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
    let x = if is_horizontal(fragment) {
        (position.x - rect.x).to_float()
    } else {
        (position.y - rect.y).to_float()
    };
    // NB: A point past the end of the text takes the offset in front of the whitespace the line trimmed.
    let offset = fragment.glyph_run.as_ref().map_or(0, |run| {
        layout_arena
            .with_grapheme_segmenter(fragment.layout_node, |segmenter| {
                run.cluster_geometry()
                    .offset_for_x(x, fragment.length_in_code_units, |offset| {
                        segmenter
                            .next_boundary(fragment.start + offset, false)
                            .map(|boundary| boundary - fragment.start)
                    })
            })
            .unwrap_or_default()
    });
    layout_arena.dom_offset_for_rendered_text_offset(
        fragment.layout_node,
        fragment.start + offset,
        crate::layout::RenderedTextBoundary::End,
    )
}
