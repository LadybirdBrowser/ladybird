/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The loops that skip over runs of plain characters: the text between two tags, the inside of an attribute value, a
//! comment. Each returns the position of the first byte the caller has to look at, or the length of the input.
//!
//! OPTIMIZATION: Most of a document's bytes are in such runs, so these loops look at a block of bytes at a time. A
//! byte needs the caller's attention when it is one of up to three delimiters, a C0 control, or the start of a
//! non-ASCII sequence. The last two share one signed comparison: as an i8, a byte at or above 0x80 is negative, so
//! "less than 0x20" covers both.

/// Which C0 controls end a run, besides the ones that are never allowed.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Controls {
    /// Tab and LF are plain. CR ends the run: it has to be normalized.
    StopAtCr,
    /// Tab, LF and CR all end the run: in an attribute value each becomes a space.
    StopAtAll,
}

#[inline(always)]
fn needs_attention(byte: u8, d0: u8, d1: u8, d2: u8, controls: Controls) -> bool {
    if byte == d0 || byte == d1 || byte == d2 {
        return true;
    }
    if (byte as i8) < 0x20 {
        return controls == Controls::StopAtAll || (byte != b'\t' && byte != b'\n');
    }
    false
}

#[inline(always)]
fn scalar(bytes: &[u8], mut pos: usize, d0: u8, d1: u8, d2: u8, controls: Controls) -> usize {
    while pos < bytes.len() && !needs_attention(bytes[pos], d0, d1, d2, controls) {
        pos += 1;
    }
    pos
}

/// The first position at or after `pos` whose byte is `d0`, `d1`, `d2`, a control selected by `controls`, or at or
/// above 0x80. Pass the same delimiter more than once to look for fewer than three.
#[inline(always)]
pub fn find(bytes: &[u8], pos: usize, d0: u8, d1: u8, d2: u8, controls: Controls) -> usize {
    let pos = block::find(bytes, pos, d0, d1, d2, controls);
    scalar(bytes, pos, d0, d1, d2, controls)
}

/// The reference implementation, for testing the block versions against.
#[cfg(test)]
pub fn find_scalar(bytes: &[u8], pos: usize, d0: u8, d1: u8, d2: u8, controls: Controls) -> usize {
    scalar(bytes, pos, d0, d1, d2, controls)
}

#[cfg(all(target_arch = "x86_64", target_feature = "avx2"))]
mod block {
    use super::Controls;
    use core::arch::x86_64::*;

    const BLOCK: usize = 32;

    /// Returns the position of the first byte that needs attention if a whole block containing it could be read, or
    /// else the position of the first byte no whole block covers.
    #[inline(always)]
    pub fn find(bytes: &[u8], mut pos: usize, d0: u8, d1: u8, d2: u8, controls: Controls) -> usize {
        while pos + BLOCK <= bytes.len() {
            // SAFETY: The block [pos, pos + BLOCK) is inside `bytes`, and AVX2 is statically enabled.
            let mask = unsafe {
                let x = _mm256_loadu_si256(bytes.as_ptr().add(pos) as *const __m256i);
                let delimiters = _mm256_or_si256(
                    _mm256_or_si256(
                        _mm256_cmpeq_epi8(x, _mm256_set1_epi8(d0 as i8)),
                        _mm256_cmpeq_epi8(x, _mm256_set1_epi8(d1 as i8)),
                    ),
                    _mm256_cmpeq_epi8(x, _mm256_set1_epi8(d2 as i8)),
                );
                let mut low = _mm256_cmpgt_epi8(_mm256_set1_epi8(0x20), x);
                if controls == Controls::StopAtCr {
                    let plain = _mm256_or_si256(
                        _mm256_cmpeq_epi8(x, _mm256_set1_epi8(b'\t' as i8)),
                        _mm256_cmpeq_epi8(x, _mm256_set1_epi8(b'\n' as i8)),
                    );
                    low = _mm256_andnot_si256(plain, low);
                }
                _mm256_movemask_epi8(_mm256_or_si256(delimiters, low)) as u32
            };
            if mask != 0 {
                return pos + mask.trailing_zeros() as usize;
            }
            pos += BLOCK;
        }
        pos
    }
}

#[cfg(all(target_arch = "x86_64", not(target_feature = "avx2")))]
mod block {
    use super::Controls;
    use core::arch::x86_64::*;

    const BLOCK: usize = 16;

