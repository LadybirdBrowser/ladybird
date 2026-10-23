/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The URL Standard, https://url.spec.whatwg.org/, and the Origin interface of the HTML Standard,
//! https://html.spec.whatwg.org/multipage/browsers.html#the-origin-interface

use ak::Utf16StringUnits;
use liburl_rust::url::UrlInput;

#[cfg(not(test))]
pub(crate) mod origin;

pub(crate) fn url_input(units: Utf16StringUnits<'_>) -> UrlInput<'_> {
    match units {
        Utf16StringUnits::Ascii(bytes) => UrlInput::Utf8(core::str::from_utf8(bytes).expect("ASCII is UTF-8")),
        Utf16StringUnits::Utf16(code_units) => UrlInput::Utf16(code_units),
    }
}
