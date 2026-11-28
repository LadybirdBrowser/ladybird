/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! HashChangeEvent, https://html.spec.whatwg.org/multipage/nav-history-apis.html#hashchangeevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::conversions::UsvString;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::lent_string::{LentUtf16FlyString, LentUtf16String};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

/// The dictionary HashChangeEventInit of HTML/HashChangeEvent.idl.
pub(crate) struct HashChangeEventInit {
    pub(crate) parent: EventInit,
    pub(crate) old_url: UsvString,
    pub(crate) new_url: UsvString,
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#hashchangeevent
#[derive(Trace)]
pub(crate) struct HashChangeEvent {
    event: CppEvent,
    old_url: UsvString,
    new_url: UsvString,
}

impl Wrappable for HashChangeEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl HashChangeEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: HashChangeEventInit,
    ) -> Ref<HashChangeEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| HashChangeEvent {
            event,
            old_url: event_init.old_url,
            new_url: event_init.new_url,
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: HashChangeEventInit,
    ) -> WebIdlResult<Ref<HashChangeEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-hashchangeevent-oldurl
    pub(crate) fn old_url(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.old_url.clone())
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-hashchangeevent-newurl
    pub(crate) fn new_url(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.new_url.clone())
    }
}

/// The Event of a new trusted HashChangeEvent in the realm, for the implementation to fire.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_hash_change_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    old_url: LentUtf16String<'_>,
    new_url: LentUtf16String<'_>,
) -> *mut c_void {
    let event_init = HashChangeEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
        old_url: UsvString::from_utf16_string(&old_url.to_utf16_string()),
        new_url: UsvString::from_utf16_string(&new_url.to_utf16_string()),
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let event = HashChangeEvent::create(vm, realm, &r#type, event_init);
    event.event().as_ptr()
}

/// The Event of a new HashChangeEvent in the realm whose type is the empty string and whose attributes are their
/// defaults, for createEvent().
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_hash_change_event_create_for_create_event(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    let event_init = HashChangeEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
        old_url: UsvString::from_ascii(b""),
        new_url: UsvString::from_ascii(b""),
    };
    HashChangeEvent::create(vm, realm, &Utf16String::from_utf8(""), event_init)
        .event()
        .as_ptr()
}
