/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#path2d

use core::cell::RefCell;

use ak::Utf16String;
use libgfx_rust::path::Path;
use libjs_rust::api::Trace;

use super::canvas_path::{
    self, UnrestrictedDoubleOrDOMPointInitOrUnrestrictedDoubleOrDOMPointInitSequence, round_rect_radii,
};
use crate::bindings::interface::{Ref, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};
use crate::geometry::dictionaries::{DOMMatrix2DInit, validate_and_fixup_2d};

/// The union (Path2D or DOMString) of HTML/Path2D.idl, whose members the bindings convert.
pub(crate) enum Path2DOrDOMString {
    Path2D(Ref<Path2D>),
    DOMString(Utf16String),
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Path2D {
    #[gc(untraced)]
    path: RefCell<Path>,
}

impl Wrappable for Path2D {}

#[allow(
    clippy::cast_possible_truncation,
    reason = "these operations take floats, as the C++ did"
)]
fn float(number: f64) -> f32 {
    number as f32
}

impl Path2D {
    /// The path, which the 2D contexts fill, stroke and clip with.
    ///
    /// # Safety
    ///
    /// Nothing may change the path while the reference is in use.
    pub(crate) unsafe fn path_unguarded(&self) -> &Path {
        // SAFETY: The caller keeps the path from changing while it reads it.
        unsafe { self.path.try_borrow_unguarded() }.expect("nothing changes the path meanwhile")
    }

    fn build<R>(&self, function: impl FnOnce(&mut Path) -> R) -> R {
        function(&mut self.path.borrow_mut())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-path2d
    pub(crate) fn construct(_: &mut CallContext, path: Option<Path2DOrDOMString>) -> WebIdlResult<Self> {
        // 1. Let output be a new Path2D object.
        let output = Self {
            path: RefCell::new(Path::new()),
        };

        // 2. If path is not given, then return output.
        let Some(path) = path else {
            return Ok(output);
        };

        let path = match path {
            // 3. If path is a Path2D object, then add all subpaths of path to output and return output.
            //    (In other words, it returns a copy of the argument.)
            Path2DOrDOMString::Path2D(path) => {
                output.path.replace(path.path.borrow().clone());
                return Ok(output);
            }
            Path2DOrDOMString::DOMString(path) => path,
        };

        // 4. Let svgPath be the result of parsing and interpreting path according to SVG 2's rules for path data.
        //    [SVG]
        let svg_path = libweb_rust::gfx_path_from_path_data(path.as_units());

        if !svg_path.is_empty() {
            // 5. Let (x, y) be the last point in svgPath.
            let xy = svg_path.last_point();

            // 6. Add all the subpaths, if any, from svgPath to output.
            output.path.replace(svg_path);

            // 7. Create a new subpath in output with (x, y) as the only point in the subpath.
            output.build(|path| canvas_path::move_to(path, xy.x, xy.y));
        }

        // 8. Return output.
        Ok(output)
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-path2d-addpath
    pub(crate) fn add_path(
        &self,
        _: &mut CallContext,
        path: Ref<Path2D>,
        transform: DOMMatrix2DInit,
    ) -> WebIdlResult<()> {
        // The addPath(path, transform) method, when invoked on a Path2D object a, must run these steps:

        // 1. If the Path2D object path has no subpaths, then return.
        if path.path.borrow().is_empty() {
            return Ok(());
        }

        // 2. Let matrix be the result of creating a DOMMatrix from the 2D dictionary transform.
        let matrix = validate_and_fixup_2d(&transform)?;

        // 3. If one or more of matrix's m11 element, m12 element, m21 element, m22 element, m41 element, or m42
        //    element are infinite or NaN, then return.
        if !matrix.is_finite() {
            return Ok(());
        }

        // 4. Create a copy of all the subpaths in path. Let c be this copy.
        // 5. Transform all the coordinates and lines in c by the transform matrix matrix.
        let copy = path.path.borrow().copy_transformed(matrix.to_affine_transform().values);

        // 6. Let (x, y) be the last point in the last subpath of c.
        let xy = copy.last_point();

        // 7. Add all the subpaths in c to a.
        // FIXME: Is this correct?
        self.build(|path| path.append_path(&copy));

        // 8. Create a new subpath in a with (x, y) as the only point in the subpath.
        self.build(|path| canvas_path::move_to(path, xy.x, xy.y));

        Ok(())
    }

    // The operations of the CanvasPath mixin, whose steps are in canvas_path.rs.

    pub(crate) fn close_path(&self, _: &mut CallContext) -> WebIdlResult<()> {
        self.build(canvas_path::close_path);
        Ok(())
    }

    pub(crate) fn move_to(&self, _: &mut CallContext, x: f64, y: f64) -> WebIdlResult<()> {
        self.build(|path| canvas_path::move_to(path, float(x), float(y)));
        Ok(())
    }

    pub(crate) fn line_to(&self, _: &mut CallContext, x: f64, y: f64) -> WebIdlResult<()> {
        self.build(|path| canvas_path::line_to(path, float(x), float(y)));
        Ok(())
    }

    pub(crate) fn quadratic_curve_to(
        &self,
        _: &mut CallContext,
        cpx: f64,
        cpy: f64,
        x: f64,
        y: f64,
    ) -> WebIdlResult<()> {
        self.build(|path| canvas_path::quadratic_curve_to(path, float(cpx), float(cpy), float(x), float(y)));
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
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
        self.build(|path| canvas_path::bezier_curve_to(path, cp1x, cp1y, cp2x, cp2y, x, y));
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn arc_to(
        &self,
        _: &mut CallContext,
        x1: f64,
        y1: f64,
        x2: f64,
        y2: f64,
        radius: f64,
    ) -> WebIdlResult<()> {
        // NB: A Path2D has no transformation matrix, so its points are in the coordinates that it was given.
        self.build(|path| canvas_path::arc_to(path, x1, y1, x2, y2, radius))?;
        Ok(())
    }

    pub(crate) fn rect(&self, _: &mut CallContext, x: f64, y: f64, w: f64, h: f64) -> WebIdlResult<()> {
        self.build(|path| canvas_path::rect(path, x, y, w, h));
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
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
        self.build(|path| canvas_path::round_rect(path, x, y, w, h, &radii))?;
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
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
        self.build(|path| {
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

    #[allow(clippy::too_many_arguments)]
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
        self.build(|path| {
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
