/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvasfillstrokestyles

use ak::Utf16String;

use super::canvas_draw_image::CanvasImageSource;
use super::canvas_gradient::CanvasGradient;
use super::canvas_pattern::CanvasPattern;
use crate::bindings::interface::Ref;
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, RenderingContext2D};
use crate::canvas_2d::drawing_state::{FillOrStrokeStyle, serialize_color};

/// The union (DOMString or CanvasGradient or CanvasPattern) of HTML/Canvas/CanvasFillStrokeStyles.idl, the value of
/// fillStyle and strokeStyle, whose members the bindings convert.
pub(crate) enum DOMStringOrCanvasGradientOrCanvasPattern {
    DOMString(Utf16String),
    CanvasGradient(Ref<CanvasGradient>),
    CanvasPattern(Ref<CanvasPattern>),
}

impl From<FillOrStrokeStyle> for DOMStringOrCanvasGradientOrCanvasPattern {
    fn from(style: FillOrStrokeStyle) -> Self {
        match style {
            FillOrStrokeStyle::Color(color) => Self::DOMString(serialize_color(color.srgb)),
            FillOrStrokeStyle::Gradient(gradient) => Self::CanvasGradient(gradient),
            FillOrStrokeStyle::Pattern(pattern) => Self::CanvasPattern(pattern),
        }
    }
}

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-strokestyle
    pub(crate) fn stroke_style(&self, _: &mut CallContext) -> WebIdlResult<DOMStringOrCanvasGradientOrCanvasPattern> {
        Ok(self.drawing_state().stroke_style.into())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-strokestyle
    pub(crate) fn set_stroke_style(
        &self,
        _: &mut CallContext,
        style: DOMStringOrCanvasGradientOrCanvasPattern,
    ) -> WebIdlResult<()> {
        let style = match style {
            // 1. If the given value is a string:
            DOMStringOrCanvasGradientOrCanvasPattern::DOMString(string) => {
                // 1. Let context be this's canvas attribute's value, if that is an element; otherwise null.

                // 2. Let parsedValue be the result of parsing the given value with context if non-null.
                // 3. If parsedValue is failure, then return.
                let Some(parsed_value) = self.parse_a_css_color_value(&string) else {
                    return Ok(());
                };

                // 4. Set this's stroke style to parsedValue.
                self.drawing_state_mut().stroke_style = FillOrStrokeStyle::Color(parsed_value);

                // 5. Return.
                return Ok(());
            }
            DOMStringOrCanvasGradientOrCanvasPattern::CanvasGradient(gradient) => FillOrStrokeStyle::Gradient(gradient),
            DOMStringOrCanvasGradientOrCanvasPattern::CanvasPattern(pattern) => {
                // 2. If the given value is a CanvasPattern object that is marked as not origin-clean, then set this's
                //    origin-clean flag to false.
                if pattern.is_not_origin_clean() {
                    self.set_origin_clean(false);
                }
                FillOrStrokeStyle::Pattern(pattern)
            }
        };

        // 3. Set this's stroke style to the given value.
        self.drawing_state_mut().stroke_style = style;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-fillstyle
    pub(crate) fn fill_style(&self, _: &mut CallContext) -> WebIdlResult<DOMStringOrCanvasGradientOrCanvasPattern> {
        Ok(self.drawing_state().fill_style.into())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-fillstyle
    pub(crate) fn set_fill_style(
        &self,
        _: &mut CallContext,
        style: DOMStringOrCanvasGradientOrCanvasPattern,
    ) -> WebIdlResult<()> {
        let style = match style {
            // 1. If the given value is a string:
            DOMStringOrCanvasGradientOrCanvasPattern::DOMString(string) => {
                // 1. Let context be this's canvas attribute's value, if that is an element; otherwise null.

                // 2. Let parsedValue be the result of parsing the given value with context if non-null.
                // 3. If parsedValue is failure, then return.
                let Some(parsed_value) = self.parse_a_css_color_value(&string) else {
                    return Ok(());
                };

                // 4. Set this's fill style to parsedValue.
                self.drawing_state_mut().fill_style = FillOrStrokeStyle::Color(parsed_value);

                // 5. Return.
                return Ok(());
            }
            DOMStringOrCanvasGradientOrCanvasPattern::CanvasGradient(gradient) => FillOrStrokeStyle::Gradient(gradient),
            DOMStringOrCanvasGradientOrCanvasPattern::CanvasPattern(pattern) => {
                // 2. If the given value is a CanvasPattern object that is marked as not origin-clean, then set this's
                //    origin-clean flag to false.
                if pattern.is_not_origin_clean() {
                    self.set_origin_clean(false);
                }
                FillOrStrokeStyle::Pattern(pattern)
            }
        };

        // 3. Set this's fill style to the given value.
        self.drawing_state_mut().fill_style = style;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createlineargradient
    pub(crate) fn create_linear_gradient(
        &self,
        cx: &mut CallContext,
        x0: f64,
        y0: f64,
        x1: f64,
        y1: f64,
    ) -> WebIdlResult<Ref<CanvasGradient>> {
        Ok(CanvasGradient::create_linear(cx.vm(), x0, y0, x1, y1))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createradialgradient
    #[allow(clippy::too_many_arguments, reason = "the arguments of createRadialGradient()")]
    pub(crate) fn create_radial_gradient(
        &self,
        cx: &mut CallContext,
        x0: f64,
        y0: f64,
        r0: f64,
        x1: f64,
        y1: f64,
        r1: f64,
    ) -> WebIdlResult<Ref<CanvasGradient>> {
        // If either of r0 or r1 are negative, then an "IndexSizeError" DOMException must be thrown.
        if r0 < 0.0 {
            return Err(Exception::index_size_error(Utf16String::from_ascii(
                b"The r0 passed is less than 0",
            )));
        }
        if r1 < 0.0 {
            return Err(Exception::index_size_error(Utf16String::from_ascii(
                b"The r1 passed is less than 0",
            )));
        }
        Ok(CanvasGradient::create_radial(cx.vm(), x0, y0, r0, x1, y1, r1))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createconicgradient
    pub(crate) fn create_conic_gradient(
        &self,
        cx: &mut CallContext,
        start_angle: f64,
        x: f64,
        y: f64,
    ) -> WebIdlResult<Ref<CanvasGradient>> {
        Ok(CanvasGradient::create_conic(cx.vm(), start_angle, x, y))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createpattern
    pub(crate) fn create_pattern(
        &self,
        cx: &mut CallContext,
        image: CanvasImageSource,
        repetition: Utf16String,
    ) -> WebIdlResult<Option<Ref<CanvasPattern>>> {
        CanvasPattern::create_for_image(cx.vm(), &image, &repetition, self.context_attributes.color_space.into())
    }
}
