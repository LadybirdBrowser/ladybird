/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The decoder: one instruction at a time from a validated, quickened function body (see
//! `libwasm_module::quickening`).
//!
//! Validation already checked every immediate, so decoding does not check them again. It reads every instruction of
//! every proposal the engine supports, including the quickened forms, so that the builder can skip the immediates of
//! instructions it does not compile in unreachable code. Instructions the JIT does not compile come out as coarse
//! variants that only tell how they nest.

use libwasm_module::instruction_tables::{AtomicKind, SimdKind, atomic_kind, simd_kind};
use libwasm_module::opcode;
use libwasm_module::quickening;
use libwasm_module::types::{HeapType, RefType, ValType};

/// The type of a block, loop, if or try_table.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BlockType {
    Empty,
    Value(ValType),
    /// The type index of a function type, whose parameters the block takes and whose results it produces.
    Function(u32),
}

/// The memory immediate of a load or store.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MemoryArgument {
    pub memory: u32,
    pub offset: u64,
}

/// A decoded instruction. Numeric instructions without immediates are `Numeric` with their opcode.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Instruction {
    Unreachable,
    Nop,
    Block(BlockType),
    Loop(BlockType),
    If(BlockType),
    Else,
    End,
    Br(u32),
    BrIf(u32),
    /// `br_table` with `count` labels before the default; the decoder keeps the labels (see `Decoder::targets()`).
    BrTable {
        count: u32,
    },
    Return,
    Call(u32),
    CallIndirect {
        type_index: u32,
        table: u32,
    },
    ReturnCall(u32),
    ReturnCallIndirect {
        type_index: u32,
        table: u32,
    },
    CallRef(u32),
    ReturnCallRef(u32),
    Throw(u32),
    ThrowRef,
    /// A `try_table`, whose catch clauses the decoder skipped.
    TryTable(BlockType),
    /// The legacy exception handling instructions: `try`, `catch` and its tag, `catch_all`, `delegate` and its
    /// label, and `rethrow` and its label.
    Try(BlockType),
    Catch(u32),
    CatchAll,
    Delegate(u32),
    Rethrow(u32),
    Drop,
    Select,
    SelectTyped(ValType),
    LocalGet(u32),
    LocalSet(u32),
    LocalTee(u32),
    GlobalGet(u32),
    GlobalSet(u32),
    TableGet(u32),
    TableSet(u32),
    /// A core load or store (`opcode` from `I32_LOAD` to `I64_STORE32`) of memory 0, which is a memory32 with guard
    /// pages.
    Access {
        opcode: u8,
        argument: MemoryArgument,
    },
    /// The explicitly bounds-checked form of a core load or store (quickening rule 1).
    CheckedAccess {
        opcode: u8,
        argument: MemoryArgument,
    },
    MemorySize(u32),
    MemoryGrow(u32),
    I32Const(i32),
    I64Const(i64),
    /// The bits of an f32 constant.
    F32Const(u32),
    /// The bits of an f64 constant.
    F64Const(u64),
    /// An instruction from `I32_EQZ` to `I64_EXTEND32_S`, none of which has immediates.
    Numeric(u8),
    RefNull(HeapType),
    RefIsNull,
    RefFunc(u32),
    RefEq,
    RefAsNonNull,
    BrOnNull(u32),
    BrOnNonNull(u32),
    /// A `0xFC` instruction. The decoder keeps its index immediates (see `Decoder::misc_indices()`).
    Misc(u32),
    /// A `0xFB` instruction. The decoder keeps its immediates (see `Decoder::gc_indices()`, `gc_heap_types()` and
    /// `gc_flags()`).
    Gc(u32),
    /// A `0xFD` instruction. `checked` for the bounds-checked forms of accesses. The decoder keeps its immediates
    /// (see `Decoder::simd_argument()`, `simd_lane()` and `simd_bytes()`).
    Simd {
        sub_opcode: u32,
        checked: bool,
    },
    /// A `0xFE` instruction. The decoder keeps its memory immediate (see `Decoder::atomic_argument()`).
    Atomic(u32),
    /// A v128 form of `local.get`, `local.set`, `local.tee`, `global.get`, `global.set`, `drop` or `select`
    /// (quickening rule 2), and its index immediate if it has one.
    Vector {
        opcode: u8,
        index: u32,
    },
}

