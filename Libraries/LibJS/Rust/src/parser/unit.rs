/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Input code units. Source text is either 8-bit Latin-1 or 16-bit UTF-16, and the lexer is generic over the two.
//!
//! Every scan works on a block of code units narrowed to bytes, where anything above 0xFE saturates to 0xFF. ASCII stays
//! exact, so the classification is the same for both widths, and an ASCII identifier's narrowed bytes are its interning
//! key whatever the input width was. A block is 32 units with AVX2 and 16 units otherwise; the lexer only knows `BLOCK`.
//!
//! The source must be followed by at least 64 units of zero padding, so blocks may read past its end.

pub trait Unit: Copy + Eq + 'static {
    const WIDE: bool;
    fn code(self) -> u32;
    /// Loads a block of units as bytes.
    unsafe fn load(p: *const Self) -> Block;
}

impl Unit for u8 {
    const WIDE: bool = false;
    #[inline(always)]
    fn code(self) -> u32 {
        self as u32
    }
    #[inline(always)]
    unsafe fn load(p: *const u8) -> Block {
        unsafe { block::load_narrow(p) }
    }
}

impl Unit for u16 {
    const WIDE: bool = true;
    #[inline(always)]
    fn code(self) -> u32 {
        self as u32
    }
    #[inline(always)]
    unsafe fn load(p: *const u16) -> Block {
        unsafe { block::load_wide(p) }
    }
}

#[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
const fn ident_tables() -> ([u8; 16], [u8; 16]) {
    const B0: u8 = 1; // '$'
    const B1: u8 = 2; // digits
    const B2: u8 = 4; // A-O, a-o
    const B3: u8 = 8; // P-Z, p-z
    const B4: u8 = 16; // '_'
    let mut lo = [0u8; 16];
    let mut hi = [0u8; 16];
    hi[2] = B0;
    hi[3] = B1;
    hi[4] = B2;
    hi[5] = B3 | B4;
    hi[6] = B2;
    hi[7] = B3;
    lo[4] |= B0;
    let mut i = 0;
    while i < 16 {
        if i <= 9 {
            lo[i] |= B1;
        }
        if i >= 1 {
            lo[i] |= B2;
        }
        if i <= 10 {
            lo[i] |= B3;
        }
        i += 1;
    }
    lo[15] |= B4;
    (lo, hi)
}
#[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
const IDENT_TABLES: ([u8; 16], [u8; 16]) = ident_tables();

/// A word with its bytes from `len` on cleared.
#[inline(always)]
fn first_bytes(word: u64, len: usize) -> u64 {
    if len >= 8 {
        word
    } else {
        word & ((1u64 << (len * 8)) - 1)
    }
}

/// The first 16 narrowed units with everything from `len` on cleared, as two little-endian words.
#[inline(always)]
pub unsafe fn key_of(x: Block, len: usize) -> (u64, u64) {
    let (k0, k1) = unsafe { block::first_words(x) };
    if len <= 8 {
        (first_bytes(k0, len), 0)
    } else {
        (k0, first_bytes(k1, len - 8))
    }
}

pub use block::{BLOCK, Block, eq_mask, high_mask, ident_mask};

#[cfg(all(target_arch = "x86_64", target_feature = "avx2"))]
mod block {
    use super::IDENT_TABLES;
    use core::arch::x86_64::*;

    pub const BLOCK: usize = 32;
    pub type Block = __m256i;

    #[inline(always)]
    pub unsafe fn load_narrow(p: *const u8) -> Block {
        unsafe { _mm256_loadu_si256(p as *const __m256i) }
    }

    #[inline(always)]
    pub unsafe fn load_wide(p: *const u16) -> Block {
        unsafe {
            #[cfg(target_feature = "avx512bw")]
            {
                _mm512_cvtusepi16_epi8(_mm512_loadu_si512(p as *const _))
            }
            #[cfg(not(target_feature = "avx512bw"))]
            {
                let cap = _mm256_set1_epi16(0xFF);
                let a = _mm256_min_epu16(_mm256_loadu_si256(p as *const __m256i), cap);
                let b = _mm256_min_epu16(_mm256_loadu_si256(p.add(16) as *const __m256i), cap);
                _mm256_permute4x64_epi64::<0b11011000>(_mm256_packus_epi16(a, b))
            }
        }
    }

