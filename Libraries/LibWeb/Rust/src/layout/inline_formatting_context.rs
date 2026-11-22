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
        !fragment.is_fully_truncated && fragment.inline_offset + fragment.inline_length > available_inline_size
    })?;
    let fragment = &mut line.fragments[index];
    let available = available_inline_size - fragment.inline_offset;
    let Some(glyphs) = fragment.glyphs.as_mut().filter(|_| available > CssPixels::default()) else {
        return (index > 0).then_some((index, fragment.inline_offset));
    };
    let placed: Vec<_> = glyphs.glyphs.placed().collect();
    let glyph_end = |placed: &libgfx_rust::text_layout::PlacedGlyph| {
        CssPixels::nearest_value_for_f32(placed.x + placed.glyph.advance)
    };
    let keep_from_end = fragment.current_insert_direction == direction::RTL && line_direction == direction::RTL;
    let (mut start, mut end) = if keep_from_end {
        (
            placed.partition_point(|placed| {
                fragment.record.inline_length - CssPixels::nearest_value_for_f32(placed.x) > available
            }),
            placed.len(),
        )
    } else {
        (0, placed.partition_point(|placed| glyph_end(placed) <= available))
    };
    while end > start && end < placed.len() && placed[end].glyph.length_in_code_units() == 0 {
        end -= 1;
    }
    while start < end && placed[start].glyph.length_in_code_units() == 0 {
        start += 1;
    }
    if start == end {
        return (index > 0).then_some((index, fragment.inline_offset));
    }
    let origin = CssPixels::nearest_value_for_f32(placed[start].x);
    let inline_size = glyph_end(&placed[end - 1]) - origin;
    glyphs.glyphs.keep_range(start..end);
    let origin_x = origin.to_double() as f32;
    glyphs.glyphs.update_positions(|_, x, _| *x -= origin_x);
    let length = glyphs.glyphs.iter().map(|glyph| glyph.length_in_code_units()).sum();
    glyphs.width = inline_size.to_double() as f32;
    if fragment.current_insert_direction == direction::RTL && !keep_from_end {
        fragment.start += fragment.length_in_code_units - length;
    }
    fragment.length_in_code_units = length;
    fragment.inline_length = inline_size;
    fragment.trailing_whitespace = Default::default();
    Some((index + 1, fragment.inline_offset + inline_size))
}

/// The ellipsis that text-overflow: ellipsis draws, shaped in the font its block container's style gives it.
struct TextOverflowEllipsis {
    glyphs: libgfx_rust::text_layout::GlyphBuffer,
    font: libgfx_rust::font::FontHandle,
    width: f32,
    length_in_code_units: usize,
}

impl TextOverflowEllipsis {
    fn of_block(context: &InlineFormattingContext<'_>) -> Self {
        // https://drafts.csswg.org/css-overflow-4/#ellipsing-details
        // "The ellipsis is styled and baseline-aligned according to the block."
        let style = context.style(context.containing_block);
        let font_list = style.frozen_font_list();
        let first_available_font = style
            .font()
            .first_available_font()
            .expect("layout read a font group that never received a frozen font list");
        let presentation = libgfx_rust::font::emoji_presentation_for_code_point(ELLIPSIS_CODE_POINT, None, None);
        // NB: Where the block's first available font has no glyph for U+2026 HORIZONTAL ELLIPSIS, three periods stand
        //     for it, rather than an ellipsis from whatever font the cascade falls back to.
        const ELLIPSIS: [u16; 1] = [ELLIPSIS_CODE_POINT as u16];
        const THREE_PERIODS: [u16; 3] = [b'.' as u16; 3];
        // NB: The cascade can hand back the first font for a code point that no font in it has, so the first font
        //     also has to have the glyph.
        let text: &[u16] = if first_available_font.contains_glyph(ELLIPSIS_CODE_POINT)
            && font_list.font_for_code_point(ELLIPSIS_CODE_POINT, presentation) == *first_available_font
        {
            &ELLIPSIS
        } else {
            &THREE_PERIODS
        };
        // NB: The periods come from whichever font of the block has a period for them, which needn't be its first
        //     available font: That one can be a font whose unicode-range leaves the period out.
        let code_point = u32::from(text[0]);
        let font = font_list.font_for_code_point(
            code_point,
            libgfx_rust::font::emoji_presentation_for_code_point(code_point, None, None),
        );
        let shaped = libgfx_rust::text_layout::shape_text(
            &font,
            text,
            libgfx_rust::text_layout::TextType::Common,
            0.0,
            style.letter_spacing().to_double() as f32,
            style.word_spacing().to_double() as f32,
        );
        Self {
            width: shaped.width(),
            glyphs: shaped.into_glyphs(),
            font,
            length_in_code_units: text.len(),
        }
    }

    fn inline_size(&self) -> CssPixels {
        CssPixels::nearest_value_for_f32(self.width)
    }

    /// The fragment that draws the ellipsis at `inline_offset` on `line`, next to the text of `layout_node` that it
    /// stands for, from `start`.
    fn into_fragment(
        self,
        context: &InlineFormattingContext<'_>,
        line: &line_box::LineBoxData,
        layout_node: Node,
        start: usize,
        inline_offset: CssPixels,
    ) -> line_box_fragment::LineBoxFragmentData {
        let style_source = context.containing_block;
        let style = context.style(style_source);
        let inline_size = self.inline_size();
        let mut fragment = line_box_fragment::LineBoxFragmentData::new(
            layout_node,
            start,
            self.length_in_code_units,
            inline_offset,
            CssPixels::default(),
            inline_size,
            line_builder::content_area_block_size(style),
            CssPixels::default(),
            line.direction,
            line.writing_mode,
            Some(line_box_fragment::GlyphData {
                glyphs: self.glyphs,
                font: self.font,
                text_type: line_box_fragment::GLYPH_TEXT_TYPE_COMMON,
                width: self.width,
                is_upright: false,
                derived_placements_end: None,
            }),
            line_box_fragment::FragmentBuildFacts {
                style_source,
                is_atomic_inline: false,
                white_space_collapse: style.white_space_collapse(),
                text_utf16: std::ptr::null(),
                text_length_in_code_units: 0,
            },
        );
        fragment.is_text_overflow_ellipsis = true;
        // NB: Placing a line aligns its fragments, so on a line that's placed already, the ellipsis sits on the line's
        //     baseline by itself. The fragments of a placed line are offset from the start of the formatting context,
        //     and its baseline from the start of the line.
        if line.uses_last_line_alignment.is_some() {
            let ascent = CssPixels::nearest_value_for_f32(style.font_ascent());
            fragment.baseline = ascent;
            fragment.block_offset = line.block_start + line.baseline - ascent;
        }
        fragment
    }
}

/// The glyphs of `glyphs` whose clusters lie wholly within what `keeps` accepts, as the start and end of each cluster
/// along the inline axis, with the cluster's extent. A cluster such as a base character and its combining marks is
/// kept or dropped as a whole.
fn glyphs_of_clusters_kept(
    glyphs: &libgfx_rust::text_layout::GlyphBuffer,
    keeps: impl Fn(f32, f32) -> bool,
) -> (libgfx_rust::text_layout::GlyphBuffer, Option<(f32, f32)>) {
    let mut cluster_extents = HashMap::<u32, (f32, f32)>::default();
    for placed in glyphs.placed() {
        let extent = cluster_extents
            .entry(placed.cluster_start)
            .or_insert((placed.x, placed.x + placed.glyph.advance));
        extent.0 = extent.0.min(placed.x);
        extent.1 = extent.1.max(placed.x + placed.glyph.advance);
    }
    let mut kept = libgfx_rust::text_layout::GlyphBuffer::default();
    let mut kept_extent = None::<(f32, f32)>;
    for placed in glyphs.placed() {
        let (start, end) = cluster_extents[&placed.cluster_start];
        if !keeps(start, end) {
            continue;
        }
        kept_extent = Some(kept_extent.map_or((start, end), |(kept_start, kept_end)| {
            (kept_start.min(start), kept_end.max(end))
        }));
        kept.push_placed(placed);
    }
    (kept, kept_extent)
}

/// The cluster of `glyphs` that's farthest toward the line's start edge: its left end, or its right end where
/// `from_right` is set. Its start and end along the inline axis.
fn start_edge_cluster(glyphs: &libgfx_rust::text_layout::GlyphBuffer, from_right: bool) -> Option<(f32, f32)> {
    let mut extent = None::<(f32, f32)>;
    for placed in glyphs.placed() {
        let (start, end) = (placed.x, placed.x + placed.glyph.advance);
        let is_farther = extent.is_none_or(
            |(kept_start, kept_end)| {
                if from_right { end > kept_end } else { start < kept_start }
            },
        );
        if is_farther {
            extent = Some((start, end));
        }
    }
    extent
}

// https://drafts.csswg.org/css-overflow-4/#text-overflow
// "implementations must hide characters and atomic inline-level elements at the applicable edge(s) of the line as
// necessary to fit the ellipsis/string, and place the ellipsis/string immediately adjacent to the applicable edge(s)
// of the remaining inline content. The first character or atomic inline-level element on a line must be clipped
// rather than ellipsed."
fn apply_text_overflow_to_line(line: &mut line_box::LineBoxData, context: &InlineFormattingContext<'_>) {
    if !matches!(line.original_available_inline_size, AvailableSize::Definite(_)) {
        return;
    }
    let available_inline_size = line.original_available_inline_size.to_px_or_zero();
    if line.inline_length <= available_inline_size || line.fragments.is_empty() {
        return;
    }
    let ellipsis = TextOverflowEllipsis::of_block(context);
    // NB: The end edge of a right-to-left line is its left end. Its fragments are laid out from the left, in logical
    //     order, and placing the line aligns its right end with the right edge of the line box, past its indent. So
    //     on a line that's placed already, what overflows is left of where the line box starts; on one that isn't,
    //     it's whatever is more than the available inline size, less the indent, away from its right end. A line
    //     whose items are ordered right to left is mirrored as it's placed, so until then, its end is on its right
    //     like that of any other line.
    let is_placed = line.uses_last_line_alignment.is_some();
    if line.direction == direction::RTL && (is_placed || !context.atomic_items_are_right_to_left) {
        let (left_edge, shift) = if is_placed {
            (line.placed_inline_start, CssPixels::default())
        } else {
            let overflow = line.inline_length - available_inline_size;
            (overflow + line.indent, overflow)
        };
        apply_text_overflow_at_left_edge(line, context, ellipsis, left_edge, shift, available_inline_size);
    } else {
        let right_edge = if is_placed {
            line.placed_inline_start + available_inline_size
        } else {
            available_inline_size
        };
        apply_text_overflow_at_right_edge(line, context, ellipsis, right_edge, available_inline_size);
    }
}

/// Where a fragment's border box starts and ends on its line: The fragment of an atomic inline is its content box.
fn border_box_inline_extent(fragment: &line_box_fragment::LineBoxFragmentData) -> (CssPixels, CssPixels) {
    let start = fragment.inline_offset;
    let end = start + fragment.inline_length;
    match fragment.box_cells.filter(|_| fragment.is_atomic_inline) {
        Some(cells) => (
            start - cells.border_left - cells.padding_left,
            end + cells.padding_right + cells.border_right,
        ),
        None => (start, end),
    }
}

/// Hides a fragment that text-overflow elides, and the box of an atomic inline with it, since that box paints itself.
fn hide_fragment_for_text_overflow(
    fragment: &mut line_box_fragment::LineBoxFragmentData,
    context: &InlineFormattingContext<'_>,
) {
    fragment.is_fully_truncated = true;
    fragment.is_hidden_by_text_overflow = true;
    if fragment.is_atomic_inline {
        context.hide_atomic_inline(fragment.layout_node);
    }
}

