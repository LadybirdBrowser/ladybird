/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! MediaQueryListEvent, https://drafts.csswg.org/cssom-view/#mediaquerylistevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

/// The dictionary MediaQueryListEventInit of CSS/MediaQueryListEvent.idl.
pub(crate) struct MediaQueryListEventInit {
    pub(crate) parent: EventInit,
    pub(crate) matches: bool,
    pub(crate) media: Utf16String,
}

// https://drafts.csswg.org/cssom-view/#mediaquerylistevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MediaQueryListEvent {
    event: CppEvent,
    #[gc(untraced)]
    media: Utf16String,
    matches: bool,
}

impl Wrappable for MediaQueryListEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl MediaQueryListEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: MediaQueryListEventInit,
    ) -> Ref<MediaQueryListEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| MediaQueryListEvent {
            event,
            media: event_init.media,
            matches: event_init.matches,
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: MediaQueryListEventInit,
    ) -> WebIdlResult<Ref<MediaQueryListEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn media(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.media.clone())
    }

    pub(crate) fn matches(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.matches)
    }
}
