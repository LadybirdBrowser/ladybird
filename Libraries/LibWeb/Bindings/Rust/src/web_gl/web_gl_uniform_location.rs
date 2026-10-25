/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/1.0/#5.10

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use super::web_gl_object::InvalidOperation;
use super::web_gl_program::WebGLProgram;
use crate::bindings::interface::{Ref, Wrappable, allocate};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WebGLUniformLocation {
    #[gc(untraced)]
    handle: i32,
    program: Ref<WebGLProgram>,
}

impl Wrappable for WebGLUniformLocation {
    fn relevant_realm(&self, vm: &Vm) -> Option<Gc<Realm>> {
        self.program.relevant_realm(vm)
    }
}

impl WebGLUniformLocation {
    pub(crate) fn create(vm: &Vm, handle: i32, program: Ref<WebGLProgram>) -> Ref<Self> {
        allocate(vm, |_| Self { handle, program })
    }

    pub(crate) fn handle(&self, current_program: Option<Ref<WebGLProgram>>) -> Result<i32, InvalidOperation> {
        match current_program {
            Some(current_program) if current_program.as_ptr() == self.program.as_ptr() => Ok(self.handle),
            _ => Err(InvalidOperation),
        }
    }
}
