/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Scans over runs of input code units.
//!
//! A scan for the end of a run looks at a block of 16 code units at a time: it asks of all of them at once which ones
//! end the run, and gets a bit for each. Most runs in markup are over within a block or two, so a scan has nothing to
//! set up and its first block is as cheap as any other.
//!
//! The scans that only count look at a fixed-size chunk of code units at a time with no early exits inside a chunk,
//! which is the shape the compiler turns into vector instructions for whatever the target CPU has.

use crate::token::Position;
use ak::Utf16View;

/// A code unit of the input stream, in either of the forms it is stored in.
pub trait CodeUnit: Copy + Default + Eq + Into<u16> + From<u8> {
    /// Loads a block of code units as bytes, with every code unit above U+00FE as 0xFF.
    ///
    /// # Safety
    /// `BLOCK_SIZE` code units must be readable at `code_units`.
    unsafe fn load_block(code_units: *const Self) -> block::Block;

    /// One bit for each code unit of a block: whether it is a surrogate.
    ///
    /// # Safety
    /// `BLOCK_SIZE` code units must be readable at `code_units`.
    unsafe fn surrogates_in_block(code_units: *const Self) -> u32;

    fn view(code_units: &[Self]) -> Utf16View<'_>;
}

impl CodeUnit for u8 {
    #[inline(always)]
    unsafe fn load_block(code_units: *const u8) -> block::Block {
        unsafe { block::load_narrow(code_units) }
    }

    #[inline(always)]
    unsafe fn surrogates_in_block(_: *const u8) -> u32 {
        0
    }

    #[inline(always)]
    fn view(code_units: &[u8]) -> Utf16View<'_> {
        Utf16View::Latin1(code_units)
    }
}

impl CodeUnit for u16 {
    #[inline(always)]
    unsafe fn load_block(code_units: *const u16) -> block::Block {
        unsafe { block::load_wide(code_units) }
    }

    #[inline(always)]
    unsafe fn surrogates_in_block(code_units: *const u16) -> u32 {
        unsafe { block::surrogates(code_units) }
    }

    #[inline(always)]
    fn view(code_units: &[u16]) -> Utf16View<'_> {
        Utf16View::Utf16(code_units)
    }
}

use block::BLOCK_SIZE;

#[cfg(target_arch = "x86_64")]
mod block {
    use core::arch::x86_64::*;

    pub(super) const BLOCK_SIZE: usize = 16;
    pub type Block = __m128i;

    #[inline(always)]
    pub unsafe fn load_narrow(code_units: *const u8) -> Block {
        unsafe { _mm_loadu_si128(code_units.cast()) }
    }

    #[inline(always)]
    pub unsafe fn load_wide(code_units: *const u16) -> Block {
        unsafe {
            // NB: x - saturating_sub(x, 0xFF) is min(x, 0xFF), with nothing that every x86-64 CPU doesn't have.
            let limit = _mm_set1_epi16(0xFF);
            let low = _mm_loadu_si128(code_units.cast());
            let high = _mm_loadu_si128(code_units.add(8).cast());
            let low = _mm_sub_epi16(low, _mm_subs_epu16(low, limit));
            let high = _mm_sub_epi16(high, _mm_subs_epu16(high, limit));
            _mm_packus_epi16(low, high)
        }
    }

    #[inline(always)]
    pub unsafe fn surrogates(code_units: *const u16) -> u32 {
        unsafe {
            let top_bits = _mm_set1_epi16(0xF800u16 as i16);
            let surrogate = _mm_set1_epi16(0xD800u16 as i16);
            let low = _mm_loadu_si128(code_units.cast());
            let high = _mm_loadu_si128(code_units.add(8).cast());
            let low = _mm_cmpeq_epi16(_mm_and_si128(low, top_bits), surrogate);
            let high = _mm_cmpeq_epi16(_mm_and_si128(high, top_bits), surrogate);
            _mm_movemask_epi8(_mm_packs_epi16(low, high)) as u32
        }
    }

    /// One bit for each code unit of a block: whether it is `byte`.
    #[inline(always)]
    pub fn equal_to(block: Block, byte: u8) -> u32 {
        unsafe { _mm_movemask_epi8(_mm_cmpeq_epi8(block, _mm_set1_epi8(byte as i8))) as u32 }
    }

    /// One bit for each code unit of a block: whether it is above U+007F.
    #[inline(always)]
    pub fn non_ascii(block: Block) -> u32 {
        unsafe { _mm_movemask_epi8(block) as u32 }
    }
}

#[cfg(target_arch = "aarch64")]
mod block {
    use core::arch::aarch64::*;

    pub(super) const BLOCK_SIZE: usize = 16;
    pub type Block = uint8x16_t;

    #[inline(always)]
    pub unsafe fn load_narrow(code_units: *const u8) -> Block {
        unsafe { vld1q_u8(code_units) }
    }

