/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::record::trace::Observer;

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::NodeSlotId;
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::record::paint::background_resolution::body_background_is_propagated_to_root;
use crate::painting::record::paint::border::{
    ALL_PIECE_EDGES, paint_box_borders, present_css_border_widths, style_borders_data,
};
use crate::painting::record::paint::{background, outline, text};
use crate::painting::record::{PaintPhase, PaintRecorder};
use crate::painting::style_queries;

pub(crate) fn paint<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId, phase: PaintPhase) {
    // Inline decorations are interleaved with text by the containing line's plan.
    if matches!(phase, PaintPhase::Background | PaintPhase::Border) && !recorder.is_recording_svg_resource_content() {
        return;
    }
    paint_pieces(recorder, paintable, phase, None);
}

pub(crate) fn paint_piece<O: Observer>(recorder: &mut PaintRecorder<'_, O>, root: NodeSlotId, index: u32) {
    let paintable = recorder.source.committed_side_data(root).inline_box_pieces()[index as usize].node;
    let facts = recorder.base_paint_facts(paintable);
    for phase in [PaintPhase::Background, PaintPhase::Border] {
        if facts.paint_phase_mask & phase.bit() == 0 {
            continue;
        }
        if facts.has_fixed_background || facts.has_scroll_offset_dependent_background {
            recorder.mark_live_producer();
        }
        let context = recorder.context_for_phase(paintable, phase);
        recorder.recorder.set_accumulated_visual_context(context);
        paint_pieces(recorder, paintable, phase, Some(&[index]));
    }
    recorder.recorder.set_accumulated_visual_context(Default::default());
}

