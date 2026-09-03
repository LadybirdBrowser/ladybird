/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

const ELLIPSIS_CODE_POINT: u32 = 0x2026;

fn truncate_line_at_glyph_boundary(
    line: &mut line_box::LineBoxData,
    available_inline_size: CssPixels,
    line_direction: u8,
) -> Option<(usize, CssPixels)> {
    let index = line.fragments.iter().position(|fragment| {
        !fragment.is_fully_truncated
            && (fragment.has_text_overflow_ellipsis
                || fragment.inline_offset + fragment.inline_length > available_inline_size)
    })?;
    let fragment = &mut line.fragments[index];
    if fragment.has_text_overflow_ellipsis {
        if index == 0 {
            return Some((index, fragment.inline_offset));
        }
        fragment.glyphs.as_mut().unwrap().glyphs.pop();
        fragment.has_text_overflow_ellipsis = false;
    }
    let available = available_inline_size - fragment.inline_offset;
    let Some(glyphs) = fragment.glyphs.as_mut().filter(|_| available > CssPixels::default()) else {
        return (index > 0).then_some((index, fragment.inline_offset));
    };
    let glyph_end =
        |glyph: &libgfx_rust::text_layout::DrawGlyph| CssPixels::nearest_value_for_f32(glyph.x + glyph.glyph_width);
    let keep_from_end = fragment.current_insert_direction == direction::RTL && line_direction == direction::RTL;
    let (mut start, mut end) = if keep_from_end {
        (
            glyphs.glyphs.partition_point(|glyph| {
                fragment.inline_length - CssPixels::nearest_value_for_f32(glyph.x) > available
            }),
            glyphs.glyphs.len(),
        )
    } else {
        (0, glyphs.glyphs.partition_point(|glyph| glyph_end(glyph) <= available))
    };
    while end > start && end < glyphs.glyphs.len() && glyphs.glyphs[end].length_in_code_units == 0 {
        end -= 1;
    }
    while start < end && glyphs.glyphs[start].length_in_code_units == 0 {
        start += 1;
    }
    if start == end {
        return (index > 0).then_some((index, fragment.inline_offset));
    }
    let origin = CssPixels::nearest_value_for_f32(glyphs.glyphs[start].x);
    let inline_size = glyph_end(&glyphs.glyphs[end - 1]) - origin;
    glyphs.glyphs.drain(end..);
    glyphs.glyphs.drain(..start);
    for glyph in &mut glyphs.glyphs {
        glyph.x -= origin.to_double() as f32;
    }
    let length = glyphs.glyphs.iter().map(|glyph| glyph.length_in_code_units).sum();
    glyphs.width = inline_size.to_double() as f32;
    if fragment.current_insert_direction == direction::RTL && !keep_from_end {
        fragment.start += fragment.length_in_code_units - length;
    }
    fragment.length_in_code_units = length;
    fragment.inline_length = inline_size;
    fragment.trailing_whitespace = Default::default();
    Some((index + 1, fragment.inline_offset + inline_size))
}

fn apply_text_overflow_to_line(line: &mut line_box::LineBoxData) {
    if !matches!(line.original_available_inline_size, AvailableSize::Definite(_)) {
        return;
    }
    let available_inline_size = line.original_available_inline_size.to_px_or_zero();
    if line.inline_length <= available_inline_size || line.fragments.is_empty() {
        return;
    }

    let mut line_has_visible_content = false;
    for index in 0..line.fragments.len() {
        let fragment_start = line.fragments[index].inline_offset;
        if fragment_start + line.fragments[index].inline_length <= available_inline_size {
            line_has_visible_content = true;
            continue;
        }
        let Some(glyph_data) = &line.fragments[index].glyphs else {
            continue;
        };
        let font = glyph_data.font;
        let ellipsis_inline_size = font::font_glyph_width(font, ELLIPSIS_CODE_POINT);
        let available_in_fragment = (available_inline_size - fragment_start).raw_value() as f32 / 64.0;
        let max_text_inline_size = available_in_fragment - ellipsis_inline_size;

        let glyphs = &line.fragments[index].glyphs.as_ref().unwrap().glyphs;
        let mut keep_count = 0usize;
        let mut last_kept_end = 0.0f32;
        let mut glyph_block_offset = 0.0f32;
        for glyph in glyphs {
            let glyph_end = glyph.x + glyph.glyph_width;
            if glyph_end > max_text_inline_size && (keep_count > 0 || line_has_visible_content) {
                break;
            }
            keep_count += 1;
            last_kept_end = glyph_end;
            glyph_block_offset = glyph.y;
        }

        let glyph_data = line.fragments[index].glyphs.as_mut().unwrap();
        glyph_data.glyphs.truncate(keep_count);
        glyph_data.glyphs.push(libgfx_rust::text_layout::DrawGlyph {
            x: last_kept_end,
            y: glyph_block_offset,
            length_in_code_units: 1,
            glyph_width: ellipsis_inline_size,
            glyph_id: font::font_glyph_id(font, ELLIPSIS_CODE_POINT),
            should_paint: true,
        });
        line.fragments[index].inline_length = CssPixels::nearest_value_for_f32(last_kept_end + ellipsis_inline_size);
        line.fragments[index].has_text_overflow_ellipsis = true;
        for later in &mut line.fragments[index + 1..] {
            later.is_fully_truncated = true;
        }
        line.inline_length = available_inline_size;
        line.clamp_static_position_markers_to_inline_length();
        break;
    }
}

