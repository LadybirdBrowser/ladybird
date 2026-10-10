/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! x86-64 instruction encoder.

mod macro_assembler;
#[cfg(test)]
mod tests;

pub use macro_assembler::MacroAssembler;

use super::Address;
use super::Fpr;
use super::Gpr;
use super::Width;
use super::buffer::AsmError;
use super::buffer::CodeBuffer;
use super::buffer::FixupKind;
use super::buffer::Label;

pub const RAX: Gpr = Gpr(0);
pub const RCX: Gpr = Gpr(1);
pub const RDX: Gpr = Gpr(2);
pub const RBX: Gpr = Gpr(3);
pub const RSP: Gpr = Gpr(4);
pub const RBP: Gpr = Gpr(5);
pub const RSI: Gpr = Gpr(6);
pub const RDI: Gpr = Gpr(7);
pub const R8: Gpr = Gpr(8);
pub const R9: Gpr = Gpr(9);
pub const R10: Gpr = Gpr(10);
pub const R11: Gpr = Gpr(11);
pub const R12: Gpr = Gpr(12);
pub const R13: Gpr = Gpr(13);
pub const R14: Gpr = Gpr(14);
pub const R15: Gpr = Gpr(15);

pub const XMM0: Fpr = Fpr(0);
pub const XMM1: Fpr = Fpr(1);
pub const XMM2: Fpr = Fpr(2);
pub const XMM3: Fpr = Fpr(3);
pub const XMM4: Fpr = Fpr(4);
pub const XMM5: Fpr = Fpr(5);
pub const XMM6: Fpr = Fpr(6);
pub const XMM7: Fpr = Fpr(7);
pub const XMM8: Fpr = Fpr(8);
pub const XMM9: Fpr = Fpr(9);
pub const XMM10: Fpr = Fpr(10);
pub const XMM11: Fpr = Fpr(11);
pub const XMM12: Fpr = Fpr(12);
pub const XMM13: Fpr = Fpr(13);
pub const XMM14: Fpr = Fpr(14);
pub const XMM15: Fpr = Fpr(15);

/// Condition codes, by their encoding in Jcc/SETcc/CMOVcc.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Cond {
    Overflow = 0x0,
    NoOverflow = 0x1,
    Below = 0x2,
    AboveOrEqual = 0x3,
    Equal = 0x4,
    NotEqual = 0x5,
    BelowOrEqual = 0x6,
    Above = 0x7,
    Sign = 0x8,
    NotSign = 0x9,
    Parity = 0xa,
    NotParity = 0xb,
    Less = 0xc,
    GreaterOrEqual = 0xd,
    LessOrEqual = 0xe,
    Greater = 0xf,
}

impl Cond {
    pub const fn invert(self) -> Self {
        match self {
            Cond::Overflow => Cond::NoOverflow,
            Cond::NoOverflow => Cond::Overflow,
            Cond::Below => Cond::AboveOrEqual,
            Cond::AboveOrEqual => Cond::Below,
            Cond::Equal => Cond::NotEqual,
            Cond::NotEqual => Cond::Equal,
            Cond::BelowOrEqual => Cond::Above,
            Cond::Above => Cond::BelowOrEqual,
            Cond::Sign => Cond::NotSign,
            Cond::NotSign => Cond::Sign,
            Cond::Parity => Cond::NotParity,
            Cond::NotParity => Cond::Parity,
            Cond::Less => Cond::GreaterOrEqual,
            Cond::GreaterOrEqual => Cond::Less,
            Cond::LessOrEqual => Cond::Greater,
            Cond::Greater => Cond::LessOrEqual,
        }
    }
}

/// The eight classic ALU operations, by their /digit (and opcode row).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AluOp {
    Add = 0,
    Or = 1,
    Adc = 2,
    Sbb = 3,
    And = 4,
    Sub = 5,
    Xor = 6,
    Cmp = 7,
}

/// Shift and rotate operations, by their /digit.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShiftOp {
    Rol = 0,
    Ror = 1,
    Shl = 4,
    Shr = 5,
    Sar = 7,
}

/// The r/m operand of an instruction.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Rm {
    Reg(u8),
    Mem(Address),
    /// RIP-relative disp32, patched to point at a label.
    Rip(Label),
}

/// Whether register operands of an instruction are byte registers, which need
/// a REX prefix to name SPL/BPL/SIL/DIL instead of AH/CH/DH/BH.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ByteRegisters {
    No,
    Reg,
    Rm,
}

/// Operand-size and mandatory prefixes, emitted in front of REX.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Prefix {
    None,
    OperandSize,
    Rep,
    Repne,
}

/// x86-64 machine code assembler.
#[derive(Debug, Default)]
pub struct Assembler {
    buffer: CodeBuffer,
}

impl Assembler {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn offset(&self) -> usize {
        self.buffer.offset()
    }

    pub fn buffer(&self) -> &CodeBuffer {
        &self.buffer
    }

    pub fn new_label(&mut self) -> Label {
        self.buffer.new_label()
    }

    pub fn bind(&mut self, label: Label) {
        self.buffer.bind(label);
    }

    pub fn label_offset(&self, label: Label) -> Option<usize> {
        self.buffer.label_offset(label)
    }