// https://drafts.csswg.org/css-overflow-4/#text-overflow
// "For the ellipsis and string values, implementations must hide characters and atomic inline-level elements at the
// applicable edge(s) of the line as necessary to fit the ellipsis/string, and place the ellipsis/string immediately
// adjacent to the applicable edge(s) of the remaining inline content. The first character or atomic inline-level
// element on a line must be clipped rather than ellipsed."
fn apply_text_overflow_at_right_edge(
    line: &mut line_box::LineBoxData,
    context: &InlineFormattingContext<'_>,
    ellipsis: TextOverflowEllipsis,
    right_edge: CssPixels,
    available_inline_size: CssPixels,
) {
    // NB: Content can only reach as far as leaves room for the ellipsis before the edge.
    let content_right_limit = right_edge - ellipsis.inline_size();
    let mut line_has_visible_content = false;
    for index in 0..line.fragments.len() {
        let fragment_start = line.fragments[index].inline_offset;
        if border_box_inline_extent(&line.fragments[index]).1 <= content_right_limit {
            line_has_visible_content = true;
            continue;
        }
        let Some(glyph_data) = &line.fragments[index].glyphs else {
            if !line.fragments[index].is_atomic_inline {
                continue;
            }
            // NB: An atomic inline is hidden as a whole, unless it's the first content on the line, and the ellipsis
            //     then follows the content before it.
            let keeps_atomic_inline = !line_has_visible_content;
            let original_inline_length = line.fragments[index].inline_length;
            let ellipsis_offset = if keeps_atomic_inline {
                border_box_inline_extent(&line.fragments[index]).1
            } else {
                line.fragments[..index]
                    .iter()
                    .filter(|fragment| !fragment.is_fully_truncated)
                    .map(|fragment| border_box_inline_extent(fragment).1)
                    .max()
                    .unwrap_or_else(|| border_box_inline_extent(&line.fragments[index]).0)
                    .min(content_right_limit)
            };
            line.text_overflow_elision = Some(line_box::TextOverflowElision {
                inline_length: line.inline_length,
                ellipsis_fragment_start: fragment_start,
                ellipsis_fragment_inline_length: original_inline_length,
                ellipsis_fragment_inline_length_lost_on_left: None,
            });
            let first_hidden = if keeps_atomic_inline { index + 1 } else { index };
            for hidden in &mut line.fragments[first_hidden..] {
                hide_fragment_for_text_overflow(hidden, context);
            }
            line.fragments[index].has_text_overflow_ellipsis = true;
            let layout_node = line.fragments[index].layout_node;
            let ellipsis_fragment = ellipsis.into_fragment(context, line, layout_node, 0, ellipsis_offset);
            line.fragments.insert(index + 1, ellipsis_fragment);
            line.inline_length = available_inline_size;
            line.clamp_static_position_markers_to_inline_length();
            break;
        };
        let max_text_end = (content_right_limit - fragment_start).to_double() as f32;
        let (mut kept, mut kept_extent) = glyphs_of_clusters_kept(&glyph_data.glyphs, |_, end| end <= max_text_end);
        let keeps_first_character = kept_extent.is_none() && !line_has_visible_content;
        if keeps_first_character {
            let first = start_edge_cluster(&glyph_data.glyphs, false);
            (kept, kept_extent) = glyphs_of_clusters_kept(&glyph_data.glyphs, |start, _| {
                first.is_some_and(|(first_start, _)| start == first_start)
            });
        }
        let kept_end = kept_extent.map_or(0.0, |(_, end)| end);
        let original_inline_length = line.fragments[index].inline_length;
        let fragment = &mut line.fragments[index];
        let glyph_data = fragment.glyphs.as_mut().unwrap();
        glyph_data.glyphs = kept;
        glyph_data.width = kept_end;
        glyph_data.derived_placements_end = None;
        fragment.inline_length = CssPixels::nearest_value_for_f32(kept_end);
        fragment.has_text_overflow_ellipsis = true;
        let (layout_node, ellipsis_start) = (fragment.layout_node, fragment.start + fragment.length_in_code_units);
        let ellipsis_offset = if keeps_first_character {
            fragment_start + fragment.inline_length
        } else {
            (fragment_start + fragment.inline_length).min(content_right_limit)
        };
        line.text_overflow_elision = Some(line_box::TextOverflowElision {
            inline_length: line.inline_length,
            ellipsis_fragment_start: fragment_start,
            ellipsis_fragment_inline_length: original_inline_length,
            ellipsis_fragment_inline_length_lost_on_left: None,
        });
        for later in &mut line.fragments[index + 1..] {
            hide_fragment_for_text_overflow(later, context);
        }
        let ellipsis_fragment = ellipsis.into_fragment(context, line, layout_node, ellipsis_start, ellipsis_offset);
        line.fragments.insert(index + 1, ellipsis_fragment);
        line.inline_length = available_inline_size;
        line.clamp_static_position_markers_to_inline_length();
        break;
    }
}

