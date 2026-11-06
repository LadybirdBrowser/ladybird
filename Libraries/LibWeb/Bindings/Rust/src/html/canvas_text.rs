/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvastext

use core::ptr::NonNull;
use libjs_rust::api::Vm;

use ak::Utf16String;
use libcompositing_rust::display_list::commands::DisplayListGlyph;
use libgfx_rust::path::Path;
use libgfx_rust::text_layout::glyph_run_ink_bounding_box;
use libgfx_rust::{CompositingAndBlendingOperator, FloatPoint};

use super::text_metrics::{TextMeasurements, TextMetrics};
use crate::bindings::interface::Ref;
use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, ContextState, RenderingContext2D, paint_of};
use crate::canvas_2d::float;
use crate::canvas_2d::host::LentFont;
use crate::canvas_2d::text::{
    baseline_offset, horizontal_alignment_offset, path_of_runs, replace_ascii_whitespace, shape_text, text_transform,
};
use crate::html::canvas_rendering_context2d::CanvasTextBaseline;

impl ContextState {
    /// The outlines of the glyphs of the text, drawn at the point.
    fn text_path(&self, vm: &Vm, text: &[u16], x: f32, y: f32, max_width: Option<f64>) -> Path {
        let font = self.current_font(vm);
        let letter_spacing = self.resolved_letter_spacing();
        let word_spacing = self.resolved_word_spacing();
        let runs = shape_text(&font.cascade, text, FloatPoint { x, y }, letter_spacing, word_spacing);
        let path = path_of_runs(&runs);
        let text_width: f32 = runs.iter().map(|run| run.width).sum();
        let is_rtl = self.direction_is_rtl();
        let transform = text_transform(
            &self.drawing_state(),
            is_rtl,
            &font.cascade.first(),
            x,
            text_width,
            max_width,
        );
        path.copy_transformed(transform.values)
    }
}

