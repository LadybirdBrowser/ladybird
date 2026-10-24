/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvasshadowstyles

use ak::Utf16String;

use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, RenderingContext2D};
use crate::canvas_2d::drawing_state::serialize_color;
use crate::canvas_2d::float;

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsetx
    pub(crate) fn shadow_offset_x(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(f64::from(self.drawing_state().shadow_offset_x))
    }

    pub(crate) fn set_shadow_offset_x(&self, _: &mut CallContext, offset_x: f64) -> WebIdlResult<()> {
        let offset_x = float(offset_x);
        if offset_x.is_finite() {
            self.drawing_state_mut().shadow_offset_x = offset_x;
        }
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsety
    pub(crate) fn shadow_offset_y(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(f64::from(self.drawing_state().shadow_offset_y))
    }

    pub(crate) fn set_shadow_offset_y(&self, _: &mut CallContext, offset_y: f64) -> WebIdlResult<()> {
        let offset_y = float(offset_y);
        if offset_y.is_finite() {
            self.drawing_state_mut().shadow_offset_y = offset_y;
        }
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowblur
    pub(crate) fn shadow_blur(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(f64::from(self.drawing_state().shadow_blur))
    }

    pub(crate) fn set_shadow_blur(&self, _: &mut CallContext, blur_radius: f64) -> WebIdlResult<()> {
        // On setting, the attribute must be set to the new value, except if the value is negative, infinite or NaN,
        // in which case the new value must be ignored.
        let blur_radius = float(blur_radius);
        if blur_radius < 0.0 || !blur_radius.is_finite() {
            return Ok(());
        }
        self.drawing_state_mut().shadow_blur = blur_radius;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowcolor
    pub(crate) fn shadow_color(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(serialize_color(self.drawing_state().shadow_color.srgb))
    }

    pub(crate) fn set_shadow_color(&self, _: &mut CallContext, color: Utf16String) -> WebIdlResult<()> {
        // 1. Let context be this's canvas attribute's value, if that is an element; otherwise null.

        // 2. Let parsedValue be the result of parsing the given value with context if non-null.
        // 3. If parsedValue is failure, then return.
        let Some(parsed_value) = self.parse_a_css_color_value(&color) else {
            return Ok(());
        };

        // 4. Set this's shadow color to parsedValue.
        self.drawing_state_mut().shadow_color = parsed_value;
        Ok(())
    }
}
