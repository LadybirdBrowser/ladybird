/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use libjs_rust::api::Trace;

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::CppEvent;

unsafe extern "C" {
    fn web_ui_events_input_event_from_event(event: *mut c_void) -> *mut c_void;
    fn web_ui_events_input_event_is_composing(event: *mut c_void) -> bool;
}

/// https://w3c.github.io/uievents/#inputevent
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppInputEvent(CppCell);

impl CppInputEvent {
    pub(crate) fn from_event(event: &CppEvent) -> Option<Self> {
        // SAFETY: The event is live, and LibWeb returns it if it is an InputEvent.
        let event = unsafe { web_ui_events_input_event_from_event(event.as_ptr()) };
        // SAFETY: The InputEvent is live.
        (!event.is_null()).then(|| Self(unsafe { CppCell::from_raw(event) }))
    }

    /// https://w3c.github.io/uievents/#dom-inputevent-iscomposing
    pub(crate) fn is_composing(&self) -> bool {
        // SAFETY: The InputEvent is live.
        unsafe { web_ui_events_input_event_is_composing(self.0.as_ptr()) }
    }
}