    pub fn finish(self) -> Result<Vec<u8>, AsmError> {
        self.buffer.finish()
    }

    /// `finish()`, with the end of the code as the data offset: the
    /// assembler itself emits no constant pool.
    pub fn finish_with_data_offset(self) -> Result<(Vec<u8>, usize), AsmError> {
        let code = self.finish()?;
        let length = code.len();
        Ok((code, length))
    }

    // Data emission.

    pub fn data_u8(&mut self, value: u8) {
        self.buffer.emit_u8(value);
    }

    pub fn data_u32(&mut self, value: u32) {
        self.buffer.emit_u32(value);
    }

    pub fn data_u64(&mut self, value: u64) {
        self.buffer.emit_u64(value);
    }

    pub fn data_bytes(&mut self, bytes: &[u8]) {
        self.buffer.emit_bytes(bytes);
    }

    /// Pads with multi-byte NOPs until the offset is a multiple of `alignment`.
    pub fn align_code(&mut self, alignment: usize) {
        assert!(alignment.is_power_of_two());
        let padding = self.offset().next_multiple_of(alignment) - self.offset();
        self.nops(padding);
    }

    /// Pads with zero bytes until the offset is a multiple of `alignment`.
    pub fn align_data(&mut self, alignment: usize) {
        self.buffer.align_with(alignment, 0);
    }

    // Encoding helpers.

    fn emit_rex(&mut self, w: bool, reg: u8, rm: &Rm, byte_registers: ByteRegisters) {
        let mut rex = 0x40;
        if w {
            rex |= 0x08;
        }
        if reg & 8 != 0 {
            rex |= 0x04;
        }
        match rm {
            Rm::Reg(r) => {
                if r & 8 != 0 {
                    rex |= 0x01;
                }
            }
            Rm::Mem(address) => {
                if let Some((index, _)) = address.index
                    && index.0 & 8 != 0
                {
                    rex |= 0x02;
                }
                if address.base.0 & 8 != 0 {
                    rex |= 0x01;
                }
            }
            Rm::Rip(_) => {}
        }
        let needs_byte_rex = match (byte_registers, rm) {
            (ByteRegisters::Reg, _) => (4..8).contains(&reg),
            (ByteRegisters::Rm, Rm::Reg(r)) => (4..8).contains(r),
            _ => false,
        };
        if rex != 0x40 || needs_byte_rex {
            self.buffer.emit_u8(rex);
        }
    }

