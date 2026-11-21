/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Turning the source text of literals into their values. The lexer only validates literals, and a value gets computed
//! when something needs it.

use crate::utf16::Utf16View;
use ak::Utf16String;
pub use libjs_parser::escapes::EscapeMode;
use libjs_parser::escapes::decode_escape_sequences;

const fn ch(c: u8) -> u16 {
    c as u16
}

pub(crate) struct EscapeResult {
    pub(crate) value: Utf16String,
    /// Tagged template encountered an invalid escape.
    pub(crate) failed: bool,
}

/// The text of a string literal: a view of the source if it has no escape sequences, which most do not.
pub(crate) enum StringLiteralText<'t> {
    Source(Utf16View<'t>),
    Processed(Utf16String),
}

impl StringLiteralText<'_> {
    #[inline(always)]
    pub(crate) fn view(&self) -> Utf16View<'_> {
        match self {
            StringLiteralText::Source(view) => *view,
            StringLiteralText::Processed(string) => Utf16View::of_string(string),
        }
    }
}

/// Whether the text of a string literal or a template has an escape sequence in it.
#[inline(always)]
pub(crate) fn has_escape_sequence(units: Utf16View<'_>) -> bool {
    match units {
        Utf16View::Ascii(units) | Utf16View::Latin1(units) => units.contains(&b'\\'),
        Utf16View::Utf16(units) => units.contains(&u16::from(b'\\')),
    }
}

/// The value of a string literal from the text between its quotes, without a copy if that is possible.
#[inline(always)]
pub(crate) fn string_literal_text(units: Utf16View<'_>) -> StringLiteralText<'_> {
    if has_escape_sequence(units) {
        StringLiteralText::Processed(process_escape_sequences_impl(units, EscapeMode::StringLiteral).value)
    } else {
        StringLiteralText::Source(units)
    }
}

/// The value of the text of a string literal or a template, see [`decode_escape_sequences()`].
pub(crate) fn process_escape_sequences_impl(input: Utf16View<'_>, mode: EscapeMode) -> EscapeResult {
    let mut result = Vec::with_capacity(input.length_in_code_units());
    let failed = match input {
        Utf16View::Ascii(units) | Utf16View::Latin1(units) => {
            decode_escape_sequences(units, mode, |unit| result.push(unit))
        }
        Utf16View::Utf16(units) => decode_escape_sequences(units, mode, |unit| result.push(unit)),
    };
    EscapeResult {
        value: Utf16String::from_utf16(&result),
        failed,
    }
}

pub(crate) fn raw_template_value(raw: Utf16View<'_>) -> Utf16String {
    let mut result = Vec::with_capacity(raw.length_in_code_units());
    let mut i = 0;
    while i < raw.length_in_code_units() {
        if raw.code_unit_at(i) == ch(b'\r') {
            result.push(ch(b'\n'));
            if i + 1 < raw.length_in_code_units() && raw.code_unit_at(i + 1) == ch(b'\n') {
                i += 1;
            }
        } else {
            result.push(raw.code_unit_at(i));
        }
        i += 1;
    }
    Utf16String::from_utf16(&result)
}

pub(crate) fn parse_numeric_value(value: Utf16View<'_>) -> f64 {
    let s: String = value
        .code_units()
        .filter(|&c| c != '_' as u16)
        .map(|c| c as u8 as char)
        .collect();

    if s.starts_with("0x") || s.starts_with("0X") {
        parse_integer_with_radix(&s[2..], 16)
    } else if s.starts_with("0o") || s.starts_with("0O") {
        parse_integer_with_radix(&s[2..], 8)
    } else if s.starts_with("0b") || s.starts_with("0B") {
        parse_integer_with_radix(&s[2..], 2)
    } else if s.starts_with('0') && s.len() > 1 && s.as_bytes()[1].is_ascii_digit() {
        let digits = &s[1..];
        if digits.bytes().all(|b| (b'0'..=b'7').contains(&b)) {
            parse_integer_with_radix(digits, 8)
        } else {
            s.parse::<f64>().unwrap_or(f64::NAN)
        }
    } else {
        s.parse::<f64>().unwrap_or(f64::NAN)
    }
}

fn parse_integer_with_radix(digits: &str, radix: u32) -> f64 {
    if let Ok(v) = u64::from_str_radix(digits, radix) {
        return v as f64;
    }
    let mut result: f64 = 0.0;
    for ch in digits.chars() {
        let digit = ch.to_digit(radix);
        if let Some(d) = digit {
            result = result * (radix as f64) + (d as f64);
        }
    }
    result
}
