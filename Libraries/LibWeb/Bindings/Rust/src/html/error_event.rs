/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ErrorEvent, https://html.spec.whatwg.org/multipage/webappapis.html#errorevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Value, Vm};

use crate::bindings::conversions::UsvString;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{InterfaceCell, Ref, Wrappable, allocate};
use crate::bindings::lent_string::{LentUtf16FlyString, LentUtf16String};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

/// The dictionary ErrorEventInit of HTML/ErrorEvent.idl.
pub(crate) struct ErrorEventInit {
    pub(crate) parent: EventInit,
    pub(crate) message: Utf16String,
    pub(crate) filename: UsvString,
    pub(crate) lineno: u32,
    pub(crate) colno: u32,
    pub(crate) error: Option<Value>,
}

// https://html.spec.whatwg.org/multipage/webappapis.html#errorevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ErrorEvent {
    event: CppEvent,
    message: Utf16String,
    filename: UsvString,
    lineno: u32,
    colno: u32,
    error: Value,
}

impl Wrappable for ErrorEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl ErrorEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: ErrorEventInit,
    ) -> Ref<ErrorEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| ErrorEvent {
            event,
            message: event_init.message,
            filename: event_init.filename,
            lineno: event_init.lineno,
            colno: event_init.colno,
            error: event_init.error.unwrap_or(Value::UNDEFINED),
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: ErrorEventInit,
    ) -> WebIdlResult<Ref<ErrorEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://html.spec.whatwg.org/multipage/webappapis.html#dom-errorevent-message
    pub(crate) fn message(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.message.clone())
    }

    // https://html.spec.whatwg.org/multipage/webappapis.html#dom-errorevent-filename
    pub(crate) fn filename(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(self.filename.clone())
    }

    // https://html.spec.whatwg.org/multipage/webappapis.html#dom-errorevent-lineno
    pub(crate) fn lineno(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.lineno)
    }

    // https://html.spec.whatwg.org/multipage/webappapis.html#dom-errorevent-colno
    pub(crate) fn colno(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.colno)
    }

    // https://html.spec.whatwg.org/multipage/webappapis.html#dom-errorevent-error
    pub(crate) fn error(&self, _: &mut CallContext) -> WebIdlResult<Value> {
        Ok(self.error)
    }
}

/// The Event of a new trusted ErrorEvent in the realm, for the implementation to fire.
#[allow(clippy::too_many_arguments, reason = "C++ passes each member of the dictionary")]
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_error_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    bubbles: bool,
    cancelable: bool,
    composed: bool,
    message: LentUtf16String<'_>,
    filename: LentUtf16String<'_>,
    lineno: u32,
    colno: u32,
    error: Value,
) -> *mut c_void {
    let event_init = ErrorEventInit {
        parent: EventInit {
            bubbles,
            cancelable,
            composed,
        },
        message: message.to_utf16_string(),
        filename: UsvString::from_utf16_string(&filename.to_utf16_string()),
        lineno,
        colno,
        error: Some(error),
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let event = ErrorEvent::create(vm, realm, &r#type, event_init);
    event.event().as_ptr()
}

/// Stores the attributes of the ErrorEvent, whose strings the caller adopts.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_error_event_attributes(
    event: &InterfaceCell,
    message: &mut usize,
    filename: &mut usize,
    lineno: &mut u32,
    colno: &mut u32,
    error: &mut u64,
) {
    let event = event.implementation::<ErrorEvent>();
    *message = event.message.clone().into_raw();
    *filename = event.filename.clone().into_utf16_string().into_raw();
    *lineno = event.lineno;
    *colno = event.colno;
    *error = event.error.to_raw();
}
