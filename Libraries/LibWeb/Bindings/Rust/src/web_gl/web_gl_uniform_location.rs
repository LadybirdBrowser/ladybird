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
    /// The link of the program that the location is from.
    #[gc(untraced)]
    link_count: u32,
}

impl Wrappable for WebGLUniformLocation {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        self.program.relevant_realm()
    }
}

impl WebGLUniformLocation {
    pub(crate) fn create(vm: &Vm, handle: i32, program: Ref<WebGLProgram>) -> Ref<Self> {
        allocate(vm, |_| Self {
            handle,
            program,
            link_count: program.link_count(),
        })
    }

    /// The location, if it's a location in `program`.
    pub(crate) fn handle_in(&self, program: Ref<WebGLProgram>) -> Option<i32> {
        (program.as_ptr() == self.program.as_ptr() && self.is_from_last_link()).then_some(self.handle)
    }

    fn is_from_last_link(&self) -> bool {
        self.link_count == self.program.link_count()
    }

    pub(crate) fn handle(&self, current_program: Option<Ref<WebGLProgram>>) -> Result<i32, InvalidOperation> {
        match current_program {
            Some(current_program) if current_program.as_ptr() == self.program.as_ptr() && self.is_from_last_link() => {
                Ok(self.handle)
            }
            _ => Err(InvalidOperation),
        }
    }
}
