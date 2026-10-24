/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvasdrawpath

use libgfx_rust::path::{Path, StrokeStyle};
use libgfx_rust::{AffineTransform, FloatPoint, WindingRule};

use super::path2d::Path2D;
use crate::bindings::interface::Ref;
use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, RenderingContext2D};
use crate::canvas_2d::drawing_state::DrawingState;
use crate::canvas_2d::float;

/// https://html.spec.whatwg.org/multipage/canvas.html#canvasfillrule
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CanvasFillRule {
    Nonzero,
    Evenodd,
}

impl From<CanvasFillRule> for WindingRule {
    fn from(fill_rule: CanvasFillRule) -> Self {
        match fill_rule {
            CanvasFillRule::Nonzero => Self::Nonzero,
            CanvasFillRule::Evenodd => Self::EvenOdd,
        }
    }
}

/// The path of a Path2D, which nothing changes while a context draws with it, as no script runs meanwhile.
fn path_of(path: &Ref<Path2D>) -> &Path {
    // SAFETY: No script runs while the context uses the path.
    unsafe { path.path_unguarded() }
}

/// Whether the point, in the coordinates that the transform maps from, is inside the path.
fn is_point_in_path_internal(
    path: &Path,
    transform: AffineTransform,
    x: f64,
    y: f64,
    fill_rule: CanvasFillRule,
) -> bool {
    // NB: A transform that is not invertible maps every path onto a line or a point, which has no inside.
    let Some(inverse_transform) = transform.inverse() else {
        return false;
    };
    let point = inverse_transform.map_point(FloatPoint {
        x: float(x),
        y: float(y),
    });
    path.contains(point.x, point.y, WindingRule::from(fill_rule))
}

/// https://html.spec.whatwg.org/multipage/canvas.html#is-point-in-stroke-steps
fn is_point_in_stroke_internal(path: &Path, state: &DrawingState, x: f64, y: f64) -> bool {
    // 1. If x or y are infinite or NaN, then return false.
    if !x.is_finite() || !y.is_finite() {
        return false;
    }

    // 2. If the point given by the x and y coordinates, when treated as coordinates in the canvas coordinate space
    //    unaffected by the current transformation, is inside the path that results from tracing the intended path for
    //    path, using the nonzero winding rule, and using context's line styles as set by its CanvasPathDrawingStyles
    //    mixin, then return true. Points on the resulting path must be considered to be inside the path.
    // 3. Return false.
    let stroke = path.stroke_to_fill(&StrokeStyle {
        thickness: state.line_width,
        cap_style: state.cap_style(),
        join_style: state.join_style(),
        miter_limit: state.miter_limit,
        dash_array: state.dash_array(),
        dash_offset: state.line_dash_offset,
    });
    is_point_in_path_internal(&stroke, state.transform, x, y, CanvasFillRule::Nonzero)
}

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-beginpath
    pub(crate) fn begin_path(&self, _: &mut CallContext) -> WebIdlResult<()> {
        self.clear_path();
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-fill
    pub(crate) fn fill(&self, _: &mut CallContext, fill_rule: CanvasFillRule) -> WebIdlResult<()> {
        let path = self.path().clone();
        self.fill_internal(&path, fill_rule.into());
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-fill
    pub(crate) fn fill_path2d(
        &self,
        _: &mut CallContext,
        path: Ref<Path2D>,
        fill_rule: CanvasFillRule,
    ) -> WebIdlResult<()> {
        self.fill_internal(path_of(&path), fill_rule.into());
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-stroke
    pub(crate) fn stroke(&self, _: &mut CallContext) -> WebIdlResult<()> {
        let path = self.path().clone();
        self.stroke_internal(&path);
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-stroke
    pub(crate) fn stroke_path2d(&self, _: &mut CallContext, path: Ref<Path2D>) -> WebIdlResult<()> {
        self.stroke_internal(path_of(&path));
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-clip
    pub(crate) fn clip(&self, _: &mut CallContext, fill_rule: CanvasFillRule) -> WebIdlResult<()> {
        let path = self.path().clone();
        self.clip_internal(&path, fill_rule.into());
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-clip
    pub(crate) fn clip_path2d(
        &self,
        _: &mut CallContext,
        path: Ref<Path2D>,
        fill_rule: CanvasFillRule,
    ) -> WebIdlResult<()> {
        self.clip_internal(path_of(&path), fill_rule.into());
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-ispointinpath
    pub(crate) fn is_point_in_path(
        &self,
        _: &mut CallContext,
        x: f64,
        y: f64,
        fill_rule: CanvasFillRule,
    ) -> WebIdlResult<bool> {
        let transform = self.drawing_state().transform;
        Ok(is_point_in_path_internal(&self.path(), transform, x, y, fill_rule))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-ispointinpath
    pub(crate) fn is_point_in_path2d(
        &self,
        _: &mut CallContext,
        path: Ref<Path2D>,
        x: f64,
        y: f64,
        fill_rule: CanvasFillRule,
    ) -> WebIdlResult<bool> {
        let transform = self.drawing_state().transform;
        Ok(is_point_in_path_internal(path_of(&path), transform, x, y, fill_rule))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-ispointinstroke
    pub(crate) fn is_point_in_stroke(&self, _: &mut CallContext, x: f64, y: f64) -> WebIdlResult<bool> {
        // The isPointInStroke(x, y) method steps are to return the result of the is point in stroke steps given this,
        // null, x, and y.
        Ok(is_point_in_stroke_internal(&self.path(), &self.drawing_state(), x, y))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-ispointinstroke-path
    pub(crate) fn is_point_in_stroke_path2d(
        &self,
        _: &mut CallContext,
        path: Ref<Path2D>,
        x: f64,
        y: f64,
    ) -> WebIdlResult<bool> {
        // The isPointInStroke(path, x, y) method steps are to return the result of the is point in stroke steps given
        // this, path, x, and y.
        Ok(is_point_in_stroke_internal(path_of(&path), &self.drawing_state(), x, y))
    }
}