    /// Returns the position of the first byte that needs attention if a whole block containing it could be read, or
    /// else the position of the first byte no whole block covers.
    #[inline(always)]
    pub fn find(bytes: &[u8], mut pos: usize, d0: u8, d1: u8, d2: u8, controls: Controls) -> usize {
        while pos + BLOCK <= bytes.len() {
            // SAFETY: The block [pos, pos + BLOCK) is inside `bytes`, and SSE2 is part of the x86_64 baseline.
            let mask = unsafe {
                let x = _mm_loadu_si128(bytes.as_ptr().add(pos) as *const __m128i);
                let delimiters = _mm_or_si128(
                    _mm_or_si128(
                        _mm_cmpeq_epi8(x, _mm_set1_epi8(d0 as i8)),
                        _mm_cmpeq_epi8(x, _mm_set1_epi8(d1 as i8)),
                    ),
                    _mm_cmpeq_epi8(x, _mm_set1_epi8(d2 as i8)),
                );
                let mut low = _mm_cmplt_epi8(x, _mm_set1_epi8(0x20));
                if controls == Controls::StopAtCr {
                    let plain = _mm_or_si128(
                        _mm_cmpeq_epi8(x, _mm_set1_epi8(b'\t' as i8)),
                        _mm_cmpeq_epi8(x, _mm_set1_epi8(b'\n' as i8)),
                    );
                    low = _mm_andnot_si128(plain, low);
                }
                _mm_movemask_epi8(_mm_or_si128(delimiters, low)) as u32
            };
            if mask != 0 {
                return pos + mask.trailing_zeros() as usize;
            }
            pos += BLOCK;
        }
        pos
    }
}

#[cfg(target_arch = "aarch64")]
mod block {
    use super::Controls;
    use core::arch::aarch64::*;

    const BLOCK: usize = 16;

    /// Returns the position of the first byte that needs attention if a whole block containing it could be read, or
    /// else the position of the first byte no whole block covers.
    #[inline(always)]
    pub fn find(bytes: &[u8], mut pos: usize, d0: u8, d1: u8, d2: u8, controls: Controls) -> usize {
        while pos + BLOCK <= bytes.len() {
            // SAFETY: The block [pos, pos + BLOCK) is inside `bytes`, and NEON is part of the aarch64 baseline.
            let mask = unsafe {
                let x = vld1q_u8(bytes.as_ptr().add(pos));
                let delimiters = vorrq_u8(
                    vorrq_u8(vceqq_u8(x, vdupq_n_u8(d0)), vceqq_u8(x, vdupq_n_u8(d1))),
                    vceqq_u8(x, vdupq_n_u8(d2)),
                );
                let mut low = vcltq_s8(vreinterpretq_s8_u8(x), vdupq_n_s8(0x20));
                if controls == Controls::StopAtCr {
                    let plain = vorrq_u8(vceqq_u8(x, vdupq_n_u8(b'\t')), vceqq_u8(x, vdupq_n_u8(b'\n')));
                    low = vbicq_u8(low, plain);
                }
                // Narrowing each 16-bit lane by four bits leaves four mask bits per byte, in order.
                let any = vreinterpretq_u16_u8(vorrq_u8(delimiters, low));
                vget_lane_u64::<0>(vreinterpret_u64_u8(vshrn_n_u16::<4>(any)))
            };
            if mask != 0 {
                return pos + (mask.trailing_zeros() / 4) as usize;
            }
            pos += BLOCK;
        }
        pos
    }
}

#[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
mod block {
    use super::Controls;

    #[inline(always)]
    pub fn find(_bytes: &[u8], pos: usize, _d0: u8, _d1: u8, _d2: u8, _controls: Controls) -> usize {
        pos
    }
}

/// Returns where the run of ASCII name characters that starts at `pos` ends. A caller parsing a name has to look at
/// the byte there: if it is at or above 0x80 the name may go on.
#[inline(always)]
pub fn ascii_name_end(bytes: &[u8], mut pos: usize) -> usize {
    pos = name_block::ascii_name_end(bytes, pos);
    while pos < bytes.len() && crate::chars::CLASS[bytes[pos] as usize] & crate::chars::NAME != 0 {
        pos += 1;
    }
    pos
}

// OPTIMIZATION: A name is a handful of bytes, and where it ends is anyone's guess, so a loop over its bytes ends in a
// mispredicted branch nearly every time. Classifying sixteen bytes at once and counting the leading name characters
// has no such branch. The classification is two table lookups on the halves of each byte: a byte is a name
// character if the entries for its high and low nibbles share a bit.
//
// | high nibble | name characters | bit |
// |-------------|-----------------|-----|
// | 2           | - .             | 0   |
// | 3           | 0-9 :           | 1   |
// | 4, 6        | A-O, a-o        | 2   |
// | 5           | P-Z _           | 3   |
// | 7           | p-z             | 4   |
#[cfg(any(all(target_arch = "x86_64", target_feature = "ssse3"), target_arch = "aarch64"))]
const NAME_LOW_NIBBLES: [u8; 16] = [26, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 4, 4, 5, 5, 12];
#[cfg(any(all(target_arch = "x86_64", target_feature = "ssse3"), target_arch = "aarch64"))]
const NAME_HIGH_NIBBLES: [u8; 16] = [0, 0, 1, 2, 4, 8, 4, 16, 0, 0, 0, 0, 0, 0, 0, 0];