/// Whether the arguments of fillText() or strokeText() let it draw.
/// INTEROP: A maxWidth of zero or less draws nothing at all, rather than a draw of no text, which an operator such as
///          copy would composite over the whole canvas.
fn arguments_let_text_draw(x: f32, y: f32, max_width: Option<f64>) -> bool {
    x.is_finite() && y.is_finite() && max_width.is_none_or(|max_width| max_width.is_finite() && max_width > 0.0)
}

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-filltext
    pub(crate) fn fill_text(
        &self,
        cx: &mut CallContext,
        text: Utf16String,
        x: f64,
        y: f64,
        max_width: Option<f64>,
    ) -> WebIdlResult<()> {
        let (x, y) = (float(x), float(y));
        if !arguments_let_text_draw(x, y, max_width) {
            return Ok(());
        }
        if !self.recorder_is_ready() {
            return Ok(());
        }
        {
            let state = self.drawing_state();
            if !state.fill_style.is_visible()
                && state.current_compositing_and_blending_operator == CompositingAndBlendingOperator::SourceOver
            {
                return Ok(());
            }
        }

        // https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm
        // 2. Replace all ASCII whitespace in text with U+0020 SPACE characters.
        let text = replace_ascii_whitespace(&text.to_utf16());

        let font = self.current_font(cx.vm());
        let letter_spacing = self.resolved_letter_spacing();
        let word_spacing = self.resolved_word_spacing();
        let runs = shape_text(
            &font.cascade,
            &text,
            FloatPoint { x: 0.0, y: 0.0 },
            letter_spacing,
            word_spacing,
        );

        // NB: The glyphs are not filled as outlines, since the glyphs of a color font can have none. All runs are a
        //     single draw, so that the fallback fonts of the text share one shadow and one composite.
        let lent_fonts: Vec<LentFont> = runs
            .iter()
            .map(|run| LentFont(NonNull::new(run.font.as_raw().cast_mut()).expect("a font has a font")))
            .collect();
        let mut glyphs = Vec::new();
        let mut glyph_runs = Vec::with_capacity(runs.len());
        let mut text_width = 0.0;
        for (run, lent_font) in runs.iter().zip(&lent_fonts) {
            text_width += run.width;
            if run.font.is_invisible() {
                continue;
            }
            let first_glyph = glyphs.len();
            glyphs.extend(
                run.glyphs
                    .placed()
                    .filter(|placed| placed.glyph.should_paint())
                    .map(DisplayListGlyph::from),
            );
            if glyphs.len() > first_glyph {
                glyph_runs.push((lent_font, glyphs.len() - first_glyph));
            }
        }
        let first_font = font.cascade.first();
        let state = self.drawing_state();
        let transform = text_transform(&state, self.direction_is_rtl(), &first_font, x, text_width, max_width);
        let translation = transform.map_point(FloatPoint { x, y });
        let horizontal_scale = transform.a();
        self.record(|recorder| {
            let paint = paint_of(recorder, &state.fill_style, self.context_attributes.color_space);
            recorder.draw_glyph_runs(
                &glyph_runs,
                &glyphs,
                translation,
                horizontal_scale,
                &paint,
                &ContextState::compositing(&state),
            );
        });
        drop(state);
        self.did_draw();
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-stroketext
    pub(crate) fn stroke_text(
        &self,
        cx: &mut CallContext,
        text: Utf16String,
        x: f64,
        y: f64,
        max_width: Option<f64>,
    ) -> WebIdlResult<()> {
        let (x, y) = (float(x), float(y));
        if !arguments_let_text_draw(x, y, max_width) {
            return Ok(());
        }
        // https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm
        // 2. Replace all ASCII whitespace in text with U+0020 SPACE characters.
        let text = replace_ascii_whitespace(&text.to_utf16());
        self.stroke_internal(&self.text_path(cx.vm(), &text, x, y, max_width));
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-measuretext
    pub(crate) fn measure_text(&self, cx: &mut CallContext, text: Utf16String) -> WebIdlResult<Ref<TextMetrics>> {
        // The measureText(text) method steps are to run the text preparation algorithm, passing it text and the object
        // implementing the CanvasText interface, and then using the returned inline box return a new TextMetrics
        // object with members behaving as described in the following list:

        // https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm
        // 1. If maxWidth was provided but is less than or equal to zero or equal to NaN, then return an empty array.
        // 2. Replace all ASCII whitespace in text with U+0020 SPACE characters.
        let replaced_text = replace_ascii_whitespace(&text.to_utf16());

        // 3. Let font be the current font of target, as given by that object's font attribute.
        let font = self.current_font(cx.vm());
        let letter_spacing = self.resolved_letter_spacing();
        let word_spacing = self.resolved_word_spacing();
        let runs = shape_text(
            &font.cascade,
            &replaced_text,
            FloatPoint { x: 0.0, y: 0.0 },
            letter_spacing,
            word_spacing,
        );

        // FIXME: 4. Let language be the target's language.
        // FIXME: 5. If language is "inherit": ...
        // FIXME: 6. If language is the empty string, then set language to explicitly unknown.
        // 7. Apply the appropriate step from the following list to determine the value of direction: ...
        let is_rtl = self.direction_is_rtl();

        // 8. Form a hypothetical infinitely-wide CSS line box containing a single inline box containing the text text,
        //    with the CSS content language set to language, and with its CSS properties set as follows: ...
        // FIXME: Actually use a LineBox here instead of, you know, using the default font and measuring its size
        //        (which is not the spec at all).
        let mut width = 0.0_f32;
        for run in &runs {
            width += run.width;
        }

        // 9. If maxWidth was provided and the hypothetical width of the inline box in the hypothetical line box is
        //    greater than maxWidth CSS pixels, then change font to have a more condensed font (if one is available or
        //    if a reasonably readable one can be synthesized by applying a horizontal scale factor to the font) or a
        //    smaller font, and return to the previous step.
        // 10. The anchor point is a point on the inline box, and the physical alignment is one of the values left,
        //     right, and center. These variables are determined by the textAlign and textBaseline values.
        // NB: The horizontal position of the anchor point places the inline box below, and its vertical position is
        //     the baseline offset below.
        let alignment_offset = horizontal_alignment_offset(self.drawing_state().text_align, is_rtl, width);
        // 11. Let result be an array constructed by iterating over each glyph in the inline box from left to right (if
        //     any), adding to the array, for each glyph, the shape of the glyph as it is in the inline box, positioned
        //     on a coordinate space using CSS pixels with its origin at the anchor point.
        // 12. Return result, physical alignment, and the inline box.
        // NB: The inline box is as wide as the text, with the anchor point at the origin.
        let (box_left, box_right) = (alignment_offset, alignment_offset + width);

        // FIXME: Use the font that was used to create the glyphs of the runs.
        let first_font = font.cascade.first();
        let ascent = first_font.facts().ascent;
        let descent = first_font.facts().descent;
        let hanging_baseline = ascent * 0.8;
        let text_baseline: CanvasTextBaseline = self.drawing_state().text_baseline;
        let baseline_offset = baseline_offset(text_baseline, &first_font);

        // NB: The bounding rectangle of the given text is that of the ink of its glyphs, which is relative to the
        //     alphabetic baseline at the start of the inline box. Text without ink has that of the inline box.
        let ink_bounds = runs
            .iter()
            .filter(|run| !run.font.is_invisible())
            .map(|run| glyph_run_ink_bounding_box(&run.font, &run.glyphs, 1.0))
            .filter(|[_, _, width, height]| *width > 0.0 && *height > 0.0)
            .map(|[x, y, width, height]| (x, x + width, y, y + height))
            .reduce(|a, b| (a.0.min(b.0), a.1.max(b.1), a.2.min(b.2), a.3.max(b.3)));
        let (text_left, text_right, text_top, text_bottom) = match ink_bounds {
            Some((left, right, top, bottom)) => (alignment_offset + left, alignment_offset + right, top, bottom),
            None => (box_left, box_right, -ascent, descent),
        };

        let measurements = TextMeasurements {
            // width attribute: The width of that inline box, in CSS pixels. (The text's advance width.)
            width: f64::from(width),
            // actualBoundingBoxLeft attribute: The distance parallel to the baseline from the alignment point given by
            // the textAlign attribute to the left side of the bounding rectangle of the given text, in CSS pixels;
            // positive numbers indicating a distance going left from the given alignment point.
            actual_bounding_box_left: f64::from(-text_left),
            // actualBoundingBoxRight attribute: The distance parallel to the baseline from the alignment point given
            // by the textAlign attribute to the right side of the bounding rectangle of the given text, in CSS pixels;
            // positive numbers indicating a distance going right from the given alignment point.
            actual_bounding_box_right: f64::from(text_right),
            // fontBoundingBoxAscent attribute: The distance from the horizontal line indicated by the textBaseline
            // attribute to the ascent metric of the first available font, in CSS pixels; positive numbers indicating
            // a distance going up from the given baseline.
            font_bounding_box_ascent: f64::from(ascent - baseline_offset),
            // fontBoundingBoxDescent attribute: The distance from the horizontal line indicated by the textBaseline
            // attribute to the descent metric of the first available font, in CSS pixels; positive numbers indicating
            // a distance going down from the given baseline.
            font_bounding_box_descent: f64::from(descent + baseline_offset),
            // actualBoundingBoxAscent attribute: The distance from the horizontal line indicated by the textBaseline
            // attribute to the top of the bounding rectangle of the given text, in CSS pixels; positive numbers
            // indicating a distance going up from the given baseline.
            actual_bounding_box_ascent: f64::from(-text_top - baseline_offset),
            // actualBoundingBoxDescent attribute: The distance from the horizontal line indicated by the textBaseline
            // attribute to the bottom of the bounding rectangle of the given text, in CSS pixels; positive numbers
            // indicating a distance going down from the given baseline.
            actual_bounding_box_descent: f64::from(text_bottom + baseline_offset),
            // emHeightAscent attribute: The distance from the horizontal line indicated by the textBaseline attribute
            // to the highest top of the em squares in the inline box, in CSS pixels; positive numbers indicating that
            // the given baseline is below the top of that em square (so this value will usually be positive). Zero if
            // the given baseline is the top of that em square; half the font size if the given baseline is the middle
            // of that em square.
            em_height_ascent: f64::from(ascent - baseline_offset),
            // emHeightDescent attribute: The distance from the horizontal line indicated by the textBaseline attribute
            // to the lowest bottom of the em squares in the inline box, in CSS pixels; positive numbers indicating
            // that the given baseline is above the bottom of that em square. (Zero if the given baseline is the bottom
            // of that em square.)
            em_height_descent: f64::from(descent + baseline_offset),
            // hangingBaseline attribute: The distance from the horizontal line indicated by the textBaseline attribute
            // to the hanging baseline of the inline box, in CSS pixels; positive numbers indicating that the given
            // baseline is below the hanging baseline. (Zero if the given baseline is the hanging baseline.)
            hanging_baseline: f64::from(hanging_baseline - baseline_offset),
            // alphabeticBaseline attribute: The distance from the horizontal line indicated by the textBaseline
            // attribute to the alphabetic baseline of the inline box, in CSS pixels; positive numbers indicating that
            // the given baseline is below the alphabetic baseline. (Zero if the given baseline is the alphabetic
            // baseline.)
            alphabetic_baseline: f64::from(-baseline_offset),
            // ideographicBaseline attribute: The distance from the horizontal line indicated by the textBaseline
            // attribute to the ideographic-under baseline of the inline box, in CSS pixels; positive numbers
            // indicating that the given baseline is below the ideographic-under baseline. (Zero if the given baseline
            // is the ideographic-under baseline.)
            ideographic_baseline: f64::from(-descent - baseline_offset),
        };

        Ok(TextMetrics::create(cx.vm(), measurements))
    }
}
