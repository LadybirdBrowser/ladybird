/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvascompositing

use ak::Utf16String;

use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, RenderingContext2D};
use crate::canvas_2d::drawing_state::{composite_operation_name, composite_operation_named};
use crate::canvas_2d::float;

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalalpha
    pub(crate) fn global_alpha(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(f64::from(self.drawing_state().global_alpha))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalalpha
    pub(crate) fn set_global_alpha(&self, cx: &mut CallContext, alpha: f64) -> WebIdlResult<()> {
        let alpha = float(alpha);
        // 1. If the given value is either infinite, NaN, or not in the range 0.0 to 1.0, then return.
        if !alpha.is_finite() || !(0.0..=1.0).contains(&alpha) {
            return Ok(());
        }
        // 2. Otherwise, set this's global alpha to the given value.
        self.drawing_state_mut(cx.vm()).global_alpha = alpha;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalcompositeoperation
    pub(crate) fn global_composite_operation(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        let operator = self.drawing_state().current_compositing_and_blending_operator;
        Ok(Utf16String::from_ascii(composite_operation_name(operator).as_bytes()))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalcompositeoperation
    pub(crate) fn set_global_composite_operation(
        &self,
        cx: &mut CallContext,
        global_composite_operation: Utf16String,
    ) -> WebIdlResult<()> {
        // 1. If the given value is not identical to any of the values that the <blend-mode> or the <composite-mode>
        //    properties are defined to take, then return.
        // 2. Otherwise, set this's current compositing and blending operator to the given value.
        if let Some(operator) = composite_operation_named(&global_composite_operation) {
            self.drawing_state_mut(cx.vm())
                .current_compositing_and_blending_operator = operator;
        }
        Ok(())
    }
}
