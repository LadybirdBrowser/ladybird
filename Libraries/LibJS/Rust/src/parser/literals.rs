/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Turning the source text of literals into their values. The lexer only validates literals, and a value gets computed
//! when something needs it.

use crate::utf16::Utf16View;
use ak::Utf16String;

const fn ch(c: u8) -> u16 {
    c as u16
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum EscapeMode {
    /// Tagged template literals: invalid escapes produce `undefined` cooked value.
    TaggedTemplate,
    /// String literals: invalid escapes emit syntax errors, legacy octals are tracked.
    StringLiteral,
}

pub(crate) struct EscapeResult {
    pub(crate) value: Utf16String,
    /// Tagged template encountered an invalid escape.
    pub(crate) failed: bool,
}

/// Unified escape sequence processor for both string and template literals.
///
/// In `TaggedTemplate` mode, `failed` is true when an invalid escape is
/// encountered (the cooked value becomes `undefined`).
pub(crate) fn process_escape_sequences_impl(input: Utf16View<'_>, mode: EscapeMode) -> EscapeResult {
    let mut result = Vec::with_capacity(input.length_in_code_units());
    let mut i = 0;

    const N: u16 = ch(b'n');
    const R: u16 = ch(b'r');
    const T: u16 = ch(b't');
    const B: u16 = ch(b'b');
    const F: u16 = ch(b'f');
    const V: u16 = ch(b'v');
    const ZERO: u16 = ch(b'0');
    const ONE: u16 = ch(b'1');
    const SEVEN: u16 = ch(b'7');
    const EIGHT: u16 = ch(b'8');
    const NINE: u16 = ch(b'9');
    const X: u16 = ch(b'x');
    const U: u16 = ch(b'u');
    const LF: u16 = ch(b'\n');
    const CR: u16 = ch(b'\r');
    const LS: u16 = 0x2028;
    const PS: u16 = 0x2029;

    while i < input.length_in_code_units() {
        if input.code_unit_at(i) == b'\\' as u16 && i + 1 < input.length_in_code_units() {
            i += 1;
            match input.code_unit_at(i) {
                N => result.push(ch(b'\n')),
                R => result.push(ch(b'\r')),
                T => result.push(ch(b'\t')),
                B => result.push(8),
                F => result.push(12),
                V => result.push(11),
                ZERO => {
                    if mode == EscapeMode::TaggedTemplate {
                        if i + 1 < input.length_in_code_units()
                            && (is_octal_char(input.code_unit_at(i + 1))
                                || input.code_unit_at(i + 1) == EIGHT
                                || input.code_unit_at(i + 1) == NINE)
                        {
                            return EscapeResult {
                                value: Utf16String::from_utf16(&result),
                                failed: true,
                            };
                        }
                        result.push(0);
                    } else if i + 1 < input.length_in_code_units() && is_octal_char(input.code_unit_at(i + 1)) {
                        let (val, consumed) = parse_octal_escape(input, i);
                        result.push(val);
                        i += consumed;
                    } else {
                        result.push(0);
                    }
                }
                ONE..=SEVEN => {
                    if mode == EscapeMode::TaggedTemplate {
                        return EscapeResult {
                            value: Utf16String::from_utf16(&result),
                            failed: true,
                        };
                    }
                    let (val, consumed) = parse_octal_escape(input, i);
                    result.push(val);
                    i += consumed;
                }
                EIGHT | NINE => {
                    if mode == EscapeMode::TaggedTemplate {
                        return EscapeResult {
                            value: Utf16String::from_utf16(&result),
                            failed: true,
                        };
                    }
                    result.push(input.code_unit_at(i));
                }
                X => {
                    if let Some((advance, ch)) = parse_hex_escape(input, i) {
                        result.push(ch);
                        i += advance;
                    } else if mode == EscapeMode::TaggedTemplate {
                        return EscapeResult {
                            value: Utf16String::from_utf16(&result),
                            failed: true,
                        };
                    } else {
                        result.push(input.code_unit_at(i));
                    }
                }
                U => {
                    if let Some((advance, code_point)) = parse_unicode_escape(input, i) {
                        push_code_point(&mut result, code_point);
                        i += advance;
                    } else if mode == EscapeMode::TaggedTemplate {
                        return EscapeResult {
                            value: Utf16String::from_utf16(&result),
                            failed: true,
                        };
                    } else {
                        result.push(input.code_unit_at(i));
                    }
                }
                LF => { /* line continuation */ }
                CR => {
                    if i + 1 < input.length_in_code_units() && input.code_unit_at(i + 1) == LF {
                        i += 1;
                    }
                }
                LS | PS => { /* skip LS/PS */ }
                c => result.push(c),
            }
        } else if input.code_unit_at(i) == ch(b'\r') {
            // Normalize \r\n and bare \r to \n per spec (12.9.6).
            result.push(ch(b'\n'));
            if i + 1 < input.length_in_code_units() && input.code_unit_at(i + 1) == ch(b'\n') {
                i += 1;
            }
        } else {
            result.push(input.code_unit_at(i));
        }
        i += 1;
    }
    EscapeResult {
        value: Utf16String::from_utf16(&result),
        failed: false,
    }
}

fn hex_digit(c: u16) -> Option<u16> {
    match c {
        0x30..=0x39 => Some(c - 0x30),
        0x41..=0x46 => Some(c - 0x41 + 10),
        0x61..=0x66 => Some(c - 0x61 + 10),
        _ => None,
    }
}

fn is_octal_char(c: u16) -> bool {
    c >= ch(b'0') && c <= ch(b'7')
}

fn parse_octal_escape(inner: Utf16View<'_>, i: usize) -> (u16, usize) {
    let first = (inner.code_unit_at(i) - ch(b'0')) as u32;
    let mut value = first;
    let mut consumed = 0;

    if i + 1 < inner.length_in_code_units() && is_octal_char(inner.code_unit_at(i + 1)) {
        value = value * 8 + (inner.code_unit_at(i + 1) - ch(b'0')) as u32;
        consumed = 1;

        if i + 2 < inner.length_in_code_units() && is_octal_char(inner.code_unit_at(i + 2)) && first <= 3 {
            value = value * 8 + (inner.code_unit_at(i + 2) - ch(b'0')) as u32;
            consumed = 2;
        }
    }
    (value as u16, consumed)
}

fn parse_hex_escape(raw: Utf16View<'_>, i: usize) -> Option<(usize, u16)> {
    if i + 2 >= raw.length_in_code_units() {
        return None;
    }
    let high = hex_digit(raw.code_unit_at(i + 1))?;
    let low = hex_digit(raw.code_unit_at(i + 2))?;
    Some((2, high * 16 + low))
}

fn parse_unicode_escape(raw: Utf16View<'_>, i: usize) -> Option<(usize, u32)> {
    if i + 1 >= raw.length_in_code_units() {
        return None;
    }
    if raw.code_unit_at(i + 1) == ch(b'{') {
        let mut j = i + 2;
        let mut value: u32 = 0;
        let mut digits = 0;
        while j < raw.length_in_code_units() && raw.code_unit_at(j) != ch(b'}') {
            let d = hex_digit(raw.code_unit_at(j))? as u32;
            value = value * 16 + d;
            if value > 0x10FFFF {
                return None;
            }
            digits += 1;
            j += 1;
        }
        if j >= raw.length_in_code_units() || digits == 0 {
            return None;
        }
        Some((j - i, value))
    } else {
        if i + 4 >= raw.length_in_code_units() {
            return None;
        }
        let d0 = hex_digit(raw.code_unit_at(i + 1))? as u32;
        let d1 = hex_digit(raw.code_unit_at(i + 2))? as u32;
        let d2 = hex_digit(raw.code_unit_at(i + 3))? as u32;
        let d3 = hex_digit(raw.code_unit_at(i + 4))? as u32;
        Some((4, (d0 << 12) | (d1 << 8) | (d2 << 4) | d3))
    }
}

fn push_code_point(result: &mut Vec<u16>, code_point: u32) {
    if code_point > 0xFFFF {
        let adjusted = code_point - 0x10000;
        result.push((0xD800 | ((adjusted >> 10) & 0x3FF)) as u16);
        result.push((0xDC00 | (adjusted & 0x3FF)) as u16);
    } else {
        result.push(code_point as u16);
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
