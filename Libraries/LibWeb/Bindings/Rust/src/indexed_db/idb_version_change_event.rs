/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! IDBVersionChangeEvent, https://w3c.github.io/IndexedDB/#events

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

/// The dictionary IDBVersionChangeEventInit of IndexedDB/IDBVersionChangeEvent.idl.
pub(crate) struct IDBVersionChangeEventInit {
    pub(crate) parent: EventInit,
    pub(crate) new_version: Option<u64>,
    pub(crate) old_version: u64,
}

// https://w3c.github.io/IndexedDB/#events
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct IDBVersionChangeEvent {
    event: CppEvent,
    old_version: u64,
    new_version: Option<u64>,
}

impl Wrappable for IDBVersionChangeEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl IDBVersionChangeEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: &IDBVersionChangeEventInit,
    ) -> Ref<IDBVersionChangeEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| IDBVersionChangeEvent {
            event,
            old_version: event_init.old_version,
            new_version: event_init.new_version,
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: IDBVersionChangeEventInit,
    ) -> WebIdlResult<Ref<IDBVersionChangeEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, &event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn old_version(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        Ok(self.old_version)
    }

    pub(crate) fn new_version(&self, _: &mut CallContext) -> WebIdlResult<Option<u64>> {
        Ok(self.new_version)
    }
}
