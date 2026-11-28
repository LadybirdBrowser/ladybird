/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CookieChangeEvent, https://cookiestore.spec.whatwg.org/#cookiechangeevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::cookie_store::cookie_store::CookieListItem;
use crate::dom::event::EventInit;

/// The dictionary CookieChangeEventInit of CookieStore/CookieChangeEvent.idl.
pub(crate) struct CookieChangeEventInit {
    pub(crate) parent: EventInit,
    pub(crate) changed: Option<Vec<CookieListItem>>,
    pub(crate) deleted: Option<Vec<CookieListItem>>,
}

// https://cookiestore.spec.whatwg.org/#cookiechangeevent
#[derive(Trace)]
pub(crate) struct CookieChangeEvent {
    event: CppEvent,
    #[gc(untraced)]
    changed: Vec<CookieListItem>,
    #[gc(untraced)]
    deleted: Vec<CookieListItem>,
}

impl Wrappable for CookieChangeEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl CookieChangeEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: CookieChangeEventInit,
    ) -> Ref<CookieChangeEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| CookieChangeEvent {
            event,
            changed: event_init.changed.unwrap_or_default(),
            deleted: event_init.deleted.unwrap_or_default(),
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: CookieChangeEventInit,
    ) -> WebIdlResult<Ref<CookieChangeEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn changed(&self, _: &mut CallContext) -> WebIdlResult<Vec<CookieListItem>> {
        Ok(self.changed.clone())
    }

    pub(crate) fn deleted(&self, _: &mut CallContext) -> WebIdlResult<Vec<CookieListItem>> {
        Ok(self.deleted.clone())
    }
}
