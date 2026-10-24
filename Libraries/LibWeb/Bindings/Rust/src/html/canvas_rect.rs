/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvasrect

use libgfx_rust::path::Path;
use libgfx_rust::{FloatPoint, FloatRect, WindingRule};

use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, RenderingContext2D};
use crate::canvas_2d::float;

/// The outline of a rectangle, as a closed subpath that goes around it clockwise from its top left corner.
fn rect_path(x: f32, y: f32, width: f32, height: f32) -> Path {
    let mut path = Path::new();
    path.move_to(FloatPoint { x, y });
    path.line_to(FloatPoint { x: x + width, y });
    path.line_to(FloatPoint {
        x: x + width,
        y: y + height,
    });
    path.line_to(FloatPoint { x, y: y + height });
    path.line_to(FloatPoint { x, y });
    path
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
        let (x, y, w, h) = (float(x), float(y), float(w), float(h));

        // 1. If any of the arguments are infinite or NaN, then return.
        if !x.is_finite() || !y.is_finite() || !w.is_finite() || !h.is_finite() {
            return Ok(());
        }

        // 2. Take the result of tracing the path described below, using the CanvasPathDrawingStyles interface's line
        //    styles, and fill it with this's stroke style.

        // If both w and h are zero, the path has a single subpath with just one point (x, y), and no lines, and this
        // method thus has no effect (the trace a path algorithm returns an empty path in that case).

        // If just one of either w or h is zero, then the path has a single subpath consisting of two points, with
        // coordinates (x, y) and (x+w, y+h), in that order, connected by a single straight line.

        // Otherwise, the path has a single subpath consisting of four points, with coordinates (x, y), (x+w, y),
        // (x+w, y+h), and (x, y+h), connected to each other in that order by straight lines.

        // INTEROP: The path is the closed outline of the rectangle in every case. Tracing it prunes zero-length
        //          lines, so a rectangle with just one of w and h zero strokes as a closed line, with joins and no
        //          caps, and a rectangle with both zero strokes nothing.
        let mut path = rect_path(x, y, w, h);
        path.close();
        self.stroke_internal(&path);
        Ok(())
    }
}
