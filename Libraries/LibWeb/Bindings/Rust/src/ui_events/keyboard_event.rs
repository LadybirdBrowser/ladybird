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
    fn web_ui_events_keyboard_event_from_event(event: *mut c_void) -> *mut c_void;
    fn web_ui_events_keyboard_event_is_composing(event: *mut c_void) -> bool;
    fn web_ui_events_keyboard_event_key_code(event: *mut c_void) -> u32;
}

/// https://w3c.github.io/uievents/#keyboardevent
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppKeyboardEvent(CppCell);

impl CppKeyboardEvent {
    pub(crate) fn from_event(event: &CppEvent) -> Option<Self> {
        // SAFETY: The event is live, and LibWeb returns it if it is a KeyboardEvent.
        let event = unsafe { web_ui_events_keyboard_event_from_event(event.as_ptr()) };
        // SAFETY: The KeyboardEvent is live.
        (!event.is_null()).then(|| Self(unsafe { CppCell::from_raw(event) }))
    }

    /// https://w3c.github.io/uievents/#dom-keyboardevent-iscomposing
    pub(crate) fn is_composing(&self) -> bool {
        // SAFETY: The KeyboardEvent is live.
        unsafe { web_ui_events_keyboard_event_is_composing(self.0.as_ptr()) }
    }

    /// https://w3c.github.io/uievents/#dom-keyboardevent-keycode
    pub(crate) fn key_code(&self) -> u32 {
        // SAFETY: The KeyboardEvent is live.
        unsafe { web_ui_events_keyboard_event_key_code(self.0.as_ptr()) }
    }
}
