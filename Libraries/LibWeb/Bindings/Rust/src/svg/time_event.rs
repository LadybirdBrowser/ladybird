/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TimeEvent, https://svgwg.org/specs/animations/#InterfaceTimeEvent

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Object, Realm, Trace, Vm};

use crate::bindings::conversions::{CppInterface, window_proxy_from_object};
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Wrappable, allocate};
use crate::bindings::lent_string::LentUtf16FlyString;
use crate::bindings::{CallContext, WebIdlResult};

// https://svgwg.org/specs/animations/#InterfaceTimeEvent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TimeEvent {
    event: CppEvent,
    view: Cell<Option<CppInterface>>,
    detail: Cell<i32>,
}

impl Wrappable for TimeEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl TimeEvent {
    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn view(&self, _: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        Ok(self.view.get())
    }

    pub(crate) fn detail(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        Ok(self.detail.get())
    }

    // https://svgwg.org/specs/animations/#__svg__TimeEvent__initTimeEvent
    pub(crate) fn init_time_event(
        &self,
        _: &mut CallContext,
        type_arg: Utf16String,
        view_arg: Option<CppInterface>,
        detail_arg: i32,
    ) -> WebIdlResult<()> {
        // The initTimeEvent method is used to initialize the value of a TimeEvent created with
        // document.createEvent(). This method may only be called before the TimeEvent has been dispatched via the
        // dispatchEvent method, though it may be called multiple times during that phase if necessary. If called
        // multiple times, the final invocation takes precedence.
        if self.event.dispatched() {
            return Ok(());
        }

        // NB: Time events neither bubble nor are cancelable.
        self.event.initialize_event(&type_arg, false, false);
        self.view.set(view_arg);
        self.detail.set(detail_arg);
        Ok(())
    }
}

/// The Event of a new trusted TimeEvent in the realm, for the implementation to fire. The view is a WindowProxy.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_time_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    view: Option<Gc<Object>>,
    detail: i32,
) -> *mut c_void {
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let view = view.map(|view| window_proxy_from_object(view).expect("the view is a WindowProxy"));
    let event = CppEvent::create_for_owner(realm, &r#type, false, false, false);
    let object = allocate(vm, |_| TimeEvent {
        event,
        view: Cell::new(view),
        detail: Cell::new(detail),
    });
    object.event.set_owner(object);
    object.event().as_ptr()
}
