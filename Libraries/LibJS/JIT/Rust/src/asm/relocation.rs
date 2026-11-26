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

use ak::{Zone, ZoneVec};

use super::AsmError;

/// The address of a GC cell on the main thread, used as an opaque identity. It is never dereferenced by the compiler.
///
/// Its bits are private: code may hold a cell only through the operations of the macro assembler that take it as a
/// cell, which record where the code holds it, so that the code can be patched when a collection moves the cell (see
/// `asm::relocation`). Reading the bits is `unsafe` for that reason.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct CellId(u64);

impl CellId {
    pub const NULL: CellId = CellId(0);

    /// The cell at `address`, or null for 0.
    pub const fn from_address(address: u64) -> CellId {
        CellId(address)
    }

    pub fn is_null(self) -> bool {
        self.0 == 0
    }

    /// The address of the cell.
    ///
    /// # Safety
    ///
    /// The bits must not end up in compiled code or in its tables other than through the operations that record
    /// where they are (`asm::relocation`). Bits held anywhere else go stale when a collection moves the cell, and the
    /// code then uses memory the cell left.
    pub unsafe fn address(self) -> u64 {
        self.0
    }

    /// The address, for tests, whose fake heaps never move.
    #[cfg(test)]
    pub fn test_address(self) -> u64 {
        self.0
    }

    /// The cell's compressed pointer, which a field that holds the cell compares equal to without decoding.
    pub fn compressed(self) -> CompressedCell {
        CompressedCell::from_bits((self.0 >> COMPRESSED_POINTER_SHIFT) as u32)
    }
}

impl core::fmt::LowerHex for CellId {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        core::fmt::LowerHex::fmt(&self.0, f)
    }
}

/// A cell compiled code embeds, by its index among them (see `Artifact::embedded_cells()`). Tables hold it in place of
/// the address of the cell, so that they stay right when a collection moves the cell.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct EmbeddedCell(pub u32);

/// How compiled code holds a cell at the offset of a relocation.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
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
pub struct CellConstant {
    kind: RelocationKind,
    bits: u64,
}

impl CellConstant {
    /// The address of `cell`.
    pub fn address(cell: CellId) -> CellConstant {
        CellConstant {
            kind: RelocationKind::PoolAddress,
            // SAFETY: The constant is emitted only through the operations that record where it is.
            bits: unsafe { cell.address() },
        }
    }

    /// A value that is a cell: its tag bits, and the offset of the cell into the heap region.
    pub fn value(bits: u64) -> CellConstant {
        CellConstant {
            kind: RelocationKind::PoolValue,
            bits,
        }
    }

    /// The bits of the constant, which only the macro assemblers emit, recording where.
    pub(in crate::asm) fn bits(self) -> u64 {
        self.bits
    }

    /// The kind of the relocation of the pool entry that holds the constant.
    pub fn kind(self) -> RelocationKind {
        self.kind
    }
}

/// The compressed pointer of a cell (see `COMPRESSED_POINTER_SHIFT`), or null. Its bits are private for the same
/// reason as those of `CellId`: only the operations that take it as a cell emit it, recording where.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct CompressedCell(u32);

impl CompressedCell {
    pub const fn from_bits(bits: u32) -> CompressedCell {
        CompressedCell(bits)
    }

    pub fn is_null(self) -> bool {
        self.0 == 0
    }

