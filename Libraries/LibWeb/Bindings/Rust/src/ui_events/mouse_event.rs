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