/// The last fixed-width form of `i64.const` (quickening rule 3), for 10 bytes.
const I64_CONST_FIXED_LAST: u8 = quickening::I64_CONST_FIXED + 8;

/// A cursor over a function body.
pub struct Decoder<'a> {
    bytes: &'a [u8],
    position: usize,
    end: usize,
    targets: Vec<u32>,
    simd_argument: MemoryArgument,
    simd_lane: u8,
    simd_bytes: [u8; 16],
    misc_indices: [u32; 2],
    gc_indices: [u32; 2],
    gc_heap_types: [HeapType; 2],
    gc_flags: u8,
    atomic_argument: MemoryArgument,
}

impl<'a> Decoder<'a> {
    /// A decoder over `bytes[start..end]`, which must be a validated function body (or its instructions).
    pub fn new(bytes: &'a [u8], start: usize, end: usize) -> Self {
        Self {
            bytes,
            position: start,
            end,
            targets: Vec::new(),
            simd_argument: MemoryArgument { memory: 0, offset: 0 },
            simd_lane: 0,
            simd_bytes: [0; 16],
            misc_indices: [0; 2],
            gc_indices: [0; 2],
            gc_heap_types: [HeapType::Any; 2],
            gc_flags: 0,
            atomic_argument: MemoryArgument { memory: 0, offset: 0 },
        }
    }

    /// Reuses the decoder's storage for another body.
    pub fn reset(&mut self, bytes: &'a [u8], start: usize, end: usize) {
        self.bytes = bytes;
        self.position = start;
        self.end = end;
    }

    /// The module offset of the next instruction.
    pub fn position(&self) -> usize {
        self.position
    }

    pub fn is_at_end(&self) -> bool {
        self.position >= self.end
    }

    /// The labels of the last `br_table`, the default last.
    pub fn targets(&self) -> &[u32] {
        &self.targets
    }

