/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::painting::display_list::commands::{DisplayListGlyph, FontResourceId};
use crate::painting::display_list::recorder::GlyphRunForRecording;
use crate::painting::host::{FfiTextShadowLayer, FfiTextSpan};
use crate::painting::paintable_data::PaintableSlotId;
use crate::painting::record::PaintRecorder;
use crate::painting::text_fragment::{self, SelectionOffsets};
use libgfx_rust::{Color, FloatPoint, IntRect, Orientation};

fn glyphs_of(run: &crate::painting::paintable_data::GlyphRunRecord) -> Vec<DisplayListGlyph> {
    run.glyphs
        .iter()
        .map(|glyph| DisplayListGlyph {
            position: FloatPoint { x: glyph.x, y: glyph.y },
            glyph_id: glyph.glyph_id,
        })
        .collect()
}

pub(crate) struct GlyphRunEmission {
    pub glyphs: Vec<DisplayListGlyph>,
    pub baseline_start: FloatPoint,
    pub orientation: Orientation,
    pub glyph_bounding_rect: IntRect,
}

pub(crate) fn glyph_run_emission(
    fragment: &crate::painting::paintable_data::FragmentRecord,
    run: &crate::painting::paintable_data::GlyphRunRecord,
    facts: &crate::painting::host::FfiGlyphRunFacts,
    fragment_absolute_rect: CssPixelRect,
    scale: f64,
) -> GlyphRunEmission {
    let baseline_start = FloatPoint {
        x: (fragment_absolute_rect.x.to_float() as f64 * scale) as f32,
        y: ((fragment_absolute_rect.y.to_float() + fragment.baseline.to_float()) as f64 * scale) as f32,
    };
    let orientation = if fragment.writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB {
        Orientation::Horizontal
    } else {
        Orientation::Vertical
    };
    let glyph_bounding_rect = IntRect::new(
        (facts.blob_bounds[0] + baseline_start.x).round_ties_even() as i32,
        (facts.blob_bounds[1] + baseline_start.y).round_ties_even() as i32,
        facts.blob_bounds[2].round_ties_even() as i32,
        facts.blob_bounds[3].round_ties_even() as i32,
    );
    GlyphRunEmission {
        glyphs: glyphs_of(run),
        baseline_start,
        orientation,
        glyph_bounding_rect,
    }
}

pub(crate) fn paint_fragments_foreground(
    recorder: &mut PaintRecorder<'_>,
    block: PaintableSlotId,
    owner: Option<PaintableSlotId>,
) {
    let filter = crate::painting::fragment_ownership::effective_filter(recorder.paintables, owner.unwrap_or(block));
    let fragment_count = recorder.paintables.side(block).fragments.len();
    let mut owned_fragment_indices = Vec::with_capacity(fragment_count);
    filter.for_each_owned_fragment_index(fragment_count, |index| owned_fragment_indices.push(index as u32));
    let sink = recorder
        .paint_host
        .text_spans(recorder.shell(block), &owned_fragment_indices);

    let mut shadow_cursor = 0usize;
    let mut spans_with_shadows: Vec<(FfiTextSpan, Vec<FfiTextShadowLayer>)> = Vec::with_capacity(sink.spans.len());
    for span in &sink.spans {
        let count = span.shadow_layer_count as usize;
        let layers = sink.shadows[shadow_cursor..shadow_cursor + count].to_vec();
        shadow_cursor += count;
        spans_with_shadows.push((*span, layers));
    }

    for (span, _) in &spans_with_shadows {
        if Color(span.background_color).alpha() > 0 {
            let selection_rect = selection_rect(recorder, block, span);
            let converter = recorder.converter;
            recorder.recorder.fill_rect(
                converter.rounded_device_rect(selection_rect),
                Color(span.background_color),
            );
        }
    }

    for (span, layers) in &spans_with_shadows {
        paint_text_shadow(recorder, block, span, layers);
    }

    for (span, _) in &spans_with_shadows {
        let sets = crate::painting::record::paint::text_decoration::decoration_sets_for_span(recorder, block, span);
        paint_text_fragment(recorder, block, span, &sets);
    }
}

