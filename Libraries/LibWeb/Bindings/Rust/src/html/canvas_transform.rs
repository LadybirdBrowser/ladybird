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
use libjs_rust::api::Vm;

use crate::bindings::interface::{Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, RenderingContext2D, multiply_transforms, rotated, scaled, translated};
use crate::canvas_2d::float;
use crate::geometry::dictionaries::{DOMMatrix2DInit, validate_and_fixup_2d};
use crate::geometry::dom_matrix::DOMMatrix;
use crate::geometry::dom_matrix_read_only::DOMMatrixReadOnly;

fn all_finite(values: &[f64]) -> bool {
    values.iter().all(|value| value.is_finite())
}

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-scale
    pub(crate) fn scale(&self, cx: &mut CallContext, x: f64, y: f64) -> WebIdlResult<()> {
        let (x, y) = (float(x), float(y));
        if !x.is_finite() || !y.is_finite() {
            return Ok(());
        }
        {
            let mut state = self.drawing_state_mut(cx.vm());
            state.transform = scaled(state.transform, x, y);
        }
        self.flush_transform();
        // NB: A zero factor leaves the matrix without an inverse, and then there is no 1 / x or 1 / y.
        self.keep_path_in_place_after_multiplication(|| {
            Some(scaled(
                AffineTransform::identity(),
                float(1.0 / f64::from(x)),
                float(1.0 / f64::from(y)),
            ))
        });
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-translate
    pub(crate) fn translate(&self, cx: &mut CallContext, x: f64, y: f64) -> WebIdlResult<()> {
        let (x, y) = (float(x), float(y));
        if !x.is_finite() || !y.is_finite() {
            return Ok(());
        }
        {
            let mut state = self.drawing_state_mut(cx.vm());
            state.transform = translated(state.transform, x, y);
        }
        self.flush_transform();
        self.keep_path_in_place_after_multiplication(|| Some(translated(AffineTransform::identity(), -x, -y)));
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-rotate
    pub(crate) fn rotate(&self, cx: &mut CallContext, radians: f64) -> WebIdlResult<()> {
        let radians = float(radians);
        if !radians.is_finite() {
            return Ok(());
        }
        {
            let mut state = self.drawing_state_mut(cx.vm());
            state.transform = rotated(state.transform, radians);
        }
        self.flush_transform();
        self.keep_path_in_place_after_multiplication(|| Some(rotated(AffineTransform::identity(), -radians)));
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-transform
    #[allow(clippy::too_many_arguments, reason = "the arguments of transform()")]
    pub(crate) fn transform(
        &self,
        cx: &mut CallContext,
        a: f64,
        b: f64,
        c: f64,
        d: f64,
        e: f64,
        f: f64,
    ) -> WebIdlResult<()> {
        self.multiply_transform(cx.vm(), a, b, c, d, e, f);
        Ok(())
    }

    #[allow(clippy::too_many_arguments, reason = "the arguments of transform()")]
    fn multiply_transform(&self, vm: &Vm, a: f64, b: f64, c: f64, d: f64, e: f64, f: f64) {
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
            let mut state = self.drawing_state_mut(vm);
            state.transform = multiply_transforms(state.transform, transform);
        }

        self.keep_path_in_place_after_multiplication(|| transform.inverse());
        self.flush_transform();
    }

    /// Moves the current default path into the coordinates of the current transformation matrix, after a
    /// multiplication of the matrix with another whose inverse is given, so that the points of the path stay where
    /// they were on the output bitmap when they were added.
    ///
    /// NB: A path cannot be kept in the coordinates of a matrix that is not invertible. When a multiplication leaves
    ///     the matrix without an inverse, the path stays in the coordinates of the last matrix that was invertible,
    ///     and every later multiplication leaves the matrix without one too. keep_path_in_place() moves the path from
    ///     there when the matrix is replaced with an invertible one again.
    fn keep_path_in_place_after_multiplication(
        &self,
        inverse_of_multiplication: impl FnOnce() -> Option<AffineTransform>,
    ) {
        let transform = self.drawing_state().transform;
        if transform.inverse().is_none() {
            return;
        }
        if let Some(inverse) = inverse_of_multiplication() {
            self.transform_path(inverse);
        }
        self.set_path_transform(transform);
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-gettransform
    pub(crate) fn get_transform(&self, cx: &mut CallContext) -> WebIdlResult<Ref<DOMMatrix>> {
        // The getTransform() method, when invoked, must return a newly created DOMMatrix representing a copy of the
        // current transformation matrix matrix of the context.
        let transform = self.drawing_state().transform;
        let elements = transform.values.map(f64::from);
        let matrix = DOMMatrix::from_read_only(DOMMatrixReadOnly::create_2d_matrix(elements));
        Ok(allocate(cx.vm(), |_| matrix))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-settransform
    #[allow(clippy::too_many_arguments, reason = "the arguments of setTransform()")]
    pub(crate) fn set_transform(
        &self,
        cx: &mut CallContext,
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
        self.set_current_transform(
            cx.vm(),
            AffineTransform::new(float(a), float(b), float(c), float(d), float(e), float(f)),
        );
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-settransform-matrix
    pub(crate) fn set_transform_to_matrix(&self, cx: &mut CallContext, transform: DOMMatrix2DInit) -> WebIdlResult<()> {
        // 1. Let matrix be the result of creating a DOMMatrix from the 2D dictionary transform.
        let matrix = validate_and_fixup_2d(&transform)?;

        // 2. If one or more of matrix's m11 element, m12 element, m21 element, m22 element, m41 element, or m42
        //    element are infinite or NaN, then return.
        if !matrix.is_finite() {
            return Ok(());
        }

        // 3. Reset the current transformation matrix to matrix.
        self.set_current_transform(cx.vm(), matrix.to_affine_transform());
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-resettransform
    pub(crate) fn reset_transform(&self, cx: &mut CallContext) -> WebIdlResult<()> {
        // The resetTransform() method, when invoked, must reset the current transformation matrix to the identity
        // matrix.
        self.set_current_transform(cx.vm(), AffineTransform::identity());
        Ok(())
    }

    /// Replaces the current transformation matrix, which leaves the current default path where it is on the output
    /// bitmap.
    fn set_current_transform(&self, vm: &Vm, transform: AffineTransform) {
        self.drawing_state_mut(vm).transform = transform;
        self.keep_path_in_place();
        self.flush_transform();
    }

    /// Moves the current default path into the coordinates of the current transformation matrix, after a change of
    /// the matrix that is not a multiplication with another, so that the points of the path stay where they were on
    /// the output bitmap when they were added.
    pub(crate) fn keep_path_in_place(&self) {
        let transform = self.drawing_state().transform;
        // NB: A path cannot be kept in the coordinates of a matrix that is not invertible, so it stays in those of the
        //     last matrix that was, as it does for a multiplication.
        let Some(inverse) = transform.inverse() else {
            return;
        };
        let path_transform = self.path_transform();
        self.set_path_transform(transform);
        if transform.values == path_transform.values {
            return;
        }
        self.transform_path(multiply_transforms(inverse, path_transform));
    }
}
