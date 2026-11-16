/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the C++ code of LibWeb asks of the interfaces of Web Animations, declared in
//! Libraries/LibWeb/Animations/RecordHandles.cpp.

use core::ffi::c_void;

use ak::Utf16String;
use libcompositing_rust::animation_timing::OptionalTimeValue;

use super::animation_playback_event::{
    AnimationPlaybackEvent, AnimationPlaybackEventInit, time_value_as_css_numberish,
};
use crate::bindings::interface::realm_arguments;
use crate::dom::event::EventInit;

/// A new AnimationPlaybackEvent that neither bubbles nor can be canceled, as the Event that C++ dispatches.
///
/// # Safety
///
/// `vm` must be the live VM, `realm` a live realm, and `event_name` a raw string that C++ hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_animation_playback_event_create(
    vm: *mut c_void,
    realm: *mut c_void,
    event_name: usize,
    current_time: OptionalTimeValue,
    timeline_time: OptionalTimeValue,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    // SAFETY: The caller hands over the string.
    let event_name = unsafe { Utf16String::from_raw_owned(event_name) };
    let event_init = AnimationPlaybackEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
        current_time: current_time.get().map(|time| time_value_as_css_numberish(vm, time)),
        timeline_time: timeline_time.get().map(|time| time_value_as_css_numberish(vm, time)),
    };
    AnimationPlaybackEvent::create(vm, realm, &event_name, event_init)
        .event()
        .as_ptr()
}
