/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::record::trace::Observer;

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::NodeSlotId;
use crate::painting::record::paint::background_resolution::body_background_is_propagated_to_root;
use crate::painting::record::paint::border::{
    ALL_PIECE_EDGES, paint_box_borders, present_css_border_widths, style_borders_data,
};
use crate::painting::record::paint::{background, outline, text};
use crate::painting::record::{PaintPhase, PaintRecorder};
use crate::painting::style_queries;

pub(crate) fn paint<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId, phase: PaintPhase) {
    // Inline decorations are interleaved with text by the containing line's plan.
    if matches!(phase, PaintPhase::Background | PaintPhase::Border) {
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
        // afterward." So the background image or box shadow of a box broken across lines is drawn once, over a strip
        // as long as all its pieces placed end to end in the inline direction, and each piece shows its own slice.
        let slices = style.is_some_and(|style| {
            style.misc_layout().box_decoration_break == crate::css::css_enums::box_decoration_break::SLICE
        });
        // NB: The pieces are painted one by one, interleaved with the text of their lines, so a strip is measured once
        //     per recording over all of the box's pieces, in logical order, rather than over those painted now.
        let strip_of =
            |recorder: &mut PaintRecorder<'_, O>,
             kind: InlineStripKind,
             rect_of: &dyn Fn(&crate::layout::inline_content::InlineBoxPieceRecord) -> CssPixelRect| {
                if let Some(strip) = recorder.scratch.inline_strips.get(&(paintable, kind)) {
                    return strip.clone();
                }
                let strip = style.and_then(|style| {
                    InlineStrip::measure(
                        style,
                        side.inline_box_piece_indices(paintable)
                            .iter()
                            .filter(|index| !root_pieces[**index as usize].is_geometry_only_placeholder)
                            .map(|index| (*index, rect_of(&root_pieces[*index as usize]))),
                    )
                    .map(std::sync::Arc::new)
                });
                recorder.scratch.inline_strips.insert((paintable, kind), strip.clone());
                strip
            };
        let background_strip = if slices && style.is_some_and(style_queries::background_layers_have_image) {
            strip_of(recorder, InlineStripKind::Background, &background_rect_of)
        } else {
            None
        };
        let shadow_layers = style.map_or(&[][..], |style| style.effects().box_shadows.as_slice());
        let shadow_strip = if slices && !shadow_layers.is_empty() {
            strip_of(recorder, InlineStripKind::BoxShadow, &|piece| {
                CssPixelRect::from(piece.border_box_rect).translated_by(root_position_of(piece))
            })
        } else {
            None
        };

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
                if let Some(strip) = &background_strip {
                    let strip_rect = strip.rect_of_piece_at(*piece_index, piece_background_rect);
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
            if let (Some(strip), Some(style)) = (&shadow_strip, style) {
                let strip_rect = strip.rect_of_piece_at(*piece_index, border_box_rect);
                let strip_radii = crate::painting::visual_context::node_values::piece_border_radii_data(
                    style,
                    strip_rect.width,
                    strip_rect.height,
                    ALL_PIECE_EDGES,
                );
                // NB: A slice ends only where the box breaks: On every other side, the shadow reaches out as far as
                //     it does from the whole box.
                let reach = shadow_layers
                    .iter()
                    .map(|layer| {
                        CssPixels::from_raw(layer.offset_x.saturating_abs())
                            + CssPixels::from_raw(layer.offset_y.saturating_abs())
                            + CssPixels::from_raw(layer.spread_distance.saturating_abs()) * 8
                            + CssPixels::from_raw(layer.blur_radius) * 4
                    })
                    .max()
                    .unwrap_or_default();
                // The reach is measured in device pixels, with one more for the shadow painter rounding the box's edge
                // and the shadow's offset each on their own. The broken edges stay where the piece's own edges round to.
                let device_reach = recorder.converter.enclosing_device_pixels(reach) + 1;
                let reach_beyond = |edge: u8| {
                    if piece.present_edges & edge != 0 {
                        device_reach
                    } else {
                        0
                    }
                };
                use crate::painting::paintable_data::{
                    PIECE_EDGE_BOTTOM, PIECE_EDGE_LEFT, PIECE_EDGE_RIGHT, PIECE_EDGE_TOP,
                };
                let slice_rect = recorder.converter.rounded_device_rect(border_box_rect).inflated_edges(
                    reach_beyond(PIECE_EDGE_TOP),
                    reach_beyond(PIECE_EDGE_RIGHT),
                    reach_beyond(PIECE_EDGE_BOTTOM),
                    reach_beyond(PIECE_EDGE_LEFT),
                );
                let slice_clip = crate::painting::display_list::builder::PendingInlineClip::intersecting_rounded_rect(
                    slice_rect.to_float(),
                    Default::default(),
                );
                recorder.record_with_inline_clips(&[slice_clip], |recorder| {
                    crate::painting::record::paint::shadow::paint_box_shadow(
                        recorder,
                        paintable,
                        strip_rect,
                        strip_rect.shrunken(border.top, border.right, border.bottom, border.left),
                        strip_radii,
                    );
                });
            } else {
                crate::painting::record::paint::shadow::paint_box_shadow(
                    recorder,
                    paintable,
                    border_box_rect,
                    padding_box_rect,
                    border_radii,
                );
            }
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
        let (outline, outline_offset) = outline::outline_data_and_offset(
            recorder.source,
            node,
            recorder.inputs.accessibility_focus_target,
            || recorder.window_is_focused(),
            recorder.inputs.outline_auto_color.0,
        );
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
                recorder
                    .recorder
                    .paint_caret(rect, color, caret.blink_cycle_start_time_ns, caret.should_blink);
            }
        }
    }
}

