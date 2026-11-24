/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PopStateEvent, https://html.spec.whatwg.org/multipage/nav-history-apis.html#popstateevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Value, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::lent_string::LentUtf16FlyString;
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

/// The dictionary PopStateEventInit of HTML/PopStateEvent.idl.
pub(crate) struct PopStateEventInit {
    pub(crate) parent: EventInit,
    pub(crate) state: Value,
    pub(crate) has_ua_visual_transition: bool,
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#popstateevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PopStateEvent {
    event: CppEvent,
    state: Value,
    has_ua_visual_transition: bool,
}

impl Wrappable for PopStateEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl PopStateEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: PopStateEventInit,
    ) -> Ref<PopStateEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| PopStateEvent {
            event,
            state: event_init.state,
            has_ua_visual_transition: event_init.has_ua_visual_transition,
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: PopStateEventInit,
    ) -> WebIdlResult<Ref<PopStateEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-popstateevent-state
    pub(crate) fn state(&self, _: &mut CallContext) -> WebIdlResult<Value> {
        Ok(self.state)
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-popstateevent-hasuavisualtransition
    pub(crate) fn has_ua_visual_transition(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.has_ua_visual_transition)
    }
}

/// The Event of a new trusted PopStateEvent in the realm, for the implementation to fire.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_pop_state_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    state: Value,
) -> *mut c_void {
    let event_init = PopStateEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
        state,
        has_ua_visual_transition: false,
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let event = PopStateEvent::create(vm, realm, &r#type, event_init);
    event.event().set_is_trusted(true);
    event.event().as_ptr()
}