// https://drafts.csswg.org/css-overflow-4/#block-ellipsis
// The user agent makes room as necessary of the block overflow ellipsis by displacing content from the end of the
// line as if wrapping, until the last soft wrap opportunity that would still allow the entire block overflow
// ellipsis to fit on the line.
fn apply_block_ellipsis(
    line: &mut line_box::LineBoxData,
    context: &InlineFormattingContext<'_>,
    ellipsis_text: &[u16],
) {
    let (style_source, style) = (context.containing_block, context.style(context.containing_block));
    let first_code_point = char::decode_utf16(ellipsis_text.iter().copied())
        .next()
        .map_or(char::REPLACEMENT_CHARACTER, |result| {
            result.unwrap_or(char::REPLACEMENT_CHARACTER)
        }) as u32;
    let presentation = libgfx_rust::font::emoji_presentation_for_code_point(first_code_point, None);
    // SAFETY: Font cascade pointers in layout snapshots are borrowed from the host for the synchronous layout pass.
    let font = unsafe { libgfx_rust::font::FontCascadeListRef::from_raw(style.font_cascade_list()) }
        .font_for_code_point(first_code_point, presentation)
        .as_raw();
    let shaped_ellipsis = font::shape_text_with_font(
        font,
        ellipsis_text,
        line_box_fragment::GLYPH_TEXT_TYPE_COMMON,
        0.0,
        style.letter_spacing().to_double() as f32,
        style.word_spacing().to_double() as f32,
    );
    let ellipsis_width = shaped_ellipsis.width();
    let ellipsis_inline_size = CssPixels::nearest_value_for_f32(ellipsis_width);
    let available_inline_size = match line.original_available_inline_size {
        AvailableSize::Definite(size) => size,
        _ => line.inline_length + ellipsis_inline_size,
    };
    line.inline_length_before_block_ellipsis = Some(line.inline_length);
    line.trim_trailing_whitespace_before_block_ellipsis();
    let available_for_content = (available_inline_size - ellipsis_inline_size).max(CssPixels::default());
    let has_text_overflow_ellipsis = line
        .visible_fragments()
        .any(|fragment| fragment.has_text_overflow_ellipsis);
    let opportunity = line.fragments.iter().enumerate().rev().find_map(|(index, fragment)| {
        let trailing = fragment.trailing_whitespace;
        let inline_size = fragment.inline_length - trailing.inline_size;
        let inline_offset = fragment.inline_offset + inline_size;
        (fragment.has_soft_wrap_opportunity_after && inline_offset <= available_for_content).then_some((
            index + 1,
            inline_offset,
            fragment.length_in_code_units - trailing.length_in_code_units,
            inline_size,
        ))
    });
    let (first_displaced_fragment, retained_inline_size) = if line.inline_length <= available_for_content
        && !has_text_overflow_ellipsis
    {
        (line.fragments.len(), line.inline_length)
    } else if let Some((fragment_count, inline_offset, length, inline_size)) = opportunity {
        let last_retained_fragment = &mut line.fragments[fragment_count - 1];
        if last_retained_fragment.length_in_code_units > length || last_retained_fragment.has_text_overflow_ellipsis {
            let mut retained_code_units = 0usize;
            if let Some(glyphs) = &mut last_retained_fragment.glyphs {
                glyphs.glyphs.retain(|glyph| {
                    retained_code_units += glyph.length_in_code_units;
                    retained_code_units <= length
                });
                glyphs.width = inline_size.to_double() as f32;
            }
            last_retained_fragment.length_in_code_units = length;
            last_retained_fragment.inline_length = inline_size;
            last_retained_fragment.trailing_whitespace = Default::default();
            last_retained_fragment.has_text_overflow_ellipsis = false;
        }
        (fragment_count, inline_offset)
    } else {
        // INTEROP: Legacy -webkit-line-clamp truncates unbreakable text at a glyph boundary when no soft wrap exists.
        // NB: Trailing inline box edges can overflow available_for_content even when every content fragment fits.
        //     Preserve those fragments if glyph truncation therefore finds no boundary to remove.
        let mut retained_inline_size: Option<CssPixels> = None;
        let mut all_content_fits = true;
        for fragment in line.visible_fragments() {
            let content_inline_size = if fragment.is_atomic_inline {
                Some(fragment.inline_length)
            } else {
                fragment
                    .glyphs
                    .as_ref()
                    .filter(|glyphs| !glyphs.glyphs.is_empty())
                    .map(|glyphs| CssPixels::nearest_value_for_f32(glyphs.width))
            };
            let Some(content_inline_size) = content_inline_size else {
                continue;
            };
            let fragment_end = fragment.inline_offset + content_inline_size;
            if fragment_end > available_for_content {
                all_content_fits = false;
                break;
            }
            retained_inline_size = Some(retained_inline_size.unwrap_or_default().max(fragment_end));
        }
        let preserve_existing_fragments = retained_inline_size
            .filter(|_| all_content_fits)
            .map(|retained_inline_size| (line.fragments.len(), retained_inline_size));
        truncate_line_at_glyph_boundary(line, available_for_content, line.direction)
            .or(preserve_existing_fragments)
            .unwrap_or_default()
    };
    for fragment in &mut line.fragments[first_displaced_fragment..] {
        fragment.is_fully_truncated = true;
        if fragment.is_atomic_inline {
            context.hide_atomic_inline_for_line_clamp(fragment.layout_node);
        }
    }
    line.static_position_markers
        .retain(|marker| marker.inline_offset <= retained_inline_size);

    // https://drafts.csswg.org/css-overflow-4/#block-ellipsis
    // For bidi purposes, the block overflow ellipsis must be treated as an anonymous inline with unicode-bidi:
    // isolate, with the same embedding level as the bidi paragraph, and which inherits direction from the bidi
    // paragraph.
    let ellipsis_precedes_content = line.direction == direction::RTL
        && line
            .visible_fragments()
            .next_back()
            .is_none_or(|fragment| fragment.current_insert_direction != direction::LTR);
    let (content_inline_start, ellipsis_inline_offset) = if ellipsis_precedes_content {
        (ellipsis_inline_size, CssPixels::default())
    } else {
        (CssPixels::default(), retained_inline_size)
    };
    for fragment in &mut line.fragments {
        fragment.inline_offset += content_inline_start;
    }
    for marker in &mut line.static_position_markers {
        marker.inline_offset += content_inline_start;
    }
    line.inline_length = retained_inline_size + ellipsis_inline_size;

    let baseline = CssPixels::nearest_value_for_f32(style.font_ascent())
        + (style.line_height() - CssPixels::nearest_value_for_f32(style.font_ascent() + style.font_descent())) / 2;
    let boundary_fragment = line.visible_fragments().next_back().or_else(|| line.fragments.first());
    let layout_node =
        boundary_fragment.map_or_else(|| context.first_child(style_source), |fragment| fragment.layout_node);
    let start = boundary_fragment.map_or(0, |fragment| {
        if fragment.is_fully_truncated {
            fragment.start
        } else {
            fragment.start + fragment.length_in_code_units
        }
    });
    let mut ellipsis = line_box_fragment::LineBoxFragmentData::new(
        layout_node,
        start,
        ellipsis_text.len(),
        ellipsis_inline_offset,
        CssPixels::default(),
        ellipsis_inline_size,
        line_builder::normal_line_height(style),
        CssPixels::default(),
        line.direction,
        line.writing_mode,
        Some(line_box_fragment::GlyphData {
            glyphs: shaped_ellipsis.into_glyphs(),
            font,
            text_type: line_box_fragment::GLYPH_TEXT_TYPE_COMMON,
            width: ellipsis_width,
        }),
        line_box_fragment::FragmentBuildFacts {
            style_source,
            is_atomic_inline: false,
            white_space_collapse: style.white_space_collapse(),
            text_utf16: std::ptr::null(),
            text_length_in_code_units: 0,
        },
    );
    // https://drafts.csswg.org/css-overflow-4/#block-ellipsis
    // The block overflow ellipsis is wrapped in an anonymous inline whose parent is the block container's root
    // inline box. This inline is assigned line-height: 0.
    // NB: The fragment is inserted after line sizing, so its recorded block length is only used to align its glyphs
    //     and cannot increase the line box's block size.
    ellipsis.baseline = baseline;
    ellipsis.is_block_ellipsis = true;
    line.fragments.push(ellipsis);
}

pub(crate) fn apply_to_fragments(text_justify: u8, line: &mut line_box::LineBoxData, is_last_line: bool) {
    if text_justify == text_justify::NONE || is_last_line || line.has_forced_break {
        return;
    }
    assert!(matches!(
        text_justify,
        text_justify::AUTO | text_justify::INTER_WORD | text_justify::INTER_CHARACTER
    ));

    let excess_inline_space = line.original_available_inline_size.to_px_or_zero() - line.inline_length;
    let mut excess_inline_space_including_whitespace = excess_inline_space;
    let mut whitespace_count = 0usize;
    for fragment in &line.fragments {
        if !fragment.is_fully_truncated && fragment.is_justifiable_whitespace() {
            whitespace_count += 1;
            excess_inline_space_including_whitespace += fragment.inline_length;
        }
    }
    let justified_space_inline_size = if whitespace_count > 0 {
        excess_inline_space_including_whitespace / whitespace_count
    } else {
        CssPixels::default()
    };

    let mut running_diff = CssPixels::default();
    for fragment in &mut line.fragments {
        if fragment.is_fully_truncated {
            continue;
        }
        fragment.inline_offset += running_diff;
        if fragment.is_justifiable_whitespace() && fragment.inline_length != justified_space_inline_size {
            let diff = justified_space_inline_size - fragment.inline_length;
            running_diff += diff;
            for marker in &mut line.static_position_markers {
                // This intentionally compares against the fragment's already
                // shifted offset, matching the C++ ordering.
                if marker.inline_offset > fragment.inline_offset {
                    marker.inline_offset += diff;
                }
            }
            fragment.inline_length = justified_space_inline_size;
        }
    }
}