/// What an inline box draws over the strip of its pieces.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum InlineStripKind {
    Background,
    BoxShadow,
}

/// A strip as long as all the pieces of an inline box placed end to end in the inline direction, over which a box
/// broken with "box-decoration-break: slice" draws what it draws once, and from which each piece shows its own slice.
pub(crate) struct InlineStrip {
    is_horizontal: bool,
    inline_axis_is_reverse: bool,
    inline_size: CssPixels,
    /// Where each piece starts in the strip, in logical order.
    piece_inline_offsets: std::collections::HashMap<u32, CssPixels>,
}

impl InlineStrip {
    /// The strip over `pieces`, each with the rect it takes, or None where there's only the one piece to slice.
    fn measure(
        style: crate::css::computed_value_views::ComputedValuesView<'_>,
        pieces: impl Iterator<Item = (u32, CssPixelRect)>,
    ) -> Option<Self> {
        let is_horizontal = style.writing_mode() == crate::css::css_enums::writing_mode::HORIZONTAL_TB;
        let mut inline_size = CssPixels::default();
        let piece_inline_offsets: std::collections::HashMap<u32, CssPixels> = pieces
            .map(|(index, rect)| {
                let offset = inline_size;
                inline_size += if is_horizontal { rect.width } else { rect.height };
                (index, offset)
            })
            .collect();
        if piece_inline_offsets.len() < 2 {
            return None;
        }
        Some(Self {
            is_horizontal,
            inline_axis_is_reverse: style.inline_axis_is_reverse(),
            inline_size,
            piece_inline_offsets,
        })
    }

    /// The strip placed so that the piece at `piece_index`, which takes `piece_rect`, shows its own slice of it.
    fn rect_of_piece_at(&self, piece_index: u32, piece_rect: CssPixelRect) -> CssPixelRect {
        let inline_offset_in_strip = self.piece_inline_offsets.get(&piece_index).copied().unwrap_or_default();
        let piece_inline_size = if self.is_horizontal {
            piece_rect.width
        } else {
            piece_rect.height
        };
        // Where the inline axis runs right to left, or bottom to top, the first piece takes the physical end of the
        // strip.
        let offset = if self.inline_axis_is_reverse {
            self.inline_size - inline_offset_in_strip - piece_inline_size
        } else {
            inline_offset_in_strip
        };
        if self.is_horizontal {
            CssPixelRect::new(piece_rect.x - offset, piece_rect.y, self.inline_size, piece_rect.height)
        } else {
            CssPixelRect::new(piece_rect.x, piece_rect.y - offset, piece_rect.width, self.inline_size)
        }
    }
}
