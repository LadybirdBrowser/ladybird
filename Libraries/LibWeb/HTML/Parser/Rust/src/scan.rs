/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Scans over runs of input code units.
//!
//! The loops here look at a fixed-size chunk of code units at a time and have no early exits inside a chunk, which
//! is the shape the compiler turns into vector instructions for whatever the target CPU has.

use crate::token::Position;

/// A code unit of the input stream, in either of the forms it is stored in.
pub trait CodeUnit: Copy + Eq + Into<u16> + From<u8> {}

impl CodeUnit for u8 {}

impl CodeUnit for u16 {}

const CHUNK_SIZE: usize = 32;

/// The number of U+000A LF and of U+000D CR code units.
fn count_line_feeds_and_carriage_returns<T: CodeUnit>(code_units: &[T]) -> (usize, usize) {
    let mut line_feeds = 0;
    let mut carriage_returns = 0;
    let (chunks, remainder) = code_units.as_chunks::<CHUNK_SIZE>();
    for chunk in chunks {
        // NB: A chunk has fewer than 256 code units, so the narrow counters the vector loop wants can't overflow.
        let mut line_feeds_in_chunk = 0u8;
        let mut carriage_returns_in_chunk = 0u8;
        for &code_unit in chunk {
            line_feeds_in_chunk += u8::from(code_unit == T::from(b'\n'));
            carriage_returns_in_chunk += u8::from(code_unit == T::from(b'\r'));
        }
        line_feeds += usize::from(line_feeds_in_chunk);
        carriage_returns += usize::from(carriage_returns_in_chunk);
    }
    for &code_unit in remainder {
        line_feeds += usize::from(code_unit == T::from(b'\n'));
        carriage_returns += usize::from(code_unit == T::from(b'\r'));
    }
    (line_feeds, carriage_returns)
}

#[inline(always)]
fn is_surrogate_pair(high: u16, low: u16) -> bool {
    (0xD800..=0xDBFF).contains(&high) && (0xDC00..=0xDFFF).contains(&low)
}

/// The source position the tokenizer is at after it consumes `code_units` from `position`.
///
/// A line ends at a U+000A LF, a U+000D CR, or the two together, and a column is a code point. `code_units` must
/// start and end between code points, and not between a CR and the LF it belongs with.
pub fn position_after<T: CodeUnit>(position: Position, code_units: &[T]) -> Position {
    let is_line_end = |code_unit: T| code_unit == T::from(b'\n') || code_unit == T::from(b'\r');
    let Some(last_line_end) = code_units.iter().rposition(|&code_unit| is_line_end(code_unit)) else {
        return Position {
            line: position.line,
            column: position.column + code_point_count(code_units) as u64,
        };
    };
    let lines = &code_units[..=last_line_end];
    let (line_feeds, carriage_returns) = count_line_feeds_and_carriage_returns(lines);
    let line_ends = if carriage_returns == 0 {
        line_feeds
    } else {
        // NB: A CR that is followed by an LF ends the same line the LF does.
        let pairs = lines
            .windows(2)
            .filter(|pair| pair[0] == T::from(b'\r') && pair[1] == T::from(b'\n'))
            .count();
        line_feeds + carriage_returns - pairs
    };
    Position {
        line: position.line + line_ends as u64,
        column: code_point_count(&code_units[last_line_end + 1..]) as u64,
    }
}

fn code_point_count<T: CodeUnit>(code_units: &[T]) -> usize {
    let surrogate_pairs = code_units
        .windows(2)
        .filter(|pair| is_surrogate_pair(pair[0].into(), pair[1].into()))
        .count();
    code_units.len() - surrogate_pairs
}

#[cfg(test)]
mod tests {
    use super::*;

    fn code_units(input: &str) -> Vec<u16> {
        input.encode_utf16().collect()
    }

    fn position_after_all<T: CodeUnit>(code_units: &[T]) -> (u64, u64) {
        let position = position_after(Position { line: 3, column: 5 }, code_units);
        (position.line, position.column)
    }

    #[test]
    fn columns_count_code_points() {
        assert_eq!(position_after_all(b"" as &[u8]), (3, 5));
        assert_eq!(position_after_all(b"one" as &[u8]), (3, 8));
        assert_eq!(position_after_all(b"caf\xe9" as &[u8]), (3, 9));
        assert_eq!(position_after_all(&code_units("a\u{1f600}\u{e9}")), (3, 8));
        // A surrogate without its other half is a code point of its own.
        assert_eq!(position_after_all::<u16>(&[0xD83D, 0x61, 0xDE00]), (3, 8));
    }

    #[test]
    fn lines_end_at_line_feeds_and_carriage_returns() {
        assert_eq!(position_after_all(b"one\ntwo\nth" as &[u8]), (5, 2));
        assert_eq!(position_after_all(b"one\n" as &[u8]), (4, 0));
        assert_eq!(position_after_all(b"one\r\ntwo\rthree\n\rfour" as &[u8]), (7, 4));
        assert_eq!(position_after_all(b"\r\r\n\n" as &[u8]), (6, 0));
        assert_eq!(position_after_all(&code_units("\u{1f600}\na\u{1f600}\u{e9}")), (4, 3));
        let long = vec![b'\n'; CHUNK_SIZE * 9 + 5];
        assert_eq!(position_after_all(&long), (3 + (CHUNK_SIZE * 9 + 5) as u64, 0));
    }
}
