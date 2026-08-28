/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use smallvec::SmallVec;

#[derive(Clone, Copy)]
pub(super) enum Input<'a> {
    Ascii(&'a [u8]),
    Utf16(&'a [u16]),
}

impl Input<'_> {
    fn len(self) -> usize {
        match self {
            Self::Ascii(input) => input.len(),
            Self::Utf16(input) => input.len(),
        }
    }

    fn code_unit_at(self, index: usize) -> u16 {
        match self {
            Self::Ascii(input) => u16::from(input[index]),
            Self::Utf16(input) => input[index],
        }
    }
}

pub(super) struct Parser<'a> {
    input: Input<'a>,
    position: usize,
}

impl<'a> Parser<'a> {
    pub(super) fn new(input: Input<'a>) -> Self {
        Self { input, position: 0 }
    }

    pub(super) fn done(&self) -> bool {
        self.position >= self.input.len()
    }

    pub(super) fn current_ascii(&self) -> Option<u8> {
        if self.done() { None } else { self.ascii_at(0) }
    }

    pub(super) fn current(&self) -> Option<u16> {
        (!self.done()).then(|| self.input.code_unit_at(self.position))
    }

    pub(super) fn ascii_at(&self, offset: usize) -> Option<u8> {
        let index = self.position.checked_add(offset)?;
        if index >= self.input.len() {
            return None;
        }
        u8::try_from(self.input.code_unit_at(index)).ok()
    }

    pub(super) fn consume(&mut self) -> u16 {
        let unit = self.input.code_unit_at(self.position);
        self.position += 1;
        unit
    }

    pub(super) fn consume_specific(&mut self, expected: &[u8]) -> bool {
        if expected
            .iter()
            .enumerate()
            .all(|(offset, expected)| self.ascii_at(offset) == Some(*expected))
        {
            self.position += expected.len();
            true
        } else {
            false
        }
    }

    pub(super) fn parse_whitespace(&mut self) {
        while self.current_ascii().is_some_and(is_whitespace) {
            self.consume();
        }
    }

    pub(super) fn parse_optional_comma_whitespace(&mut self) {
        if !self.matches_comma_whitespace() {
            return;
        }
        if self.current_ascii() == Some(b',') {
            self.consume();
            self.parse_whitespace();
        } else {
            self.parse_whitespace();
            if self.current_ascii() == Some(b',') {
                self.consume();
            }
            self.parse_whitespace();
        }
    }

    // https://www.w3.org/TR/SVG11/types.html#DataTypeNumber
    pub(super) fn parse_number(&mut self) -> Result<f32, ()> {
        let sign = match self.current_ascii() {
            Some(b'-') => {
                self.consume();
                -1.0
            }
            Some(b'+') => {
                self.consume();
                1.0
            }
            _ => 1.0,
        };
        Ok(sign * self.parse_nonnegative_number()?)
    }

    // https://www.w3.org/TR/SVG11/paths.html#PathDataBNF
    pub(super) fn parse_nonnegative_number(&mut self) -> Result<f32, ()> {
        let starts_with_digit = self.current_ascii().is_some_and(|unit| unit.is_ascii_digit());
        let starts_with_fraction =
            self.current_ascii() == Some(b'.') && self.ascii_at(1).is_some_and(|unit| unit.is_ascii_digit());
        if !starts_with_digit && !starts_with_fraction {
            return Err(());
        }
        let start = self.position;

        let mut integer_digits = 0;
        while self.current_ascii().is_some_and(|unit| unit.is_ascii_digit()) {
            integer_digits += 1;
            self.consume();
        }

        let mut fractional_digits = 0;
        if self.current_ascii() == Some(b'.') {
            self.consume();
            while self.current_ascii().is_some_and(|unit| unit.is_ascii_digit()) {
                fractional_digits += 1;
                self.consume();
            }
        }
        if integer_digits == 0 && fractional_digits == 0 {
            return Err(());
        }

        if matches!(self.current_ascii(), Some(b'e' | b'E')) {
            let exponent_start = self.position;
            self.consume();
            if matches!(self.current_ascii(), Some(b'+' | b'-')) {
                self.consume();
            }
            let digits_start = self.position;
            while self.current_ascii().is_some_and(|unit| unit.is_ascii_digit()) {
                self.consume();
            }
            if self.position == digits_start {
                self.position = exponent_start;
            }
        }

        let number = self.ascii_range(start, self.position)?;
        std::str::from_utf8(&number)
            .map_err(|_| ())?
            .parse::<f32>()
            .map_err(|_| ())
    }

    pub(super) fn parse_integer(&mut self) -> Result<i32, ()> {
        let start = self.position;
        if matches!(self.current_ascii(), Some(b'+' | b'-')) {
            self.consume();
        }
        let digits_start = self.position;
        while self.current_ascii().is_some_and(|unit| unit.is_ascii_digit()) {
            self.consume();
        }
        if self.position == digits_start {
            return Err(());
        }
        let integer = self.ascii_range(start, self.position)?;
        std::str::from_utf8(&integer)
            .map_err(|_| ())?
            .parse::<i32>()
            .map_err(|_| ())
    }

    pub(super) fn matches_coordinate(&self) -> bool {
        let Some(mut unit) = self.current_ascii() else {
            return false;
        };
        let mut offset = 0;
        if matches!(unit, b'+' | b'-') {
            offset += 1;
            let Some(next) = self.ascii_at(offset) else {
                return false;
            };
            unit = next;
        }
        if unit == b'.' {
            offset += 1;
            let Some(next) = self.ascii_at(offset) else {
                return false;
            };
            unit = next;
        }
        unit.is_ascii_digit()
    }

    pub(super) fn matches_comma_whitespace(&self) -> bool {
        self.current_ascii()
            .is_some_and(|unit| unit == b',' || is_whitespace(unit))
    }

    fn ascii_range(&self, start: usize, end: usize) -> Result<SmallVec<[u8; 32]>, ()> {
        let mut bytes = SmallVec::with_capacity(end - start);
        for index in start..end {
            bytes.push(u8::try_from(self.input.code_unit_at(index)).map_err(|_| ())?);
        }
        Ok(bytes)
    }
}

fn is_whitespace(unit: u8) -> bool {
    matches!(unit, b'\t' | b'\n' | 0x0c | b'\r' | b' ')
}
