/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paintable_data::{PIECE_EDGE_BOTTOM, PIECE_EDGE_LEFT, PIECE_EDGE_RIGHT, PIECE_EDGE_TOP};
use crate::painting::paintable_geometry;
use crate::painting::record::paint::border::paint_box_borders;
use crate::painting::record::paint::border::{BorderDataDevicePixels, BordersDataDevicePixels};
use crate::painting::record::paint::{background, outline, text};
use crate::painting::record::{PaintPhase, PaintRecorder};

pub(crate) fn paint(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId, phase: PaintPhase) {
    let root = {
        let block = recorder.data(paintable).containing_block;
        if block.is_invalid()
            || !recorder.layout_arena.paintable_row_is_populated(block)
            || !recorder.data(block).kind.has_lines()
        {
            return;
        }
        block
    };
    let root_position = paintable_geometry::absolute_position(recorder.layout_arena, root);
    let layout_arena = recorder.layout_arena;
    let piece_indices = &layout_arena.paintable_side_data(paintable).piece_indices;
    let root_pieces = &layout_arena.paintable_side_data(root).inline_box_pieces;
    let facts = recorder.base_paint_facts(paintable);
    let self_painting_inline = crate::painting::fragment_ownership::is_self_painting_inline(layout_arena, paintable);

    if phase == PaintPhase::Background && facts.is_visible {
        crate::painting::record::paint::paint_backdrop_filter(recorder, paintable, &facts);
        let background_is_propagated_to_root = {
            let node_flags = recorder.layout_arena.node_flags_if_live(paintable);
            node_flags & crate::layout::node_data::NodeFlag::IsBody as u32 != 0
                && recorder
                    .paint_host
                    .root_background_source()
                    .use_body_background_properties
        };
        let has_borders = {
            let style = recorder.layout_arena.node_style_if_live(paintable);
            let zero = CssPixels::from_raw(0);
            style.is_some_and(|style| {
                style.border_top_width() != zero
                    || style.border_right_width() != zero
                    || style.border_bottom_width() != zero
                    || style.border_left_width() != zero
            })
        };
        for piece_index in piece_indices {
            let piece = &root_pieces[*piece_index as usize];
            if piece.is_geometry_only_placeholder {
                continue;
            }
            let border_box_rect = CssPixelRect::from(piece.border_box_rect).translated_by(root_position);
            let padding_box_rect = piece.shrunken_by_present_edges(
                border_box_rect,
                crate::painting::paintable_geometry::committed_border(recorder.layout_arena, paintable),
            );
            let border_radii = recorder.piece_border_radii(paintable, piece);
            if !background_is_propagated_to_root {
                background::paint_background_within(
                    recorder,
                    paintable,
                    if has_borders { border_box_rect } else { padding_box_rect },
                    border_radii,
                );
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
        let Some(style) = recorder.layout_arena.node_style_if_live(paintable) else {
            return;
        };
        let zero = CssPixels::from_raw(0);
        let default_side = BorderDataDevicePixels {
            color: libgfx_rust::Color::TRANSPARENT,
            line_style: crate::css::css_enums::line_style::NONE,
            width: 0,
        };
        let side = |present: bool, color: u32, line_style: u8, width: CssPixels| {
            if !present {
                return (zero, default_side);
            }
            (
                width,
                BorderDataDevicePixels {
                    color: libgfx_rust::Color(color),
                    line_style,
                    width: converter.enclosing_device_pixels(width),
                },
            )
        };
        let border = crate::painting::paintable_geometry::committed_border(recorder.layout_arena, paintable);
        for piece_index in piece_indices {
            let piece = &root_pieces[*piece_index as usize];
            if piece.is_geometry_only_placeholder {
                continue;
            }
            let has = |edge: u8| piece.present_edges & edge != 0;
            let (top_width, top) = side(
                border.top != zero && has(PIECE_EDGE_TOP),
                style.border_top_color(),
                style.border_top_style(),
                style.border_top_width(),
            );
            let (right_width, right) = side(
                border.right != zero && has(PIECE_EDGE_RIGHT),
                style.border_right_color(),
                style.border_right_style(),
                style.border_right_width(),
            );
            let (bottom_width, bottom) = side(
                border.bottom != zero && has(PIECE_EDGE_BOTTOM),
                style.border_bottom_color(),
                style.border_bottom_style(),
                style.border_bottom_width(),
            );
            let (left_width, left) = side(
                border.left != zero && has(PIECE_EDGE_LEFT),
                style.border_left_color(),
                style.border_left_style(),
                style.border_left_width(),
            );
            let borders_data = BordersDataDevicePixels {
                top,
                right,
                bottom,
                left,
            };
            let border_radii = recorder.piece_border_radii(paintable, piece);
            paint_box_borders(
                recorder,
                paintable,
                &facts,
                CssPixelRect::from(piece.border_box_rect).translated_by(root_position),
                [top_width, right_width, bottom_width, left_width],
                &borders_data,
                border_radii,
            );
        }
    }

    if phase == PaintPhase::Outline && facts.is_visible {
        let node = paintable;
        let outline = crate::painting::style_queries::outline_data(
            recorder.layout_arena,
            node,
            recorder.inputs.window_is_focused,
            recorder.inputs.outline_auto_color,
        );
        let outline_offset = crate::painting::style_queries::outline_offset(recorder.layout_arena, node);
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
                CssPixelRect::from(piece.border_box_rect).translated_by(root_position),
                border_radii,
            );
        }
    }

    if phase == PaintPhase::Foreground {
        // Fragments (and the caret between their glyphs) are not gated on this box being
        // visible: descendants may set visibility: visible again under a hidden box, so each
        // fragment is filtered by its own node's visibility.
        if self_painting_inline {
            text::paint_fragments_foreground(recorder, root, Some(paintable));
            text::paint_cursor(recorder, root, Some(paintable));
        }
        if facts.is_visible {
            let cursor = recorder.paint_host.cursor_facts(
                recorder.layout_node_shell(paintable),
                recorder.layout_node_shell(paintable),
            );
            if cursor.paints {
                let color = libgfx_rust::Color(cursor.color);
                if color.alpha() != 0 {
                    let rect = recorder
                        .converter
                        .rounded_device_rect(crate::css::css_pixels::CssPixelRect::from(cursor.rect));
                    recorder.recorder.fill_rect(rect, color);
                }
            }
        }
    }
}
