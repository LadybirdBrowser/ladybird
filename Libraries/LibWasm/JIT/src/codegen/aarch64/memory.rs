/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Loads and stores of memory 0, globals and instance data.
//!
//! Memory 0 is a memory32 with guard pages: an access is at base + zero-extended index + offset, with no bounds
//! check, and faults in the guard region where it is out of bounds. i32 values are always zero-extended in
//! registers, so the index register serves as is.

use super::{ADDRESS, CodeGenerator, SCRATCH, fits_offset, fpr, fpr_access, gpr, gpr_access};
use crate::asm::aarch64::{AtomicSize, Extend, MemOp, MemOperand, XZR};
use crate::asm::{Gpr, Width};
use crate::convention::{self, Register};
use crate::ir::{LOAD_ATOMIC, LoadKind, Op, Type, Value};
use crate::regalloc::CONSTANT_INPUT;

/// The memory operation of a load of kind `kind` into a general purpose register.
fn load_op(kind: LoadKind) -> MemOp {
    match kind {
        LoadKind::I32 | LoadKind::I64U32 => MemOp::LdrW,
        LoadKind::I64 => MemOp::LdrX,
        LoadKind::F32 => MemOp::LdrS,
        LoadKind::F64 => MemOp::LdrD,
        LoadKind::I32S8 => MemOp::LdrsbW,
        LoadKind::I64S8 => MemOp::LdrsbX,
        LoadKind::I32U8 | LoadKind::I64U8 => MemOp::Ldrb,
        LoadKind::I32S16 => MemOp::LdrshW,
        LoadKind::I64S16 => MemOp::LdrshX,
        LoadKind::I32U16 | LoadKind::I64U16 => MemOp::Ldrh,
        LoadKind::I64S32 => MemOp::Ldrsw,
    }
}

