/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tokenizer. It runs on demand, one token ahead of the parser, because `/` and `}` mean different things depending
//! on where the parser is. No value is computed for numbers or strings here: a literal is validated and left in the
//! source, and only code that gets compiled reads it again.

use super::base::Parser;
use super::token::*;
use super::unit::*;

const fn keyword_kinds() -> [T; 36] {
    let mut kinds = [T::Eof; 36];
    let mut i = 0;
    while i < 36 {
        kinds[i] = KEYWORDS[i].1;
        i += 1;
    }
    kinds
}
static KEYWORD_KINDS: [T; 36] = keyword_kinds();

// https://tc39.es/ecma262/#prod-IdentifierStartChar
// IdentifierStartChar :: UnicodeIDStart | $ | _
pub fn is_id_start(c: u32) -> bool {
    if c < 128 {
        return (c as u8).is_ascii_alphabetic() || c == 0x24 || c == 0x5F;
    }
    libunicode_rust::character_types::code_point_has_identifier_start_property(c)
}

// https://tc39.es/ecma262/#prod-IdentifierPartChar
// IdentifierPartChar :: UnicodeIDContinue | $
pub fn is_id_continue(c: u32) -> bool {
    if c < 128 {
        return (c as u8).is_ascii_alphanumeric() || c == 0x24 || c == 0x5F;
    }
    matches!(c, 0x200C | 0x200D) || libunicode_rust::character_types::code_point_has_identifier_continue_property(c)
}

fn is_unicode_whitespace(c: u32) -> bool {
    matches!(c, 0xA0 | 0xFEFF | 0x1680 | 0x2000..=0x200A | 0x202F | 0x205F | 0x3000)
}

fn hex_value(c: u32) -> Option<u32> {
    char::from_u32(c)?.to_digit(16)
}