#[cfg(all(target_arch = "x86_64", target_feature = "ssse3"))]
mod name_block {
    use super::NAME_HIGH_NIBBLES;
    use super::NAME_LOW_NIBBLES;
    use core::arch::x86_64::*;

    #[inline(always)]
    pub fn ascii_name_end(bytes: &[u8], mut pos: usize) -> usize {
        while pos + 16 <= bytes.len() {
            // SAFETY: The block [pos, pos + 16) is inside `bytes`, the tables are sixteen bytes, and SSSE3 is
            // statically enabled.
            let others = unsafe {
                let x = _mm_loadu_si128(bytes.as_ptr().add(pos) as *const __m128i);
                let low_table = _mm_loadu_si128(NAME_LOW_NIBBLES.as_ptr() as *const __m128i);
                let high_table = _mm_loadu_si128(NAME_HIGH_NIBBLES.as_ptr() as *const __m128i);
                // A byte at or above 0x80 selects zero from the first table, and so is never a name character.
                let low = _mm_shuffle_epi8(low_table, x);
                let high = _mm_shuffle_epi8(high_table, _mm_and_si128(_mm_srli_epi16::<4>(x), _mm_set1_epi8(0x0F)));
                _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(low, high), _mm_setzero_si128())) as u32
            };
            if others != 0 {
                return pos + others.trailing_zeros() as usize;
            }
            pos += 16;
        }
        pos
    }
}

#[cfg(target_arch = "aarch64")]
mod name_block {
    use super::NAME_HIGH_NIBBLES;
    use super::NAME_LOW_NIBBLES;
    use core::arch::aarch64::*;

    #[inline(always)]
    pub fn ascii_name_end(bytes: &[u8], mut pos: usize) -> usize {
        while pos + 16 <= bytes.len() {
            // SAFETY: The block [pos, pos + 16) is inside `bytes`, the tables are sixteen bytes, and NEON is part of
            // the aarch64 baseline.
            let others = unsafe {
                let x = vld1q_u8(bytes.as_ptr().add(pos));
                let low_table = vld1q_u8(NAME_LOW_NIBBLES.as_ptr());
                let high_table = vld1q_u8(NAME_HIGH_NIBBLES.as_ptr());
                // The high nibble of a byte at or above 0x80 selects zero from the second table, and so such a byte
                // is never a name character.
                let low = vqtbl1q_u8(low_table, vandq_u8(x, vdupq_n_u8(0x0F)));
                let high = vqtbl1q_u8(high_table, vshrq_n_u8::<4>(x));
                let other = vceqzq_u8(vandq_u8(low, high));
                vget_lane_u64::<0>(vreinterpret_u64_u8(vshrn_n_u16::<4>(vreinterpretq_u16_u8(other))))
            };
            if others != 0 {
                return pos + (others.trailing_zeros() / 4) as usize;
            }
            pos += 16;
        }
        pos
    }
}

#[cfg(not(any(all(target_arch = "x86_64", target_feature = "ssse3"), target_arch = "aarch64")))]
mod name_block {
    #[inline(always)]
    pub fn ascii_name_end(_bytes: &[u8], pos: usize) -> usize {
        pos
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn block_and_scalar_agree() {
        // A deterministic xorshift, so a failure reproduces.
        let mut state = 0x9E37_79B9_7F4A_7C15u64;
        let mut next = move || {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            state
        };
        for _ in 0..20_000 {
            let length = (next() % 100) as usize;
            let mut bytes = vec![b'a'; length];
            for _ in 0..(next() % 4) {
                if length > 0 {
                    let index = (next() % length as u64) as usize;
                    bytes[index] = (next() % 256) as u8;
                }
            }
            let start = if length == 0 {
                0
            } else {
                (next() % length as u64) as usize
            };
            for controls in [Controls::StopAtCr, Controls::StopAtAll] {
                assert_eq!(
                    find(&bytes, start, b'<', b'&', b']', controls),
                    find_scalar(&bytes, start, b'<', b'&', b']', controls),
                    "{bytes:?} from {start}"
                );
            }
        }
    }

    #[test]
    fn name_end_agrees_with_the_class_table() {
        let mut bytes: Vec<u8> = (0..=255u8).collect();
        bytes.extend(std::iter::repeat_n(b'a', 40));
        bytes.extend_from_slice(b"-._:09AZaz /=>\x80abc");
        bytes.extend(std::iter::repeat_n(b'z', 40));
        for start in 0..bytes.len() {
            let mut expected = start;
            while expected < bytes.len() && crate::chars::CLASS[bytes[expected] as usize] & crate::chars::NAME != 0 {
                expected += 1;
            }
            assert_eq!(ascii_name_end(&bytes, start), expected, "from {start}");
        }
    }
}