fn selection_rect(recorder: &PaintRecorder<'_>, block: PaintableSlotId, span: &FfiTextSpan) -> CssPixelRect {
    if !span.has_selection_offsets {
        return CssPixelRect::default();
    }
    let fragment = &recorder.paintables.side(block).fragments[span.fragment_index as usize];
    let offsets = SelectionOffsets {
        start: span.selection_start,
        end: span.selection_end,
    };
    text_fragment::rect_for_selection_offsets(recorder.layout_arena, recorder.paintables, fragment, offsets, || {
        text_fragment::first_available_font(recorder.layout_arena, fragment)
    })
}

fn paint_text_shadow(
    recorder: &mut PaintRecorder<'_>,
    block: PaintableSlotId,
    span: &FfiTextSpan,
    shadow_layers: &[FfiTextShadowLayer],
) {
    if shadow_layers.is_empty() {
        return;
    }
    let fragment = &recorder.paintables.side(block).fragments[span.fragment_index as usize];
    let Some(run) = &fragment.glyph_run else {
        return;
    };
    if run.glyphs.is_empty() {
        return;
    }

    // If this is a partial span, slice the glyph run to only include the relevant glyphs.
    let glyphs = glyphs_of(run);
    let mut span_glyphs = glyphs.as_slice();
    if span.start_code_unit != 0 || span.end_code_unit != fragment.length_in_code_units {
        let mut start_glyph = 0usize;
        let mut glyph_count = 0usize;
        let mut code_unit_offset = 0usize;
        for (i, glyph) in run.glyphs.iter().enumerate() {
            if code_unit_offset == span.start_code_unit {
                start_glyph = i;
            }
            code_unit_offset += glyph.length_in_code_units;
            if code_unit_offset == span.end_code_unit {
                glyph_count = i - start_glyph + 1;
                break;
            }
        }
        if glyph_count > 0 {
            span_glyphs = &glyphs[start_glyph..start_glyph + glyph_count];
        }
    }

    let converter = recorder.converter;
    let scale = recorder.inputs.device_pixels_per_css_pixel;
    let fragment_width = converter.enclosing_device_pixels(fragment.size.width);
    let fragment_height = converter.enclosing_device_pixels(fragment.size.height);
    let fragment_baseline = converter.rounded_device_pixels(fragment.baseline);
    let fragment_absolute_rect = text_fragment::absolute_rect(recorder.layout_arena, recorder.paintables, fragment);
    let facts = recorder
        .paint_host
        .glyph_run_facts(recorder.shell(block), span.fragment_index, scale);

    // Shadow layers are ordered front-to-back, so we paint them in reverse.
    for layer in shadow_layers.iter().rev() {
        let blur_radius = converter.rounded_device_pixels(layer.blur_radius);
        // Space around the painted text to allow it to blur.
        let margin = blur_radius * 2;
        let text_rect = IntRect::new(margin, margin, fragment_width, fragment_height);
        let bounding_rect = IntRect::new(
            0,
            0,
            text_rect.width + margin + margin,
            text_rect.height + margin + margin,
        );

        // FIXME: this is close but not quite perfect. non integer scale values can be offset by tiny amounts.
        let css_margin = layer.blur_radius * 2;
        let draw_location = FloatPoint {
            x: (fragment_absolute_rect.x + layer.offset_x - css_margin).to_float() * scale as f32,
            y: (fragment_absolute_rect.y + layer.offset_y - css_margin).to_float() * scale as f32,
        };
        recorder.recorder.paint_text_shadow(
            blur_radius,
            bounding_rect,
            IntRect::new(
                text_rect.x,
                text_rect.y + fragment_baseline,
                text_rect.width,
                text_rect.height,
            ),
            GlyphRunForRecording {
                font_id: FontResourceId(facts.font_id),
                glyphs: span_glyphs,
            },
            scale,
            Color(layer.color),
            draw_location,
        );
    }
}

