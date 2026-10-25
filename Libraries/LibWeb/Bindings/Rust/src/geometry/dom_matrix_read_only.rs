/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.fxtf.org/geometry/#dommatrixreadonly

use core::cell::Cell;

use ak::Utf16String;
use libjs_rust::api::{
    ArrayBuffer, ArrayBufferOrder, ArrayBufferShared, Gc, Trace, TypedArrayBase, TypedArrayKind,
    create_typed_array_on_buffer, is_typed_array_out_of_bounds, make_typed_array_with_buffer_witness_record,
    number_to_string, typed_array_byte_length,
};

#[cfg_attr(
    not(web_idl_bindings),
    allow(unused_imports, reason = "the generated bindings name the dictionaries here")
)]
pub(crate) use super::dictionaries::DOMMatrix2DInit;
pub(crate) use super::dictionaries::DOMMatrixInit;
use super::dictionaries::{FixedUpMatrixInit, Matrix2D, validate_and_fixup};
use super::dom_matrix::DOMMatrix;
use super::dom_point::DOMPoint;
use super::dom_point_read_only::{DOMPointInit, DOMPointReadOnly};
use super::matrix::{ColumnMajor, Matrix4, to_radians};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, realm_is_window};
use crate::bindings::structured_data::{Serializable, SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, Exception, WebIdlResult};

unsafe extern "C" {
    // Defined in Libraries/LibWeb/Geometry/DOMMatrixReadOnly.cpp, which parses the string with LibWeb's CSS parser.
    fn web_dom_matrix_parse_transform_list(
        transform_list: usize,
        out_elements: &mut [f64; 16],
        out_is_2d: &mut bool,
    ) -> bool;
}

/// The union (DOMString or sequence<unrestricted double>) of the constructors of DOMMatrixReadOnly and DOMMatrix.
pub(crate) enum DOMStringOrUnrestrictedDoubleSequence {
    DOMString(Utf16String),
    Sequence(Vec<f64>),
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DOMMatrixReadOnly {
    #[gc(untraced)]
    matrix: Cell<Matrix4>,
    #[gc(untraced)]
    is_2d: Cell<bool>,
}

impl Wrappable for DOMMatrixReadOnly {}

impl DOMMatrixReadOnly {
    pub(crate) fn new(matrix: Matrix4, is_2d: bool) -> Self {
        Self {
            matrix: Cell::new(matrix),
            is_2d: Cell::new(is_2d),
        }
    }

    /// https://drafts.fxtf.org/geometry/#create-a-2d-matrix
    pub(crate) fn create_2d_matrix([m11, m12, m21, m22, m41, m42]: [f64; 6]) -> Self {
        // 1. Let matrix be a new instance of type.
        // 2. Set m11 element, m12 element, m21 element, m22 element, m41 element and m42 element to the values of
        //    init in order starting with the first value.
        // 3. Set m13 element, m14 element, m23 element, m24 element, m31 element, m32 element, m34 element, and m43
        //    element to 0.
        // 4. Set m33 element and m44 element to 1.
        let matrix = Matrix4::from_column_major([
            m11, m12, 0.0, 0.0, m21, m22, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, m41, m42, 0.0, 1.0,
        ]);

        // 5. Set is 2D to true.
        // 6. Return matrix
        Self::new(matrix, true)
    }

    /// https://drafts.fxtf.org/geometry/#create-a-3d-matrix
    pub(crate) fn create_3d_matrix(init: [f64; 16]) -> Self {
        // 1. Let matrix be a new instance of type.
        // 2. Set m11 element to m44 element to the values of init in column-major order.
        // 3. Set is 2D to false.
        // 4. Return matrix
        Self::new(Matrix4::from_column_major(init), false)
    }

    /// The steps of the constructors of DOMMatrixReadOnly and DOMMatrix,
    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-dommatrixreadonly
    pub(crate) fn from_init(
        cx: &CallContext,
        init: Option<DOMStringOrUnrestrictedDoubleSequence>,
    ) -> WebIdlResult<Self> {
        match init {
            // -> If init is omitted
            None => {
                // Return the result of invoking create a 2d matrix of type DOMMatrixReadOnly or DOMMatrix as
                // appropriate, with the sequence [1, 0, 0, 1, 0, 0].
                Ok(Self::create_2d_matrix([1.0, 0.0, 0.0, 1.0, 0.0, 0.0]))
            }
            // -> If init is a DOMString
            Some(DOMStringOrUnrestrictedDoubleSequence::DOMString(init)) => {
                // 1. If current global object is not a Window object, then throw a TypeError exception.
                if !realm_is_window(cx.realm()) {
                    return Err(Exception::TypeError(Utf16String::from_ascii(
                        b"This can only be used in a Window context",
                    )));
                }

                // 2. Parse init into an abstract matrix, and let matrix and 2dTransform be the result. If the result
                //    is failure, then throw a "SyntaxError" DOMException.
                let (matrix, is_2d_transform) = parse_transform_list(&init)?;

                // -> If 2dTransform is true
                if is_2d_transform {
                    // Return the result of invoking create a 2d matrix of type DOMMatrixReadOnly or DOMMatrix as
                    // appropriate, with a sequence of numbers, the values being the elements m11, m12, m21, m22, m41
                    // and m42 of matrix.
                    return Ok(Self::create_2d_matrix([
                        matrix.element(1, 1),
                        matrix.element(1, 2),
                        matrix.element(2, 1),
                        matrix.element(2, 2),
                        matrix.element(4, 1),
                        matrix.element(4, 2),
                    ]));
                }

                // -> Otherwise
                // Return the result of invoking create a 3d matrix of type DOMMatrixReadOnly or DOMMatrix as
                // appropriate, with a sequence of numbers, the values being the 16 elements of matrix.
                Ok(Self::create_3d_matrix(matrix.to_column_major()))
            }
            Some(DOMStringOrUnrestrictedDoubleSequence::Sequence(init)) => {
                Self::from_sequence(&init).ok_or_else(|| {
                    // -> Otherwise
                    // Throw a TypeError exception.
                    Exception::TypeError(Utf16String::from_ascii(
                        format!(
                            "Sequence must contain exactly 6 or 16 elements, got {} element(s)",
                            init.len()
                        )
                        .as_bytes(),
                    ))
                })
            }
        }
    }

