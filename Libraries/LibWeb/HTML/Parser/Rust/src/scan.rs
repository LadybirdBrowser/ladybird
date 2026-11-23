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
pub trait CodeUnit: Copy + Eq + Into<u16> + From<u8> {
    /// Whether this is half of a surrogate pair. One-byte code units never are.
    fn is_surrogate(self) -> bool;
}

impl CodeUnit for u8 {
    #[inline(always)]
    fn is_surrogate(self) -> bool {
        false
    }
}

impl CodeUnit for u16 {
    #[inline(always)]
    fn is_surrogate(self) -> bool {
        self & 0xF800 == 0xD800
    }
}

const CHUNK_SIZE: usize = 32;

// Most runs of text in markup are short, and are over before a chunk would be.
const SHORT_RUN_LENGTH: usize = 16;

/// The offset of the first code unit that `stops` accepts, or the length of `code_units` if there is none. The bits of
/// the code units before it are added to `seen_bits`.
#[inline(always)]
fn find_first<T: CodeUnit>(code_units: &[T], stops: impl Fn(T) -> bool, seen_bits: &mut u16) -> usize {
    let scan_one_by_one = |range: std::ops::Range<usize>, seen_bits: &mut u16| {
        for offset in range {
            let code_unit = code_units[offset];
            if stops(code_unit) {
                return Some(offset);
            }
            *seen_bits |= code_unit.into();
        }
        None
    };

    let mut offset = code_units.len().min(SHORT_RUN_LENGTH);
    if let Some(stop) = scan_one_by_one(0..offset, seen_bits) {
        return stop;
    }
    let (chunks, _) = code_units[offset..].as_chunks::<CHUNK_SIZE>();
    for chunk in chunks {
        let mut any_stops = false;
        let mut bits = 0;
        for &code_unit in chunk {
            any_stops |= stops(code_unit);
            bits |= code_unit.into();
        }
        if any_stops {
            break;
        }
        *seen_bits |= bits;
        offset += CHUNK_SIZE;
    }
    scan_one_by_one(offset..code_units.len(), seen_bits).unwrap_or(code_units.len())
}

/// A run of code units at the start of some input.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Run {
    pub length: usize,
    pub is_ascii: bool,
}

/// The run of code units at the start of `code_units` that the data state emits as they are: everything up to the
/// first U+003C (<), U+0026 (&), U+0000 NULL, U+000D CR, or surrogate that isn't half of a pair.
pub fn data_run<T: CodeUnit>(code_units: &[T]) -> Run {
    let stops = |code_unit: T| {
        code_unit == T::from(b'<')
            || code_unit == T::from(b'&')
            || code_unit == T::from(0)
            || code_unit == T::from(b'\r')
            || code_unit.is_surrogate()
    };
    let mut length = 0;
    let mut seen_bits = 0;
    loop {
        length += find_first(&code_units[length..], stops, &mut seen_bits);
        let Some(&[high, low]) = code_units.get(length..length + 2) else {
            break;
        };
        if !is_surrogate_pair(high.into(), low.into()) {
            break;
        }
        seen_bits |= high.into();
        length += 2;
    }
    Run {
        length,
        is_ascii: seen_bits < 0x80,
    }
}

/// Whether any code unit is something other than the ASCII whitespace the tree builder ignores for "frameset-ok".
pub fn contains_non_whitespace<T: CodeUnit>(code_units: &[T]) -> bool {
    code_units
        .iter()
        .any(|&code_unit| !matches!(code_unit.into(), 0x09 | 0x0A | 0x0C | 0x0D | 0x20))
}

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

    fn run(length: usize, is_ascii: bool) -> Run {
        Run { length, is_ascii }
    }

    #[test]
    fn data_runs_stop_at_markup_and_characters_the_tokenizer_changes() {
        assert_eq!(data_run(b"plain text" as &[u8]), run(10, true));
        assert_eq!(data_run(b"" as &[u8]), run(0, true));
        assert_eq!(data_run(b"a<b" as &[u8]), run(1, true));
        assert_eq!(data_run(b"a&b" as &[u8]), run(1, true));
        assert_eq!(data_run(b"a\0b" as &[u8]), run(1, true));
        assert_eq!(data_run(b"a\r\nb" as &[u8]), run(1, true));
        assert_eq!(data_run(b"caf\xe9\n<" as &[u8]), run(5, false));
        assert_eq!(data_run(b"caf<\xe9" as &[u8]), run(3, true));
    }

    #[test]
    fn data_runs_find_stops_and_non_ascii_at_every_offset() {
        // Long enough to put a stop in the short run the scan starts with, in the chunks after it, in the remainder
        // after those, and on the seams.
        let length = SHORT_RUN_LENGTH + CHUNK_SIZE * 3;
        for offset in 0..length {
            let mut input = vec![b'x'; length];
            input[offset] = b'<';
            assert_eq!(data_run(&input), run(offset, true));
            let mut wide: Vec<u16> = input.iter().copied().map(u16::from).collect();
            assert_eq!(data_run(&wide), run(offset, true));

            input[offset] = 0xe9;
            assert_eq!(data_run(&input), run(length, false));
            wide[offset] = 0x2014;
            assert_eq!(data_run(&wide), run(length, false));
        }
        assert_eq!(data_run(&vec![b'x'; length]), run(length, true));
    }

    #[test]
    fn data_runs_take_surrogate_pairs_but_not_lone_surrogates() {
        assert_eq!(data_run(&code_units("a\u{1f600}b\u{1f600}<")), run(6, false));
        assert_eq!(data_run::<u16>(&[0x61, 0xD83D]), run(1, true));
        assert_eq!(data_run::<u16>(&[0x61, 0xD83D, 0x62]), run(1, true));
        assert_eq!(data_run::<u16>(&[0x61, 0xDE00, 0xD83D]), run(1, true));
        assert_eq!(data_run::<u16>(&[0x61, 0xD83D, 0xDE00]), run(3, false));
    }

    #[test]
    fn finds_non_whitespace() {
        assert!(!contains_non_whitespace(b" \t\n\x0c" as &[u8]));
        assert!(contains_non_whitespace(b" \t\n\x0c." as &[u8]));
        assert!(contains_non_whitespace(&code_units(" \u{a0}")));
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
