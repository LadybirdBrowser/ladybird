/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paintable_geometry;
use crate::painting::record::paint::background_resolution::body_background_is_propagated_to_root;
use crate::painting::record::paint::border::{paint_box_borders, present_css_border_widths, style_borders_data};
use crate::painting::record::paint::{background, outline, text};
use crate::painting::record::{PaintPhase, PaintRecorder};
use crate::painting::style_queries;

pub(crate) fn paint(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId, phase: PaintPhase) {
    let root = {
        let block = recorder.data(paintable).containing_block;
        if block.is_invalid()
            || !recorder.layout_arena.paintable_row_is_populated(block)
            || !crate::painting::node_painting::has_lines(recorder.layout_arena, block)
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
        let background_is_propagated_to_root = body_background_is_propagated_to_root(
            recorder.layout_arena,
            paintable,
            recorder.inputs.root_background_source,
        );
        let has_borders = recorder
            .layout_arena
            .node_style_if_live(paintable)
            .is_some_and(style_queries::has_css_borders);
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
        let border = crate::painting::paintable_geometry::committed_border(recorder.layout_arena, paintable);
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
                CssPixelRect::from(piece.border_box_rect).translated_by(root_position),
                present_css_border_widths(style, border, piece.present_edges),
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
            recorder.inputs.outline_auto_color.0,
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
                let color = cursor.color;
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
