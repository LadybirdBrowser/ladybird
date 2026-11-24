/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CustomEvent, https://dom.spec.whatwg.org/#interface-customevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, Realm, Trace, Value, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::lent_string::LentUtf16FlyString;
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

/// The dictionary CustomEventInit of DOM/CustomEvent.idl.
pub(crate) struct CustomEventInit {
    pub(crate) parent: EventInit,
    pub(crate) detail: Value,
}

// https://dom.spec.whatwg.org/#customevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CustomEvent {
    event: CppEvent,
    // https://dom.spec.whatwg.org/#dom-customevent-initcustomevent-type-bubbles-cancelable-detail-detail
    detail: GcRefCell<Value>,
}

impl Wrappable for CustomEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl CustomEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: CustomEventInit,
    ) -> Ref<CustomEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| CustomEvent {
            event,
            detail: GcRefCell::new(event_init.detail),
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: CustomEventInit,
    ) -> WebIdlResult<Ref<CustomEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://dom.spec.whatwg.org/#dom-customevent-detail
    pub(crate) fn detail(&self, _: &mut CallContext) -> WebIdlResult<Value> {
        Ok(*self.detail.borrow())
    }

    // https://dom.spec.whatwg.org/#dom-customevent-initcustomevent
    pub(crate) fn init_custom_event(
        &self,
        cx: &mut CallContext,
        r#type: Utf16String,
        bubbles: bool,
        cancelable: bool,
        detail: Value,
    ) -> WebIdlResult<()> {
        // 1. If this’s dispatch flag is set, then return.
        if self.event.dispatched() {
            return Ok(());
        }

        // 2. Initialize this with type, bubbles, and cancelable.
        self.event.initialize_event(&r#type, bubbles, cancelable);

        // 3. Set this’s detail attribute to detail.
        *self.detail.borrow_mut(cx.vm()) = detail;
        Ok(())
    }
}

/// The Event of a new trusted CustomEvent in the realm, for the implementation to fire.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_custom_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    detail: Value,
) -> *mut c_void {
    let event_init = CustomEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
        detail,
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let event = CustomEvent::create(vm, realm, &r#type, event_init);
    event.event().set_is_trusted(true);
    event.event().as_ptr()
}

/// The Event of a new CustomEvent in the realm whose type is the empty string, for createEvent().
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_custom_event_create_for_create_event(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    let event_init = CustomEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
        detail: Value::NULL,
    };
    CustomEvent::create(vm, realm, &Utf16String::from_utf8(""), event_init)
        .event()
        .as_ptr()
}
