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
                let used = context.used(fragment.layout_node);
                inline_start -= used.margin_left.get() + used.border_box_left(false);
                inline_end += used.margin_right.get() + used.border_box_right(false);
            }

            let interrupting_block = if interrupting {
                let block_used = context.used(fragment.layout_node);
                let collapsed = block_used.uses_collapsing_borders_model.get();
                let (border_box_inline_low, border_box_block_low, border_box_inline_size, border_box_block_size) = (
                    block_used.border_box_left(collapsed),
                    block_used.border_box_top(collapsed),
                    block_used.border_box_inline_size(collapsed),
                    block_used.border_box_block_size(collapsed),
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
        let used = context.used(node);
        let reversed = context.facts(node).inline_axis_is_reverse();
        let (
            border_padding_low,
            border_padding_high,
            border_padding_block_low,
            border_padding_block_high,
            margin_low,
            margin_high,
        ) = (
            used.border_box_left(false),
            used.border_box_right(false),
            used.border_box_top(false),
            used.border_box_bottom(false),
            used.margin_left.get(),
            used.margin_right.get(),
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
                used,
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
    used: &UsedValues,
    inline_axis_is_reverse: bool,
    container_inline_axis_is_reverse: bool,
) -> Option<CssPixelRect> {
    let first = corners.first?;
    let last = corners.last?;

    let (border_inline_low, border_inline_high, border_block_low, border_block_high) = (
        used.border_left.get(),
        used.border_right.get(),
        used.border_top.get(),
        used.border_bottom.get(),
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
/// the first ancestor that is not inline-flow.
pub(crate) fn accumulated_relative_insets_from_inline_ancestor_chain(
    records: &RunRecords,
    callbacks: &LayoutPass<'_>,
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
    pub(crate) layout_mode: LayoutMode,
    pub(crate) input: LayoutInput,
    pub(crate) callbacks: LayoutPass<'context>,
    pub(crate) parent: &'context block_formatting_context::BlockFormattingContext<'context>,
    pub(crate) containing_used_values: &'context UsedValues,
    pub(crate) fragmented_inlines_in_pre_order: Vec<Node>,
    pub(crate) automatic_content_inline_size: CssPixels,
    pub(crate) automatic_content_block_size: CssPixels,
    pub(crate) current_line_start: Cell<fragmentation::LinePosition>,
    inline_boxes_continued_from_previous_fragment: Vec<Node>,
    inline_boxes_continued_in_next_fragment: RefCell<Vec<Node>>,
    block_axis_float_clearance: Cell<CssPixels>,
}

impl<'context> InlineFormattingContext<'context> {
    pub(crate) fn new_with_rust_parent(
        run: &'context FormattingContextRun<'context>,
        containing_block: Node,
        layout_mode: LayoutMode,
        input: LayoutInput,
        callbacks: LayoutPass<'context>,
        parent: &'context block_formatting_context::BlockFormattingContext<'context>,
    ) -> Self {
        let containing_used_values = run.records.used_values(containing_block);
        containing_used_values.ensure_line_data();
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
            automatic_content_block_size: CssPixels::default(),
            current_line_start: Cell::new(fragmentation::LinePosition::default()),
            inline_boxes_continued_from_previous_fragment: Vec::new(),
            inline_boxes_continued_in_next_fragment: RefCell::new(Vec::new()),
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
            fragments.discard_unplaced_subtree(node, self.used(node));
        }
        self.used(node).is_invisible_for_line_clamp.set(true);
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
        self.containing_used_values.building_line_data()
    }

    pub(crate) fn line_data_mut(&self) -> RefMut<'_, used_values::LineData> {
        self.containing_used_values.building_line_data_mut()
    }

    pub(crate) fn containing_used(&self) -> &'context UsedValues {
        self.containing_used_values
    }

    #[track_caller]
    pub(crate) fn used(&self, node: Node) -> &'context UsedValues {
        self.run.records.used_values(node)
    }

    pub(crate) fn create_used_values(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> &'context UsedValues {
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

    fn content_box_position_in_bfc_root(&self) -> FfiCssPixelPoint {
        let position = self
            .input
            .content_box_position_in_bfc_root
            .expect("inline layout requires the containing block position in the BFC root");
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
        let containing_block_offset_in_root = self.content_box_position_in_bfc_root().y;
        let next = self
            .parent
            .next_float_band_block_start_after(containing_block_offset_in_root + block_offset);
        next.map(|next| next - containing_block_offset_in_root)
    }

    fn layout_inside(&self, node: Node, available_space: AvailableSpace) -> DerivedBaselines {
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

    pub(crate) fn dimension_box_on_line(&self, node: Node) -> DerivedBaselines {
        let available_space = self.input.available_space;
        let facts = self.facts(node);
        // Any fragmented inline box should have generated line box fragments already.
        if facts.is_fragmented_inline() {
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

    #[inline]
    pub(crate) fn sizing(&self) -> sizing_context::SizingContext<'context> {
        self.parent.sizing()
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
            let line = line_box::LineBoxData {
                start: fragmentation::LinePosition::default(),
                record: *retained_line,
                fragments: previous.fragments[fragment_start..fragment_end]
                    .iter()
                    .map(|fragment| line_box_fragment::LineBoxFragmentData::with_record(fragment.clone(), None))
                    .collect(),
                static_position_markers: Vec::new(),
                inline_box_baselines: Vec::new(),
                text_overflow_elision: None,
            };
            fragment_start = fragment_end;
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
                    || fragment.content_baselines != Some(item.content_baselines)
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
        breaks_between_graphemes(style)
            || (style.overflow_wrap() == overflow_wrap::BREAK_WORD
                && self.input.available_space.inline_size != AvailableSize::MinContent)
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

    pub(crate) fn generate_line_boxes(&mut self, mut iterator: inline_level_iterator::InlineLevelIterator) {
        self.fragmented_inlines_in_pre_order = iterator.fragmented_inlines_in_pre_order(self);
        let fragmentation = self.parent.line_root_fragmentation(self.containing_block);
        let resume = fragmentation.and_then(|fragmentation| fragmentation.resume);
        let (reused_lines, reused_item_count) = if self.parent.has_line_clamp() || resume.is_some() {
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
        let mut pending_resume_offset = resume.map(|resume| resume.offset_in_node);
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

        let mut leading_margin = CssPixels::default();
        let mut leading_border = CssPixels::default();
        let mut leading_padding = CssPixels::default();
        let mut absolute_boxes = Vec::new();

        let mut previous_text_item_allows_overflow_break_after = false;
        let mut state_at_clamp_point = None;
        let mut items_after_clamp_point = Vec::new();
        let mut floating_boxes_after_clamp_point = Vec::new();
        let floating_box_count_before_lines = self.parent.floating_box_count();
        let mut line_limit = fragmentation
            .filter(|_| !self.parent.has_line_clamp())
            .and_then(|fragmentation| fragmentation.block_size)
            .filter(|_| {
                shares_items
                    || iterator.items().iter().all(|item| {
                        matches!(
                            item.type_,
                            inline_level_iterator::ItemType::Text
                                | inline_level_iterator::ItemType::Element
                                | inline_level_iterator::ItemType::ForcedBreak
                                | inline_level_iterator::ItemType::AbsolutelyPositionedElement
                        )
                    })
            })
            .map(|block_size| FragmentainerLineLimit {
                block_size,
                widows: self.style(self.containing_block).widows().max(1) as usize,
                checked_line_count: 0,
                lines_past_end: 0,
                minimum_line_block_size: None,
            });
        let mut stopped_at_line_limit = false;
        let mut consumed_item_nodes = (self.run.fragmentainer.is_some()
            && self.run.fragments.is_some()
            && !iterator.entered_inline_boxes().is_empty())
        .then(Vec::new);
        loop {
            if self.stops_at_line_clamp_point(&line_builder, &mut state_at_clamp_point) {
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
                    if shares_items && self.run.records.used_values_if_owned(item.node).is_none() {
                        self.create_used_values(item.node, self.input.containing_block_constraints);
                        self.dimension_box_on_line(item.node);
                    }
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
                    if self.stops_at_line_clamp_point(&line_builder, &mut state_at_clamp_point) {
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
                    if self.stops_at_line_clamp_point(&line_builder, &mut state_at_clamp_point) {
                        break;
                    }
                    self.parent.layout_interrupting_block_inside_inline_context(
                        self.run,
                        item.node,
                        self.containing_block,
                        self.input,
                        &mut line_builder,
                        state_at_clamp_point.is_some(),
                    );
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
                    self.create_used_values(item.node, self.input.containing_block_constraints)
                        .is_invisible_for_line_clamp
                        .set(state_at_clamp_point.is_some());
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
                                &mut line_builder,
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
                    if self.stops_at_line_clamp_point(&line_builder, &mut state_at_clamp_point) {
                        break;
                    }
                    line_builder.append_text_item(&mut item, style.line_height());
                }
            }
        }

        if let Some(state_at_clamp_point) = state_at_clamp_point {
            line_builder.update_last_line(false);
            for line in &mut self.line_data_mut().line_boxes[state_at_clamp_point.current_line_index..] {
                line.trim_trailing_whitespace();
            }
            let content_inline_size = self.containing_used().content_inline_size.get();
            let mut lines_after_clamp_point_rect = CssPixelRect::default();
            let mut atomic_inlines_after_clamp_point = Vec::new();
            for line in self.line_data().line_boxes[state_at_clamp_point.current_line_index..]
                .iter()
                .filter(|line| !line.is_empty())
            {
                lines_after_clamp_point_rect.unite(line_rect(line, content_inline_size).into());
                for fragment in line.visible_fragments() {
                    let (x, y) = fragment.offset();
                    let relative_insets = accumulated_relative_insets_from_inline_ancestor_chain(
                        self.run.records,
                        &self.callbacks,
                        self.callbacks.parent(fragment.layout_node),
                        self.containing_block,
                    );
                    let position = FfiCssPixelPoint {
                        x: x + relative_insets.offset_x,
                        y: y + relative_insets.offset_y,
                    };
                    if fragment.is_atomic_inline {
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
                self.used(node).is_invisible_for_line_clamp.set(true);
                formatting_context::place_child(self.run, node, position, None);
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
            if let Some(fragments) = self.run.fragments.as_deref() {
                for node in &skipped_atomic_inlines {
                    fragments.discard_unplaced_subtree(*node, self.used(*node));
                }
                for node in dropped_atomic_inlines.into_iter().chain(unconsumed_atomic_inlines) {
                    fragments.discard_unplaced_subtree(node, self.used(node));
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
                    formatting_context::place_child(self.run, node, FfiCssPixelPoint::default(), None);
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
                let relative_insets = accumulated_relative_insets_from_inline_ancestor_chain(
                    self.run.records,
                    &self.callbacks,
                    self.callbacks.parent(fragment.layout_node),
                    self.containing_block,
                );
                formatting_context::place_child(
                    self.run,
                    fragment.layout_node,
                    FfiCssPixelPoint {
                        x: x + relative_insets.offset_x,
                        y: y + relative_insets.offset_y,
                    },
                    Some(used_values::LineBoxFragmentCoordinate {
                        line_box_index: line_index,
                        fragment_index,
                    }),
                );
            }
        }

        if self.layout_mode == LayoutMode::Normal {
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
        }
        line_builder.remove_last_line_if_empty();
        self.parent.append_floating_boxes(floating_boxes_after_clamp_point);
    }

    pub(crate) fn run(&mut self) {
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
        }
        self.automatic_content_block_size = {
            let data = self.line_data();
            let lines = &data.line_boxes;
            if lines.iter().any(|line| line.has_block_level_box) {
                lines
                    .last()
                    .map_or(CssPixels::default(), |line| line.physical_vertical_end())
            } else {
                lines
                    .iter()
                    .fold(CssPixels::default(), |sum, line| sum + line.physical_vertical_extent())
            }
        };
        self.automatic_content_inline_size = self
            .parent
            .greatest_child_inline_size_including_floats(self.containing_block);
        let baselines = formatting_context::derive_baselines(self.run.records, &self.callbacks, self.containing_block);
        if self.containing_block == self.parent.root_box() {
            self.parent.record_derived_baselines_of_root_box(baselines);
        } else {
            formatting_context::store_derived_baselines(self.used(self.containing_block), baselines);
        }
    }

    fn compute_inline_box_pieces(&mut self) {
        let (pieces, inline_containing_block_rect_candidates) = compute(self);
        self.line_data_mut().inline_box_pieces = pieces;
        for candidate in inline_containing_block_rect_candidates {
            let relative_inset_chain = accumulated_relative_insets_from_inline_ancestor_chain(
                self.run.records,
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
            self.parent
                .break_line_root_before_line(self.containing_block, next_line_start);
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
                    let chain = accumulated_relative_insets_from_inline_ancestor_chain(
                        self.run.records,
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
        self.trailing_whitespace_inline_size = trailing_whitespace_inline_size;
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
                let trailing_whitespace_inline_size = if matches!(
                    style.white_space_collapse(),
                    white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
                ) {
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
        let cluster_inline_sizes = item.grapheme_cluster_inline_sizes(
            &text_content.text,
            text_content.grapheme_segmenter(),
            style.letter_spacing().to_double() as f32,
            style.word_spacing().to_double() as f32,
        );
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