    /// One bit per unit of the block: whether it is an ASCII identifier part.
    #[inline(always)]
    pub unsafe fn ident_mask(x: Block) -> u32 {
        unsafe {
            let lo_table = _mm256_broadcastsi128_si256(_mm_loadu_si128(IDENT_TABLES.0.as_ptr() as *const __m128i));
            let hi_table = _mm256_broadcastsi128_si256(_mm_loadu_si128(IDENT_TABLES.1.as_ptr() as *const __m128i));
            let hi_index = _mm256_and_si256(_mm256_srli_epi16::<4>(x), _mm256_set1_epi8(0x0F));
            let lo = _mm256_shuffle_epi8(lo_table, x);
            let hi = _mm256_shuffle_epi8(hi_table, hi_index);
            let none = _mm256_cmpeq_epi8(_mm256_and_si256(lo, hi), _mm256_setzero_si256());
            !(_mm256_movemask_epi8(none) as u32)
        }
    }

    #[inline(always)]
    pub unsafe fn eq_mask(x: Block, byte: u8) -> u32 {
        unsafe { _mm256_movemask_epi8(_mm256_cmpeq_epi8(x, _mm256_set1_epi8(byte as i8))) as u32 }
    }

    /// The units at or above 0x80.
    #[inline(always)]
    pub unsafe fn high_mask(x: Block) -> u32 {
        unsafe { _mm256_movemask_epi8(x) as u32 }
    }

    #[inline(always)]
    pub unsafe fn first_words(x: Block) -> (u64, u64) {
        unsafe {
            let low = _mm256_castsi256_si128(x);
            (_mm_cvtsi128_si64(low) as u64, _mm_extract_epi64::<1>(low) as u64)
        }
    }
}

#[cfg(all(target_arch = "x86_64", not(target_feature = "avx2")))]
mod block {
    use super::IDENT_TABLES;
    use core::arch::x86_64::*;

    pub const BLOCK: usize = 16;
    pub type Block = __m128i;

    #[inline(always)]
    pub unsafe fn load_narrow(p: *const u8) -> Block {
        unsafe { _mm_loadu_si128(p as *const __m128i) }
    }

    #[inline(always)]
    pub unsafe fn load_wide(p: *const u16) -> Block {
        unsafe {
            let cap = _mm_set1_epi16(0xFF);
            let a = _mm_min_epu16(_mm_loadu_si128(p as *const __m128i), cap);
            let b = _mm_min_epu16(_mm_loadu_si128(p.add(8) as *const __m128i), cap);
            _mm_packus_epi16(a, b)
        }
    }

    /// One bit per unit of the block: whether it is an ASCII identifier part.
    #[inline(always)]
    pub unsafe fn ident_mask(x: Block) -> u32 {
        unsafe {
            let lo_table = _mm_loadu_si128(IDENT_TABLES.0.as_ptr() as *const __m128i);
            let hi_table = _mm_loadu_si128(IDENT_TABLES.1.as_ptr() as *const __m128i);
            let hi_index = _mm_and_si128(_mm_srli_epi16::<4>(x), _mm_set1_epi8(0x0F));
            let lo = _mm_shuffle_epi8(lo_table, x);
            let hi = _mm_shuffle_epi8(hi_table, hi_index);
            let none = _mm_cmpeq_epi8(_mm_and_si128(lo, hi), _mm_setzero_si128());
            !(_mm_movemask_epi8(none) as u32) & 0xFFFF
        }
    }

    #[inline(always)]
    pub unsafe fn eq_mask(x: Block, byte: u8) -> u32 {
        unsafe { _mm_movemask_epi8(_mm_cmpeq_epi8(x, _mm_set1_epi8(byte as i8))) as u32 }
    }

    /// The units at or above 0x80.
    #[inline(always)]
    pub unsafe fn high_mask(x: Block) -> u32 {
        unsafe { _mm_movemask_epi8(x) as u32 }
    }

