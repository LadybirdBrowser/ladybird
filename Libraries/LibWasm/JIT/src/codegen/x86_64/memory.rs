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

use super::{CodeGenerator, SCRATCH, fpr, gpr, precision, width};
use crate::asm::{Address, Scale, Width};
use crate::convention::{self, Register};
use crate::ir::{LoadKind, Op, Type, Value};
use crate::regalloc::CONSTANT_INPUT;

impl CodeGenerator<'_> {
    /// The address of an access of memory 0 with base `base`, index input `index` and offset `offset`.
    fn memory_address(&mut self, value: Value, base: Register, index: Register, offset: u32) -> Address {
        let base = gpr(base);
        if index == CONSTANT_INPUT {
            let constant = self.immediate(value, 1) & 0xffff_ffff;
            return Address::new(base, (constant + u64::from(offset)) as i32);
        }
        if let Ok(offset) = i32::try_from(offset) {
            return Address::indexed(base, gpr(index), Scale::One, offset);
        }
        // NB: Offsets of 2^31 and up do not fit a displacement; the index and offset, both below 2^32, add up
        //     without overflowing 64 bits.
        self.asm.mov_ri(SCRATCH, u64::from(offset));
        self.asm.add_rr(Width::W64, SCRATCH, gpr(index));
        Address::indexed(base, SCRATCH, Scale::One, 0)
    }

    pub(super) fn lower_memory(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        let record = *self.allocation.node(value);
        let [a, b, c] = record.inputs;
        let output = record.output;
        match node.op {
            Op::Load => {
                let address = self.memory_address(value, a, b, node.args[2]);
                let dst = output;
                match LoadKind::from_flags(node.flags) {
                    LoadKind::I32 | LoadKind::I64U32 => self.asm.mov_rm(Width::W32, gpr(dst), &address),
                    LoadKind::I64 => self.asm.mov_rm(Width::W64, gpr(dst), &address),
                    LoadKind::F32 => self
                        .asm
                        .vmovs_rm(crate::asm::x86_64::Precision::Single, fpr(dst), &address),
                    LoadKind::F64 => self
                        .asm
                        .vmovs_rm(crate::asm::x86_64::Precision::Double, fpr(dst), &address),
                    LoadKind::I32S8 => self.asm.movsx8_rm(Width::W32, gpr(dst), &address),
                    LoadKind::I64S8 => self.asm.movsx8_rm(Width::W64, gpr(dst), &address),
                    LoadKind::I32U8 | LoadKind::I64U8 => self.asm.movzx8_rm(gpr(dst), &address),
                    LoadKind::I32S16 => self.asm.movsx16_rm(Width::W32, gpr(dst), &address),
                    LoadKind::I64S16 => self.asm.movsx16_rm(Width::W64, gpr(dst), &address),
                    LoadKind::I32U16 | LoadKind::I64U16 => self.asm.movzx16_rm(gpr(dst), &address),
                    LoadKind::I64S32 => self.asm.movsxd_rm(gpr(dst), &address),
                }
            }
            Op::VectorLoad => {
                let address = self.memory_address(value, a, b, node.args[2]);
                self.vector_load(value, &address);
            }
            Op::Store => {
                let address = self.memory_address(value, a, b, node.args[2]);
                let stored = func.inputs(value)[2];
                self.store(stored, c, crate::ir::store_size(node.flags), &address);
            }
            Op::GlobalGet => {
                let address = Address::new(gpr(a), 0);
                if node.ty == Type::V128 {
                    self.load_vector(fpr(output).0, &address);
                } else if node.ty.is_float() {
                    self.asm.vmovs_rm(precision(node.ty), fpr(output), &address);
                } else {
                    self.asm.mov_rm(width(node.ty), gpr(output), &address);
                }
            }
            Op::GlobalSet => {
                let address = Address::new(gpr(a), 0);
                let stored = node.args[1];
                let size = match func.node(stored).ty {
                    Type::V128 => 16,
                    ty if ty.is_64() => 8,
                    _ => 4,
                };
                self.store(stored, b, size, &address);
            }
            Op::FrameLoad => {
                let mut address = Address::new(gpr(a), node.args[1] as i32);
                if node.flags == 1 {
                    self.asm.mov_rm(Width::W64, SCRATCH, &address);
                    address = Address::new(SCRATCH, 0);
                }
                if node.ty == Type::V128 {
                    self.load_vector(fpr(output).0, &address);
                } else if node.ty.is_float() {
                    self.asm.vmovs_rm(precision(node.ty), fpr(output), &address);
                } else {
                    self.asm.mov_rm(width(node.ty), gpr(output), &address);
                }
            }
            Op::InstanceField | Op::LoadPointer => {
                let address = Address::new(gpr(a), node.args[1] as i32);
                // NB: A 32-bit load zero-extends, as an i32 must be in its register.
                let load_width = if node.op == Op::LoadPointer && node.flags == 4 {
                    Width::W32
                } else {
                    Width::W64
                };
                self.asm.mov_rm(load_width, gpr(output), &address);
            }
            _ => unreachable!("{} is no memory op", node.op.name()),
        }
    }

    /// Stores `size` bytes of `stored`, which is in `register` or a constant immediate, at `address`.
    fn store(&mut self, stored: Value, register: Register, size: u32, address: &Address) {
        let ty = self.func.node(stored).ty;
        if register == CONSTANT_INPUT {
            let bits = self.func.node(stored).constant().expect("immediates are constants");
            match size {
                1 => self.asm.mov8_mi(address, bits as u8),
                2 => self.asm.mov16_mi(address, bits as u16),
                4 => self.asm.mov_mi(Width::W32, address, bits as u32 as i32),
                _ => self.asm.mov_mi(Width::W64, address, bits as i64 as i32),
            }
            return;
        }
        if size == 16 {
            self.store_vector(register, address);
            return;
        }
        if convention::is_fpr(register) {
            let p = if size == 8 || ty == Type::F64 {
                crate::asm::x86_64::Precision::Double
            } else {
                crate::asm::x86_64::Precision::Single
            };
            self.asm.vmovs_mr(p, address, fpr(register));
            return;
        }
        let register = gpr(register);
        match size {
            1 => self.asm.mov8_mr(address, register),
            2 => self.asm.mov16_mr(address, register),
            4 => self.asm.mov_mr(Width::W32, address, register),
            _ => self.asm.mov_mr(Width::W64, address, register),
        }
    }
}