    #[inline(always)]
    pub unsafe fn load_wide(code_units: *const u16) -> Block {
        unsafe {
            vcombine_u8(
                vqmovn_u16(vld1q_u16(code_units)),
                vqmovn_u16(vld1q_u16(code_units.add(8))),
            )
        }
    }

    /// One bit per byte, from a vector whose bytes are all ones or all zeros.
    #[inline(always)]
    fn bits_of(bytes: Block) -> u32 {
        const BITS: [u8; 16] = [1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128];
        unsafe {
            let masked = vandq_u8(bytes, vld1q_u8(BITS.as_ptr()));
            vaddv_u8(vget_low_u8(masked)) as u32 | (vaddv_u8(vget_high_u8(masked)) as u32) << 8
        }
    }

    #[inline(always)]
    pub unsafe fn surrogates(code_units: *const u16) -> u32 {
        unsafe {
            let top_bits = vdupq_n_u16(0xF800);
            let surrogate = vdupq_n_u16(0xD800);
            let low = vceqq_u16(vandq_u16(vld1q_u16(code_units), top_bits), surrogate);
            let high = vceqq_u16(vandq_u16(vld1q_u16(code_units.add(8)), top_bits), surrogate);
            bits_of(vcombine_u8(vmovn_u16(low), vmovn_u16(high)))
        }
    }

    /// One bit for each code unit of a block: whether it is `byte`.
    #[inline(always)]
    pub fn equal_to(block: Block, byte: u8) -> u32 {
        unsafe { bits_of(vceqq_u8(block, vdupq_n_u8(byte))) }
    }

    /// One bit for each code unit of a block: whether it is above U+007F.
    #[inline(always)]
    pub fn non_ascii(block: Block) -> u32 {
        unsafe { bits_of(vcgeq_u8(block, vdupq_n_u8(0x80))) }
    }
}

#[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
mod block {
    pub(super) const BLOCK_SIZE: usize = 16;
    pub type Block = [u8; BLOCK_SIZE];

    #[inline(always)]
    pub unsafe fn load_narrow(code_units: *const u8) -> Block {
        unsafe { core::ptr::read_unaligned(code_units.cast()) }
    }

    #[inline(always)]
    pub unsafe fn load_wide(code_units: *const u16) -> Block {
        core::array::from_fn(|i| unsafe { core::ptr::read_unaligned(code_units.add(i)) }.min(0xFF) as u8)
    }

    #[inline(always)]
    pub unsafe fn surrogates(code_units: *const u16) -> u32 {
        (0..BLOCK_SIZE).fold(0, |bits, i| {
            let code_unit = unsafe { core::ptr::read_unaligned(code_units.add(i)) };
            bits | u32::from(code_unit & 0xF800 == 0xD800) << i
        })
    }

    #[inline(always)]
    fn bits_of(block: Block, test: impl Fn(u8) -> bool) -> u32 {
        block
            .iter()
            .enumerate()
            .fold(0, |bits, (i, &byte)| bits | u32::from(test(byte)) << i)
    }

    /// One bit for each code unit of a block: whether it is `byte`.
    #[inline(always)]
    pub fn equal_to(block: Block, byte: u8) -> u32 {
        bits_of(block, |code_unit| code_unit == byte)
    }

    /// One bit for each code unit of a block: whether it is above U+007F.
    #[inline(always)]
    pub fn non_ascii(block: Block) -> u32 {
        bits_of(block, |code_unit| code_unit >= 0x80)
    }
}

const CHUNK_SIZE: usize = 32;

/// The offset of the first code unit that is one of `stops` or a surrogate, or the length of `code_units` if there is
/// none. `seen_non_ascii` is set if a code unit before it is above U+007F.
#[inline(always)]
fn find_first<T: CodeUnit, const N: usize>(code_units: &[T], stops: [u8; N], seen_non_ascii: &mut bool) -> usize {
    // One bit for each code unit of the block at `block`: whether it ends the scan, and whether it isn't ASCII.
    let scan_block = |block: *const T| {
        // SAFETY: Both callers pass a pointer to `BLOCK_SIZE` code units.
        let (bytes, surrogates) = unsafe { (T::load_block(block), T::surrogates_in_block(block)) };
        let stops = stops
            .iter()
            .fold(surrogates, |bits, &stop| bits | block::equal_to(bytes, stop));
        (stops, block::non_ascii(bytes))
    };

    let mut offset = 0;
    while offset + BLOCK_SIZE <= code_units.len() {
        let (stops, non_ascii) = scan_block(code_units[offset..].as_ptr());
        if stops != 0 {
            let stop = stops.trailing_zeros();
            *seen_non_ascii |= non_ascii & ((1 << stop) - 1) != 0;
            return offset + stop as usize;
        }
        *seen_non_ascii |= non_ascii != 0;
        offset += BLOCK_SIZE;
    }

    // NB: What is left is less than a block, and is looked at as the start of one that ends the scan after it.
    let remaining = code_units.len() - offset;
    let mut last_block = [T::default(); BLOCK_SIZE];
    last_block[..remaining].copy_from_slice(&code_units[offset..]);
    let (stops, non_ascii) = scan_block(last_block.as_ptr());
    let stop = (stops | 1 << remaining).trailing_zeros();
    *seen_non_ascii |= non_ascii & ((1 << stop) - 1) != 0;
    offset + stop as usize
}

