/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvastransform
//!
//! The current default path is kept in the coordinates of the current transformation matrix, so an operation that
//! changes the matrix transforms the path by the inverse of the change.

use libgfx_rust::AffineTransform;

use crate::bindings::conversions::CppInterface;
use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::canvas::create_dom_matrix;
use crate::canvas_2d::context::{CanvasKind, RenderingContext2D, multiply_transforms, rotated, scaled, translated};
use crate::canvas_2d::float;
use crate::geometry::dom_matrix_read_only::{DOMMatrix2DInit, validate_and_fixup_2d};

fn all_finite(values: &[f64]) -> bool {
    values.iter().all(|value| value.is_finite())
}

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-scale
    pub(crate) fn scale(&self, _: &mut CallContext, x: f64, y: f64) -> WebIdlResult<()> {
        let (x, y) = (float(x), float(y));
        if !x.is_finite() || !y.is_finite() {
            return Ok(());
        }
        {
            let mut state = self.drawing_state_mut();
            state.transform = scaled(state.transform, x, y);
        }
        self.flush_transform();
        self.transform_path(scaled(
            AffineTransform::identity(),
            float(1.0 / f64::from(x)),
            float(1.0 / f64::from(y)),
        ));
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-translate
    pub(crate) fn translate(&self, _: &mut CallContext, x: f64, y: f64) -> WebIdlResult<()> {
        let (x, y) = (float(x), float(y));
        if !x.is_finite() || !y.is_finite() {
            return Ok(());
        }
        {
            let mut state = self.drawing_state_mut();
            state.transform = translated(state.transform, x, y);
        }
        self.flush_transform();
        self.transform_path(translated(AffineTransform::identity(), -x, -y));
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-rotate
    pub(crate) fn rotate(&self, _: &mut CallContext, radians: f64) -> WebIdlResult<()> {
        let radians = float(radians);
        if !radians.is_finite() {
            return Ok(());
        }
        {
            let mut state = self.drawing_state_mut();
            state.transform = rotated(state.transform, radians);
        }
        self.flush_transform();
        self.transform_path(rotated(AffineTransform::identity(), -radians));
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-transform
    #[allow(clippy::too_many_arguments, reason = "the arguments of transform()")]
    pub(crate) fn transform(
        &self,
        _: &mut CallContext,
        a: f64,
        b: f64,
        c: f64,
        d: f64,
        e: f64,
        f: f64,
    ) -> WebIdlResult<()> {
        self.multiply_transform(a, b, c, d, e, f);
        Ok(())
    }

    fn multiply_transform(&self, a: f64, b: f64, c: f64, d: f64, e: f64, f: f64) {
        // 1. If any of the arguments are infinite or NaN, then return.
        if !all_finite(&[a, b, c, d, e, f]) {
            return;
        }

        // 2. Replace the current transformation matrix with the result of multiplying the current transformation
        //    matrix with the matrix described by:
        //    a c e
        //    b d f
        //    0 0 1
        let transform = AffineTransform::new(float(a), float(b), float(c), float(d), float(e), float(f));
        {
            let mut state = self.drawing_state_mut();
            state.transform = multiply_transforms(state.transform, transform);
        }

        if let Some(inverse) = transform.inverse() {
            self.transform_path(inverse);
        }
        self.flush_transform();
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-gettransform
    pub(crate) fn get_transform(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        let transform = self.drawing_state().transform;
        Ok(create_dom_matrix(cx.realm(), transform))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-settransform
    #[allow(clippy::too_many_arguments, reason = "the arguments of setTransform()")]
    pub(crate) fn set_transform(
        &self,
        _: &mut CallContext,
        a: f64,
        b: f64,
        c: f64,
        d: f64,
        e: f64,
        f: f64,
    ) -> WebIdlResult<()> {
        // 1. If any of the arguments are infinite or NaN, then return.
        if !all_finite(&[a, b, c, d, e, f]) {
            return Ok(());
        }

        // 2. Reset the current transformation matrix to the matrix described by:
        //    a c e
        //    b d f
        //    0 0 1
        // NB: That is the identity matrix multiplied with the matrix.
        self.drawing_state_mut().transform = AffineTransform::identity();
        self.flush_transform();
        self.multiply_transform(a, b, c, d, e, f);
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-settransform-matrix
    pub(crate) fn set_transform_to_matrix(&self, _: &mut CallContext, transform: DOMMatrix2DInit) -> WebIdlResult<()> {
        // 1. Let matrix be the result of creating a DOMMatrix from the 2D dictionary transform.
        let matrix = validate_and_fixup_2d(&transform)?;

        // 2. If one or more of matrix's m11 element, m12 element, m21 element, m22 element, m41 element, or m42
        //    element are infinite or NaN, then return.
        if !matrix.is_finite() {
            return Ok(());
        }

        // 3. Reset the current transformation matrix to matrix.
        let original_transform = {
            let mut state = self.drawing_state_mut();
            core::mem::replace(&mut state.transform, matrix.to_affine_transform())
        };

        self.transform_path(original_transform);

        self.flush_transform();
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-resettransform
    pub(crate) fn reset_transform(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // The resetTransform() method, when invoked, must reset the current transformation matrix to the identity
        // matrix.
        self.drawing_state_mut().transform = AffineTransform::identity();
        self.flush_transform();
        Ok(())
    }
}
