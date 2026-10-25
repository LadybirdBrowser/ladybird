/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.fxtf.org/geometry/#dommatrix

use ak::Utf16String;
use libjs_rust::api::{Gc, Trace, TypedArrayBase};

use super::dom_matrix_read_only::{
    DOMMatrixInit, DOMMatrixReadOnly, DOMStringOrUnrestrictedDoubleSequence, deserialize_matrix, parse_transform_list,
    serialize_matrix,
};
use super::matrix::ColumnMajor;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::structured_data::{Serializable, SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DOMMatrix {
    matrix: DOMMatrixReadOnly,
}

impl Wrappable for DOMMatrix {}

impl Inherits<DOMMatrixReadOnly> for DOMMatrix {
    fn parent(&self) -> &DOMMatrixReadOnly {
        &self.matrix
    }
}

impl DOMMatrix {
    pub(crate) fn from_read_only(matrix: DOMMatrixReadOnly) -> Self {
        Self { matrix }
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-dommatrix
    pub(crate) fn construct(
        cx: &mut CallContext,
        init: Option<DOMStringOrUnrestrictedDoubleSequence>,
    ) -> WebIdlResult<Self> {
        Ok(Self::from_read_only(DOMMatrixReadOnly::from_init(cx, init)?))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-frommatrix
    pub(crate) fn from_matrix(cx: &mut CallContext, other: DOMMatrixInit) -> WebIdlResult<Ref<Self>> {
        // The fromMatrix(other) static method on DOMMatrix must create a DOMMatrix from the dictionary other.
        let matrix = Self::from_dictionary(&other)?;
        Ok(allocate(cx.vm(), |_| matrix))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-fromfloat32array
    pub(crate) fn from_float32_array(cx: &mut CallContext, array32: Gc<TypedArrayBase>) -> WebIdlResult<Ref<Self>> {
        let matrix = DOMMatrixReadOnly::from_typed_array(array32)?;
        Ok(allocate(cx.vm(), |_| Self::from_read_only(matrix)))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-fromfloat64array
    pub(crate) fn from_float64_array(cx: &mut CallContext, array64: Gc<TypedArrayBase>) -> WebIdlResult<Ref<Self>> {
        let matrix = DOMMatrixReadOnly::from_typed_array(array64)?;
        Ok(allocate(cx.vm(), |_| Self::from_read_only(matrix)))
    }

    /// https://drafts.fxtf.org/geometry/#create-a-dommatrix-from-the-dictionary
    pub(crate) fn from_dictionary(other: &DOMMatrixInit) -> WebIdlResult<Self> {
        Ok(Self::from_read_only(DOMMatrixReadOnly::from_dictionary(other)?))
    }

    /// Sets the element mRC of the 2D matrix to the value, as the setter of its attribute does.
    fn set_element(&self, row: usize, column: usize, value: f64) {
        let mut matrix = self.matrix.matrix();
        matrix.set_element(row, column, value);
        self.matrix.set_matrix(matrix);
    }

    /// Sets an element outside of the 2D matrix: the element to the new value and, if the new value is not the value
    /// of the identity matrix, is 2D to false.
    fn set_3d_element(&self, row: usize, column: usize, value: f64) {
        self.set_element(row, column, value);
        let identity_value = if row == column { 1.0 } else { 0.0 };
        if value != identity_value {
            self.matrix.set_is_2d(false);
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-a
    pub(crate) fn set_a(&self, cx: &mut CallContext, value: f64) -> WebIdlResult<()> {
        self.set_m11(cx, value)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-b
    pub(crate) fn set_b(&self, cx: &mut CallContext, value: f64) -> WebIdlResult<()> {
        self.set_m12(cx, value)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-c
    pub(crate) fn set_c(&self, cx: &mut CallContext, value: f64) -> WebIdlResult<()> {
        self.set_m21(cx, value)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-d
    pub(crate) fn set_d(&self, cx: &mut CallContext, value: f64) -> WebIdlResult<()> {
        self.set_m22(cx, value)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-e
    pub(crate) fn set_e(&self, cx: &mut CallContext, value: f64) -> WebIdlResult<()> {
        self.set_m41(cx, value)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-f
    pub(crate) fn set_f(&self, cx: &mut CallContext, value: f64) -> WebIdlResult<()> {
        self.set_m42(cx, value)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m11
    pub(crate) fn set_m11(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m11 or the a attribute must set the m11 element to the new value.
        self.set_element(1, 1, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m12
    pub(crate) fn set_m12(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m12 or the b attribute must set the m12 element to the new value.
        self.set_element(1, 2, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m13
    pub(crate) fn set_m13(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m13 attribute must set the m13 element to the new value and, if the
        // new value is not 0 or -0, set is 2D to false.
        self.set_3d_element(1, 3, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m14
    pub(crate) fn set_m14(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m14 attribute must set the m14 element to the new value and, if the
        // new value is not 0 or -0, set is 2D to false.
        self.set_3d_element(1, 4, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m21
    pub(crate) fn set_m21(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m21 or the c attribute must set the m21 element to the new value.
        self.set_element(2, 1, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m22
    pub(crate) fn set_m22(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m22 or the d attribute must set the m22 element to the new value.
        self.set_element(2, 2, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m23
    pub(crate) fn set_m23(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m23 attribute must set the m23 element to the new value and, if the
        // new value is not 0 or -0, set is 2D to false.
        self.set_3d_element(2, 3, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m24
    pub(crate) fn set_m24(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m24 attribute must set the m24 element to the new value and, if the
        // new value is not 0 or -0, set is 2D to false.
        self.set_3d_element(2, 4, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m31
    pub(crate) fn set_m31(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m31 attribute must set the m31 element to the new value and, if the
        // new value is not 0 or -0, set is 2D to false.
        self.set_3d_element(3, 1, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m32
    pub(crate) fn set_m32(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m32 attribute must set the m32 element to the new value and, if the
        // new value is not 0 or -0, set is 2D to false.
        self.set_3d_element(3, 2, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m33
    pub(crate) fn set_m33(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m33 attribute must set the m33 element to the new value and, if the
        // new value is not 1, set is 2D to false.
        self.set_3d_element(3, 3, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m34
    pub(crate) fn set_m34(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m34 attribute must set the m34 element to the new value and, if the
        // new value is not 0 or -0, set is 2D to false.
        self.set_3d_element(3, 4, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m41
    pub(crate) fn set_m41(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m41 or the e attribute must set the m41 element to the new value.
        self.set_element(4, 1, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m42
    pub(crate) fn set_m42(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m42 or the f attribute must set the m42 element to the new value.
        self.set_element(4, 2, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m43
    pub(crate) fn set_m43(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m43 attribute must set the m43 element to the new value and, if the
        // new value is not 0 or -0, set is 2D to false.
        self.set_3d_element(4, 3, value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m44
    pub(crate) fn set_m44(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMMatrix interface, setting the m44 attribute must set the m44 element to the new value and, if the
        // new value is not 1, set is 2D to false.
        self.set_3d_element(4, 4, value);
        Ok(())
    }

    // NB: The mutable transformation methods return the current matrix, so they receive the object that they run on,
    //     which the bindings pass as they pass `&self` to the other methods.

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-multiplyself
    pub(crate) fn multiply_self(
        this: &Ref<Self>,
        _: &mut CallContext,
        other: DOMMatrixInit,
    ) -> WebIdlResult<Ref<Self>> {
        // 1. Let otherObject be the result of invoking create a DOMMatrix from the dictionary other.
        let other_object = Self::from_dictionary(&other)?;

        // 2. The otherObject matrix gets post-multiplied to the current matrix.
        // 3. If is 2D of otherObject is false, set is 2D of the current matrix to false.
        this.matrix.multiply_self_steps(&other_object.matrix);

        // 4. Return the current matrix.
        Ok(*this)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-premultiplyself
    pub(crate) fn pre_multiply_self(
        this: &Ref<Self>,
        _: &mut CallContext,
        other: DOMMatrixInit,
    ) -> WebIdlResult<Ref<Self>> {
        // 1. Let otherObject be the result of invoking create a DOMMatrix from the dictionary other.
        let other_object = Self::from_dictionary(&other)?;

        // 2. The otherObject matrix gets pre-multiplied to the current matrix.
        // 3. If is 2D of otherObject is false, set is 2D of the current matrix to false.
        this.matrix.pre_multiply_self_steps(&other_object.matrix);

        // 4. Return the current matrix.
        Ok(*this)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-translateself
    pub(crate) fn translate_self(
        this: &Ref<Self>,
        _: &mut CallContext,
        tx: f64,
        ty: f64,
        tz: f64,
    ) -> WebIdlResult<Ref<Self>> {
        this.matrix.translate_self_steps(tx, ty, tz);

        // 3. Return the current matrix.
        Ok(*this)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-scaleself
    #[allow(clippy::too_many_arguments, reason = "the operation has six arguments")]
    pub(crate) fn scale_self(
        this: &Ref<Self>,
        _: &mut CallContext,
        scale_x: f64,
        scale_y: Option<f64>,
        scale_z: f64,
        origin_x: f64,
        origin_y: f64,
        origin_z: f64,
    ) -> WebIdlResult<Ref<Self>> {
        this.matrix
            .scale_self_steps(scale_x, scale_y, scale_z, origin_x, origin_y, origin_z);

        // 7. Return the current matrix.
        Ok(*this)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-scale3dself
    pub(crate) fn scale3d_self(
        this: &Ref<Self>,
        _: &mut CallContext,
        scale: f64,
        origin_x: f64,
        origin_y: f64,
        origin_z: f64,
    ) -> WebIdlResult<Ref<Self>> {
        this.matrix.scale3d_self_steps(scale, origin_x, origin_y, origin_z);

        // 5. Return the current matrix.
        Ok(*this)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-rotateself
    pub(crate) fn rotate_self(
        this: &Ref<Self>,
        _: &mut CallContext,
        rot_x: f64,
        rot_y: Option<f64>,
        rot_z: Option<f64>,
    ) -> WebIdlResult<Ref<Self>> {
        this.matrix.rotate_self_steps(rot_x, rot_y, rot_z);

        // 8. Return the current matrix.
        Ok(*this)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-rotatefromvectorself
    pub(crate) fn rotate_from_vector_self(
        this: &Ref<Self>,
        _: &mut CallContext,
        x: f64,
        y: f64,
    ) -> WebIdlResult<Ref<Self>> {
        this.matrix.rotate_from_vector_self_steps(x, y);

        // 2. Return the current matrix.
        Ok(*this)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-rotateaxisangleself
    pub(crate) fn rotate_axis_angle_self(
        this: &Ref<Self>,
        _: &mut CallContext,
        x: f64,
        y: f64,
        z: f64,
        angle: f64,
    ) -> WebIdlResult<Ref<Self>> {
        this.matrix.rotate_axis_angle_self_steps(x, y, z, angle);

        // 3. Return the current matrix.
        Ok(*this)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-skewxself
    pub(crate) fn skew_x_self(this: &Ref<Self>, _: &mut CallContext, sx: f64) -> WebIdlResult<Ref<Self>> {
        this.matrix.skew_x_self_steps(sx);

        // 2. Return the current matrix.
        Ok(*this)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-skewyself
    pub(crate) fn skew_y_self(this: &Ref<Self>, _: &mut CallContext, sy: f64) -> WebIdlResult<Ref<Self>> {
        this.matrix.skew_y_self_steps(sy);

        // 2. Return the current matrix.
        Ok(*this)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-invertself
    pub(crate) fn invert_self(this: &Ref<Self>, _: &mut CallContext) -> WebIdlResult<Ref<Self>> {
        this.matrix.invert_self_steps();

        // 3. Return the current matrix.
        Ok(*this)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dommatrix-setmatrixvalue
    pub(crate) fn set_matrix_value(
        this: &Ref<Self>,
        _: &mut CallContext,
        transform_list: Utf16String,
    ) -> WebIdlResult<Ref<Self>> {
        // 1. Parse transformList into an abstract matrix, and let matrix and 2dTransform be the result. If the result
        //    is failure, then throw a "SyntaxError" DOMException.
        let (matrix, is_2d_transform) = parse_transform_list(&transform_list)?;

        // 2. Set is 2D to the value of 2dTransform.
        // 3. Set m11 element through m44 element to the element values of matrix in column-major order.
        this.matrix.set_matrix_value_steps(matrix, is_2d_transform);

        // 4. Return the current matrix.
        Ok(*this)
    }
}

impl Serializable for DOMMatrix {
    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn serialization_steps(
        &self,
        _: &mut CallContext,
        serialized: &mut SerializationWriter,
        _for_storage: bool,
    ) -> WebIdlResult<()> {
        serialize_matrix(&self.matrix, serialized);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn deserialization_steps(_: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        Ok(Self::from_read_only(deserialize_matrix(serialized)?))
    }
}
