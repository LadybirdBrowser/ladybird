/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWeb's C++ code parses WebVTT files and creates VTTCue objects for the cues in them,
//! which Libraries/LibWeb/WebVTT/Parser.cpp and Libraries/LibWeb/WebVTT/VTTCue.cpp declare.

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::Vm;

use crate::bindings::interface::cell_into_abi;
use crate::web_vtt::parser::parse_webvtt_file;
use crate::web_vtt::vtt_cue::VTTCue;

/// https://w3c.github.io/webvtt/#dom-vttcue-vttcue
///
/// # Safety
///
/// `vm` must be the live VM, and `text` a string that the caller lends for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_vtt_cue_create(
    vm: *mut c_void,
    start_time: f64,
    end_time: f64,
    text: usize,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and lends the string.
    let (vm, text) = unsafe { (&*vm.cast::<Vm>(), Utf16String::from_raw_borrowed(text)) };
    let Ok(cue) = VTTCue::create(vm, start_time, end_time, text) else {
        unreachable!("C++ creates cues whose end time is neither negative infinity nor NaN");
    };
    cell_into_abi(cue)
}

/// https://w3c.github.io/webvtt/#webvtt-parser
///
/// Parses the bytes and passes each cue to `on_cue`, with the identifier and the text, whose references the callee
/// adopts. Returns false if the file does not start with the WebVTT file signature.
///
/// # Safety
///
/// `bytes` must point to `length` bytes, and `on_cue` must be safe to call with `ctx`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_vtt_parse_webvtt_file(
    bytes: *const u8,
    length: usize,
    ctx: *mut c_void,
    on_cue: unsafe extern "C" fn(ctx: *mut c_void, identifier: usize, start_time: f64, end_time: f64, text: usize),
) -> bool {
    // SAFETY: The caller passes `length` bytes.
    let bytes = unsafe { ak::slice_from_raw_parts(bytes, length) };
    let Some(cues) = parse_webvtt_file(bytes) else {
        return false;
    };
    for cue in cues {
        let identifier = cue.identifier.into_raw();
        let text = cue.text.into_raw();
        // SAFETY: The caller passes a callback that is safe to call with the context.
        unsafe { on_cue(ctx, identifier, cue.start_time, cue.end_time, text) };
    }
    true
}
