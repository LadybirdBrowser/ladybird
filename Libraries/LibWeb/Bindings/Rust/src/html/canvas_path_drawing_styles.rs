/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvaspathdrawingstyles

use super::canvas_rendering_context2d::{CanvasLineCap, CanvasLineJoin};
use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, RenderingContext2D};
use crate::canvas_2d::float;

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-linewidth
    pub(crate) fn line_width(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // On getting, it must return the current value.
        Ok(f64::from(self.drawing_state().line_width))
    }

    pub(crate) fn set_line_width(&self, cx: &mut CallContext, line_width: f64) -> WebIdlResult<()> {
        // On setting, zero, negative, infinite, and NaN values must be ignored, leaving the value unchanged;
        let line_width = float(line_width);
        if line_width <= 0.0 || !line_width.is_finite() {
            return Ok(());
        }
        // other values must change the current value to the new value.
        self.drawing_state_mut(cx.vm()).line_width = line_width;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-linecap
    pub(crate) fn line_cap(&self, _: &mut CallContext) -> WebIdlResult<CanvasLineCap> {
        Ok(self.drawing_state().line_cap)
    }

    pub(crate) fn set_line_cap(&self, cx: &mut CallContext, line_cap: CanvasLineCap) -> WebIdlResult<()> {
        self.drawing_state_mut(cx.vm()).line_cap = line_cap;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-linejoin
    pub(crate) fn line_join(&self, _: &mut CallContext) -> WebIdlResult<CanvasLineJoin> {
        Ok(self.drawing_state().line_join)
    }

    pub(crate) fn set_line_join(&self, cx: &mut CallContext, line_join: CanvasLineJoin) -> WebIdlResult<()> {
        self.drawing_state_mut(cx.vm()).line_join = line_join;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-miterlimit
    pub(crate) fn miter_limit(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(f64::from(self.drawing_state().miter_limit))
    }

    pub(crate) fn set_miter_limit(&self, cx: &mut CallContext, miter_limit: f64) -> WebIdlResult<()> {
        // On setting, zero, negative, infinite, and NaN values must be ignored, leaving the value unchanged;
        let miter_limit = float(miter_limit);
        if miter_limit <= 0.0 || !miter_limit.is_finite() {
            return Ok(());
        }
        // other values must change the current value to the new value.
        self.drawing_state_mut(cx.vm()).miter_limit = miter_limit;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-setlinedash
    pub(crate) fn set_line_dash(&self, cx: &mut CallContext, mut segments: Vec<f64>) -> WebIdlResult<()> {
        // 1. If any value in segments is not finite (e.g. an Infinity or a NaN value), or if any value is negative
        //    (less than zero), then return (without throwing an exception; user agents could show a message on a
        //    developer console, though, as that would be helpful for debugging).
        if segments.iter().any(|segment| !segment.is_finite() || *segment < 0.0) {
            return Ok(());
        }

        // 2. If the number of elements in segments is odd, then let segments be the concatenation of two copies of
        //    segments.
        if segments.len() % 2 == 1 {
            segments.extend_from_within(..);
        }

        // 3. Set the object's dash list to segments.
        self.drawing_state_mut(cx.vm()).dash_list = segments;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-getlinedash
    pub(crate) fn get_line_dash(&self, _: &mut CallContext) -> WebIdlResult<Vec<f64>> {
        // When the getLineDash() method is invoked, it must return a sequence whose values are the values of the
        // object's dash list, in the same order.
        Ok(self.drawing_state().dash_list.clone())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-linedashoffset
    pub(crate) fn line_dash_offset(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(f64::from(self.drawing_state().line_dash_offset))
    }

    pub(crate) fn set_line_dash_offset(&self, cx: &mut CallContext, line_dash_offset: f64) -> WebIdlResult<()> {
        // On setting, infinite and NaN values must be ignored, leaving the value unchanged;
        let line_dash_offset = float(line_dash_offset);
        if !line_dash_offset.is_finite() {
            return Ok(());
        }
        // other values must change the current value to the new value.
        self.drawing_state_mut(cx.vm()).line_dash_offset = line_dash_offset;
        Ok(())
    }
}