pub(crate) const EDGE_TOP: u8 = 1 << 0;
pub(crate) const EDGE_RIGHT: u8 = 1 << 1;
pub(crate) const EDGE_BOTTOM: u8 = 1 << 2;
pub(crate) const EDGE_LEFT: u8 = 1 << 3;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct InlineCssPixelRect {
    pub(crate) x: CssPixels,
    pub(crate) y: CssPixels,
    pub(crate) width: CssPixels,
    pub(crate) height: CssPixels,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct InlineBoxPieceData {
    pub(crate) node: Node,
    pub(crate) first_fragment_index: u32,
    pub(crate) fragment_count: u32,
    pub(crate) line_index: u32,
    pub(crate) border_box_rect: InlineCssPixelRect,
    pub(crate) relpos_delta: FfiCssPixelPoint,
    pub(crate) baseline: CssPixels,
    pub(crate) accumulated_vertical_shift: CssPixels,
    pub(crate) present_edges: u8,
    pub(crate) is_geometry_only_placeholder: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct StagedPiece {
    pub(crate) piece: InlineBoxPieceData,
    pub(crate) depth: u32,
    pub(crate) discovery_index: usize,
}

pub(crate) fn sort_for_emission(pieces: &mut [StagedPiece]) {
    pieces.sort_by_key(|piece| (piece.piece.line_index, piece.depth, piece.discovery_index));
}

#[derive(Debug)]
struct PerLine {
    line_index: usize,
    contributions_inline_range: Option<(CssPixels, CssPixels)>,
    first_direct_fragment_block_start: Option<CssPixels>,
    max_direct_fragment_block_length: CssPixels,
    fallback_block_start_from_contributions: Option<CssPixels>,
    interrupting_block: Option<((CssPixels, CssPixels), CandidateLineCorners)>,
    first_fragment_index: Option<u32>,
    fragment_count: u32,
}

impl PerLine {
    fn new(line_index: usize) -> Self {
        Self {
            line_index,
            contributions_inline_range: None,
            first_direct_fragment_block_start: None,
            max_direct_fragment_block_length: CssPixels::default(),
            fallback_block_start_from_contributions: None,
            interrupting_block: None,
            first_fragment_index: None,
            fragment_count: 0,
        }
    }
}

#[derive(Debug)]
struct PerNode {
    node: Node,
    parent_index: Option<usize>,
    depth: u32,
    lines: Vec<PerLine>,
    first_line_with_content: Option<usize>,
    last_line_with_content: Option<usize>,
}

fn ensure_line(per_node: &mut PerNode, line_index: usize) -> &mut PerLine {
    let insertion = per_node.lines.partition_point(|line| line.line_index <= line_index);
    if insertion != 0 && per_node.lines[insertion - 1].line_index == line_index {
        return &mut per_node.lines[insertion - 1];
    }
    per_node.lines.insert(insertion, PerLine::new(line_index));
    &mut per_node.lines[insertion]
}

fn note_contribution(line: &mut PerLine, inline_start: CssPixels, inline_end: CssPixels, block_start: CssPixels) {
    if line
        .contributions_inline_range
        .is_none_or(|(current_start, _)| inline_start < current_start)
    {
        line.fallback_block_start_from_contributions = Some(block_start);
    }
    line.contributions_inline_range = Some(
        line.contributions_inline_range
            .map_or((inline_start, inline_end), |(current_start, current_end)| {
                (current_start.min(inline_start), current_end.max(inline_end))
            }),
    );
}

fn nesting_depth(context: &InlineFormattingContext, node: Node) -> u32 {
    let mut depth = 1;
    let mut ancestor = context.nearest_fragmented_inline_ancestor(node);
    while !ancestor.is_invalid() {
        depth += 1;
        ancestor = context.nearest_fragmented_inline_ancestor(ancestor);
    }
    depth
}

fn edge_bits(horizontal: bool, low: bool, high: bool) -> u8 {
    let mut edges = if horizontal {
        EDGE_TOP | EDGE_BOTTOM
    } else {
        EDGE_LEFT | EDGE_RIGHT
    };
    if low {
        edges |= if horizontal { EDGE_LEFT } else { EDGE_TOP };
    }
    if high {
        edges |= if horizontal { EDGE_RIGHT } else { EDGE_BOTTOM };
    }
    edges
}

pub(crate) struct InlineContainingBlockRectCandidate {
    pub(crate) inline_containing_block: Node,
    pub(crate) rect: formatting_context::PhysicalRect,
}

#[derive(Clone, Copy, Debug)]
struct CandidateLineCorners {
    inline_start: CssPixels,
    inline_end: CssPixels,
    block_start: CssPixels,
    block_end: CssPixels,
}

#[derive(Default)]
struct FirstAndLastContentLineCorners {
    first: Option<CandidateLineCorners>,
    last: Option<CandidateLineCorners>,
}

pub(crate) fn compute(
    context: &InlineFormattingContext,
) -> (Vec<InlineBoxPieceData>, Vec<InlineContainingBlockRectCandidate>) {
    let horizontal = context.style(context.containing_block).writing_mode() == writing_mode::HORIZONTAL_TB;
    let collect_inline_containing_block_rects = context
        .run
        .fragments
        .as_deref()
        .is_some_and(fragment_tree::RunFragmentBuilder::any_pending_abspos_has_inline_containing_block);
    let container_inline_axis_is_reverse =
        collect_inline_containing_block_rects && context.facts(context.containing_block).inline_axis_is_reverse();
    let mut inline_containing_block_rect_candidates = Vec::<InlineContainingBlockRectCandidate>::new();
    let mut per_nodes = Vec::<PerNode>::new();
    let mut node_to_index = HashMap::<Node, usize>::default();

    let mut committed_fragment_index = 0u32;
    for (line_index, line) in context.line_data().line_boxes.iter().enumerate() {
        for fragment in &line.fragments {
            if fragment.is_fully_truncated || fragment.is_block_ellipsis {
                continue;
            }
            let fragment_index = committed_fragment_index;
            committed_fragment_index += 1;
            let interrupting = line.has_block_level_box;
            debug_assert!(
                !interrupting || context.style(fragment.style_source).display().is_block_outside(),
                "an interrupting line's fragment must be a block-level box"
            );
            let position = fragment.offset();
            let size = fragment.size();
            let mut inline_start = if horizontal { position.0 } else { position.1 };
            let mut inline_end = inline_start + if horizontal { size.0 } else { size.1 };
            let block_start = if horizontal { position.1 } else { position.0 };
            let block_length = if horizontal { size.1 } else { size.0 };

            if !interrupting && fragment.is_atomic_inline {
                let used = context.used(fragment.layout_node);
                if horizontal {
                    inline_start -= used.margin_left.get() + used.border_box_left(false);
                    inline_end += used.margin_right.get() + used.border_box_right(false);
                } else {
                    inline_start -= used.margin_top.get() + used.border_box_top(false);
                    inline_end += used.margin_bottom.get() + used.border_box_bottom(false);
                }
            }

            let interrupting_block = if interrupting {
                let block_used = context.used(fragment.layout_node);
                let collapsed = block_used.uses_collapsing_borders_model.get();
                let (border_box_inline_low, border_box_block_low, border_box_inline_size, border_box_block_size) =
                    if horizontal {
                        (
                            block_used.border_box_left(collapsed),
                            block_used.border_box_top(collapsed),
                            block_used.border_box_inline_size(collapsed),
                            block_used.border_box_block_size(collapsed),
                        )
                    } else {
                        (
                            block_used.border_box_top(collapsed),
                            block_used.border_box_left(collapsed),
                            block_used.border_box_block_size(collapsed),
                            block_used.border_box_inline_size(collapsed),
                        )
                    };
                let extent_inline_start = inline_start - border_box_inline_low;
                let extent_block_start = block_start - border_box_block_low;
                Some((
                    position,
                    CandidateLineCorners {
                        inline_start: extent_inline_start,
                        inline_end: extent_inline_start + border_box_inline_size,
                        block_start: extent_block_start,
                        block_end: extent_block_start + border_box_block_size,
                    },
                ))
            } else {
                None
            };

            let mut direct = true;
            let mut previous: Option<usize> = None;
            let mut ancestor = context.nearest_fragmented_inline_ancestor(fragment.layout_node);
            while !ancestor.is_invalid() {
                let node_index = if let Some(index) = node_to_index.get(&ancestor) {
                    *index
                } else {
                    let index = per_nodes.len();
                    per_nodes.push(PerNode {
                        node: ancestor,
                        parent_index: None,
                        depth: nesting_depth(context, ancestor),
                        lines: Vec::new(),
                        first_line_with_content: None,
                        last_line_with_content: None,
                    });
                    node_to_index.insert(ancestor, index);
                    index
                };
                if let Some(previous) = previous {
                    per_nodes[previous].parent_index = Some(node_index);
                }
                previous = Some(node_index);
                let per_node = &mut per_nodes[node_index];
                let line = ensure_line(per_node, line_index);
                let first = *line.first_fragment_index.get_or_insert(fragment_index);
                line.fragment_count = fragment_index + 1 - first;
                if let Some(interrupting_block) = interrupting_block {
                    line.interrupting_block.get_or_insert(interrupting_block);
                    ancestor = context.nearest_fragmented_inline_ancestor(ancestor);
                    continue;
                }
                if direct {
                    note_contribution(line, inline_start, inline_end, block_start);
                    line.first_direct_fragment_block_start.get_or_insert(block_start);
                    line.max_direct_fragment_block_length = line.max_direct_fragment_block_length.max(block_length);
                } else if line.fallback_block_start_from_contributions.is_none() {
                    line.fallback_block_start_from_contributions = Some(block_start);
                }
                per_node.first_line_with_content.get_or_insert(line_index);
                per_node.last_line_with_content = Some(line_index);
                direct = false;
                ancestor = context.nearest_fragmented_inline_ancestor(ancestor);
            }
        }
    }

    let without_fragments: Vec<_> = context
        .fragmented_inlines_in_pre_order
        .iter()
        .copied()
        .filter(|node| context.facts(*node).has_dom_node() && !node_to_index.contains_key(node))
        .collect();

    let mut staged = Vec::<StagedPiece>::new();
    let mut deepest_first: Vec<_> = (0..per_nodes.len()).collect();
    deepest_first.sort_by(|left, right| {
        per_nodes[*right]
            .depth
            .cmp(&per_nodes[*left].depth)
            .then_with(|| left.cmp(right))
    });

    for node_index in deepest_first {
        let (node, parent_index, depth, first_line_with_content, last_line_with_content, lines) = {
            let per_node = &mut per_nodes[node_index];
            (
                per_node.node,
                per_node.parent_index,
                per_node.depth,
                per_node.first_line_with_content,
                per_node.last_line_with_content,
                std::mem::take(&mut per_node.lines),
            )
        };
        let used = context.used(node);
        let reversed = context.facts(node).inline_axis_is_reverse();
        let (
            border_padding_low,
            border_padding_high,
            border_padding_block_low,
            border_padding_block_high,
            margin_low,
            margin_high,
        ) = if horizontal {
            (
                used.border_box_left(false),
                used.border_box_right(false),
                used.border_box_top(false),
                used.border_box_bottom(false),
                used.margin_left.get(),
                used.margin_right.get(),
            )
        } else {
            (
                used.border_box_top(false),
                used.border_box_bottom(false),
                used.border_box_left(false),
                used.border_box_right(false),
                used.margin_top.get(),
                used.margin_bottom.get(),
            )
        };

        let node_is_inline_containing_block = collect_inline_containing_block_rects
            && context
                .run
                .fragments
                .as_deref()
                .is_some_and(|fragments| fragments.any_pending_abspos_names_inline_containing_block(node));
        let mut corners = FirstAndLastContentLineCorners::default();

        for line in lines {
            let inline_box_anchor = || {
                let line_data = context.line_data();
                let line_box = &line_data.line_boxes[line.line_index];
                line_box.inline_box_baseline(node).map_or(
                    (line_box.block_start + line_box.baseline, CssPixels::default()),
                    |entry| (entry.baseline, entry.accumulated_vertical_shift),
                )
            };
            let Some((contributions_inline_start, contributions_inline_end)) = line.contributions_inline_range else {
                if let Some((position, extent)) = line.interrupting_block {
                    if node_is_inline_containing_block {
                        corners.first.get_or_insert(extent);
                        corners.last = Some(extent);
                    }
                    let (inline_box_baseline, inline_box_vertical_shift) = inline_box_anchor();
                    staged.push(StagedPiece {
                        piece: InlineBoxPieceData {
                            node,
                            first_fragment_index: line.first_fragment_index.unwrap_or(0),
                            fragment_count: line.fragment_count,
                            line_index: line.line_index as u32,
                            border_box_rect: InlineCssPixelRect {
                                x: position.0,
                                y: position.1,
                                ..Default::default()
                            },
                            relpos_delta: FfiCssPixelPoint::default(),
                            baseline: inline_box_baseline,
                            accumulated_vertical_shift: inline_box_vertical_shift,
                            present_edges: edge_bits(horizontal, true, true),
                            is_geometry_only_placeholder: true,
                        },
                        depth,
                        discovery_index: node_index,
                    });
                }
                continue;
            };
            let first = first_line_with_content == Some(line.line_index);
            let last = last_line_with_content == Some(line.line_index);
            let has_low_edge = if reversed { last } else { first };
            let has_high_edge = if reversed { first } else { last };
            let content_block_start = line
                .first_direct_fragment_block_start
                .or(line.fallback_block_start_from_contributions)
                .unwrap_or_default();
            let content_block_length = if line.first_direct_fragment_block_start.is_some() {
                line.max_direct_fragment_block_length
            } else {
                context.style(node).line_height()
            };
            let border_inline_start = contributions_inline_start
                - if has_low_edge {
                    border_padding_low
                } else {
                    CssPixels::default()
                };
            let border_inline_end = contributions_inline_end
                + if has_high_edge {
                    border_padding_high
                } else {
                    CssPixels::default()
                };
            let border_block_start = content_block_start - border_padding_block_low;
            let border_block_length = content_block_length + border_padding_block_low + border_padding_block_high;
            let (inline_box_baseline, inline_box_vertical_shift) = inline_box_anchor();
            let rect = if horizontal {
                InlineCssPixelRect {
                    x: border_inline_start,
                    y: border_block_start,
                    width: border_inline_end - border_inline_start,
                    height: border_block_length,
                }
            } else {
                InlineCssPixelRect {
                    x: border_block_start,
                    y: border_inline_start,
                    width: border_block_length,
                    height: border_inline_end - border_inline_start,
                }
            };
            staged.push(StagedPiece {
                piece: InlineBoxPieceData {
                    node,
                    first_fragment_index: line.first_fragment_index.unwrap_or(0),
                    fragment_count: line.fragment_count,
                    line_index: line.line_index as u32,
                    border_box_rect: rect,
                    relpos_delta: FfiCssPixelPoint::default(),
                    baseline: inline_box_baseline,
                    accumulated_vertical_shift: inline_box_vertical_shift,
                    present_edges: edge_bits(horizontal, has_low_edge, has_high_edge),
                    is_geometry_only_placeholder: false,
                },
                depth,
                discovery_index: node_index,
            });
            if node_is_inline_containing_block {
                let source = CandidateLineCorners {
                    inline_start: border_inline_start,
                    inline_end: border_inline_end,
                    block_start: border_block_start,
                    block_end: border_block_start + border_block_length,
                };
                corners.first.get_or_insert(source);
                corners.last = Some(source);
            }
            if let Some(parent_index) = parent_index {
                let parent_line = ensure_line(&mut per_nodes[parent_index], line.line_index);
                note_contribution(
                    parent_line,
                    border_inline_start - if has_low_edge { margin_low } else { CssPixels::default() },
                    border_inline_end
                        + if has_high_edge {
                            margin_high
                        } else {
                            CssPixels::default()
                        },
                    content_block_start,
                );
            }
        }

        if node_is_inline_containing_block
            && let Some(rect) = padding_box_rect_spanning_first_and_last_content_lines(
                &corners,
                &used,
                horizontal,
                reversed,
                container_inline_axis_is_reverse,
            )
        {
            inline_containing_block_rect_candidates.push(InlineContainingBlockRectCandidate {
                inline_containing_block: node,
                rect,
            });
        }
    }

    for (index, node) in without_fragments.into_iter().enumerate() {
        context.used(node);
        let line_height = context.style(node).line_height();
        let placeholder_rect = if horizontal {
            InlineCssPixelRect {
                height: line_height,
                ..Default::default()
            }
        } else {
            InlineCssPixelRect {
                width: line_height,
                ..Default::default()
            }
        };
        if collect_inline_containing_block_rects
            && context
                .run
                .fragments
                .as_deref()
                .is_some_and(|fragments| fragments.any_pending_abspos_names_inline_containing_block(node))
        {
            inline_containing_block_rect_candidates.push(InlineContainingBlockRectCandidate {
                inline_containing_block: node,
                rect: formatting_context::PhysicalRect {
                    x: placeholder_rect.x,
                    y: placeholder_rect.y,
                    width: placeholder_rect.width,
                    height: placeholder_rect.height,
                },
            });
        }
        staged.push(StagedPiece {
            piece: InlineBoxPieceData {
                node,
                first_fragment_index: 0,
                fragment_count: 0,
                line_index: 0,
                border_box_rect: placeholder_rect,
                relpos_delta: FfiCssPixelPoint::default(),
                baseline: CssPixels::default(),
                accumulated_vertical_shift: CssPixels::default(),
                present_edges: edge_bits(horizontal, true, true),
                is_geometry_only_placeholder: true,
            },
            depth: nesting_depth(context, node),
            discovery_index: per_nodes.len() + index,
        });
    }
    sort_for_emission(&mut staged);
    let pieces = staged.into_iter().map(|staged| staged.piece).collect();
    (pieces, inline_containing_block_rect_candidates)
}

fn padding_box_rect_spanning_first_and_last_content_lines(
    corners: &FirstAndLastContentLineCorners,
    used: &UsedValues,
    horizontal: bool,
    inline_axis_is_reverse: bool,
    container_inline_axis_is_reverse: bool,
) -> Option<formatting_context::PhysicalRect> {
    let first = corners.first?;
    let last = corners.last?;

    let (border_inline_low, border_inline_high, border_block_low, border_block_high) = if horizontal {
        (
            used.border_left.get(),
            used.border_right.get(),
            used.border_top.get(),
            used.border_bottom.get(),
        )
    } else {
        (
            used.border_top.get(),
            used.border_bottom.get(),
            used.border_left.get(),
            used.border_right.get(),
        )
    };
    let direction_matches = inline_axis_is_reverse == container_inline_axis_is_reverse;
    let (contracted_inline_low, contracted_inline_high) = if direction_matches {
        (border_inline_low, border_inline_high)
    } else {
        Default::default()
    };

    let block_start = first.block_start + border_block_low;
    let block_size = (last.block_end - border_block_high - block_start).max(CssPixels::default());
    let (inline_low, inline_size) = if !container_inline_axis_is_reverse {
        let start = first.inline_start + contracted_inline_low;
        let end = last.inline_end - contracted_inline_high;
        (start, (end - start).max(CssPixels::default()))
    } else {
        let start = first.inline_end - contracted_inline_high;
        let end = last.inline_start + contracted_inline_low;
        let size = (start - end).max(CssPixels::default());
        (start - size, size)
    };
    Some(if horizontal {
        formatting_context::PhysicalRect {
            x: inline_low,
            y: block_start,
            width: inline_size,
            height: block_size,
        }
    } else {
        formatting_context::PhysicalRect {
            x: block_start,
            y: inline_low,
            width: block_size,
            height: inline_size,
        }
    })
}
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct InlineAncestorChainRelativeOffset {
    pub(crate) offset_x: crate::layout::CssPixels,
    pub(crate) offset_y: crate::layout::CssPixels,
    pub(crate) found_fragmented_inline_node: bool,
}

/// Accumulates relative-position insets from a chain of inline-flow
/// ancestors, starting at first_ancestor and walking up until stop_at or
/// the first ancestor that is not inline-flow.
pub(crate) fn accumulated_relative_insets_from_inline_ancestor_chain(
    records: &RunRecords,
    callbacks: &FfiLayoutFcCallbacks,
    first_ancestor: Node,
    stop_at: Node,
) -> InlineAncestorChainRelativeOffset {
    let mut result = InlineAncestorChainRelativeOffset::default();
    let mut ancestor = first_ancestor;
    while !ancestor.is_invalid() && ancestor != stop_at {
        let facts = NodeFacts::new(callbacks, ancestor);
        if !facts.has_box_model_metrics() {
            break;
        }
        let display = facts.display();
        if !display.is_inline_outside() || !display.is_flow_inside() {
            break;
        }
        result.found_fragmented_inline_node |= facts.is_fragmented_inline();
        if facts.is_relatively_positioned() {
            // A relatively positioned inline-flow ancestor reachable from a
            // committed fragment or piece was entered by its inline
            // formatting context this pass, which created its used values
            // and resolved its insets.
            let used = records.used_values(ancestor);
            result.offset_x += used.inset_left.get();
            result.offset_y += used.inset_top.get();
        }
        ancestor = callbacks.parent(ancestor);
    }
    result
}

pub(crate) struct InlineFormattingContext<'context> {
    pub(crate) run: &'context FormattingContextRun,
    pub(crate) containing_block: Node,
    pub(crate) layout_mode: LayoutMode,
    pub(crate) input: LayoutInput,
    pub(crate) callbacks: FfiLayoutFcCallbacks,
    pub(crate) parent: &'context block_formatting_context::BlockFormattingContext,
    pub(crate) containing_used_values: std::rc::Rc<UsedValues>,
    pub(crate) fragmented_inlines_in_pre_order: Vec<Node>,
    pub(crate) automatic_content_inline_size: CssPixels,
    pub(crate) min_content_inline_size_from_max_content_layout: Option<CssPixels>,
    pub(crate) automatic_content_block_size: CssPixels,
    block_axis_float_clearance: Cell<CssPixels>,
}

impl<'context> InlineFormattingContext<'context> {
    pub(crate) fn new_with_rust_parent(
        run: &'context FormattingContextRun,
        containing_block: Node,
        layout_mode: LayoutMode,
        input: LayoutInput,
        callbacks: FfiLayoutFcCallbacks,
        parent: &'context block_formatting_context::BlockFormattingContext,
    ) -> Self {
        let containing_used_values = run.records.used_values(containing_block);
        containing_used_values.line_data_cell();
        Self {
            run,
            containing_block,
            layout_mode,
            input,
            callbacks,
            parent,
            containing_used_values,
            fragmented_inlines_in_pre_order: Vec::new(),
            automatic_content_inline_size: CssPixels::default(),
            min_content_inline_size_from_max_content_layout: None,
            automatic_content_block_size: CssPixels::default(),
            block_axis_float_clearance: Cell::new(CssPixels::default()),
        }
    }

    pub(crate) fn block_axis_float_clearance(&self) -> CssPixels {
        self.block_axis_float_clearance.get()
    }

    pub(crate) fn set_block_axis_float_clearance(&self, clearance: CssPixels) {
        self.block_axis_float_clearance.set(clearance);
    }

    fn hide_atomic_inline_for_line_clamp(&self, node: Node) {
        if let Some(fragments) = self.run.fragments.as_deref() {
            fragments.discard_unplaced_subtree(node);
        }
        self.used(node).is_invisible_for_line_clamp.set(true);
    }

    pub(crate) fn prepare_line_for_line_clamp(
        &self,
        line_index: usize,
        has_immediate_continuation: bool,
        line_block_end: CssPixels,
    ) {
        let content_box_position_in_bfc_root = self
            .input
            .content_box_position_in_bfc_root
            .expect("line clamping requires the containing block position in the BFC root");
        let block_offset_adjustment = self
            .parent
            .block_offset_adjustment_from_pending_ancestor_block_start_margins(self.containing_block);
        let line_block_end_in_bfc_root = content_box_position_in_bfc_root.y + block_offset_adjustment + line_block_end;
        let mut line_data = self.line_data_mut();
        let line = &mut line_data.line_boxes[line_index];
        if self.parent.register_line_for_line_clamp(
            self.containing_block,
            line,
            has_immediate_continuation,
            line_block_end_in_bfc_root,
        ) {
            line.trim_trailing_whitespace();
            if self.text_overflow_applies() {
                apply_text_overflow_to_line(line);
            }
            let ellipsis_text = match self.style(self.containing_block).block_ellipsis() {
                crate::css::style_value::StyleValueData::Keyword { keyword: code } if *code == keyword::NO_ELLIPSIS => {
                    None
                }
                crate::css::style_value::StyleValueData::Keyword { keyword: code } if *code == keyword::AUTO => {
                    Some(vec![ELLIPSIS_CODE_POINT as u16])
                }
                crate::css::style_value::StyleValueData::String { string, .. } => Some(
                    crate::css::serialize::with_fly_string_units(string, |units| match units {
                        crate::css::serialize::StringUnits::Ascii(bytes) => {
                            bytes.iter().map(|byte| u16::from(*byte)).collect()
                        }
                        crate::css::serialize::StringUnits::Utf16(code_units) => code_units.to_vec(),
                    }),
                ),
                _ => unreachable!("computed block-ellipsis is no-ellipsis, auto, or a string"),
            };
            if let Some(ellipsis_text) = ellipsis_text.filter(|text| !text.is_empty()) {
                apply_block_ellipsis(line, self, &ellipsis_text);
            }
        }
    }

    pub(crate) fn style(&self, node: Node) -> StyleValues<'static> {
        StyleValues::for_node(&self.callbacks, node)
    }

    pub(crate) fn facts(&self, node: Node) -> NodeFacts<'_> {
        NodeFacts::new(&self.callbacks, node)
    }

    pub(crate) fn style_source(&self, node: Node) -> Node {
        if self.facts(node).is_text_node() {
            self.parent_node(node)
        } else {
            node
        }
    }

    pub(crate) fn line_data(&self) -> Ref<'_, used_values::LineData> {
        Ref::map(self.containing_used_values.line_data_cell().borrow(), |shared| {
            &**shared
        })
    }

    pub(crate) fn line_data_mut(&self) -> RefMut<'_, used_values::LineData> {
        RefMut::map(
            self.containing_used_values.line_data_cell().borrow_mut(),
            std::rc::Rc::make_mut,
        )
    }

    pub(crate) fn containing_used(&self) -> std::rc::Rc<UsedValues> {
        self.containing_used_values.clone()
    }

    #[track_caller]
    pub(crate) fn used(&self, node: Node) -> std::rc::Rc<UsedValues> {
        self.run.records.used_values(node)
    }

    pub(crate) fn create_used_values(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> std::rc::Rc<UsedValues> {
        self.run.records.create_used_values(&self.callbacks, node, constraints)
    }

    pub(crate) fn parent_node(&self, node: Node) -> Node {
        self.callbacks.parent(node)
    }

    pub(crate) fn first_child(&self, node: Node) -> Node {
        self.callbacks.first_child(node)
    }

    pub(crate) fn next_sibling(&self, node: Node) -> Node {
        self.callbacks.next_sibling(node)
    }

    pub(crate) fn nearest_fragmented_inline_ancestor(&self, node: Node) -> Node {
        let mut ancestor = self.parent_node(node);
        while !ancestor.is_invalid() {
            // The containing block bounds this context's subtree even when it
            // is itself inline-outside with flow inside (an inline list-item
            // root): ancestors beyond it belong to the enclosing run.
            if ancestor == self.containing_block {
                break;
            }
            let display = self.style(ancestor).display();
            if !display.is_inline_outside() || !display.is_flow_inside() {
                break;
            }
            let facts = self.facts(ancestor);
            if facts.is_fragmented_inline() && !facts.is_floating_or_absolutely_positioned() {
                return ancestor;
            }
            ancestor = self.parent_node(ancestor);
        }
        NodeSlotId::INVALID
    }

    pub(crate) fn text_may_require_bidi_processing(&self, node: Node) -> bool {
        self.callbacks.text_content(node).may_require_bidi_processing
    }

    pub(crate) fn compute_inset(&self, node: Node) {
        let used = self.containing_used();
        abspos_engine::compute_inset_native(
            self.run,
            node,
            used.content_inline_size.get(),
            used.content_block_size.get(),
        );
    }

    pub(crate) fn parent_commit_pending_margin_before_inline_content(&self) -> CssPixels {
        self.parent.commit_pending_margin_before_inline_content()
    }

    pub(crate) fn intrusion_by_floats_into_containing_block(
        &self,
        block_start: CssPixels,
        block_end: CssPixels,
    ) -> SpaceUsedByFloats {
        let content_box_position_in_bfc_root = self
            .input
            .content_box_position_in_bfc_root
            .expect("float intrusion requires the containing block position in the BFC root");
        let adjustment = self
            .parent
            .block_offset_adjustment_from_pending_ancestor_block_start_margins(self.containing_block);
        let rect = FfiCssPixelRect {
            x: content_box_position_in_bfc_root.x,
            y: content_box_position_in_bfc_root.y + adjustment,
            width: self.containing_used().content_inline_size.get(),
            height: self.containing_used().content_block_size.get(),
        };
        self.parent.intrusion_by_floats_into_rect(rect, block_start, block_end)
    }

    pub(crate) fn leftmost_inline_offset_at(&self, block_offset: CssPixels, line_block_size: CssPixels) -> CssPixels {
        self.intrusion_by_floats_into_containing_block(block_offset, block_offset + line_block_size)
            .left
    }

    pub(crate) fn available_space_for_line(
        &self,
        block_offset: CssPixels,
        line_block_size: CssPixels,
    ) -> AvailableSize {
        if !matches!(self.input.available_space.inline_size, AvailableSize::Definite(_)) {
            return self.input.available_space.inline_size;
        }
        let intrusions = self.intrusion_by_floats_into_containing_block(block_offset, block_offset + line_block_size);
        AvailableSize::definite(
            self.input.available_space.inline_size.to_px_or_zero() - intrusions.left - intrusions.right,
        )
    }

    pub(crate) fn any_floats_intrude_in_block_range(&self, block_start: CssPixels, block_end: CssPixels) -> bool {
        let intrusions = self.intrusion_by_floats_into_containing_block(block_start, block_end);
        intrusions.left > CssPixels::default() || intrusions.right > CssPixels::default()
    }

    pub(crate) fn can_fit_new_line_at_block_offset(&self, block_offset: CssPixels, line_block_size: CssPixels) -> bool {
        if !matches!(self.input.available_space.inline_size, AvailableSize::Definite(_)) {
            return true;
        }
        self.available_space_for_line(block_offset, line_block_size)
            .to_px_or_zero()
            > CssPixels::default()
    }

    pub(crate) fn next_float_band_block_start_after(&self, block_offset: CssPixels) -> Option<CssPixels> {
        let content_box_position_in_bfc_root = self
            .input
            .content_box_position_in_bfc_root
            .expect("float-band lookup requires the containing block position in the BFC root");
        let adjustment = self
            .parent
            .block_offset_adjustment_from_pending_ancestor_block_start_margins(self.containing_block);
        let containing_block_offset_in_root = content_box_position_in_bfc_root.y + adjustment;
        let next = self
            .parent
            .next_float_band_block_start_after(containing_block_offset_in_root + block_offset);
        next.map(|next| next - containing_block_offset_in_root)
    }

    fn layout_inside(&mut self, node: Node, available_space: AvailableSpace) -> DerivedBaselines {
        let input = LayoutInput::new(
            available_space,
            self.input.containing_block_constraints,
            ParticipationInParentFormattingContext::AtomicInline,
        );
        match formatting_context::layout_inside_child(
            self.run,
            Some(self.parent),
            None,
            node,
            self.layout_mode,
            input,
            false,
        ) {
            ChildLayoutOutcome::Created(result) => result.baselines,
            ChildLayoutOutcome::ReenterCurrent => {
                self.parent.run(self.run, input);
                self.used(node).content_baselines_from_cells()
            }
            ChildLayoutOutcome::Skipped => self.used(node).content_baselines_from_cells(),
        }
    }

    pub(crate) fn dimension_box_on_line(&mut self, node: Node) -> DerivedBaselines {
        let available_space = self.input.available_space;
        let facts = self.facts(node);
        // Any fragmented inline box should have generated line box fragments already.
        if facts.is_fragmented_inline() {
            // SAFETY: The callback table and layout node remain live for this
            // synchronous formatting-context run.
            unsafe {
                (self.callbacks.report_unexpected_fragmented_inline)(
                    self.callbacks.context,
                    self.callbacks.shell(node),
                );
            }
            return DerivedBaselines::default();
        }

        let content_baselines = self.layout_inside(node, available_space);
        debug_assert!(
            self.used(node).has_definite_inline_size.get()
                || self.used(node).inline_size_constraint.get() != SizeConstraint::None,
            "atomic inline-level run left its root's inline size unresolved"
        );
        content_baselines
    }

    pub(crate) fn paired_min_content_inline_size_for_atomic_root(&self, node: Node) -> Option<CssPixels> {
        self.parent.sizing().paired_min_content_inline_size_for_atomic_root(
            node,
            self.input.available_space,
            self.input.containing_block_constraints,
        )
    }

    fn clear_floating_boxes(&self, node: Node) -> bool {
        self.parent.clear_floating_boxes(
            node,
            Some(self),
            self.input
                .content_box_position_in_bfc_root
                .expect("clearing floats requires the containing block position in the BFC root"),
        )
    }

    fn reset_parent_margin_state(&self) {
        self.parent.reset_margin_state();
    }

    fn text_overflow_applies(&self) -> bool {
        let mut block = self.containing_block;
        if self.facts(block).is_anonymous() {
            block = self.callbacks.non_anonymous_containing_block(block);
        }
        if block.is_invalid() {
            return false;
        }
        let style = self.style(block);
        style.text_overflow() == text_overflow::ELLIPSIS && style.overflow_x() != overflow::VISIBLE
    }

    fn reusable_atomic_line_prefix(
        &self,
        previous: &used_values::LineData,
        iterator: &inline_level_iterator::InlineLevelIterator,
    ) -> (Vec<line_box::LineBoxData>, usize) {
        if self.containing_block != self.run.box_
            || !previous.inline_box_pieces.is_empty()
            || previous
                .line_boxes
                .iter()
                .any(|line| line.writing_mode != writing_mode::HORIZONTAL_TB)
            || iterator
                .items()
                .iter()
                .any(|item| item.type_ != inline_level_iterator::ItemType::Element)
        {
            return (Vec::new(), 0);
        }

        let mut item_index = 0usize;
        let mut reused_lines = Vec::new();
        let mut item_count_before_last_line = 0usize;
        for line in &previous.line_boxes {
            if line.fragments.is_empty()
                || line.has_block_level_box
                || !line.static_position_markers.is_empty()
                || !line.inline_box_baselines.is_empty()
            {
                break;
            }
            let line_item_start = item_index;
            let mut running_inline_length = CssPixels::default();
            let mut matched = true;
            for fragment in &line.fragments {
                let Some(item) = iterator.items().get(item_index) else {
                    matched = false;
                    break;
                };
                let used = self.used(item.node);
                let expected_inline_offset =
                    running_inline_length + item.margin_start + item.border_start + item.padding_start;
                if !fragment.is_atomic_inline
                    || fragment.layout_node != item.node
                    || fragment.inline_offset != expected_inline_offset
                    || fragment.inline_length != item.inline_size
                    || fragment.block_length != used.content_block_size.get()
                    || fragment.border_box_block_start != used.border_box_top(false)
                {
                    matched = false;
                    break;
                }
                running_inline_length += item.margin_start
                    + item.border_start
                    + item.padding_start
                    + item.inline_size
                    + item.padding_end
                    + item.border_end
                    + item.margin_end;
                item_index += 1;
            }
            if !matched || running_inline_length != line.inline_length {
                item_index = line_item_start;
                break;
            }
            item_count_before_last_line = line_item_start;
            reused_lines.push(line.clone());
        }

        // Additional content can fit on the old final line, so that line is
        // damaged even when every old fragment before the insertion matches.
        if item_index < iterator.items().len() && reused_lines.len() == previous.line_boxes.len() {
            reused_lines.pop();
            item_index = item_count_before_last_line;
        }
        (reused_lines, item_index)
    }

    fn min_content_inline_size_from_max_content_items(
        &self,
        items: &[inline_level_iterator::Item],
    ) -> Option<CssPixels> {
        if self.input.available_space.inline_size != AvailableSize::MaxContent {
            return None;
        }
        if self.facts(self.containing_block).is_scroll_container() {
            return None;
        }
        if self.style(self.containing_block).writing_mode() != writing_mode::HORIZONTAL_TB {
            return None;
        }

        let containing_style = self.style(self.containing_block);
        let containing_inline_size = self.input.containing_block_constraints.inline_basis();
        if containing_style.text_indent().to_px(containing_inline_size) != CssPixels::default() {
            return None;
        }

        let wraps = containing_style.text_wrap_mode() == text_wrap_mode::WRAP;
        let mut maximum = CssPixels::default();
        let mut current = CssPixels::default();
        let mut line_has_content = false;
        let finish_line = |maximum: &mut CssPixels, current: &mut CssPixels, line_has_content: &mut bool| {
            *maximum = (*maximum).max(*current);
            *current = CssPixels::default();
            *line_has_content = false;
        };
        for item in items {
            match item.type_ {
                inline_level_iterator::ItemType::Element => {
                    if item.has_box_model_metrics() {
                        return None;
                    }
                    if wraps && line_has_content {
                        finish_line(&mut maximum, &mut current, &mut line_has_content);
                    }
                    current += item.min_content_inline_size?;
                    line_has_content = true;
                }
                inline_level_iterator::ItemType::Text => {
                    if item.has_box_model_metrics() || item.contains_tab(self) {
                        return None;
                    }
                    if item.length_in_node == 0 && item.inline_size == CssPixels::default() {
                        continue;
                    }
                    let parent_style = self.style(self.parent_node(item.node));
                    let wraps = parent_style.text_wrap_mode() == text_wrap_mode::WRAP;
                    if wraps && breaks_between_graphemes(parent_style) {
                        return None;
                    }
                    if !wraps {
                        current += item.inline_size;
                        line_has_content = true;
                        continue;
                    }
                    if item.is_ascii_whitespace(self) {
                        if !item.is_collapsible_whitespace {
                            return None;
                        }
                        if line_has_content {
                            finish_line(&mut maximum, &mut current, &mut line_has_content);
                        }
                        continue;
                    }
                    if item.trailing_whitespace.inline_size != CssPixels::default() {
                        return None;
                    }
                    if item.can_break_before && line_has_content {
                        finish_line(&mut maximum, &mut current, &mut line_has_content);
                    }
                    current += item.inline_size;
                    line_has_content = true;
                }
                inline_level_iterator::ItemType::ForcedBreak => {
                    finish_line(&mut maximum, &mut current, &mut line_has_content);
                }
                inline_level_iterator::ItemType::BlockLevelBox
                | inline_level_iterator::ItemType::AbsolutelyPositionedElement
                | inline_level_iterator::ItemType::FloatingElement => {
                    return None;
                }
            }
        }
        finish_line(&mut maximum, &mut current, &mut line_has_content);
        Some(maximum)
    }

    pub(crate) fn overflow_break_applies(&self, text_node: Node) -> bool {
        self.overflow_break_applies_to_style(self.style(self.parent_node(text_node)))
    }

    pub(crate) fn overflow_break_applies_to_style(&self, style: StyleValues<'_>) -> bool {
        if style.text_wrap_mode() != text_wrap_mode::WRAP {
            return false;
        }
        breaks_between_graphemes(style)
            || (style.overflow_wrap() == overflow_wrap::BREAK_WORD
                && self.input.available_space.inline_size != AvailableSize::MinContent)
    }

    fn break_overflowing_text_item(
        &self,
        line_builder: &mut line_builder::LineBuilder<'_, '_>,
        item: &mut inline_level_iterator::Item,
    ) {
        if line_builder
            .remaining_inline_size_for_overflow_break()
            .is_none_or(|remaining_inline_size| item.border_box_inline_size() <= remaining_inline_size)
        {
            return;
        }
        let style = self.style(self.parent_node(item.node));
        let text_content = self.callbacks.text_content(item.node);
        let letter_spacing = style.letter_spacing().to_double() as f32;
        let word_spacing = style.word_spacing().to_double() as f32;
        loop {
            let Some(remaining_inline_size) = line_builder.remaining_inline_size_for_overflow_break() else {
                return;
            };
            if item.border_box_inline_size() <= remaining_inline_size {
                return;
            }
            let line_is_empty = self.line_data().line_boxes.last().is_some_and(|line| line.is_empty());
            if line_is_empty && line_builder.current_line_has_no_space_left_by_floats() {
                line_builder.break_line(line_builder::ForcedBreak::No, None);
                continue;
            }
            let split_item = item.split_for_overflow_break(
                &text_content.text,
                text_content.grapheme_segmenter(),
                letter_spacing,
                word_spacing,
                remaining_inline_size,
                line_is_empty,
            );
            let Some(mut prefix) = split_item else {
                if line_is_empty {
                    return;
                }
                line_builder.break_line(line_builder::ForcedBreak::No, Some(item.border_box_inline_size()));
                continue;
            };
            line_builder.append_text_item(&mut prefix, style.line_height());
            line_builder.break_line(line_builder::ForcedBreak::No, None);
        }
    }

    pub(crate) fn generate_line_boxes(&mut self) {
        let mut iterator = inline_level_iterator::InlineLevelIterator::new(self);
        self.min_content_inline_size_from_max_content_layout =
            self.min_content_inline_size_from_max_content_items(iterator.items());
        self.fragmented_inlines_in_pre_order = iterator.take_visited_fragmented_inlines();
        let (reused_lines, reused_item_count) = if self.parent.has_line_clamp() {
            Default::default()
        } else {
            self.run
                .previous_line_data
                .as_deref()
                .map(|previous| self.reusable_atomic_line_prefix(previous, &iterator))
                .unwrap_or_default()
        };
        {
            let mut data = self.line_data_mut();
            data.line_boxes = reused_lines;
            data.inline_box_pieces.clear();
        }
        iterator.skip_items(reused_item_count);
        let reused_line_count = self.line_data().line_boxes.len();
        let mut line_builder = if reused_line_count == 0 {
            line_builder::LineBuilder::new(self)
        } else {
            line_builder::LineBuilder::new_after_reused_lines(self)
        };

        let mut leading_margin = CssPixels::default();
        let mut leading_border = CssPixels::default();
        let mut leading_padding = CssPixels::default();
        let mut absolute_boxes = Vec::new();

        let mut previous_text_item_allows_overflow_break_after = false;
        while !self.parent.line_clamp_reached() {
            let Some(mut item) = iterator.next() else {
                break;
            };
            let can_break_after_previous_overflow_item =
                std::mem::take(&mut previous_text_item_allows_overflow_break_after);
            let line_starts_with_whitespace = self
                .line_data()
                .line_boxes
                .last()
                .is_none_or(|line| line.is_empty_or_ends_in_whitespace() || line.has_block_level_box);
            if item.is_collapsible_whitespace && line_starts_with_whitespace {
                if self.style(self.style_source(item.node)).text_wrap_mode() == text_wrap_mode::WRAP {
                    let next_inline_size = iterator.next_inline_run_size(self).unwrap_or_default();
                    if next_inline_size > CssPixels::default() {
                        line_builder.prepare_to_append_inline_content();
                        line_builder.break_if_needed(next_inline_size);
                    }
                }
                leading_margin += item.margin_start;
                leading_border += item.border_start;
                leading_padding += item.padding_start;
                continue;
            }

            item.margin_start += leading_margin;
            item.border_start += leading_border;
            item.padding_start += leading_padding;
            leading_margin = CssPixels::default();
            leading_border = CssPixels::default();
            leading_padding = CssPixels::default();

            match item.type_ {
                inline_level_iterator::ItemType::ForcedBreak => {
                    let continuation = iterator.next_inline_run_size(self);
                    line_builder.break_line(line_builder::ForcedBreak::Yes, continuation);
                    if !item.node.is_invalid() && self.clear_floating_boxes(item.node) {
                        line_builder.did_introduce_clearance(self.block_axis_float_clearance.get());
                        self.reset_parent_margin_state();
                    }
                }
                inline_level_iterator::ItemType::Element => {
                    line_builder.prepare_to_append_inline_content();
                    self.compute_inset(item.node);
                    if self.style(self.containing_block).text_wrap_mode() == text_wrap_mode::WRAP {
                        let mut minimum = item.border_box_inline_size();
                        if item.margin_start < CssPixels::default() {
                            minimum += item.margin_start;
                        }
                        if item.margin_end < CssPixels::default() {
                            minimum += item.margin_end;
                        }
                        line_builder.break_if_needed(minimum);
                        line_builder.note_soft_wrap_opportunity();
                    }
                    if self.parent.line_clamp_reached() {
                        break;
                    }
                    line_builder.append_box(
                        item.node,
                        item.border_start + item.padding_start,
                        item.padding_end + item.border_end,
                        item.margin_start,
                        item.margin_end,
                        item.content_baselines,
                    );
                }
                inline_level_iterator::ItemType::BlockLevelBox => {
                    leading_margin += item.margin_start;
                    leading_border += item.border_start;
                    leading_padding += item.padding_start;
                    line_builder.finish_current_line_before_block_level_box();
                    if self.parent.line_clamp_reached() {
                        continue;
                    }
                    self.parent.layout_interrupting_block_inside_inline_context(
                        self.run,
                        item.node,
                        self.containing_block,
                        self.input,
                        &mut line_builder,
                    );
                }
                inline_level_iterator::ItemType::AbsolutelyPositionedElement => {
                    if !self.facts(item.node).is_box() {
                        continue;
                    }
                    let preceded = item.preceded_by_unattached_inline_start_edges
                        || item.margin_start != CssPixels::default()
                        || item.border_start != CssPixels::default()
                        || item.padding_start != CssPixels::default();
                    line_builder.append_static_position_marker(item.node, preceded);
                    absolute_boxes.push(item.node);
                }
                inline_level_iterator::ItemType::FloatingElement => {
                    line_builder.commit_pending_margin_before_float();
                    self.create_used_values(item.node, self.input.containing_block_constraints);
                    self.clear_floating_boxes(item.node);
                    line_builder.set_unbreakable_run_inline_size_interrupted_by_float(
                        iterator.next_unbreakable_run_inline_size(self),
                    );
                    self.parent.layout_floating_box(
                        self.run,
                        item.node,
                        self.input,
                        CssPixels::default(),
                        Some(&mut line_builder),
                    );
                }
                inline_level_iterator::ItemType::Text => {
                    line_builder.prepare_to_append_inline_content();
                    let style = self.style(self.parent_node(item.node));
                    if style.text_wrap_mode() == text_wrap_mode::WRAP {
                        let is_whitespace = item.is_collapsible_whitespace || item.is_ascii_whitespace(self);
                        let item_inline_size = item.border_box_inline_size();
                        let next_inline_size = if is_whitespace {
                            iterator.next_inline_run_size(self).unwrap_or_default()
                        } else {
                            CssPixels::default()
                        };
                        if is_whitespace && next_inline_size > CssPixels::default() {
                            let sequence_inline_size = item_inline_size + next_inline_size;
                            let broke_line = if iterator.next_non_whitespace_text_allows_overflow_break(self) {
                                line_builder.break_if_needed_before_overflow_breakable_item(sequence_inline_size)
                            } else {
                                line_builder.break_if_needed(sequence_inline_size)
                            };
                            if broke_line {
                                line_builder.set_trailing_whitespace_on_previous_line();
                                continue;
                            }
                        }
                        let overflow_break_allowed = !is_whitespace && self.overflow_break_applies_to_style(style);
                        let line_is_empty_or_ends_in_whitespace = self
                            .line_data()
                            .line_boxes
                            .last()
                            .is_some_and(line_box::LineBoxData::is_empty_or_ends_in_whitespace);
                        let can_break_before_item = item.can_break_before
                            || (can_break_after_previous_overflow_item && !overflow_break_allowed)
                            || line_is_empty_or_ends_in_whitespace;
                        if !is_whitespace && can_break_before_item {
                            if overflow_break_allowed {
                                line_builder.break_if_needed_before_overflow_breakable_item(item_inline_size);
                            } else {
                                line_builder.break_if_needed(item_inline_size);
                            }
                            line_builder.note_soft_wrap_opportunity();
                        }
                        if overflow_break_allowed {
                            self.break_overflowing_text_item(&mut line_builder, &mut item);
                            previous_text_item_allows_overflow_break_after = true;
                        }
                    }
                    if self.parent.line_clamp_reached() {
                        break;
                    }
                    line_builder.append_text_item(&mut item, style.line_height());
                }
            }
        }

        if self.parent.line_clamp_reached() {
            for item in iterator.items() {
                if item.type_ == inline_level_iterator::ItemType::Element {
                    self.hide_atomic_inline_for_line_clamp(item.node);
                } else if item.type_ == inline_level_iterator::ItemType::AbsolutelyPositionedElement
                    && self.facts(item.node).is_box()
                    && self.line_data().line_boxes.iter().any(|line| {
                        line.visible_fragments().any(|fragment| {
                            self.callbacks
                                .is_ancestor(self.callbacks.parent(item.node), fragment.layout_node)
                        })
                    })
                {
                    line_builder.append_static_position_marker(item.node, false);
                    absolute_boxes.push(item.node);
                }
            }
        }
        if self.parent.has_line_clamp() {
            line_builder.update_last_line(false);
        }
        let line_count = self.line_data().line_boxes.len();
        for line_index in reused_line_count..line_count {
            if self.line_data().line_boxes[line_index]
                .inline_length_before_block_ellipsis
                .is_none()
            {
                self.line_data_mut().line_boxes[line_index].trim_trailing_whitespace();
            }
        }
        if self.text_overflow_applies() {
            for line_index in reused_line_count..line_count {
                if self.line_data().line_boxes[line_index]
                    .inline_length_before_block_ellipsis
                    .is_none()
                {
                    apply_text_overflow_to_line(&mut self.line_data_mut().line_boxes[line_index]);
                }
            }
        }
        let containing_style = self.style(self.containing_block);
        if containing_style.text_align() == text_align::JUSTIFY {
            let line_count = self.line_data().line_boxes.len();
            for index in 0..line_count {
                let is_last_line = index + 1 == line_count
                    && self.line_data().line_boxes[index]
                        .inline_length_before_block_ellipsis
                        .is_none();
                apply_to_fragments(
                    containing_style.text_justify(),
                    &mut self.line_data_mut().line_boxes[index],
                    is_last_line,
                );
            }
        }
        if !self.parent.has_line_clamp() {
            line_builder.update_last_line(false);
        }

        for line_index in 0..self.line_data().line_boxes.len() {
            if self.line_data().line_boxes[line_index].has_block_level_box {
                continue;
            }
            let fragment_count = self.line_data().line_boxes[line_index].fragments.len();
            for fragment_index in 0..fragment_count {
                let fragment = &self.line_data().line_boxes[line_index].fragments[fragment_index];
                if !fragment.is_atomic_inline {
                    continue;
                }
                let (x, y) = fragment.offset();
                formatting_context::place_child(
                    self.run,
                    fragment.layout_node,
                    FfiCssPixelPoint { x, y },
                    Some(used_values::LineBoxFragmentCoordinate {
                        line_box_index: line_index,
                        fragment_index,
                    }),
                );
            }
        }

        if self.layout_mode == LayoutMode::Normal {
            for box_ in absolute_boxes {
                let mut static_position = abspos_inputs::StaticPositionRect {
                    rect: Default::default(),
                    inline_alignment: StaticPositionAlignment::Start,
                    block_alignment: StaticPositionAlignment::Start,
                    alignment_derives_from_own_computed_values: false,
                };
                'lines: for line in &self.line_data().line_boxes {
                    for marker in &line.static_position_markers {
                        if marker.box_ != box_ {
                            continue;
                        }
                        if self
                            .facts(box_)
                            .display_before_box_type_transformation_is_block_outside()
                        {
                            let block_position = if marker.preceded_by_in_flow_content {
                                line.physical_vertical_end()
                            } else {
                                marker.offset().1
                            };
                            let inline_size = self.input.containing_block_constraints.inline_basis();
                            static_position.rect.offset.inline_offset = CssPixels::default();
                            static_position.rect.offset.block_offset = block_position;
                            static_position.rect.size.inline_size = inline_size;
                        } else {
                            let (x, y) = marker.offset();
                            static_position.rect.offset.inline_offset = x;
                            static_position.rect.offset.block_offset = y;
                        }
                        if containing_style.direction() == direction::RTL {
                            static_position.inline_alignment = StaticPositionAlignment::End;
                        }
                        break 'lines;
                    }
                }
                formatting_context::register_contained_abspos_child(
                    &self.callbacks,
                    self.run.fragments.as_deref(),
                    self.containing_block,
                    box_,
                    static_position,
                    None,
                );
            }
        }
        line_builder.remove_last_line_if_empty();
    }

    pub(crate) fn run(&mut self) {
        assert!(self.facts(self.containing_block).children_are_inline());
        self.generate_line_boxes();
        if self.layout_mode == LayoutMode::Normal && !self.run.purpose.is_measurement() {
            self.compute_inline_box_pieces();
            self.fold_inline_ancestor_relative_insets_into_line_data();
        }
        self.automatic_content_block_size = {
            let data = self.line_data();
            let lines = &data.line_boxes;
            if lines.iter().any(|line| line.has_block_level_box) {
                lines
                    .last()
                    .map_or(CssPixels::default(), line_box::LineBoxData::physical_vertical_end)
            } else {
                lines
                    .iter()
                    .fold(CssPixels::default(), |sum, line| sum + line.physical_vertical_extent())
            }
        };
        self.automatic_content_inline_size = self
            .parent
            .greatest_child_inline_size_including_floats(self.containing_block);
        let baselines =
            formatting_context::derive_baselines(&self.run.records, &self.callbacks, self.containing_block, false);
        if self.containing_block == self.parent.root_box() {
            self.parent.record_derived_baselines_of_root_box(baselines);
        } else {
            formatting_context::store_derived_baselines(&self.used(self.containing_block), baselines);
        }
    }

    fn compute_inline_box_pieces(&mut self) {
        let (pieces, inline_containing_block_rect_candidates) = compute(self);
        self.line_data_mut().inline_box_pieces = pieces;
        for candidate in inline_containing_block_rect_candidates {
            let relative_inset_chain = accumulated_relative_insets_from_inline_ancestor_chain(
                &self.run.records,
                &self.callbacks,
                candidate.inline_containing_block,
                self.containing_block,
            );
            let mut rect = candidate.rect;
            rect.x += relative_inset_chain.offset_x;
            rect.y += relative_inset_chain.offset_y;
            if let Some(fragments) = self.run.fragments.as_deref() {
                fragments.register_inline_containing_block_rect(
                    candidate.inline_containing_block,
                    rect,
                    self.containing_block,
                );
            }
        }
    }

    /// Runs after all line post-processing and after atomic-inline placement,
    /// so everything that reads static offsets has already read them.
    fn fold_inline_ancestor_relative_insets_into_line_data(&self) {
        let mut data = self.line_data_mut();
        // Every inline box that parents fragments gets a piece, so no pieces
        // means no fragment has an inline ancestor and every chain is empty.
        if data.inline_box_pieces.is_empty() {
            return;
        }
        let mut accumulated_relative_offset_by_chain_start = HashMap::<Node, FfiCssPixelPoint>::default();
        let mut accumulated_relative_offset_from = |first_ancestor: Node| -> FfiCssPixelPoint {
            *accumulated_relative_offset_by_chain_start
                .entry(first_ancestor)
                .or_insert_with(|| {
                    let chain = accumulated_relative_insets_from_inline_ancestor_chain(
                        &self.run.records,
                        &self.callbacks,
                        first_ancestor,
                        self.containing_block,
                    );
                    FfiCssPixelPoint {
                        x: chain.offset_x,
                        y: chain.offset_y,
                    }
                })
        };
        for line in &mut data.line_boxes {
            for fragment in &mut line.fragments {
                if fragment.is_fully_truncated {
                    continue;
                }
                fragment.relpos_delta = accumulated_relative_offset_from(self.callbacks.parent(fragment.layout_node));
            }
        }
        for piece in &mut data.inline_box_pieces {
            piece.relpos_delta = accumulated_relative_offset_from(piece.node);
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct SpaceUsedByFloats {
    pub left: CssPixels,
    pub right: CssPixels,
}

fn breaks_between_graphemes(style: StyleValues<'_>) -> bool {
    style.overflow_wrap() == overflow_wrap::ANYWHERE || style.word_break() == word_break::BREAK_WORD
}

pub(crate) fn line_physical_horizontal_extent(line: &line_box::LineBoxData) -> CssPixels {
    if line.has_block_level_box || line.writing_mode == writing_mode::HORIZONTAL_TB {
        if let Some(ellipsis) = line.fragments.last().filter(|fragment| fragment.is_block_ellipsis) {
            return line.inline_length - ellipsis.inline_length;
        }
        return line.inline_length;
    }
    let Some(first) = line.fragments.first() else {
        return CssPixels::default();
    };
    let mut left = first.offset().0;
    let mut right = left + first.physical_horizontal_extent();
    for fragment in &line.fragments[1..] {
        let fragment_left = fragment.offset().0;
        left = left.min(fragment_left);
        right = right.max(fragment_left + fragment.physical_horizontal_extent());
    }
    right - left
}

pub(crate) fn line_rect(line: &line_box::LineBoxData, content_inline_size: CssPixels) -> FfiCssPixelRect {
    let mut rect = None::<FfiCssPixelRect>;
    let mut include = |x, y, width, height| {
        if width <= CssPixels::default() || height <= CssPixels::default() {
            return;
        }
        rect = Some(if let Some(rect) = rect {
            let right = (rect.x + rect.width).max(x + width);
            let bottom = (rect.y + rect.height).max(y + height);
            let x = rect.x.min(x);
            let y = rect.y.min(y);
            FfiCssPixelRect {
                x,
                y,
                width: right - x,
                height: bottom - y,
            }
        } else {
            FfiCssPixelRect { x, y, width, height }
        });
    };
    for fragment in line.visible_fragments() {
        let (x, y) = fragment.offset();
        let (width, height) = fragment.size();
        include(x, y, width, height);
    }
    let Some(mut rect) = rect else {
        return FfiCssPixelRect {
            x: CssPixels::default(),
            y: line.physical_vertical_end() - line.physical_vertical_extent(),
            width: content_inline_size,
            height: line.physical_vertical_extent(),
        };
    };
    if line.writing_mode == writing_mode::HORIZONTAL_TB {
        rect.y = line.physical_vertical_end() - line.physical_vertical_extent();
        rect.height = line.physical_vertical_extent();
    }
    rect
}