/// `left_edge` is where what's left of the line starts, and `shift` is how far its fragments then move left, so that
/// placing the line leaves them there.
fn apply_text_overflow_at_left_edge(
    line: &mut line_box::LineBoxData,
    context: &InlineFormattingContext<'_>,
    ellipsis: TextOverflowEllipsis,
    left_edge: CssPixels,
    shift: CssPixels,
    available_inline_size: CssPixels,
) {
    let ellipsis_inline_size = ellipsis.inline_size();
    // NB: Content can only start as far left as leaves room for the ellipsis after the edge.
    let content_left_limit = left_edge + ellipsis_inline_size;
    let mut line_has_visible_content = false;
    for index in (0..line.fragments.len()).rev() {
        let fragment_start = line.fragments[index].inline_offset;
        if border_box_inline_extent(&line.fragments[index]).0 >= content_left_limit {
            line_has_visible_content = true;
            continue;
        }
        let Some(glyph_data) = &line.fragments[index].glyphs else {
            if !line.fragments[index].is_atomic_inline {
                continue;
            }
            // NB: An atomic inline is hidden as a whole, unless it's the first content on the line, and the ellipsis
            //     then precedes the content after it.
            let keeps_atomic_inline = !line_has_visible_content;
            let ellipsis_offset = if keeps_atomic_inline {
                border_box_inline_extent(&line.fragments[index]).0 - ellipsis_inline_size
            } else {
                line.fragments[index + 1..]
                    .iter()
                    .filter(|fragment| !fragment.is_fully_truncated)
                    .map(|fragment| border_box_inline_extent(fragment).0)
                    .min()
                    .unwrap_or_else(|| border_box_inline_extent(&line.fragments[index]).1)
                    .max(content_left_limit)
                    - ellipsis_inline_size
            };
            let original_inline_length = line.fragments[index].inline_length;
            let elided_inline_length = line.inline_length;
            let last_hidden = if keeps_atomic_inline { index } else { index + 1 };
            for hidden in &mut line.fragments[..last_hidden] {
                hide_fragment_for_text_overflow(hidden, context);
            }
            line.fragments[index].has_text_overflow_ellipsis = true;
            let layout_node = line.fragments[index].layout_node;
            let ellipsis_fragment = ellipsis.into_fragment(context, line, layout_node, 0, ellipsis_offset);
            line.fragments.insert(index, ellipsis_fragment);
            if shift != CssPixels::default() {
                for fragment in &mut line.fragments {
                    fragment.inline_offset -= shift;
                }
                for marker in &mut line.static_position_markers {
                    marker.inline_offset -= shift;
                }
            }
            line.text_overflow_elision = Some(line_box::TextOverflowElision {
                inline_length: elided_inline_length,
                ellipsis_fragment_start: line.fragments[index + 1].inline_offset,
                ellipsis_fragment_inline_length: original_inline_length,
                ellipsis_fragment_inline_length_lost_on_left: Some(CssPixels::default()),
            });
            line.inline_length = available_inline_size;
            line.clamp_static_position_markers_to_inline_length();
            break;
        };
        let min_text_start = (content_left_limit - fragment_start).to_double() as f32;
        let (mut kept, mut kept_extent) =
            glyphs_of_clusters_kept(&glyph_data.glyphs, |start, _| start >= min_text_start);
        let keeps_first_character = kept_extent.is_none() && !line_has_visible_content;
        if keeps_first_character {
            let first = start_edge_cluster(&glyph_data.glyphs, true);
            (kept, kept_extent) = glyphs_of_clusters_kept(&glyph_data.glyphs, |_, end| {
                first.is_some_and(|(_, first_end)| end == first_end)
            });
        }
        let original_inline_length = line.fragments[index].inline_length;
        let kept_start = kept_extent.map_or(original_inline_length.to_double() as f32, |(start, _)| start);
        let kept_start_px = CssPixels::nearest_value_for_f32(kept_start);
        let fragment = &mut line.fragments[index];
        kept.update_positions(|_, x, _| *x -= kept_start);
        let glyph_data = fragment.glyphs.as_mut().unwrap();
        glyph_data.glyphs = kept;
        glyph_data.width -= kept_start;
        glyph_data.derived_placements_end = None;
        fragment.inline_offset += kept_start_px;
        fragment.inline_length -= kept_start_px;
        fragment.has_text_overflow_ellipsis = true;
        let (layout_node, ellipsis_start) = (fragment.layout_node, fragment.start + fragment.length_in_code_units);
        let ellipsis_offset = if keeps_first_character {
            fragment.inline_offset - ellipsis_inline_size
        } else {
            (fragment.inline_offset - ellipsis_inline_size).max(left_edge)
        };
        let elided_inline_length = line.inline_length;
        for earlier in &mut line.fragments[..index] {
            hide_fragment_for_text_overflow(earlier, context);
        }
        let ellipsis_fragment = ellipsis.into_fragment(context, line, layout_node, ellipsis_start, ellipsis_offset);
        line.fragments.insert(index, ellipsis_fragment);
        if shift != CssPixels::default() {
            for fragment in &mut line.fragments {
                fragment.inline_offset -= shift;
            }
            for marker in &mut line.static_position_markers {
                marker.inline_offset -= shift;
            }
        }
        // NB: As for a line whose right end is elided, the line box starts at the cut fragment's offset less this one,
        //     wherever placing the line moves them.
        let cut_fragment = &line.fragments[index + 1];
        line.text_overflow_elision = Some(line_box::TextOverflowElision {
            inline_length: elided_inline_length,
            ellipsis_fragment_start: cut_fragment.inline_offset,
            ellipsis_fragment_inline_length: original_inline_length,
            ellipsis_fragment_inline_length_lost_on_left: Some(kept_start_px),
        });
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
    // NB: The code points after the first can make it the start of an emoji sequence.
    let mut code_points = char::decode_utf16(ellipsis_text.iter().copied())
        .map(|result| result.unwrap_or(char::REPLACEMENT_CHARACTER) as u32);
    let first_code_point = code_points.next().unwrap_or(char::REPLACEMENT_CHARACTER as u32);
    let next_code_point = code_points.next();
    let code_point_after_next = code_points.next();
    let presentation =
        libgfx_rust::font::emoji_presentation_for_code_point(first_code_point, next_code_point, code_point_after_next);
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
    // NB: The block overflow ellipsis takes the place of any text-overflow ellipsis on the line, and the fragment that
    //     text-overflow cut keeps what of its text still fits.
    line.fragments.retain(|fragment| !fragment.is_text_overflow_ellipsis);
    for fragment in &mut line.fragments {
        fragment.has_text_overflow_ellipsis = false;
    }
    line.trim_trailing_whitespace_before_block_ellipsis();
    let available_for_content = (available_inline_size - ellipsis_inline_size).max(CssPixels::default());
    let content_fits = line.inline_length <= available_for_content;
    let opportunity = if content_fits {
        None
    } else {
        line.fragments.iter().enumerate().rev().find_map(|(index, fragment)| {
            let trailing = fragment.trailing_whitespace;
            let inline_size = fragment.inline_length - trailing.inline_size;
            let inline_offset = fragment.inline_offset + inline_size;
            if fragment.has_soft_wrap_opportunity_after && inline_offset <= available_for_content {
                return Some((
                    index + 1,
                    inline_offset,
                    fragment.length_in_code_units - trailing.length_in_code_units,
                    inline_size,
                    None,
                ));
            }
            let offsets = &fragment.block_ellipsis_text.as_ref()?.soft_wrap_offsets;
            let style = context.style(fragment.style_source);
            let text_content = context.pass.text_content(fragment.layout_node);
            offsets.iter().rev().find_map(|offset| {
                if *offset <= fragment.start || *offset >= fragment.start + fragment.length_in_code_units {
                    return None;
                }
                let length = *offset - fragment.start;
                let (inline_size, glyphs) = fragment.reshape_prefix_for_block_ellipsis(
                    &text_content.text,
                    *offset,
                    style.letter_spacing().to_double() as f32,
                    style.word_spacing().to_double() as f32,
                );
                let inline_offset = fragment.inline_offset + inline_size;
                (inline_offset <= available_for_content)
                    .then(|| (index + 1, inline_offset, length, inline_size, Some(glyphs)))
            })
        })
    };
    let (first_displaced_fragment, retained_inline_size) = if content_fits {
        (line.fragments.len(), line.inline_length)
    } else if let Some((fragment_count, inline_offset, length, inline_size, reshaped_glyphs)) = opportunity {
        let last_retained_fragment = &mut line.fragments[fragment_count - 1];
        if last_retained_fragment.length_in_code_units > length {
            let mut retained_code_units = 0usize;
            if let Some(glyphs) = reshaped_glyphs {
                last_retained_fragment.glyphs = Some(glyphs);
            } else if let Some(glyphs) = &mut last_retained_fragment.glyphs {
                let retained_glyph_count = glyphs
                    .glyphs
                    .iter()
                    .take_while(|glyph| {
                        retained_code_units += glyph.length_in_code_units();
                        retained_code_units <= length
                    })
                    .count();
                glyphs.glyphs.truncate(retained_glyph_count);
                glyphs.width = inline_size.to_double() as f32;
            }
            last_retained_fragment.length_in_code_units = length;
            last_retained_fragment.inline_length = inline_size;
            last_retained_fragment.trailing_whitespace = Default::default();
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
        fragment.is_hidden_by_text_overflow = false;
        if fragment.is_atomic_inline {
            context.hide_atomic_inline(fragment.layout_node);
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
        line_builder::content_area_block_size(style),
        CssPixels::default(),
        line.direction,
        line.writing_mode,
        Some(line_box_fragment::GlyphData {
            glyphs: shaped_ellipsis.into_glyphs(),
            font,
            text_type: line_box_fragment::GLYPH_TEXT_TYPE_COMMON,
            width: ellipsis_width,
            is_upright: false,
            derived_placements_end: None,
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

pub(crate) fn apply_to_fragments(
    text_justify: u8,
    atomic_items_are_right_to_left: bool,
    line: &mut line_box::LineBoxData,
) {
    if text_justify == text_justify::NONE {
        return;
    }
    assert!(matches!(
        text_justify,
        text_justify::AUTO | text_justify::INTER_WORD | text_justify::INTER_CHARACTER
    ));

    let excess_inline_space = line.original_available_inline_size.to_px_or_zero() - line.inline_length;
    // https://drafts.csswg.org/css-text-3/#text-align-property
    // If (after justification, if any) the inline contents of a line box are too long to fit within it, then the
    // contents are start-aligned: any content that doesn't fit overflows the line box's end edge.
    if excess_inline_space < CssPixels::default() {
        return;
    }
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
    // Expand spaces in increasing inline-coordinate order, including when logical order was mirrored for placement.
    let fragment_count = line.fragments.len();
    for index in 0..fragment_count {
        let index = if atomic_items_are_right_to_left {
            fragment_count - 1 - index
        } else {
            index
        };
        let fragment = &mut line.fragments[index];
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
            for strut in &mut line.inline_box_struts {
                if strut.inline_offset > fragment.inline_offset {
                    strut.inline_offset += diff;
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
    interrupting_block: Option<((CssPixels, CssPixels), CandidateLineCorners)>,
    first_fragment_index: Option<u32>,
    fragment_count: u32,
    /// Whether the inline box is on the line by its strut alone, with no fragment of its content there.
    is_strut: bool,
}

impl PerLine {
    fn new(line_index: usize) -> Self {
        Self {
            line_index,
            contributions_inline_range: None,
            first_direct_fragment_block_start: None,
            max_direct_fragment_block_length: CssPixels::default(),
            interrupting_block: None,
            first_fragment_index: None,
            fragment_count: 0,
            is_strut: false,
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

fn note_contribution(line: &mut PerLine, inline_start: CssPixels, inline_end: CssPixels) {
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
            // NB: The text-overflow ellipsis is committed like any other fragment, so it takes an index, but it
            //     belongs to no inline box.
            if fragment.is_text_overflow_ellipsis {
                committed_fragment_index += 1;
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
                let extent_block_start = block_start - block.border_box_top(collapsed);
                // NB: A block-level box inside an inline box adds nothing to the inline extent of that inline box, so
                //     on the block's line, the inline box takes no more room than an empty piece at the line's start.
                let line_start = if context.facts(context.containing_block).inline_axis_is_reverse() {
                    context.inline_size_of_block_container_holding_lines()
                } else {
                    CssPixels::default()
                };
                Some((
                    position,
                    CandidateLineCorners {
                        inline_start: line_start,
                        inline_end: line_start,
                        block_start: extent_block_start,
                        block_end: extent_block_start + block.border_box_block_size(collapsed),
                    },
                ))
            } else {
                None
            };

            let mut direct = true;
            let mut previous: Option<usize> = None;
            let mut ancestor = context.inline_box_for_fragment(fragment.layout_node);
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
                    note_contribution(line, inline_start, inline_end);
                    if !fragment.is_atomic_inline {
                        line.first_direct_fragment_block_start.get_or_insert(block_start);
                        line.max_direct_fragment_block_length = line.max_direct_fragment_block_length.max(block_length);
                    }
                }
                per_node.first_line_with_content.get_or_insert(line_index);
                per_node.last_line_with_content = Some(line_index);
                direct = false;
                ancestor = context.nearest_fragmented_inline_ancestor(ancestor);
            }
        }
        if !line.takes_inline_box_struts() {
            continue;
        }
        // An inline box that's on the line by its strut alone gets a piece there too, as wide as nothing.
        for strut in &line.inline_box_struts {
            let node_index = *node_to_index.entry(strut.box_).or_insert_with(|| {
                per_nodes.push(PerNode {
                    node: strut.box_,
                    parent_index: None,
                    depth: nesting_depth(context, strut.box_),
                    lines: Vec::new(),
                    first_line_with_content: None,
                    last_line_with_content: None,
                });
                per_nodes.len() - 1
            });
            let per_node = &mut per_nodes[node_index];
            let per_line = ensure_line(per_node, line_index);
            if per_line.first_fragment_index.is_some() {
                continue;
            }
            per_line.is_strut = true;
            note_contribution(per_line, strut.inline_offset, strut.inline_offset);
            per_node.first_line_with_content.get_or_insert(line_index);
            per_node.last_line_with_content = Some(line_index);
        }
    }

    let without_fragments: Vec<_> = context
        .fragmented_inlines_in_pre_order
        .iter()
        .copied()
        .filter(|node| context.facts(*node).has_dom_node() && !node_to_index.contains_key(node))
        .collect();

    let mut empty_inline_positions = HashMap::<Node, (usize, CssPixels, CssPixels)>::default();
    if !without_fragments.is_empty() {
        for (line_index, line) in context.line_data().line_boxes.iter().enumerate() {
            for marker in &line.static_position_markers {
                let mut ancestor = context.nearest_fragmented_inline_ancestor(marker.box_);
                while !ancestor.is_invalid() {
                    if !node_to_index.contains_key(&ancestor) {
                        empty_inline_positions.entry(ancestor).or_insert((
                            line_index,
                            marker.inline_offset,
                            marker.block_offset,
                        ));
                    }
                    ancestor = context.nearest_fragmented_inline_ancestor(ancestor);
                }
            }
        }
    }

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
                .is_some_and(|fragments| fragments.any_pending_abspos_names_inline_containing_block(node));
        let mut corners = FirstAndLastContentLineCorners::default();
        // NB: Lines that hold only a block-level box inside the inline box don't count as its first or last line. They
        //     stand in for it only where it has no other lines.
        let mut corners_from_block_lines = FirstAndLastContentLineCorners::default();

        // NB: A box with content on some line, in this fragment or another, has its edges where that content is, so
        //     only an empty box is on the line that holds its strut alone, for its background, borders, and outline
        //     to be painted there and its border box to be hit there. Elsewhere, such a line holds a placeholder piece.
        let holds_content = lines.iter().any(|line| line.first_fragment_index.is_some())
            || context.inline_boxes_continued_from_previous_fragment.contains(&node)
            || context.inline_boxes_continued_in_next_fragment.borrow().contains(&node);
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
                        corners_from_block_lines.first.get_or_insert(extent);
                        corners_from_block_lines.last = Some(extent);
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
                    let height = line_builder::content_area_block_size(style);
                    let baseline = line_builder::LineBuilder::baseline_for_style(style, height);
                    (inline_box_baseline - baseline, height)
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
            // NB: A placeholder piece holds the content box of its inline box.
            let is_placeholder = line.is_strut && holds_content;
            let rect = if is_placeholder {
                FfiCssPixelRect {
                    x: contributions_inline_start,
                    y: content_block_start,
                    width: CssPixels::default(),
                    height: content_block_length,
                }
            } else {
                FfiCssPixelRect {
                    x: border_inline_start,
                    y: border_block_start,
                    width: border_inline_end - border_inline_start,
                    height: border_block_length,
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
                    present_edges: edge_bits(has_low_edge, has_high_edge),
                    is_geometry_only_placeholder: is_placeholder,
                },
                depth,
                discovery_index: node_index,
            });
            if node_is_inline_containing_block {
                let block_start = if context.style(context.containing_block).writing_mode() == writing_mode::VERTICAL_LR
                {
                    context.line_data().line_boxes[line.line_index]
                        .mirrored_block_offset(border_block_start, border_block_length)
                } else {
                    border_block_start
                };
                let source = CandidateLineCorners {
                    inline_start: border_inline_start,
                    inline_end: border_inline_end,
                    block_start,
                    block_end: block_start + border_block_length,
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
                );
            }
        }

        if corners.first.is_none() {
            corners = corners_from_block_lines;
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
        let style = context.style(node);
        let mut placeholder_rect = FfiCssPixelRect {
            height: style.line_height(),
            ..Default::default()
        };
        let mut placeholder_line_index = 0;
        if let Some(&(line_index, inline_offset, block_offset)) = empty_inline_positions.get(&node) {
            // An inline with only out-of-flow content still occupies its position on the line. Its child's marker
            // retains that position even though the inline has no in-flow fragment to supply its containing block.
            let data = context.line_data();
            let line = &data.line_boxes[line_index];
            placeholder_line_index = line_index as u32;
            placeholder_rect.x = inline_offset;
            if line.is_empty() {
                placeholder_rect.y = block_offset;
                placeholder_rect.height = CssPixels::default();
            } else {
                placeholder_rect.height = line_builder::content_area_block_size(style);
                placeholder_rect.y = line.block_start + line.baseline
                    - line_builder::LineBuilder::baseline_for_style(style, placeholder_rect.height);
            }
        }
        if collect_inline_containing_block_rects
            && context
                .run
                .fragments
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
                line_index: placeholder_line_index,
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
    pass: &LayoutPass<'_>,
    first_ancestor: Node,
    stop_at: Node,
    used_insets_of: impl Fn(Node) -> run_state::UsedInsets,
) -> InlineAncestorChainRelativeOffset {
    let mut result = InlineAncestorChainRelativeOffset::default();
    let mut ancestor = first_ancestor;
    while !ancestor.is_invalid() && ancestor != stop_at {
        let facts = NodeFacts::new(pass, ancestor);
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
        ancestor = pass.parent(ancestor);
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
            .filter(|line| line.counts_toward_widows_and_orphans())
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
    pub(crate) pass: LayoutPass<'context>,
    pub(crate) parent: &'context block_formatting_context::BlockFormattingContext<'context>,
    /// The lines this context lays out, which it hands back once they are laid out.
    line_data: RefCell<used_values::LineData>,
    pub(crate) fragmented_inlines_in_pre_order: Vec<Node>,
    pub(crate) current_line_start: Cell<fragmentation::LinePosition>,
    pub(crate) atomic_items_are_right_to_left: bool,
    inline_boxes_continued_from_previous_fragment: Vec<Node>,
    inline_boxes_continued_in_next_fragment: RefCell<Vec<Node>>,
    block_axis_float_clearance: Cell<CssPixels>,
    /// What the layout of each atomic inline hands its placement, which places it once its line is final.
    atomic_inline_fragment_arrivals: RefCell<HashMap<Node, fragment_tree::RunRootFragment>>,
    /// What placing each atomic inline this context laid out takes.
    placed_atomic_inlines: RefCell<HashMap<Node, formatting_context::PlacedChild>>,
    /// The break token of the containing block, once the fragmentainer stops before one of its lines.
    fragmentainer_stop_token: RefCell<Option<std::rc::Rc<fragmentation::BlockBreakToken>>>,
    lines_are_unbreakable: Cell<bool>,
    fragments_lines: Cell<bool>,
    positioned_floats: RefCell<Vec<(usize, block_formatting_context::PositionedFloat)>>,
    deferred_float_items: RefCell<Vec<(usize, Node)>>,
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
        pass: LayoutPass<'context>,
        parent: &'context block_formatting_context::BlockFormattingContext<'context>,
    ) -> Self {
        Self {
            run,
            containing_block: block_container.node,
            block_container,
            input,
            containing_block_position_in_bfc_root,
            pass,
            parent,
            line_data: RefCell::new(used_values::LineData::default()),
            fragmented_inlines_in_pre_order: Vec::new(),
            current_line_start: Cell::new(fragmentation::LinePosition::default()),
            atomic_items_are_right_to_left: false,
            inline_boxes_continued_from_previous_fragment: Vec::new(),
            inline_boxes_continued_in_next_fragment: RefCell::new(Vec::new()),
            block_axis_float_clearance: Cell::new(CssPixels::default()),
            atomic_inline_fragment_arrivals: RefCell::new(HashMap::default()),
            placed_atomic_inlines: RefCell::new(HashMap::default()),
            fragmentainer_stop_token: RefCell::new(None),
            lines_are_unbreakable: Cell::new(false),
            fragments_lines: Cell::new(false),
            positioned_floats: RefCell::new(Vec::new()),
            deferred_float_items: RefCell::new(Vec::new()),
            margins_collapse_through_interrupting_blocks: RefCell::new(HashMap::default()),
            entered_inline_boxes: RefCell::new(HashMap::default()),
            used_insets_of_floats: RefCell::new(HashMap::default()),
            floats_laid_out_ahead_of_lines: RefCell::new(HashMap::default()),
        }
    }

    fn keep_atomic_inline_fragment_arrival(
        &self,
        node: Node,
        fragment_arrival: Option<fragment_tree::RunRootFragment>,
    ) {
        let mut arrivals = self.atomic_inline_fragment_arrivals.borrow_mut();
        let previous = match fragment_arrival {
            Some(fragment_arrival) => arrivals.insert(node, fragment_arrival),
            None => arrivals.remove(&node),
        };
        debug_assert!(
            previous.is_none(),
            "an atomic inline is laid out once by a run before its placement"
        );
    }

    fn take_atomic_inline_fragment_arrival(&self, node: Node) -> Option<fragment_tree::RunRootFragment> {
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
                .show
                .hide_after_clamp_point();
            placed_child
        });
    }

    fn discard_unplaced_atomic_inline(&self, node: Node) {
        let fragment_arrival = self.take_atomic_inline_fragment_arrival(node);
        if let Some(fragments) = self.run.fragments {
            fragments.discard_unplaced_subtree(node, fragment_arrival);
        }
    }

    pub(crate) fn block_axis_float_clearance(&self) -> CssPixels {
        self.block_axis_float_clearance.get()
    }

    pub(crate) fn set_block_axis_float_clearance(&self, clearance: CssPixels) {
        self.block_axis_float_clearance.set(clearance);
    }

    /// Hides an atomic inline that a line clamp or text-overflow left out of its line.
    fn hide_atomic_inline(&self, node: Node) {
        self.discard_unplaced_atomic_inline(node);
        self.hide_placed_atomic_inline_for_line_clamp(node);
    }

    fn stops_at_line_clamp_point<'builder>(
        &'builder self,
        line_builder: &line_builder::LineBuilder<'builder, 'context>,
        state_at_clamp_point: &mut Option<LineBuildingState<'builder, 'context>>,
    ) -> bool {
        if line_builder.only_finds_line_breaks() || state_at_clamp_point.is_some() || !self.parent.line_clamp_reached()
        {
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
                apply_text_overflow_to_line(line, self);
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
        StyleValues::for_node(&self.pass, node)
    }

    #[inline]
    pub(crate) fn facts(&self, node: Node) -> NodeFacts<'_> {
        NodeFacts::new(&self.pass, node)
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
            &self.pass,
            first_ancestor,
            self.containing_block,
            used_insets_of_entered_inline_box,
        )
    }

    /// Starts holding `node`, an atomic inline-level box, which a run of its own lays out in the formatting context it
    /// establishes. Returns the cells the box starts out with.
    pub(crate) fn hold_atomic_inline(&self, node: Node) -> used_values::RootInputCells {
        used_values::cells_before_sizing(&self.pass, node, self.input.containing_block_constraints)
    }

    pub(crate) fn parent_node(&self, node: Node) -> Node {
        self.pass.parent(node)
    }

    pub(crate) fn first_child(&self, node: Node) -> Node {
        self.pass.first_child(node)
    }

    fn fragmented_inline_ancestors(&self, node: Node) -> Vec<Node> {
        let mut ancestors = Vec::new();
        let mut ancestor = self.inline_box_for_fragment(node);
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

    fn inline_box_for_fragment(&self, node: Node) -> Node {
        if !node.is_invalid() && self.facts(node).is_fragmented_inline() {
            node
        } else {
            self.nearest_fragmented_inline_ancestor(node)
        }
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

    /// How far floats reach into the start side of a line between these block offsets. A line laid out in an
    /// indefinite inline size, as for an intrinsic size, has no floats beside it to measure.
    pub(crate) fn start_side_float_intrusion(
        &self,
        block_start: CssPixels,
        block_end: CssPixels,
        line_direction: u8,
    ) -> CssPixels {
        if !matches!(self.input.available_space.inline_size, AvailableSize::Definite(_)) {
            return CssPixels::default();
        }
        let intrusion = self.intrusion_by_floats_into_containing_block(block_start, block_end);
        if line_direction == direction::RTL {
            intrusion.right
        } else {
            intrusion.left
        }
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
        let laid_out = formatting_context::layout_inside_child_with_input_cells(
            self.run,
            Some(self.parent),
            None,
            node,
            input,
            cells,
            self.parent.take_table_inline_layout_of(node),
            false,
            self.block_container.percentage_block_insets_have_definite_basis,
        );
        laid_out.report_percentage_block_size_dependency_of(self.run, node);
        let baselines = laid_out.baselines();
        let cells = laid_out.cells();
        self.keep_placed_atomic_inline(node, laid_out.placed_child());
        self.keep_atomic_inline_fragment_arrival(node, laid_out.into_fragment_arrival(self.run));
        (baselines, cells)
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
            .clear_floating_boxes(node, Some(self), None, self.containing_block_position_in_bfc_root)
            .is_some()
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
            || self.atomic_items_are_right_to_left
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
                inline_box_struts: Vec::new(),
                text_overflow_elision: None,
                block_level_box: None,
                uses_last_line_alignment: None,
                placed_inline_start: CssPixels::default(),
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
                    || fragment.content_baselines != Some(item.content_baselines())
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
        prepared: &prepared_inline_content::PreparedInlineContent,
    ) -> sizing_context::ContentInlineSizes {
        let containing_style = self.style(self.containing_block);
        let text_indentation =
            TextIndentation::of(containing_style, self.input.containing_block_constraints.inline_basis());
        let mut max_content_lines =
            ContentSizingLines::new(LinesWithoutLineBoxes::indented_by(false, text_indentation));
        let mut min_content_lines = ContentSizingLines::new(LinesWithoutLineBoxes::indented_by(true, text_indentation));
        for index in 0..items.len() {
            max_content_lines.append(self, items, prepared, index);
            min_content_lines.append(self, items, prepared, index);
        }
        max_content_lines.append_pending_start_edges();
        min_content_lines.append_pending_start_edges();
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
                block_size: false,
            },
        }
    }

    #[inline]
    #[allow(clippy::too_many_arguments)]
    fn text_item_wrap_opportunity(
        &self,
        prepared: &prepared_inline_content::PreparedInlineContent,
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
            return inline_level_iterator::whitespace_and_sequence_inline_size(self, prepared, item, remaining_items)
                .map_or(
                    TextItemWrapOpportunity::None,
                    TextItemWrapOpportunity::BeforeWhitespaceSequence,
                );
        }
        if item.can_break_before || line_ends_in_whitespace {
            TextItemWrapOpportunity::BeforeText
        } else if can_break_after_previous_overflow_item && !self.overflow_break_applies_to_style(style) {
            TextItemWrapOpportunity::AfterOverflowBreakableText
        } else {
            TextItemWrapOpportunity::None
        }
    }

    fn line_is_empty_or_ends_in_whitespace_that_wraps(&self, line: &line_box::LineBoxData) -> bool {
        line.fragments.last().is_none_or(|fragment| {
            fragment.ends_in_whitespace() && self.style(fragment.style_source).text_wrap_mode() == text_wrap_mode::WRAP
        })
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

    /// Splits off the longest prefix of `item` that fits `available` for an overflow break. A prefix that still ends at
    /// the item's soft hyphen ends the line in a hyphen as well, so it has to leave room for `hyphen` too.
    #[allow(clippy::too_many_arguments)]
    fn split_for_overflow_break_before_hyphen(
        &self,
        item: &mut inline_level_iterator::Item,
        prepared: &prepared_inline_content::PreparedInlineContent,
        text: &[u16],
        letter_spacing: f32,
        word_spacing: f32,
        available: CssPixels,
        hyphen: CssPixels,
        line_is_empty: bool,
    ) -> Option<inline_level_iterator::Item> {
        let split = |item: &mut inline_level_iterator::Item, available| {
            self.pass.with_grapheme_segmenter(item.node, |segmenter| {
                item.split_for_overflow_break(
                    prepared,
                    text,
                    segmenter,
                    letter_spacing,
                    word_spacing,
                    available,
                    line_is_empty,
                )
            })
        };
        if hyphen > CssPixels::default() {
            let end = item.offset_in_node + item.length_in_node;
            let mut remainder = item.clone();
            if let Some(prefix) = split(&mut remainder, available)
                && prefix.offset_in_node + prefix.length_in_node < end
            {
                *item = remainder;
                return Some(prefix);
            }
            return split(item, available - hyphen);
        }
        split(item, available)
    }

    #[allow(clippy::too_many_arguments)]
    fn break_soft_wrapped_text_item<'builder>(
        &'builder self,
        line_builder: &mut line_builder::LineBuilder<'builder, 'context>,
        iterator: &inline_level_iterator::InlineLevelIterator,
        item: &mut inline_level_iterator::Item,
        item_index: usize,
        can_break_before: bool,
        mut line_limit: Option<&mut FragmentainerLineLimit>,
        state_at_clamp_point: &mut Option<LineBuildingState<'builder, 'context>>,
    ) -> bool {
        let prepared = iterator.prepared_content();
        let mut following_inline_size = iterator.inline_size_following_text(self, item);
        // NB: Where nothing glued follows an item that ends in a soft hyphen, the line ends in that hyphen if the next
        //     run doesn't fit after the item.
        if following_inline_size == CssPixels::default()
            && let Some(remaining) = line_builder.remaining_inline_size_for_soft_wrap()
            && inline_level_iterator::inline_extent([&*item]) + iterator.next_line_content_inline_size(self) > remaining
        {
            following_inline_size = inline_level_iterator::hyphen_inline_size_after(self, prepared, item);
        }
        if line_builder
            .remaining_inline_size_for_soft_wrap()
            .is_none_or(|remaining| inline_level_iterator::inline_extent([&*item]) + following_inline_size <= remaining)
        {
            return false;
        }
        let style = self.style(self.parent_node(item.node));
        let text_content = self.pass.text_content(item.node);
        let letter_spacing = style.letter_spacing().to_double() as f32;
        let word_spacing = style.word_spacing().to_double() as f32;
        let overflow_break_allowed = self.overflow_break_applies_to_style(style);
        let end = item.offset_in_node + item.length_in_node;
        let mut offset = item.offset_in_node;
        loop {
            if self.stops_at_line_clamp_point(line_builder, state_at_clamp_point) {
                return false;
            }
            if line_limit.as_deref_mut().is_some_and(|limit| limit.is_reached(self)) {
                return true;
            }
            let remaining = line_builder
                .remaining_inline_size_for_soft_wrap()
                .expect("splitting text requires a definite line width");
            let line_is_empty = line_builder.current_line_is_empty();
            self.set_current_line_start_offset(item_index, offset);
            if line_is_empty && line_builder.current_line_has_no_space_left_by_floats() {
                line_builder.break_line(line_builder::ForcedBreak::No, None, None);
                continue;
            }
            // Keep the remainder as an offset into the item. Each line needs only its candidate prefixes shaped.
            let mut prefix = item.fragment_at_soft_wrap(
                prepared,
                &text_content.text,
                offset,
                letter_spacing,
                word_spacing,
                remaining,
                following_inline_size,
                inline_level_iterator::soft_hyphen_hyphenates(style),
            );
            let hyphen = if prefix.offset_in_node + prefix.length_in_node == end {
                CssPixels::default()
            } else {
                inline_level_iterator::hyphen_inline_size_after(self, prepared, &prefix)
            };
            if inline_level_iterator::inline_extent([&prefix]) + hyphen > remaining {
                if overflow_break_allowed {
                    let physical_remaining = line_builder
                        .remaining_inline_size_for_overflow_break()
                        .expect("splitting text requires a definite line width");
                    if prefix.border_box_inline_size() + hyphen > physical_remaining {
                        let split = self.split_for_overflow_break_before_hyphen(
                            &mut prefix,
                            prepared,
                            &text_content.text,
                            letter_spacing,
                            word_spacing,
                            physical_remaining,
                            hyphen,
                            line_is_empty,
                        );
                        if let Some(split) = split {
                            prefix = split;
                        } else if !line_is_empty {
                            line_builder.break_line(
                                line_builder::ForcedBreak::No,
                                None,
                                Some(prefix.border_box_inline_size()),
                            );
                            continue;
                        }
                    }
                } else if !line_is_empty && can_break_before {
                    line_builder.break_line(
                        line_builder::ForcedBreak::No,
                        None,
                        Some(prefix.border_box_inline_size()),
                    );
                    continue;
                }
            }
            offset = prefix.offset_in_node + prefix.length_in_node;
            if offset == end {
                *item = prefix;
                return false;
            }
            line_builder.append_text_item(&mut prefix, prepared, style);
            self.set_current_line_start_offset(item_index, offset);
            line_builder.break_line(line_builder::ForcedBreak::No, None, None);
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn break_overflowing_text_item<'builder>(
        &'builder self,
        line_builder: &mut line_builder::LineBuilder<'builder, 'context>,
        iterator: &inline_level_iterator::InlineLevelIterator,
        item: &mut inline_level_iterator::Item,
        item_index: usize,
        can_break_before: bool,
        mut line_limit: Option<&mut FragmentainerLineLimit>,
        state_at_clamp_point: &mut Option<LineBuildingState<'builder, 'context>>,
    ) -> bool {
        let prepared = iterator.prepared_content();
        if item.has_soft_wraps(prepared) {
            return self.break_soft_wrapped_text_item(
                line_builder,
                iterator,
                item,
                item_index,
                can_break_before,
                line_limit,
                state_at_clamp_point,
            );
        }
        let style = self.style(self.parent_node(item.node));
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
            // NB: An item that ends in a soft hyphen ends the line in a hyphen if the run after it doesn't fit there too.
            let hyphen = match inline_level_iterator::hyphen_inline_size_after(self, prepared, item) {
                hyphen
                    if hyphen > CssPixels::default()
                        && item.border_box_inline_size() + iterator.next_line_content_inline_size(self)
                            > remaining_inline_size =>
                {
                    hyphen
                }
                _ => CssPixels::default(),
            };
            if item.border_box_inline_size() + hyphen <= remaining_inline_size {
                return false;
            }
            if !self.overflow_break_applies_to_style(style) {
                return false;
            }
            let line_is_empty = self.line_data().line_boxes.last().is_some_and(|line| line.is_empty());
            self.set_current_line_start_offset(item_index, item.offset_in_node);
            if line_is_empty && line_builder.current_line_has_no_space_left_by_floats() {
                line_builder.break_line(line_builder::ForcedBreak::No, None, None);
                continue;
            }
            let text_content = self.pass.text_content(item.node);
            let letter_spacing = style.letter_spacing().to_double() as f32;
            let word_spacing = style.word_spacing().to_double() as f32;
            let split_item = self.split_for_overflow_break_before_hyphen(
                item,
                prepared,
                &text_content.text,
                letter_spacing,
                word_spacing,
                remaining_inline_size,
                hyphen,
                line_is_empty,
            );
            let Some(mut prefix) = split_item else {
                if line_is_empty {
                    return false;
                }
                line_builder.break_line(line_builder::ForcedBreak::No, None, Some(item.border_box_inline_size()));
                continue;
            };
            line_builder.append_text_item(&mut prefix, prepared, style);
            self.set_current_line_start_offset(item_index, item.offset_in_node);
            line_builder.break_line(line_builder::ForcedBreak::No, None, None);
        }
    }

    /// Whether text-wrap-style: balance balances the lines of this context. Lines are only balanced while finding where
    /// they break leaves the rest of the layout alone, so not with block-level boxes among them, and not when they are
    /// clamped or fragmented.
    fn balances_lines(
        &self,
        iterator: &inline_level_iterator::InlineLevelIterator,
        fragmentation: Option<&fragmentation::LineRootFragmentation>,
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
            &self.pass,
            node,
            self.input.containing_block_constraints,
        ));
        self.parent.lay_out_float_ahead_of_its_position(
            self.run,
            block_formatting_context::ChildSizedHere::ahead_of_its_run(node, &cells_of_float_ahead_of_its_layout),
            self.block_container,
            self.input.clone(),
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

    /// Finds the heights that line fitting needs before placing floats or atomic content. Trials stop at block-level
    /// content, whose layout can change the space available to the following lines.
    fn fit_lines_around_floats<'builder>(
        &'builder self,
        iterator: &inline_level_iterator::InlineLevelIterator,
        line_builder: &mut line_builder::LineBuilder<'builder, 'context>,
        item_count: usize,
        pending_resume_offset: Option<usize>,
        item_state: InlineItemRunState,
    ) {
        let items = iterator.items();
        let segment_has_float = items
            .iter()
            .take_while(|item| item.type_ != inline_level_iterator::ItemType::BlockLevelBox)
            .any(|item| item.type_ == inline_level_iterator::ItemType::FloatingElement);
        if self.parent.floating_box_count() == 0 && !segment_has_float {
            return;
        }
        let first_changed_line = self.line_data().line_boxes.len().saturating_sub(2);
        let saved_lines = self.line_data().line_boxes[first_changed_line..].to_vec();
        let line_start = self.current_line_start.get();
        let mut heights = Vec::new();
        loop {
            let state = self.state_before_finding_line_breaks();
            let mut trial = line_builder.clone();
            trial.set_float_fit_heights(heights.clone(), true);
            let mut trial_items = iterator.duplicate(0..items.len(), items.len());
            self.break_items_into_lines_with_float_fitting(
                &mut trial_items,
                &mut trial,
                item_count,
                pending_resume_offset,
                None,
                None,
                item_state,
                true,
            );
            trial.update_last_line(false);
            let refit = trial.float_refit();
            {
                let mut data = self.line_data_mut();
                data.line_boxes.truncate(first_changed_line);
                data.line_boxes.extend(saved_lines.iter().cloned());
            }
            self.restore_state_after_finding_line_breaks(state);
            self.current_line_start.set(line_start);
            let Some((line_index, height)) = refit else {
                break;
            };
            heights.resize(heights.len().max(line_index + 1), CssPixels::default());
            assert!(height > heights[line_index]);
            heights[line_index] = height;
        }
        line_builder.set_float_fit_heights(heights, false);
    }

    /// Breaks the items left in `iterator` into lines with `line_builder`. `item_count` is the number of items the
    /// iterator started with.
    fn break_items_into_lines<'builder>(
        &'builder self,
        iterator: &mut inline_level_iterator::InlineLevelIterator,
        line_builder: &mut line_builder::LineBuilder<'builder, 'context>,
        item_count: usize,
        pending_resume_offset: Option<usize>,
        line_limit: Option<FragmentainerLineLimit>,
        consumed_item_nodes: Option<Vec<(usize, Node)>>,
    ) -> BrokenLines<'builder, 'context> {
        self.break_items_into_lines_with_float_fitting(
            iterator,
            line_builder,
            item_count,
            pending_resume_offset,
            line_limit,
            consumed_item_nodes,
            InlineItemRunState {
                collapsed_whitespace_node: NodeSlotId::INVALID,
                ..Default::default()
            },
            false,
        )
    }

    #[allow(clippy::too_many_arguments)]
    /// Breaks the items left in `iterator` into lines with `line_builder`. `item_count` is the number of items the
    /// iterator started with.
    fn break_items_into_lines_with_float_fitting<'builder>(
        &'builder self,
        iterator: &mut inline_level_iterator::InlineLevelIterator,
        line_builder: &mut line_builder::LineBuilder<'builder, 'context>,
        item_count: usize,
        mut pending_resume_offset: Option<usize>,
        mut line_limit: Option<FragmentainerLineLimit>,
        mut consumed_item_nodes: Option<Vec<(usize, Node)>>,
        mut item_state: InlineItemRunState,
        float_fit_trial: bool,
    ) -> BrokenLines<'builder, 'context> {
        let shares_items = iterator.shares_items();
        let mut absolute_boxes = Vec::new();
        let mut state_at_clamp_point = None;
        let mut items_after_clamp_point = Vec::new();
        let mut stopped_at_line_limit = false;
        let mut needs_float_fitting = !float_fit_trial;
        loop {
            if float_fit_trial && line_builder.float_refit().is_some() {
                break;
            }
            if needs_float_fitting {
                needs_float_fitting = false;
                self.fit_lines_around_floats(iterator, line_builder, item_count, pending_resume_offset, item_state);
            }
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
            // An inline box that holds no in-flow item is on the line that's current when the items reach it. And
            // whitespace never starts a line, so an empty inline box right after it is on this line too — even if
            // the line goes on to break at that whitespace.
            // NB: The start edges that collapsed whitespace left for the next item come before that item too.
            let pending_leading_edges =
                item_state.leading_margin + item_state.leading_border + item_state.leading_padding;
            // NB: An empty inline box after a tab is placed after where the tab ends on this line, so the tabs move
            //     there before the boxes are.
            let start_edges_before_transfer =
                pending_leading_edges + item.margin_start + item.border_start + item.padding_start;
            line_builder.note_next_content(iterator.prepared_content(), &item);
            item.place_tabs_at_line_position(iterator.prepared_content(), || {
                line_builder.inline_position_of_next_content() + start_edges_before_transfer
            });
            if iterator.has_empty_inline_boxes() {
                for empty_box in iterator.empty_inline_boxes_before_last_item() {
                    line_builder
                        .add_strut_of_empty_inline_box(empty_box.node, pending_leading_edges + empty_box.leading_edges);
                }
                if item.is_collapsible_whitespace
                    || (item.type_ == inline_level_iterator::ItemType::Text && item.is_ascii_whitespace(self))
                {
                    // NB: Preserved white space stays on the line, hanging off its end if it has to, so a box after
                    //     it comes after it on the line too.
                    let preserved_whitespace_inline_size = if item.is_collapsible_whitespace {
                        CssPixels::default()
                    } else {
                        inline_level_iterator::inline_extent([&item])
                    };
                    for empty_box in iterator.empty_inline_boxes_after_last_item() {
                        line_builder.add_strut_of_empty_inline_box(
                            empty_box.node,
                            pending_leading_edges + preserved_whitespace_inline_size + empty_box.leading_edges,
                        );
                    }
                }
            }
            // NB: The inline boxes around whitespace are on this line whether the whitespace stays there, collapses,
            //     or ends up hanging off the end of the line. Under the line-height quirk, the line builder takes
            //     their line height away unless they have a border or padding at their start or end.
            if item.is_collapsible_whitespace {
                line_builder.add_struts_of_inline_boxes_around(
                    item.node,
                    pending_leading_edges + item.margin_start + item.border_start + item.padding_start,
                );
            }
            if let Some(offset_in_node) = pending_resume_offset.take()
                && item.type_ == inline_level_iterator::ItemType::Text
                && offset_in_node > item.offset_in_node
            {
                let style = self.style(self.parent_node(item.node));
                let text_content = self.pass.text_content(item.node);
                item.drop_prefix_before(
                    iterator.prepared_content(),
                    offset_in_node,
                    &text_content.text,
                    style.letter_spacing().to_double() as f32,
                    style.word_spacing().to_double() as f32,
                );
            }
            let has_leading_edges = item_state.leading_margin != CssPixels::default()
                || item_state.leading_border != CssPixels::default()
                || item_state.leading_padding != CssPixels::default();
            if !has_leading_edges {
                self.current_line_start.set(fragmentation::LinePosition {
                    item_index,
                    offset_in_node: item.offset_in_node,
                });
            }
            let can_break_after_previous_overflow_item =
                std::mem::take(&mut item_state.previous_text_item_allows_overflow_break_after);
            let run_fits_after_previous_whitespace = std::mem::take(&mut item_state.whitespace_measured_next_run);
            let line_starts_with_whitespace = self
                .line_data()
                .line_boxes
                .last()
                .is_none_or(|line| line.is_empty_or_ends_in_whitespace() || line.has_block_level_box);
            if item.is_collapsible_whitespace && line_starts_with_whitespace {
                // NB: The whitespace disappears, but the inline boxes that end with it still end on this line.
                item_state.collapsed_whitespace_node = item.node;
                item_state.leading_margin += item.margin_start;
                item_state.leading_border += item.border_start;
                item_state.leading_padding += item.padding_start;
                item_state.leading_right_to_left_end_margin += item.entered_right_to_left_end_margin;
                let zero = CssPixels::default();
                if item.margin_end != zero || item.border_end != zero || item.padding_end != zero {
                    // NB: A box that ends here started on this line, so its start edges come before its end edges.
                    line_builder.append_start_edges_of_collapsed_whitespace(
                        item.node,
                        std::mem::take(&mut item_state.leading_margin),
                        std::mem::take(&mut item_state.leading_border),
                        std::mem::take(&mut item_state.leading_padding),
                    );
                    line_builder.append_end_edges_of_collapsed_whitespace(
                        item.node,
                        item.margin_end,
                        item.border_end,
                        item.padding_end,
                    );
                }
                if self.style(self.style_source(item.node)).text_wrap_mode() == text_wrap_mode::WRAP {
                    let next_inline_size = iterator.next_inline_run_size(self).unwrap_or_default();
                    if next_inline_size > CssPixels::default() {
                        line_builder.prepare_to_append_inline_content();
                        line_builder.break_if_needed(next_inline_size);
                    }
                }
                continue;
            }

            // NB: A float is not on the line, so the leading edges wait for the next item that is.
            if item.type_ != inline_level_iterator::ItemType::FloatingElement {
                item.margin_start += item_state.leading_margin;
                item.border_start += item_state.leading_border;
                item.padding_start += item_state.leading_padding;
                item.entered_right_to_left_end_margin += item_state.leading_right_to_left_end_margin;
                item_state.leading_margin = CssPixels::default();
                item_state.leading_border = CssPixels::default();
                item_state.leading_padding = CssPixels::default();
                item_state.leading_right_to_left_end_margin = CssPixels::default();
            }

            match item.type_ {
                inline_level_iterator::ItemType::InlineEdge => {
                    if self.stops_at_line_clamp_point(line_builder, &mut state_at_clamp_point) {
                        break;
                    }
                    line_builder.append_inline_edge(&item);
                }
                inline_level_iterator::ItemType::ForcedBreak => {
                    line_builder.append_start_edges_of_collapsed_whitespace(
                        item_state.collapsed_whitespace_node,
                        item.margin_start,
                        item.border_start,
                        item.padding_start,
                    );
                    self.current_line_start.set(fragmentation::LinePosition {
                        item_index: item_index + 1,
                        offset_in_node: 0,
                    });
                    let continuation = iterator.next_unbreakable_run_inline_size(self);
                    let position = iterator.forced_break_position(&item);
                    line_builder.add_struts_of_inline_boxes_around(position.node, CssPixels::default());
                    line_builder.break_line(line_builder::ForcedBreak::Yes, Some(position), continuation);
                    if !item.node.is_invalid() && self.clear_floating_boxes(item.node) {
                        line_builder.did_introduce_clearance(self.block_axis_float_clearance.get());
                        self.reset_parent_margin_state();
                    }
                }
                inline_level_iterator::ItemType::Element => {
                    line_builder.prepare_to_append_inline_content();
                    if shares_items || float_fit_trial {
                        let cells = if self.laid_out_atomic_inline(item.node) {
                            self.placed_atomic_inline(item.node).geometry.cells
                        } else {
                            let cells = self.hold_atomic_inline(item.node);
                            self.dimension_box_on_line(item.node, cells).1
                        };
                        item.set_box_cells(cells);
                    }
                    if !line_builder.only_finds_line_breaks() {
                        let insets = self.compute_inset(item.node);
                        self.update_placed_atomic_inline(item.node, |placed_child| {
                            placed_child.with_in_flow_box_insets(insets)
                        });
                    }
                    if item.can_break_before
                        || (line_builder.current_line_is_empty()
                            && self.style(self.containing_block).text_wrap_mode() == text_wrap_mode::WRAP)
                    {
                        // NB: The box goes on the line with whatever can't be broken from it after it, such as a box in
                        //     the same nowrap inline box. A box by itself needs only its advance, which a negative end
                        //     margin reduces.
                        let box_inline_size = item.margin_start + item.border_box_inline_size() + item.margin_end;
                        let needed_inline_size = if iterator.next_line_content_continues_run(self) {
                            iterator
                                .unbreakable_run_inline_size(self, &item, false)
                                .max(box_inline_size)
                        } else {
                            box_inline_size
                        };
                        line_builder.break_if_needed(needed_inline_size);
                        line_builder.note_soft_wrap_opportunity();
                    }
                    if self.stops_at_line_clamp_point(line_builder, &mut state_at_clamp_point) {
                        break;
                    }
                    line_builder.append_box(&item);
                }
                inline_level_iterator::ItemType::BlockLevelBox => {
                    item_state.leading_margin += item.margin_start;
                    item_state.leading_border += item.border_start;
                    item_state.leading_padding += item.padding_start;
                    item_state.leading_right_to_left_end_margin += item.entered_right_to_left_end_margin;
                    // https://drafts.csswg.org/css2/#anonymous-block-level
                    // NB: The inline boxes around the block-level box are broken around it, so they're on the line
                    //     before it and on the line after it.
                    line_builder.add_struts_of_inline_boxes_around(item.node, CssPixels::default());
                    line_builder.finish_current_line_before_block_level_box();
                    if float_fit_trial {
                        break;
                    }
                    needs_float_fitting = true;
                    if self.stops_at_line_clamp_point(line_builder, &mut state_at_clamp_point) {
                        break;
                    }
                    let relative_insets = self.relative_insets_of_inline_ancestors(self.pass.parent(item.node));
                    let collapses_through = self.parent.layout_interrupting_block_inside_inline_context(
                        self.run,
                        item.node,
                        self.block_container,
                        self.input.clone(),
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
                    line_builder.add_struts_of_inline_boxes_around(item.node, CssPixels::default());
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
                    line_builder.append_start_edges_of_collapsed_whitespace(
                        item_state.collapsed_whitespace_node,
                        item.margin_start,
                        item.border_start,
                        item.padding_start,
                    );
                    line_builder.append_static_position_marker(item.node, preceded);
                    absolute_boxes.push(item.node);
                }
                inline_level_iterator::ItemType::FloatingElement => {
                    let mut fragment_facts = run_state::FragmentFactsDecidedByParent::default();
                    if state_at_clamp_point.is_some() {
                        fragment_facts.show.hide_after_clamp_point();
                    }
                    self.lay_out_float_on_line(
                        &item,
                        item_index,
                        line_builder,
                        iterator.next_unbreakable_run_inline_size(self).unwrap_or_default(),
                        fragment_facts,
                    );
                }
                inline_level_iterator::ItemType::Text => {
                    line_builder.prepare_to_append_inline_content();
                    // https://drafts.csswg.org/css-text-3/#white-space-phase-2
                    // "each preserved tab is rendered as a horizontal shift that lines up the start edge of the next
                    // glyph with the next tab stop."
                    // NB: Where the item starts on its line decides where its tabs end: on the line it would join, for
                    //     the measurements that decide where the line breaks, and then on the line it does join.
                    // NB: The item's start edges took the leading edges that collapsed whitespace left for it.
                    let start_edges = item.margin_start + item.border_start + item.padding_start;
                    item.place_tabs_at_line_position(iterator.prepared_content(), || {
                        line_builder.inline_position_of_next_content() + start_edges
                    });
                    let style = self.style(self.parent_node(item.node));
                    if style.text_wrap_mode() == text_wrap_mode::WRAP {
                        let is_whitespace = item.is_collapsible_whitespace || item.is_ascii_whitespace(self);
                        let line_ends_in_whitespace = self
                            .line_data()
                            .line_boxes
                            .last()
                            .is_some_and(|line| self.line_is_empty_or_ends_in_whitespace_that_wraps(line));
                        let opportunity = self.text_item_wrap_opportunity(
                            iterator.prepared_content(),
                            &item,
                            style,
                            is_whitespace,
                            iterator.items(),
                            line_ends_in_whitespace,
                            can_break_after_previous_overflow_item,
                        );
                        let overflow_break_allowed = !is_whitespace && self.overflow_break_applies_to_style(style);
                        let has_soft_wraps = item.has_soft_wraps(iterator.prepared_content());
                        match opportunity {
                            TextItemWrapOpportunity::BeforeWhitespaceSequence(sequence_inline_size) => {
                                // https://drafts.csswg.org/css-text-3/#white-space-phase-2
                                // "If 'white-space' is set to 'pre-wrap', the UA must (unconditionally) hang this
                                // sequence, unless the sequence is followed by a forced line break"
                                if !item.is_collapsible_whitespace {
                                    line_builder.set_hanging_whitespace_inline_size(
                                        inline_level_iterator::inline_extent([&item]),
                                    );
                                }
                                // NB: The white space ends the line it breaks, and so do the inline boxes that end with it.
                                line_builder.set_end_edges_of_whitespace_at_break(
                                    item.margin_end + item.border_end + item.padding_end,
                                );
                                let broke_line = if iterator.next_non_whitespace_text_allows_overflow_break(self) {
                                    line_builder.break_if_needed_before_overflow_breakable_item(sequence_inline_size)
                                } else {
                                    // NB: The white space stays at the end of this line, so a line past floats has to
                                    //     fit only what follows it.
                                    let after_whitespace =
                                        sequence_inline_size - inline_level_iterator::inline_extent([&item]);
                                    line_builder
                                        .break_if_needed_sized_by(sequence_inline_size, &mut |_| after_whitespace)
                                };
                                line_builder.set_hanging_whitespace_inline_size(CssPixels::default());
                                line_builder.set_end_edges_of_whitespace_at_break(CssPixels::default());
                                if broke_line {
                                    line_builder.set_trailing_whitespace_on_previous_line(&item);
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
                                item_state.whitespace_measured_next_run = true;
                            }
                            TextItemWrapOpportunity::BeforeText
                            | TextItemWrapOpportunity::AfterOverflowBreakableText => {
                                let measure_rest_of_run = !is_whitespace
                                    && !run_fits_after_previous_whitespace
                                    && line_builder.can_break_before_next_item();
                                let unbreakable_run_inline_size = if measure_rest_of_run || has_soft_wraps {
                                    let stop_at_overflow_breakable_text = opportunity
                                        == TextItemWrapOpportunity::AfterOverflowBreakableText
                                        || line_builder.current_line_is_empty();
                                    iterator.unbreakable_run_inline_size(self, &item, stop_at_overflow_breakable_text)
                                } else {
                                    inline_level_iterator::inline_extent([&item])
                                };
                                if overflow_break_allowed {
                                    line_builder
                                        .break_if_needed_before_overflow_breakable_item(unbreakable_run_inline_size);
                                } else {
                                    // NB: Lines past floats start elsewhere, which moves tabs that start the item.
                                    let prepared = iterator.prepared_content();
                                    line_builder.break_if_needed_sized_by(
                                        unbreakable_run_inline_size,
                                        &mut |position| {
                                            unbreakable_run_inline_size - item.inline_size
                                                + item.inline_size_with_tabs_at(prepared, position + start_edges)
                                        },
                                    );
                                }
                                line_builder.note_soft_wrap_opportunity();
                            }
                            TextItemWrapOpportunity::None => {}
                        }
                        if overflow_break_allowed || has_soft_wraps {
                            if self.break_overflowing_text_item(
                                line_builder,
                                iterator,
                                &mut item,
                                item_index,
                                opportunity != TextItemWrapOpportunity::None,
                                line_limit.as_mut(),
                                &mut state_at_clamp_point,
                            ) {
                                stopped_at_line_limit = true;
                                break;
                            }
                            item_state.previous_text_item_allows_overflow_break_after = overflow_break_allowed;
                        }
                    } else if item.can_break_before
                        || can_break_after_previous_overflow_item
                        || (line_builder.current_line_is_empty()
                            && self.style(self.containing_block).text_wrap_mode() == text_wrap_mode::WRAP)
                    {
                        line_builder.break_if_needed(iterator.unbreakable_run_inline_size(
                            self,
                            &item,
                            !item.can_break_before,
                        ));
                        line_builder.note_soft_wrap_opportunity();
                    }
                    if self.stops_at_line_clamp_point(line_builder, &mut state_at_clamp_point) {
                        break;
                    }
                    item.place_tabs_at_line_position(iterator.prepared_content(), || {
                        line_builder.inline_position_of_next_content() + start_edges
                    });
                    line_builder.append_text_item(&mut item, iterator.prepared_content(), style);
                }
            }
        }
        if iterator.items().is_empty() {
            for empty_box in iterator.empty_inline_boxes_after_last_item() {
                line_builder.add_strut_of_empty_inline_box(
                    empty_box.node,
                    item_state.leading_margin
                        + item_state.leading_border
                        + item_state.leading_padding
                        + empty_box.leading_edges,
                );
            }
            line_builder.append_start_edges_of_collapsed_whitespace(
                item_state.collapsed_whitespace_node,
                item_state.leading_margin,
                item_state.leading_border,
                item_state.leading_padding,
            );
            line_builder.keep_line_of_collapsed_whitespace_edges();
        }
        BrokenLines {
            absolute_boxes,
            state_at_clamp_point,
            items_after_clamp_point,
            stopped_at_line_limit,
            consumed_item_nodes,
        }
    }

    fn lay_out_float_on_line(
        &self,
        item: &inline_level_iterator::Item,
        item_index: usize,
        line_builder: &mut line_builder::LineBuilder<'_, '_>,
        next_unbreakable_run: CssPixels,
        fragment_facts: run_state::FragmentFactsDecidedByParent,
    ) {
        line_builder.commit_pending_margin_before_float();
        self.clear_floating_boxes(item.node);
        line_builder.set_unbreakable_run_inline_size_interrupted_by_float(next_unbreakable_run);
        if line_builder.only_finds_line_breaks() {
            if !self.floats_laid_out_ahead_of_lines.borrow().contains_key(&item.node) {
                let laid_out_float = self.lay_out_float_ahead_of_its_position(item.node);
                self.floats_laid_out_ahead_of_lines
                    .borrow_mut()
                    .insert(item.node, laid_out_float);
            }
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
                self.input.clone(),
                self.containing_block_position_in_bfc_root,
                line_builder,
                self.fragments_lines.get(),
            );
            return;
        }
        let laid_out_ahead = self.floats_laid_out_ahead_of_lines.borrow_mut().remove(&item.node);
        let laid_out_float = laid_out_ahead.unwrap_or_else(|| self.lay_out_float_ahead_of_its_position(item.node));
        if self.fragments_lines.get() {
            let (positioned, deferred) = self.parent.position_inline_float_in_column(
                laid_out_float,
                self.block_container,
                self.input.clone(),
                self.containing_block_position_in_bfc_root,
                line_builder,
            );
            if deferred {
                self.deferred_float_items.borrow_mut().push((item_index, item.node));
                if let Some(fragments) = self.run.fragments {
                    fragments.discard_unplaced_subtree(item.node, positioned.laid_out_float.fragment_arrival);
                }
            } else {
                self.positioned_floats.borrow_mut().push((item_index, positioned));
            }
            return;
        }
        let (insets, _, _) = self.parent.position_laid_out_float(
            self.run,
            laid_out_float,
            self.block_container,
            self.input.clone(),
            self.containing_block_position_in_bfc_root,
            CssPixels::default(),
            Some(&mut *line_builder),
            fragment_facts,
        );
        self.used_insets_of_floats
            .borrow_mut()
            .insert(item.node, insets.used_insets.unwrap_or_default());
    }

    pub(crate) fn generate_line_boxes(&mut self, mut iterator: inline_level_iterator::InlineLevelIterator) {
        self.fragmented_inlines_in_pre_order = iterator.fragmented_inlines_in_pre_order(self);
        let fragmentation = self.parent.line_root_fragmentation(self.containing_block);
        let resume = fragmentation.as_ref().and_then(|fragmentation| fragmentation.resume);
        self.fragments_lines.set(
            fragmentation.is_some()
                && !self.parent.has_line_clamp()
                && iterator
                    .items()
                    .iter()
                    .all(|item| item.type_ != inline_level_iterator::ItemType::BlockLevelBox),
        );
        let balances_lines = self.balances_lines(&iterator, fragmentation.as_ref());
        // NB: A reused first line box keeps the outside marker it was laid out with.
        let (reused_lines, reused_item_count) = if self.parent.has_line_clamp()
            || resume.is_some()
            || balances_lines
            || self.parent.pending_outside_marker_line().is_some()
            || self.facts(self.containing_block).is_list_item_box()
        {
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
            data.inline_box_rects_after_clamp = None;
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
        let deferred_floats = fragmentation
            .as_ref()
            .map(|fragmentation| fragmentation.deferred_floats.as_slice())
            .unwrap_or_default();
        let mut float_indices = HashMap::default();
        if !deferred_floats.is_empty() {
            for (index, item) in iterator.items().iter().enumerate() {
                if item.type_ == inline_level_iterator::ItemType::FloatingElement {
                    float_indices.entry(item.node).or_insert(index);
                }
            }
        }
        let resumed_floats: Vec<_> = deferred_floats
            .iter()
            .map(|&node| {
                let index = *float_indices
                    .get(&node)
                    .expect("a deferred float is present in the resumed inline items");
                (index, iterator.items()[index].clone())
            })
            .collect();
        let preceding_break = resumes_after_forced_break.then(|| iterator.items()[start_item_index - 1].node);
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
            .as_ref()
            .filter(|_| !self.parent.has_line_clamp())
            .and_then(|fragmentation| fragmentation.block_size)
            .filter(|_| self.fragments_lines.get())
            .map(|block_size| FragmentainerLineLimit {
                block_size,
                widows: self.style(self.containing_block).widows().max(1) as usize,
                checked_line_count: 0,
                lines_past_end: 0,
                minimum_line_block_size: None,
            });
        for (index, item) in resumed_floats {
            self.lay_out_float_on_line(
                &item,
                index,
                &mut line_builder,
                CssPixels::default(),
                Default::default(),
            );
        }
        if let Some(node) = preceding_break
            && !node.is_invalid()
            && self.clear_floating_boxes(node)
        {
            line_builder.did_introduce_clearance(self.block_axis_float_clearance.get());
            self.reset_parent_margin_state();
        }
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
                        self.relative_insets_of_inline_ancestors(self.pass.parent(fragment.layout_node));
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
                let rects = self.collect_clamped_inline_box_rects(state_at_clamp_point.current_line_index);
                let mut data = self.line_data_mut();
                data.lines_after_clamp_point_rect =
                    (!lines_after_clamp_point_rect.is_empty()).then(|| Box::new(lines_after_clamp_point_rect.into()));
                data.inline_box_rects_after_clamp = rects;
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
                    self.hide_atomic_inline(node);
                } else if item_type == inline_level_iterator::ItemType::AbsolutelyPositionedElement
                    && self.facts(node).is_box()
                    && self.line_data().line_boxes.iter().any(|line| {
                        line.visible_fragments().any(|fragment| {
                            self.pass
                                .is_ancestor(self.pass.parent(node), fragment.layout_node, self.containing_block)
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
                    let mut line_data = self.line_data_mut();
                    apply_text_overflow_to_line(&mut line_data.line_boxes[line_index], self);
                }
            }
        }
        if !self.parent.has_line_clamp() {
            line_builder.update_last_line(false);
        }
        let containing_style = self.style(self.containing_block);
        if line_builder::may_justify_lines(containing_style) {
            let line_count = self.line_data().line_boxes.len();
            for index in 0..line_count {
                let uses_last_line_alignment = {
                    let line_data = self.line_data();
                    let line = &line_data.line_boxes[index];
                    line.uses_last_line_alignment.unwrap_or_else(|| {
                        let is_last_line =
                            index + 1 == line_count && line.inline_length_before_block_ellipsis.is_none();
                        is_last_line || line.has_forced_break
                    })
                };
                if line_builder::line_text_align(containing_style, uses_last_line_alignment) != text_align::JUSTIFY {
                    continue;
                }
                apply_to_fragments(
                    containing_style.text_justify(),
                    self.atomic_items_are_right_to_left,
                    &mut self.line_data_mut().line_boxes[index],
                );
            }
        }

        if self.parent.keeps_column_box_layouts()
            && let Some(last_line) = self.line_data().line_boxes.iter().rfind(|line| !line.is_empty())
        {
            self.parent
                .note_column_content_block_end(self.content_box_position_in_bfc_root().y + last_line.block_end);
        }
        let mut lines_are_fragmented = false;
        let mut kept_lines_end = None;
        if let Some(fragmentation) = fragmentation {
            let breakable = self.fragments_lines.get();
            let dropped_atomic_inlines = if breakable {
                lines_are_fragmented = true;
                let (dropped_atomic_inlines, next_line_start) =
                    self.keep_lines_of_fragmentainer(&fragmentation, stopped_at_line_limit);
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
        } else if let Some(last_line) = self.line_data().line_boxes.iter().rfind(|line| !line.is_empty()) {
            let lines_block_end_in_root = self.content_box_position_in_bfc_root().y + last_line.block_end;
            self.parent
                .note_unfragmented_line_root(self.containing_block, lines_block_end_in_root);
        }
        if lines_are_fragmented {
            let stop = kept_lines_end.unwrap_or(fragmentation::LinePosition::AFTER_LAST_LINE);
            let positioned_floats = self.positioned_floats.take();
            let kept: HashSet<_> = positioned_floats
                .iter()
                .filter(|(index, _)| *index < stop.item_index)
                .map(|(_, positioned)| positioned.laid_out_float.node)
                .collect();
            self.parent.retain_inline_floats(floating_box_count_before_lines, &kept);
            for (index, positioned) in positioned_floats {
                let node = positioned.laid_out_float.node;
                if index < stop.item_index {
                    let (insets, _, _) = self.parent.place_positioned_float(
                        self.run,
                        positioned,
                        self.block_container,
                        Default::default(),
                    );
                    self.used_insets_of_floats
                        .borrow_mut()
                        .insert(node, insets.used_insets.unwrap_or_default());
                } else if let Some(fragments) = self.run.fragments {
                    fragments.discard_unplaced_subtree(node, positioned.laid_out_float.fragment_arrival);
                }
            }
            let deferred: Vec<_> = self
                .deferred_float_items
                .take()
                .into_iter()
                .filter(|&(index, _)| index < stop.item_index)
                .map(|(_, node)| node)
                .collect();
            if kept_lines_end.is_some() || !deferred.is_empty() {
                let stop_token = self
                    .parent
                    .break_line_root_before_line(self.containing_block, stop, deferred);
                self.fragmentainer_stop_token.replace(stop_token);
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
                let relative_insets = self.relative_insets_of_inline_ancestors(self.pass.parent(fragment.layout_node));
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
                inline_alignment: AxisEdge::Start,
                block_alignment: AxisEdge::Start,
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
                        static_position.inline_alignment = AxisEdge::End;
                    }
                    break 'lines;
                }
            }
            formatting_context::register_contained_abspos_child(
                &self.pass,
                self.run.fragments,
                self.containing_block,
                box_,
                static_position,
                None,
            );
        }
        if self.run.fragments.is_some() {
            self.compute_inline_box_pieces();
        }
        line_builder.remove_last_line_if_empty();
        self.parent.append_floating_boxes(floating_boxes_after_clamp_point);
    }

    pub(crate) fn run(&mut self) -> block_formatting_context::ChildrenSummary {
        assert!(self.facts(self.containing_block).children_are_inline());
        let run = self.run;
        let iterator = match run.fragmentainer.as_ref() {
            Some(fragmentainer) => {
                inline_level_iterator::InlineLevelIterator::new_sharing_items(self, &fragmentainer.inline_items)
            }
            None => inline_level_iterator::InlineLevelIterator::new(self),
        };
        let uses_token_ink_metrics = self.facts(self.containing_block).display().is_math_inside()
            && matches!(
                self.pass.arena().math_layout_kind(self.containing_block),
                math_formatting_context::MathLayoutKind::Token
                    | math_formatting_context::MathLayoutKind::Mtext
                    | math_formatting_context::MathLayoutKind::Mo
            )
            && self
                .pass
                .children(self.containing_block)
                .all(|child| self.facts(child).is_text_node())
            && iterator.contains_only_unbreakable_text(self);
        self.generate_line_boxes(iterator);
        if uses_token_ink_metrics && let [line] = self.line_data.get_mut().line_boxes.as_mut_slice() {
            math_formatting_context::size_token_line_to_ink(line);
        }
        if self.run.fragments.is_some() {
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
        // https://drafts.csswg.org/css2/#adjoining-margins
        // - no line boxes, no clearance, no padding and no border separate them (Note that certain zero-height line boxes
        //   (see 9.4.2) are ignored for this purpose.)
        // NB: A line box that holds text, or ends with a forced break (not one that holds an interrupting block),
        //     separates them even where it's zero-height, e.g. with a zero font size.
        let has_line_with_text = self.line_data.get_mut().line_boxes.iter().any(|line| {
            (line.has_forced_break && line.block_level_box.is_none())
                || line
                    .fragments
                    .iter()
                    .any(|fragment| fragment.box_cells.is_none() && fragment.text_length_in_code_units > 0)
        });
        block_formatting_context::ChildrenSummary {
            baselines,
            lines: Some(std::mem::take(self.line_data.get_mut())),
            fragmentainer_stop_token: self.fragmentainer_stop_token.take(),
            lines_are_unbreakable: self.lines_are_unbreakable.get(),
            last_line,
            has_child_whose_margins_do_not_collapse_through: has_line_with_text
                || !self.children_let_margins_collapse_through(self.containing_block),
            ..Default::default()
        }
    }

    /// Whether margins collapse through the children of `node`, the containing block or an inline box in it: through
    /// each inline-level box this context holds, and through each block-level box it interrupted its lines for.
    fn children_let_margins_collapse_through(&self, node: Node) -> bool {
        self.pass.children(node).all(|child| {
            let facts = self.facts(child);
            if facts.is_absolutely_positioned() || facts.is_floating() {
                return true;
            }
            // https://drafts.csswg.org/css2/#inline-formatting
            // Line boxes that contain no text, no preserved white space, no inline elements with non-zero margins,
            // padding, or borders or other in-flow content (such as images, inline blocks or inline tables), and do not
            // end with a preserved newline must be treated as zero-height line boxes for the purposes of determining
            // the positions of any elements inside of them, and must be treated as not existing for any other purpose.
            // NB: Only the inline-axis margins, padding, and borders of an inline box count here.
            if !facts.is_box() {
                let Some(sides) = self.entered_inline_box(child) else {
                    return true;
                };
                // NB: The sides of an inline box are relative to the layout frame, whose left and right are the inline
                //     axis in every writing mode.
                let inline_edges = [
                    sides.margin_left,
                    sides.margin_right,
                    sides.border_left,
                    sides.border_right,
                    sides.padding_left,
                    sides.padding_right,
                ];
                return inline_edges.iter().all(|edge| *edge == CssPixels::default())
                    && self.children_let_margins_collapse_through(child);
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
            // NB: clear applies only to block-level boxes, so no inline-level box introduces clearance.
            block_formatting_context::margins_can_collapse_through_box_itself(facts, false, || match entered_inline_box
            {
                Some(sides) => sides.border_box_block_size(false),
                None => {
                    let atomic_inline = self.placed_atomic_inline(child).geometry.cells;
                    atomic_inline.border_box_block_size(false)
                }
            }) && self.children_let_margins_collapse_through(child)
        })
    }

    fn leave_atomic_inlines_without_a_line_unplaced(&self) {
        let mut unplaced: Vec<_> = self.atomic_inline_fragment_arrivals.take().into_iter().collect();
        unplaced.sort_by(|(left, _), (right, _)| {
            if self.pass.is_before(*left, *right) {
                std::cmp::Ordering::Less
            } else {
                std::cmp::Ordering::Greater
            }
        });
        for (node, fragment_arrival) in unplaced {
            formatting_context::leave_child_unplaced(self.run, self.placed_atomic_inline(node), Some(fragment_arrival));
        }
    }

    fn collect_clamped_inline_box_rects(
        &self,
        first_line: usize,
    ) -> Option<Box<[inline_content::ClampedInlineBoxRect]>> {
        if self.fragmented_inlines_in_pre_order.is_empty() {
            return None;
        }
        let (pieces, _) = compute(self);
        let data = self.line_data();
        let rects: Vec<_> = pieces
            .into_iter()
            .filter(|piece| piece.line_index as usize >= first_line && !piece.is_geometry_only_placeholder)
            .map(|piece| {
                let mut rect = piece.border_box_rect;
                let relative_insets = self.relative_insets_of_inline_ancestors(piece.node);
                rect.x += relative_insets.offset_x;
                rect.y += relative_insets.offset_y;
                let line = &data.line_boxes[piece.line_index as usize];
                if self.style(self.containing_block).writing_mode() == writing_mode::VERTICAL_LR {
                    rect.y = line.mirrored_block_offset(rect.y, rect.height);
                }
                inline_content::ClampedInlineBoxRect {
                    node: piece.node,
                    rect,
                    box_fragment: 0,
                }
            })
            .collect();
        (!rects.is_empty()).then(|| rects.into_boxed_slice())
    }

    fn compute_inline_box_pieces(&self) {
        let (pieces, inline_containing_block_rect_candidates) = compute(self);
        self.line_data_mut().inline_box_pieces = pieces;
        for candidate in inline_containing_block_rect_candidates {
            let relative_inset_chain = self.relative_insets_of_inline_ancestors(candidate.inline_containing_block);
            let mut rect = candidate.rect;
            rect.x += relative_inset_chain.offset_x;
            rect.y += relative_inset_chain.offset_y;
            if let Some(fragments) = self.run.fragments {
                fragments.register_inline_containing_block_rect(
                    candidate.inline_containing_block,
                    rect,
                    self.containing_block,
                );
            }
        }
    }

    fn register_inline_anchor_candidates(&self) {
        let Some(fragments) = self.run.fragments else {
            return;
        };
        // An inline anchor's border box is the union of its line fragments, including relative positioning.
        // Keep discovery order so that repeated layout runs produce the same propagated candidates.
        let mut candidates = Vec::<(Node, CssPixelRect)>::new();
        let mut indices = HashMap::<Node, usize>::default();
        for piece in &self.line_data().inline_box_pieces {
            if !self.facts(piece.node).is_anchor_candidate() {
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
        fragmentation: &fragmentation::LineRootFragmentation,
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
                    !line.counts_toward_widows_and_orphans(),
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
            let content_lines: Vec<_> = self
                .line_data()
                .line_boxes
                .iter()
                .enumerate()
                .filter_map(|(index, line)| line.counts_toward_widows_and_orphans().then_some(index))
                .collect();
            let kept_content_lines = content_lines.partition_point(|&index| index < stop);
            let style = self.style(self.containing_block);
            // https://drafts.csswg.org/css-break-3/#widows-orphans
            let orphans = style.orphans().max(1) as usize;
            let widows = style.widows().max(1) as usize;
            if kept_content_lines < orphans {
                if is_first_fragment
                    && fragmentation.fragmentainer_had_content
                    && !fragmentation.break_before_is_avoided
                {
                    self.parent.request_early_break_before(self.containing_block);
                } else {
                    self.parent.note_violated_break_avoidance();
                }
            } else if !stopped_at_line_limit && content_lines.len().saturating_sub(kept_content_lines) < widows {
                let lines_keeping_widows = content_lines.len().saturating_sub(widows);
                if lines_keeping_widows >= orphans {
                    stop = content_lines[lines_keeping_widows];
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
            .filter(|node| is_first_fragment && self.pass.first_child(*node).is_invalid());
        present.extend(childless_inline_boxes.clone());
        let mut walked = HashSet::<Node>::default();
        for node in kept_item_nodes
            .chain(nodes_on_lines)
            .chain(childless_inline_boxes)
            .filter(|node| !node.is_invalid())
        {
            let mut ancestor = if self.facts(node).is_fragmented_inline() {
                node
            } else {
                self.pass.parent(node)
            };
            while !ancestor.is_invalid() && ancestor != self.containing_block && walked.insert(ancestor) {
                present.insert(ancestor);
                ancestor = self.pass.parent(ancestor);
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
                let first_ancestor = if self.facts(fragment.layout_node).is_fragmented_inline() {
                    fragment.layout_node
                } else {
                    self.pass.parent(fragment.layout_node)
                };
                fragment.relpos_delta = accumulated_relative_offset_from(first_ancestor);
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

#[derive(Clone, Copy, Default)]
struct InlineItemRunState {
    leading_margin: CssPixels,
    leading_border: CssPixels,
    leading_padding: CssPixels,
    leading_right_to_left_end_margin: CssPixels,
    collapsed_whitespace_node: Node,
    previous_text_item_allows_overflow_break_after: bool,
    whitespace_measured_next_run: bool,
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
    has_edges_of_collapsed_whitespace: bool,
    ends_in_whitespace: bool,
    trailing_whitespace_wraps: bool,
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

    pub(super) fn append_atomic_inline(&mut self, border_box_inline_size: CssPixels, can_break_before: bool) {
        if can_break_before && self.breaks_before_next_fragment() {
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

    fn is_empty_or_ends_in_whitespace_that_wraps(&self) -> bool {
        !self.has_fragments || (self.ends_in_whitespace && self.trailing_whitespace_wraps)
    }

    fn prevent_wrap_after_trailing_whitespace(&mut self) {
        self.trailing_whitespace_wraps = false;
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
        self.trailing_whitespace_wraps = true;
        // NB: Adjacent all-space chunks form one trailing sequence, including across inline boundaries.
        if ends_in_whitespace && border_box_inline_size == trailing_whitespace_inline_size {
            self.trailing_whitespace_inline_size += trailing_whitespace_inline_size;
        } else {
            self.trailing_whitespace_inline_size = trailing_whitespace_inline_size;
        }
    }

    fn append_edges_of_collapsed_whitespace(&mut self, margin: CssPixels, border: CssPixels, padding: CssPixels) {
        self.inline_size += margin + border + padding;
        self.has_edges_of_collapsed_whitespace |= [margin, border, padding]
            .iter()
            .any(|edge| *edge != CssPixels::default());
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
        self.has_edges_of_collapsed_whitespace = false;
        self.ends_in_whitespace = false;
        self.trailing_whitespace_inline_size = CssPixels::default();
    }

    fn finish(&mut self) {
        if self.has_edges_of_collapsed_whitespace {
            self.start_line_if_empty();
        }
        if self.has_fragments || self.has_edges_of_collapsed_whitespace || !self.floats_beside_line.is_empty() {
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
    pending_start_margin: CssPixels,
    pending_start_border: CssPixels,
    pending_start_padding: CssPixels,
    previous_text_item_breaks_between_graphemes: bool,
}

impl ContentSizingLines {
    fn new(lines: LinesWithoutLineBoxes) -> Self {
        Self {
            lines,
            pending_start_margin: CssPixels::default(),
            pending_start_border: CssPixels::default(),
            pending_start_padding: CssPixels::default(),
            previous_text_item_breaks_between_graphemes: false,
        }
    }

    fn measures_min_content(&self) -> bool {
        self.lines.breaks_at_every_opportunity
    }

    fn append(
        &mut self,
        context: &InlineFormattingContext<'_>,
        items: &[inline_level_iterator::Item],
        prepared: &prepared_inline_content::PreparedInlineContent,
        index: usize,
    ) {
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
            self.lines
                .append_edges_of_collapsed_whitespace(item.margin_end, item.border_end, item.padding_end);
            if self.lines.breaks_before_next_fragment()
                && matches!(
                    context.text_item_wrap_opportunity(
                        prepared,
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
            item.min_content_inline_size().unwrap_or(item.inline_size)
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
            inline_level_iterator::ItemType::ForcedBreak => {
                self.lines
                    .append_edges_of_collapsed_whitespace(margin_start, border_start, padding_start);
                self.lines.force_break();
            }
            inline_level_iterator::ItemType::InlineEdge => {
                self.lines.append(border_box_inline_size, false, CssPixels::default());
            }
            inline_level_iterator::ItemType::Element => {
                self.lines
                    .append_atomic_inline(border_box_inline_size, item.can_break_before);
            }
            // An absolutely positioned box only takes a static position, which comes after the pending inline edges.
            inline_level_iterator::ItemType::AbsolutelyPositionedElement => {
                self.lines
                    .append_edges_of_collapsed_whitespace(margin_start, border_start, padding_start);
            }
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
                    // NB: An ideographic space at the end hangs, but it still counts toward the intrinsic sizes.
                    item.trailing_whitespace.ascii_inline_size
                } else {
                    CssPixels::default()
                };
                // NB: Min-content breaks at every soft wrap opportunity, so content that ends in a soft hyphen ends
                //     its line in a hyphen wherever the content after it can start a line. And min-content breaks
                //     before every atomic inline that a line can break before, as append_atomic_inline() does.
                let next_content = remaining_items.iter().find(|next| {
                    !matches!(
                        next.type_,
                        inline_level_iterator::ItemType::InlineEdge
                            | inline_level_iterator::ItemType::AbsolutelyPositionedElement
                            | inline_level_iterator::ItemType::FloatingElement
                    )
                });
                let breaks_after_item = next_content.is_some_and(|next| match next.type_ {
                    inline_level_iterator::ItemType::Text | inline_level_iterator::ItemType::Element => {
                        next.can_break_before
                    }
                    _ => false,
                });
                if style.text_wrap_mode() == text_wrap_mode::WRAP {
                    let is_whitespace = item.is_collapsible_whitespace || item.is_ascii_whitespace(context);
                    let breaks_between_graphemes = !is_whitespace && breaks_between_graphemes(style);
                    let opportunity = if self.lines.breaks_before_next_fragment() {
                        let can_break_after_previous_grapheme_broken_item =
                            previous_text_item_breaks_between_graphemes && !breaks_between_graphemes;
                        context.text_item_wrap_opportunity(
                            prepared,
                            item,
                            style,
                            is_whitespace,
                            remaining_items,
                            self.lines.is_empty_or_ends_in_whitespace_that_wraps()
                                || can_break_after_previous_grapheme_broken_item,
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
                            prepared,
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
                    if self.measures_min_content() && item.has_soft_wraps(prepared) {
                        let text = context.pass.text_content(item.node);
                        let sizes = item.soft_wrap_segment_inline_sizes(
                            prepared,
                            &text.text,
                            style.letter_spacing().to_double() as f32,
                            style.word_spacing().to_double() as f32,
                            inline_level_iterator::soft_hyphen_hyphenates(style),
                            breaks_after_item,
                        );
                        let last_index = sizes.len() - 1;
                        for (index, size) in sizes.into_iter().enumerate() {
                            let leading = if index == 0 {
                                margin_start + border_start + padding_start
                            } else {
                                CssPixels::default()
                            };
                            let trailing = if index == last_index {
                                item.padding_end + item.border_end + item.margin_end
                            } else {
                                CssPixels::default()
                            };
                            self.lines
                                .append(leading + size + trailing, false, CssPixels::default());
                            if index != last_index {
                                self.lines.finish();
                            }
                        }
                        return;
                    }
                } else if self.lines.breaks_before_next_fragment()
                    && (item.can_break_before || previous_text_item_breaks_between_graphemes)
                {
                    self.lines.finish();
                }
                let hyphen = if self.measures_min_content() && breaks_after_item {
                    inline_level_iterator::hyphen_inline_size_after(context, prepared, item)
                } else {
                    CssPixels::default()
                };
                self.lines.append(
                    border_box_inline_size + hyphen,
                    item.ends_in_ascii_space(context),
                    trailing_whitespace_inline_size,
                );
                if style.text_wrap_mode() != text_wrap_mode::WRAP {
                    self.lines.prevent_wrap_after_trailing_whitespace();
                }
            }
        }
    }

    fn append_pending_start_edges(&mut self) {
        self.lines.append_edges_of_collapsed_whitespace(
            std::mem::take(&mut self.pending_start_margin),
            std::mem::take(&mut self.pending_start_border),
            std::mem::take(&mut self.pending_start_padding),
        );
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
                item.min_content_inline_size().unwrap_or(item.inline_size),
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

    #[allow(clippy::too_many_arguments)]
    fn append_text_broken_between_graphemes(
        &mut self,
        context: &InlineFormattingContext<'_>,
        item: &inline_level_iterator::Item,
        prepared: &prepared_inline_content::PreparedInlineContent,
        style: StyleValues<'_>,
        margin_start: CssPixels,
        border_and_padding_start: CssPixels,
        trailing_whitespace_inline_size: CssPixels,
    ) {
        let text_content = context.pass.text_content(item.node);
        let cluster_inline_sizes = context.pass.with_grapheme_segmenter(item.node, |grapheme_segmenter| {
            item.grapheme_cluster_inline_sizes(
                prepared,
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
    AfterOverflowBreakableText,
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
    let content_inline_shift = if elision.ellipsis_fragment_inline_length_lost_on_left.is_some() {
        CssPixels::default()
    } else {
        content_inline_start - line_box_inline_start
    };
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
    for (index, elided) in line.fragments.iter().enumerate().filter(|(index, fragment)| {
        !fragment.is_text_overflow_ellipsis && (*index == ellipsis_index || fragment.is_fully_truncated)
    }) {
        let (inline_start, inline_size) = if index == ellipsis_index {
            (
                elided.inline_offset - elision.ellipsis_fragment_inline_length_lost_on_left.unwrap_or_default(),
                elision.ellipsis_fragment_inline_length,
            )
        } else {
            (elided.inline_offset, elided.inline_length)
        };
        rect.unite(to_rect(
            inline_start + content_inline_shift,
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