    /// The catch clauses of the last `try_table`, as its encoding has them: the kind (0 for catch, 1 catch_ref, 2
    /// catch_all, 3 catch_all_ref), the tag and the label of each.
    pub fn catch_clauses(&self) -> impl Iterator<Item = [u32; 3]> + '_ {
        self.targets.as_chunks::<3>().0.iter().copied()
    }

    /// The memory immediate of the last SIMD load or store.
    pub fn simd_argument(&self) -> MemoryArgument {
        self.simd_argument
    }

    /// The lane immediate of the last SIMD instruction with one.
    pub fn simd_lane(&self) -> u8 {
        self.simd_lane
    }

    /// The index immediates of the last `0xFC` instruction with any, in order: one, or two for memory.init,
    /// memory.copy, table.init and table.copy.
    pub fn misc_indices(&self) -> [u32; 2] {
        self.misc_indices
    }

    /// The index immediates of the last `0xFB` instruction with any, in order: a type index, then a field index, a
    /// count, or a data or element segment index; for `array.copy`, the two type indices; for `br_on_cast` and
    /// `br_on_cast_fail`, the label first.
    pub fn gc_indices(&self) -> [u32; 2] {
        self.gc_indices
    }

    /// The heap types of the last `ref.test`, `ref.cast` (the first only), `br_on_cast` or `br_on_cast_fail` (source,
    /// then target).
    pub fn gc_heap_types(&self) -> [HeapType; 2] {
        self.gc_heap_types
    }

    /// The flags byte of the last `br_on_cast` or `br_on_cast_fail`: bit 0 for a nullable source, bit 1 for a
    /// nullable target.
    pub fn gc_flags(&self) -> u8 {
        self.gc_flags
    }

    /// The memory immediate of the last atomic access.
    pub fn atomic_argument(&self) -> MemoryArgument {
        self.atomic_argument
    }

    /// The 16 bytes of the last `v128.const` or `i8x16.shuffle`.
    pub fn simd_bytes(&self) -> [u8; 16] {
        self.simd_bytes
    }

    #[inline]
    fn byte(&mut self) -> u8 {
        let byte = self.bytes[self.position];
        self.position += 1;
        byte
    }

    #[inline]
    fn u32(&mut self) -> u32 {
        let byte = self.byte();
        if byte < 0x80 {
            return u32::from(byte);
        }
        self.u64_continued(byte) as u32
    }

    fn u64_continued(&mut self, first: u8) -> u64 {
        let mut result = u64::from(first & 0x7f);
        let mut shift = 7;
        loop {
            let byte = self.byte();
            result |= u64::from(byte & 0x7f) << shift;
            if byte & 0x80 == 0 {
                return result;
            }
            shift += 7;
        }
    }

    fn u64(&mut self) -> u64 {
        let byte = self.byte();
        if byte < 0x80 {
            return u64::from(byte);
        }
        self.u64_continued(byte)
    }

    fn signed(&mut self) -> i64 {
        let mut result = 0i64;
        let mut shift = 0;
        loop {
            let byte = self.byte();
            result |= i64::from(byte & 0x7f) << shift;
            shift += 7;
            if byte & 0x80 == 0 {
                if shift < 64 && byte & 0x40 != 0 {
                    result |= -1i64 << shift;
                }
                return result;
            }
        }
    }

    /// The value of a fixed-width constant (quickening rule 3) of `length` bytes, for a type of `size` bytes: sign
    /// extended from its bytes, or its first `size` bytes where it has more.
    fn fixed_constant(&mut self, length: usize, size: usize) -> i64 {
        let bytes = &self.bytes[self.position..self.position + length];
        self.position += length;
        let stored = length.min(size);
        let mut value = 0u64;
        for (index, byte) in bytes[..stored].iter().enumerate() {
            value |= u64::from(*byte) << (8 * index);
        }
        let bits = 8 * stored as u32;
        if bits < 64 {
            let shift = 64 - bits;
            ((value << shift) as i64) >> shift
        } else {
            value as i64
        }
    }

    fn fixed_u32(&mut self) -> u32 {
        let bytes = &self.bytes[self.position..self.position + 4];
        self.position += 4;
        u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
    }

    fn fixed_u64(&mut self) -> u64 {
        let mut bytes = [0; 8];
        bytes.copy_from_slice(&self.bytes[self.position..self.position + 8]);
        self.position += 8;
        u64::from_le_bytes(bytes)
    }

    fn heap_type(&mut self) -> HeapType {
        let byte = self.bytes[self.position];
        let heap = match byte {
            0x70 => HeapType::Func,
            0x73 => HeapType::NoFunc,
            0x6f => HeapType::Extern,
            0x72 => HeapType::NoExtern,
            0x6e => HeapType::Any,
            0x6d => HeapType::Eq,
            0x6c => HeapType::I31,
            0x6b => HeapType::Struct,
            0x6a => HeapType::Array,
            0x71 => HeapType::None,
            0x69 => HeapType::Exn,
            0x74 => HeapType::NoExn,
            _ => return HeapType::Concrete(self.signed() as u32),
        };
        self.position += 1;
        heap
    }

    fn value_type(&mut self) -> ValType {
        let value = match self.bytes[self.position] {
            0x7f => ValType::I32,
            0x7e => ValType::I64,
            0x7d => ValType::F32,
            0x7c => ValType::F64,
            0x7b => ValType::V128,
            0x63 => {
                self.position += 1;
                return ValType::Ref(RefType::new(true, self.heap_type()));
            }
            0x64 => {
                self.position += 1;
                return ValType::Ref(RefType::new(false, self.heap_type()));
            }
            _ => return ValType::Ref(RefType::new(true, self.heap_type())),
        };
        self.position += 1;
        value
    }

    fn block_type(&mut self) -> BlockType {
        match self.bytes[self.position] {
            0x40 => {
                self.position += 1;
                BlockType::Empty
            }
            0x7f | 0x7e | 0x7d | 0x7c | 0x7b | 0x63 | 0x64 | 0x70 | 0x6f | 0x6e | 0x6d | 0x6c | 0x6b | 0x6a | 0x69
            | 0x71 | 0x72 | 0x73 | 0x74 => BlockType::Value(self.value_type()),
            _ => BlockType::Function(self.signed() as u32),
        }
    }

    fn memory_argument(&mut self) -> MemoryArgument {
        let alignment = self.u32();
        let memory = if alignment & 0x40 != 0 { self.u32() } else { 0 };
        MemoryArgument {
            memory,
            offset: self.u64(),
        }
    }

    /// Decodes the next instruction.
    pub fn decode(&mut self) -> Instruction {
        let opcode = self.byte();
        match opcode {
            opcode::UNREACHABLE => Instruction::Unreachable,
            opcode::NOP => Instruction::Nop,
            opcode::BLOCK => Instruction::Block(self.block_type()),
            opcode::LOOP => Instruction::Loop(self.block_type()),
            opcode::IF => Instruction::If(self.block_type()),
            opcode::ELSE => Instruction::Else,
            opcode::END => Instruction::End,
            opcode::BR => Instruction::Br(self.u32()),
            opcode::BR_IF => Instruction::BrIf(self.u32()),
            opcode::BR_TABLE => {
                let count = self.u32();
                self.targets.clear();
                for _ in 0..=count {
                    let target = self.u32();
                    self.targets.push(target);
                }
                Instruction::BrTable { count }
            }
            opcode::RETURN => Instruction::Return,
            opcode::CALL => Instruction::Call(self.u32()),
            opcode::CALL_INDIRECT => {
                let type_index = self.u32();
                Instruction::CallIndirect {
                    type_index,
                    table: self.u32(),
                }
            }
            opcode::RETURN_CALL => Instruction::ReturnCall(self.u32()),
            opcode::RETURN_CALL_INDIRECT => {
                let type_index = self.u32();
                Instruction::ReturnCallIndirect {
                    type_index,
                    table: self.u32(),
                }
            }
            opcode::CALL_REF => Instruction::CallRef(self.u32()),
            opcode::RETURN_CALL_REF => Instruction::ReturnCallRef(self.u32()),
            opcode::THROW => Instruction::Throw(self.u32()),
            opcode::THROW_REF => Instruction::ThrowRef,
            opcode::TRY => Instruction::Try(self.block_type()),
            opcode::CATCH => Instruction::Catch(self.u32()),
            opcode::CATCH_ALL => Instruction::CatchAll,
            opcode::DELEGATE => Instruction::Delegate(self.u32()),
            opcode::RETHROW => Instruction::Rethrow(self.u32()),
            opcode::TRY_TABLE => {
                let block_type = self.block_type();
                let count = self.u32();
                self.targets.clear();
                for _ in 0..count {
                    let kind = self.byte();
                    let tag = if kind < 2 { self.u32() } else { 0 };
                    let label = self.u32();
                    self.targets.extend_from_slice(&[u32::from(kind), tag, label]);
                }
                Instruction::TryTable(block_type)
            }
            opcode::DROP => Instruction::Drop,
            opcode::SELECT => Instruction::Select,
            opcode::SELECT_TYPED => {
                self.u32();
                Instruction::SelectTyped(self.value_type())
            }
            opcode::LOCAL_GET => Instruction::LocalGet(self.u32()),
            opcode::LOCAL_SET => Instruction::LocalSet(self.u32()),
            opcode::LOCAL_TEE => Instruction::LocalTee(self.u32()),
            opcode::GLOBAL_GET => Instruction::GlobalGet(self.u32()),
            opcode::GLOBAL_SET => Instruction::GlobalSet(self.u32()),
            opcode::TABLE_GET => Instruction::TableGet(self.u32()),
            opcode::TABLE_SET => Instruction::TableSet(self.u32()),
            opcode::I32_LOAD..=opcode::I64_STORE32 => Instruction::Access {
                opcode,
                argument: self.memory_argument(),
            },
            opcode::MEMORY_SIZE => Instruction::MemorySize(self.u32()),
            opcode::MEMORY_GROW => Instruction::MemoryGrow(self.u32()),
            opcode::I32_CONST => Instruction::I32Const(self.signed() as i32),
            opcode::I64_CONST => Instruction::I64Const(self.signed()),
            opcode::F32_CONST => Instruction::F32Const(self.fixed_u32()),
            opcode::F64_CONST => Instruction::F64Const(self.fixed_u64()),
            opcode::I32_EQZ..=opcode::I64_EXTEND32_S => Instruction::Numeric(opcode),
            opcode::REF_NULL => Instruction::RefNull(self.heap_type()),
            opcode::REF_IS_NULL => Instruction::RefIsNull,
            opcode::REF_FUNC => Instruction::RefFunc(self.u32()),
            opcode::REF_EQ => Instruction::RefEq,
            opcode::REF_AS_NON_NULL => Instruction::RefAsNonNull,
            opcode::BR_ON_NULL => Instruction::BrOnNull(self.u32()),
            opcode::BR_ON_NON_NULL => Instruction::BrOnNonNull(self.u32()),
            opcode::PREFIX_MISC => self.misc(),
            opcode::PREFIX_GC => self.gc(),
            opcode::PREFIX_SIMD => {
                let sub_opcode = self.u32();
                self.simd_immediates(sub_opcode);
                Instruction::Simd {
                    sub_opcode,
                    checked: false,
                }
            }
            opcode::PREFIX_ATOMIC => {
                let sub_opcode = self.u32();
                match atomic_kind(sub_opcode) {
                    Some(AtomicKind::Fence) => {
                        self.byte();
                    }
                    Some(_) => {
                        self.atomic_argument = self.memory_argument();
                    }
                    None => unreachable!("validated code has no unknown atomic instructions"),
                }
                Instruction::Atomic(sub_opcode)
            }
            quickening::QUICK_PREFIX => self.quickened(),
            quickening::I32_CONST_FIXED..quickening::I64_CONST_FIXED => {
                let length = usize::from(opcode - quickening::I32_CONST_FIXED) + 2;
                Instruction::I32Const(self.fixed_constant(length, 4) as i32)
            }
            quickening::I64_CONST_FIXED..=I64_CONST_FIXED_LAST => {
                let length = usize::from(opcode - quickening::I64_CONST_FIXED) + 2;
                Instruction::I64Const(self.fixed_constant(length, 8))
            }
            quickening::LOCAL_GET_V128..=quickening::GLOBAL_SET_V128 => Instruction::Vector {
                opcode,
                index: self.u32(),
            },
            quickening::DROP_V128 | quickening::SELECT_V128 => Instruction::Vector { opcode, index: 0 },
            _ => unreachable!("validated code has no unknown opcode {opcode:#x}"),
        }
    }

    fn misc(&mut self) -> Instruction {
        let sub_opcode = self.u32();
        match sub_opcode {
            opcode::misc::I32_TRUNC_SAT_F32_S..=opcode::misc::I64_TRUNC_SAT_F64_U => {}
            // memory.init, memory.copy, table.init and table.copy take two indices.
            opcode::misc::MEMORY_INIT
            | opcode::misc::MEMORY_COPY
            | opcode::misc::TABLE_INIT
            | opcode::misc::TABLE_COPY => {
                self.misc_indices = [self.u32(), self.u32()];
            }
            _ => {
                self.misc_indices = [self.u32(), 0];
            }
        }
        Instruction::Misc(sub_opcode)
    }

    fn gc(&mut self) -> Instruction {
        use opcode::gc::*;
        let sub_opcode = self.u32();
        match sub_opcode {
            STRUCT_NEW | STRUCT_NEW_DEFAULT | ARRAY_NEW | ARRAY_NEW_DEFAULT | ARRAY_GET | ARRAY_GET_S | ARRAY_GET_U
            | ARRAY_SET | ARRAY_FILL => {
                self.gc_indices = [self.u32(), 0];
            }
            STRUCT_GET | STRUCT_GET_S | STRUCT_GET_U | STRUCT_SET | ARRAY_NEW_FIXED | ARRAY_NEW_DATA
            | ARRAY_NEW_ELEM | ARRAY_COPY | ARRAY_INIT_DATA | ARRAY_INIT_ELEM => {
                self.gc_indices = [self.u32(), self.u32()];
            }
            REF_TEST | REF_TEST_NULL | REF_CAST | REF_CAST_NULL => {
                self.gc_heap_types[0] = self.heap_type();
            }
            BR_ON_CAST | BR_ON_CAST_FAIL => {
                self.gc_flags = self.byte();
                self.gc_indices[0] = self.u32();
                self.gc_heap_types = [self.heap_type(), self.heap_type()];
            }
            _ => {}
        }
        Instruction::Gc(sub_opcode)
    }

    fn simd_immediates(&mut self, sub_opcode: u32) {
        match simd_kind(sub_opcode) {
            Some(SimdKind::Load { .. } | SimdKind::Store { .. }) => {
                self.simd_argument = self.memory_argument();
            }
            Some(SimdKind::LoadLane { .. } | SimdKind::StoreLane { .. }) => {
                self.simd_argument = self.memory_argument();
                self.simd_lane = self.byte();
            }
            Some(SimdKind::Const | SimdKind::Shuffle) => {
                self.simd_bytes
                    .copy_from_slice(&self.bytes[self.position..self.position + 16]);
                self.position += 16;
            }
            Some(SimdKind::ExtractLane { .. } | SimdKind::ReplaceLane { .. }) => {
                self.simd_lane = self.byte();
            }
            Some(_) => {}
            None => unreachable!("validated code has no unknown SIMD instructions"),
        }
    }

    fn quickened(&mut self) -> Instruction {
        let sub_opcode = self.byte();
        if sub_opcode < quickening::CHECKED_SIMD_ACCESS {
            let memory = if sub_opcode & quickening::CHECKED_ACCESS_HAS_MEMORY_INDEX != 0 {
                self.u32()
            } else {
                0
            };
            return Instruction::CheckedAccess {
                opcode: sub_opcode & !quickening::CHECKED_ACCESS_HAS_MEMORY_INDEX,
                argument: MemoryArgument {
                    memory,
                    offset: self.u64(),
                },
            };
        }
        let index = sub_opcode - quickening::CHECKED_SIMD_ACCESS;
        let has_memory = index & quickening::CHECKED_SIMD_ACCESS_HAS_MEMORY_INDEX != 0;
        let simd = quickening::SIMD_ACCESSES[(index & !quickening::CHECKED_SIMD_ACCESS_HAS_MEMORY_INDEX) as usize];
        let memory = if has_memory { self.u32() } else { 0 };
        self.simd_argument = MemoryArgument {
            memory,
            offset: self.u64(),
        };
        if matches!(
            simd_kind(simd),
            Some(SimdKind::LoadLane { .. } | SimdKind::StoreLane { .. })
        ) {
            self.simd_lane = self.byte();
        }
        Instruction::Simd {
            sub_opcode: simd,
            checked: true,
        }
    }
}

/// How an instruction changes the nesting of blocks: +1 for those that open one, -1 for `end`.
pub fn nesting_change(instruction: &Instruction) -> i32 {
    match instruction {
        Instruction::Block(_)
        | Instruction::Loop(_)
        | Instruction::If(_)
        | Instruction::TryTable(_)
        | Instruction::Try(_) => 1,
        // NB: `catch` and `catch_all` continue the `try` they are in, like `else`.
        Instruction::End | Instruction::Delegate(_) => -1,
        _ => 0,
    }
}