    /// The matrix of a sequence of 6 or 16 elements, of which the constructors and fromFloat32Array() and
    /// fromFloat64Array() create a 2d or a 3d matrix.
    fn from_sequence(init: &[f64]) -> Option<Self> {
        match init.len() {
            // -> If init is a sequence with 6 elements
            // Return the result of invoking create a 2d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate,
            // with the sequence init.
            6 => Some(Self::create_2d_matrix(
                init.try_into().expect("the sequence has 6 elements"),
            )),
            // -> If init is a sequence with 16 elements
            // Return the result of invoking create a 3d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate,
            // with the sequence init.
            16 => Some(Self::create_3d_matrix(
                init.try_into().expect("the sequence has 16 elements"),
            )),
            _ => None,
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-dommatrixreadonly
    pub(crate) fn construct(
        cx: &mut CallContext,
        init: Option<DOMStringOrUnrestrictedDoubleSequence>,
    ) -> WebIdlResult<Self> {
        Self::from_init(cx, init)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-frommatrix
    pub(crate) fn from_matrix(cx: &mut CallContext, other: DOMMatrixInit) -> WebIdlResult<Ref<Self>> {
        // The fromMatrix(other) static method on DOMMatrixReadOnly must create a DOMMatrixReadOnly from the dictionary
        // other.
        let matrix = Self::from_dictionary(&other)?;
        Ok(allocate(cx.vm(), |_| matrix))
    }

    /// https://drafts.fxtf.org/geometry/#create-a-dommatrixreadonly-from-the-dictionary
    pub(crate) fn from_dictionary(other: &DOMMatrixInit) -> WebIdlResult<Self> {
        // 1. Validate and fixup other.
        let FixedUpMatrixInit {
            matrix_2d:
                Matrix2D {
                    m11,
                    m12,
                    m21,
                    m22,
                    m41,
                    m42,
                },
            init: [m13, m14, m23, m24, m31, m32, m33, m34, m43, m44],
            is_2d,
        } = validate_and_fixup(other)?;

        // 2. If the is2D dictionary member of other is true
        if is_2d {
            // Return the result of invoking create a 2d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate,
            // with a sequence of numbers, the values being the 6 elements m11, m12, m21, m22, m41 and m42 of other in
            // the given order.
            return Ok(Self::create_2d_matrix([m11, m12, m21, m22, m41, m42]));
        }

        // Otherwise
        // Return the result of invoking create a 3d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate, with
        // a sequence of numbers, the values being the 16 elements m11, m12, m13, ..., m44 of other in the given order.
        Ok(Self::create_3d_matrix([
            m11, m12, m13, m14, m21, m22, m23, m24, m31, m32, m33, m34, m41, m42, m43, m44,
        ]))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-fromfloat32array
    pub(crate) fn from_float32_array(cx: &mut CallContext, array32: Gc<TypedArrayBase>) -> WebIdlResult<Ref<Self>> {
        let matrix = Self::from_typed_array(array32)?;
        Ok(allocate(cx.vm(), |_| matrix))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-fromfloat64array
    pub(crate) fn from_float64_array(cx: &mut CallContext, array64: Gc<TypedArrayBase>) -> WebIdlResult<Ref<Self>> {
        let matrix = Self::from_typed_array(array64)?;
        Ok(allocate(cx.vm(), |_| matrix))
    }

    /// The steps of fromFloat32Array() and fromFloat64Array() on DOMMatrixReadOnly and DOMMatrix, for a Float32Array
    /// or a Float64Array.
    pub(crate) fn from_typed_array(array: Gc<TypedArrayBase>) -> WebIdlResult<Self> {
        // -> If array32 has 6 elements
        //    Return the result of invoking create a 2d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate,
        //    with a sequence of numbers taking the values from array32 in the provided order.
        // -> If array32 has 16 elements
        //    Return the result of invoking create a 3d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate,
        //    with a sequence of numbers taking the values from array32 in the provided order.
        // -> Otherwise
        //    Throw a TypeError exception.
        Self::from_sequence(&typed_array_elements(array)).ok_or_else(|| {
            Exception::TypeError(Utf16String::from_ascii(
                b"Sequence must contain exactly 6 or 16 elements",
            ))
        })
    }

    pub(crate) fn matrix(&self) -> Matrix4 {
        self.matrix.get()
    }

    pub(crate) fn is_2d(&self) -> bool {
        self.is_2d.get()
    }

    pub(super) fn set_matrix(&self, matrix: Matrix4) {
        self.matrix.set(matrix);
    }

    pub(super) fn set_is_2d(&self, is_2d: bool) {
        self.is_2d.set(is_2d);
    }

    fn element(&self, row: usize, column: usize) -> f64 {
        self.matrix.get().element(row, column)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-a
    pub(crate) fn a(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The a attribute is an alias to the m11 attribute.
        Ok(self.element(1, 1))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-b
    pub(crate) fn b(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The b attribute is an alias to the m12 attribute.
        Ok(self.element(1, 2))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-c
    pub(crate) fn c(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The c attribute is an alias to the m21 attribute.
        Ok(self.element(2, 1))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-d
    pub(crate) fn d(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The d attribute is an alias to the m22 attribute.
        Ok(self.element(2, 2))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-e
    pub(crate) fn e(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The e attribute is an alias to the m41 attribute.
        Ok(self.element(4, 1))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-f
    pub(crate) fn f(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The f attribute is an alias to the m42 attribute.
        Ok(self.element(4, 2))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m11
    pub(crate) fn m11(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(1, 1))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m12
    pub(crate) fn m12(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(1, 2))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m13
    pub(crate) fn m13(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(1, 3))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m14
    pub(crate) fn m14(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(1, 4))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m21
    pub(crate) fn m21(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(2, 1))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m22
    pub(crate) fn m22(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(2, 2))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m23
    pub(crate) fn m23(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(2, 3))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m24
    pub(crate) fn m24(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(2, 4))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m31
    pub(crate) fn m31(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(3, 1))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m32
    pub(crate) fn m32(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(3, 2))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m33
    pub(crate) fn m33(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(3, 3))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m34
    pub(crate) fn m34(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(3, 4))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m41
    pub(crate) fn m41(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(4, 1))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m42
    pub(crate) fn m42(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(4, 2))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m43
    pub(crate) fn m43(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(4, 3))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m44
    pub(crate) fn m44(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.element(4, 4))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-is2d
    pub(crate) fn is2d(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The is2D attribute must return the value of is 2D.
        Ok(self.is_2d.get())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-isidentity
    pub(crate) fn is_identity(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The isIdentity attribute must return true if m12 element, m13 element, m14 element, m21 element, m23
        // element, m24 element, m31 element, m32 element, m34 element, m41 element, m42 element, m43 element are 0 or
        // -0 and m11 element, m22 element, m33 element, m44 element are 1. Otherwise it must return false.
        Ok(self.matrix.get() == Matrix4::identity())
    }

    /// A new DOMMatrix initialized to the values of the current matrix, which the immutable transformation methods
    /// transform and return.
    fn copy(&self) -> DOMMatrix {
        DOMMatrix::from_read_only(DOMMatrixReadOnly::new(self.matrix.get(), self.is_2d.get()))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-translate
    pub(crate) fn translate(&self, cx: &mut CallContext, tx: f64, ty: f64, tz: f64) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let result be the resulting matrix initialized to the values of the current matrix.
        let result = self.copy();

        // 2. Perform a translateSelf() transformation on result with the arguments tx, ty, tz.
        result.parent().translate_self_steps(tx, ty, tz);

        // 3. Return result.
        Ok(allocate(cx.vm(), |_| result))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-scale
    #[allow(clippy::too_many_arguments, reason = "the operation has six arguments")]
    pub(crate) fn scale(
        &self,
        cx: &mut CallContext,
        scale_x: f64,
        scale_y: Option<f64>,
        scale_z: f64,
        origin_x: f64,
        origin_y: f64,
        origin_z: f64,
    ) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. If scaleY is missing, set scaleY to the value of scaleX.
        let scale_y = scale_y.unwrap_or(scale_x);

        // 2. Let result be the resulting matrix initialized to the values of the current matrix.
        let result = self.copy();

        // 3. Perform a scaleSelf() transformation on result with the arguments scaleX, scaleY, scaleZ, originX,
        //    originY, originZ.
        result
            .parent()
            .scale_self_steps(scale_x, Some(scale_y), scale_z, origin_x, origin_y, origin_z);

        // 4. Return result.
        Ok(allocate(cx.vm(), |_| result))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-scalenonuniform
    pub(crate) fn scale_non_uniform(
        &self,
        cx: &mut CallContext,
        scale_x: f64,
        scale_y: f64,
    ) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let result be the resulting matrix initialized to the values of the current matrix.
        let result = self.copy();

        // 2. Perform a scaleSelf() transformation on result with the arguments scaleX, scaleY, 1, 0, 0, 0.
        result
            .parent()
            .scale_self_steps(scale_x, Some(scale_y), 1.0, 0.0, 0.0, 0.0);

        // 3. Return result.
        Ok(allocate(cx.vm(), |_| result))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-scale3d
    pub(crate) fn scale3d(
        &self,
        cx: &mut CallContext,
        scale: f64,
        origin_x: f64,
        origin_y: f64,
        origin_z: f64,
    ) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let result be the resulting matrix initialized to the values of the current matrix.
        let result = self.copy();

        // 2. Perform a scale3dSelf() transformation on result with the arguments scale, originX, originY, originZ.
        result.parent().scale3d_self_steps(scale, origin_x, origin_y, origin_z);

        // 3. Return result.
        Ok(allocate(cx.vm(), |_| result))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-rotate
    pub(crate) fn rotate(
        &self,
        cx: &mut CallContext,
        rot_x: f64,
        rot_y: Option<f64>,
        rot_z: Option<f64>,
    ) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let result be the resulting matrix initialized to the values of the current matrix.
        let result = self.copy();

        // 2. Perform a rotateSelf() transformation on result with the arguments rotX, rotY, rotZ.
        result.parent().rotate_self_steps(rot_x, rot_y, rot_z);

        // 3. Return result.
        Ok(allocate(cx.vm(), |_| result))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-rotatefromvector
    pub(crate) fn rotate_from_vector(&self, cx: &mut CallContext, x: f64, y: f64) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let result be the resulting matrix initialized to the values of the current matrix.
        let result = self.copy();

        // 2. Perform a rotateFromVectorSelf() transformation on result with the arguments x, y.
        result.parent().rotate_from_vector_self_steps(x, y);

        // 3. Return result.
        Ok(allocate(cx.vm(), |_| result))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-rotateaxisangle
    pub(crate) fn rotate_axis_angle(
        &self,
        cx: &mut CallContext,
        x: f64,
        y: f64,
        z: f64,
        angle: f64,
    ) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let result be the resulting matrix initialized to the values of the current matrix.
        let result = self.copy();

        // 2. Perform a rotateAxisAngleSelf() transformation on result with the arguments x, y, z, angle.
        result.parent().rotate_axis_angle_self_steps(x, y, z, angle);

        // 3. Return result.
        Ok(allocate(cx.vm(), |_| result))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-skewx
    pub(crate) fn skew_x(&self, cx: &mut CallContext, sx: f64) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let result be the resulting matrix initialized to the values of the current matrix.
        let result = self.copy();

        // 2. Perform a skewXSelf() transformation on result with the argument sx.
        result.parent().skew_x_self_steps(sx);

        // 3. Return result.
        Ok(allocate(cx.vm(), |_| result))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-skewy
    pub(crate) fn skew_y(&self, cx: &mut CallContext, sy: f64) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let result be the resulting matrix initialized to the values of the current matrix.
        let result = self.copy();

        // 2. Perform a skewYSelf() transformation on result with the argument sy.
        result.parent().skew_y_self_steps(sy);

        // 3. Return result.
        Ok(allocate(cx.vm(), |_| result))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-multiply
    pub(crate) fn multiply(&self, cx: &mut CallContext, other: DOMMatrixInit) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let result be the resulting matrix initialized to the values of the current matrix.
        let result = self.copy();

        // 2. Perform a multiplySelf() transformation on result with the argument other.
        let other_object = DOMMatrix::from_dictionary(&other)?;
        result.parent().multiply_self_steps(other_object.parent());

        // 3. Return result.
        Ok(allocate(cx.vm(), |_| result))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-flipx
    pub(crate) fn flip_x(&self, cx: &mut CallContext) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let result be the resulting matrix initialized to the values of the current matrix.
        let result = self.copy();

        // 2. Post-multiply result with new DOMMatrix([-1, 0, 0, 1, 0, 0]).
        let matrix = result.parent().matrix().multiplied(Matrix4::scale(-1.0, 1.0, 1.0));
        result.parent().set_matrix(matrix);

        // 3. Return result.
        Ok(allocate(cx.vm(), |_| result))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-flipy
    pub(crate) fn flip_y(&self, cx: &mut CallContext) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let result be the resulting matrix initialized to the values of the current matrix.
        let result = self.copy();

        // 2. Post-multiply result with new DOMMatrix([1, 0, 0, -1, 0, 0]).
        let matrix = result.parent().matrix().multiplied(Matrix4::scale(1.0, -1.0, 1.0));
        result.parent().set_matrix(matrix);

        // 3. Return result.
        Ok(allocate(cx.vm(), |_| result))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-inverse
    pub(crate) fn inverse(&self, cx: &mut CallContext) -> WebIdlResult<Ref<DOMMatrix>> {
        // 1. Let result be the resulting matrix initialized to the values of the current matrix.
        let result = self.copy();

        // 2. Perform a invertSelf() transformation on result.
        result.parent().invert_self_steps();

        // 3. Return result.
        // The current matrix is not modified.
        Ok(allocate(cx.vm(), |_| result))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-transformpoint
    pub(crate) fn transform_point(&self, cx: &mut CallContext, point: DOMPointInit) -> WebIdlResult<Ref<DOMPoint>> {
        // Let pointObject be the result of invoking create a DOMPoint from the dictionary point. Return the result of
        // invoking transform a point with a matrix, given pointObject and the current matrix. The passed argument
        // does not get modified.
        let point_object = DOMPointReadOnly::from_dictionary(&point);
        Ok(allocate(cx.vm(), |_| self.transform_point_steps(&point_object)))
    }

    /// https://drafts.fxtf.org/geometry/#transform-a-point-with-a-matrix
    pub(crate) fn transform_point_steps(&self, point: &DOMPointReadOnly) -> DOMPoint {
        // 1. Let x be point’s x coordinate.
        // 2. Let y be point’s y coordinate.
        // 3. Let z be point’s z coordinate.
        // 4. Let w be point’s w perspective.
        // 5. Let pointVector be a new column vector with the elements being x, y, z, and w, respectively.
        let point_vector = point.coordinates();

        // 6. Set pointVector to pointVector pre-multiplied by matrix.
        let [x, y, z, w] = self.matrix.get().map_vector4(point_vector);

        // 7. Let transformedPoint be a new DOMPoint object.
        // 8. Set transformedPoint’s x coordinate to pointVector’s first element.
        // 9. Set transformedPoint’s y coordinate to pointVector’s second element.
        // 10. Set transformedPoint’s z coordinate to pointVector’s third element.
        // 11. Set transformedPoint’s w perspective to pointVector’s fourth element.
        // 12. Return transformedPoint.
        DOMPoint::new(x, y, z, w)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-tofloat32array
    #[allow(clippy::cast_possible_truncation, reason = "a Float32Array holds floats")]
    pub(crate) fn to_float32_array(&self, cx: &mut CallContext) -> WebIdlResult<Gc<TypedArrayBase>> {
        // Returns the serialized 16 elements m11 to m44 of the current matrix in column-major order as Float32Array.
        let bytes: Vec<u8> = self
            .matrix
            .get()
            .to_column_major()
            .iter()
            .flat_map(|element| (*element as f32).to_ne_bytes())
            .collect();
        create_typed_array(cx, TypedArrayKind::Float32Array, &bytes)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-tofloat64array
    pub(crate) fn to_float64_array(&self, cx: &mut CallContext) -> WebIdlResult<Gc<TypedArrayBase>> {
        // Returns the serialized 16 elements m11 to m44 of the current matrix in column-major order as Float64Array.
        let bytes: Vec<u8> = self
            .matrix
            .get()
            .to_column_major()
            .iter()
            .flat_map(|element| element.to_ne_bytes())
            .collect();
        create_typed_array(cx, TypedArrayKind::Float64Array, &bytes)
    }

    /// https://drafts.fxtf.org/geometry/#dommatrixreadonly-stringification-behavior
    pub(crate) fn to_string(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        self.stringify().ok_or_else(|| {
            Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Cannot stringify non-finite matrix values"),
            )
        })
    }

    /// The stringification behavior, which has no string for a matrix with a non-finite element.
    pub(crate) fn stringify(&self) -> Option<Utf16String> {
        let matrix = self.matrix.get();

        // 1. If one or more of m11 element through m44 element are a non-finite value, then throw an
        //    "InvalidStateError" DOMException.
        // NOTE: The CSS syntax cannot represent NaN or Infinity values.
        if !matrix.to_column_major().iter().all(|element| element.is_finite()) {
            return None;
        }

        // 2. Let string be the empty string.
        // 3. If is 2D is true, then:
        //     1. Append "matrix(" to string.
        //     2. Append ! ToString(m11 element) to string.
        //     3. Append ", " to string.
        //     4. Append ! ToString(m12 element) to string.
        //     5. Append ", " to string.
        //     6. Append ! ToString(m21 element) to string.
        //     7. Append ", " to string.
        //     8. Append ! ToString(m22 element) to string.
        //     9. Append ", " to string.
        //     10. Append ! ToString(m41 element) to string.
        //     11. Append ", " to string.
        //     12. Append ! ToString(m42 element) to string.
        //     13. Append ")" to string.
        // 4. Otherwise:
        //     1. Append "matrix3d(" to string.
        //     2. Append ! ToString(m11 element) to string.
        //     3. Append ", " to string.
        //     4. Append ! ToString(m12 element) to string.
        //     5. Append ", " to string.
        //     6. Append ! ToString(m13 element) to string.
        //     7. Append ", " to string.
        //     8. Append ! ToString(m14 element) to string.
        //     9. Append ", " to string.
        //     10. Append ! ToString(m21 element) to string.
        //     11. Append ", " to string.
        //     12. Append ! ToString(m22 element) to string.
        //     13. Append ", " to string.
        //     14. Append ! ToString(m23 element) to string.
        //     15. Append ", " to string.
        //     16. Append ! ToString(m24 element) to string.
        //     17. Append ", " to string.
        //     18. Append ! ToString(m41 element) to string.
        //     19. Append ", " to string.
        //     20. Append ! ToString(m42 element) to string.
        //     21. Append ", " to string.
        //     22. Append ! ToString(m43 element) to string.
        //     23. Append ", " to string.
        //     24. Append ! ToString(m44 element) to string.
        //     25. Append ")" to string.
        // NB: The steps leave out m31 to m34, which matrix3d() requires, so they are appended after m24.
        let (function, elements) = if self.is_2d.get() {
            let m = |row, column| matrix.element(row, column);
            ("matrix(", vec![m(1, 1), m(1, 2), m(2, 1), m(2, 2), m(4, 1), m(4, 2)])
        } else {
            ("matrix3d(", matrix.to_column_major().to_vec())
        };
        let numbers: Vec<String> = elements.into_iter().map(number_to_string).collect();
        let string = format!("{function}{})", numbers.join(", "));

        // 5. Return string.
        Some(Utf16String::from_ascii(string.as_bytes()))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-multiplyself, from step 2 on.
    pub(super) fn multiply_self_steps(&self, other_object: &DOMMatrixReadOnly) {
        // 2. The otherObject matrix gets post-multiplied to the current matrix.
        self.matrix.set(self.matrix.get().multiplied(other_object.matrix.get()));

        // 3. If is 2D of otherObject is false, set is 2D of the current matrix to false.
        if !other_object.is_2d.get() {
            self.is_2d.set(false);
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-premultiplyself, from step 2 on.
    pub(super) fn pre_multiply_self_steps(&self, other_object: &DOMMatrixReadOnly) {
        // 2. The otherObject matrix gets pre-multiplied to the current matrix.
        self.matrix.set(other_object.matrix.get().multiplied(self.matrix.get()));

        // 3. If is 2D of otherObject is false, set is 2D of the current matrix to false.
        if !other_object.is_2d.get() {
            self.is_2d.set(false);
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-translateself
    pub(crate) fn translate_self_steps(&self, tx: f64, ty: f64, tz: f64) {
        // 1. Post-multiply a translation transformation on the current matrix. The 3D translation matrix is described
        //    in CSS Transforms. [CSS3-TRANSFORMS]
        self.matrix
            .set(self.matrix.get().multiplied(Matrix4::translation(tx, ty, tz)));

        // 2. If tz is specified and not 0 or -0, set is 2D of the current matrix to false.
        if tz != 0.0 {
            self.is_2d.set(false);
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-scaleself
    pub(crate) fn scale_self_steps(
        &self,
        scale_x: f64,
        scale_y: Option<f64>,
        scale_z: f64,
        origin_x: f64,
        origin_y: f64,
        origin_z: f64,
    ) {
        // 1. Perform a translateSelf() transformation on the current matrix with the arguments originX, originY,
        //    originZ.
        self.translate_self_steps(origin_x, origin_y, origin_z);

        // 2. If scaleY is missing, set scaleY to the value of scaleX.
        let scale_y = scale_y.unwrap_or(scale_x);

        // 3. Post-multiply a non-uniform scale transformation on the current matrix. The 3D scale matrix is described
        //    in CSS Transforms with sx = scaleX, sy = scaleY and sz = scaleZ. [CSS3-TRANSFORMS]
        self.matrix
            .set(self.matrix.get().multiplied(Matrix4::scale(scale_x, scale_y, scale_z)));

        // 4. Negate originX, originY and originZ.
        // 5. Perform a translateSelf() transformation on the current matrix with the arguments originX, originY,
        //    originZ.
        self.translate_self_steps(-origin_x, -origin_y, -origin_z);

        // 6. If scaleZ is not 1, set is 2D of the current matrix to false.
        if scale_z != 1.0 {
            self.is_2d.set(false);
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-scale3dself
    pub(super) fn scale3d_self_steps(&self, scale: f64, origin_x: f64, origin_y: f64, origin_z: f64) {
        // 1. Apply a translateSelf() transformation to the current matrix with the arguments originX, originY,
        //    originZ.
        self.translate_self_steps(origin_x, origin_y, origin_z);

        // 2. Post-multiply a uniform 3D scale transformation (m11 = m22 = m33 = scale) on the current matrix. The 3D
        //    scale matrix is described in CSS Transforms with sx = sy = sz = scale. [CSS3-TRANSFORMS]
        self.matrix
            .set(self.matrix.get().multiplied(Matrix4::scale(scale, scale, scale)));

        // 3. Apply a translateSelf() transformation to the current matrix with the arguments -originX, -originY,
        //    -originZ.
        self.translate_self_steps(-origin_x, -origin_y, -origin_z);

        // 4. If scale is not 1, set is 2D of the current matrix to false.
        if scale != 1.0 {
            self.is_2d.set(false);
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-rotateself
    pub(super) fn rotate_self_steps(&self, rot_x: f64, rot_y: Option<f64>, rot_z: Option<f64>) {
        // 1. If rotY and rotZ are both missing, set rotZ to the value of rotX and set rotX and rotY to 0.
        let (rot_x, rot_y, rot_z) = match (rot_y, rot_z) {
            (None, None) => (0.0, Some(0.0), Some(rot_x)),
            _ => (rot_x, rot_y, rot_z),
        };

        // 2. If rotY is still missing, set rotY to 0.
        let rot_y = rot_y.unwrap_or(0.0);

        // 3. If rotZ is still missing, set rotZ to 0.
        let rot_z = rot_z.unwrap_or(0.0);

        // 4. If rotX or rotY are not 0 or -0, set is 2D of the current matrix to false.
        if rot_x != 0.0 || rot_y != 0.0 {
            self.is_2d.set(false);
        }

        let mut matrix = self.matrix.get();

        // 5. Post-multiply a rotation transformation on the current matrix around the vector 0, 0, 1 by the specified
        //    rotation rotZ in degrees. The 3D rotation matrix is described in CSS Transforms with alpha = rotZ in
        //    degrees. [CSS3-TRANSFORMS]
        matrix = matrix.multiplied(Matrix4::rotation([0.0, 0.0, 1.0], to_radians(rot_z)));

        // 6. Post-multiply a rotation transformation on the current matrix around the vector 0, 1, 0 by the specified
        //    rotation rotY in degrees. The 3D rotation matrix is described in CSS Transforms with alpha = rotY in
        //    degrees. [CSS3-TRANSFORMS]
        matrix = matrix.multiplied(Matrix4::rotation([0.0, 1.0, 0.0], to_radians(rot_y)));

        // 7. Post-multiply a rotation transformation on the current matrix around the vector 1, 0, 0 by the specified
        //    rotation rotX in degrees. The 3D rotation matrix is described in CSS Transforms with alpha = rotX in
        //    degrees. [CSS3-TRANSFORMS]
        matrix = matrix.multiplied(Matrix4::rotation([1.0, 0.0, 0.0], to_radians(rot_x)));

        self.matrix.set(matrix);
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-rotatefromvectorself
    pub(super) fn rotate_from_vector_self_steps(&self, x: f64, y: f64) {
        // 1. Post-multiply a rotation transformation on the current matrix. The rotation angle is determined by the
        //    angle between the vector (1,0)T and (x,y)T in the clockwise direction. If x and y should both be 0 or -0,
        //    the angle is specified as 0. The 2D rotation matrix is described in CSS Transforms where alpha is the
        //    angle between the vector (1,0)T and (x,y)T in degrees. [CSS3-TRANSFORMS]
        let angle = if x == 0.0 && y == 0.0 { 0.0 } else { y.atan2(x) };
        self.matrix
            .set(self.matrix.get().multiplied(Matrix4::rotation([0.0, 0.0, 1.0], angle)));
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-rotateaxisangleself
    pub(crate) fn rotate_axis_angle_self_steps(&self, x: f64, y: f64, z: f64, angle: f64) {
        // 1. Post-multiply a rotation transformation on the current matrix around the specified vector x, y, z by the
        //    specified rotation angle in degrees. The 3D rotation matrix is described in CSS Transforms with alpha =
        //    angle in degrees. [CSS3-TRANSFORMS]
        // https://drafts.csswg.org/css-transforms-2/#funcdef-rotate3d
        // "A direction vector that cannot be normalized, such as [0,0,0], will cause the rotation to not be applied."
        let length = (x * x + y * y + z * z).sqrt();
        if length != 0.0 {
            let inverse_length = 1.0 / length;
            let axis = [x * inverse_length, y * inverse_length, z * inverse_length];
            self.matrix
                .set(self.matrix.get().multiplied(Matrix4::rotation(axis, to_radians(angle))));
        }

        // 2. If x or y are not 0 or -0, set is 2D of the current matrix to false.
        if x != 0.0 || y != 0.0 {
            self.is_2d.set(false);
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-skewxself
    pub(super) fn skew_x_self_steps(&self, sx: f64) {
        // 1. Post-multiply a skewX transformation on the current matrix by the specified angle sx in degrees. The 2D
        //    skewX matrix is described in CSS Transforms with alpha = sx in degrees. [CSS3-TRANSFORMS]
        let mut skew = Matrix4::identity();
        skew.set_element(2, 1, to_radians(sx).tan());
        self.matrix.set(self.matrix.get().multiplied(skew));
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-skewyself
    pub(super) fn skew_y_self_steps(&self, sy: f64) {
        // 1. Post-multiply a skewX transformation on the current matrix by the specified angle sy in degrees. The 2D
        //    skewY matrix is described in CSS Transforms with beta = sy in degrees. [CSS3-TRANSFORMS]
        let mut skew = Matrix4::identity();
        skew.set_element(1, 2, to_radians(sy).tan());
        self.matrix.set(self.matrix.get().multiplied(skew));
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-invertself
    pub(super) fn invert_self_steps(&self) {
        // 1. Invert the current matrix.
        match self.matrix.get().inverse() {
            Some(inverse) => self.matrix.set(inverse),
            // 2. If the current matrix is not invertible set all attributes to NaN and set is 2D to false.
            None => {
                self.matrix.set(Matrix4 {
                    elements: [[f64::NAN; 4]; 4],
                });
                self.is_2d.set(false);
            }
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-setmatrixvalue, from step 2 on.
    pub(super) fn set_matrix_value_steps(&self, matrix: Matrix4, is_2d_transform: bool) {
        // 2. Set is 2D to the value of 2dTransform.
        self.is_2d.set(is_2d_transform);

        // 3. Set m11 element through m44 element to the element values of matrix in column-major order.
        self.matrix.set(matrix);
    }
}

/// The elements of a Float32Array or a Float64Array, none for one that is detached or out of bounds.
fn typed_array_elements(array: Gc<TypedArrayBase>) -> Vec<f64> {
    let record = make_typed_array_with_buffer_witness_record(&array, ArrayBufferOrder::SeqCst);
    // NB: The offset of a view of a detached buffer, or of one that a resize left out of bounds, can be past the end of
    //     the buffer.
    if is_typed_array_out_of_bounds(&record) {
        return Vec::new();
    }
    let byte_length = typed_array_byte_length(&record) as usize;
    let mut bytes = vec![0; byte_length];
    let byte_offset = array.byte_offset() as usize;
    array
        .viewed_array_buffer()
        .with_data_block(|data_block| data_block.copy_to(byte_offset, &mut bytes));
    if array.kind() == TypedArrayKind::Float32Array {
        let (elements, _) = bytes.as_chunks::<4>();
        elements
            .iter()
            .map(|element| f64::from(f32::from_ne_bytes(*element)))
            .collect()
    } else {
        let (elements, _) = bytes.as_chunks::<8>();
        elements.iter().map(|element| f64::from_ne_bytes(*element)).collect()
    }
}

/// A new typed array of the kind in the realm of the call context, which holds the bytes.
fn create_typed_array(cx: &CallContext, kind: TypedArrayKind, bytes: &[u8]) -> WebIdlResult<Gc<TypedArrayBase>> {
    let buffer = ArrayBuffer::create(cx.vm(), cx.realm(), bytes.len(), ArrayBufferShared::No)?;
    buffer.with_data_block(|data_block| data_block.overwrite(0, bytes));
    let element_count = 16;
    Ok(create_typed_array_on_buffer(
        cx.vm(),
        cx.realm(),
        kind,
        element_count,
        buffer,
    ))
}

/// https://drafts.fxtf.org/geometry/#parse-a-string-into-an-abstract-matrix, which throws a "SyntaxError"
/// DOMException for failure.
pub(crate) fn parse_transform_list(transform_list: &Utf16String) -> WebIdlResult<(Matrix4, bool)> {
    let mut elements = [0.0; 16];
    let mut is_2d_transform = true;
    // SAFETY: The string is lent for the call.
    let parsed = unsafe {
        web_dom_matrix_parse_transform_list(transform_list.raw_identity(), &mut elements, &mut is_2d_transform)
    };
    if !parsed {
        return Err(Exception::syntax_error(Utf16String::from_ascii(
            b"Failed to parse CSS transform string.",
        )));
    }
    Ok((Matrix4::from_column_major(elements), is_2d_transform))
}

impl Serializable for DOMMatrixReadOnly {
    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn serialization_steps(
        &self,
        _: &mut CallContext,
        serialized: &mut SerializationWriter,
        _for_storage: bool,
    ) -> WebIdlResult<()> {
        serialize_matrix(self, serialized);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn deserialization_steps(_: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        deserialize_matrix(serialized)
    }
}

/// The serialization steps for DOMMatrixReadOnly and DOMMatrix, https://drafts.fxtf.org/geometry/#structured-serialization
pub(crate) fn serialize_matrix(matrix: &DOMMatrixReadOnly, serialized: &mut SerializationWriter) {
    // NB: Is 2D is serialized first, so that the deserialization steps know how many elements follow.
    serialized.write_bool(matrix.is_2d.get());

    let elements = matrix.matrix.get();
    let m = |row, column| elements.element(row, column);

    // 1. If value’s is 2D is true:
    if matrix.is_2d.get() {
        // 1. Set serialized.[[M11]] to value’s m11 element.
        // 2. Set serialized.[[M12]] to value’s m12 element.
        // 3. Set serialized.[[M21]] to value’s m21 element.
        // 4. Set serialized.[[M22]] to value’s m22 element.
        // 5. Set serialized.[[M41]] to value’s m41 element.
        // 6. Set serialized.[[M42]] to value’s m42 element.
        // 7. Set serialized.[[Is2D]] to true.
        for element in [m(1, 1), m(1, 2), m(2, 1), m(2, 2), m(4, 1), m(4, 2)] {
            serialized.write_f64(element);
        }
    }
    // 2. Otherwise:
    else {
        // 1. Set serialized.[[M11]] to value’s m11 element.
        // 2. Set serialized.[[M12]] to value’s m12 element.
        // 3. Set serialized.[[M13]] to value’s m13 element.
        // 4. Set serialized.[[M14]] to value’s m14 element.
        // 5. Set serialized.[[M21]] to value’s m21 element.
        // 6. Set serialized.[[M22]] to value’s m22 element.
        // 7. Set serialized.[[M23]] to value’s m23 element.
        // 8. Set serialized.[[M24]] to value’s m24 element.
        // 9. Set serialized.[[M31]] to value’s m31 element.
        // 10. Set serialized.[[M32]] to value’s m32 element.
        // 11. Set serialized.[[M33]] to value’s m33 element.
        // 12. Set serialized.[[M34]] to value’s m34 element.
        // 13. Set serialized.[[M41]] to value’s m41 element.
        // 14. Set serialized.[[M42]] to value’s m42 element.
        // 15. Set serialized.[[M43]] to value’s m43 element.
        // 16. Set serialized.[[M44]] to value’s m44 element.
        // 17. Set serialized.[[Is2D]] to false.
        for element in elements.to_column_major() {
            serialized.write_f64(element);
        }
    }
}

/// The deserialization steps of DOMMatrixReadOnly and DOMMatrix, which give the matrix of the new object.
pub(crate) fn deserialize_matrix(serialized: &mut SerializationReader) -> WebIdlResult<DOMMatrixReadOnly> {
    let is_2d = serialized.read_bool()?;

    // 1. If serialized.[[Is2D]] is true:
    if is_2d {
        // 1. Set value’s m11 element to serialized.[[M11]].
        // 2. Set value’s m12 element to serialized.[[M12]].
        // 3. Set value’s m13 element to 0.
        // 4. Set value’s m14 element to 0.
        // 5. Set value’s m21 element to serialized.[[M21]].
        // 6. Set value’s m22 element to serialized.[[M22]].
        // 7. Set value’s m23 element to 0.
        // 8. Set value’s m24 element to 0.
        // 9. Set value’s m31 element to 0.
        // 10. Set value’s m32 element to 0.
        // 11. Set value’s m33 element to 1.
        // 12. Set value’s m34 element to 0.
        // 13. Set value’s m41 element to serialized.[[M41]].
        // 14. Set value’s m42 element to serialized.[[M42]].
        // 15. Set value’s m43 element to 0.
        // 16. Set value’s m44 element to 1.
        // 17. Set value’s is 2D to true.
        let mut elements = [0.0; 6];
        for element in &mut elements {
            *element = serialized.read_f64()?;
        }
        return Ok(DOMMatrixReadOnly::create_2d_matrix(elements));
    }

    // 2. Otherwise:
    // 1. Set value’s m11 element to serialized.[[M11]].
    // 2. Set value’s m12 element to serialized.[[M12]].
    // 3. Set value’s m13 element to serialized.[[M13]].
    // 4. Set value’s m14 element to serialized.[[M14]].
    // 5. Set value’s m21 element to serialized.[[M21]].
    // 6. Set value’s m22 element to serialized.[[M22]].
    // 7. Set value’s m23 element to serialized.[[M23]].
    // 8. Set value’s m24 element to serialized.[[M24]].
    // 9. Set value’s m31 element to serialized.[[M31]].
    // 10. Set value’s m32 element to serialized.[[M32]].
    // 11. Set value’s m33 element to serialized.[[M33]].
    // 12. Set value’s m34 element to serialized.[[M34]].
    // 13. Set value’s m41 element to serialized.[[M41]].
    // 14. Set value’s m42 element to serialized.[[M42]].
    // 15. Set value’s m43 element to serialized.[[M43]].
    // 16. Set value’s m44 element to serialized.[[M44]].
    // 17. Set value’s is 2D to false.
    let mut elements = [0.0; 16];
    for element in &mut elements {
        *element = serialized.read_f64()?;
    }
    Ok(DOMMatrixReadOnly::create_3d_matrix(elements))
}