    fn emit_modrm_rm(&mut self, reg: u8, rm: &Rm, trailing_immediate_size: usize) {
        let reg = (reg & 7) << 3;
        match rm {
            Rm::Reg(r) => self.buffer.emit_u8(0xc0 | reg | (r & 7)),
            Rm::Rip(label) => {
                self.buffer.emit_u8(reg | 0b101);
                let at = self.offset();
                self.buffer.emit_u32(0);
                // The displacement is relative to the end of the instruction,
                // which is the end of the disp32 only without an immediate.
                assert_eq!(
                    trailing_immediate_size, 0,
                    "RIP-relative operand with trailing immediate"
                );
                self.buffer.add_fixup(*label, at, FixupKind::X86Rel32);
            }
            Rm::Mem(address) => {
                let base = address.base.0 & 7;
                let displacement = address.displacement;
                // [rbp]/[r13] without displacement would mean RIP-relative or
                // no base, so they always get at least a disp8.
                let mode = if displacement == 0 && base != 5 {
                    0b00
                } else if i8::try_from(displacement).is_ok() {
                    0b01
                } else {
                    0b10
                };
                match address.index {
                    None if base != 4 => {
                        self.buffer.emit_u8((mode << 6) | reg | base);
                    }
                    None => {
                        // [rsp]/[r12] as base need a SIB byte with no index.
                        self.buffer.emit_u8((mode << 6) | reg | 0b100);
                        self.buffer.emit_u8(0b00_100_100);
                    }
                    Some((index, scale)) => {
                        assert_ne!(index.0, 4, "rsp cannot be an index register");
                        self.buffer.emit_u8((mode << 6) | reg | 0b100);
                        self.buffer.emit_u8((scale.log2() << 6) | ((index.0 & 7) << 3) | base);
                    }
                }
                match mode {
                    0b01 => self.buffer.emit_u8(displacement as i8 as u8),
                    0b10 => self.buffer.emit_u32(displacement as u32),
                    _ => {}
                }
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn emit_instruction(
        &mut self,
        prefix: Prefix,
        w: bool,
        opcode: &[u8],
        reg: u8,
        rm: Rm,
        byte_registers: ByteRegisters,
        trailing_immediate_size: usize,
    ) {
        match prefix {
            Prefix::None => {}
            Prefix::OperandSize => self.buffer.emit_u8(0x66),
            Prefix::Rep => self.buffer.emit_u8(0xf3),
            Prefix::Repne => self.buffer.emit_u8(0xf2),
        }
        self.emit_rex(w, reg, &rm, byte_registers);
        self.buffer.emit_bytes(opcode);
        self.emit_modrm_rm(reg, &rm, trailing_immediate_size);
    }

    fn op_rr(&mut self, width: Width, opcode: &[u8], reg: Gpr, rm: Gpr) {
        self.emit_instruction(
            Prefix::None,
            width == Width::W64,
            opcode,
            reg.0,
            Rm::Reg(rm.0),
            ByteRegisters::No,
            0,
        );
    }

    fn op_rm(&mut self, width: Width, opcode: &[u8], reg: u8, address: &Address) {
        self.emit_instruction(
            Prefix::None,
            width == Width::W64,
            opcode,
            reg,
            Rm::Mem(*address),
            ByteRegisters::No,
            0,
        );
    }

    fn sse_rr(&mut self, prefix: Prefix, w: bool, opcode: &[u8], reg: u8, rm: u8) {
        self.emit_instruction(prefix, w, opcode, reg, Rm::Reg(rm), ByteRegisters::No, 0);
    }

    // Moves.

    /// `mov dst, src`. The 32-bit form zero-extends into the upper half.
    pub fn mov_rr(&mut self, width: Width, dst: Gpr, src: Gpr) {
        self.op_rr(width, &[0x89], src, dst);
    }

    /// `mov dst, [address]`.
    pub fn mov_rm(&mut self, width: Width, dst: Gpr, address: &Address) {
        self.op_rm(width, &[0x8b], dst.0, address);
    }

    /// `mov [address], src`.
    pub fn mov_mr(&mut self, width: Width, address: &Address, src: Gpr) {
        self.op_rm(width, &[0x89], src.0, address);
    }

    /// `mov r32, imm32`, zero-extending into the upper half.
    pub fn mov_ri32(&mut self, dst: Gpr, imm: u32) {
        self.emit_rex(false, 0, &Rm::Reg(dst.0), ByteRegisters::No);
        self.buffer.emit_u8(0xb8 | (dst.0 & 7));
        self.buffer.emit_u32(imm);
    }

    /// `mov r64, simm32`, sign-extending the immediate.
    pub fn mov_ri_sign_extended(&mut self, dst: Gpr, imm: i32) {
        self.emit_instruction(Prefix::None, true, &[0xc7], 0, Rm::Reg(dst.0), ByteRegisters::No, 4);
        self.buffer.emit_u32(imm as u32);
    }

    /// `movabs r64, imm64`.
    pub fn movabs(&mut self, dst: Gpr, imm: u64) {
        self.emit_rex(true, 0, &Rm::Reg(dst.0), ByteRegisters::No);
        self.buffer.emit_u8(0xb8 | (dst.0 & 7));
        self.buffer.emit_u64(imm);
    }

    /// Materializes a 64-bit immediate with the shortest encoding. Never
    /// changes the flags.
    pub fn mov_ri(&mut self, dst: Gpr, imm: u64) {
        if let Ok(imm) = u32::try_from(imm) {
            self.mov_ri32(dst, imm);
        } else if let Ok(imm) = i32::try_from(imm as i64) {
            self.mov_ri_sign_extended(dst, imm);
        } else {
            self.movabs(dst, imm);
        }
    }

    /// `mov [address], imm` for 32 bits, or sign-extended imm32 for 64 bits.
    pub fn mov_mi(&mut self, width: Width, address: &Address, imm: i32) {
        self.emit_instruction(
            Prefix::None,
            width == Width::W64,
            &[0xc7],
            0,
            Rm::Mem(*address),
            ByteRegisters::No,
            4,
        );
        self.buffer.emit_u32(imm as u32);
    }

    /// `mov byte [address], src8`.
    pub fn mov8_mr(&mut self, address: &Address, src: Gpr) {
        self.emit_instruction(
            Prefix::None,
            false,
            &[0x88],
            src.0,
            Rm::Mem(*address),
            ByteRegisters::Reg,
            0,
        );
    }

    /// `mov word [address], src16`.
    pub fn mov16_mr(&mut self, address: &Address, src: Gpr) {
        self.emit_instruction(
            Prefix::OperandSize,
            false,
            &[0x89],
            src.0,
            Rm::Mem(*address),
            ByteRegisters::No,
            0,
        );
    }

    /// `mov byte [address], imm8`.
    pub fn mov8_mi(&mut self, address: &Address, imm: u8) {
        self.emit_instruction(Prefix::None, false, &[0xc6], 0, Rm::Mem(*address), ByteRegisters::No, 1);
        self.buffer.emit_u8(imm);
    }

    /// `mov word [address], imm16`.
    pub fn mov16_mi(&mut self, address: &Address, imm: u16) {
        self.emit_instruction(
            Prefix::OperandSize,
            false,
            &[0xc7],
            0,
            Rm::Mem(*address),
            ByteRegisters::No,
            2,
        );
        self.buffer.emit_u16(imm);
    }

    /// `movzx r32, r8` (zero-extends into all 64 bits).
    pub fn movzx8_rr(&mut self, dst: Gpr, src: Gpr) {
        self.emit_instruction(
            Prefix::None,
            false,
            &[0x0f, 0xb6],
            dst.0,
            Rm::Reg(src.0),
            ByteRegisters::Rm,
            0,
        );
    }

    /// `movzx r32, byte [address]`.
    pub fn movzx8_rm(&mut self, dst: Gpr, address: &Address) {
        self.op_rm(Width::W32, &[0x0f, 0xb6], dst.0, address);
    }

    /// `movzx r32, r16`.
    pub fn movzx16_rr(&mut self, dst: Gpr, src: Gpr) {
        self.op_rr(Width::W32, &[0x0f, 0xb7], dst, src);
    }

    /// `movzx r32, word [address]`.
    pub fn movzx16_rm(&mut self, dst: Gpr, address: &Address) {
        self.op_rm(Width::W32, &[0x0f, 0xb7], dst.0, address);
    }

    /// `movsx r, r8`.
    pub fn movsx8_rr(&mut self, width: Width, dst: Gpr, src: Gpr) {
        self.emit_instruction(
            Prefix::None,
            width == Width::W64,
            &[0x0f, 0xbe],
            dst.0,
            Rm::Reg(src.0),
            ByteRegisters::Rm,
            0,
        );
    }

    /// `movsx r, byte [address]`.
    pub fn movsx8_rm(&mut self, width: Width, dst: Gpr, address: &Address) {
        self.op_rm(width, &[0x0f, 0xbe], dst.0, address);
    }

    /// `movsx r, r16`.
    pub fn movsx16_rr(&mut self, width: Width, dst: Gpr, src: Gpr) {
        self.op_rr(width, &[0x0f, 0xbf], dst, src);
    }

    /// `movsx r, word [address]`.
    pub fn movsx16_rm(&mut self, width: Width, dst: Gpr, address: &Address) {
        self.op_rm(width, &[0x0f, 0xbf], dst.0, address);
    }

    /// `movsxd r64, r32`.
    pub fn movsxd_rr(&mut self, dst: Gpr, src: Gpr) {
        self.op_rr(Width::W64, &[0x63], dst, src);
    }

    /// `movsxd r64, dword [address]`.
    pub fn movsxd_rm(&mut self, dst: Gpr, address: &Address) {
        self.op_rm(Width::W64, &[0x63], dst.0, address);
    }

    /// `lea dst, [address]`.
    pub fn lea(&mut self, width: Width, dst: Gpr, address: &Address) {
        self.op_rm(width, &[0x8d], dst.0, address);
    }

    /// `mov dst, qword [rip + label]`.
    pub fn mov_r_label(&mut self, dst: Gpr, label: Label) {
        self.emit_instruction(Prefix::None, true, &[0x8b], dst.0, Rm::Rip(label), ByteRegisters::No, 0);
    }

    /// `cmp lhs, qword [rip + label]`.
    pub fn cmp_r_label(&mut self, lhs: Gpr, label: Label) {
        self.emit_instruction(Prefix::None, true, &[0x3b], lhs.0, Rm::Rip(label), ByteRegisters::No, 0);
    }

    /// `op lhs, qword [rip + label]`.
    pub fn alu_r_label(&mut self, op: AluOp, lhs: Gpr, label: Label) {
        self.emit_instruction(
            Prefix::None,
            true,
            &[(op as u8) << 3 | 0x03],
            lhs.0,
            Rm::Rip(label),
            ByteRegisters::No,
            0,
        );
    }

    /// `lea dst, [rip + label]`.
    pub fn lea_label(&mut self, dst: Gpr, label: Label) {
        self.emit_instruction(Prefix::None, true, &[0x8d], dst.0, Rm::Rip(label), ByteRegisters::No, 0);
    }

    /// `xchg a, b`.
    pub fn xchg_rr(&mut self, width: Width, a: Gpr, b: Gpr) {
        self.op_rr(width, &[0x87], b, a);
    }

    // Integer arithmetic and logic.

    /// `op dst, src`.
    pub fn alu_rr(&mut self, op: AluOp, width: Width, dst: Gpr, src: Gpr) {
        self.op_rr(width, &[(op as u8) << 3 | 0x01], src, dst);
    }

    /// `op dst, imm` (imm sign-extended to 64 bits for 64-bit operations).
    pub fn alu_ri(&mut self, op: AluOp, width: Width, dst: Gpr, imm: i32) {
        self.alu_imm(op, width, Rm::Reg(dst.0), imm);
    }

    /// `op dst, [address]`.
    pub fn alu_rm(&mut self, op: AluOp, width: Width, dst: Gpr, address: &Address) {
        self.op_rm(width, &[(op as u8) << 3 | 0x03], dst.0, address);
    }

    /// `op [address], src`.
    pub fn alu_mr(&mut self, op: AluOp, width: Width, address: &Address, src: Gpr) {
        self.op_rm(width, &[(op as u8) << 3 | 0x01], src.0, address);
    }

    /// `op [address], imm`.
    pub fn alu_mi(&mut self, op: AluOp, width: Width, address: &Address, imm: i32) {
        self.alu_imm(op, width, Rm::Mem(*address), imm);
    }

    fn alu_imm(&mut self, op: AluOp, width: Width, rm: Rm, imm: i32) {
        let w = width == Width::W64;
        if let Ok(imm8) = i8::try_from(imm) {
            self.emit_instruction(Prefix::None, w, &[0x83], op as u8, rm, ByteRegisters::No, 1);
            self.buffer.emit_u8(imm8 as u8);
        } else {
            self.emit_instruction(Prefix::None, w, &[0x81], op as u8, rm, ByteRegisters::No, 4);
            self.buffer.emit_u32(imm as u32);
        }
    }

    pub fn add_rr(&mut self, width: Width, dst: Gpr, src: Gpr) {
        self.alu_rr(AluOp::Add, width, dst, src);
    }

    pub fn add_ri(&mut self, width: Width, dst: Gpr, imm: i32) {
        self.alu_ri(AluOp::Add, width, dst, imm);
    }

    pub fn sub_rr(&mut self, width: Width, dst: Gpr, src: Gpr) {
        self.alu_rr(AluOp::Sub, width, dst, src);
    }

    pub fn sub_ri(&mut self, width: Width, dst: Gpr, imm: i32) {
        self.alu_ri(AluOp::Sub, width, dst, imm);
    }

    pub fn and_rr(&mut self, width: Width, dst: Gpr, src: Gpr) {
        self.alu_rr(AluOp::And, width, dst, src);
    }

    pub fn and_ri(&mut self, width: Width, dst: Gpr, imm: i32) {
        self.alu_ri(AluOp::And, width, dst, imm);
    }

    pub fn or_rr(&mut self, width: Width, dst: Gpr, src: Gpr) {
        self.alu_rr(AluOp::Or, width, dst, src);
    }

    pub fn or_ri(&mut self, width: Width, dst: Gpr, imm: i32) {
        self.alu_ri(AluOp::Or, width, dst, imm);
    }

    pub fn xor_rr(&mut self, width: Width, dst: Gpr, src: Gpr) {
        self.alu_rr(AluOp::Xor, width, dst, src);
    }

    pub fn xor_ri(&mut self, width: Width, dst: Gpr, imm: i32) {
        self.alu_ri(AluOp::Xor, width, dst, imm);
    }

    pub fn cmp_rr(&mut self, width: Width, lhs: Gpr, rhs: Gpr) {
        self.alu_rr(AluOp::Cmp, width, lhs, rhs);
    }

    pub fn cmp_ri(&mut self, width: Width, lhs: Gpr, imm: i32) {
        self.alu_ri(AluOp::Cmp, width, lhs, imm);
    }

    /// `cmp [address], rhs`.
    pub fn cmp_mr(&mut self, width: Width, address: &Address, rhs: Gpr) {
        self.alu_mr(AluOp::Cmp, width, address, rhs);
    }

    /// `cmp [address], imm`.
    pub fn cmp_mi(&mut self, width: Width, address: &Address, imm: i32) {
        self.alu_mi(AluOp::Cmp, width, address, imm);
    }

    /// `cmp word ptr [address], imm8` (sign-extended to 16 bits). An imm8
    /// avoids the length-changing prefix stall of `cmp` with an imm16.
    pub fn cmp16_mi8(&mut self, address: &Address, imm: i8) {
        self.emit_instruction(
            Prefix::OperandSize,
            false,
            &[0x83],
            AluOp::Cmp as u8,
            Rm::Mem(*address),
            ByteRegisters::No,
            1,
        );
        self.buffer.emit_u8(imm as u8);
    }

    /// `test a, b`.
    pub fn test_rr(&mut self, width: Width, a: Gpr, b: Gpr) {
        self.op_rr(width, &[0x85], b, a);
    }

    /// `test reg, imm` (imm sign-extended for 64-bit operations).
    pub fn test_ri(&mut self, width: Width, reg: Gpr, imm: i32) {
        self.emit_instruction(
            Prefix::None,
            width == Width::W64,
            &[0xf7],
            0,
            Rm::Reg(reg.0),
            ByteRegisters::No,
            4,
        );
        self.buffer.emit_u32(imm as u32);
    }

    /// `test [address], imm`.
    pub fn test_mi(&mut self, width: Width, address: &Address, imm: i32) {
        self.emit_instruction(
            Prefix::None,
            width == Width::W64,
            &[0xf7],
            0,
            Rm::Mem(*address),
            ByteRegisters::No,
            4,
        );
        self.buffer.emit_u32(imm as u32);
    }

    /// `test byte [address], imm8`.
    pub fn test8_mi(&mut self, address: &Address, imm: u8) {
        self.emit_instruction(Prefix::None, false, &[0xf6], 0, Rm::Mem(*address), ByteRegisters::No, 1);
        self.buffer.emit_u8(imm);
    }

    pub fn neg(&mut self, width: Width, reg: Gpr) {
        self.emit_instruction(
            Prefix::None,
            width == Width::W64,
            &[0xf7],
            3,
            Rm::Reg(reg.0),
            ByteRegisters::No,
            0,
        );
    }

    pub fn not(&mut self, width: Width, reg: Gpr) {
        self.emit_instruction(
            Prefix::None,
            width == Width::W64,
            &[0xf7],
            2,
            Rm::Reg(reg.0),
            ByteRegisters::No,
            0,
        );
    }

    /// `op reg, imm8`. The amount is masked by the CPU to 5 (or 6) bits.
    pub fn shift_ri(&mut self, op: ShiftOp, width: Width, reg: Gpr, amount: u8) {
        let w = width == Width::W64;
        if amount == 1 {
            self.emit_instruction(Prefix::None, w, &[0xd1], op as u8, Rm::Reg(reg.0), ByteRegisters::No, 0);
        } else {
            self.emit_instruction(Prefix::None, w, &[0xc1], op as u8, Rm::Reg(reg.0), ByteRegisters::No, 1);
            self.buffer.emit_u8(amount);
        }
    }

    /// `op reg, cl`.
    pub fn shift_rcl(&mut self, op: ShiftOp, width: Width, reg: Gpr) {
        self.emit_instruction(
            Prefix::None,
            width == Width::W64,
            &[0xd3],
            op as u8,
            Rm::Reg(reg.0),
            ByteRegisters::No,
            0,
        );
    }

    pub fn shl_ri(&mut self, width: Width, reg: Gpr, amount: u8) {
        self.shift_ri(ShiftOp::Shl, width, reg, amount);
    }

    pub fn shr_ri(&mut self, width: Width, reg: Gpr, amount: u8) {
        self.shift_ri(ShiftOp::Shr, width, reg, amount);
    }

    pub fn sar_ri(&mut self, width: Width, reg: Gpr, amount: u8) {
        self.shift_ri(ShiftOp::Sar, width, reg, amount);
    }

    /// `imul dst, src`. Sets OF/CF when the signed result does not fit.
    pub fn imul_rr(&mut self, width: Width, dst: Gpr, src: Gpr) {
        self.op_rr(width, &[0x0f, 0xaf], dst, src);
    }

    /// `imul dst, src, imm`.
    pub fn imul_rri(&mut self, width: Width, dst: Gpr, src: Gpr, imm: i32) {
        let w = width == Width::W64;
        if let Ok(imm8) = i8::try_from(imm) {
            self.emit_instruction(Prefix::None, w, &[0x6b], dst.0, Rm::Reg(src.0), ByteRegisters::No, 1);
            self.buffer.emit_u8(imm8 as u8);
        } else {
            self.emit_instruction(Prefix::None, w, &[0x69], dst.0, Rm::Reg(src.0), ByteRegisters::No, 4);
            self.buffer.emit_u32(imm as u32);
        }
    }

    /// `cdq` (32-bit) or `cqo` (64-bit): sign-extend rax into rdx.
    pub fn sign_extend_rax_into_rdx(&mut self, width: Width) {
        if width == Width::W64 {
            self.buffer.emit_u8(0x48);
        }
        self.buffer.emit_u8(0x99);
    }

    /// `idiv divisor`: signed divide rdx:rax.
    pub fn idiv(&mut self, width: Width, divisor: Gpr) {
        self.emit_instruction(
            Prefix::None,
            width == Width::W64,
            &[0xf7],
            7,
            Rm::Reg(divisor.0),
            ByteRegisters::No,
            0,
        );
    }

    /// `setcc dst8`. Only writes the low byte.
    pub fn setcc(&mut self, cond: Cond, dst: Gpr) {
        self.emit_instruction(
            Prefix::None,
            false,
            &[0x0f, 0x90 | cond as u8],
            0,
            Rm::Reg(dst.0),
            ByteRegisters::Rm,
            0,
        );
    }

    /// `cmovcc dst, src`.
    pub fn cmovcc(&mut self, cond: Cond, width: Width, dst: Gpr, src: Gpr) {
        self.op_rr(width, &[0x0f, 0x40 | cond as u8], dst, src);
    }

    // Control flow.

    /// `jcc label`, rel8 when the label is bound and in range, rel32 otherwise.
    pub fn jcc(&mut self, cond: Cond, label: Label) {
        if self.backward_rel8_reaches(label, 2) {
            self.jcc_short(cond, label);
        } else {
            self.buffer.emit_bytes(&[0x0f, 0x80 | cond as u8]);
            self.emit_rel32(label);
        }
    }

    /// `jcc rel8 label`. Fails at bind or finish time if out of range.
    pub fn jcc_short(&mut self, cond: Cond, label: Label) {
        self.buffer.emit_u8(0x70 | cond as u8);
        self.emit_rel8(label);
    }

    /// `jmp label`, rel8 when the label is bound and in range, rel32 otherwise.
    pub fn jmp(&mut self, label: Label) {
        if self.backward_rel8_reaches(label, 2) {
            self.jmp_short(label);
        } else {
            self.buffer.emit_u8(0xe9);
            self.emit_rel32(label);
        }
    }

    /// `jmp rel8 label`. Fails at bind or finish time if out of range.
    pub fn jmp_short(&mut self, label: Label) {
        self.buffer.emit_u8(0xeb);
        self.emit_rel8(label);
    }

    /// `call label` (rel32).
    pub fn call(&mut self, label: Label) {
        self.buffer.emit_u8(0xe8);
        self.emit_rel32(label);
    }

    /// `call reg`.
    pub fn call_r(&mut self, target: Gpr) {
        self.emit_instruction(Prefix::None, false, &[0xff], 2, Rm::Reg(target.0), ByteRegisters::No, 0);
    }

    /// `call qword [address]`.
    pub fn call_m(&mut self, address: &Address) {
        self.emit_instruction(Prefix::None, false, &[0xff], 2, Rm::Mem(*address), ByteRegisters::No, 0);
    }

    /// `jmp reg`.
    pub fn jmp_r(&mut self, target: Gpr) {
        self.emit_instruction(Prefix::None, false, &[0xff], 4, Rm::Reg(target.0), ByteRegisters::No, 0);
    }

    /// `call qword [rip + label]`.
    pub fn call_label_indirect(&mut self, label: Label) {
        self.emit_instruction(Prefix::None, false, &[0xff], 2, Rm::Rip(label), ByteRegisters::No, 0);
    }

    /// `jmp qword [address]`.
    pub fn jmp_m(&mut self, address: &Address) {
        self.emit_instruction(Prefix::None, false, &[0xff], 4, Rm::Mem(*address), ByteRegisters::No, 0);
    }

    fn backward_rel8_reaches(&self, label: Label, instruction_size: usize) -> bool {
        self.buffer
            .label_offset(label)
            .is_some_and(|target| FixupKind::X86Rel8.reaches(self.offset() + instruction_size - 1, target))
    }

    fn emit_rel8(&mut self, label: Label) {
        let at = self.offset();
        self.buffer.emit_u8(0);
        self.buffer.add_fixup(label, at, FixupKind::X86Rel8);
    }

    fn emit_rel32(&mut self, label: Label) {
        let at = self.offset();
        self.buffer.emit_u32(0);
        self.buffer.add_fixup(label, at, FixupKind::X86Rel32);
    }

    pub fn push(&mut self, reg: Gpr) {
        self.emit_rex(false, 0, &Rm::Reg(reg.0), ByteRegisters::No);
        self.buffer.emit_u8(0x50 | (reg.0 & 7));
    }

    pub fn pop(&mut self, reg: Gpr) {
        self.emit_rex(false, 0, &Rm::Reg(reg.0), ByteRegisters::No);
        self.buffer.emit_u8(0x58 | (reg.0 & 7));
    }

    pub fn ret(&mut self) {
        self.buffer.emit_u8(0xc3);
    }

    pub fn nop(&mut self) {
        self.buffer.emit_u8(0x90);
    }

    /// Emits `size` bytes of NOPs using the recommended multi-byte forms.
    pub fn nops(&mut self, mut size: usize) {
        const NOPS: [&[u8]; 9] = [
            &[0x90],
            &[0x66, 0x90],
            &[0x0f, 0x1f, 0x00],
            &[0x0f, 0x1f, 0x40, 0x00],
            &[0x0f, 0x1f, 0x44, 0x00, 0x00],
            &[0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00],
            &[0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00],
            &[0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00],
            &[0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00],
        ];
        while size > 0 {
            let chunk = size.min(NOPS.len());
            self.buffer.emit_bytes(NOPS[chunk - 1]);
            size -= chunk;
        }
    }

    pub fn int3(&mut self) {
        self.buffer.emit_u8(0xcc);
    }

    pub fn ud2(&mut self) {
        self.buffer.emit_bytes(&[0x0f, 0x0b]);
    }

    // Scalar double SSE2.

    /// `movsd dst, src` (merges into the low lane; prefer `movapd_rr`).
    pub fn movsd_rr(&mut self, dst: Fpr, src: Fpr) {
        self.sse_rr(Prefix::Repne, false, &[0x0f, 0x10], dst.0, src.0);
    }

    /// `movapd dst, src`: full register copy without a merge dependency.
    pub fn movapd_rr(&mut self, dst: Fpr, src: Fpr) {
        self.sse_rr(Prefix::OperandSize, false, &[0x0f, 0x28], dst.0, src.0);
    }

    /// `movsd dst, qword [address]`.
    pub fn movsd_rm(&mut self, dst: Fpr, address: &Address) {
        self.emit_instruction(
            Prefix::Repne,
            false,
            &[0x0f, 0x10],
            dst.0,
            Rm::Mem(*address),
            ByteRegisters::No,
            0,
        );
    }

    /// `movsd dst, qword [rip + label]`.
    pub fn movsd_r_label(&mut self, dst: Fpr, label: Label) {
        self.emit_instruction(
            Prefix::Repne,
            false,
            &[0x0f, 0x10],
            dst.0,
            Rm::Rip(label),
            ByteRegisters::No,
            0,
        );
    }

    /// `movsd qword [address], src`.
    pub fn movsd_mr(&mut self, address: &Address, src: Fpr) {
        self.emit_instruction(
            Prefix::Repne,
            false,
            &[0x0f, 0x11],
            src.0,
            Rm::Mem(*address),
            ByteRegisters::No,
            0,
        );
    }

    /// `movups dst, xmmword [rip + label]`.
    pub fn movups_r_label(&mut self, dst: Fpr, label: Label) {
        self.emit_instruction(
            Prefix::None,
            false,
            &[0x0f, 0x10],
            dst.0,
            Rm::Rip(label),
            ByteRegisters::No,
            0,
        );
    }

    /// `movups xmmword [address], src`.
    pub fn movups_mr(&mut self, address: &Address, src: Fpr) {
        self.emit_instruction(
            Prefix::None,
            false,
            &[0x0f, 0x11],
            src.0,
            Rm::Mem(*address),
            ByteRegisters::No,
            0,
        );
    }

    /// `movq xmm, r64`.
    pub fn movq_xr(&mut self, dst: Fpr, src: Gpr) {
        self.sse_rr(Prefix::OperandSize, true, &[0x0f, 0x6e], dst.0, src.0);
    }

    /// `movq r64, xmm`.
    pub fn movq_rx(&mut self, dst: Gpr, src: Fpr) {
        self.sse_rr(Prefix::OperandSize, true, &[0x0f, 0x7e], src.0, dst.0);
    }

    pub fn addsd(&mut self, dst: Fpr, src: Fpr) {
        self.sse_rr(Prefix::Repne, false, &[0x0f, 0x58], dst.0, src.0);
    }

    pub fn mulsd(&mut self, dst: Fpr, src: Fpr) {
        self.sse_rr(Prefix::Repne, false, &[0x0f, 0x59], dst.0, src.0);
    }

    pub fn subsd(&mut self, dst: Fpr, src: Fpr) {
        self.sse_rr(Prefix::Repne, false, &[0x0f, 0x5c], dst.0, src.0);
    }

    pub fn divsd(&mut self, dst: Fpr, src: Fpr) {
        self.sse_rr(Prefix::Repne, false, &[0x0f, 0x5e], dst.0, src.0);
    }

    pub fn sqrtsd(&mut self, dst: Fpr, src: Fpr) {
        self.sse_rr(Prefix::Repne, false, &[0x0f, 0x51], dst.0, src.0);
    }

    /// `roundsd dst, src, mode` (SSE4.1): rounds to an integral double in
    /// `mode` (1 down, 2 up), with bit 3 suppressing the precision exception.
    pub fn roundsd(&mut self, dst: Fpr, src: Fpr, mode: u8) {
        self.emit_instruction(
            Prefix::OperandSize,
            false,
            &[0x0f, 0x3a, 0x0b],
            dst.0,
            Rm::Reg(src.0),
            ByteRegisters::No,
            1,
        );
        self.buffer.emit_u8(mode);
    }

    /// `ucomisd lhs, rhs`: ZF/PF/CF = unordered 111, less 001, equal 100, greater 000.
    pub fn ucomisd(&mut self, lhs: Fpr, rhs: Fpr) {
        self.sse_rr(Prefix::OperandSize, false, &[0x0f, 0x2e], lhs.0, rhs.0);
    }

    /// `cvtsi2sd dst, r32/r64`.
    pub fn cvtsi2sd(&mut self, width: Width, dst: Fpr, src: Gpr) {
        self.sse_rr(Prefix::Repne, width == Width::W64, &[0x0f, 0x2a], dst.0, src.0);
    }

    /// `cvttsd2si r32/r64, src`. Produces the "integer indefinite" value
    /// (`0x80000000` / `0x8000000000000000`) for NaN and out of range inputs.
    pub fn cvttsd2si(&mut self, width: Width, dst: Gpr, src: Fpr) {
        self.sse_rr(Prefix::Repne, width == Width::W64, &[0x0f, 0x2c], dst.0, src.0);
    }

    pub fn xorpd(&mut self, dst: Fpr, src: Fpr) {
        self.sse_rr(Prefix::OperandSize, false, &[0x0f, 0x57], dst.0, src.0);
    }

    pub fn andpd(&mut self, dst: Fpr, src: Fpr) {
        self.sse_rr(Prefix::OperandSize, false, &[0x0f, 0x54], dst.0, src.0);
    }

    /// `movss dst, dword [address]`.
    pub fn movss_rm(&mut self, dst: Fpr, address: &Address) {
        self.emit_instruction(
            Prefix::Rep,
            false,
            &[0x0f, 0x10],
            dst.0,
            Rm::Mem(*address),
            ByteRegisters::No,
            0,
        );
    }

    /// `movss dword [address], src`.
    pub fn movss_mr(&mut self, address: &Address, src: Fpr) {
        self.emit_instruction(
            Prefix::Rep,
            false,
            &[0x0f, 0x11],
            src.0,
            Rm::Mem(*address),
            ByteRegisters::No,
            0,
        );
    }

    /// `cvtss2sd dst, src`: widens a float to a double.
    pub fn cvtss2sd(&mut self, dst: Fpr, src: Fpr) {
        self.sse_rr(Prefix::Rep, false, &[0x0f, 0x5a], dst.0, src.0);
    }

    /// `cvtsd2ss dst, src`: rounds a double to a float.
    pub fn cvtsd2ss(&mut self, dst: Fpr, src: Fpr) {
        self.sse_rr(Prefix::Repne, false, &[0x0f, 0x5a], dst.0, src.0);
    }

    /// `movmskpd r32, xmm`: the sign bits of both lanes in bits 0 and 1.
    pub fn movmskpd(&mut self, dst: Gpr, src: Fpr) {
        self.sse_rr(Prefix::OperandSize, false, &[0x0f, 0x50], dst.0, src.0);
    }
}
