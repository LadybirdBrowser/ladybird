/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where compiled code holds the cells it embeds, so that the runtime can patch the code when a collection that moves
//! cells moves one of them.
//!
//! Code holds a cell in one of two ways: a 64-bit constant (a `CellConstant`), which comes from an entry of the constant
//! pool after the instructions, or a compressed pointer in the 32-bit immediates of the instructions that compare
//! against or store it. Only the operations of `PortableMacroAssembler` that take a cell as such emit either, and the
//! macro assembler lists each place as it emits it. Nothing is ever taken for a cell because of its value: immediates
//! of the other operations are plain bits, and code whose plain bits are the same as an embedded cell does not compile
//! (see `PortableMacroAssembler::forbid_untyped_cells()`), so that a cell passed as plain bits by mistake cannot go
//! stale when the cell moves.

/// How compiled code holds a cell at the offset of a relocation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum RelocationKind {
    /// An entry of the constant pool that is the address of the cell.
    PoolAddress = 0,
    /// An entry of the constant pool that is a value that holds the cell: its tag bits, and the offset of the cell
    /// into the heap region.
    PoolValue = 1,
    /// The little-endian 32-bit immediate of an x86-64 instruction, which is the compressed pointer of the cell.
    X86CompressedImmediate = 2,
    /// A `movz` and a `movk` of a 32-bit register on AArch64, whose 16-bit immediates are the low and the high half of
    /// the compressed pointer of the cell.
    Aarch64CompressedMove = 3,
}

impl RelocationKind {
    pub fn from_word(word: u32) -> RelocationKind {
        match word {
            0 => RelocationKind::PoolAddress,
            1 => RelocationKind::PoolValue,
            2 => RelocationKind::X86CompressedImmediate,
            3 => RelocationKind::Aarch64CompressedMove,
            _ => unreachable!("{word} is no kind of relocation"),
        }
    }

    /// How many bytes of the code the relocation covers.
    pub const fn size(self) -> usize {
        match self {
            RelocationKind::PoolAddress | RelocationKind::PoolValue | RelocationKind::Aarch64CompressedMove => 8,
            RelocationKind::X86CompressedImmediate => 4,
        }
    }
}

/// A place in compiled code that holds the embedded cell with index `cell` (see `Artifact::embedded_cells()`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Relocation {
    pub offset: u32,
    pub kind: RelocationKind,
    pub cell: u32,
}

/// A 64-bit constant that is a cell the code embeds, which comes from the constant pool so that the runtime can patch it
/// when the cell moves.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum CellConstant {
    /// The address of the cell.
    Address(u64),
    /// A value that is the cell: its tag bits, and the offset of the cell into the heap region.
    Value(u64),
}

impl CellConstant {
    /// The bits of the constant.
    pub fn bits(self) -> u64 {
        match self {
            CellConstant::Address(bits) | CellConstant::Value(bits) => bits,
        }
    }

    /// The kind of the relocation of the pool entry that holds the constant.
    pub fn kind(self) -> RelocationKind {
        match self {
            CellConstant::Address(_) => RelocationKind::PoolAddress,
            CellConstant::Value(_) => RelocationKind::PoolValue,
        }
    }
}

/// How far a cell's address is shifted right to compress it: cells store
/// pointers to other cells as the low 32 bits of the shifted address (their
/// offset into the 32 GiB heap region, which is aligned to its size, in units
/// of the 8-byte cell alignment), and 0 for null.
pub const COMPRESSED_POINTER_SHIFT: u8 = 3;

/// How cells are laid out in the heap region, which relocations need to encode their addresses.
#[derive(Clone, Copy, Debug)]
pub struct CellEncoding {
    pub heap_region_base: u64,
    /// The bits of a value that are the offset of its cell into the heap region.
    pub value_offset_mask: u64,
    /// The bits every value that is a cell has.
    pub value_is_cell_pattern: u64,
}

impl CellEncoding {
    /// Whether the 64-bit `imm` is one of `cells` (sorted addresses), as its address or as a value.
    pub fn is_one_of(&self, cells: &[u64], imm: u64) -> bool {
        (imm != 0 && cells.binary_search(&imm).is_ok())
            || (self.is_cell_value(imm) && cells.binary_search(&self.address_of_value(imm)).is_ok())
    }

    /// Whether the value `value` is a cell. No value is if cells have no tag bits, as in some tests.
    pub fn is_cell_value(&self, value: u64) -> bool {
        self.value_is_cell_pattern != 0 && value & self.value_is_cell_pattern == self.value_is_cell_pattern
    }

    /// The address of the cell that the value `value` is.
    pub fn address_of_value(&self, value: u64) -> u64 {
        self.heap_region_base + (value & self.value_offset_mask)
    }

    /// The address of the cell that the pool entry `word` of a relocation of `kind` holds.
    pub fn address_in_pool(&self, kind: RelocationKind, word: u64) -> u64 {
        match kind {
            RelocationKind::PoolAddress => word,
            RelocationKind::PoolValue => self.address_of_value(word),
            RelocationKind::X86CompressedImmediate | RelocationKind::Aarch64CompressedMove => {
                unreachable!("compressed pointers are not in the pool")
            }
        }
    }