    /// The bits of the compressed pointer, which only the macro assemblers emit, recording where.
    pub(in crate::asm) fn bits(self) -> u32 {
        self.0
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
    /// The value that is `cell`, with the tag bits `tag`, as code and tables hold tagged constants.
    pub fn value_of(&self, cell: CellId, tag: u64) -> u64 {
        // SAFETY: The value is a tagged constant, which code holds only through the operations that take it as a cell
        //         once its tag says it is one, and tables only as an embedded cell (see `EmbeddedCells`).
        tag | (unsafe { cell.address() } - self.heap_region_base)
    }

    /// The cell that the value `value` is, if its tag says it is one.
    pub fn cell_of_value(&self, value: u64) -> Option<CellId> {
        self.is_cell_value(value)
            .then(|| CellId::from_address(self.address_of_value(value)))
    }

    /// The tag bits of the value `value`, without the offset of its cell.
    pub fn tag_of_value(&self, value: u64) -> u64 {
        value & !self.value_offset_mask
    }

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

/// The cells compiled code embeds, in the order of `Artifact::embedded_cells()`, whose indices name them in its
/// relocations and tables: the tables of the code hold the index of a cell, never its address, so that only the code
/// needs patching when a collection moves the cell.
pub struct EmbeddedCells<'w> {
    cells: &'w [CellId],
    encoding: CellEncoding,
    /// The addresses of the cells, sorted, with their indices.
    by_address: ZoneVec<'w, (u64, u32)>,
}

impl<'w> EmbeddedCells<'w> {
    pub fn new_in(cells: &'w [CellId], encoding: CellEncoding, work: &'w Zone) -> Self {
        // SAFETY: The addresses stay in this list, which finds the cells of the places the macro assembler recorded.
        let mut by_address = ZoneVec::from_iter_in(
            (0u32..)
                .zip(cells)
                .map(|(index, cell)| (unsafe { cell.address() }, index)),
            work,
        );
        by_address.sort_unstable();
        Self {
            cells,
            encoding,
            by_address,
        }
    }

    pub fn encoding(&self) -> CellEncoding {
        self.encoding
    }

    pub fn cells(&self) -> &'w [CellId] {
        self.cells
    }

    /// The index of `cell`, if the code embeds it.
    pub fn index_of(&self, cell: CellId) -> Option<EmbeddedCell> {
        // SAFETY: The address is only looked up.
        let address = unsafe { cell.address() };
        self.by_address
            .binary_search_by_key(&address, |(address, _)| *address)
            .ok()
            .map(|index| EmbeddedCell(self.by_address[index].1))
    }

    pub fn cell(&self, index: EmbeddedCell) -> CellId {
        self.cells[index.0 as usize]
    }

    /// The sorted addresses of the cells, which the macro assemblers check plain bits against.
    pub(in crate::asm) fn addresses(&self) -> impl Iterator<Item = u64> + '_ {
        self.by_address.iter().map(|(address, _)| *address)
    }

    /// The relocations of `code` at `places`, the places of cells the macro assembler recorded as it emitted them.
    /// A cell the code does not embed would not be relocated, so the compile fails then.
    pub fn relocations<'z>(
        &self,
        code: &[u8],
        places: &[(u32, RelocationKind)],
        work: &'z Zone,
    ) -> Result<ZoneVec<'z, Relocation>, AsmError> {
        let mut by_compressed = ZoneVec::from_iter_in(
            self.by_address
                .iter()
                .map(|(address, index)| (CellEncoding::compressed(*address), *index)),
            work,
        );
        by_compressed.sort_unstable();
        let mut relocations = ZoneVec::new_in(work);
        for (offset, kind) in places {
            let bytes = &code[*offset as usize..];
            let found = match kind {
                RelocationKind::X86CompressedImmediate | RelocationKind::Aarch64CompressedMove => {
                    let compressed = compressed_at(*kind, bytes);
                    by_compressed
                        .binary_search_by_key(&compressed, |(compressed, _)| *compressed)
                        .map(|index| by_compressed[index].1)
                }
                RelocationKind::PoolAddress | RelocationKind::PoolValue => {
                    let word = u64::from_le_bytes(bytes[..8].try_into().expect("8 bytes"));
                    let address = self.encoding.address_in_pool(*kind, word);
                    self.by_address
                        .binary_search_by_key(&address, |(address, _)| *address)
                        .map(|index| self.by_address[index].1)
                }
            };
            relocations.push(Relocation {
                offset: *offset,
                kind: *kind,
                cell: found.map_err(|_| AsmError::UnknownCell)?,
            });
        }
        Ok(relocations)
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
