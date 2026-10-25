/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/extensions/WEBGL_draw_buffers/

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use super::constants::GL_INVALID_VALUE;
use super::proxy::recorder::bytes_of;
use super::state::{ContextRef, ContextState};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WebGLDrawBuffers {
    context: ContextRef,
}

impl Wrappable for WebGLDrawBuffers {
    fn relevant_realm(&self, _vm: &Vm) -> Option<Gc<Realm>> {
        self.context.state().canvas().relevant_realm()
    }
}

impl WebGLDrawBuffers {
    pub(crate) fn create(vm: &Vm, context: &ContextState) -> Ref<Self> {
        allocate(vm, |_| Self {
            context: context.context_ref(),
        })
    }

    pub(crate) fn draw_buffers_webgl(&self, _: &mut CallContext, buffers: Vec<u32>) -> WebIdlResult<()> {
        let context = self.context.state();
        let Ok(buffer_count) = i32::try_from(buffers.len()) else {
            context.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        context.proxy.draw_buffers_ext(buffer_count, bytes_of(&buffers));
        Ok(())
    }
}
