/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Functions the parser and codegen import from the runtime that embeds them.
//!
//! The runtime implements them in frontend_host.rs. The cargo test binary, which
//! builds the frontend without the runtime, uses the stand-ins in test_host.rs.

use crate::ast::Utf16String;

#[cfg(not(test))]
use crate::frontend_host as implementation;
#[cfg(test)]
use crate::test_host as implementation;

/// Convert a JS number to its UTF-16 string representation using the
/// ECMA-262 Number::toString algorithm, which the host implements.
pub fn js_number_to_utf16(value: f64) -> Utf16String {
    Utf16String(implementation::number_to_utf16(value))
}

/// Check a regex pattern+flags with the host's regex engine, so that an invalid
/// one becomes an early error. Returns the error message if it is invalid.
pub fn validate_regex(pattern: &[u16], flags: &[u16]) -> Result<(), Utf16String> {
    implementation::validate_regex(pattern, flags).map_err(|message| Utf16String(message.encode_utf16().collect()))
}