    /// The compressed pointer of the cell at `address`.
    pub fn compressed(address: u64) -> u32 {
        (address >> COMPRESSED_POINTER_SHIFT) as u32
    }
}

/// The 16-bit immediate of an AArch64 move wide instruction.
const AARCH64_MOVE_WIDE_IMMEDIATE_SHIFT: u32 = 5;
const AARCH64_MOVE_WIDE_IMMEDIATE_MASK: u32 = 0xffff << AARCH64_MOVE_WIDE_IMMEDIATE_SHIFT;

fn aarch64_instruction(bytes: &[u8], index: usize) -> u32 {
    u32::from_le_bytes(bytes[4 * index..4 * index + 4].try_into().expect("4 bytes"))
}

/// The compressed pointer that `bytes`, the bytes of the code at a relocation of a compressed kind, hold.
pub fn compressed_at(kind: RelocationKind, bytes: &[u8]) -> u32 {
    match kind {
        RelocationKind::X86CompressedImmediate => u32::from_le_bytes(bytes[..4].try_into().expect("4 bytes")),
        RelocationKind::Aarch64CompressedMove => {
            let half = |index| (aarch64_instruction(bytes, index) & AARCH64_MOVE_WIDE_IMMEDIATE_MASK) >> 5;
            half(0) | (half(1) << 16)
        }
        RelocationKind::PoolAddress | RelocationKind::PoolValue => unreachable!("pool entries are 64 bits"),
    }
}

/// The bytes of the code at a relocation of `kind` once its cell is at `address`, from `bytes`, which the code holds
/// there now. Only the first `kind.size()` bytes are meaningful.
pub fn relocated_bytes(kind: RelocationKind, bytes: &[u8], address: u64, encoding: CellEncoding) -> [u8; 8] {
    let compressed = CellEncoding::compressed(address);
    let mut result = [0; 8];
    match kind {
        RelocationKind::PoolAddress => result = address.to_le_bytes(),
        RelocationKind::PoolValue => {
            let value = u64::from_le_bytes(bytes[..8].try_into().expect("8 bytes"));
            let offset = address - encoding.heap_region_base;
            assert_eq!(offset & !encoding.value_offset_mask, 0, "cells are in the heap region");
            result = ((value & !encoding.value_offset_mask) | offset).to_le_bytes();
        }
        RelocationKind::X86CompressedImmediate => result[..4].copy_from_slice(&compressed.to_le_bytes()),
        RelocationKind::Aarch64CompressedMove => {
            for (index, half) in [compressed & 0xffff, compressed >> 16].into_iter().enumerate() {
                let instruction = (aarch64_instruction(bytes, index) & !AARCH64_MOVE_WIDE_IMMEDIATE_MASK)
                    | (half << AARCH64_MOVE_WIDE_IMMEDIATE_SHIFT);
                result[4 * index..4 * index + 4].copy_from_slice(&instruction.to_le_bytes());
            }
        }
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    const ENCODING: CellEncoding = CellEncoding {
        heap_region_base: 0x7f00_0000_0000,
        value_offset_mask: (1 << 35) - 1,
        value_is_cell_pattern: 0xfff8 << 48,
    };

    #[test]
    fn pool_values_keep_their_tags() {
        let tag: u64 = 0xfffa_0000_0000_0000;
        let old = (tag | 0x1234_5678).to_le_bytes();
        let new = relocated_bytes(
            RelocationKind::PoolValue,
            &old,
            ENCODING.heap_region_base + 0x8765_4320,
            ENCODING,
        );
        assert_eq!(u64::from_le_bytes(new), tag | 0x8765_4320);
    }

    #[test]
    fn aarch64_compressed_moves_keep_their_registers() {
        // movz w3, #0x1111; movk w3, #0x2222, lsl #16
        let movz = 0x5280_0000 | (0x1111 << 5) | 3;
        let movk = 0x72a0_0000 | (0x2222 << 5) | 3;
        let mut old = [0; 8];
        old[..4].copy_from_slice(&u32::to_le_bytes(movz));
        old[4..].copy_from_slice(&u32::to_le_bytes(movk));
        assert_eq!(compressed_at(RelocationKind::Aarch64CompressedMove, &old), 0x2222_1111);
        let address = ENCODING.heap_region_base | (0xabcd_1234u64 << 3);
        let new = relocated_bytes(RelocationKind::Aarch64CompressedMove, &old, address, ENCODING);
        assert_eq!(compressed_at(RelocationKind::Aarch64CompressedMove, &new), 0xabcd_1234);
        assert_eq!(aarch64_instruction(&new, 0) & 0x1f, 3);
        assert_eq!(
            aarch64_instruction(&new, 1) & !AARCH64_MOVE_WIDE_IMMEDIATE_MASK,
            movk & !AARCH64_MOVE_WIDE_IMMEDIATE_MASK
        );
    }
}