fn paint_pieces<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    phase: PaintPhase,
    only_piece_indices: Option<&[u32]>,
) {
    let root = {
        let block = recorder.data(paintable).containing_block;
        if block.is_invalid()
            || !recorder.source.paintable_row_is_populated(block)
            || !crate::painting::node_painting::has_lines(recorder.source, block)
        {
            return;
        }
        block
    };
    let root_positions = recorder.line_root_positions(root);
    let layout_arena = recorder.source;
    let side = layout_arena.committed_side_data(root);
    let root_position_of = |piece: &crate::layout::inline_content::InlineBoxPieceRecord| {
        root_positions.of_line(side.lines(), piece.line_index as usize)
    };
    let root_pieces = &side.inline_box_pieces();
    let piece_indices = only_piece_indices.unwrap_or_else(|| side.inline_box_piece_indices(paintable));
    let facts = recorder.base_paint_facts(paintable);

    if phase == PaintPhase::Background && facts.is_visible {
        crate::painting::record::paint::paint_backdrop_filter(recorder, paintable, &facts);
        let background_is_propagated_to_root = body_background_is_propagated_to_root(
            recorder.source,
            paintable,
            recorder.inputs.uncaptured.root_background_source,
        );
        let style = recorder.source.node_style_if_live(paintable);
        let has_borders = style.is_some_and(style_queries::has_css_borders);
        let border = crate::painting::paintable_geometry::committed_border(recorder.source, paintable);
        let background_rect_of = |piece: &crate::layout::inline_content::InlineBoxPieceRecord| {
            let border_box_rect = CssPixelRect::from(piece.border_box_rect).translated_by(root_position_of(piece));
            if has_borders {
                border_box_rect
            } else {
                piece.shrunken_by_present_edges(border_box_rect, border)
            }
        };

        // https://drafts.csswg.org/css-break-4/#valdef-box-decoration-break-slice
        // "The effect is as though the element were rendered with no breaks present, and then sliced by the breaks
        // afterward." So a background image of a box broken across lines is laid out once, over a strip as long as
        // all of its pieces placed end to end in the inline direction, and each piece shows its own slice of it.
        let background_strip = style
            .filter(|style| {
                style.misc_layout().box_decoration_break == crate::css::css_enums::box_decoration_break::SLICE
                    && style_queries::background_layers_have_image(*style)
            })
            .map(|style| {
                let is_horizontal = style.writing_mode() == crate::css::css_enums::writing_mode::HORIZONTAL_TB;
                let inline_axis_is_reverse = style.inline_axis_is_reverse();
                let inline_size_of = |rect: CssPixelRect| if is_horizontal { rect.width } else { rect.height };
                // NB: The pieces are painted one by one, interleaved with the text of their lines, so the strip is
                //     measured over all of the box's pieces, in logical order, rather than those painted now.
                let all_piece_indices = side.inline_box_piece_indices(paintable);
                let inline_sizes: Vec<(u32, CssPixels)> = all_piece_indices
                    .iter()
                    .filter(|index| !root_pieces[**index as usize].is_geometry_only_placeholder)
                    .map(|index| {
                        (
                            *index,
                            inline_size_of(background_rect_of(&root_pieces[*index as usize])),
                        )
                    })
                    .collect();
                let strip_inline_size = inline_sizes
                    .iter()
                    .fold(CssPixels::default(), |total, (_, size)| total + *size);
                (is_horizontal, inline_axis_is_reverse, strip_inline_size, inline_sizes)
            })
            .filter(|(_, _, _, inline_sizes)| inline_sizes.len() > 1);

        for piece_index in piece_indices {
            let piece = &root_pieces[*piece_index as usize];
            if piece.is_geometry_only_placeholder {
                continue;
            }
            let border_box_rect = CssPixelRect::from(piece.border_box_rect).translated_by(root_position_of(piece));
            let padding_box_rect = piece.shrunken_by_present_edges(border_box_rect, border);
            let border_radii = recorder.piece_border_radii(paintable, piece);
            if !background_is_propagated_to_root {
                let piece_background_rect = background_rect_of(piece);
                if let Some((is_horizontal, inline_axis_is_reverse, strip_inline_size, inline_sizes)) =
                    &background_strip
                {
                    let (is_horizontal, inline_axis_is_reverse, strip_inline_size) =
                        (*is_horizontal, *inline_axis_is_reverse, *strip_inline_size);
                    let inline_offset_in_strip = inline_sizes
                        .iter()
                        .take_while(|(index, _)| index != piece_index)
                        .fold(CssPixels::default(), |total, (_, size)| total + *size);
                    let piece_inline_size = if is_horizontal {
                        piece_background_rect.width
                    } else {
                        piece_background_rect.height
                    };
                    // Where the inline axis runs right to left, or bottom to top, the first piece takes the physical
                    // end of the strip.
                    let offset = if inline_axis_is_reverse {
                        strip_inline_size - inline_offset_in_strip - piece_inline_size
                    } else {
                        inline_offset_in_strip
                    };
                    let strip_rect = if is_horizontal {
                        CssPixelRect::new(
                            piece_background_rect.x - offset,
                            piece_background_rect.y,
                            strip_inline_size,
                            piece_background_rect.height,
                        )
                    } else {
                        CssPixelRect::new(
                            piece_background_rect.x,
                            piece_background_rect.y - offset,
                            piece_background_rect.width,
                            strip_inline_size,
                        )
                    };
                    let piece_clip =
                        crate::painting::display_list::builder::PendingInlineClip::intersecting_rounded_rect(
                            recorder.converter.rounded_device_rect(piece_background_rect).to_float(),
                            border_radii.as_corners(&recorder.converter),
                        );
                    recorder.record_with_inline_clips(&[piece_clip], |recorder| {
                        background::paint_background_within(
                            recorder,
                            paintable,
                            strip_rect,
                            crate::painting::border_radii::BorderRadii::default(),
                            None,
                            Some(piece_background_rect),
                        );
                    });
                } else {
                    background::paint_background_within(
                        recorder,
                        paintable,
                        piece_background_rect,
                        border_radii,
                        None,
                        None,
                    );
                }
            }
            crate::painting::record::paint::shadow::paint_box_shadow(
                recorder,
                paintable,
                border_box_rect,
                padding_box_rect,
                border_radii,
            );
        }
    }

    if phase == PaintPhase::Border && facts.is_visible {
        let converter = recorder.converter;
        let Some(style) = recorder.source.node_style_if_live(paintable) else {
            return;
        };
        let border = crate::painting::paintable_geometry::committed_border(recorder.source, paintable);
        for piece_index in piece_indices {
            let piece = &root_pieces[*piece_index as usize];
            if piece.is_geometry_only_placeholder {
                continue;
            }
            let borders_data = style_borders_data(style, border, piece.present_edges, &converter);
            let border_radii = recorder.piece_border_radii(paintable, piece);
            paint_box_borders(
                recorder,
                paintable,
                &facts,
                CssPixelRect::from(piece.border_box_rect).translated_by(root_position_of(piece)),
                present_css_border_widths(style, border, piece.present_edges),
                ALL_PIECE_EDGES,
                &borders_data,
                border_radii,
            );
        }
    }

    if phase == PaintPhase::Outline && facts.is_visible {
        let node = paintable;
        let outline = outline::outline_data_for_paint(recorder, node);
        let outline_offset = crate::painting::style_queries::outline_offset(recorder.source, node);
        for piece_index in piece_indices {
            let piece = &root_pieces[*piece_index as usize];
            if piece.is_geometry_only_placeholder {
                continue;
            }
            let border_radii = recorder.piece_border_radii(paintable, piece);
            outline::paint_outline(
                recorder,
                outline,
                outline_offset,
                CssPixelRect::from(piece.border_box_rect).translated_by(root_position_of(piece)),
                border_radii,
            );
        }
    }

    if phase == PaintPhase::Foreground {
        // Fragments (and the caret between their glyphs) are not gated on this box being
        // visible: descendants may set visibility: visible again under a hidden box, so each
        // fragment is filtered by its own node's visibility.
        if crate::painting::fragment_ownership::is_self_painting_inline(layout_arena, paintable) {
            if recorder.is_recording_svg_resource_content() {
                text::paint_fragments_foreground(recorder, root, Some(paintable));
            }
            text::paint_cursor(recorder, root, Some(paintable));
        }
        if facts.is_visible
            && let Some(caret) = recorder.inputs.caret
            && caret.target == crate::painting::record::inputs::CaretTarget::EmptyInline(paintable)
            && recorder.window_is_focused()
        {
            let color = caret.color;
            if color.alpha() != 0 {
                let rect = recorder.converter.rounded_device_rect(caret.rect);
                recorder.recorder.fill_rect(rect, color, ForceDarkRole::Foreground);
            }
        }
    }
}
