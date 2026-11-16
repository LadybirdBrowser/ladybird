/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Character classes of XML 1.0 (fifth edition) and strict UTF-8 decoding.

/// S ::= (#x20 | #x9 | #xD | #xA)+
pub const SPACE: u8 = 1 << 0;
/// An ASCII NameStartChar, including the colon.
pub const NAME_START: u8 = 1 << 1;
/// An ASCII NameChar, including the colon.
pub const NAME: u8 = 1 << 2;
/// PubidChar ::= #x20 | #xD | #xA | [a-zA-Z0-9] | [-'()+,./:=?;!*#@$_%]
pub const PUBID: u8 = 1 << 3;
/// An ASCII character that can't appear anywhere in a document: the C0 controls other than tab, LF and CR.
pub const INVALID: u8 = 1 << 4;
pub const DIGIT: u8 = 1 << 5;
pub const HEX: u8 = 1 << 6;

pub static CLASS: [u8; 256] = build_class_table();

const fn build_class_table() -> [u8; 256] {
    let mut table = [0u8; 256];
    let mut i = 0;
    while i < 128 {
        let c = i as u8;
        let mut class = 0;
        if matches!(c, b' ' | b'\t' | b'\r' | b'\n') {
            class |= SPACE;
        }
        if c.is_ascii_alphabetic() || c == b'_' || c == b':' {
            class |= NAME_START | NAME;
        }
        if c.is_ascii_digit() || c == b'-' || c == b'.' {
            class |= NAME;
        }
        if c.is_ascii_alphanumeric()
            || matches!(
                c,
                b' ' | b'\r'
                    | b'\n'
                    | b'-'
                    | b'\''
                    | b'('
                    | b')'
                    | b'+'
                    | b','
                    | b'.'
                    | b'/'
                    | b':'
                    | b'='
                    | b'?'
                    | b';'
                    | b'!'
                    | b'*'
                    | b'#'
                    | b'@'
                    | b'$'
                    | b'_'
                    | b'%'
            )
        {
            class |= PUBID;
        }
        if c < 0x20 && !matches!(c, b'\t' | b'\r' | b'\n') {
            class |= INVALID;
        }
        if c.is_ascii_digit() {
            class |= DIGIT;
        }
        if c.is_ascii_hexdigit() {
            class |= HEX;
        }
        table[i] = class;
        i += 1;
    }
    table
}

#[inline(always)]
pub fn is_space(byte: u8) -> bool {
    CLASS[byte as usize] & SPACE != 0
}

/// Char ::= #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD] | [#x10000-#x10FFFF]
#[inline]
pub fn is_char(c: u32) -> bool {
    if c < 0x20 {
        return matches!(c, 0x9 | 0xA | 0xD);
    }
    c <= 0xD7FF || (0xE000..=0xFFFD).contains(&c) || (0x10000..=0x10FFFF).contains(&c)
}

/// NameStartChar, for code points at or above U+0080.
pub fn is_name_start_char(c: u32) -> bool {
    matches!(c,
        0xC0..=0xD6
        | 0xD8..=0xF6
        | 0xF8..=0x2FF
        | 0x370..=0x37D
        | 0x37F..=0x1FFF
        | 0x200C..=0x200D
        | 0x2070..=0x218F
        | 0x2C00..=0x2FEF
        | 0x3001..=0xD7FF
        | 0xF900..=0xFDCF
        | 0xFDF0..=0xFFFD
        | 0x10000..=0xEFFFF)
}

/// NameChar, for code points at or above U+0080.
pub fn is_name_char(c: u32) -> bool {
    is_name_start_char(c) || matches!(c, 0xB7 | 0x300..=0x36F | 0x203F..=0x2040)
}

/// Decodes the UTF-8 sequence that starts at `bytes[pos]`, which must be a byte at or above 0x80. Returns the code
/// point and its length, or None for anything that isn't well-formed UTF-8: a stray continuation byte, an overlong
/// form, a surrogate, a value past U+10FFFF, or a sequence cut off by the end of the input.
#[inline]
pub fn decode_utf8(bytes: &[u8], pos: usize) -> Option<(u32, usize)> {
    let b0 = bytes[pos] as u32;
    let continuation = |index: usize| -> Option<u32> {
        let byte = *bytes.get(index)? as u32;
        if byte & 0xC0 == 0x80 { Some(byte & 0x3F) } else { None }
    };
    if b0 < 0xC2 {
        return None;
    }
    if b0 < 0xE0 {
        let c1 = continuation(pos + 1)?;
        return Some(((b0 & 0x1F) << 6 | c1, 2));
    }
    if b0 < 0xF0 {
        let c1 = continuation(pos + 1)?;
        let c2 = continuation(pos + 2)?;
        let c = (b0 & 0x0F) << 12 | c1 << 6 | c2;
        if c < 0x800 || (0xD800..=0xDFFF).contains(&c) {
            return None;
        }
        return Some((c, 3));
    }
    if b0 < 0xF5 {
        let c1 = continuation(pos + 1)?;
        let c2 = continuation(pos + 2)?;
        let c3 = continuation(pos + 3)?;
        let c = (b0 & 0x07) << 18 | c1 << 12 | c2 << 6 | c3;
        if !(0x10000..=0x10FFFF).contains(&c) {
            return None;
        }
        return Some((c, 4));
    }
    None
}

/// Appends the UTF-8 encoding of `c`, which must be a Unicode scalar value.
pub fn encode_utf8(c: u32, out: &mut Vec<u8>) {
    let mut buffer = [0u8; 4];
    let c = char::from_u32(c).unwrap_or('\u{FFFD}');
    out.extend_from_slice(c.encode_utf8(&mut buffer).as_bytes());
}

/// Whether `bytes` is a Name. With `allow_colon` unset it has to be an NCName.
pub fn is_name(bytes: &[u8], allow_colon: bool) -> bool {
    let mut pos = 0;
    let mut first = true;
    while pos < bytes.len() {
        let byte = bytes[pos];
        if byte < 0x80 {
            let wanted = if first { NAME_START } else { NAME };
            if CLASS[byte as usize] & wanted == 0 || (byte == b':' && !allow_colon) {
                return false;
            }
            pos += 1;
        } else {
            let Some((c, length)) = decode_utf8(bytes, pos) else {
                return false;
            };
            if !(if first { is_name_start_char(c) } else { is_name_char(c) }) {
                return false;
            }
            pos += length;
        }
        first = false;
    }
    !first
}
