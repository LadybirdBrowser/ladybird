/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/1.0/#5.8

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use super::state::ContextState;
use super::web_gl_object::WebGLObject;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WebGLShader {
    object: WebGLObject,
    #[gc(untraced)]
    r#type: u32,
}

impl Wrappable for WebGLShader {
    fn relevant_realm(&self, vm: &Vm) -> Option<Gc<Realm>> {
        self.object.relevant_realm(vm)
    }
}

impl Inherits<WebGLObject> for WebGLShader {
    fn parent(&self) -> &WebGLObject {
        &self.object
    }
}

impl WebGLShader {
    pub(crate) fn create(vm: &Vm, context: &ContextState, handle: u32, r#type: u32) -> Ref<Self> {
        allocate(vm, |_| Self {
            object: WebGLObject::new(context, handle),
            r#type,
        })
    }

    pub(crate) fn r#type(&self) -> u32 {
        self.r#type
    }
}
