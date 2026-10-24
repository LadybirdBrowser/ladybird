/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.fxtf.org/geometry/#dompoint

use libjs_rust::api::Trace;

use super::dom_point_read_only::{DOMPointInit, DOMPointReadOnly, deserialize_point, serialize_point};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::structured_data::{Serializable, SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DOMPoint {
    point: DOMPointReadOnly,
}

impl Wrappable for DOMPoint {}

impl Inherits<DOMPointReadOnly> for DOMPoint {
    fn parent(&self) -> &DOMPointReadOnly {
        &self.point
    }
}

impl DOMPoint {
    pub(crate) fn new(x: f64, y: f64, z: f64, w: f64) -> Self {
        Self {
            point: DOMPointReadOnly::new(x, y, z, w),
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-dompoint-dompoint
    pub(crate) fn construct(cx: &mut CallContext, x: f64, y: f64, z: f64, w: f64) -> WebIdlResult<Self> {
        Ok(Self {
            point: DOMPointReadOnly::construct(cx, x, y, z, w)?,
        })
    }

    /// https://drafts.fxtf.org/geometry/#dom-dompoint-frompoint
    pub(crate) fn from_point(cx: &mut CallContext, other: DOMPointInit) -> WebIdlResult<Ref<Self>> {
        // The fromPoint(other) static method on DOMPoint must create a DOMPoint from the dictionary other.
        Ok(allocate(cx.vm(), Self::from_dictionary(&other)))
    }

    /// https://drafts.fxtf.org/geometry/#create-a-dompoint-from-the-dictionary
    pub(crate) fn from_dictionary(other: &DOMPointInit) -> Self {
        Self {
            point: DOMPointReadOnly::from_dictionary(other),
        }
    }

    /// Sets the x and y coordinates, for an SVGPointList that synchronizes its points with a reflected attribute.
    pub(crate) fn set_x_and_y(&self, x: f64, y: f64) {
        self.point.set_x_coordinate(x);
        self.point.set_y_coordinate(y);
    }

    /// https://drafts.fxtf.org/geometry/#dom-dompointreadonly-x
    pub(crate) fn set_x(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMPoint interface, setting the x attribute must set the x coordinate to the new value.
        self.point.set_x_coordinate(value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dompointreadonly-y
    pub(crate) fn set_y(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMPoint interface, setting the y attribute must set the y coordinate to the new value.
        self.point.set_y_coordinate(value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dompointreadonly-z
    pub(crate) fn set_z(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMPoint interface, setting the z attribute must set the z coordinate to the new value.
        self.point.set_z_coordinate(value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-dompointreadonly-w
    pub(crate) fn set_w(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMPoint interface, setting the w attribute must set the w perspective to the new value.
        self.point.set_w_perspective(value);
        Ok(())
    }
}

impl Serializable for DOMPoint {
    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn serialization_steps(
        &self,
        _: &mut CallContext,
        serialized: &mut SerializationWriter,
        _for_storage: bool,
    ) -> WebIdlResult<()> {
        serialize_point(&self.point, serialized);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn deserialization_steps(_: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        Ok(Self {
            point: deserialize_point(serialized)?,
        })
    }
}
