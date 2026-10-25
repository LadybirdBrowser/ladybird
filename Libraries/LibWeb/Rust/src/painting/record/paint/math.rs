/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::paint_read::PaintRead;
use crate::painting::record::trace::Observer;

use crate::layout::math_formatting_context::MathPaintData;
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::{DisplayListGlyph, FontResourceId};
use crate::painting::display_list::recorder::GlyphRunForRecording;
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::paintable_geometry;
use crate::painting::record::PaintRecorder;
use libgfx_rust::{Color, FloatPoint, Orientation};

/// Paints what MathML layout drew for a box: its fraction bar, its radical symbol and overbar, or the glyphs of its
/// stretched operator.
pub(crate) fn paint_foreground<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    data: &MathPaintData,
) {
    // https://w3c.github.io/mathml-core/#fraction-with-nonzero-line-thickness
    // https://w3c.github.io/mathml-core/#radical-symbol
    // The radical symbol must only be painted if the visibility of the <msqrt> or <mroot> element is visible. In that
    // case, the radical symbol must be painted with the color of that element.
    if !recorder.is_visible(paintable) {
        return;
    }
    let Some(style) = recorder.source.node_style_if_live(paintable) else {
        return;
    };
    let color = Color(style.inherited_text().color);
    // NB: The glyphs of a stretched or large operator are painted instead of its text, so they use the text fill
    //     color like the text would.
    let glyph_color = if data.replaces_text {
        Color(style.inherited_text().webkit_text_fill_color)
    } else {
        color
    };
    let font_smoothing = style.inherited_text().font_smoothing;
    let content_box = paintable_geometry::absolute_rect(recorder.source, paintable);
    let converter = recorder.converter;
    for rule in &data.rules {
        let rect = rule.translated_by(content_box.location());
        recorder
            .recorder
            .fill_rect(converter.rounded_device_rect(rect), color, ForceDarkRole::Foreground);
    }
    let scale = recorder.inputs.device_pixels_per_css_pixel;
    let origin = FloatPoint {
        x: (f64::from(content_box.x.to_float()) * scale) as f32,
        y: (f64::from(content_box.y.to_float()) * scale) as f32,
    };
    for run in &data.glyph_runs {
        let font_id = recorder.register_font(&run.font);
        // NB: Display list glyphs are positioned by the top of the font's ascent rather than by their baseline.
        let ascent = run.font.facts().ascent;
        let glyphs: Vec<DisplayListGlyph> = run
            .glyphs
            .iter()
            .map(|glyph| DisplayListGlyph {
                position: FloatPoint {
                    x: glyph.x,
                    y: glyph.y - ascent,
                },
                glyph_id: glyph.glyph_id,
            })
            .collect();
        let ink_rect = converter.enclosing_device_rect(run.ink_bounds.translated_by(content_box.location()));
        recorder.recorder.draw_glyph_run(
            origin,
            GlyphRunForRecording {
                font_smoothing,
                font_id: FontResourceId(font_id),
                glyphs: &glyphs,
            },
            glyph_color,
            ink_rect,
            scale,
            Orientation::Horizontal,
            ink_rect,
            ForceDarkRole::Foreground,
        );
    }
}
