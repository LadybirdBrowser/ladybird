/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/media.html#mediaerror

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{InterfaceCell, Wrappable, allocate, cell_into_abi};
use crate::bindings::lent_string::LentUtf16String;
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MediaError {
    code: u16,
    message: Utf16String,
}

impl Wrappable for MediaError {}

impl MediaError {
    // https://html.spec.whatwg.org/multipage/media.html#dom-mediaerror-code
    pub(crate) fn code(&self, _: &mut CallContext) -> WebIdlResult<u16> {
        Ok(self.code)
    }

    // https://html.spec.whatwg.org/multipage/media.html#dom-mediaerror-message
    pub(crate) fn message(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.message.clone())
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_media_error_create(vm: &Vm, code: u16, message: LentUtf16String<'_>) -> *mut c_void {
    let message = message.to_utf16_string();
    cell_into_abi(allocate(vm, |_| MediaError { code, message }))
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_media_error_code(error: &InterfaceCell) -> u16 {
    error.implementation::<MediaError>().code
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_media_error_message(error: &InterfaceCell) -> usize {
    error.implementation::<MediaError>().message.clone().into_raw()
}
