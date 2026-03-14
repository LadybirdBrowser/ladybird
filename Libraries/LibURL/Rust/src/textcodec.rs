/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum EncodeItem {
    Byte(u8),
    Error(u32),
}

type FfiByteFn = unsafe extern "C" fn(*mut c_void, u8);
type FfiCodePointFn = unsafe extern "C" fn(*mut c_void, u32);

unsafe extern "C" {
    fn textcodec_rust_encode(
        encoding: *const u8,
        encoding_length: usize,
        input: *const u8,
        input_length: usize,
        ctx: *mut c_void,
        on_byte: FfiByteFn,
        on_error: FfiCodePointFn,
    ) -> bool;
}

struct EncodeCallbacks<'a> {
    on_item: &'a mut dyn FnMut(EncodeItem),
}

unsafe extern "C" fn on_encode_byte_with_callbacks(ctx: *mut c_void, byte: u8) {
    // SAFETY: `ctx` was set to `addr_of_mut!(callbacks)` in `encode_into`.
    let callbacks = unsafe { &mut *(ctx as *mut EncodeCallbacks<'_>) };
    (callbacks.on_item)(EncodeItem::Byte(byte));
}

unsafe extern "C" fn on_encode_error_with_callbacks(ctx: *mut c_void, error: u32) {
    // SAFETY: `ctx` was set to `addr_of_mut!(callbacks)` in `encode_into`.
    let callbacks = unsafe { &mut *(ctx as *mut EncodeCallbacks<'_>) };
    (callbacks.on_item)(EncodeItem::Error(error));
}

// https://encoding.spec.whatwg.org/#get-an-output-encoding
pub(crate) fn get_output_encoding(encoding: &str) -> &str {
    // 1. If encoding is replacement or UTF-16BE/LE, then return UTF-8.
    if encoding.eq_ignore_ascii_case("replacement")
        || encoding.eq_ignore_ascii_case("utf-16le")
        || encoding.eq_ignore_ascii_case("utf-16be")
    {
        return "UTF-8";
    }

    // 2. Return encoding.
    encoding
}

pub(crate) fn encode_into(encoding: &str, input: &str, mut on_item: impl FnMut(EncodeItem)) -> bool {
    let mut callbacks = EncodeCallbacks { on_item: &mut on_item };

    // SAFETY: `encoding`, `input`, and `callbacks` are valid for the duration of the call.
    unsafe {
        textcodec_rust_encode(
            encoding.as_ptr(),
            encoding.len(),
            input.as_ptr(),
            input.len(),
            std::ptr::addr_of_mut!(callbacks) as *mut c_void,
            on_encode_byte_with_callbacks,
            on_encode_error_with_callbacks,
        )
    }
}
