/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/1.0/#5.12

use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WebGLShaderPrecisionFormat {
    #[gc(untraced)]
    range_min: i32,
    #[gc(untraced)]
    range_max: i32,
    #[gc(untraced)]
    precision: i32,
}

impl Wrappable for WebGLShaderPrecisionFormat {}

impl WebGLShaderPrecisionFormat {
    pub(crate) fn create(vm: &Vm, range_min: i32, range_max: i32, precision: i32) -> Ref<Self> {
        allocate(vm, |_| Self {
            range_min,
            range_max,
            precision,
        })
    }

    pub(crate) fn range_min(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        Ok(self.range_min)
    }

    pub(crate) fn range_max(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        Ok(self.range_max)
    }

    pub(crate) fn precision(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        Ok(self.precision)
    }
}
