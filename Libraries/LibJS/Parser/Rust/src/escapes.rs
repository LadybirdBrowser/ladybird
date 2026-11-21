/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The escape sequences of string literals and templates. The lexer only validates them, and their value gets worked
//! out when something needs it, one code unit at a time, into whatever the caller keeps it in.

use super::unit::Unit;

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

/// Unified escape sequence processor for both string and template literals, which hands each code unit of the value to
/// `push`.
///
/// In `TaggedTemplate` mode, the result is true when an invalid escape is encountered (the cooked value becomes
/// `undefined`), and the code units up to it are all that were pushed.
#[inline]
pub fn decode_escape_sequences<U: Unit>(input: &[U], mode: EscapeMode, mut push: impl FnMut(u16)) -> bool {
    let at = |index: usize| input[index].code() as u16;
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

    while i < input.len() {
        if at(i) == b'\\' as u16 && i + 1 < input.len() {
            i += 1;
            match at(i) {
                N => push(ch(b'\n')),
                R => push(ch(b'\r')),
                T => push(ch(b'\t')),
                B => push(8),
                F => push(12),
                V => push(11),
                ZERO => {
                    if mode == EscapeMode::TaggedTemplate {
                        if i + 1 < input.len() && (is_octal_char(at(i + 1)) || at(i + 1) == EIGHT || at(i + 1) == NINE)
                        {
                            return true;
                        }
                        push(0);
                    } else if i + 1 < input.len() && is_octal_char(at(i + 1)) {
                        let (val, consumed) = parse_octal_escape(input, i);
                        push(val);
                        i += consumed;
                    } else {
                        push(0);
                    }
                }
                ONE..=SEVEN => {
                    if mode == EscapeMode::TaggedTemplate {
                        return true;
                    }
                    let (val, consumed) = parse_octal_escape(input, i);
                    push(val);
                    i += consumed;
                }
                EIGHT | NINE => {
                    if mode == EscapeMode::TaggedTemplate {
                        return true;
                    }
                    push(at(i));
                }
                X => {
                    if let Some((advance, ch)) = parse_hex_escape(input, i) {
                        push(ch);
                        i += advance;
                    } else if mode == EscapeMode::TaggedTemplate {
                        return true;
                    } else {
                        push(at(i));
                    }
                }
                U => {
                    if let Some((advance, code_point)) = parse_unicode_escape(input, i) {
                        if code_point > 0xFFFF {
                            let adjusted = code_point - 0x10000;
                            push((0xD800 | ((adjusted >> 10) & 0x3FF)) as u16);
                            push((0xDC00 | (adjusted & 0x3FF)) as u16);
                        } else {
                            push(code_point as u16);
                        }
                        i += advance;
                    } else if mode == EscapeMode::TaggedTemplate {
                        return true;
                    } else {
                        push(at(i));
                    }
                }
                LF => { /* line continuation */ }
                CR => {
                    if i + 1 < input.len() && at(i + 1) == LF {
                        i += 1;
                    }
                }
                LS | PS => { /* skip LS/PS */ }
                c => push(c),
            }
        } else if at(i) == ch(b'\r') {
            // Normalize \r\n and bare \r to \n per spec (12.9.6).
            push(ch(b'\n'));
            if i + 1 < input.len() && at(i + 1) == ch(b'\n') {
                i += 1;
            }
        } else {
            push(at(i));
        }
        i += 1;
    }
    false
}

/// Whether the value of a string literal, from the text between its quotes, is `text`, which is ASCII.
pub fn string_literal_equals<U: Unit>(units: &[U], text: &str) -> bool {
    debug_assert!(text.is_ascii());
    let text = text.as_bytes();
    if !units.iter().any(|unit| unit.code() == u32::from(b'\\')) {
        return units.len() == text.len()
            && units
                .iter()
                .zip(text)
                .all(|(unit, &byte)| unit.code() == u32::from(byte));
    }
    let mut length = 0;
    let mut is_equal = true;
    decode_escape_sequences(units, EscapeMode::StringLiteral, |unit| {
        is_equal &= text.get(length).is_some_and(|&byte| u16::from(byte) == unit);
        length += 1;
    });
    is_equal && length == text.len()
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

fn parse_octal_escape<U: Unit>(inner: &[U], i: usize) -> (u16, usize) {
    let at = |index: usize| inner[index].code() as u16;
    let first = (at(i) - ch(b'0')) as u32;
    let mut value = first;
    let mut consumed = 0;

    if i + 1 < inner.len() && is_octal_char(at(i + 1)) {
        value = value * 8 + (at(i + 1) - ch(b'0')) as u32;
        consumed = 1;

        if i + 2 < inner.len() && is_octal_char(at(i + 2)) && first <= 3 {
            value = value * 8 + (at(i + 2) - ch(b'0')) as u32;
            consumed = 2;
        }
    }
    (value as u16, consumed)
}

fn parse_hex_escape<U: Unit>(raw: &[U], i: usize) -> Option<(usize, u16)> {
    let at = |index: usize| raw[index].code() as u16;
    if i + 2 >= raw.len() {
        return None;
    }
    let high = hex_digit(at(i + 1))?;
    let low = hex_digit(at(i + 2))?;
    Some((2, high * 16 + low))
}

fn parse_unicode_escape<U: Unit>(raw: &[U], i: usize) -> Option<(usize, u32)> {
    let at = |index: usize| raw[index].code() as u16;
    if i + 1 >= raw.len() {
        return None;
    }
    if at(i + 1) == ch(b'{') {
        let mut j = i + 2;
        let mut value: u32 = 0;
        let mut digits = 0;
        while j < raw.len() && at(j) != ch(b'}') {
            let d = hex_digit(at(j))? as u32;
            value = value * 16 + d;
            if value > 0x10FFFF {
                return None;
            }
            digits += 1;
            j += 1;
        }
        if j >= raw.len() || digits == 0 {
            return None;
        }
        Some((j - i, value))
    } else {
        if i + 4 >= raw.len() {
            return None;
        }
        let d0 = hex_digit(at(i + 1))? as u32;
        let d1 = hex_digit(at(i + 2))? as u32;
        let d2 = hex_digit(at(i + 3))? as u32;
        let d3 = hex_digit(at(i + 4))? as u32;
        Some((4, (d0 << 12) | (d1 << 8) | (d2 << 4) | d3))
    }
}
