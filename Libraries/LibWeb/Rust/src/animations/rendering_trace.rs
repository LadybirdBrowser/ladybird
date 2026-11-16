/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The rendering trace of the event loop, which notes the animation work of its documents as it happens.

unsafe extern "C" {
    fn web_animation_note_rendering_trace_event(event: *const u8, length: usize);
}

/// Notes `event` in the rendering trace of the event loop, where a trace is being taken.
pub(crate) fn note_rendering_trace_event(event: &str) {
    // SAFETY: The event is live for the call.
    unsafe { web_animation_note_rendering_trace_event(event.as_ptr(), event.len()) };
}

#[cfg(test)]
mod ffi_test_stubs {
    #[unsafe(no_mangle)]
    extern "C" fn web_animation_note_rendering_trace_event(_event: *const u8, _length: usize) {}
}
