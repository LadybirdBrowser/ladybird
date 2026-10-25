/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/1.0/#5.11

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WebGLActiveInfo {
    #[gc(untraced)]
    name: Utf16String,
    #[gc(untraced)]
    r#type: u32,
    #[gc(untraced)]
    size: i32,
}

impl Wrappable for WebGLActiveInfo {}

impl WebGLActiveInfo {
    pub(crate) fn create(vm: &Vm, name: Utf16String, r#type: u32, size: i32) -> Ref<Self> {
        allocate(vm, |_| Self { name, r#type, size })
    }

    pub(crate) fn size(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        Ok(self.size)
    }

    pub(crate) fn r#type(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.r#type)
    }

    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.name.clone())
    }
}
