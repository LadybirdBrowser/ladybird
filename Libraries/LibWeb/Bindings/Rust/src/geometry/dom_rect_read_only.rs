/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.fxtf.org/geometry/#domrectreadonly

use core::cell::Cell;

use libjs_rust::api::Trace;

pub(crate) use super::dictionaries::DOMRectInit;
use super::{nan_safe_maximum, nan_safe_minimum};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::structured_data::{Serializable, SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DOMRectReadOnly {
    #[gc(untraced)]
    x: Cell<f64>,
    #[gc(untraced)]
    y: Cell<f64>,
    #[gc(untraced)]
    width: Cell<f64>,
    #[gc(untraced)]
    height: Cell<f64>,
}

impl Wrappable for DOMRectReadOnly {}

impl DOMRectReadOnly {
    pub(crate) fn new(x: f64, y: f64, width: f64, height: f64) -> Self {
        Self {
            x: Cell::new(x),
            y: Cell::new(y),
            width: Cell::new(width),
            height: Cell::new(height),
        }
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrectreadonly-domrectreadonly
    pub(crate) fn construct(_: &mut CallContext, x: f64, y: f64, width: f64, height: f64) -> WebIdlResult<Self> {
        // 1. Let rect be a new DOMRectReadOnly or DOMRect object as appropriate.
        // 2. Set rect’s variables x coordinate to x, y coordinate to y, width dimension to width and height dimension
        //    to height.
        // 3. Return rect.
        Ok(Self::new(x, y, width, height))
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrectreadonly-fromrect
    pub(crate) fn from_rect(cx: &mut CallContext, other: DOMRectInit) -> WebIdlResult<Ref<Self>> {
        // The fromRect(other) static method on DOMRectReadOnly must create a DOMRectReadOnly from the dictionary other.
        Ok(allocate(cx.vm(), |_| Self::from_dictionary(&other)))
    }

    /// https://drafts.fxtf.org/geometry/#create-a-domrectreadonly-from-the-dictionary
    pub(crate) fn from_dictionary(other: &DOMRectInit) -> Self {
        // 1. Let rect be a new DOMRectReadOnly or DOMRect as appropriate.
        // 2. Set rect’s variables x coordinate to other’s x dictionary member, y coordinate to other’s y dictionary
        //    member, width dimension to other’s width dictionary member and height dimension to other’s height
        //    dictionary member.
        // 3. Return rect.
        Self::new(other.x, other.y, other.width, other.height)
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrectreadonly-x
    pub(crate) fn x(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.x.get())
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrectreadonly-y
    pub(crate) fn y(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.y.get())
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrectreadonly-width
    pub(crate) fn width(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.width.get())
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrectreadonly-height
    pub(crate) fn height(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.height.get())
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrectreadonly-top
    pub(crate) fn top(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The top attribute, on getting, must return the NaN-safe minimum of the y coordinate and the sum of the y
        // coordinate and the height dimension.
        Ok(nan_safe_minimum(&[self.y.get(), self.y.get() + self.height.get()]))
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrectreadonly-right
    pub(crate) fn right(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The right attribute, on getting, must return the NaN-safe maximum of the x coordinate and the sum of the x
        // coordinate and the width dimension.
        Ok(nan_safe_maximum(&[self.x.get(), self.x.get() + self.width.get()]))
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrectreadonly-bottom
    pub(crate) fn bottom(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The bottom attribute, on getting, must return the NaN-safe maximum of the y coordinate and the sum of the y
        // coordinate and the height dimension.
        Ok(nan_safe_maximum(&[self.y.get(), self.y.get() + self.height.get()]))
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrectreadonly-left
    pub(crate) fn left(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The left attribute, on getting, must return the NaN-safe minimum of the x coordinate and the sum of the x
        // coordinate and the width dimension.
        Ok(nan_safe_minimum(&[self.x.get(), self.x.get() + self.width.get()]))
    }

    pub(super) fn set_x_coordinate(&self, value: f64) {
        self.x.set(value);
    }

    pub(super) fn set_y_coordinate(&self, value: f64) {
        self.y.set(value);
    }

    pub(super) fn set_width_dimension(&self, value: f64) {
        self.width.set(value);
    }

    pub(super) fn set_height_dimension(&self, value: f64) {
        self.height.set(value);
    }
}

impl Serializable for DOMRectReadOnly {
    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn serialization_steps(
        &self,
        _: &mut CallContext,
        serialized: &mut SerializationWriter,
        _for_storage: bool,
    ) -> WebIdlResult<()> {
        serialize_rect(self, serialized);
        Ok(())
    }

    /// https://drafts.fxtf.org/geometry/#structured-serialization
    fn deserialization_steps(_: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        deserialize_rect(serialized)
    }
}

/// The serialization steps for DOMRectReadOnly and DOMRect, https://drafts.fxtf.org/geometry/#structured-serialization
pub(crate) fn serialize_rect(rect: &DOMRectReadOnly, serialized: &mut SerializationWriter) {
    // 1. Set serialized.[[X]] to value’s x coordinate.
    serialized.write_f64(rect.x.get());

    // 2. Set serialized.[[Y]] to value’s y coordinate.
    serialized.write_f64(rect.y.get());

    // 3. Set serialized.[[Width]] to value’s width dimension.
    serialized.write_f64(rect.width.get());

    // 4. Set serialized.[[Height]] to value’s height dimension.
    serialized.write_f64(rect.height.get());
}

/// The deserialization steps of DOMRectReadOnly and DOMRect, which give the dimensions of the new rectangle.
pub(crate) fn deserialize_rect(serialized: &mut SerializationReader) -> WebIdlResult<DOMRectReadOnly> {
    // 1. Set value’s x coordinate to serialized.[[X]].
    let x = serialized.read_f64()?;

    // 2. Set value’s y coordinate to serialized.[[Y]].
    let y = serialized.read_f64()?;

    // 3. Set value’s width dimension to serialized.[[Width]].
    let width = serialized.read_f64()?;

    // 4. Set value’s height dimension to serialized.[[Height]].
    let height = serialized.read_f64()?;

    Ok(DOMRectReadOnly::new(x, y, width, height))
}
