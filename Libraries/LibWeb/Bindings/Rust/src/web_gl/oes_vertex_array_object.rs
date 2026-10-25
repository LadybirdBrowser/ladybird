/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/extensions/OES_vertex_array_object/

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use super::state::VertexArrayBinding;
use super::state::{ContextRef, ContextState};
use super::web_gl_vertex_array_object_oes::WebGLVertexArrayObjectOES;
use crate::bindings::interface::Inherits;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct OESVertexArrayObject {
    context: ContextRef,
}

impl Wrappable for OESVertexArrayObject {
    fn relevant_realm(&self, _vm: &Vm) -> Option<Gc<Realm>> {
        self.context.state().canvas().relevant_realm()
    }
}

impl OESVertexArrayObject {
    pub(crate) fn create(vm: &Vm, context: &ContextState) -> Ref<Self> {
        allocate(vm, |_| Self {
            context: context.context_ref(),
        })
    }

    fn is_bound(&self, array_object: Ref<WebGLVertexArrayObjectOES>) -> bool {
        matches!(
            self.context.state().current_vertex_array.get(),
            Some(VertexArrayBinding::Oes(bound)) if bound.as_ptr() == array_object.as_ptr()
        )
    }

    pub(crate) fn create_vertex_array_oes(&self, cx: &mut CallContext) -> WebIdlResult<Ref<WebGLVertexArrayObjectOES>> {
        let context = self.context.state();
        let mut handle = [0];
        context.proxy.gen_vertex_arrays_oes(&mut handle);
        Ok(WebGLVertexArrayObjectOES::create(cx.vm(), context, handle[0]))
    }

    pub(crate) fn delete_vertex_array_oes(
        &self,
        _: &mut CallContext,
        array_object: Option<Ref<WebGLVertexArrayObjectOES>>,
    ) -> WebIdlResult<()> {
        let context = self.context.state();
        let Some(array_object) = array_object else {
            return Ok(());
        };
        // FIXME: An object of another context is an INVALID_OPERATION error.
        let Ok(Some(handle)) = array_object.parent().handle_for_deletion(context) else {
            return Ok(());
        };
        context.proxy.delete_vertex_arrays_oes(1, &[handle]);
        if self.is_bound(array_object) {
            context.current_vertex_array.set(None);
        }
        Ok(())
    }

    pub(crate) fn is_vertex_array_oes(
        &self,
        _: &mut CallContext,
        array_object: Option<Ref<WebGLVertexArrayObjectOES>>,
    ) -> WebIdlResult<bool> {
        let context = self.context.state();
        let Some(array_object) = array_object else {
            return Ok(false);
        };
        let Ok(Some(handle)) = array_object.parent().handle_for_query(context) else {
            return Ok(false);
        };
        Ok(context.proxy.is_vertex_array_oes(handle))
    }

    pub(crate) fn bind_vertex_array_oes(
        &self,
        _: &mut CallContext,
        array_object: Option<Ref<WebGLVertexArrayObjectOES>>,
    ) -> WebIdlResult<()> {
        let context = self.context.state();
        let mut handle = 0;
        if let Some(array_object) = array_object {
            // FIXME: An object that the context cannot use is an INVALID_OPERATION error.
            let Ok(array_object_handle) = array_object.parent().handle(context) else {
                return Ok(());
            };
            handle = array_object_handle;
        }
        context.proxy.bind_vertex_array_oes(handle);
        context
            .current_vertex_array
            .set(array_object.map(VertexArrayBinding::Oes));
        Ok(())
    }
}
