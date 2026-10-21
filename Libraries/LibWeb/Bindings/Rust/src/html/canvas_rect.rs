/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvasrect

use libgfx_rust::path::{OwnedPath, PathBuilder};
use libgfx_rust::{FloatRect, WindingRule};

use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, RenderingContext2D};
use crate::canvas_2d::float;

/// The outline of a rectangle, as a closed subpath that goes around it clockwise from its top left corner.
fn rect_path(x: f32, y: f32, width: f32, height: f32) -> OwnedPath {
    let mut path = PathBuilder::new();
    path.move_to(x, y);
    path.line_to(x + width, y);
    path.line_to(x + width, y + height);
    path.line_to(x, y + height);
    path.line_to(x, y);
    path.build()
}

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-clearrect
    pub(crate) fn clear_rect(&self, _: &mut CallContext, x: f64, y: f64, w: f64, h: f64) -> WebIdlResult<()> {
        let (x, y, w, h) = (float(x), float(y), float(w), float(h));

        // 1. If any of the arguments are infinite or NaN, then return.
        if !x.is_finite() || !y.is_finite() || !w.is_finite() || !h.is_finite() {
            return Ok(());
        }

        let color = self.clear_color();
        if self.record_if_ready(|recorder| recorder.clear_rect(FloatRect::new(x, y, w, h), color)) {
            self.did_draw();
        }
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-fillrect
    pub(crate) fn fill_rect(&self, _: &mut CallContext, x: f64, y: f64, w: f64, h: f64) -> WebIdlResult<()> {
        self.fill_internal(&rect_path(float(x), float(y), float(w), float(h)), WindingRule::EvenOdd);
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-strokerect
    pub(crate) fn stroke_rect(&self, _: &mut CallContext, x: f64, y: f64, w: f64, h: f64) -> WebIdlResult<()> {
        self.stroke_internal(&rect_path(float(x), float(y), float(w), float(h)));
        Ok(())
    }
}