fn paint_text_fragment(
    recorder: &mut PaintRecorder<'_>,
    block: PaintableSlotId,
    span: &FfiTextSpan,
    decoration_sets: &[crate::painting::record::paint::text_decoration::TextDecorationSet],
) {
    // Skip non-text spans (they're only for shadow painting).
    if span.start_code_unit == span.end_code_unit {
        return;
    }
    let fragment = &recorder.paintables.side(block).fragments[span.fragment_index as usize];
    if recorder.inputs.should_show_line_box_borders {
        let converter = recorder.converter;
        let fragment_absolute_rect = text_fragment::absolute_rect(recorder.layout_arena, recorder.paintables, fragment);
        let fragment_absolute_device_rect = converter.enclosing_device_rect(fragment_absolute_rect);
        recorder
            .recorder
            .draw_rect(fragment_absolute_device_rect, Color::from_rgb(0, 255, 0), false);
        let one = crate::css::css_pixels::CssPixels::from_integer(1);
        let baseline_start = converter.rounded_device_point(
            fragment_absolute_rect
                .location()
                .translated(crate::css::css_pixels::CssPixels::default(), fragment.baseline),
        );
        let baseline_end = converter.rounded_device_point(
            crate::css::css_pixels::CssPixelPoint::new(fragment_absolute_rect.right(), fragment_absolute_rect.y)
                .translated(-one, fragment.baseline),
        );
        recorder.recorder.draw_line(
            baseline_start,
            baseline_end,
            Color::from_rgb(255, 0, 0),
            1,
            libgfx_rust::LineStyle::Solid,
            Color::TRANSPARENT,
        );
    }
    let Some(run) = &fragment.glyph_run else {
        return;
    };
    let converter = recorder.converter;
    let scale = recorder.inputs.device_pixels_per_css_pixel;
    let fragment_absolute_rect = text_fragment::absolute_rect(recorder.layout_arena, recorder.paintables, fragment);
    let fragment_device_rect = converter.enclosing_device_rect(fragment_absolute_rect);
    let facts = recorder
        .paint_host
        .glyph_run_facts(recorder.shell(block), span.fragment_index, scale);
    let GlyphRunEmission {
        glyphs,
        baseline_start,
        orientation,
        glyph_bounding_rect,
    } = glyph_run_emission(fragment, run, &facts, fragment_absolute_rect, scale);
    let run_for_recording = GlyphRunForRecording {
        font_id: FontResourceId(facts.font_id),
        glyphs: &glyphs,
    };

    // Paint text, clipped to span range if not full fragment.
    let is_full_fragment = span.start_code_unit == 0 && span.end_code_unit == fragment.length_in_code_units;
    let mut decoration_box = fragment_absolute_rect;
    if is_full_fragment {
        recorder.recorder.draw_glyph_run(
            baseline_start,
            run_for_recording,
            Color(span.text_color),
            fragment_device_rect,
            scale,
            orientation,
            glyph_bounding_rect,
        );
    } else {
        let offsets = text_fragment::selection_offsets_for_dom_range(
            fragment,
            fragment.dom_start_offset_in_node + span.start_code_unit,
            fragment.dom_start_offset_in_node + span.end_code_unit,
        );
        let range_rect = match offsets {
            Some(offsets) => text_fragment::rect_for_selection_offsets(
                recorder.layout_arena,
                recorder.paintables,
                fragment,
                offsets,
                || text_fragment::first_available_font(recorder.layout_arena, fragment),
            ),
            None => CssPixelRect::default(),
        };
        let span_rect = converter.rounded_device_rect(range_rect);
        recorder.recorder.save();
        recorder.recorder.add_clip_rect_int(span_rect);
        recorder.recorder.draw_glyph_run(
            baseline_start,
            run_for_recording,
            Color(span.text_color),
            fragment_device_rect,
            scale,
            orientation,
            glyph_bounding_rect,
        );
        recorder.recorder.restore();
        decoration_box.x = range_rect.x;
        decoration_box.width = range_rect.width;
    }

    for set in decoration_sets {
        crate::painting::record::paint::text_decoration::paint_decoration_lines(
            recorder,
            block,
            span.fragment_index,
            decoration_box,
            set,
        );
    }
}

// Paints the caret when it sits in a fragment owned by `owner`; the block itself
// (owner == None) also handles blank lines and empty editable elements.
pub(crate) fn paint_cursor(recorder: &mut PaintRecorder<'_>, block: PaintableSlotId, owner: Option<PaintableSlotId>) {
    let owner_shell = owner.map_or(std::ptr::null_mut(), |owner| recorder.shell(owner));
    let facts = recorder.paint_host.cursor_facts(recorder.shell(block), owner_shell);
    if !facts.paints {
        return;
    }
    let color = Color(facts.color);
    if color.alpha() == 0 {
        return;
    }
    let converter = recorder.converter;
    recorder
        .recorder
        .fill_rect(converter.rounded_device_rect(CssPixelRect::from(facts.rect)), color);
}