impl<U: Unit> Parser<'_, U> {
    /// The unit at `index`, which may be in the padding after the source.
    #[inline(always)]
    pub fn at(&self, index: usize) -> u32 {
        // SAFETY: The lexer never goes further than one block into the padding.
        unsafe { (*self.src.add(index)).code() }
    }

    #[inline(always)]
    fn block(&self, index: usize) -> Block {
        // SAFETY: As for at().
        unsafe { U::load(self.src.add(index)) }
    }

    #[inline(always)]
    pub(crate) fn next(&mut self) {
        self.prev_end = self.tok_end;
        self.prev_start = self.tok_start;
        self.lex();
    }

    #[inline(always)]
    fn token(&mut self, kind: T, end: usize) {
        self.tok = kind;
        self.tok_end = end as u32;
        self.pos = end as u32;
    }

    pub(crate) fn lex(&mut self) {
        let mut p = self.pos as usize;
        let mut newline = false;
        let c = loop {
            let c = self.at(p);
            match c {
                0x20 | 0x09 | 0x0B | 0x0C => p += 1,
                0x0A | 0x0D => {
                    newline = true;
                    p += 1;
                }
                0x2F => {
                    let d = self.at(p + 1);
                    if d == 0x2F {
                        p = self.skip_line_comment(p + 2);
                    } else if d == 0x2A {
                        match self.skip_block_comment(p + 2) {
                            Some((end, had_newline)) => {
                                p = end;
                                newline |= had_newline;
                            }
                            None => {
                                self.fail(self.len, "Unterminated multi-line comment");
                                return;
                            }
                        }
                    } else {
                        break c;
                    }
                }
                0x3C if !self.module && self.at(p + 1) == 0x21 && self.at(p + 2) == 0x2D && self.at(p + 3) == 0x2D => {
                    p = self.skip_line_comment(p + 4);
                }
                0x2D if !self.module
                    && (newline || self.tok_end == 0)
                    && self.at(p + 1) == 0x2D
                    && self.at(p + 2) == 0x3E =>
                {
                    p = self.skip_line_comment(p + 3);
                }
                _ if c >= 0x80 => {
                    if c == 0x2028 || c == 0x2029 {
                        newline = true;
                        p += 1;
                    } else if is_unicode_whitespace(c) {
                        p += 1;
                    } else {
                        break c;
                    }
                }
                _ => break c,
            }
        };
        self.nl_before = newline;
        self.tok_start = p as u32;

        if c >= 0x80 {
            self.lex_identifier_slow(p, false);
            return;
        }
        match c as u8 {
            b'a'..=b'z' | b'A'..=b'Z' | b'_' | b'$' => self.lex_identifier(p),
            b'0'..=b'9' => self.lex_number(p),
            b'"' | b'\'' => self.lex_string(p, c as u8),
            b'`' => self.token(T::Backtick, p + 1),
            b'(' => self.token(T::LParen, p + 1),
            b')' => self.token(T::RParen, p + 1),
            b'{' => self.token(T::LBrace, p + 1),
            b'}' => self.token(T::RBrace, p + 1),
            b'[' => self.token(T::LBracket, p + 1),
            b']' => self.token(T::RBracket, p + 1),
            b';' => self.token(T::Semi, p + 1),
            b',' => self.token(T::Comma, p + 1),
            b':' => self.token(T::Colon, p + 1),
            b'~' => self.token(T::Tilde, p + 1),
            b'.' => {
                let d = self.at(p + 1);
                if (0x30..=0x39).contains(&d) {
                    self.lex_number(p);
                } else if d == 0x2E && self.at(p + 2) == 0x2E {
                    self.token(T::Ellipsis, p + 3);
                } else {
                    self.token(T::Dot, p + 1);
                }
            }
            b'=' => {
                if self.at(p + 1) == 0x3D {
                    if self.at(p + 2) == 0x3D {
                        self.token(T::EqEqEq, p + 3);
                    } else {
                        self.token(T::EqEq, p + 2);
                    }
                } else if self.at(p + 1) == 0x3E {
                    self.token(T::Arrow, p + 2);
                } else {
                    self.token(T::Assign, p + 1);
                }
            }
            b'!' => {
                if self.at(p + 1) == 0x3D {
                    if self.at(p + 2) == 0x3D {
                        self.token(T::NotEqEq, p + 3);
                    } else {
                        self.token(T::NotEq, p + 2);
                    }
                } else {
                    self.token(T::Not, p + 1);
                }
            }
            b'+' => match self.at(p + 1) {
                0x2B => self.token(T::Inc, p + 2),
                0x3D => self.token(T::AddAssign, p + 2),
                _ => self.token(T::Plus, p + 1),
            },
            b'-' => match self.at(p + 1) {
                0x2D => self.token(T::Dec, p + 2),
                0x3D => self.token(T::SubAssign, p + 2),
                _ => self.token(T::Minus, p + 1),
            },
            b'*' => {
                if self.at(p + 1) == 0x2A {
                    if self.at(p + 2) == 0x3D {
                        self.token(T::ExpAssign, p + 3);
                    } else {
                        self.token(T::StarStar, p + 2);
                    }
                } else if self.at(p + 1) == 0x3D {
                    self.token(T::MulAssign, p + 2);
                } else {
                    self.token(T::Star, p + 1);
                }
            }
            b'/' => {
                if self.at(p + 1) == 0x3D {
                    self.token(T::DivAssign, p + 2);
                } else {
                    self.token(T::Slash, p + 1);
                }
            }
            b'%' => {
                if self.at(p + 1) == 0x3D {
                    self.token(T::ModAssign, p + 2);
                } else {
                    self.token(T::Percent, p + 1);
                }
            }
            b'<' => {
                if self.at(p + 1) == 0x3C {
                    if self.at(p + 2) == 0x3D {
                        self.token(T::ShlAssign, p + 3);
                    } else {
                        self.token(T::Shl, p + 2);
                    }
                } else if self.at(p + 1) == 0x3D {
                    self.token(T::Le, p + 2);
                } else {
                    self.token(T::Lt, p + 1);
                }
            }
            b'>' => {
                if self.at(p + 1) == 0x3E {
                    if self.at(p + 2) == 0x3E {
                        if self.at(p + 3) == 0x3D {
                            self.token(T::UShrAssign, p + 4);
                        } else {
                            self.token(T::UShr, p + 3);
                        }
                    } else if self.at(p + 2) == 0x3D {
                        self.token(T::ShrAssign, p + 3);
                    } else {
                        self.token(T::Shr, p + 2);
                    }
                } else if self.at(p + 1) == 0x3D {
                    self.token(T::Ge, p + 2);
                } else {
                    self.token(T::Gt, p + 1);
                }
            }
            b'&' => {
                if self.at(p + 1) == 0x26 {
                    if self.at(p + 2) == 0x3D {
                        self.token(T::LogAndAssign, p + 3);
                    } else {
                        self.token(T::LogAnd, p + 2);
                    }
                } else if self.at(p + 1) == 0x3D {
                    self.token(T::AndAssign, p + 2);
                } else {
                    self.token(T::BitAnd, p + 1);
                }
            }
            b'|' => {
                if self.at(p + 1) == 0x7C {
                    if self.at(p + 2) == 0x3D {
                        self.token(T::LogOrAssign, p + 3);
                    } else {
                        self.token(T::LogOr, p + 2);
                    }
                } else if self.at(p + 1) == 0x3D {
                    self.token(T::OrAssign, p + 2);
                } else {
                    self.token(T::BitOr, p + 1);
                }
            }
            b'^' => {
                if self.at(p + 1) == 0x3D {
                    self.token(T::XorAssign, p + 2);
                } else {
                    self.token(T::BitXor, p + 1);
                }
            }
            b'?' => {
                let d = self.at(p + 1);
                if d == 0x3F {
                    if self.at(p + 2) == 0x3D {
                        self.token(T::NullishAssign, p + 3);
                    } else {
                        self.token(T::Nullish, p + 2);
                    }
                } else if d == 0x2E && !(0x30..=0x39).contains(&self.at(p + 2)) {
                    self.token(T::QDot, p + 2);
                } else {
                    self.token(T::Question, p + 1);
                }
            }
            b'#' => self.lex_identifier_slow(p + 1, true),
            b'\\' => self.lex_identifier_slow(p, false),
            0 if p >= self.len as usize => {
                self.tok = T::Eof;
                self.tok_end = p as u32;
                self.pos = p as u32;
            }
            _ => {
                self.tok_start = p as u32;
                self.token(T::Invalid, p + 1);
            }
        }
    }

    fn skip_line_comment(&mut self, start: usize) -> usize {
        let mut p = start;
        let len = self.len as usize;
        loop {
            let x = self.block(p);
            // SAFETY: The block came from a load.
            let mut mask = unsafe { eq_mask(x, b'\n') | eq_mask(x, b'\r') };
            if U::WIDE {
                // SAFETY: As above.
                mask |= unsafe { high_mask(x) };
            }
            while mask != 0 {
                let q = p + mask.trailing_zeros() as usize;
                let c = self.at(q);
                if c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029 {
                    return q.min(len);
                }
                mask &= mask - 1;
            }
            p += BLOCK;
            if p >= len {
                return len;
            }
        }
    }

    fn skip_block_comment(&mut self, start: usize) -> Option<(usize, bool)> {
        let mut p = start;
        let len = self.len as usize;
        let mut newline = false;
        loop {
            let x = self.block(p);
            // SAFETY: The block came from a load.
            let mut mask = unsafe {
                let mut mask = eq_mask(x, b'*') | eq_mask(x, b'\n') | eq_mask(x, b'\r');
                if U::WIDE {
                    mask |= high_mask(x);
                }
                mask
            };
            while mask != 0 {
                let q = p + mask.trailing_zeros() as usize;
                match self.at(q) {
                    0x2A => {
                        if self.at(q + 1) == 0x2F && q + 1 < len {
                            return Some((q + 2, newline));
                        }
                    }
                    0x0A | 0x0D | 0x2028 | 0x2029 => newline = true,
                    _ => {}
                }
                mask &= mask - 1;
            }
            p += BLOCK;
            if p >= len {
                return None;
            }
        }
    }

    #[inline(always)]
    fn lex_identifier(&mut self, p: usize) {
        let x = self.block(p);
        // SAFETY: The block came from a load.
        let n = unsafe { (!ident_mask(x)).trailing_zeros() } as usize;
        let stop = self.at(p + n);
        if n > 16 || n == BLOCK || stop >= 0x80 || stop == 0x5C {
            self.lex_identifier_long(p);
            return;
        }
        // SAFETY: As above.
        let (k0, k1) = unsafe { key_of(x, n) };
        let id = self.interner.intern_key(k0, k1, n);
        self.tok_name = id;
        self.tok_escaped = false;
        self.tok = if id < KEYWORD_COUNT {
            KEYWORD_KINDS[id as usize]
        } else {
            T::Name
        };
        self.tok_end = (p + n) as u32;
        self.pos = (p + n) as u32;
    }

    /// An ASCII name of more than 16 units, or the start of something the slow path has to look at.
    #[inline(never)]
    fn lex_identifier_long(&mut self, p: usize) {
        let mut q = p;
        loop {
            // SAFETY: The block came from a load.
            let n = unsafe { (!ident_mask(self.block(q))).trailing_zeros() } as usize;
            q += n;
            if n < BLOCK {
                break;
            }
        }
        let stop = self.at(q);
        if stop >= 0x80 || stop == 0x5C {
            self.lex_identifier_slow(p, false);
            return;
        }
        self.name_buffer.clear();
        for i in p..q {
            let c = self.at(i) as u8;
            self.name_buffer.push(c);
        }
        let id = self.interner.intern_bytes(&self.name_buffer);
        self.tok_name = id;
        self.tok_escaped = false;
        self.tok = if id < KEYWORD_COUNT {
            KEYWORD_KINDS[id as usize]
        } else {
            T::Name
        };
        self.tok_end = q as u32;
        self.pos = q as u32;
    }

    fn code_point_at(&self, p: usize) -> (u32, usize) {
        let c = self.at(p);
        if U::WIDE && (0xD800..0xDC00).contains(&c) {
            let d = self.at(p + 1);
            if (0xDC00..0xE000).contains(&d) {
                return (0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00), 2);
            }
        }
        (c, 1)
    }

    /// `\uXXXX` or `\u{X...}` from the `u`. Returns the code point and the position after it.
    fn read_unicode_escape(&self, p: usize) -> Option<(u32, usize)> {
        if self.at(p) != 0x75 {
            return None;
        }
        if self.at(p + 1) == 0x7B {
            let mut q = p + 2;
            let mut value = 0u32;
            let mut digits = 0;
            while let Some(digit) = hex_value(self.at(q)) {
                value = value * 16 + digit;
                if value > 0x10FFFF {
                    return None;
                }
                digits += 1;
                q += 1;
            }
            if digits == 0 || self.at(q) != 0x7D {
                return None;
            }
            Some((value, q + 1))
        } else {
            let mut value = 0;
            for i in 1..=4 {
                value = value * 16 + hex_value(self.at(p + i))?;
            }
            Some((value, p + 5))
        }
    }

    #[inline(never)]
    fn lex_identifier_slow(&mut self, start: usize, private: bool) {
        let mut p = start;
        let mut escaped = false;
        let mut first = true;
        let mut buffer = std::mem::take(&mut self.name_buffer);
        buffer.clear();
        loop {
            let (mut c, width) = self.code_point_at(p);
            let mut next = p + width;
            if c == 0x5C {
                let Some((value, after)) = self.read_unicode_escape(p + 1) else {
                    self.fail(p as u32, "Malformed unicode escape sequence");
                    break;
                };
                c = value;
                next = after;
                escaped = true;
                let valid = if first { is_id_start(c) } else { is_id_continue(c) };
                if !valid {
                    self.fail(p as u32, "Invalid escaped identifier character");
                    break;
                }
            } else if !(if first { is_id_start(c) } else { is_id_continue(c) }) {
                break;
            }
            let mut encoded = [0u8; 4];
            let ch = char::from_u32(c).unwrap_or('\u{FFFD}');
            buffer.extend_from_slice(ch.encode_utf8(&mut encoded).as_bytes());
            first = false;
            p = next;
        }
        if self.err.is_some() {
            self.name_buffer = buffer;
            return;
        }
        if first {
            self.name_buffer = buffer;
            self.fail(p as u32, "Illegal character");
            return;
        }
        let id = self.interner.intern_bytes(&buffer);
        self.name_buffer = buffer;
        self.tok_name = id;
        self.tok_escaped = escaped;
        self.tok = if private {
            T::PrivateName
        } else if id < KEYWORD_COUNT {
            if escaped {
                T::EscapedKeyword
            } else {
                KEYWORD_KINDS[id as usize]
            }
        } else {
            T::Name
        };
        self.tok_end = p as u32;
        self.pos = p as u32;
    }

    #[inline(never)]
    fn lex_number(&mut self, start: usize) {
        let mut p = start;
        let first = self.at(p);
        let mut kind = T::Num;
        self.tok_legacy = false;
        let radix = if first == 0x30 {
            match self.at(p + 1) | 0x20 {
                0x78 => 16,
                0x6F => 8,
                0x62 => 2,
                _ => 10,
            }
        } else {
            10
        };
        if radix != 10 {
            p += 2;
            if !self.scan_digits(&mut p, radix, true) {
                return;
            }
            if self.at(p) == 0x6E {
                kind = T::BigInt;
                p += 1;
            }
        } else if first == 0x30 && (0x30..=0x39).contains(&self.at(p + 1)) {
            // Legacy octal, or a decimal with a leading zero.
            self.tok_legacy = true;
            let mut octal = true;
            while (0x30..=0x39).contains(&self.at(p)) {
                if self.at(p) >= 0x38 {
                    octal = false;
                }
                p += 1;
            }
            if self.at(p) == 0x5F {
                self.fail(
                    p as u32,
                    "Numeric literal must not be immediately followed by identifier",
                );
                return;
            }
            if !octal && !self.scan_fraction_and_exponent(&mut p) {
                return;
            }
        } else {
            if first != 0x2E {
                if !self.scan_digits(&mut p, 10, true) {
                    return;
                }
                if first == 0x30 && p != start + 1 {
                    self.fail(
                        start as u32,
                        "Numeric literal must not be immediately followed by identifier",
                    );
                    return;
                }
                if self.at(p) == 0x6E {
                    kind = T::BigInt;
                    p += 1;
                }
            }
            if kind == T::Num && !self.scan_fraction_and_exponent(&mut p) {
                return;
            }
        }
        let (c, _) = self.code_point_at(p);
        if is_id_start(c) || (0x30..=0x39).contains(&c) || c == 0x5C {
            self.fail(
                p as u32,
                "Numeric literal must not be immediately followed by identifier",
            );
            return;
        }
        self.token(kind, p);
    }

    fn scan_fraction_and_exponent(&mut self, p: &mut usize) -> bool {
        if self.at(*p) == 0x2E {
            *p += 1;
            if (0x30..=0x39).contains(&self.at(*p)) && !self.scan_digits(p, 10, true) {
                return false;
            }
            if self.at(*p) == 0x5F {
                self.fail(
                    *p as u32,
                    "Numeric literal must not be immediately followed by identifier",
                );
                return false;
            }
        }
        if self.at(*p) | 0x20 == 0x65 {
            *p += 1;
            if matches!(self.at(*p), 0x2B | 0x2D) {
                *p += 1;
            }
            if !self.scan_digits(p, 10, true) {
                return false;
            }
        }
        true
    }

    fn scan_digits(&mut self, p: &mut usize, radix: u32, separators: bool) -> bool {
        let start = *p;
        let mut last_separator = false;
        loop {
            let c = self.at(*p);
            if c == 0x5F && separators {
                if last_separator || *p == start {
                    self.fail(
                        *p as u32,
                        "Numeric literal must not be immediately followed by identifier",
                    );
                    return false;
                }
                last_separator = true;
                *p += 1;
                continue;
            }
            match char::from_u32(c).and_then(|ch| ch.to_digit(radix)) {
                Some(_) => {
                    last_separator = false;
                    *p += 1;
                }
                None => break,
            }
        }
        if last_separator {
            self.fail(
                *p as u32,
                "Numeric literal must not be immediately followed by identifier",
            );
            return false;
        }
        if *p == start {
            self.fail(start as u32, "Invalid numeric literal");
            return false;
        }
        true
    }

    #[inline(always)]
    fn lex_string(&mut self, start: usize, quote: u8) {
        self.tok_legacy = false;
        self.tok_escaped = false;
        let len = self.len as usize;
        let mut p = start + 1;
        loop {
            let x = self.block(p);
            // SAFETY: The block came from a load.
            let mask = unsafe { eq_mask(x, quote) | eq_mask(x, b'\\') | eq_mask(x, b'\n') | eq_mask(x, b'\r') };
            if mask == 0 {
                p += BLOCK;
                if p >= len {
                    self.fail(start as u32, "Unterminated string literal");
                    return;
                }
                continue;
            }
            p += mask.trailing_zeros() as usize;
            let c = self.at(p);
            if c == quote as u32 {
                self.token(T::Str, p + 1);
                return;
            }
            if c == 0x5C {
                self.tok_escaped = true;
                match self.skip_escape(p + 1) {
                    Some(after) => p = after,
                    None => return,
                }
            } else {
                self.fail(start as u32, "Unterminated string literal");
                return;
            }
        }
    }

    /// Validates a string escape from the unit after the backslash, and returns the position after it.
    #[inline(never)]
    fn skip_escape(&mut self, p: usize) -> Option<usize> {
        let c = self.at(p);
        match c {
            0x0D => Some(if self.at(p + 1) == 0x0A { p + 2 } else { p + 1 }),
            0x78 => {
                if hex_value(self.at(p + 1)).is_some() && hex_value(self.at(p + 2)).is_some() {
                    Some(p + 3)
                } else {
                    self.fail(p as u32, "Malformed hexadecimal escape sequence");
                    None
                }
            }
            0x75 => match self.read_unicode_escape(p) {
                Some((_, after)) => Some(after),
                None => {
                    self.fail(p as u32, "Malformed unicode escape sequence");
                    None
                }
            },
            0x30..=0x37 => {
                let next = self.at(p + 1);
                if c == 0x30 && !(0x30..=0x39).contains(&next) {
                    return Some(p + 1);
                }
                self.tok_legacy = true;
                let mut q = p + 1;
                if (0x30..=0x37).contains(&self.at(q)) {
                    q += 1;
                    if c <= 0x33 && (0x30..=0x37).contains(&self.at(q)) {
                        q += 1;
                    }
                }
                Some(q)
            }
            0x38 | 0x39 => {
                self.tok_legacy = true;
                Some(p + 1)
            }
            0 if p >= self.len as usize => {
                self.fail(p as u32, "Unterminated string literal");
                None
            }
            _ => Some(p + self.code_point_at(p).1),
        }
    }

    /// Reads template characters from `self.pos`. Returns whether the chunk ended the template, and whether it has an
    /// escape that only a tagged template may contain, and leaves `self.pos` after the closing `` ` `` or `${`.
    pub(crate) fn read_template_chunk(&mut self) -> (bool, bool) {
        let len = self.len as usize;
        let mut p = self.pos as usize;
        let mut invalid = false;
        loop {
            let x = self.block(p);
            // SAFETY: The block came from a load.
            let mask = unsafe { eq_mask(x, b'`') | eq_mask(x, b'$') | eq_mask(x, b'\\') };
            if mask == 0 {
                p += BLOCK;
                if p >= len {
                    break;
                }
                continue;
            }
            p += mask.trailing_zeros() as usize;
            if p >= len {
                break;
            }
            match self.at(p) {
                0x60 => {
                    self.pos = (p + 1) as u32;
                    return (true, invalid);
                }
                0x24 => {
                    if self.at(p + 1) == 0x7B {
                        self.pos = (p + 2) as u32;
                        return (false, invalid);
                    }
                    p += 1;
                }
                _ => {
                    let c = self.at(p + 1);
                    match c {
                        0x30 if !(0x30..=0x39).contains(&self.at(p + 2)) => p += 2,
                        0x30..=0x39 => {
                            invalid = true;
                            p += 2;
                        }
                        0x78 => {
                            if hex_value(self.at(p + 2)).is_some() && hex_value(self.at(p + 3)).is_some() {
                                p += 4;
                            } else {
                                invalid = true;
                                p += 2;
                            }
                        }
                        0x75 => match self.read_unicode_escape(p + 1) {
                            Some((_, after)) => p = after,
                            None => {
                                invalid = true;
                                p += 2;
                            }
                        },
                        0x0D if self.at(p + 2) == 0x0A => p += 3,
                        _ => p += 1 + self.code_point_at(p + 1).1,
                    }
                }
            }
        }
        self.fail(self.tok_start, "Unterminated template literal");
        (true, false)
    }

    /// Reads a regular expression literal in place of the `/` or `/=` token.
    pub(crate) fn rescan_regex(&mut self) {
        let start = self.tok_start as usize;
        let len = self.len as usize;
        let mut p = start + 1;
        let mut in_class = false;
        loop {
            if p >= len {
                self.fail(start as u32, "Unterminated regular expression literal");
                return;
            }
            let c = self.at(p);
            match c {
                0x0A | 0x0D | 0x2028 | 0x2029 => {
                    self.fail(start as u32, "Unterminated regular expression literal");
                    return;
                }
                0x5C => {
                    let d = self.at(p + 1);
                    if matches!(d, 0x0A | 0x0D | 0x2028 | 0x2029) {
                        self.fail(start as u32, "Unterminated regular expression literal");
                        return;
                    }
                    p += 2;
                    continue;
                }
                0x5B => in_class = true,
                0x5D => in_class = false,
                0x2F if !in_class => break,
                _ => {}
            }
            p += 1;
        }
        self.regex_body_end = p as u32;
        p += 1;
        let flags_start = p;
        loop {
            let (c, width) = self.code_point_at(p);
            if c == 0x5C {
                self.fail(p as u32, "Invalid regular expression flags");
                return;
            }
            if !is_id_continue(c) {
                break;
            }
            p += width;
        }
        let units_of = |parser: &Self, start: usize, end: usize| -> Vec<u16> {
            (start..end).map(|i| parser.at(i) as u16).collect()
        };
        let flags = units_of(self, flags_start, p);
        let mut seen = [false; 128];
        for &flag in &flags {
            if flag >= 128 || !b"dgimsuvy".contains(&(flag as u8)) {
                let flag = char::from_u32(flag as u32).unwrap_or('?');
                self.fail_with(self.tok_start, format!("Invalid RegExp flag '{flag}'"));
                return;
            }
            if seen[flag as usize] {
                let flag = flag as u8 as char;
                self.fail_with(self.tok_start, format!("Repeated RegExp flag '{flag}'"));
                return;
            }
            seen[flag as usize] = true;
        }
        let pattern = units_of(self, start + 1, self.regex_body_end as usize);
        if let Err(message) = crate::host::validate_regex(&pattern, &flags) {
            self.fail_with(start as u32, String::from_utf16_lossy(&message.to_utf16()));
            return;
        }
        self.token(T::Regex, p);
    }

    /// Whether the directive prologue at `start` contains "use strict". This looks ahead in the source so that the
    /// rest of the prologue is parsed knowing it.
    pub(crate) fn has_use_strict_directive(&mut self, start: usize) -> bool {
        let mut p = start;
        loop {
            p = self.skip_trivia_raw(p).0;
            let quote = self.at(p);
            if quote != 0x22 && quote != 0x27 {
                return false;
            }
            let content = p + 1;
            let mut q = content;
            loop {
                let c = self.at(q);
                if c == quote {
                    break;
                }
                if q >= self.len as usize || c == 0x0A || c == 0x0D {
                    return false;
                }
                q += if c == 0x5C { 2 } else { 1 };
            }
            let is_use_strict = q - content == 10 && (0..10).all(|i| self.at(content + i) == b"use strict"[i] as u32);
            let (after, newline) = self.skip_trivia_raw(q + 1);
            let next = self.at(after);
            if is_use_strict {
                if next == 0x3B || next == 0x7D || after >= self.len as usize {
                    return true;
                }
                let continues = matches!(
                    next as u8 as char,
                    '(' | '`' | '.' | '[' | '+' | '-' | '/' | '*' | '%' | '<' | '>' | '=' | ',' | '?' | '^' | '&' | '|'
                ) && next < 0x80
                    || (next == 0x21 && self.at(after + 1) == 0x3D);
                return newline && !continues;
            }
            p = after;
            if next == 0x3B {
                p += 1;
            }
        }
    }

    fn skip_trivia_raw(&mut self, start: usize) -> (usize, bool) {
        let mut p = start;
        let mut newline = false;
        loop {
            let c = self.at(p);
            match c {
                0x20 | 0x09 | 0x0B | 0x0C => p += 1,
                0x0A | 0x0D | 0x2028 | 0x2029 => {
                    newline = true;
                    p += 1;
                }
                0x2F if self.at(p + 1) == 0x2F => p = self.skip_line_comment(p + 2),
                0x2F if self.at(p + 1) == 0x2A => match self.skip_block_comment(p + 2) {
                    Some((end, had_newline)) => {
                        p = end;
                        newline |= had_newline;
                    }
                    None => return (p, newline),
                },
                _ if c >= 0x80 && is_unicode_whitespace(c) => p += 1,
                _ => return (p, newline),
            }
        }
    }
}
