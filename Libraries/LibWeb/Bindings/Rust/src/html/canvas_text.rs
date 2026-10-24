/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvastext

use core::ptr::NonNull;

use ak::Utf16String;
use libcompositing_rust::display_list::commands::DisplayListGlyph;
use libgfx_rust::path::{OwnedPath, PathBuilder};
use libgfx_rust::{CompositingAndBlendingOperator, FloatPoint, WindingRule};

use super::text_metrics::{TextMeasurements, TextMetrics};
use crate::bindings::interface::Ref;
use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, ContextState, RenderingContext2D, paint_of};
use crate::canvas_2d::float;
use crate::canvas_2d::host::LentFont;
use crate::canvas_2d::text::{baseline_offset, path_of_runs, replace_ascii_whitespace, shape_text, text_transform};
use crate::html::canvas_rendering_context2d::CanvasTextBaseline;

impl ContextState {
    /// The outlines of the glyphs of the text, drawn at the point.
    fn text_path(&self, text: &[u16], x: f32, y: f32, max_width: Option<f64>) -> OwnedPath {
        if max_width.is_some_and(|max_width| max_width <= 0.0) {
            return PathBuilder::new().build();
        }

        let font = self.current_font();
        let letter_spacing = self.resolved_letter_spacing();
        let runs = shape_text(&font.cascade, text, FloatPoint { x, y }, letter_spacing);
        let path = path_of_runs(&runs);
        let text_width: f32 = runs.iter().map(|run| run.width).sum();
        let transform = text_transform(&self.drawing_state(), &font.cascade.first(), x, text_width, max_width);
        path.copy_transformed(transform.values)
    }
}

/// Whether the arguments of fillText() or strokeText() let it draw.
fn are_finite(x: f32, y: f32, max_width: Option<f64>) -> bool {
    x.is_finite() && y.is_finite() && max_width.is_none_or(f64::is_finite)
}

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-filltext
    pub(crate) fn fill_text(
        &self,
        _: &mut CallContext,
        text: Utf16String,
        x: f64,
        y: f64,
        max_width: Option<f64>,
    ) -> WebIdlResult<()> {
        let (x, y) = (float(x), float(y));
        if !are_finite(x, y, max_width) {
            return Ok(());
        }
        // https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm
        // 2. Replace all ASCII whitespace in text with U+0020 SPACE characters.
        let text = replace_ascii_whitespace(&text.to_utf16());

        // Complex text effects continue to use the combined outline so they are applied once to the entire text,
        // including runs that use different fallback fonts.
        let has_shadow = {
            let state = self.drawing_state();
            state.shadow_blur != 0.0 || state.shadow_offset_x != 0.0 || state.shadow_offset_y != 0.0
        };
        if max_width.is_some() || has_shadow {
            self.fill_internal(&self.text_path(&text, x, y, max_width), WindingRule::Nonzero);
            return Ok(());
        }

        let font = self.current_font();
        let letter_spacing = self.resolved_letter_spacing();
        let runs = shape_text(&font.cascade, &text, FloatPoint { x: 0.0, y: 0.0 }, letter_spacing);
        let [run] = runs.as_slice() else {
            self.fill_internal(&self.text_path(&text, x, y, max_width), WindingRule::Nonzero);
            return Ok(());
        };
        if run.font.is_invisible() {
            self.fill_internal(&self.text_path(&text, x, y, max_width), WindingRule::Nonzero);
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
        let glyphs: Vec<DisplayListGlyph> = run
            .glyphs
            .iter()
            .filter(|glyph| glyph.should_paint)
            .map(|glyph| DisplayListGlyph {
                position: FloatPoint { x: glyph.x, y: glyph.y },
                glyph_id: glyph.glyph_id,
            })
            .collect();
        let first_font = font.cascade.first();
        let state = self.drawing_state();
        let translation = text_transform(&state, &first_font, x, run.width, None).map_point(FloatPoint { x, y });
        let lent_font = LentFont(NonNull::new(run.font.as_raw().cast_mut()).expect("a font has a font"));
        self.record(|recorder| {
            let paint = paint_of(recorder, &state.fill_style, self.context_attributes.color_space);
            recorder.draw_glyph_run(
                &lent_font,
                &glyphs,
                translation,
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
        _: &mut CallContext,
        text: Utf16String,
        x: f64,
        y: f64,
        max_width: Option<f64>,
    ) -> WebIdlResult<()> {
        let (x, y) = (float(x), float(y));
        if !are_finite(x, y, max_width) {
            return Ok(());
        }
        // https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm
        // 2. Replace all ASCII whitespace in text with U+0020 SPACE characters.
        let text = replace_ascii_whitespace(&text.to_utf16());
        self.stroke_internal(&self.text_path(&text, x, y, max_width));
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
        let font = self.current_font();
        let letter_spacing = self.resolved_letter_spacing();
        let runs = shape_text(
            &font.cascade,
            &replaced_text,
            FloatPoint { x: 0.0, y: 0.0 },
            letter_spacing,
        );

        // FIXME: 4. Let language be the target's language.
        // FIXME: 5. If language is "inherit": ...
        // FIXME: 6. If language is the empty string, then set language to explicitly unknown.
        // FIXME: 7. Apply the appropriate step from the following list to determine the value of direction: ...

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
        // FIXME: 10. The anchor point is a point on the inline box, and the physical alignment is one of the values
        //            left, right, and center. These variables are determined by the textAlign and textBaseline values.
        // 11. Let result be an array constructed by iterating over each glyph in the inline box from left to right (if
        //     any), adding to the array, for each glyph, the shape of the glyph as it is in the inline box, positioned
        //     on a coordinate space using CSS pixels with its origin at the anchor point.
        // 12. Return result, physical alignment, and the inline box.
        // NB: The inline box is at the origin, as wide as the text.
        let (box_left, box_right) = (0.0_f32, width);

        // FIXME: Use the font that was used to create the glyphs of the runs.
        let first_font = font.cascade.first();
        let ascent = first_font.facts().ascent;
        let descent = first_font.facts().descent;
        let hanging_baseline = ascent * 0.8;
        let text_baseline: CanvasTextBaseline = self.drawing_state().text_baseline;
        let baseline_offset = baseline_offset(text_baseline, &first_font);

        let measurements = TextMeasurements {
            // width attribute: The width of that inline box, in CSS pixels. (The text's advance width.)
            width: f64::from(width),
            // actualBoundingBoxLeft attribute: The distance parallel to the baseline from the alignment point given by
            // the textAlign attribute to the left side of the bounding rectangle of the given text, in CSS pixels;
            // positive numbers indicating a distance going left from the given alignment point.
            actual_bounding_box_left: f64::from(-box_left),
            // actualBoundingBoxRight attribute: The distance parallel to the baseline from the alignment point given
            // by the textAlign attribute to the right side of the bounding rectangle of the given text, in CSS pixels;
            // positive numbers indicating a distance going right from the given alignment point.
            actual_bounding_box_right: f64::from(box_right),
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
            actual_bounding_box_ascent: f64::from(ascent - baseline_offset),
            // actualBoundingBoxDescent attribute: The distance from the horizontal line indicated by the textBaseline
            // attribute to the bottom of the bounding rectangle of the given text, in CSS pixels; positive numbers
            // indicating a distance going down from the given baseline.
            actual_bounding_box_descent: f64::from(descent + baseline_offset),
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