    #[inline(always)]
    pub unsafe fn first_words(x: Block) -> (u64, u64) {
        unsafe { (_mm_cvtsi128_si64(x) as u64, _mm_extract_epi64::<1>(x) as u64) }
    }
}

#[cfg(target_arch = "aarch64")]
mod block {
    use super::IDENT_TABLES;
    use core::arch::aarch64::*;

    pub const BLOCK: usize = 16;
    pub type Block = uint8x16_t;

    #[inline(always)]
    pub unsafe fn load_narrow(p: *const u8) -> Block {
        unsafe { vld1q_u8(p) }
    }

    #[inline(always)]
    pub unsafe fn load_wide(p: *const u16) -> Block {
        unsafe { vcombine_u8(vqmovn_u16(vld1q_u16(p)), vqmovn_u16(vld1q_u16(p.add(8)))) }
    }

    /// One bit per byte, from a vector whose bytes are all ones or all zeros.
    #[inline(always)]
    unsafe fn bits_of(x: Block) -> u32 {
        const BITS: [u8; 16] = [1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128];
        unsafe {
            let masked = vandq_u8(x, vld1q_u8(BITS.as_ptr()));
            vaddv_u8(vget_low_u8(masked)) as u32 | (vaddv_u8(vget_high_u8(masked)) as u32) << 8
        }
    }

    /// One bit per unit of the block: whether it is an ASCII identifier part.
    #[inline(always)]
    pub unsafe fn ident_mask(x: Block) -> u32 {
        unsafe {
            let lo = vqtbl1q_u8(vld1q_u8(IDENT_TABLES.0.as_ptr()), vandq_u8(x, vdupq_n_u8(0x0F)));
            let hi = vqtbl1q_u8(vld1q_u8(IDENT_TABLES.1.as_ptr()), vshrq_n_u8::<4>(x));
            bits_of(vtstq_u8(lo, hi))
        }
    }

    #[inline(always)]
    pub unsafe fn eq_mask(x: Block, byte: u8) -> u32 {
        unsafe { bits_of(vceqq_u8(x, vdupq_n_u8(byte))) }
    }

    /// The units at or above 0x80.
    #[inline(always)]
    pub unsafe fn high_mask(x: Block) -> u32 {
        unsafe { bits_of(vcgeq_u8(x, vdupq_n_u8(0x80))) }
    }

    #[inline(always)]
    pub unsafe fn first_words(x: Block) -> (u64, u64) {
        unsafe {
            let words = vreinterpretq_u64_u8(x);
            (vgetq_lane_u64::<0>(words), vgetq_lane_u64::<1>(words))
        }
    }
}

#[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
mod block {
    pub const BLOCK: usize = 16;
    pub type Block = [u8; 16];

    #[inline(always)]
    pub unsafe fn load_narrow(p: *const u8) -> Block {
        unsafe { core::ptr::read_unaligned(p as *const Block) }
    }

    #[inline(always)]
    pub unsafe fn load_wide(p: *const u16) -> Block {
        core::array::from_fn(|i| unsafe { core::ptr::read_unaligned(p.add(i)) }.min(0xFF) as u8)
    }

    #[inline(always)]
    fn bits_of(x: Block, test: impl Fn(u8) -> bool) -> u32 {
        x.iter()
            .enumerate()
            .fold(0, |bits, (i, &byte)| bits | (test(byte) as u32) << i)
    }

    /// One bit per unit of the block: whether it is an ASCII identifier part.
    #[inline(always)]
    pub unsafe fn ident_mask(x: Block) -> u32 {
        bits_of(x, |c| c.is_ascii_alphanumeric() || c == b'$' || c == b'_')
    }

    #[inline(always)]
    pub unsafe fn eq_mask(x: Block, byte: u8) -> u32 {
        bits_of(x, |c| c == byte)
    }

    /// The units at or above 0x80.
    #[inline(always)]
    pub unsafe fn high_mask(x: Block) -> u32 {
        bits_of(x, |c| c >= 0x80)
    }

    #[inline(always)]
    pub unsafe fn first_words(x: Block) -> (u64, u64) {
        (
            u64::from_le_bytes(x[..8].try_into().unwrap()),
            u64::from_le_bytes(x[8..].try_into().unwrap()),
        )
    }
}
