/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Input code units, and scans over blocks of them. Style sheet text is 8-bit ASCII or 16-bit UTF-16, and the lexer is
//! generic over the two.
//!
//! A scan works on a block of 16 code units narrowed to bytes, where anything above 0xFE saturates to 0xFF. ASCII stays
//! exact, so the classification is the same for both widths.

use crate::css::css_tokenizer::TokenizerInput;

/// How many code units a block holds.
pub(crate) const BLOCK: usize = 16;

pub(crate) trait Unit: Copy + Eq + std::hash::Hash + 'static {
    /// Whether a code point can take two units.
    const WIDE: bool;
    fn code(self) -> u32;
    /// Loads a block of units as bytes.
    fn load(units: &[Self; BLOCK]) -> Block;
    fn input(units: &[Self]) -> TokenizerInput<'_>;
}

impl Unit for u8 {
    const WIDE: bool = false;

    #[inline(always)]
    fn code(self) -> u32 {
        u32::from(self)
    }

    #[inline(always)]
    fn load(units: &[u8; BLOCK]) -> Block {
        imp::load_narrow(units)
    }

    #[inline(always)]
    fn input(units: &[u8]) -> TokenizerInput<'_> {
        TokenizerInput::Latin1(units)
    }
}

impl Unit for u16 {
    const WIDE: bool = true;

    #[inline(always)]
    fn code(self) -> u32 {
        u32::from(self)
    }

    #[inline(always)]
    fn load(units: &[u16; BLOCK]) -> Block {
        imp::load_wide(units)
    }

    #[inline(always)]
    fn input(units: &[u16]) -> TokenizerInput<'_> {
        TokenizerInput::Utf16(units)
    }
}

pub(crate) use imp::{Block, eq_mask, ident_mask};

#[cfg(target_arch = "x86_64")]
mod imp {
    use super::BLOCK;
    use core::arch::x86_64::*;

    pub(crate) type Block = __m128i;

    #[inline(always)]
    pub(super) fn load_narrow(units: &[u8; BLOCK]) -> Block {
        // SAFETY: The array is 16 readable bytes, and SSE2 is part of every x86-64 target.
        unsafe { _mm_loadu_si128(units.as_ptr().cast()) }
    }

    #[inline(always)]
    pub(super) fn load_wide(units: &[u16; BLOCK]) -> Block {
        // SAFETY: The array is 32 readable bytes, and SSE2 is part of every x86-64 target.
        unsafe {
            let narrow = |half: __m128i| {
                // NB: Packing saturates as if the units were signed, so a unit with a high byte is made 0xFF first.
                let fits = _mm_cmpeq_epi16(_mm_srli_epi16::<8>(half), _mm_setzero_si128());
                _mm_or_si128(
                    _mm_and_si128(half, _mm_set1_epi16(0xFF)),
                    _mm_andnot_si128(fits, _mm_set1_epi16(0xFF)),
                )
            };
            let low = narrow(_mm_loadu_si128(units.as_ptr().cast()));
            let high = narrow(_mm_loadu_si128(units.as_ptr().add(8).cast()));
            _mm_packus_epi16(low, high)
        }
    }

    /// One bit per unit of the block: whether it is an ident code point, which every unit from 0x80 up is.
    #[inline(always)]
    pub(crate) fn ident_mask(block: Block) -> u32 {
        // SAFETY: SSE2 is part of every x86-64 target.
        unsafe {
            // NB: x <= k, unsigned, is min(x, k) == x.
            let at_most = |x: __m128i, k: i8| _mm_cmpeq_epi8(_mm_min_epu8(x, _mm_set1_epi8(k)), x);
            let letter = _mm_sub_epi8(_mm_or_si128(block, _mm_set1_epi8(0x20)), _mm_set1_epi8(b'a' as i8));
            let digit = _mm_sub_epi8(block, _mm_set1_epi8(b'0' as i8));
            let underscore = _mm_cmpeq_epi8(block, _mm_set1_epi8(b'_' as i8));
            let hyphen = _mm_cmpeq_epi8(block, _mm_set1_epi8(b'-' as i8));
            let ascii = _mm_or_si128(
                _mm_or_si128(at_most(letter, 25), at_most(digit, 9)),
                _mm_or_si128(underscore, hyphen),
            );
            // NB: The sign bit of a unit from 0x80 up is set already.
            _mm_movemask_epi8(_mm_or_si128(ascii, block)) as u32
        }
    }

    /// One bit per unit of the block: whether it is `byte`.
    #[inline(always)]
    pub(crate) fn eq_mask(block: Block, byte: u8) -> u32 {
        // SAFETY: SSE2 is part of every x86-64 target.
        unsafe { _mm_movemask_epi8(_mm_cmpeq_epi8(block, _mm_set1_epi8(byte as i8))) as u32 }
    }
}

#[cfg(target_arch = "aarch64")]
mod imp {
    use super::BLOCK;
    use core::arch::aarch64::*;

    pub(crate) type Block = uint8x16_t;

