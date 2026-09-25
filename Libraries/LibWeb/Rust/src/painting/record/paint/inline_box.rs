/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::record::trace::Observer;

use crate::css::css_pixels::CssPixelRect;
use crate::layout::node_data::NodeSlotId;
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::paintable_geometry;
use crate::painting::record::paint::background_resolution::body_background_is_propagated_to_root;
use crate::painting::record::paint::border::{paint_box_borders, present_css_border_widths, style_borders_data};
use crate::painting::record::paint::{background, outline, text};
use crate::painting::record::{PaintPhase, PaintRecorder};
use crate::painting::style_queries;

pub(crate) fn paint<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId, phase: PaintPhase) {
    // Inline decorations are interleaved with text by the containing line's plan.
    if matches!(phase, PaintPhase::Background | PaintPhase::Border) && !recorder.is_recording_svg_resource_content() {
        return;
    }
    let arena = recorder.layout_arena;
    paint_pieces(
        recorder,
        paintable,
        phase,
        &arena.paintable_side_data(paintable).piece_indices,
    );
}

pub(crate) fn paint_piece<O: Observer>(recorder: &mut PaintRecorder<'_, O>, root: NodeSlotId, index: u32) {
    let paintable = recorder.layout_arena.paintable_side_data(root).inline_box_pieces()[index as usize].node;
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
        paint_pieces(recorder, paintable, phase, &[index]);
    }
    recorder.recorder.set_accumulated_visual_context(Default::default());
}

fn paint_pieces<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    phase: PaintPhase,
    piece_indices: &[u32],
) {
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
    let side = layout_arena.paintable_side_data(root);
    let root_pieces = &side.inline_box_pieces();
    let facts = recorder.base_paint_facts(paintable);

    if phase == PaintPhase::Background && facts.is_visible {
        crate::painting::record::paint::paint_backdrop_filter(recorder, paintable, &facts);
        let background_is_propagated_to_root = body_background_is_propagated_to_root(
            recorder.layout_arena,
            paintable,
            recorder.inputs.uncaptured.root_background_source,
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
        let outline = outline::outline_data_for_paint(recorder, node);
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
        if crate::painting::fragment_ownership::is_self_painting_inline(layout_arena, paintable) {
            if recorder.is_recording_svg_resource_content() {
                text::paint_fragments_foreground(recorder, root, Some(paintable));
            }
            text::paint_cursor(recorder, root, Some(paintable));
        }
        if facts.is_visible
            && let Some(caret) = recorder.inputs.caret
            && caret.target == crate::painting::record::inputs::CaretTarget::EmptyInline(paintable)
        {
            let color = caret.color;
            if color.alpha() != 0 {
                let rect = recorder.converter.rounded_device_rect(caret.rect);
                recorder.recorder.fill_rect(rect, color, ForceDarkRole::Foreground);
            }
        }
    }
}
