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
        fragment.glyphs.as_mut().unwrap().glyphs.to_mut().pop();
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
                fragment.record.inline_length - CssPixels::nearest_value_for_f32(glyph.x) > available
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
    let kept_glyphs = glyphs.glyphs.to_mut();
    kept_glyphs.drain(end..);
    kept_glyphs.drain(..start);
    for glyph in kept_glyphs.iter_mut() {
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
        let font = glyph_data.font.clone();
        let ellipsis_inline_size = font.glyph_width(ELLIPSIS_CODE_POINT);
        let available_in_fragment = (available_inline_size - fragment_start).raw_value() as f32 / 64.0;
        let max_text_inline_size = available_in_fragment - ellipsis_inline_size;

        let glyphs = &line.fragments[index].glyphs.as_ref().unwrap().glyphs;
        let mut keep_count = 0usize;
        let mut last_kept_end = 0.0f32;
        let mut glyph_block_offset = 0.0f32;
        for glyph in glyphs.iter() {
            let glyph_end = glyph.x + glyph.glyph_width;
            if glyph_end > max_text_inline_size && (keep_count > 0 || line_has_visible_content) {
                break;
            }
            keep_count += 1;
            last_kept_end = glyph_end;
            glyph_block_offset = glyph.y;
        }

        let glyph_data = line.fragments[index].glyphs.as_mut().unwrap();
        let kept_glyphs = glyph_data.glyphs.to_mut();
        kept_glyphs.truncate(keep_count);
        kept_glyphs.push(libgfx_rust::text_layout::DrawGlyph {
            x: last_kept_end,
            y: glyph_block_offset,
            length_in_code_units: 1,
            glyph_width: ellipsis_inline_size,
            glyph_id: font.glyph_id_for_code_point(ELLIPSIS_CODE_POINT),
            cluster_start: 0,
            should_paint: true,
            is_rtl: false,
        });
        let ellipsis_fragment_inline_length = line.fragments[index].inline_length;
        line.fragments[index].inline_length = CssPixels::nearest_value_for_f32(last_kept_end + ellipsis_inline_size);
        line.fragments[index].has_text_overflow_ellipsis = true;
        line.text_overflow_elision = Some(line_box::TextOverflowElision {
            inline_length: line.inline_length,
            ellipsis_fragment_start: fragment_start,
            ellipsis_fragment_inline_length,
        });
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
    let font = style
        .frozen_font_list()
        .font_for_code_point(first_code_point, presentation);
    let shaped_ellipsis = libgfx_rust::text_layout::shape_text(
        &font,
        ellipsis_text,
        libgfx_rust::text_layout::TextType::Common,
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
                glyphs.glyphs.to_mut().retain(|glyph| {
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
            is_upright: false,
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
    line.push_fragment(ellipsis);
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

pub(crate) use inline_content::InlineBoxPieceRecord as InlineBoxPieceData;

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

fn edge_bits(low: bool, high: bool) -> u8 {
    let mut edges = EDGE_TOP | EDGE_BOTTOM;
    if low {
        edges |= EDGE_LEFT;
    }
    if high {
        edges |= EDGE_RIGHT;
    }
    edges
}

pub(crate) struct InlineContainingBlockRectCandidate {
    pub(crate) inline_containing_block: Node,
    pub(crate) rect: CssPixelRect,
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
            // Block-level here means what the line builder interrupted the line for, which includes
            // boxes whose outer display is not `block` (e.g. `run-in`, laid out as a block).
            debug_assert!(
                !interrupting || context.facts(fragment.layout_node).is_inline_flow_interrupting_block(),
                "an interrupting line's fragment must be a block-level box"
            );
            let position = fragment.offset();
            let size = fragment.size();
            let mut inline_start = position.0;
            let mut inline_end = inline_start + size.0;
            let block_start = position.1;
            let block_length = size.1;

            if !interrupting && fragment.is_atomic_inline {
                let atomic_inline = fragment.box_cells();
                inline_start -= atomic_inline.margin_left + atomic_inline.border_box_left(false);
                inline_end += atomic_inline.margin_right + atomic_inline.border_box_right(false);
            }

            let interrupting_block = if interrupting {
                let block = line
                    .block_level_box
                    .expect("a line interrupted for a block-level box keeps the box")
                    .cells;
                let collapsed = block.uses_collapsing_borders_model;
                let (border_box_inline_low, border_box_block_low, border_box_inline_size, border_box_block_size) = (
                    block.border_box_left(collapsed),
                    block.border_box_top(collapsed),
                    block.border_box_inline_size(collapsed),
                    block.border_box_block_size(collapsed),
                );
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
        let sides = context.sides_of_entered_inline_box(node);
        let reversed = context.facts(node).inline_axis_is_reverse();
        let (
            border_padding_low,
            border_padding_high,
            border_padding_block_low,
            border_padding_block_high,
            margin_low,
            margin_high,
        ) = (
            sides.border_box_left(false),
            sides.border_box_right(false),
            sides.border_box_top(false),
            sides.border_box_bottom(false),
            sides.margin_left,
            sides.margin_right,
        );

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
                            border_box_rect: FfiCssPixelRect {
                                x: position.0,
                                y: position.1,
                                ..Default::default()
                            },
                            relpos_delta: FfiCssPixelPoint::default(),
                            baseline: inline_box_baseline,
                            accumulated_vertical_shift: inline_box_vertical_shift,
                            present_edges: edge_bits(true, true),
                            is_geometry_only_placeholder: true,
                        },
                        depth,
                        discovery_index: node_index,
                    });
                }
                continue;
            };
            let first = first_line_with_content == Some(line.line_index)
                && !context.inline_boxes_continued_from_previous_fragment.contains(&node);
            let last = last_line_with_content == Some(line.line_index)
                && !context.inline_boxes_continued_in_next_fragment.borrow().contains(&node);
            let has_low_edge = if reversed { last } else { first };
            let has_high_edge = if reversed { first } else { last };
            let (inline_box_baseline, inline_box_vertical_shift) = inline_box_anchor();
            // https://www.w3.org/TR/CSS22/visudet.html#inline-non-replaced
            // The height of the content area should be based on the font, but this specification does not specify how.
            let (content_block_start, content_block_length) =
                if let Some(block_start) = line.first_direct_fragment_block_start {
                    (block_start, line.max_direct_fragment_block_length)
                } else {
                    let style = context.style(node);
                    let height = line_builder::normal_line_height(style);
                    let baseline = line_builder::LineBuilder::baseline_for_style(style, height);
                    // NB: Line-relative alignment and atomic-only descendants can leave this box without a
                    //     recorded baseline. In that case, retain the position of its aligned contributions.
                    let block_start = if context.line_data().line_boxes[line.line_index]
                        .inline_box_baseline(node)
                        .is_some()
                    {
                        inline_box_baseline - baseline
                    } else {
                        line.fallback_block_start_from_contributions.unwrap_or_default()
                    };
                    (block_start, height)
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
            let rect = FfiCssPixelRect {
                x: border_inline_start,
                y: border_block_start,
                width: border_inline_end - border_inline_start,
                height: border_block_length,
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
                    present_edges: edge_bits(has_low_edge, has_high_edge),
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
                &sides,
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
        assert!(
            context.entered_inline_box(node).is_some(),
            "a fragmented inline without fragments is an inline box the context entered"
        );
        let line_height = context.style(node).line_height();
        let placeholder_rect = FfiCssPixelRect {
            height: line_height,
            ..Default::default()
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
                rect: CssPixelRect {
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
                present_edges: edge_bits(true, true),
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
    sides: &used_values::RootInputCells,
    inline_axis_is_reverse: bool,
    container_inline_axis_is_reverse: bool,
) -> Option<CssPixelRect> {
    let first = corners.first?;
    let last = corners.last?;

    let (border_inline_low, border_inline_high, border_block_low, border_block_high) = (
        sides.border_left,
        sides.border_right,
        sides.border_top,
        sides.border_bottom,
    );
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
    Some(CssPixelRect {
        x: inline_low,
        y: block_start,
        width: inline_size,
        height: block_size,
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
/// the first ancestor that is not inline-flow. `used_insets_of` gives the
/// used insets of a relatively positioned ancestor in the chain.
pub(crate) fn accumulated_relative_insets_from_inline_ancestor_chain(
    callbacks: &LayoutPass<'_>,
    first_ancestor: Node,
    stop_at: Node,
    used_insets_of: impl Fn(Node) -> run_state::UsedInsets,
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
            // formatting context this pass, which resolved its insets.
            let insets = used_insets_of(ancestor);
            result.offset_x += insets.left;
            result.offset_y += insets.top;
        }
        ancestor = callbacks.parent(ancestor);
    }
    result
}

struct FragmentainerLineLimit {
    block_size: CssPixels,
    widows: usize,
    checked_line_count: usize,
    lines_past_end: usize,
    minimum_line_block_size: Option<CssPixels>,
}

impl FragmentainerLineLimit {
    fn is_reached(&mut self, context: &InlineFormattingContext<'_>) -> bool {
        let data = context.line_data();
        let finished_line_count = data.line_boxes.len().saturating_sub(1);
        if finished_line_count <= self.checked_line_count {
            return false;
        }
        let root_block_offset = context.content_box_position_in_bfc_root().y;
        for line in data.line_boxes[self.checked_line_count..finished_line_count]
            .iter()
            .filter(|line| !line.is_empty())
        {
            let line_block_size = line.block_end - line.block_start;
            self.minimum_line_block_size = Some(
                self.minimum_line_block_size
                    .map_or(line_block_size, |minimum| minimum.min(line_block_size)),
            );
            if root_block_offset + line.block_end > self.block_size {
                self.lines_past_end += 1;
            }
        }
        self.checked_line_count = finished_line_count;
        let lines_per_fragmentainer = self
            .minimum_line_block_size
            .filter(|minimum| *minimum > CssPixels::default())
            .map_or(usize::MAX, |minimum| {
                (self.block_size.to_double() / minimum.to_double()) as usize + 1
            });
        self.lines_past_end > self.widows.min(lines_per_fragmentainer)
    }
}

pub(crate) struct InlineFormattingContext<'context> {
    pub(crate) run: &'context FormattingContextRun<'context>,
    pub(crate) containing_block: Node,
    /// The containing block, with the cells its lines are laid out against.
    block_container: block_formatting_context::BlockContainer,
    pub(crate) input: LayoutInput,
    containing_block_position_in_bfc_root: FfiCssPixelPoint,
    pub(crate) callbacks: LayoutPass<'context>,
    pub(crate) parent: &'context block_formatting_context::BlockFormattingContext<'context>,
    /// The lines this context lays out, which it hands back once they are laid out.
    line_data: RefCell<used_values::LineData>,
    pub(crate) fragmented_inlines_in_pre_order: Vec<Node>,
    pub(crate) current_line_start: Cell<fragmentation::LinePosition>,
    inline_boxes_continued_from_previous_fragment: Vec<Node>,
    inline_boxes_continued_in_next_fragment: RefCell<Vec<Node>>,
    block_axis_float_clearance: Cell<CssPixels>,
    /// What the layout of each atomic inline hands its placement, which places it once its line is final.
    atomic_inline_fragment_arrivals: RefCell<HashMap<Node, fragment_tree::ChildFragmentArrival>>,
    /// What placing each atomic inline this context laid out takes.
    placed_atomic_inlines: RefCell<HashMap<Node, formatting_context::PlacedChild>>,
    /// The break token of the containing block, once the fragmentainer stops before one of its lines.
    fragmentainer_stop_token: RefCell<Option<std::rc::Rc<fragmentation::BlockBreakToken>>>,
    lines_are_unbreakable: Cell<bool>,
    /// Whether margins collapse through each block-level box this context interrupted its lines for, as the layout of
    /// the box found.
    margins_collapse_through_interrupting_blocks: RefCell<HashMap<Node, bool>>,
    /// Each inline box this context entered, as it resolved the box on entering it.
    entered_inline_boxes: RefCell<HashMap<Node, EnteredInlineBox>>,
    /// The used insets of each float this context laid out among its lines, as the float's layout resolved them.
    used_insets_of_floats: RefCell<HashMap<Node, run_state::UsedInsets>>,
    /// The floats among the lines laid out ahead of them, to balance the lines.
    floats_laid_out_ahead_of_lines: RefCell<HashMap<Node, block_formatting_context::FloatLaidOutAheadOfItsPosition>>,
}

/// What an inline formatting context resolved of an inline box on entering it.
#[derive(Clone, Copy)]
struct EnteredInlineBox {
    /// The sizes and box sides of the box.
    sides: used_values::RootInputCells,
    /// What the insets of the box resolved to.
    insets: abspos_engine::InFlowBoxInsets,
}

impl<'context> InlineFormattingContext<'context> {
    pub(crate) fn new_with_rust_parent(
        run: &'context FormattingContextRun<'context>,
        block_container: block_formatting_context::BlockContainer,
        input: LayoutInput,
        containing_block_position_in_bfc_root: FfiCssPixelPoint,
        callbacks: LayoutPass<'context>,
        parent: &'context block_formatting_context::BlockFormattingContext<'context>,
    ) -> Self {
        Self {
            run,
            containing_block: block_container.node,
            block_container,
            input,
            containing_block_position_in_bfc_root,
            callbacks,
            parent,
            line_data: RefCell::new(used_values::LineData::default()),
            fragmented_inlines_in_pre_order: Vec::new(),
            current_line_start: Cell::new(fragmentation::LinePosition::default()),
            inline_boxes_continued_from_previous_fragment: Vec::new(),
            inline_boxes_continued_in_next_fragment: RefCell::new(Vec::new()),
            block_axis_float_clearance: Cell::new(CssPixels::default()),
            atomic_inline_fragment_arrivals: RefCell::new(HashMap::default()),
            placed_atomic_inlines: RefCell::new(HashMap::default()),
            fragmentainer_stop_token: RefCell::new(None),
            lines_are_unbreakable: Cell::new(false),
            margins_collapse_through_interrupting_blocks: RefCell::new(HashMap::default()),
            entered_inline_boxes: RefCell::new(HashMap::default()),
            used_insets_of_floats: RefCell::new(HashMap::default()),
            floats_laid_out_ahead_of_lines: RefCell::new(HashMap::default()),
        }
    }

    fn keep_atomic_inline_fragment_arrival(
        &self,
        node: Node,
        fragment_arrival: Option<fragment_tree::ChildFragmentArrival>,
    ) {
        let mut arrivals = self.atomic_inline_fragment_arrivals.borrow_mut();
        let previous = match fragment_arrival {
            Some(fragment_arrival) => arrivals.insert(node, fragment_arrival),
            None => arrivals.remove(&node),
        };
        debug_assert!(
            !matches!(previous, Some(fragment_tree::ChildFragmentArrival::RunRoot { .. })),
            "an atomic inline is laid out once by a run before its placement"
        );
    }

    fn take_atomic_inline_fragment_arrival(&self, node: Node) -> Option<fragment_tree::ChildFragmentArrival> {
        self.atomic_inline_fragment_arrivals.borrow_mut().remove(&node)
    }

    /// Keeps what placing `node`, an atomic inline this context laid out, takes. A box laid out again keeps what the
    /// context decided about it before.
    fn keep_placed_atomic_inline(&self, node: Node, mut placed_child: formatting_context::PlacedChild) {
        let mut placed_atomic_inlines = self.placed_atomic_inlines.borrow_mut();
        if let Some(previous) = placed_atomic_inlines.get(&node) {
            placed_child.facts_decided_by_parent = previous.facts_decided_by_parent;
        }
        placed_atomic_inlines.insert(node, placed_child);
    }

    fn update_placed_atomic_inline(
        &self,
        node: Node,
        update: impl FnOnce(formatting_context::PlacedChild) -> formatting_context::PlacedChild,
    ) {
        let mut placed_atomic_inlines = self.placed_atomic_inlines.borrow_mut();
        if let Some(placed_child) = placed_atomic_inlines.remove(&node) {
            placed_atomic_inlines.insert(node, update(placed_child));
        }
    }

    /// Whether this context laid out `node` as an atomic inline-level box.
    fn laid_out_atomic_inline(&self, node: Node) -> bool {
        self.placed_atomic_inlines.borrow().contains_key(&node)
    }

    #[track_caller]
    fn placed_atomic_inline(&self, node: Node) -> formatting_context::PlacedChild {
        self.placed_atomic_inlines
            .borrow()
            .get(&node)
            .cloned()
            .expect("an atomic inline is laid out before it is placed")
    }

    fn hide_placed_atomic_inline_for_line_clamp(&self, node: Node) {
        self.update_placed_atomic_inline(node, |mut placed_child| {
            placed_child
                .facts_decided_by_parent
                .fragment_facts
                .is_invisible_for_line_clamp = true;
            placed_child
        });
    }

    fn discard_unplaced_atomic_inline(&self, node: Node) {
        let fragment_arrival = self.take_atomic_inline_fragment_arrival(node);
        if let Some(fragments) = self.run.fragments.as_deref() {
            fragments.discard_unplaced_subtree(node, fragment_arrival);
        }
    }

    pub(crate) fn block_axis_float_clearance(&self) -> CssPixels {
        self.block_axis_float_clearance.get()
    }

    pub(crate) fn set_block_axis_float_clearance(&self, clearance: CssPixels) {
        self.block_axis_float_clearance.set(clearance);
    }

    fn hide_atomic_inline_for_line_clamp(&self, node: Node) {
        self.discard_unplaced_atomic_inline(node);
        self.hide_placed_atomic_inline_for_line_clamp(node);
    }

    fn stops_at_line_clamp_point<'builder>(
        &'builder self,
        line_builder: &line_builder::LineBuilder<'builder, 'context>,
        state_at_clamp_point: &mut Option<LineBuildingState<'builder, 'context>>,
    ) -> bool {
        if state_at_clamp_point.is_some() || !self.parent.line_clamp_reached() {
            return false;
        }
        if !self.parent.clamped_content_is_scrollable_overflow() || self.run.fragments.is_none() {
            return true;
        }
        let data = self.line_data();
        let current_line_index = data
            .line_boxes
            .len()
            .checked_sub(1)
            .expect("a clamp point follows a line");
        *state_at_clamp_point = Some(LineBuildingState {
            line_builder: line_builder.clone(),
            current_line_index,
            current_line: data.line_boxes[current_line_index].clone(),
            floating_box_count: self.parent.floating_box_count(),
        });
        false
    }

    pub(crate) fn prepare_line_for_line_clamp(
        &self,
        line_index: usize,
        has_immediate_continuation: bool,
        line_block_end: CssPixels,
    ) {
        let line_block_end_in_bfc_root = self.content_box_position_in_bfc_root().y + line_block_end;
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

    #[inline]
    pub(crate) fn style(&self, node: Node) -> StyleValues<'context> {
        StyleValues::for_node(&self.callbacks, node)
    }

    #[inline]
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
        self.line_data.borrow()
    }

    pub(crate) fn line_data_mut(&self) -> RefMut<'_, used_values::LineData> {
        self.line_data.borrow_mut()
    }

    fn inline_size_of_block_container_holding_lines(&self) -> CssPixels {
        self.input
            .containing_block_constraints
            .percentage_basis_inline_size
            .expect("lines are laid out in a block container of definite inline size")
    }

    /// The sizes, box sides and baselines of `node`, a box this context lays out or one it keeps for a run of its own
    /// that has not laid it out yet.
    #[track_caller]
    /// Keeps the sizes and box sides `sides` and the used insets `insets` this context resolved for `node`, an inline
    /// box it entered.
    pub(crate) fn keep_entered_inline_box(
        &self,
        node: Node,
        sides: used_values::RootInputCells,
        insets: abspos_engine::InFlowBoxInsets,
    ) {
        self.entered_inline_boxes
            .borrow_mut()
            .insert(node, EnteredInlineBox { sides, insets });
    }

    fn entered_inline_box(&self, node: Node) -> Option<used_values::RootInputCells> {
        self.entered_inline_boxes
            .borrow()
            .get(&node)
            .map(|entered| entered.sides)
    }

    /// The sizes and box sides of `node`, an inline box this context entered.
    #[track_caller]
    pub(crate) fn sides_of_entered_inline_box(&self, node: Node) -> used_values::RootInputCells {
        self.entered_inline_box(node)
            .expect("the inline formatting context entered the inline box")
    }

    /// What placing `node`, an inline box this context entered, takes.
    #[track_caller]
    fn placed_entered_inline_box(&self, node: Node) -> formatting_context::PlacedChild {
        let entered = *self
            .entered_inline_boxes
            .borrow()
            .get(&node)
            .expect("the inline formatting context entered the inline box");
        formatting_context::PlacedChild::new(fragment_tree::FragmentGeometry {
            cells: entered.sides,
            line_data: None,
        })
        .with_in_flow_box_insets(entered.insets)
    }

    /// The relative-position insets of the inline boxes this context entered from `first_ancestor` up to its
    /// containing block, summed.
    fn relative_insets_of_inline_ancestors(&self, first_ancestor: Node) -> InlineAncestorChainRelativeOffset {
        let used_insets_of_entered_inline_box = |ancestor| {
            if let Some(entered) = self.entered_inline_boxes.borrow().get(&ancestor) {
                return entered.insets.used_insets.unwrap_or_default();
            }
            // NB: A floated inline node can hold inline content this context lays out among its lines, while its
            //     float's own layout resolves its insets.
            *self
                .used_insets_of_floats
                .borrow()
                .get(&ancestor)
                .expect("a relatively positioned inline ancestor in the chain is a box the context laid out")
        };
        accumulated_relative_insets_from_inline_ancestor_chain(
            &self.callbacks,
            first_ancestor,
            self.containing_block,
            used_insets_of_entered_inline_box,
        )
    }

    /// Starts holding `node`, an atomic inline-level box, which a run of its own lays out in the formatting context it
    /// establishes. Returns the cells the box starts out with.
    pub(crate) fn hold_atomic_inline(&self, node: Node) -> used_values::RootInputCells {
        used_values::cells_before_sizing(&self.callbacks, node, self.input.containing_block_constraints)
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

    fn fragmented_inline_ancestors(&self, node: Node) -> Vec<Node> {
        let mut ancestors = Vec::new();
        let mut ancestor = self.nearest_fragmented_inline_ancestor(node);
        while !ancestor.is_invalid() {
            ancestors.push(ancestor);
            ancestor = self.nearest_fragmented_inline_ancestor(ancestor);
        }
        ancestors
    }

    fn fragmented_inline_ancestors_spanning(&self, before: Node, after: Node) -> Vec<Node> {
        let after_ancestors = self.fragmented_inline_ancestors(after);
        let mut ancestors = self.fragmented_inline_ancestors(before);
        ancestors.retain(|ancestor| after_ancestors.contains(ancestor));
        ancestors
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

    /// Resolves the insets of `node`, an in-flow box in this context's lines, and keeps them for the box.
    pub(crate) fn compute_inset(&self, node: Node) -> abspos_engine::InFlowBoxInsets {
        let block_container_cells = self.block_container.cells;
        abspos_engine::compute_inset_native(
            self.run,
            node,
            &block_container_cells,
            geometry::LogicalSize {
                inline_size: block_container_cells.content_inline_size,
                block_size: block_container_cells.content_block_size,
            },
            self.block_container.percentage_block_insets_have_definite_basis,
        )
    }

    pub(crate) fn parent_commit_pending_margin_before_inline_content(&self) -> CssPixels {
        self.parent.commit_pending_margin_before_inline_content()
    }

    fn content_box_position_in_bfc_root(&self) -> FfiCssPixelPoint {
        let position = self.containing_block_position_in_bfc_root;
        FfiCssPixelPoint {
            x: position.x,
            y: position.y
                + self
                    .parent
                    .block_offset_adjustment_from_pending_ancestor_block_start_margins(self.containing_block),
        }
    }

    pub(crate) fn intrusion_by_floats_into_containing_block(
        &self,
        block_start: CssPixels,
        block_end: CssPixels,
    ) -> SpaceUsedByFloats {
        let content_box_position_in_bfc_root = self.content_box_position_in_bfc_root();
        let rect = FfiCssPixelRect {
            x: content_box_position_in_bfc_root.x,
            y: content_box_position_in_bfc_root.y,
            width: self.inline_size_of_block_container_holding_lines(),
            height: self.block_container.cells.content_block_size,
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
        let containing_block_offset_in_root = self.content_box_position_in_bfc_root().y;
        let next = self
            .parent
            .next_float_band_block_start_after(containing_block_offset_in_root + block_offset);
        next.map(|next| next - containing_block_offset_in_root)
    }

    /// Lays out the contents of `node`, which starts out with the cells `cells`, and returns the baselines of its
    /// contents and the cells its layout left it with.
    fn layout_inside(
        &self,
        node: Node,
        available_space: AvailableSpace,
        cells: used_values::RootInputCells,
    ) -> (DerivedBaselines, used_values::RootInputCells) {
        let input = LayoutInput::new(
            available_space,
            self.input.containing_block_constraints,
            ParticipationInParentFormattingContext::AtomicInline,
        );
        let formatting_context::ChildLayout {
            outcome,
            fragment_arrival,
            laid_out_box,
        } = formatting_context::layout_inside_child_with_input_cells(
            self.run,
            Some(self.parent),
            None,
            node,
            input,
            cells,
            false,
            self.block_container.percentage_block_insets_have_definite_basis,
        );
        self.keep_atomic_inline_fragment_arrival(node, fragment_arrival);
        outcome.report_percentage_block_size_dependency_of(self.run, node);
        let baselines = laid_out_box.baselines;
        self.keep_placed_atomic_inline(node, formatting_context::PlacedChild::of_laid_out_box(&laid_out_box));
        (baselines, laid_out_box.cells)
    }

    /// Lays out `node`, an atomic inline-level box on a line that starts out with the cells `cells`, and returns the
    /// baselines of its contents and the cells its layout left it with.
    pub(crate) fn dimension_box_on_line(
        &self,
        node: Node,
        cells: used_values::RootInputCells,
    ) -> (DerivedBaselines, used_values::RootInputCells) {
        let available_space = self.input.available_space;
        let facts = self.facts(node);
        // Any fragmented inline box should have generated line box fragments already.
        if facts.is_fragmented_inline() {
            return (DerivedBaselines::default(), cells);
        }

        let (content_baselines, laid_out_cells) = self.layout_inside(node, available_space, cells);
        debug_assert!(
            laid_out_cells.has_definite_inline_size(),
            "atomic inline-level run left its root's inline size unresolved"
        );
        (content_baselines, laid_out_cells)
    }

    #[inline]
    pub(crate) fn sizing(&self) -> sizing_context::SizingContext<'context, 'context> {
        self.parent.sizing()
    }

    fn clear_floating_boxes(&self, node: Node) -> bool {
        self.parent
            .clear_floating_boxes(node, Some(self), self.containing_block_position_in_bfc_root)
    }

    fn reset_parent_margin_state(&self) {
        self.parent.reset_margin_state();
    }

    fn text_overflow_applies(&self) -> bool {
        let facts = self.facts(self.containing_block);
        if facts.is_anonymous() {
            return facts.inherits_text_overflow_ellipsis();
        }
        node_facts::node_applies_text_overflow_ellipsis(facts.computed_values_view_if_styled())
    }

    fn reusable_atomic_line_prefix(
        &self,
        previous: &inline_content::InlineContent,
        iterator: &inline_level_iterator::InlineLevelIterator,
    ) -> (Vec<line_box::LineBoxData>, usize) {
        if self.containing_block != self.run.box_
            || !previous.inline_box_pieces.is_empty()
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
        let mut fragment_start = 0;
        for retained_line in &previous.lines {
            if !retained_line.reusable_atomic_prefix {
                break;
            }
            let fragment_end = fragment_start + retained_line.fragment_count as usize;
            let mut line = line_box::LineBoxData {
                start: fragmentation::LinePosition::default(),
                record: *retained_line,
                fragments: previous.fragments[fragment_start..fragment_end]
                    .iter()
                    .map(|fragment| line_box_fragment::LineBoxFragmentData::with_record(fragment.clone(), None))
                    .collect(),
                static_position_markers: Vec::new(),
                inline_box_baselines: Vec::new(),
                text_overflow_elision: None,
                block_level_box: None,
            };
            fragment_start = fragment_end;
            let line_item_start = item_index;
            let mut running_inline_length = CssPixels::default();
            let mut matched = true;
            for fragment in &mut line.fragments {
                let Some(item) = iterator.items().get(item_index) else {
                    matched = false;
                    break;
                };
                let atomic_inline = item.box_cells();
                let expected_inline_offset =
                    running_inline_length + item.margin_start + item.border_start + item.padding_start;
                if !fragment.is_atomic_inline
                    || fragment.layout_node != item.node
                    || fragment.inline_offset != expected_inline_offset
                    || fragment.inline_length != item.inline_size
                    || fragment.block_length != atomic_inline.content_block_size
                    || fragment.content_baselines != Some(item.content_baselines)
                    || fragment.border_box_block_start != atomic_inline.border_box_top(false)
                {
                    matched = false;
                    break;
                }
                fragment.box_cells = Some(atomic_inline);
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
            reused_lines.push(line);
        }

        // Changed content can fit on the last reused line, including when an insertion
        // precedes an existing line. Removing all following lines also changes its final-break
        // state. Resume there even if all its old fragments still match.
        if !reused_lines.is_empty()
            && (item_index < iterator.items().len() || reused_lines.len() < previous.lines.len())
        {
            reused_lines.pop();
            item_index = item_count_before_last_line;
        }
        (reused_lines, item_index)
    }

    pub(crate) fn content_sizes_from_items(
        &self,
        items: &[inline_level_iterator::Item],
        depends_on_block_size: bool,
    ) -> sizing_context::ContentInlineSizes {
        let containing_style = self.style(self.containing_block);
        let container_wraps = containing_style.text_wrap_mode() == text_wrap_mode::WRAP;
        let text_indentation =
            TextIndentation::of(containing_style, self.input.containing_block_constraints.inline_basis());
        let mut max_content_lines = ContentSizingLines::new(
            LinesWithoutLineBoxes::indented_by(false, text_indentation),
            container_wraps,
        );
        let mut min_content_lines = ContentSizingLines::new(
            LinesWithoutLineBoxes::indented_by(true, text_indentation),
            container_wraps,
        );
        for index in 0..items.len() {
            max_content_lines.append(self, items, index);
            min_content_lines.append(self, items, index);
        }
        let (max_content_inline_size, first_line_max_content) =
            max_content_lines.lines.finish_measurement_and_first_line();
        let min_content_inline_size = min_content_lines.lines.finish_measurement();
        let sizing = self.sizing();
        sizing_context::ContentInlineSizes {
            sizes: sizing_context::ContentSizes {
                min: min_content_inline_size,
                max: max_content_inline_size,
            },
            first_line_max_content,
            dependencies: IntrinsicMeasurementDependencies {
                percentage_block_size: sizing.resolve_percentage_block_size_dependency(self.containing_block),
                percentage_inline_basis: sizing
                    .measurement_root_observes_percentage_inline_basis(self.containing_block),
                block_size: depends_on_block_size,
            },
        }
    }

    fn text_item_wrap_opportunity(
        &self,
        item: &inline_level_iterator::Item,
        style: StyleValues<'_>,
        is_whitespace: bool,
        remaining_items: &[inline_level_iterator::Item],
        line_ends_in_whitespace: bool,
        can_break_after_previous_overflow_item: bool,
    ) -> TextItemWrapOpportunity {
        if style.text_wrap_mode() != text_wrap_mode::WRAP {
            return TextItemWrapOpportunity::None;
        }
        if is_whitespace {
            if style.white_space_collapse() == white_space_collapse::BREAK_SPACES {
                // https://drafts.csswg.org/css-text-4/#valdef-white-space-collapse-break-spaces
                // A soft wrap opportunity exists after every preserved white space character and after every other
                // space separator (including between adjacent spaces).
                return if line_ends_in_whitespace {
                    TextItemWrapOpportunity::BeforeText
                } else {
                    TextItemWrapOpportunity::None
                };
            }
            let next_inline_size =
                inline_level_iterator::sequence_inline_size(self, remaining_items, false).unwrap_or_default();
            return if next_inline_size > CssPixels::default() {
                TextItemWrapOpportunity::BeforeWhitespaceSequence(item.border_box_inline_size() + next_inline_size)
            } else {
                TextItemWrapOpportunity::None
            };
        }
        if item.can_break_before
            || (can_break_after_previous_overflow_item && !self.overflow_break_applies_to_style(style))
            || line_ends_in_whitespace
        {
            TextItemWrapOpportunity::BeforeText
        } else {
            TextItemWrapOpportunity::None
        }
    }

    pub(crate) fn overflow_break_applies(&self, text_node: Node) -> bool {
        self.overflow_break_applies_to_style(self.style(self.parent_node(text_node)))
    }

    pub(crate) fn overflow_break_applies_to_style(&self, style: StyleValues<'_>) -> bool {
        if style.text_wrap_mode() != text_wrap_mode::WRAP {
            return false;
        }
        breaks_between_graphemes(style) || style.overflow_wrap() == overflow_wrap::BREAK_WORD
    }

    fn set_current_line_start_offset(&self, item_index: usize, offset_in_node: usize) {
        self.current_line_start.set(fragmentation::LinePosition {
            item_index,
            offset_in_node,
        });
    }

    fn break_overflowing_text_item<'builder>(
        &'builder self,
        line_builder: &mut line_builder::LineBuilder<'builder, 'context>,
        item: &mut inline_level_iterator::Item,
        item_index: usize,
        mut line_limit: Option<&mut FragmentainerLineLimit>,
        state_at_clamp_point: &mut Option<LineBuildingState<'builder, 'context>>,
    ) -> bool {
        if line_builder
            .remaining_inline_size_for_overflow_break()
            .is_none_or(|remaining_inline_size| item.border_box_inline_size() <= remaining_inline_size)
        {
            return false;
        }
        let style = self.style(self.parent_node(item.node));
        let text_content = self.callbacks.text_content(item.node);
        let letter_spacing = style.letter_spacing().to_double() as f32;
        let word_spacing = style.word_spacing().to_double() as f32;
        loop {
            // NB: Splitting one text item can create several lines. Observe the clamp point before creating more,
            //     preserving the first clamped line while laying out legacy scrollable overflow.
            if self.stops_at_line_clamp_point(line_builder, state_at_clamp_point) {
                return false;
            }
            if line_limit.as_deref_mut().is_some_and(|limit| limit.is_reached(self)) {
                return true;
            }
            let Some(remaining_inline_size) = line_builder.remaining_inline_size_for_overflow_break() else {
                return false;
            };
            if item.border_box_inline_size() <= remaining_inline_size {
                return false;
            }
            let line_is_empty = self.line_data().line_boxes.last().is_some_and(|line| line.is_empty());
            self.set_current_line_start_offset(item_index, item.offset_in_node);
            if line_is_empty && line_builder.current_line_has_no_space_left_by_floats() {
                line_builder.break_line(line_builder::ForcedBreak::No, NodeSlotId::INVALID, None);
                continue;
            }
            let split_item = self.callbacks.with_grapheme_segmenter(item.node, |grapheme_segmenter| {
                item.split_for_overflow_break(
                    &text_content.text,
                    grapheme_segmenter,
                    letter_spacing,
                    word_spacing,
                    remaining_inline_size,
                    line_is_empty,
                )
            });
            let Some(mut prefix) = split_item else {
                if line_is_empty {
                    return false;
                }
                line_builder.break_line(
                    line_builder::ForcedBreak::No,
                    NodeSlotId::INVALID,
                    Some(item.border_box_inline_size()),
                );
                continue;
            };
            line_builder.append_text_item(&mut prefix, style.line_height());
            self.set_current_line_start_offset(item_index, item.offset_in_node);
            line_builder.break_line(line_builder::ForcedBreak::No, NodeSlotId::INVALID, None);
        }
    }

    /// Whether text-wrap-style: balance balances the lines of this context. Lines are only balanced while finding where
    /// they break leaves the rest of the layout alone, so not with block-level boxes among them, and not when they are
    /// clamped or fragmented.
    fn balances_lines(
        &self,
        iterator: &inline_level_iterator::InlineLevelIterator,
        fragmentation: Option<fragmentation::LineRootFragmentation>,
    ) -> bool {
        let style = self.style(self.containing_block);
        if style.text_wrap_style() != text_wrap_style::BALANCE || style.text_wrap_mode() != text_wrap_mode::WRAP {
            return false;
        }
        if self.parent.has_line_clamp()
            || self.run.fragmentainer.is_some()
            || iterator.shares_items()
            || fragmentation.is_some()
        {
            return false;
        }
        if !matches!(self.input.available_space.inline_size, AvailableSize::Definite(available) if available > CssPixels::default())
        {
            return false;
        }
        // NB: Floats are laid out ahead of the lines, before the floats ahead of them are placed, so not the ones whose
        //     stretch inline size avoids those floats.
        iterator.items().iter().all(|item| {
            item.type_.stays_on_lines()
                || (item.type_ == inline_level_iterator::ItemType::FloatingElement
                    && !self.style(item.node).width().is_stretch())
        })
    }

    /// Lays out the floats among the items of `iterator` ahead of the lines, so that finding where the lines break
    /// can float them beside the lines without laying them out again.
    fn lay_out_floats_ahead_of_lines(&self, iterator: &inline_level_iterator::InlineLevelIterator) {
        for item in iterator.items() {
            if item.type_ == inline_level_iterator::ItemType::FloatingElement {
                let laid_out_float = self.lay_out_float_ahead_of_its_position(item.node);
                self.floats_laid_out_ahead_of_lines
                    .borrow_mut()
                    .insert(item.node, laid_out_float);
            }
        }
    }

    fn lay_out_float_ahead_of_its_position(
        &self,
        node: Node,
    ) -> block_formatting_context::FloatLaidOutAheadOfItsPosition {
        // NB: Every float is sized in cells ahead of its layout, which takes them.
        let cells_of_float_ahead_of_its_layout = Cell::new(used_values::cells_before_sizing(
            &self.callbacks,
            node,
            self.input.containing_block_constraints,
        ));
        self.parent.lay_out_float_ahead_of_its_position(
            self.run,
            block_formatting_context::ChildSizedHere::ahead_of_its_run(node, &cells_of_float_ahead_of_its_layout),
            self.block_container,
            self.input,
            self.containing_block_position_in_bfc_root,
        )
    }

    // https://drafts.csswg.org/css-text-4/#valdef-text-wrap-style-balance
    // Line breaks are chosen to balance the remaining (empty) space in each line box, if better balance than auto is
    // possible. This should avoid changing-- and in the case of 5 or fewer lines must not change-- the number of line
    // boxes the block would contain if text-wrap were set to auto.
    // ...
    // The remaining space to consider is that which remains after placing floats and inline content, but before any
    // adjustments due to text justification. Line boxes are balanced when the standard deviation from the average
    // inline-size of the remaining space in each line box is reduced over the block (including lines that end in a
    // forced line break).
    // ...
    // Groups of lines separated by a forced line break are processed separately.
    /// The inline size taken off the available inline size of each line of each group to balance them, if they are
    /// balanced. Leaving each line of a group the same space unused balances the space that remains on them.
    fn balancing_reductions(&self, iterator: &inline_level_iterator::InlineLevelIterator) -> Vec<Option<CssPixels>> {
        let items = iterator.items();
        let group_ends: Vec<usize> = items
            .iter()
            .enumerate()
            .filter(|(_, item)| item.type_ == inline_level_iterator::ItemType::ForcedBreak)
            .map(|(index, _)| index)
            .chain(std::iter::once(items.len()))
            .collect();
        let state = self.state_before_finding_line_breaks();
        let mut line_builder = line_builder::LineBuilder::new_for_finding_balanced_line_breaks(self);
        let mut reductions = Vec::new();
        let mut group_start = 0;
        for (group_index, &group_end) in group_ends.iter().enumerate() {
            // NB: Breaking the items looks ahead past the forced break after them, as far as the next group reaches.
            let look_ahead_end = group_ends
                .get(group_index + 1)
                .map_or(items.len(), |&next_group_end| (next_group_end + 1).min(items.len()));
            let reduction = self.balancing_reduction_of_group(
                iterator,
                group_start..group_end,
                look_ahead_end,
                &line_builder,
                &reductions,
            );
            reductions.push(reduction);
            if group_end == items.len() {
                break;
            }
            // NB: Break the group and the forced break after it into lines as balanced, so that the lines of the next
            //     group are tried from where they start.
            line_builder.set_balancing_reductions(reductions.clone());
            let mut group_items = iterator.duplicate(group_start..group_end + 1, look_ahead_end);
            let item_count = group_items.items().len();
            self.break_items_into_lines(&mut group_items, &mut line_builder, item_count, None, None, None);
            group_start = group_end + 1;
        }
        self.line_data_mut().line_boxes.clear();
        self.restore_state_after_finding_line_breaks(state);
        reductions
    }

    /// The inline size taken off the available inline size of each line of the items in `group` to balance them:
    /// found by bisecting down to a pixel, it breaks the items into as many lines as the available inline size does.
    fn balancing_reduction_of_group<'builder>(
        &'builder self,
        iterator: &inline_level_iterator::InlineLevelIterator,
        group: std::ops::Range<usize>,
        look_ahead_end: usize,
        line_builder_at_group_start: &line_builder::LineBuilder<'builder, 'context>,
        reductions_of_previous_groups: &[Option<CssPixels>],
    ) -> Option<CssPixels> {
        // INTEROP: Only balance up to 6 lines, since each try breaks all of the lines again.
        const MAXIMUM_LINE_COUNT: usize = 6;
        let available_inline_size = self.input.available_space.inline_size.to_px_or_zero();
        // NB: Skip measuring lines for content far too long to balance.
        let content_inline_size = iterator.items()[group.clone()]
            .iter()
            .fold(CssPixels::default(), |sum, item| sum + item.border_box_inline_size());
        if content_inline_size > available_inline_size * (MAXIMUM_LINE_COUNT * 2) {
            return None;
        }
        let find_line_breaks = |reduction| {
            self.find_line_breaks_of_group(
                iterator,
                group.clone(),
                look_ahead_end,
                line_builder_at_group_start,
                reductions_of_previous_groups,
                reduction,
            )
        };
        let (line_count, inline_size_of_lines) = find_line_breaks(CssPixels::default());
        if line_count <= 1 || line_count > MAXIMUM_LINE_COUNT {
            return None;
        }
        // NB: Bisect between no reduction and one that leaves less than the average line, which is too much unless
        //     lines overflow.
        let mut too_much = available_inline_size - (inline_size_of_lines / line_count).scaled(0.8);
        let mut reduction = CssPixels::default();
        while too_much - reduction > CssPixels::from_integer(1) {
            let candidate = reduction + (too_much - reduction) / 2;
            if find_line_breaks(candidate).0 <= line_count {
                reduction = candidate;
            } else {
                too_much = candidate;
            }
        }
        (reduction > CssPixels::default()).then_some(reduction)
    }

    /// The number of lines of the items in `group`, and the sum of their inline sizes, when `line_builder_at_group_start`
    /// breaks them into lines reduced by `reduction`. The lines and floats of the context are left as they were.
    fn find_line_breaks_of_group<'builder>(
        &'builder self,
        iterator: &inline_level_iterator::InlineLevelIterator,
        group: std::ops::Range<usize>,
        look_ahead_end: usize,
        line_builder_at_group_start: &line_builder::LineBuilder<'builder, 'context>,
        reductions_of_previous_groups: &[Option<CssPixels>],
        reduction: CssPixels,
    ) -> (usize, CssPixels) {
        let state = self.state_before_finding_line_breaks();
        // NB: The lines of the group start with the current line, and breaking it can change the line before it.
        let first_line_index = self.line_data().line_boxes.len() - 1;
        let first_changed_line_index = first_line_index.saturating_sub(1);
        let lines_before_breaking = self.line_data().line_boxes[first_changed_line_index..].to_vec();
        let mut line_builder = line_builder_at_group_start.clone();
        let mut reductions = reductions_of_previous_groups.to_vec();
        reductions.push(Some(reduction));
        line_builder.set_balancing_reductions(reductions);
        let mut group_items = iterator.duplicate(group, look_ahead_end);
        let item_count = group_items.items().len();
        self.break_items_into_lines(&mut group_items, &mut line_builder, item_count, None, None, None);
        let mut data = self.line_data_mut();
        let lines = &mut data.line_boxes;
        let mut end = lines.len();
        while end > first_line_index && lines[end - 1].is_empty() {
            end -= 1;
        }
        let inline_size_of_lines = lines[first_line_index..end]
            .iter_mut()
            .fold(CssPixels::default(), |sum, line| {
                line.trim_trailing_whitespace();
                sum + line.inline_length
            });
        lines.truncate(first_changed_line_index);
        lines.extend(lines_before_breaking);
        drop(data);
        self.restore_state_after_finding_line_breaks(state);
        (end - first_line_index, inline_size_of_lines)
    }

    /// The state of this context and its parent that breaking items into lines changes beyond the lines.
    fn state_before_finding_line_breaks(&self) -> StateBeforeFindingLineBreaks {
        StateBeforeFindingLineBreaks {
            parent: self.parent.state_before_finding_line_breaks(),
            block_axis_float_clearance: self.block_axis_float_clearance.get(),
        }
    }

    /// Restores the state that breaking items into lines changed beyond the lines, only to find where they break.
    fn restore_state_after_finding_line_breaks(&self, state: StateBeforeFindingLineBreaks) {
        self.parent.restore_state_after_finding_line_breaks(state.parent);
        self.block_axis_float_clearance.set(state.block_axis_float_clearance);
    }

    /// Breaks the items left in `iterator` into lines with `line_builder`. `item_count` is the number of items the
    /// iterator started with.
    fn break_items_into_lines<'builder>(
        &'builder self,
        iterator: &mut inline_level_iterator::InlineLevelIterator,
        line_builder: &mut line_builder::LineBuilder<'builder, 'context>,
        item_count: usize,
        mut pending_resume_offset: Option<usize>,
        mut line_limit: Option<FragmentainerLineLimit>,
        mut consumed_item_nodes: Option<Vec<(usize, Node)>>,
    ) -> BrokenLines<'builder, 'context> {
        let shares_items = iterator.shares_items();
        let mut leading_margin = CssPixels::default();
        let mut leading_border = CssPixels::default();
        let mut leading_padding = CssPixels::default();
        let mut absolute_boxes = Vec::new();
        let mut previous_text_item_allows_overflow_break_after = false;
        let mut state_at_clamp_point = None;
        let mut items_after_clamp_point = Vec::new();
        let mut stopped_at_line_limit = false;
        loop {
            if self.stops_at_line_clamp_point(line_builder, &mut state_at_clamp_point) {
                break;
            }
            if line_limit.as_mut().is_some_and(|limit| limit.is_reached(self)) {
                stopped_at_line_limit = true;
                break;
            }
            let Some(mut item) = iterator.next() else {
                break;
            };
            let item_index = item_count - iterator.items().len() - 1;
            if let Some(consumed_item_nodes) = consumed_item_nodes.as_mut() {
                consumed_item_nodes.push((item_index, item.node));
            }
            if let Some(offset_in_node) = pending_resume_offset.take()
                && item.type_ == inline_level_iterator::ItemType::Text
                && offset_in_node > item.offset_in_node
            {
                let style = self.style(self.parent_node(item.node));
                let text_content = self.callbacks.text_content(item.node);
                item.drop_prefix_before(
                    offset_in_node,
                    &text_content.text,
                    style.letter_spacing().to_double() as f32,
                    style.word_spacing().to_double() as f32,
                );
            }
            let has_leading_edges = leading_margin != CssPixels::default()
                || leading_border != CssPixels::default()
                || leading_padding != CssPixels::default();
            if !has_leading_edges {
                self.current_line_start.set(fragmentation::LinePosition {
                    item_index,
                    offset_in_node: item.offset_in_node,
                });
            }
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
                    self.current_line_start.set(fragmentation::LinePosition {
                        item_index: item_index + 1,
                        offset_in_node: 0,
                    });
                    let continuation = iterator.next_inline_run_size(self);
                    line_builder.break_line(line_builder::ForcedBreak::Yes, item.node, continuation);
                    if !item.node.is_invalid() && self.clear_floating_boxes(item.node) {
                        line_builder.did_introduce_clearance(self.block_axis_float_clearance.get());
                        self.reset_parent_margin_state();
                    }
                }
                inline_level_iterator::ItemType::Element => {
                    line_builder.prepare_to_append_inline_content();
                    if shares_items && !self.laid_out_atomic_inline(item.node) {
                        // NB: The box is laid out again in this run, and keeps the content baselines it had.
                        let cells = self.hold_atomic_inline(item.node);
                        let (_, laid_out_cells) = self.dimension_box_on_line(item.node, cells);
                        item.box_cells = Some(Box::new(laid_out_cells));
                    }
                    if !line_builder.only_finds_line_breaks() {
                        let insets = self.compute_inset(item.node);
                        self.update_placed_atomic_inline(item.node, |placed_child| {
                            placed_child.with_in_flow_box_insets(insets)
                        });
                    }
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
                    if self.stops_at_line_clamp_point(line_builder, &mut state_at_clamp_point) {
                        break;
                    }
                    line_builder.append_box(&item);
                }
                inline_level_iterator::ItemType::BlockLevelBox => {
                    leading_margin += item.margin_start;
                    leading_border += item.border_start;
                    leading_padding += item.padding_start;
                    line_builder.finish_current_line_before_block_level_box();
                    if self.stops_at_line_clamp_point(line_builder, &mut state_at_clamp_point) {
                        break;
                    }
                    let relative_insets = self.relative_insets_of_inline_ancestors(self.callbacks.parent(item.node));
                    let collapses_through = self.parent.layout_interrupting_block_inside_inline_context(
                        self.run,
                        item.node,
                        self.block_container,
                        self.input,
                        self.containing_block_position_in_bfc_root,
                        line_builder,
                        state_at_clamp_point.is_some(),
                        if relative_insets.found_fragmented_inline_node {
                            FfiCssPixelPoint {
                                x: relative_insets.offset_x,
                                y: relative_insets.offset_y,
                            }
                        } else {
                            FfiCssPixelPoint::default()
                        },
                    );
                    self.margins_collapse_through_interrupting_blocks
                        .borrow_mut()
                        .insert(item.node, collapses_through);
                }
                inline_level_iterator::ItemType::AbsolutelyPositionedElement => {
                    if !self.facts(item.node).is_box() {
                        continue;
                    }
                    if state_at_clamp_point.is_some() {
                        items_after_clamp_point.push((item.type_, item.node));
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
                    self.clear_floating_boxes(item.node);
                    line_builder.set_unbreakable_run_inline_size_interrupted_by_float(
                        iterator.next_unbreakable_run_inline_size(self),
                    );
                    if line_builder.only_finds_line_breaks() {
                        let cells = self
                            .floats_laid_out_ahead_of_lines
                            .borrow()
                            .get(&item.node)
                            .expect("floats among balanced lines are laid out ahead of them")
                            .cells;
                        self.parent.add_float_for_finding_line_breaks(
                            item.node,
                            &cells,
                            self.block_container,
                            self.input,
                            self.containing_block_position_in_bfc_root,
                            line_builder,
                        );
                        continue;
                    }
                    let fragment_facts = run_state::FragmentFactsDecidedByParent {
                        is_invisible_for_line_clamp: state_at_clamp_point.is_some(),
                        ..run_state::FragmentFactsDecidedByParent::default()
                    };
                    let laid_out_ahead = self.floats_laid_out_ahead_of_lines.borrow_mut().remove(&item.node);
                    let laid_out_float =
                        laid_out_ahead.unwrap_or_else(|| self.lay_out_float_ahead_of_its_position(item.node));
                    let (insets, _, _) = self.parent.position_laid_out_float(
                        self.run,
                        laid_out_float,
                        self.block_container,
                        self.input,
                        self.containing_block_position_in_bfc_root,
                        CssPixels::default(),
                        Some(&mut *line_builder),
                        fragment_facts,
                    );
                    self.used_insets_of_floats
                        .borrow_mut()
                        .insert(item.node, insets.used_insets.unwrap_or_default());
                }
                inline_level_iterator::ItemType::Text => {
                    line_builder.prepare_to_append_inline_content();
                    let style = self.style(self.parent_node(item.node));
                    if style.text_wrap_mode() == text_wrap_mode::WRAP {
                        let is_whitespace = item.is_collapsible_whitespace || item.is_ascii_whitespace(self);
                        let item_inline_size = item.border_box_inline_size();
                        let line_ends_in_whitespace = self
                            .line_data()
                            .line_boxes
                            .last()
                            .is_some_and(line_box::LineBoxData::is_empty_or_ends_in_whitespace);
                        let opportunity = self.text_item_wrap_opportunity(
                            &item,
                            style,
                            is_whitespace,
                            iterator.items(),
                            line_ends_in_whitespace,
                            can_break_after_previous_overflow_item,
                        );
                        let overflow_break_allowed = !is_whitespace && self.overflow_break_applies_to_style(style);
                        match opportunity {
                            TextItemWrapOpportunity::BeforeWhitespaceSequence(sequence_inline_size) => {
                                let broke_line = if iterator.next_non_whitespace_text_allows_overflow_break(self) {
                                    line_builder.break_if_needed_before_overflow_breakable_item(sequence_inline_size)
                                } else {
                                    line_builder.break_if_needed(sequence_inline_size)
                                };
                                if broke_line {
                                    line_builder.set_trailing_whitespace_on_previous_line();
                                    let next_line_start = fragmentation::LinePosition {
                                        item_index: item_index + 1,
                                        offset_in_node: 0,
                                    };
                                    self.current_line_start.set(next_line_start);
                                    if let Some(line) = self.line_data_mut().line_boxes.last_mut() {
                                        line.start = next_line_start;
                                    }
                                    continue;
                                }
                            }
                            TextItemWrapOpportunity::BeforeText => {
                                if overflow_break_allowed {
                                    line_builder.break_if_needed_before_overflow_breakable_item(item_inline_size);
                                } else {
                                    line_builder.break_if_needed(item_inline_size);
                                }
                                line_builder.note_soft_wrap_opportunity();
                            }
                            TextItemWrapOpportunity::None => {}
                        }
                        if overflow_break_allowed {
                            if self.break_overflowing_text_item(
                                line_builder,
                                &mut item,
                                item_index,
                                line_limit.as_mut(),
                                &mut state_at_clamp_point,
                            ) {
                                stopped_at_line_limit = true;
                                break;
                            }
                            previous_text_item_allows_overflow_break_after = true;
                        }
                    }
                    if self.stops_at_line_clamp_point(line_builder, &mut state_at_clamp_point) {
                        break;
                    }
                    line_builder.append_text_item(&mut item, style.line_height());
                }
            }
        }
        BrokenLines {
            absolute_boxes,
            state_at_clamp_point,
            items_after_clamp_point,
            stopped_at_line_limit,
            consumed_item_nodes,
        }
    }

    pub(crate) fn generate_line_boxes(&mut self, mut iterator: inline_level_iterator::InlineLevelIterator) {
        self.fragmented_inlines_in_pre_order = iterator.fragmented_inlines_in_pre_order(self);
        let fragmentation = self.parent.line_root_fragmentation(self.containing_block);
        let resume = fragmentation.and_then(|fragmentation| fragmentation.resume);
        let balances_lines = self.balances_lines(&iterator, fragmentation);
        let (reused_lines, reused_item_count) = if self.parent.has_line_clamp() || resume.is_some() || balances_lines {
            Default::default()
        } else {
            self.parent
                .previous_line_data
                .as_deref()
                .map(|previous| self.reusable_atomic_line_prefix(previous, &iterator))
                .unwrap_or_default()
        };
        {
            let mut data = self.line_data_mut();
            data.line_boxes = reused_lines;
            data.inline_box_pieces.clear();
            data.lines_after_clamp_point_rect = None;
        }
        let item_count = iterator.items().len();
        let start_item_index = resume.map_or(reused_item_count, |resume| resume.item_index.min(item_count));
        let shares_items = iterator.shares_items();
        let skipped_atomic_inlines: Vec<Node> = if resume.is_some() && !shares_items {
            iterator.items()[..start_item_index]
                .iter()
                .filter(|item| item.type_ == inline_level_iterator::ItemType::Element)
                .map(|item| item.node)
                .collect()
        } else {
            Vec::new()
        };
        let pending_resume_offset = resume.map(|resume| resume.offset_in_node);
        self.inline_boxes_continued_from_previous_fragment = {
            let (before, after) = iterator.items().split_at(start_item_index.min(item_count));
            let has_node = |item: &&inline_level_iterator::Item| !item.node.is_invalid();
            let left_fragment = |item: &&inline_level_iterator::Item| {
                has_node(item)
                    && item.type_ != inline_level_iterator::ItemType::ForcedBreak
                    && item.type_ != inline_level_iterator::ItemType::AbsolutelyPositionedElement
                    && !item.is_collapsible_whitespace
            };
            match after.first() {
                Some(item) if pending_resume_offset.is_some_and(|offset| offset > item.offset_in_node) => {
                    self.fragmented_inline_ancestors(item.node)
                }
                _ if resume.is_some() => match (before.iter().rev().find(left_fragment), after.iter().find(has_node)) {
                    (Some(before), Some(after)) => self.fragmented_inline_ancestors_spanning(before.node, after.node),
                    _ => Vec::new(),
                },
                _ => Vec::new(),
            }
        };
        let resumes_after_forced_break = resume.is_some()
            && start_item_index > 0
            && iterator.items()[start_item_index - 1].type_ == inline_level_iterator::ItemType::ForcedBreak;
        iterator.skip_items(start_item_index);
        // NB: Trials to balance the lines move the start of the current line, so balance before the lines start.
        let balancing_reductions = if balances_lines {
            self.lay_out_floats_ahead_of_lines(&iterator);
            self.balancing_reductions(&iterator)
        } else {
            Vec::new()
        };
        self.current_line_start.set(fragmentation::LinePosition {
            item_index: start_item_index,
            offset_in_node: pending_resume_offset.unwrap_or_default(),
        });
        let reused_line_count = self.line_data().line_boxes.len();
        let mut line_builder = if resume.is_some() {
            line_builder::LineBuilder::new_for_continuation(self, resumes_after_forced_break)
        } else if reused_line_count == 0 {
            line_builder::LineBuilder::new(self)
        } else {
            line_builder::LineBuilder::new_after_reused_lines(self)
        };
        line_builder.set_balancing_reductions(balancing_reductions);

        let mut floating_boxes_after_clamp_point = Vec::new();
        let floating_box_count_before_lines = self.parent.floating_box_count();
        let line_limit = fragmentation
            .filter(|_| !self.parent.has_line_clamp())
            .and_then(|fragmentation| fragmentation.block_size)
            .filter(|_| shares_items || iterator.items().iter().all(|item| item.type_.stays_on_lines()))
            .map(|block_size| FragmentainerLineLimit {
                block_size,
                widows: self.style(self.containing_block).widows().max(1) as usize,
                checked_line_count: 0,
                lines_past_end: 0,
                minimum_line_block_size: None,
            });
        let consumed_item_nodes = (self.run.fragmentainer.is_some()
            && self.run.fragments.is_some()
            && !iterator.entered_inline_boxes().is_empty())
        .then(Vec::new);
        let BrokenLines {
            mut absolute_boxes,
            state_at_clamp_point,
            items_after_clamp_point,
            stopped_at_line_limit,
            consumed_item_nodes,
        } = self.break_items_into_lines(
            &mut iterator,
            &mut line_builder,
            item_count,
            pending_resume_offset,
            line_limit,
            consumed_item_nodes,
        );

        if let Some(state_at_clamp_point) = state_at_clamp_point {
            line_builder.update_last_line(false);
            for line in &mut self.line_data_mut().line_boxes[state_at_clamp_point.current_line_index..] {
                line.trim_trailing_whitespace();
            }
            let content_inline_size = self.inline_size_of_block_container_holding_lines();
            let mut lines_after_clamp_point_rect = CssPixelRect::default();
            let mut atomic_inlines_after_clamp_point = Vec::new();
            for line in self.line_data().line_boxes[state_at_clamp_point.current_line_index..]
                .iter()
                .filter(|line| !line.is_empty())
            {
                lines_after_clamp_point_rect.unite(line_rect(line, content_inline_size).into());
                for fragment in line.visible_fragments() {
                    let (x, y) = fragment.offset();
                    let relative_insets =
                        self.relative_insets_of_inline_ancestors(self.callbacks.parent(fragment.layout_node));
                    let position = FfiCssPixelPoint {
                        x: x + relative_insets.offset_x,
                        y: y + relative_insets.offset_y,
                    };
                    // A replaced element counts as atomic even when it's block-level, but a block-level box on a line
                    // interrupted for it was laid out and placed as a block, not as an atomic inline.
                    if fragment.is_atomic_inline && !line.has_block_level_box {
                        atomic_inlines_after_clamp_point.push((fragment.layout_node, position));
                    } else {
                        let (width, height) = fragment.size();
                        lines_after_clamp_point_rect.unite(CssPixelRect::new(position.x, position.y, width, height));
                    }
                }
            }
            {
                let mut data = self.line_data_mut();
                data.lines_after_clamp_point_rect =
                    (!lines_after_clamp_point_rect.is_empty()).then(|| Box::new(lines_after_clamp_point_rect.into()));
                data.line_boxes.truncate(state_at_clamp_point.current_line_index);
                data.line_boxes.push(state_at_clamp_point.current_line);
            }
            line_builder = state_at_clamp_point.line_builder;
            floating_boxes_after_clamp_point = self
                .parent
                .take_floating_boxes_from(state_at_clamp_point.floating_box_count);
            for (node, position) in atomic_inlines_after_clamp_point {
                self.hide_placed_atomic_inline_for_line_clamp(node);
                let fragment_arrival = self.take_atomic_inline_fragment_arrival(node);
                formatting_context::place_child(
                    self.run,
                    node,
                    self.placed_atomic_inline(node),
                    position,
                    None,
                    fragment_arrival,
                );
            }
        }
        if self.parent.line_clamp_reached() {
            let remaining_items = iterator.items().iter().map(|item| (item.type_, item.node));
            for (item_type, node) in items_after_clamp_point.into_iter().chain(remaining_items) {
                if item_type == inline_level_iterator::ItemType::Element {
                    self.hide_atomic_inline_for_line_clamp(node);
                } else if item_type == inline_level_iterator::ItemType::AbsolutelyPositionedElement
                    && self.facts(node).is_box()
                    && self.line_data().line_boxes.iter().any(|line| {
                        line.visible_fragments().any(|fragment| {
                            self.callbacks.is_ancestor(
                                self.callbacks.parent(node),
                                fragment.layout_node,
                                self.containing_block,
                            )
                        })
                    })
                {
                    line_builder.append_static_position_marker(node, false);
                    absolute_boxes.push(node);
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

        let mut lines_are_fragmented = false;
        let mut kept_lines_end = None;
        if let Some(fragmentation) = fragmentation {
            let breakable = !self.parent.has_line_clamp()
                && self.parent.floating_box_count() == floating_box_count_before_lines
                && !self.line_data().line_boxes.iter().any(|line| line.has_block_level_box);
            let dropped_atomic_inlines = if breakable {
                lines_are_fragmented = true;
                let (dropped_atomic_inlines, next_line_start) =
                    self.keep_lines_of_fragmentainer(fragmentation, stopped_at_line_limit);
                kept_lines_end = next_line_start;
                dropped_atomic_inlines
            } else {
                let lines_block_end_in_root = self.content_box_position_in_bfc_root().y
                    + self
                        .line_data()
                        .line_boxes
                        .iter()
                        .rfind(|line| !line.is_empty())
                        .map_or(CssPixels::default(), |line| line.block_end);
                self.parent
                    .note_unbreakable_line_root(self.containing_block, lines_block_end_in_root);
                self.lines_are_unbreakable.set(true);
                Vec::new()
            };
            let unconsumed_atomic_inlines: Vec<Node> = if stopped_at_line_limit && !shares_items {
                iterator
                    .items()
                    .iter()
                    .filter(|item| item.type_ == inline_level_iterator::ItemType::Element)
                    .map(|item| item.node)
                    .collect()
            } else {
                Vec::new()
            };
            if self.run.fragments.is_some() {
                for &node in &skipped_atomic_inlines {
                    self.discard_unplaced_atomic_inline(node);
                }
                for node in dropped_atomic_inlines.into_iter().chain(unconsumed_atomic_inlines) {
                    self.discard_unplaced_atomic_inline(node);
                }
            }
        }
        if let Some(consumed_item_nodes) = consumed_item_nodes {
            let present_inline_boxes = lines_are_fragmented.then(|| {
                self.inline_boxes_present_in_lines(
                    iterator.entered_inline_boxes(),
                    &consumed_item_nodes,
                    kept_lines_end,
                    resume.is_none(),
                )
            });
            for &node in iterator.entered_inline_boxes() {
                if present_inline_boxes
                    .as_ref()
                    .is_none_or(|present| present.contains(&node))
                {
                    formatting_context::place_child(
                        self.run,
                        node,
                        self.placed_entered_inline_box(node),
                        FfiCssPixelPoint::default(),
                        None,
                        None,
                    );
                }
            }
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
                // Atomic boxes are painted independently of their line fragments. Apply the inline ancestors'
                // relative insets to their placement as well as to the line data folded below.
                let relative_insets =
                    self.relative_insets_of_inline_ancestors(self.callbacks.parent(fragment.layout_node));
                let fragment_arrival = self.take_atomic_inline_fragment_arrival(fragment.layout_node);
                formatting_context::place_child(
                    self.run,
                    fragment.layout_node,
                    self.placed_atomic_inline(fragment.layout_node),
                    FfiCssPixelPoint {
                        x: x + relative_insets.offset_x,
                        y: y + relative_insets.offset_y,
                    },
                    Some(used_values::LineBoxFragmentCoordinate {
                        line_box_index: line_index,
                    }),
                    fragment_arrival,
                );
            }
        }

        for box_ in absolute_boxes {
            if lines_are_fragmented
                && !self
                    .line_data()
                    .line_boxes
                    .iter()
                    .any(|line| line.static_position_markers.iter().any(|marker| marker.box_ == box_))
            {
                continue;
            }
            let mut static_position = abspos_inputs::StaticPositionRect {
                rect: Default::default(),
                inline_alignment: StaticPositionAlignment::Start,
                block_alignment: StaticPositionAlignment::Start,
                alignment_derives_from_own_computed_values: false,
                is_known: true,
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
        line_builder.remove_last_line_if_empty();
        self.parent.append_floating_boxes(floating_boxes_after_clamp_point);
    }

    pub(crate) fn run(&mut self) -> block_formatting_context::ChildrenSummary {
        assert!(self.facts(self.containing_block).children_are_inline());
        let run = self.run;
        let iterator = match run.fragmentainer.as_deref() {
            Some(fragmentainer) => {
                inline_level_iterator::InlineLevelIterator::new_sharing_items(self, &fragmentainer.inline_items)
            }
            None => inline_level_iterator::InlineLevelIterator::new(self),
        };
        self.generate_line_boxes(iterator);
        if self.run.fragments.is_some() {
            self.compute_inline_box_pieces();
            self.fold_inline_ancestor_relative_insets_into_line_data();
            self.register_inline_anchor_candidates();
        }
        self.leave_atomic_inlines_without_a_line_unplaced();
        let last_line = self
            .line_data
            .get_mut()
            .line_boxes
            .last()
            .map(line_box::LineBoxData::retained_metrics);
        let baselines = formatting_context::derive_baselines_from_lines(&self.line_data.get_mut().line_boxes);
        block_formatting_context::ChildrenSummary {
            baselines,
            lines: Some(std::mem::take(self.line_data.get_mut())),
            fragmentainer_stop_token: self.fragmentainer_stop_token.take(),
            lines_are_unbreakable: self.lines_are_unbreakable.get(),
            last_line,
            has_child_whose_margins_do_not_collapse_through: !self
                .children_let_margins_collapse_through(self.containing_block),
            ..Default::default()
        }
    }

    /// Whether margins collapse through the children of `node`, the containing block or an inline box in it: through
    /// each inline-level box this context holds, and through each block-level box it interrupted its lines for.
    fn children_let_margins_collapse_through(&self, node: Node) -> bool {
        self.callbacks.children(node).all(|child| {
            let facts = self.facts(child);
            if !facts.is_box() || facts.is_absolutely_positioned() || facts.is_floating() {
                return true;
            }
            let interrupting_block_collapses_through = self
                .margins_collapse_through_interrupting_blocks
                .borrow()
                .get(&child)
                .copied();
            if let Some(collapses_through) = interrupting_block_collapses_through {
                return collapses_through;
            }
            let entered_inline_box = self.entered_inline_box(child);
            if entered_inline_box.is_none() && !self.laid_out_atomic_inline(child) {
                return true;
            }
            block_formatting_context::margins_can_collapse_through_box_itself(facts, self.style(child), || {
                match entered_inline_box {
                    Some(sides) => sides.border_box_block_size(false),
                    None => {
                        let atomic_inline = self.placed_atomic_inline(child).geometry.cells;
                        atomic_inline.border_box_block_size(false)
                    }
                }
            }) && self.children_let_margins_collapse_through(child)
        })
    }

    fn leave_atomic_inlines_without_a_line_unplaced(&self) {
        let mut unplaced: Vec<_> = self.atomic_inline_fragment_arrivals.take().into_iter().collect();
        unplaced.sort_by(|(left, _), (right, _)| {
            if self.callbacks.is_before(*left, *right) {
                std::cmp::Ordering::Less
            } else {
                std::cmp::Ordering::Greater
            }
        });
        for (node, fragment_arrival) in unplaced {
            formatting_context::leave_child_unplaced(self.run, self.placed_atomic_inline(node), Some(fragment_arrival));
        }
    }

    fn compute_inline_box_pieces(&mut self) {
        let (pieces, inline_containing_block_rect_candidates) = compute(self);
        self.line_data_mut().inline_box_pieces = pieces;
        for candidate in inline_containing_block_rect_candidates {
            let relative_inset_chain = self.relative_insets_of_inline_ancestors(candidate.inline_containing_block);
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

    fn register_inline_anchor_candidates(&self) {
        let Some(fragments) = self.run.fragments.as_deref() else {
            return;
        };
        // An inline anchor's border box is the union of its line fragments, including relative positioning.
        // Keep discovery order so that repeated layout runs produce the same propagated candidates.
        let mut candidates = Vec::<(Node, CssPixelRect)>::new();
        let mut indices = HashMap::<Node, usize>::default();
        for piece in &self.line_data().inline_box_pieces {
            if !self.facts(piece.node).has_anchor_names() {
                continue;
            }
            let rect = CssPixelRect::from(piece.border_box_rect).translated_by(piece.relpos_delta.into());
            if let Some(&index) = indices.get(&piece.node) {
                candidates[index].1.unite(rect);
            } else {
                indices.insert(piece.node, candidates.len());
                candidates.push((piece.node, rect));
            }
        }
        for (node, rect) in candidates {
            fragments.register_inline_anchor_candidate(node, rect, self.containing_block);
        }
    }

    fn keep_lines_of_fragmentainer(
        &self,
        fragmentation: fragmentation::LineRootFragmentation,
        stopped_at_line_limit: bool,
    ) -> (Vec<Node>, Option<fragmentation::LinePosition>) {
        let line_count = self.line_data().line_boxes.len();
        let is_first_fragment = fragmentation.resume.is_none();
        let root_block_offset = self.content_box_position_in_bfc_root().y;
        let mut stop = line_count;
        for index in 0..line_count {
            let (block_start, block_end, is_empty) = {
                let line = &self.line_data().line_boxes[index];
                (
                    root_block_offset + line.block_start,
                    root_block_offset + line.block_end,
                    line.is_empty(),
                )
            };
            if is_empty {
                continue;
            }
            let exceeds = fragmentation
                .block_size
                .is_some_and(|block_size| block_end > block_size);
            if exceeds && index > 0 {
                self.parent
                    .note_line_for_fragmentation(block_end - block_start, block_end, false);
                stop = index;
                break;
            }
            if exceeds {
                if is_first_fragment && fragmentation.fragmentainer_had_content {
                    self.parent.request_early_break_before(self.containing_block);
                }
                self.parent.note_overflowing_content();
            }
            self.parent
                .note_line_for_fragmentation(block_end - block_start, block_end, true);
        }
        if stop < line_count {
            let content_line_count = self
                .line_data()
                .line_boxes
                .iter()
                .rposition(|line| !line.is_empty())
                .map_or(0, |index| index + 1);
            let style = self.style(self.containing_block);
            // https://drafts.csswg.org/css-break-3/#widows-orphans
            let orphans = style.orphans().max(1) as usize;
            let widows = style.widows().max(1) as usize;
            if stop < orphans {
                if is_first_fragment
                    && fragmentation.fragmentainer_had_content
                    && !fragmentation.break_before_is_avoided
                {
                    self.parent.request_early_break_before(self.containing_block);
                } else {
                    self.parent.note_violated_break_avoidance();
                }
            } else if !stopped_at_line_limit && content_line_count.saturating_sub(stop) < widows {
                let stop_keeping_widows = content_line_count.saturating_sub(widows);
                if stop_keeping_widows >= orphans {
                    stop = stop_keeping_widows;
                } else {
                    self.parent.note_violated_break_avoidance();
                }
            }
        }

        *self.inline_boxes_continued_in_next_fragment.borrow_mut() = {
            let data = self.line_data();
            let last_kept = data.line_boxes[..stop]
                .iter()
                .flat_map(|line| line.fragments.iter())
                .rfind(|fragment| !fragment.layout_node.is_invalid());
            let first_dropped = data.line_boxes[stop..]
                .iter()
                .flat_map(|line| line.fragments.iter())
                .find(|fragment| !fragment.layout_node.is_invalid());
            match (last_kept, first_dropped) {
                (Some(last_kept), Some(first_dropped)) => {
                    self.fragmented_inline_ancestors_spanning(last_kept.layout_node, first_dropped.layout_node)
                }
                _ => Vec::new(),
            }
        };
        let mut data = self.line_data_mut();
        let dropped_atomic_inlines = data.line_boxes[stop..]
            .iter()
            .flat_map(|line| line.fragments.iter())
            .filter(|fragment| fragment.is_atomic_inline)
            .map(|fragment| fragment.layout_node)
            .collect();
        let next_line_start = data.line_boxes.get(stop).map(|line| line.start);
        data.line_boxes.truncate(stop);
        drop(data);
        if let Some(next_line_start) = next_line_start {
            let stop_token = self
                .parent
                .break_line_root_before_line(self.containing_block, next_line_start);
            self.fragmentainer_stop_token.replace(stop_token);
        }
        (dropped_atomic_inlines, next_line_start)
    }

    fn inline_boxes_present_in_lines(
        &self,
        entered_inline_boxes: &[Node],
        consumed_item_nodes: &[(usize, Node)],
        kept_lines_end: Option<fragmentation::LinePosition>,
        is_first_fragment: bool,
    ) -> HashSet<Node> {
        let mut present: HashSet<Node> = self
            .inline_boxes_continued_from_previous_fragment
            .iter()
            .chain(self.inline_boxes_continued_in_next_fragment.borrow().iter())
            .copied()
            .collect();
        let kept_item_nodes = consumed_item_nodes
            .iter()
            .filter(|(item_index, _)| {
                kept_lines_end.is_none_or(|end| {
                    *item_index < end.item_index || (*item_index == end.item_index && end.offset_in_node > 0)
                })
            })
            .map(|(_, node)| *node);
        let data = self.line_data();
        let nodes_on_lines = data.line_boxes.iter().flat_map(|line| {
            line.fragments
                .iter()
                .map(|fragment| fragment.layout_node)
                .chain(line.static_position_markers.iter().map(|marker| marker.box_))
        });
        let childless_inline_boxes = entered_inline_boxes
            .iter()
            .copied()
            .filter(|node| is_first_fragment && self.callbacks.first_child(*node).is_invalid());
        present.extend(childless_inline_boxes.clone());
        let mut walked = HashSet::<Node>::default();
        for node in kept_item_nodes
            .chain(nodes_on_lines)
            .chain(childless_inline_boxes)
            .filter(|node| !node.is_invalid())
        {
            let mut ancestor = self.callbacks.parent(node);
            while !ancestor.is_invalid() && ancestor != self.containing_block && walked.insert(ancestor) {
                present.insert(ancestor);
                ancestor = self.callbacks.parent(ancestor);
            }
        }
        present
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
                    let chain = self.relative_insets_of_inline_ancestors(first_ancestor);
                    FfiCssPixelPoint {
                        x: chain.offset_x,
                        y: chain.offset_y,
                    }
                })
        };
        for line in &mut data.line_boxes {
            // NB: Fragments that text-overflow elided keep their shift too, since the elided content still extends
            //     the scrollable overflow.
            for fragment in &mut line.fragments {
                fragment.relpos_delta = accumulated_relative_offset_from(self.callbacks.parent(fragment.layout_node));
            }
        }
        for piece in &mut data.inline_box_pieces {
            piece.relpos_delta = accumulated_relative_offset_from(piece.node);
        }
    }
}

/// The state of an inline formatting context and its parent that breaking items into lines changes beyond the lines.
struct StateBeforeFindingLineBreaks {
    parent: block_formatting_context::StateBeforeFindingLineBreaks,
    block_axis_float_clearance: CssPixels,
}

/// What breaking the items into lines left to finish once the lines are built.
struct BrokenLines<'builder, 'context> {
    absolute_boxes: Vec<Node>,
    state_at_clamp_point: Option<LineBuildingState<'builder, 'context>>,
    items_after_clamp_point: Vec<(inline_level_iterator::ItemType, Node)>,
    stopped_at_line_limit: bool,
    consumed_item_nodes: Option<Vec<(usize, Node)>>,
}

struct LineBuildingState<'builder, 'context> {
    line_builder: line_builder::LineBuilder<'builder, 'context>,
    current_line_index: usize,
    current_line: line_box::LineBoxData,
    floating_box_count: usize,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct SpaceUsedByFloats {
    pub left: CssPixels,
    pub right: CssPixels,
}

fn breaks_between_graphemes(style: StyleValues<'_>) -> bool {
    style.overflow_wrap() == overflow_wrap::ANYWHERE || style.word_break() == word_break::BREAK_WORD
}

#[derive(Clone, Copy, Default)]
pub(crate) struct TextIndentation {
    inline_size: CssPixels,
    hanging: bool,
    each_line: bool,
}

impl TextIndentation {
    pub(crate) fn of(style: StyleValues<'_>, inline_basis: CssPixels) -> Self {
        Self {
            inline_size: style.text_indent().to_px(inline_basis),
            hanging: style.text_indent_hanging(),
            each_line: style.text_indent_each_line(),
        }
    }

    pub(crate) fn indent_of_line(self, is_first_line: bool, follows_forced_break: bool) -> CssPixels {
        let indents = is_first_line || (self.each_line && follows_forced_break);
        if indents != self.hanging {
            self.inline_size
        } else {
            CssPixels::default()
        }
    }
}

#[derive(Clone, Copy, Default)]
struct FloatsBesideLine {
    side_of_latest_float_is_left: Option<bool>,
    clear_of_first_float: u8,
    inline_size_of_floats_since_latest_clearance: CssPixels,
    largest_inline_size_of_floats_before_a_clearance: CssPixels,
    inline_size_of_floats_before_first_clearance: Option<CssPixels>,
}

impl FloatsBesideLine {
    fn is_empty(&self) -> bool {
        self.side_of_latest_float_is_left.is_none()
    }

    fn place(&mut self, floats_to_the_left: bool, clear: u8, margin_box_inline_size: CssPixels) {
        let clears_latest_float = match self.side_of_latest_float_is_left {
            Some(true) => intrinsic_sizing::clears_left_floats(clear),
            Some(false) => intrinsic_sizing::clears_right_floats(clear),
            None => {
                self.clear_of_first_float = clear;
                false
            }
        };
        if clears_latest_float {
            let inline_size_of_cleared_floats = std::mem::take(&mut self.inline_size_of_floats_since_latest_clearance);
            self.largest_inline_size_of_floats_before_a_clearance = self
                .largest_inline_size_of_floats_before_a_clearance
                .max(inline_size_of_cleared_floats);
            self.inline_size_of_floats_before_first_clearance
                .get_or_insert(inline_size_of_cleared_floats);
        }
        self.inline_size_of_floats_since_latest_clearance += margin_box_inline_size.max(CssPixels::default());
        self.side_of_latest_float_is_left = Some(floats_to_the_left);
    }

    fn largest_inline_size_beside_one_line(&self) -> CssPixels {
        self.largest_inline_size_of_floats_before_a_clearance
            .max(self.inline_size_of_floats_since_latest_clearance)
    }

    fn beside_first_line(&self, inline_size_of_content: CssPixels) -> FirstLineMaxContent {
        FirstLineMaxContent {
            inline_size_of_content,
            inline_size_of_floats_before_first_clearance: self
                .inline_size_of_floats_before_first_clearance
                .unwrap_or(self.inline_size_of_floats_since_latest_clearance),
            clear_of_first_float: self.clear_of_first_float,
        }
    }
}

#[derive(Default)]
pub(super) struct LinesWithoutLineBoxes {
    breaks_at_every_opportunity: bool,
    text_indentation: TextIndentation,
    greatest_inline_size: CssPixels,
    first_line: Option<FirstLineMaxContent>,
    inline_size: CssPixels,
    has_fragments: bool,
    ends_in_whitespace: bool,
    trailing_whitespace_inline_size: CssPixels,
    follows_forced_break: bool,
    floats_beside_line: FloatsBesideLine,
}

impl LinesWithoutLineBoxes {
    fn indented_by(breaks_at_every_opportunity: bool, text_indentation: TextIndentation) -> Self {
        Self {
            breaks_at_every_opportunity,
            text_indentation,
            ..Self::default()
        }
    }

    pub(super) fn append_atomic_inline(&mut self, border_box_inline_size: CssPixels, container_wraps: bool) {
        if container_wraps && self.breaks_before_next_fragment() {
            self.finish();
        }
        self.append(border_box_inline_size, false, CssPixels::default());
    }

    pub(super) fn finish_measurement(mut self) -> CssPixels {
        self.finish();
        self.greatest_inline_size
    }

    fn finish_measurement_and_first_line(mut self) -> (CssPixels, FirstLineMaxContent) {
        self.finish();
        (self.greatest_inline_size, self.first_line.unwrap_or_default())
    }

    fn is_empty_or_ends_in_whitespace(&self) -> bool {
        !self.has_fragments || self.ends_in_whitespace
    }

    fn breaks_before_next_fragment(&self) -> bool {
        self.breaks_at_every_opportunity && self.has_fragments
    }

    fn start_line_if_empty(&mut self) {
        if !self.has_fragments {
            self.inline_size += self
                .text_indentation
                .indent_of_line(self.first_line.is_none(), self.follows_forced_break);
        }
    }

    fn append(
        &mut self,
        border_box_inline_size: CssPixels,
        ends_in_whitespace: bool,
        trailing_whitespace_inline_size: CssPixels,
    ) {
        self.start_line_if_empty();
        self.inline_size += border_box_inline_size;
        self.has_fragments = true;
        self.ends_in_whitespace = ends_in_whitespace;
        // NB: Adjacent all-space chunks form one trailing sequence, including across inline boundaries.
        if ends_in_whitespace && border_box_inline_size == trailing_whitespace_inline_size {
            self.trailing_whitespace_inline_size += trailing_whitespace_inline_size;
        } else {
            self.trailing_whitespace_inline_size = trailing_whitespace_inline_size;
        }
    }

    fn note_line(&mut self, inline_size: CssPixels) {
        let floats_beside_line = std::mem::take(&mut self.floats_beside_line);
        self.greatest_inline_size = self
            .greatest_inline_size
            .max(inline_size + floats_beside_line.largest_inline_size_beside_one_line());
        self.first_line
            .get_or_insert(floats_beside_line.beside_first_line(inline_size.max(CssPixels::default())));
        self.inline_size = CssPixels::default();
        self.has_fragments = false;
        self.ends_in_whitespace = false;
        self.trailing_whitespace_inline_size = CssPixels::default();
    }

    fn finish(&mut self) {
        if self.has_fragments || !self.floats_beside_line.is_empty() {
            self.note_line(self.inline_size - self.trailing_whitespace_inline_size);
            self.follows_forced_break = false;
        }
    }

    fn place_float_beside_line(&mut self, floats_to_the_left: bool, clear: u8, margin_box_inline_size: CssPixels) {
        self.floats_beside_line
            .place(floats_to_the_left, clear, margin_box_inline_size);
    }

    fn note_float_on_line_of_its_own(&mut self, margin_box_inline_size: CssPixels) {
        self.greatest_inline_size = self.greatest_inline_size.max(margin_box_inline_size);
    }

    fn force_break(&mut self) {
        self.start_line_if_empty();
        self.note_line(self.inline_size - self.trailing_whitespace_inline_size);
        self.follows_forced_break = true;
    }

    fn append_block_level_box(&mut self, margin_box_inline_size: CssPixels) {
        self.finish();
        self.first_line.get_or_insert_default();
        self.note_line(margin_box_inline_size);
        self.follows_forced_break = false;
    }
}

struct ContentSizingLines {
    lines: LinesWithoutLineBoxes,
    container_wraps: bool,
    pending_start_margin: CssPixels,
    pending_start_border: CssPixels,
    pending_start_padding: CssPixels,
    previous_text_item_breaks_between_graphemes: bool,
}

impl ContentSizingLines {
    fn new(lines: LinesWithoutLineBoxes, container_wraps: bool) -> Self {
        Self {
            lines,
            container_wraps,
            pending_start_margin: CssPixels::default(),
            pending_start_border: CssPixels::default(),
            pending_start_padding: CssPixels::default(),
            previous_text_item_breaks_between_graphemes: false,
        }
    }

    fn measures_min_content(&self) -> bool {
        self.lines.breaks_at_every_opportunity
    }

    fn append(&mut self, context: &InlineFormattingContext<'_>, items: &[inline_level_iterator::Item], index: usize) {
        let item = &items[index];
        if item.type_ == inline_level_iterator::ItemType::FloatingElement {
            self.append_float(context, item);
            return;
        }
        let remaining_items = &items[index + 1..];
        let previous_text_item_breaks_between_graphemes =
            std::mem::take(&mut self.previous_text_item_breaks_between_graphemes);
        let line_is_empty_or_ends_in_whitespace = self.lines.is_empty_or_ends_in_whitespace();
        if item.is_collapsible_whitespace && line_is_empty_or_ends_in_whitespace {
            if self.lines.breaks_before_next_fragment()
                && matches!(
                    context.text_item_wrap_opportunity(
                        item,
                        context.style(context.parent_node(item.node)),
                        true,
                        remaining_items,
                        line_is_empty_or_ends_in_whitespace,
                        false,
                    ),
                    TextItemWrapOpportunity::BeforeWhitespaceSequence(_)
                )
            {
                self.lines.finish();
            }
            self.pending_start_margin += item.margin_start;
            self.pending_start_border += item.border_start;
            self.pending_start_padding += item.padding_start;
            return;
        }
        let content_inline_size = if self.measures_min_content() {
            item.min_content_inline_size.unwrap_or(item.inline_size)
        } else {
            item.inline_size
        };
        if item.type_ == inline_level_iterator::ItemType::BlockLevelBox {
            self.lines.append_block_level_box(line_box::inline_advance(
                item.margin_start,
                item.border_start + item.padding_start,
                content_inline_size,
                item.padding_end + item.border_end,
                item.margin_end,
            ));
            return;
        }
        let margin_start = item.margin_start + std::mem::take(&mut self.pending_start_margin);
        let border_start = item.border_start + std::mem::take(&mut self.pending_start_border);
        let padding_start = item.padding_start + std::mem::take(&mut self.pending_start_padding);
        let border_box_inline_size = line_box::inline_advance(
            margin_start,
            border_start + padding_start,
            content_inline_size,
            item.padding_end + item.border_end,
            item.margin_end,
        );
        match item.type_ {
            inline_level_iterator::ItemType::ForcedBreak => self.lines.force_break(),
            inline_level_iterator::ItemType::Element => {
                self.lines
                    .append_atomic_inline(border_box_inline_size, self.container_wraps);
            }
            // An absolutely positioned box only takes a static position, which the pending inline edges attach to.
            inline_level_iterator::ItemType::AbsolutelyPositionedElement => {}
            inline_level_iterator::ItemType::BlockLevelBox | inline_level_iterator::ItemType::FloatingElement => {
                unreachable!(
                    "content sizing places floats beside lines and sizes block-level boxes as lines of their own"
                )
            }
            inline_level_iterator::ItemType::Text => {
                let style = context.style(context.parent_node(item.node));
                // https://drafts.csswg.org/css-text-3/#hanging
                // Glyphs that conditionally hang are not taken into account when computing min-content sizes
                // and any sizes derived thereof, but they are taken into account for max-content sizes and any sizes
                // derived thereof.
                let trailing_whitespace_inline_size = if matches!(
                    style.white_space_collapse(),
                    white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
                ) || (self.measures_min_content()
                    && style.white_space_collapse() == white_space_collapse::PRESERVE
                    && style.text_wrap_mode() == text_wrap_mode::WRAP)
                {
                    item.trailing_whitespace.inline_size
                } else {
                    CssPixels::default()
                };
                if style.text_wrap_mode() == text_wrap_mode::WRAP {
                    let is_whitespace = item.is_collapsible_whitespace || item.is_ascii_whitespace(context);
                    let breaks_between_graphemes = !is_whitespace && breaks_between_graphemes(style);
                    let opportunity = if self.lines.breaks_before_next_fragment() {
                        let can_break_after_previous_grapheme_broken_item =
                            previous_text_item_breaks_between_graphemes && !breaks_between_graphemes;
                        context.text_item_wrap_opportunity(
                            item,
                            style,
                            is_whitespace,
                            remaining_items,
                            line_is_empty_or_ends_in_whitespace || can_break_after_previous_grapheme_broken_item,
                            false,
                        )
                    } else {
                        TextItemWrapOpportunity::None
                    };
                    if is_whitespace {
                        if matches!(opportunity, TextItemWrapOpportunity::BeforeWhitespaceSequence(_)) {
                            self.lines.finish();
                            return;
                        }
                        if opportunity == TextItemWrapOpportunity::BeforeText {
                            self.lines.finish();
                        }
                        self.lines
                            .append(border_box_inline_size, true, trailing_whitespace_inline_size);
                        return;
                    }
                    if breaks_between_graphemes && self.measures_min_content() {
                        self.previous_text_item_breaks_between_graphemes = true;
                        self.append_text_broken_between_graphemes(
                            context,
                            item,
                            style,
                            margin_start,
                            border_start + padding_start,
                            trailing_whitespace_inline_size,
                        );
                        return;
                    }
                    if opportunity == TextItemWrapOpportunity::BeforeText {
                        self.lines.finish();
                    }
                }
                self.lines.append(
                    border_box_inline_size,
                    item.ends_in_ascii_space(context),
                    trailing_whitespace_inline_size,
                );
            }
        }
    }

    fn append_float(&mut self, context: &InlineFormattingContext<'_>, item: &inline_level_iterator::Item) {
        let margin_box_inline_size = |content_inline_size| {
            line_box::inline_advance(
                item.margin_start,
                item.border_start + item.padding_start,
                content_inline_size,
                item.padding_end + item.border_end,
                item.margin_end,
            )
        };
        if self.measures_min_content() {
            self.lines.note_float_on_line_of_its_own(margin_box_inline_size(
                item.min_content_inline_size.unwrap_or(item.inline_size),
            ));
            return;
        }
        let style = context.style(item.node);
        self.lines.place_float_beside_line(
            intrinsic_sizing::floats_to_the_left(style),
            style.clear(),
            margin_box_inline_size(item.inline_size),
        );
    }

    fn append_text_broken_between_graphemes(
        &mut self,
        context: &InlineFormattingContext<'_>,
        item: &inline_level_iterator::Item,
        style: StyleValues<'_>,
        margin_start: CssPixels,
        border_and_padding_start: CssPixels,
        trailing_whitespace_inline_size: CssPixels,
    ) {
        let text_content = context.callbacks.text_content(item.node);
        let cluster_inline_sizes = context
            .callbacks
            .with_grapheme_segmenter(item.node, |grapheme_segmenter| {
                item.grapheme_cluster_inline_sizes(
                    &text_content.text,
                    grapheme_segmenter,
                    style.letter_spacing().to_double() as f32,
                    style.word_spacing().to_double() as f32,
                )
            });
        let border_and_padding_end = item.padding_end + item.border_end;
        if border_and_padding_start + item.inline_size + border_and_padding_end > CssPixels::default() {
            self.lines.finish();
        }
        let last_cluster_index = cluster_inline_sizes.len() - 1;
        for (index, cluster_inline_size) in cluster_inline_sizes.into_iter().enumerate() {
            let leading = if index == 0 {
                margin_start + border_and_padding_start
            } else {
                CssPixels::default()
            };
            if index < last_cluster_index {
                self.lines
                    .append(leading + cluster_inline_size, false, CssPixels::default());
                self.lines.finish();
            } else {
                self.lines.append(
                    leading + cluster_inline_size + border_and_padding_end + item.margin_end,
                    item.ends_in_ascii_space(context),
                    trailing_whitespace_inline_size,
                );
            }
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TextItemWrapOpportunity {
    None,
    BeforeWhitespaceSequence(CssPixels),
    BeforeText,
}

pub(crate) fn line_physical_horizontal_extent(line: &line_box::LineBoxData) -> CssPixels {
    if let Some(ellipsis) = line.fragments.last().filter(|fragment| fragment.is_block_ellipsis) {
        return line.inline_length - ellipsis.inline_length;
    }
    line.inline_length
}

// https://drafts.csswg.org/css-overflow-3/#ellipsing-details
// Ellipsing only affects rendering and must not affect layout
// NB: So the content that text-overflow elided still extends the scrollable overflow. This is where the line's content
//     would be without the ellipsis: overflowing its line box from the inline start edge.
pub(crate) fn elided_content_rect(line: &line_box::LineBoxData) -> Option<FfiCssPixelRect> {
    let elision = line.text_overflow_elision?;
    let inline_length = elision.inline_length;
    let ellipsis_index = line
        .fragments
        .iter()
        .position(|fragment| fragment.has_text_overflow_ellipsis)?;
    let fragment = &line.fragments[ellipsis_index];
    let line_box_inline_start = fragment.inline_offset - elision.ellipsis_fragment_start;
    let content_inline_start = if line.direction == direction::RTL {
        line_box_inline_start + line.original_available_inline_size.to_px_or_zero() - inline_length
    } else {
        line_box_inline_start
    };
    let content_inline_shift = content_inline_start - line_box_inline_start;
    let to_rect = |inline_start, block_start, inline_size, block_size, relpos_delta: FfiCssPixelPoint| {
        let (x, y) = geometry::to_physical(line.writing_mode, inline_start, block_start);
        let (width, height) = geometry::to_physical(line.writing_mode, inline_size, block_size);
        CssPixelRect {
            x: x + relpos_delta.x,
            y: y + relpos_delta.y,
            width,
            height,
        }
    };
    let mut rect = to_rect(
        content_inline_start,
        fragment.block_offset,
        inline_length,
        fragment.block_length,
        FfiCssPixelPoint::default(),
    );
    // NB: Like the fragments that were not elided, each elided fragment's content moves with its own relative
    //     positioning, so unite the elided content of each one where it would be without the ellipsis.
    for (index, elided) in line.fragments.iter().enumerate().skip(ellipsis_index) {
        let inline_size = if index == ellipsis_index {
            elision.ellipsis_fragment_inline_length
        } else {
            elided.inline_length
        };
        rect.unite(to_rect(
            elided.inline_offset + content_inline_shift,
            elided.block_offset,
            inline_size,
            elided.block_length,
            elided.relpos_delta,
        ));
    }
    Some(rect.into())
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
    rect.y = line.physical_vertical_end() - line.physical_vertical_extent();
    rect.height = line.physical_vertical_extent();
    rect
}
