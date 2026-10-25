/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/1.0/#5.6

use core::cell::Cell;

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use super::state::ContextState;
use super::web_gl_object::WebGLObject;
use super::web_gl_shader::WebGLShader;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WebGLProgram {
    object: WebGLObject,
    pub(crate) attached_vertex_shader: Cell<Option<Ref<WebGLShader>>>,
    pub(crate) attached_fragment_shader: Cell<Option<Ref<WebGLShader>>>,
}

impl Wrappable for WebGLProgram {
    fn relevant_realm(&self, vm: &Vm) -> Option<Gc<Realm>> {
        self.object.relevant_realm(vm)
    }
}

impl Inherits<WebGLObject> for WebGLProgram {
    fn parent(&self) -> &WebGLObject {
        &self.object
    }
}

impl WebGLProgram {
    pub(crate) fn create(vm: &Vm, context: &ContextState, handle: u32) -> Ref<Self> {
        allocate(vm, |_| Self {
            object: WebGLObject::new(context, handle),
            attached_vertex_shader: Cell::new(None),
            attached_fragment_shader: Cell::new(None),
        })
    }
}
