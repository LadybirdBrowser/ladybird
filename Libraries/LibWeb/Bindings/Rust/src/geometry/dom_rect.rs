/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.fxtf.org/geometry/#domrect

use libjs_rust::api::Trace;

use super::dom_rect_read_only::{DOMRectInit, DOMRectReadOnly, deserialize_rect, serialize_rect};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::structured_data::{Serializable, SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DOMRect {
    rect: DOMRectReadOnly,
}

impl Wrappable for DOMRect {}

impl Inherits<DOMRectReadOnly> for DOMRect {
    fn parent(&self) -> &DOMRectReadOnly {
        &self.rect
    }
}

impl DOMRect {
    pub(crate) fn new(x: f64, y: f64, width: f64, height: f64) -> Self {
        Self {
            rect: DOMRectReadOnly::new(x, y, width, height),
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrect-domrect
    pub(crate) fn construct(cx: &mut CallContext, x: f64, y: f64, width: f64, height: f64) -> WebIdlResult<Self> {
        Ok(Self {
            rect: DOMRectReadOnly::construct(cx, x, y, width, height)?,
        })
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrect-fromrect
    pub(crate) fn from_rect(cx: &mut CallContext, other: DOMRectInit) -> WebIdlResult<Ref<Self>> {
        // The fromRect(other) static method on DOMRect must create a DOMRect from the dictionary other.
        Ok(allocate(cx.vm(), |_| Self {
            rect: DOMRectReadOnly::from_dictionary(&other),
        }))
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrect-x
    pub(crate) fn set_x(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMRect interface, setting the x attribute must set the x coordinate to the new value.
        self.rect.set_x_coordinate(value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrect-y
    pub(crate) fn set_y(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMRect interface, setting the y attribute must set the y coordinate to the new value.
        self.rect.set_y_coordinate(value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrect-width
    pub(crate) fn set_width(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMRect interface, setting the width attribute must set the width dimension to the new value.
        self.rect.set_width_dimension(value);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrect-height
    pub(crate) fn set_height(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // For the DOMRect interface, setting the height attribute must set the height dimension value to the new
        // value.
        self.rect.set_height_dimension(value);
        Ok(())
    }
}

impl Serializable for DOMRect {
    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn serialization_steps(
        &self,
        _: &mut CallContext,
        serialized: &mut SerializationWriter,
        _for_storage: bool,
    ) -> WebIdlResult<()> {
        serialize_rect(&self.rect, serialized);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn deserialization_steps(_: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        Ok(Self {
            rect: deserialize_rect(serialized)?,
        })
    }
}