impl CodeGenerator<'_> {
    /// The operand of an access of `1 << size_log2` bytes of memory 0 with base `base`, index input `index` and
    /// offset `offset`.
    pub(super) fn memory_operand(
        &mut self,
        value: Value,
        base: Register,
        index: Register,
        offset: u32,
        size_log2: u32,
    ) -> MemOperand {
        let base = gpr(base);
        if index == CONSTANT_INPUT {
            let constant = (self.immediate(value, 1) & 0xffff_ffff) + u64::from(offset);
            if fits_offset(constant as i64, size_log2) {
                return MemOperand::Offset(base, constant as i32);
            }
            self.asm.mov_imm(Width::W64, ADDRESS, constant);
            return MemOperand::Register {
                base,
                index: ADDRESS,
                extend: Extend::Uxtx,
                shifted: false,
            };
        }
        if offset == 0 {
            return MemOperand::Register {
                base,
                index: gpr(index),
                extend: Extend::Uxtx,
                shifted: false,
            };
        }
        // NB: The index and offset, both below 2^32, add up without overflowing 64 bits.
        self.asm.add_any(ADDRESS, gpr(index), i64::from(offset), ADDRESS);
        MemOperand::Register {
            base,
            index: ADDRESS,
            extend: Extend::Uxtx,
            shifted: false,
        }
    }

    /// The address of an access in a register: `base` itself if `operand` is `[base]`, else x17.
    pub(super) fn address_register(&mut self, operand: MemOperand) -> Gpr {
        match operand {
            MemOperand::Offset(base, 0) => base,
            MemOperand::Offset(base, offset) => {
                self.asm.add_any(ADDRESS, base, i64::from(offset), ADDRESS);
                ADDRESS
            }
            MemOperand::Register { base, index, .. } => {
                self.asm.add_extended(Width::W64, ADDRESS, base, index, Extend::Uxtx, 0);
                ADDRESS
            }
            _ => unreachable!("no address of {operand:?}"),
        }
    }

    pub(super) fn lower_memory(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        let record = *self.allocation.node(value);
        let [a, b, c] = record.inputs;
        let output = record.output;
        match node.op {
            Op::Load => {
                let kind = LoadKind::from_flags(node.flags & !LOAD_ATOMIC);
                let size_log2 = kind.size().trailing_zeros();
                let operand = self.memory_operand(value, a, b, node.args[2], size_log2);
                if node.flags & LOAD_ATOMIC != 0 {
                    // Acquire loads, with the stores' exchanges, make atomic accesses sequentially consistent.
                    let address = self.address_register(operand);
                    self.asm
                        .load_acquire(AtomicSize::from_log2(size_log2), gpr(output), address);
                    return;
                }
                let op = load_op(kind);
                let register = if kind.size() >= 4 && matches!(kind, LoadKind::F32 | LoadKind::F64) {
                    fpr(output).0
                } else {
                    gpr(output).0
                };
                self.asm.load_store(op, register, operand);
            }
            Op::VectorLoad => {
                let operand_of = |generator: &mut Self, size_log2: u32| {
                    generator.memory_operand(value, a, b, node.args[2], size_log2)
                };
                self.vector_load(value, operand_of);
            }
            Op::Store => {
                let size = crate::ir::store_size(node.flags);
                let size_log2 = size.trailing_zeros();
                let operand = self.memory_operand(value, a, b, node.args[2], size_log2);
                let stored = func.inputs(value)[2];
                self.store(stored, c, size_log2, operand);
            }
            Op::GlobalGet => {
                let size_log2 = if node.ty == Type::V128 {
                    4
                } else if node.ty.is_64() {
                    3
                } else {
                    2
                };
                let operand = MemOperand::Offset(gpr(a), 0);
                if node.ty.uses_xmm() {
                    self.asm.load_store(fpr_access(size_log2, true), fpr(output).0, operand);
                } else {
                    self.asm.load_store(gpr_access(size_log2, true), gpr(output).0, operand);
                }
            }
            Op::GlobalSet => {
                let stored = node.args[1];
                let size_log2 = match func.node(stored).ty {
                    Type::V128 => 4,
                    ty if ty.is_64() => 3,
                    _ => 2,
                };
                self.store(stored, b, size_log2, MemOperand::Offset(gpr(a), 0));
            }
            Op::FrameLoad => {
                let size_log2 = if node.ty == Type::V128 {
                    4
                } else if node.ty.is_64() {
                    3
                } else {
                    2
                };
                // NB: The slot of a v128 local holds the address of its storage, 8 bytes; other slots the value, whose
                //     size decides which offsets the load takes.
                let slot_size_log2 = if node.flags == 1 { 3 } else { size_log2 };
                let mut operand = self.operand(gpr(a), i64::from(node.args[1] as i32), slot_size_log2);
                if node.flags == 1 {
                    self.asm.ldr(Width::W64, ADDRESS, operand);
                    operand = MemOperand::Offset(ADDRESS, 0);
                }
                if node.ty.uses_xmm() {
                    self.asm.load_store(fpr_access(size_log2, true), fpr(output).0, operand);
                } else {
                    self.asm.load_store(gpr_access(size_log2, true), gpr(output).0, operand);
                }
            }
            Op::InstanceField | Op::LoadPointer => {
                let size_log2 = if node.op == Op::LoadPointer && node.flags == 4 {
                    2
                } else {
                    3
                };
                let operand = self.operand(gpr(a), i64::from(node.args[1]), size_log2);
                self.asm.load_store(gpr_access(size_log2, true), gpr(output).0, operand);
            }
            _ => unreachable!("{} is no memory op", node.op.name()),
        }
    }

    /// Stores `1 << size_log2` bytes of `stored`, which is in `register` or a constant immediate, at `operand`.
    pub(super) fn store(&mut self, stored: Value, register: Register, size_log2: u32, operand: MemOperand) {
        if register == CONSTANT_INPUT {
            let node = self.func.node(stored);
            if node.op == Op::Const128 {
                let bits = self.constant_bits(stored);
                self.load_vector_constant(bits, super::SCRATCH_FPR, true);
                self.asm.load_store(fpr_access(4, false), super::SCRATCH_FPR.0, operand);
                return;
            }
            let bits = node.constant().expect("immediates are constants");
            let source = if bits == 0 {
                XZR
            } else {
                self.asm.mov_imm(Width::W64, SCRATCH, bits);
                SCRATCH
            };
            self.asm.load_store(gpr_access(size_log2, false), source.0, operand);
            return;
        }
        if convention::is_fpr(register) {
            self.asm
                .load_store(fpr_access(size_log2.max(2), false), fpr(register).0, operand);
            return;
        }
        self.asm
            .load_store(gpr_access(size_log2, false), gpr(register).0, operand);
    }
}
