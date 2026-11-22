/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ClipboardEvent, https://w3c.github.io/clipboard-apis/#clipboardevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;
use crate::html::data_transfer::DataTransfer;

/// The dictionary ClipboardEventInit of Clipboard/ClipboardEvent.idl.
pub(crate) struct ClipboardEventInit {
    pub(crate) parent: EventInit,
    pub(crate) clipboard_data: Option<Ref<DataTransfer>>,
}

// https://w3c.github.io/clipboard-apis/#clipboardevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ClipboardEvent {
    event: CppEvent,
    clipboard_data: Option<Ref<DataTransfer>>,
}

impl Wrappable for ClipboardEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl ClipboardEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: &EventInit,
        clipboard_data: Option<Ref<DataTransfer>>,
    ) -> Ref<ClipboardEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        let object = allocate(vm, |_| ClipboardEvent { event, clipboard_data });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: ClipboardEventInit,
    ) -> WebIdlResult<Ref<ClipboardEvent>> {
        Ok(Self::create(
            cx.vm(),
            cx.realm(),
            &r#type,
            &event_init_dict.parent,
            event_init_dict.clipboard_data,
        ))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn clipboard_data(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<DataTransfer>>> {
        Ok(self.clipboard_data)
    }
}
