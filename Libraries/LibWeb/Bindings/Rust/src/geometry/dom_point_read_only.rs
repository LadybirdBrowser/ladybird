/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.fxtf.org/geometry/#dompointreadonly

use core::cell::Cell;

use libjs_rust::api::Trace;

pub(crate) use super::dictionaries::DOMPointInit;
use super::dom_matrix::DOMMatrix;
use super::dom_matrix_read_only::DOMMatrixInit;
use super::dom_point::DOMPoint;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::structured_data::{Serializable, SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DOMPointReadOnly {
    #[gc(untraced)]
    x: Cell<f64>,
    #[gc(untraced)]
    y: Cell<f64>,
    #[gc(untraced)]
    z: Cell<f64>,
    #[gc(untraced)]
    w: Cell<f64>,
}

impl Wrappable for DOMPointReadOnly {}

impl DOMPointReadOnly {
    pub(crate) fn new(x: f64, y: f64, z: f64, w: f64) -> Self {
        Self {
            x: Cell::new(x),
            y: Cell::new(y),
            z: Cell::new(z),
            w: Cell::new(w),
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-dompointreadonly-dompointreadonly
    pub(crate) fn construct(_: &mut CallContext, x: f64, y: f64, z: f64, w: f64) -> WebIdlResult<Self> {
        // 1. Let point be a new DOMPointReadOnly or DOMPoint object as appropriate.
        // 2. Set point’s variables x coordinate to x, y coordinate to y, z coordinate to z and w perspective to w.
        // 3. Return point.
        Ok(Self::new(x, y, z, w))
    }

    /// https://drafts.fxtf.org/geometry/#dom-dompointreadonly-frompoint
    pub(crate) fn from_point(cx: &mut CallContext, other: DOMPointInit) -> WebIdlResult<Ref<Self>> {
        // The fromPoint(other) static method on DOMPointReadOnly must create a DOMPointReadOnly from the dictionary
        // other.
        Ok(allocate(cx.vm(), |_| Self::from_dictionary(&other)))
    }

    /// https://drafts.fxtf.org/geometry/#create-a-dompointreadonly-from-the-dictionary
    pub(crate) fn from_dictionary(other: &DOMPointInit) -> Self {
        // 1. Let point be a new DOMPointReadOnly or DOMPoint as appropriate.
        // 2. Set point’s variables x coordinate to other’s x dictionary member, y coordinate to other’s y dictionary
        //    member, z coordinate to other’s z dictionary member and w perspective to other’s w dictionary member.
        // 3. Return point.
        Self::new(other.x, other.y, other.z, other.w)
    }

    /// https://drafts.fxtf.org/geometry/#dom-dompointreadonly-x
    pub(crate) fn x(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.x.get())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dompointreadonly-y
    pub(crate) fn y(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.y.get())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dompointreadonly-z
    pub(crate) fn z(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.z.get())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dompointreadonly-w
    pub(crate) fn w(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.w.get())
    }

    /// The coordinates x, y, z and w, in that order.
    pub(crate) fn coordinates(&self) -> [f64; 4] {
        [self.x.get(), self.y.get(), self.z.get(), self.w.get()]
    }

    pub(super) fn set_x_coordinate(&self, value: f64) {
        self.x.set(value);
    }

    pub(super) fn set_y_coordinate(&self, value: f64) {
        self.y.set(value);
    }

    pub(super) fn set_z_coordinate(&self, value: f64) {
        self.z.set(value);
    }

    pub(super) fn set_w_perspective(&self, value: f64) {
        self.w.set(value);
    }

    /// https://drafts.fxtf.org/geometry/#dom-dompointreadonly-matrixtransform
    pub(crate) fn matrix_transform(&self, cx: &mut CallContext, matrix: DOMMatrixInit) -> WebIdlResult<Ref<DOMPoint>> {
        // 1. Let matrixObject be the result of invoking create a DOMMatrix from the dictionary matrix.
        let matrix_object = DOMMatrix::from_dictionary(&matrix)?;

        // 2. Return the result of invoking transform a point with a matrix, given the current point and matrixObject.
        //    The current point does not get modified.
        Ok(allocate(cx.vm(), |_| {
            matrix_object.parent().transform_point_steps(self)
        }))
    }
}

impl Serializable for DOMPointReadOnly {
    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn serialization_steps(
        &self,
        _: &mut CallContext,
        serialized: &mut SerializationWriter,
        _for_storage: bool,
    ) -> WebIdlResult<()> {
        serialize_point(self, serialized);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn deserialization_steps(_: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        deserialize_point(serialized)
    }
}

/// The serialization steps for DOMPointReadOnly and DOMPoint, https://drafts.fxtf.org/geometry/#structured-serialization
pub(crate) fn serialize_point(point: &DOMPointReadOnly, serialized: &mut SerializationWriter) {
    // 1. Set serialized.[[X]] to value’s x coordinate.
    serialized.write_f64(point.x.get());

    // 2. Set serialized.[[Y]] to value’s y coordinate.
    serialized.write_f64(point.y.get());

    // 3. Set serialized.[[Z]] to value’s z coordinate.
    serialized.write_f64(point.z.get());

    // 4. Set serialized.[[W]] to value’s w perspective.
    serialized.write_f64(point.w.get());
}

/// The deserialization steps of DOMPointReadOnly and DOMPoint, which give the coordinates of the new point.
pub(crate) fn deserialize_point(serialized: &mut SerializationReader) -> WebIdlResult<DOMPointReadOnly> {
    // 1. Set value’s x coordinate to serialized.[[X]].
    let x = serialized.read_f64()?;

    // 2. Set value’s y coordinate to serialized.[[Y]].
    let y = serialized.read_f64()?;

    // 3. Set value’s z coordinate to serialized.[[Z]].
    let z = serialized.read_f64()?;

    // 4. Set value’s w perspective to serialized.[[W]].
    let w = serialized.read_f64()?;

    Ok(DOMPointReadOnly::new(x, y, z, w))
}
