/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use crate::bindings::event_target::CppEvent;

unsafe extern "C" {
    fn web_ui_events_mouse_event_from_event(event: *mut c_void) -> *mut c_void;
}

/// https://w3c.github.io/uievents/#mouseevent
pub(crate) fn is_mouse_event(event: &CppEvent) -> bool {
    // SAFETY: The event is live, and LibWeb returns it if it is a MouseEvent.
    !unsafe { web_ui_events_mouse_event_from_event(event.as_ptr()) }.is_null()
}

/// The dictionary MouseEventInit of UIEvents/MouseEvent.idl.
pub(crate) struct MouseEventInit {
    pub(crate) parent: super::event_modifier::EventModifierInit,
    pub(crate) screen_x: f64,
    pub(crate) screen_y: f64,
    pub(crate) client_x: f64,
    pub(crate) client_y: f64,
    pub(crate) movement_x: f64,
    pub(crate) movement_y: f64,
    pub(crate) button: i16,
    pub(crate) buttons: u16,
    pub(crate) related_target: Option<crate::bindings::conversions::CppInterface>,
}
