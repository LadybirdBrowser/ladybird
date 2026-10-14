/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::{Utf16String, Utf16StringUnits, append_utf16_as_utf8_up_to_unpaired_surrogate};

/// UTF-8, except that a surrogate that is not half of a pair is encoded as if it were a code point.
pub(crate) fn utf8_bytes(string: &Utf16String) -> Vec<u8> {
    let mut code_units = match string.as_units() {
        Utf16StringUnits::Ascii(bytes) => return bytes.to_vec(),
        Utf16StringUnits::Utf16(code_units) => code_units,
    };
    let mut bytes = Vec::with_capacity(code_units.len());
    while !code_units.is_empty() {
        let encoded_code_units = append_utf16_as_utf8_up_to_unpaired_surrogate(&mut bytes, code_units);
        code_units = &code_units[encoded_code_units..];
        if let [surrogate, rest @ ..] = code_units {
            let surrogate = u32::from(*surrogate);
            bytes.extend_from_slice(&[
                0xE0 | (surrogate >> 12) as u8,
                0x80 | ((surrogate >> 6) & 0x3F) as u8,
                0x80 | (surrogate & 0x3F) as u8,
            ]);
            code_units = rest;
        }
    }
    bytes
}

pub(crate) fn ascii_bytes(string: &Utf16String) -> Option<Vec<u8>> {
    match string.as_units() {
        Utf16StringUnits::Ascii(bytes) => Some(bytes.to_vec()),
        Utf16StringUnits::Utf16(code_units) => code_units
            .iter()
            .map(|&code_unit| u8::try_from(code_unit).ok().filter(u8::is_ascii))
            .collect(),
    }
}

/// Lossy, as a driver can return bytes that are not UTF-8.
pub(crate) fn utf16_string_from_gl_string(bytes: &[u8]) -> Utf16String {
    if bytes.is_ascii() {
        return Utf16String::from_ascii(bytes);
    }
    Utf16String::from_utf8(&String::from_utf8_lossy(bytes))
}
