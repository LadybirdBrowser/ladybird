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
    fn web_ui_events_pointer_event_from_event(event: *mut c_void) -> *mut c_void;
    fn web_ui_events_pointer_event_pointer_id(event: *mut c_void) -> i32;
}

pub(crate) const MOUSE_POINTER_ID: i32 = 0;

/// https://w3c.github.io/pointerevents/#pointerevent-interface
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppPointerEvent(CppCell);

impl CppPointerEvent {
    pub(crate) fn from_event(event: &CppEvent) -> Option<Self> {
        // SAFETY: The event is live, and LibWeb returns it if it is a PointerEvent.
        let event = unsafe { web_ui_events_pointer_event_from_event(event.as_ptr()) };
        // SAFETY: The PointerEvent is live.
        (!event.is_null()).then(|| Self(unsafe { CppCell::from_raw(event) }))
    }

    /// https://w3c.github.io/pointerevents/#dom-pointerevent-pointerid
    pub(crate) fn pointer_id(&self) -> i32 {
        // SAFETY: The PointerEvent is live.
        unsafe { web_ui_events_pointer_event_pointer_id(self.0.as_ptr()) }
    }
}