/// The bytes of `name`, which has to be ASCII, if they fit in `buffer`.
#[inline(always)]
pub fn ascii_name_bytes<'a, T: CodeUnit, const N: usize>(name: &[T], buffer: &'a mut [u8; N]) -> Option<&'a [u8]> {
    let bytes = buffer.get_mut(..name.len())?;
    for (byte, &code_unit) in bytes.iter_mut().zip(name) {
        *byte = code_unit.into() as u8;
    }
    Some(bytes)
}

/// A run of code units at the start of some input.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Run {
    pub length: usize,
    pub is_ascii: bool,
}

/// The run of code units at the start of `code_units` that the data and RCDATA states emit as they are: everything
/// up to the first U+003C (<), U+0026 (&), U+0000 NULL, U+000D CR, or surrogate that isn't half of a pair.
pub fn data_run<T: CodeUnit>(code_units: &[T]) -> Run {
    text_run(code_units, [b'<', b'&', 0, b'\r'])
}

/// The run of code units at the start of `code_units` that the RAWTEXT and script data states emit as they are:
/// everything up to the first U+003C (<), U+0000 NULL, U+000D CR, or surrogate that isn't half of a pair.
pub fn raw_text_run<T: CodeUnit>(code_units: &[T]) -> Run {
    text_run(code_units, [b'<', 0, b'\r'])
}

/// The number of code units at the start of `code_units` that a quoted attribute value takes as they are, and whether
/// they are all ASCII: everything up to the first `quote`, U+0026 (&), U+0000 NULL, U+000D CR, or surrogate that isn't
/// half of a pair.
pub fn quoted_attribute_value_run<T: CodeUnit>(code_units: &[T], quote: u8) -> Run {
    text_run(code_units, [quote, b'&', 0, b'\r'])
}

#[inline(always)]
fn text_run<T: CodeUnit, const N: usize>(code_units: &[T], stops: [u8; N]) -> Run {
    let mut length = 0;
    let mut seen_non_ascii = false;
    loop {
        length += find_first(&code_units[length..], stops, &mut seen_non_ascii);
        let Some(&[high, low]) = code_units.get(length..length + 2) else {
            break;
        };
        if !is_surrogate_pair(high.into(), low.into()) {
            break;
        }
        seen_non_ascii = true;
        length += 2;
    }
    Run {
        length,
        is_ascii: !seen_non_ascii,
    }
}

/// The number of code units at the start of `code_units` that a quoted attribute value takes as they are: everything
/// up to the first `quote`, U+0026 (&), U+0000 NULL, U+000D CR, or surrogate.
pub fn attribute_value_run<T: CodeUnit>(code_units: &[T], quote: u8) -> usize {
    find_first(code_units, [quote, b'&', 0, b'\r'], &mut false)
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
    fn raw_text_runs_go_on_past_ampersands() {
        assert_eq!(raw_text_run(b"a && b < c" as &[u8]), run(7, true));
        assert_eq!(raw_text_run(b"a && b\0" as &[u8]), run(6, true));
        assert_eq!(raw_text_run(b"a\r\n" as &[u8]), run(1, true));
        assert_eq!(raw_text_run(&code_units("a&\u{1f600}</")), run(4, false));
        assert_eq!(raw_text_run::<u16>(&[0x61, 0x26, 0xD83D]), run(2, true));
    }

    #[test]
    fn data_runs_find_stops_and_non_ascii_at_every_offset() {
        // Long enough to put a stop in every place of a block, in the part that is less than a block, and on the
        // seams.
        let length = BLOCK_SIZE * 5 + 7;
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
    fn attribute_value_runs_stop_at_the_quote_and_characters_the_tokenizer_changes() {
        assert_eq!(attribute_value_run(b"a b<c>'d\" e" as &[u8], b'"'), 8);
        assert_eq!(attribute_value_run(b"a b<c>'d\" e" as &[u8], b'\''), 6);
        assert_eq!(attribute_value_run(b"caf\xe9&amp;" as &[u8], b'"'), 4);
        assert_eq!(attribute_value_run(b"a\0" as &[u8], b'"'), 1);
        assert_eq!(attribute_value_run(b"a\r\n" as &[u8], b'"'), 1);
        assert_eq!(attribute_value_run(&code_units("a\u{2014}b\u{1f600}\""), b'"'), 3);
        let length = BLOCK_SIZE * 5 + 7;
        for offset in 0..length {
            let mut input = vec![b'x'; length];
            input[offset] = b'"';
            assert_eq!(attribute_value_run(&input, b'"'), offset);
            assert_eq!(attribute_value_run(&input, b'\''), length);
        }
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
