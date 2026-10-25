/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/extensions/ANGLE_instanced_arrays/

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use super::state::{ContextRef, ContextState};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ANGLEInstancedArrays {
    context: ContextRef,
}

impl Wrappable for ANGLEInstancedArrays {
    fn relevant_realm(&self, _vm: &Vm) -> Option<Gc<Realm>> {
        self.context.state().canvas().relevant_realm()
    }
}

impl ANGLEInstancedArrays {
    pub(crate) fn create(vm: &Vm, context: &ContextState) -> Ref<Self> {
        allocate(vm, |_| Self {
            context: context.context_ref(),
        })
    }

    pub(crate) fn draw_arrays_instanced_angle(
        &self,
        _: &mut CallContext,
        mode: u32,
        first: i32,
        count: i32,
        primcount: i32,
    ) -> WebIdlResult<()> {
        self.context
            .state()
            .proxy
            .draw_arrays_instanced_angle(mode, first, count, primcount);
        Ok(())
    }

    pub(crate) fn draw_elements_instanced_angle(
        &self,
        _: &mut CallContext,
        mode: u32,
        count: i32,
        r#type: u32,
        offset: i64,
        primcount: i32,
    ) -> WebIdlResult<()> {
        self.context
            .state()
            .proxy
            .draw_elements_instanced_angle(mode, count, r#type, offset, primcount);
        Ok(())
    }

    pub(crate) fn vertex_attrib_divisor_angle(
        &self,
        _: &mut CallContext,
        index: u32,
        divisor: u32,
    ) -> WebIdlResult<()> {
        self.context.state().proxy.vertex_attrib_divisor_angle(index, divisor);
        Ok(())
    }
}
