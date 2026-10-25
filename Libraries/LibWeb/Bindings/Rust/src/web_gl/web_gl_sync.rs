/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/2.0/#3.4

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use super::state::ContextState;
use super::web_gl_object::{InvalidOperation, WebGLObject};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WebGLSync {
    object: WebGLObject,
    /// The name of the sync object, which is no GL name of the kind that other objects have.
    #[gc(untraced)]
    sync_handle: u32,
}

impl Wrappable for WebGLSync {
    fn relevant_realm(&self, vm: &Vm) -> Option<Gc<Realm>> {
        self.object.relevant_realm(vm)
    }
}

impl Inherits<WebGLObject> for WebGLSync {
    fn parent(&self) -> &WebGLObject {
        &self.object
    }
}

impl WebGLSync {
    pub(crate) fn create(vm: &Vm, context: &ContextState, sync_handle: u32) -> Ref<Self> {
        allocate(vm, |_| Self {
            object: WebGLObject::new(context, 0),
            sync_handle,
        })
    }

    pub(crate) fn sync_handle(&self, context: &ContextState) -> Result<u32, InvalidOperation> {
        self.object.handle(context).map(|_| self.sync_handle)
    }

    pub(crate) fn sync_handle_for_deletion(&self, context: &ContextState) -> Result<Option<u32>, InvalidOperation> {
        Ok(self.object.handle_for_deletion(context)?.map(|_| self.sync_handle))
    }
}
