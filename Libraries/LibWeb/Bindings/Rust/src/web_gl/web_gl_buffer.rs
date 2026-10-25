/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/1.0/#5.4

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use super::buffer_kind::{BufferKind, KindOfFirstBinding};
use super::state::ContextState;
use super::web_gl_object::WebGLObject;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WebGLBuffer {
    object: WebGLObject,
    #[gc(untraced)]
    kind: KindOfFirstBinding,
}

impl Wrappable for WebGLBuffer {
    fn relevant_realm(&self, vm: &Vm) -> Option<Gc<Realm>> {
        self.object.relevant_realm(vm)
    }
}

impl Inherits<WebGLObject> for WebGLBuffer {
    fn parent(&self) -> &WebGLObject {
        &self.object
    }
}

impl WebGLBuffer {
    pub(crate) fn create(vm: &Vm, context: &ContextState, handle: u32) -> Ref<Self> {
        allocate(vm, |_| Self {
            object: WebGLObject::new(context, handle),
            kind: KindOfFirstBinding::default(),
        })
    }

    pub(crate) fn kind_allows_binding_to(&self, target: u32) -> bool {
        self.kind.allows_binding_to(target)
    }

    pub(crate) fn was_bound_as(&self, kind: BufferKind) {
        self.kind.record_binding_as(kind);
    }
}
