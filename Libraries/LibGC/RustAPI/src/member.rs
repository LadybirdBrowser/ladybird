/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compressed cell pointers, as GC::Member holds them in the fields of cells: the cell's offset into the heap region
//! in units of the 8-byte cell alignment, which fits in 32 bits because the region is 32 GiB, with 0 for null, which
//! never names a cell because the bottom of the heap region is never mapped.
//!
//! The crates that define cells each wrap these in field types of their own, which implement
//! [`MemberSlot`](crate::MemberSlot), and keep a copy of the region's base that their decoding reads.

/// How far a cell's offset into the heap region is shifted right to compress it: cells are 8-byte aligned.
pub const COMPRESSED_POINTER_SHIFT: u32 = 3;

/// The size of the heap region, which a compressed pointer covers whatever its 32 bits are.
pub const COMPRESSED_POINTER_RANGE: u64 = 1 << (32 + COMPRESSED_POINTER_SHIFT);

/// The compressed pointer of the cell at `address` in the heap region at `base`.
#[inline(always)]
pub fn compress(base: usize, address: usize) -> u32 {
    let offset = address.wrapping_sub(base);
    debug_assert!((offset as u64) < COMPRESSED_POINTER_RANGE);
    debug_assert!(offset.is_multiple_of(1 << COMPRESSED_POINTER_SHIFT));
    (offset >> COMPRESSED_POINTER_SHIFT) as u32
}

/// The address the compressed pointer `compressed`, which is not null, names in the heap region at `base`.
///
/// Makes the compressed pointer live up to this point, and the decoded address from this point on. The conservative
/// stack scan recognizes a cell by its address or by its compressed pointer in either half of a word, but not by its
/// unshifted offset, which the compiler could otherwise keep instead while either is held across an allocation.
#[inline(always)]
pub fn decompress(base: usize, compressed: u32) -> usize {
    let mut address = base + ((compressed as usize) << COMPRESSED_POINTER_SHIFT);
    // SAFETY: The assembly is empty: it only has the compiler put `compressed` and `address` in registers here.
    unsafe {
        core::arch::asm!(
            "/* {compressed} {address} */",
            compressed = in(reg) compressed as usize,
            address = inout(reg) address,
            options(nomem, nostack, preserves_flags),
        );
    }
    address
}

/// Where a collector that has to know about stores into cells, like a generational or incremental one, will hook in,
/// as GC::Detail::member_write_barrier() does for the stores C++ makes. Every store of a compressed pointer into the
/// field at `_slot` goes through it. It does nothing, as no collector of LibGC needs one.
#[inline(always)]
pub fn write_barrier(_slot: *const u32, _compressed: u32) {}
