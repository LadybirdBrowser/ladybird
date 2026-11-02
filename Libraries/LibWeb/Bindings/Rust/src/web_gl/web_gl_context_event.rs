/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! WebGLContextEvent, https://registry.khronos.org/webgl/specs/latest/1.0/#5.15

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

pub(crate) struct WebGLContextEventInit {
    pub(crate) parent: EventInit,
    pub(crate) status_message: Utf16String,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WebGLContextEvent {
    event: CppEvent,
    status_message: Utf16String,
}

impl Wrappable for WebGLContextEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl WebGLContextEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: &WebGLContextEventInit,
    ) -> Ref<WebGLContextEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| WebGLContextEvent {
            event,
            status_message: event_init.status_message.clone(),
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init: WebGLContextEventInit,
    ) -> WebIdlResult<Ref<WebGLContextEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, &event_init))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn status_message(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.status_message.clone())
    }
}
