/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! BeforeUnloadEvent, https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-beforeunloadevent-interface

use core::cell::RefCell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{InterfaceCell, Ref, Wrappable, allocate};
use crate::bindings::lent_string::{LentUtf16FlyString, LentUtf16String};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#beforeunloadevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct BeforeUnloadEvent {
    event: CppEvent,
    #[gc(untraced)]
    return_value: RefCell<Utf16String>,
}

impl Wrappable for BeforeUnloadEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl BeforeUnloadEvent {
    fn create(vm: &Vm, realm: Gc<Realm>, event_name: &Utf16String, event_init: &EventInit) -> Ref<BeforeUnloadEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        let object = allocate(vm, |_| BeforeUnloadEvent {
            event,
            return_value: RefCell::new(Utf16String::from_utf8("")),
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-beforeunloadevent-returnvalue
    pub(crate) fn return_value(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.return_value.borrow().clone())
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-beforeunloadevent-returnvalue
    pub(crate) fn set_return_value(&self, _: &mut CallContext, return_value: Utf16String) -> WebIdlResult<()> {
        *self.return_value.borrow_mut() = return_value;
        Ok(())
    }
}

/// The Event of a new trusted BeforeUnloadEvent in the realm, for the implementation to fire.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_before_unload_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    bubbles: bool,
    cancelable: bool,
    composed: bool,
) -> *mut c_void {
    let event_init = EventInit {
        bubbles,
        cancelable,
        composed,
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let event = BeforeUnloadEvent::create(vm, realm, &r#type, &event_init);
    event.event().as_ptr()
}

/// The Event of a new BeforeUnloadEvent in the realm whose type is the empty string, for createEvent().
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_before_unload_event_create_for_create_event(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    let event_init = EventInit {
        bubbles: false,
        cancelable: false,
        composed: false,
    };
    BeforeUnloadEvent::create(vm, realm, &Utf16String::from_utf8(""), &event_init)
        .event()
        .as_ptr()
}

/// The returnValue of the BeforeUnloadEvent, which the caller adopts.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_before_unload_event_return_value(event: &InterfaceCell) -> usize {
    let event = event.implementation::<BeforeUnloadEvent>();
    event.return_value.borrow().clone().into_raw()
}

/// Sets the returnValue of the BeforeUnloadEvent.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_before_unload_event_set_return_value(
    event: &InterfaceCell,
    return_value: LentUtf16String<'_>,
) {
    let event = event.implementation::<BeforeUnloadEvent>();
    *event.return_value.borrow_mut() = return_value.to_utf16_string();
}
