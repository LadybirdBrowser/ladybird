/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The operations of the CanvasPath mixin on the current default path of a 2D context, whose steps are in
//! html/canvas_path.rs.

use libgfx_rust::path::Path;

use super::context::{CanvasKind, RenderingContext2D};
use super::float;
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::canvas_path::{
    self, UnrestrictedDoubleOrDOMPointInitOrUnrestrictedDoubleOrDOMPointInitSequence, round_rect_radii,
};

impl<K: CanvasKind> RenderingContext2D<K> {
    fn build_path<R>(&self, function: impl FnOnce(&mut Path) -> R) -> R {
        function(&mut self.path_mut())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-closepath
    pub(crate) fn close_path(&self, _: &mut CallContext) -> WebIdlResult<()> {
        self.build_path(canvas_path::close_path);
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-moveto
    pub(crate) fn move_to(&self, _: &mut CallContext, x: f64, y: f64) -> WebIdlResult<()> {
        self.build_path(|path| canvas_path::move_to(path, float(x), float(y)));
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-lineto
    pub(crate) fn line_to(&self, _: &mut CallContext, x: f64, y: f64) -> WebIdlResult<()> {
        self.build_path(|path| canvas_path::line_to(path, float(x), float(y)));
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-quadraticcurveto
    pub(crate) fn quadratic_curve_to(
        &self,
        _: &mut CallContext,
        cpx: f64,
        cpy: f64,
        x: f64,
        y: f64,
    ) -> WebIdlResult<()> {
        self.build_path(|path| canvas_path::quadratic_curve_to(path, float(cpx), float(cpy), float(x), float(y)));
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-beziercurveto
    #[allow(clippy::too_many_arguments, reason = "the arguments of bezierCurveTo()")]
    pub(crate) fn bezier_curve_to(
        &self,
        _: &mut CallContext,
        cp1x: f64,
        cp1y: f64,
        cp2x: f64,
        cp2y: f64,
        x: f64,
        y: f64,
    ) -> WebIdlResult<()> {
        self.build_path(|path| canvas_path::bezier_curve_to(path, cp1x, cp1y, cp2x, cp2y, x, y));
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-arcto
    #[allow(clippy::too_many_arguments, reason = "the arguments of arcTo()")]
    pub(crate) fn arc_to(
        &self,
        _: &mut CallContext,
        x1: f64,
        y1: f64,
        x2: f64,
        y2: f64,
        radius: f64,
    ) -> WebIdlResult<()> {
        self.build_path(|path| canvas_path::arc_to(path, x1, y1, x2, y2, radius))?;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-rect
    pub(crate) fn rect(&self, _: &mut CallContext, x: f64, y: f64, w: f64, h: f64) -> WebIdlResult<()> {
        self.build_path(|path| canvas_path::rect(path, x, y, w, h));
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-roundrect
    #[allow(clippy::too_many_arguments, reason = "the arguments of roundRect()")]
    pub(crate) fn round_rect(
        &self,
        _: &mut CallContext,
        x: f64,
        y: f64,
        w: f64,
        h: f64,
        radii: UnrestrictedDoubleOrDOMPointInitOrUnrestrictedDoubleOrDOMPointInitSequence,
    ) -> WebIdlResult<()> {
        let radii = round_rect_radii(radii);
        self.build_path(|path| canvas_path::round_rect(path, x, y, w, h, &radii))?;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-arc
    #[allow(clippy::too_many_arguments, reason = "the arguments of arc()")]
    pub(crate) fn arc(
        &self,
        _: &mut CallContext,
        x: f64,
        y: f64,
        radius: f64,
        start_angle: f64,
        end_angle: f64,
        counterclockwise: bool,
    ) -> WebIdlResult<()> {
        self.build_path(|path| {
            canvas_path::arc(
                path,
                float(x),
                float(y),
                float(radius),
                float(start_angle),
                float(end_angle),
                counterclockwise,
            )
        })?;
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-ellipse
    #[allow(clippy::too_many_arguments, reason = "the arguments of ellipse()")]
    pub(crate) fn ellipse(
        &self,
        _: &mut CallContext,
        x: f64,
        y: f64,
        radius_x: f64,
        radius_y: f64,
        rotation: f64,
        start_angle: f64,
        end_angle: f64,
        counterclockwise: bool,
    ) -> WebIdlResult<()> {
        self.build_path(|path| {
            canvas_path::ellipse(
                path,
                float(x),
                float(y),
                float(radius_x),
                float(radius_y),
                float(rotation),
                float(start_angle),
                float(end_angle),
                counterclockwise,
            )
        })?;
        Ok(())
    }
}