    #[inline(always)]
    pub(super) fn load_narrow(units: &[u8; BLOCK]) -> Block {
        // SAFETY: The array is 16 readable bytes, and NEON is part of every AArch64 target.
        unsafe { vld1q_u8(units.as_ptr()) }
    }

    #[inline(always)]
    pub(super) fn load_wide(units: &[u16; BLOCK]) -> Block {
        // SAFETY: The array is 32 readable bytes, and NEON is part of every AArch64 target.
        unsafe {
            vcombine_u8(
                vqmovn_u16(vld1q_u16(units.as_ptr())),
                vqmovn_u16(vld1q_u16(units.as_ptr().add(8))),
            )
        }
    }

    /// One bit per byte, from a vector whose bytes are all ones or all zeros.
    #[inline(always)]
    fn bits_of(block: Block) -> u32 {
        const BITS: [u8; 16] = [1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128];
        // SAFETY: NEON is part of every AArch64 target.
        unsafe {
            let masked = vandq_u8(block, vld1q_u8(BITS.as_ptr()));
            u32::from(vaddv_u8(vget_low_u8(masked))) | u32::from(vaddv_u8(vget_high_u8(masked))) << 8
        }
    }

    /// One bit per unit of the block: whether it is an ident code point, which every unit from 0x80 up is.
    #[inline(always)]
    pub(crate) fn ident_mask(block: Block) -> u32 {
        // SAFETY: NEON is part of every AArch64 target.
        unsafe {
            let letter = vcleq_u8(
                vsubq_u8(vorrq_u8(block, vdupq_n_u8(0x20)), vdupq_n_u8(b'a')),
                vdupq_n_u8(25),
            );
            let digit = vcleq_u8(vsubq_u8(block, vdupq_n_u8(b'0')), vdupq_n_u8(9));
            let underscore = vceqq_u8(block, vdupq_n_u8(b'_'));
            let hyphen = vceqq_u8(block, vdupq_n_u8(b'-'));
            let high = vcgeq_u8(block, vdupq_n_u8(0x80));
            bits_of(vorrq_u8(
                vorrq_u8(letter, digit),
                vorrq_u8(vorrq_u8(underscore, hyphen), high),
            ))
        }
    }

    /// One bit per unit of the block: whether it is `byte`.
    #[inline(always)]
    pub(crate) fn eq_mask(block: Block, byte: u8) -> u32 {
        // SAFETY: NEON is part of every AArch64 target.
        unsafe { bits_of(vceqq_u8(block, vdupq_n_u8(byte))) }
    }
}

#[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
mod imp {
    use super::BLOCK;

    pub(crate) type Block = [u8; BLOCK];

    #[inline(always)]
    pub(super) fn load_narrow(units: &[u8; BLOCK]) -> Block {
        *units
    }

    #[inline(always)]
    pub(super) fn load_wide(units: &[u16; BLOCK]) -> Block {
        units.map(|unit| unit.min(0xFF) as u8)
    }

    #[inline(always)]
    fn bits_of(block: Block, test: impl Fn(u8) -> bool) -> u32 {
        block
            .iter()
            .enumerate()
            .fold(0, |bits, (index, &byte)| bits | u32::from(test(byte)) << index)
    }

    /// One bit per unit of the block: whether it is an ident code point, which every unit from 0x80 up is.
    #[inline(always)]
    pub(crate) fn ident_mask(block: Block) -> u32 {
        bits_of(block, |byte| {
            byte >= 0x80 || byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-'
        })
    }

    /// One bit per unit of the block: whether it is `byte`.
    #[inline(always)]
    pub(crate) fn eq_mask(block: Block, byte: u8) -> u32 {
        bits_of(block, |unit| unit == byte)
    }
}

#[cfg(test)]
mod tests {
    use super::{BLOCK, Unit, eq_mask, ident_mask};

    #[test]
    fn masks_agree_with_the_scalar_classes() {
        for first in 0..=255u8 {
            let mut narrow = [b'a'; BLOCK];
            narrow[3] = first;
            narrow[15] = first;
            let expected = u32::from(first >= 0x80 || first.is_ascii_alphanumeric() || first == b'_' || first == b'-');
            let mask = ident_mask(u8::load(&narrow));
            assert_eq!(mask >> 3 & 1, expected, "{first:#x}");
            assert_eq!(mask >> 15 & 1, expected, "{first:#x}");
            assert_eq!(mask & 0x7ff7, 0x7ff7);
            assert_eq!(eq_mask(u8::load(&narrow), first) & (1 << 3 | 1 << 15), 1 << 3 | 1 << 15);
        }
        for unit in [0x80u16, 0xFF, 0x100, 0x2D00, 0x7FFF, 0x8000, 0xD800, 0xFFFF] {
            let mut wide = [u16::from(b'{'); BLOCK];
            wide[0] = unit;
            wide[9] = unit;
            assert_eq!(ident_mask(u16::load(&wide)), 1 | 1 << 9, "{unit:#x}");
            assert_eq!(eq_mask(u16::load(&wide), b'{'), 0xffff & !(1 | 1 << 9), "{unit:#x}");
        }
    }
}
