/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PromiseRejectionEvent, https://html.spec.whatwg.org/multipage/webappapis.html#promiserejectionevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Object, Realm, Trace, Value, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::lent_string::LentUtf16FlyString;
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

/// The dictionary PromiseRejectionEventInit of HTML/PromiseRejectionEvent.idl.
pub(crate) struct PromiseRejectionEventInit {
    pub(crate) parent: EventInit,
    pub(crate) promise: Gc<Object>,
    pub(crate) reason: Option<Value>,
}

// https://html.spec.whatwg.org/multipage/webappapis.html#promiserejectionevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PromiseRejectionEvent {
    event: CppEvent,
    promise: Gc<Object>,
    reason: Value,
}

impl Wrappable for PromiseRejectionEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl PromiseRejectionEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: PromiseRejectionEventInit,
    ) -> Ref<PromiseRejectionEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| PromiseRejectionEvent {
            event,
            promise: event_init.promise,
            reason: event_init.reason.unwrap_or(Value::UNDEFINED),
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: PromiseRejectionEventInit,
    ) -> WebIdlResult<Ref<PromiseRejectionEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://html.spec.whatwg.org/multipage/webappapis.html#dom-promiserejectionevent-promise
    pub(crate) fn promise(&self, _: &mut CallContext) -> WebIdlResult<Gc<Object>> {
        Ok(self.promise)
    }

    // https://html.spec.whatwg.org/multipage/webappapis.html#dom-promiserejectionevent-reason
    pub(crate) fn reason(&self, _: &mut CallContext) -> WebIdlResult<Value> {
        Ok(self.reason)
    }
}

/// The Event of a new trusted PromiseRejectionEvent in the realm, for the implementation to fire.
#[allow(clippy::too_many_arguments, reason = "C++ passes each member of the dictionary")]
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_promise_rejection_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    bubbles: bool,
    cancelable: bool,
    composed: bool,
    promise: Gc<Object>,
    reason: Value,
) -> *mut c_void {
    let event_init = PromiseRejectionEventInit {
        parent: EventInit {
            bubbles,
            cancelable,
            composed,
        },
        promise,
        reason: Some(reason),
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let event = PromiseRejectionEvent::create(vm, realm, &r#type, event_init);
    event.event().set_is_trusted(true);
    event.event().as_ptr()
}
