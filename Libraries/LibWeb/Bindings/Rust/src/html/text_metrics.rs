/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#textmetrics

use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

/// The measurements of a text, in CSS pixels, as the members of TextMetrics describe them.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct TextMeasurements {
    // x-direction
    pub(crate) width: f64,
    pub(crate) actual_bounding_box_left: f64,
    pub(crate) actual_bounding_box_right: f64,

    // y-direction
    pub(crate) font_bounding_box_ascent: f64,
    pub(crate) font_bounding_box_descent: f64,
    pub(crate) actual_bounding_box_ascent: f64,
    pub(crate) actual_bounding_box_descent: f64,
    pub(crate) em_height_ascent: f64,
    pub(crate) em_height_descent: f64,
    pub(crate) hanging_baseline: f64,
    pub(crate) alphabetic_baseline: f64,
    pub(crate) ideographic_baseline: f64,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TextMetrics {
    #[gc(untraced)]
    measurements: TextMeasurements,
}

impl Wrappable for TextMetrics {}

impl TextMetrics {
    pub(crate) fn create(vm: &Vm, measurements: TextMeasurements) -> Ref<Self> {
        allocate(vm, |_| Self { measurements })
    }

    pub(crate) fn width(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.measurements.width)
    }

    pub(crate) fn actual_bounding_box_left(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.measurements.actual_bounding_box_left)
    }

    pub(crate) fn actual_bounding_box_right(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.measurements.actual_bounding_box_right)
    }

    pub(crate) fn font_bounding_box_ascent(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.measurements.font_bounding_box_ascent)
    }

    pub(crate) fn font_bounding_box_descent(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.measurements.font_bounding_box_descent)
    }

    pub(crate) fn actual_bounding_box_ascent(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.measurements.actual_bounding_box_ascent)
    }

    pub(crate) fn actual_bounding_box_descent(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.measurements.actual_bounding_box_descent)
    }

    pub(crate) fn em_height_ascent(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.measurements.em_height_ascent)
    }

    pub(crate) fn em_height_descent(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.measurements.em_height_descent)
    }

    pub(crate) fn hanging_baseline(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.measurements.hanging_baseline)
    }

    pub(crate) fn alphabetic_baseline(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.measurements.alphabetic_baseline)
    }

    pub(crate) fn ideographic_baseline(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.measurements.ideographic_baseline)
    }
}
