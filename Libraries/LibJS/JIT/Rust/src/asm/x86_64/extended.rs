/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Encoders of the instructions the WebAssembly JIT uses beyond what LibJS needs: VEX three-operand
//! scalar float arithmetic for f32 and f64, rounding and conversions, bitwise operations on xmm registers, BMI1 and
//! BMI2 (`andn`, `shlx`, `sarx`, `shrx`, `rorx`), bit counting (`lzcnt`, `tzcnt`, `popcnt`) and unsigned division,
//! and `vex()`, which encodes any VEX instruction from its opcode, for the vector instructions.
//!
//! VEX instructions with a scalar operand ignore VEX.L (they are LIG); this file encodes them with L = 0, like the
//! 128-bit forms.
//! With `Assembler::for_host`, SIMD, rotations and bit counts select SSE and bit-scan fallbacks as needed.

use super::super::Address;
use super::super::Fpr;
use super::super::Gpr;
use super::super::Width;
use super::super::buffer::Label;
use super::Assembler;
use super::ByteRegisters;
use super::Prefix;
use super::Rm;

/// The implied mandatory prefix of a VEX instruction (VEX.pp).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VexPrefix {
    None = 0,
    OperandSize = 1,
    Rep = 2,
    Repne = 3,
}

/// The opcode map of a VEX instruction (VEX.m-mmmm).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VexMap {
    Map0F = 1,
    Map0F38 = 2,
    Map0F3A = 3,
}

/// A VEX instruction, as the manuals write it: `VEX.128.66.0F38.W0 00` is `VexOpcode::new(OperandSize, Map0F38,
/// 0x00)`. Instructions that ignore VEX.W are encoded with W0, and VEX.L is always 0 (128 bits, or scalar).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct VexOpcode {
    pub prefix: VexPrefix,
    pub map: VexMap,
    pub opcode: u8,
    pub w: bool,
}

impl VexOpcode {
    pub const fn new(prefix: VexPrefix, map: VexMap, opcode: u8) -> Self {
        Self {
            prefix,
            map,
            opcode,
            w: false,
        }
    }

    /// The same instruction with VEX.W1, like the 64-bit forms of `vpinsrq` and `vmovq`.
    pub const fn w1(self) -> Self {
        Self { w: true, ..self }
    }
}

/// The r/m operand of `Assembler::vex()`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VexOperand {
    /// A register by its encoding: an xmm register, or a general purpose one for the instructions that take one.
    Register(u8),
    Memory(Address),
    /// `[rip + label]`.
    Label(Label),
}

/// A scalar float operation of SSE's arithmetic row, by its opcode.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FloatOp {
    Sqrt = 0x51,
    Add = 0x58,
    Mul = 0x59,
    Sub = 0x5c,
    Min = 0x5d,
    Div = 0x5e,
    Max = 0x5f,
}

/// A bitwise operation on whole xmm registers, by its opcode.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BitwiseOp {
    And = 0x54,
    AndNot = 0x55,
    Or = 0x56,
    Xor = 0x57,
}

/// The precision of a scalar float instruction.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Precision {
    Single,
    Double,
}

impl Precision {
    fn scalar_prefix(self) -> VexPrefix {
        match self {
            Precision::Single => VexPrefix::Rep,
            Precision::Double => VexPrefix::Repne,
        }
    }
}

/// The rounding of `vroundss`/`vroundsd`, with the precision exception suppressed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Rounding {
    Nearest = 0x8,
    Down = 0x9,
    Up = 0xa,
    TowardZero = 0xb,
}

/// The bit counting instructions, by their second opcode byte.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BitCount {
    Popcnt = 0xb8,
    Tzcnt = 0xbc,
    Lzcnt = 0xbd,
}

/// The BMI2 shifts, by their implied prefix.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShiftX {
    Shl,
    Sar,
    Shr,
}

impl Assembler {
    /// Emits a VEX prefix and opcode with ModRM for `reg`, the extra source `vvvv` and `rm`, using the two-byte form
    /// where it can.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn vex_instruction(
        &mut self,
        prefix: VexPrefix,
        map: VexMap,
        w: bool,
        opcode: u8,
        reg: u8,
        vvvv: u8,
        rm: Rm,
        trailing_immediate_size: usize,
    ) {
        let (x, b) = match rm {
            Rm::Reg(register) => (false, register & 8 != 0),
            Rm::Mem(address) => (
                address.index.is_some_and(|(index, _)| index.0 & 8 != 0),
                address.base.0 & 8 != 0,
            ),
            Rm::Rip(_) => (false, false),
        };
        let r = reg & 8 != 0;
        let low = (u8::from(!r) << 7) | ((!vvvv & 0xf) << 3) | prefix as u8;
        if !x && !b && !w && map == VexMap::Map0F {
            self.buffer.emit_u8(0xc5);
            self.buffer.emit_u8(low);
        } else {
            self.buffer.emit_u8(0xc4);
            self.buffer
                .emit_u8((u8::from(!r) << 7) | (u8::from(!x) << 6) | (u8::from(!b) << 5) | map as u8);
            self.buffer.emit_u8((u8::from(w) << 7) | (low & 0x7f));
        }
        self.buffer.emit_u8(opcode);
        self.emit_modrm_rm(reg, &rm, trailing_immediate_size);
    }

    /// Emits the VEX instruction `opcode` with `reg` in ModRM.reg, `vvvv` as the extra source (0 where the
    /// instruction has none), `rm` and an immediate byte. Registers are by their encoding, whatever their class.
    /// With host instruction selection, the Wasm SIMD subset uses equivalent SSE sequences without AVX2.
    pub fn vex(&mut self, opcode: VexOpcode, reg: u8, vvvv: u8, rm: VexOperand, immediate: Option<u8>) {
        if !self.cpu_features().avx2 {
            self.legacy_vector(opcode, reg, vvvv, rm, immediate);
            return;
        }
        let rm = match rm {
            VexOperand::Register(register) => Rm::Reg(register),
            VexOperand::Memory(address) => Rm::Mem(address),
            VexOperand::Label(label) => Rm::Rip(label),
        };
        self.vex_instruction(
            opcode.prefix,
            opcode.map,
            opcode.w,
            opcode.opcode,
            reg,
            vvvv,
            rm,
            usize::from(immediate.is_some()),
        );
        if let Some(immediate) = immediate {
            self.buffer.emit_u8(immediate);
        }
    }

    /// `op dst, lhs, rhs` on xmm registers.
    pub fn vex_rrr(&mut self, opcode: VexOpcode, dst: Fpr, lhs: Fpr, rhs: Fpr) {
        self.vex(opcode, dst.0, lhs.0, VexOperand::Register(rhs.0), None);
    }

    // Scalar float arithmetic.

    /// `vaddss`/`vsubss`/... `dst, lhs, rhs` (and the `sd` forms). The bits above the scalar come from `lhs`.
    pub fn vfloat_rrr(&mut self, op: FloatOp, precision: Precision, dst: Fpr, lhs: Fpr, rhs: Fpr) {
        self.simd_instruction(
            precision.scalar_prefix(),
            VexMap::Map0F,
            false,
            op as u8,
            dst.0,
            lhs.0,
            Rm::Reg(rhs.0),
            0,
        );
    }

    /// `vaddss`/`vsubss`/... `dst, lhs, [address]` (and the `sd` forms).
    pub fn vfloat_rrm(&mut self, op: FloatOp, precision: Precision, dst: Fpr, lhs: Fpr, address: &Address) {
        self.simd_instruction(
            precision.scalar_prefix(),
            VexMap::Map0F,
            false,
            op as u8,
            dst.0,
            lhs.0,
            Rm::Mem(*address),
            0,
        );
    }

    /// `imul dst, [address]`.
    pub fn imul_rm(&mut self, width: Width, dst: Gpr, address: &Address) {
        self.op_rm(width, &[0x0f, 0xaf], dst.0, address);
    }

    /// `vroundss`/`vroundsd dst, src, src, mode`.
    pub fn vround(&mut self, precision: Precision, dst: Fpr, src: Fpr, rounding: Rounding) {
        let opcode = match precision {
            Precision::Single => 0x0a,
            Precision::Double => 0x0b,
        };
        self.simd_instruction(
            VexPrefix::OperandSize,
            VexMap::Map0F3A,
            false,
            opcode,
            dst.0,
            src.0,
            Rm::Reg(src.0),
            1,
        );
        self.buffer.emit_u8(rounding as u8);
    }

    /// `vandps`/`vandnps`/`vorps`/`vxorps dst, lhs, rhs` on all 128 bits. `AndNot` computes `!lhs & rhs`.
    pub fn vbitwise_rrr(&mut self, op: BitwiseOp, dst: Fpr, lhs: Fpr, rhs: Fpr) {
        self.simd_instruction(
            VexPrefix::None,
            VexMap::Map0F,
            false,
            op as u8,
            dst.0,
            lhs.0,
            Rm::Reg(rhs.0),
            0,
        );
    }

    /// `vandps`/... `dst, lhs, xmmword [rip + label]`.
    pub fn vbitwise_r_label(&mut self, op: BitwiseOp, dst: Fpr, lhs: Fpr, label: Label) {
        self.simd_instruction(
            VexPrefix::None,
            VexMap::Map0F,
            false,
            op as u8,
            dst.0,
            lhs.0,
            Rm::Rip(label),
            0,
        );
    }

    /// `vmovaps dst, src`.
    pub fn vmovaps_rr(&mut self, dst: Fpr, src: Fpr) {
        self.simd_instruction(VexPrefix::None, VexMap::Map0F, false, 0x28, dst.0, 0, Rm::Reg(src.0), 0);
    }

    /// `vucomiss`/`vucomisd lhs, rhs`: ZF/PF/CF = unordered 111, less 001, equal 100, greater 000.
    pub fn vucomis(&mut self, precision: Precision, lhs: Fpr, rhs: Fpr) {
        let prefix = match precision {
            Precision::Single => VexPrefix::None,
            Precision::Double => VexPrefix::OperandSize,
        };
        self.simd_instruction(prefix, VexMap::Map0F, false, 0x2e, lhs.0, 0, Rm::Reg(rhs.0), 0);
    }

    // Loads and stores.

    /// `vmovss`/`vmovsd dst, [address]`, zeroing the bits above the scalar.
    pub fn vmovs_rm(&mut self, precision: Precision, dst: Fpr, address: &Address) {
        self.simd_instruction(
            precision.scalar_prefix(),
            VexMap::Map0F,
            false,
            0x10,
            dst.0,
            0,
            Rm::Mem(*address),
            0,
        );
    }

    /// `vmovss`/`vmovsd [address], src`.
    pub fn vmovs_mr(&mut self, precision: Precision, address: &Address, src: Fpr) {
        self.simd_instruction(
            precision.scalar_prefix(),
            VexMap::Map0F,
            false,
            0x11,
            src.0,
            0,
            Rm::Mem(*address),
            0,
        );
    }

    /// `vmovss`/`vmovsd dst, [rip + label]`.
    pub fn vmovs_r_label(&mut self, precision: Precision, dst: Fpr, label: Label) {
        self.simd_instruction(
            precision.scalar_prefix(),
            VexMap::Map0F,
            false,
            0x10,
            dst.0,
            0,
            Rm::Rip(label),
            0,
        );
    }

    // Moves between register classes.

    /// `vmovd xmm, r32` or `vmovq xmm, r64`, zeroing the rest of the register.
    pub fn vmov_xr(&mut self, width: Width, dst: Fpr, src: Gpr) {
        self.simd_instruction(
            VexPrefix::OperandSize,
            VexMap::Map0F,
            width == Width::W64,
            0x6e,
            dst.0,
            0,
            Rm::Reg(src.0),
            0,
        );
    }

    /// `vmovd r32, xmm` or `vmovq r64, xmm`.
    pub fn vmov_rx(&mut self, width: Width, dst: Gpr, src: Fpr) {
        self.simd_instruction(
            VexPrefix::OperandSize,
            VexMap::Map0F,
            width == Width::W64,
            0x7e,
            src.0,
            0,
            Rm::Reg(dst.0),
            0,
        );
    }

    // Conversions.

    /// `vcvtsi2ss`/`vcvtsi2sd dst, upper, r32/r64`: the signed integer as a float, with the bits above the scalar
    /// from `upper`.
    pub fn vcvtsi2s(&mut self, precision: Precision, width: Width, dst: Fpr, upper: Fpr, src: Gpr) {
        self.simd_instruction(
            precision.scalar_prefix(),
            VexMap::Map0F,
            width == Width::W64,
            0x2a,
            dst.0,
            upper.0,
            Rm::Reg(src.0),
            0,
        );
    }

    /// `vcvttss2si`/`vcvttsd2si r32/r64, src`: truncates toward zero. Produces the "integer indefinite" value (only
    /// the sign bit set) for NaN and out of range inputs.
    pub fn vcvtts2si(&mut self, precision: Precision, width: Width, dst: Gpr, src: Fpr) {
        self.simd_instruction(
            precision.scalar_prefix(),
            VexMap::Map0F,
            width == Width::W64,
            0x2c,
            dst.0,
            0,
            Rm::Reg(src.0),
            0,
        );
    }

    /// `vcvtss2sd dst, upper, src`: widens a float to a double.
    pub fn vcvtss2sd(&mut self, dst: Fpr, upper: Fpr, src: Fpr) {
        self.simd_instruction(
            VexPrefix::Rep,
            VexMap::Map0F,
            false,
            0x5a,
            dst.0,
            upper.0,
            Rm::Reg(src.0),
            0,
        );
    }

    /// `vcvtsd2ss dst, upper, src`: rounds a double to a float.
    pub fn vcvtsd2ss(&mut self, dst: Fpr, upper: Fpr, src: Fpr) {
        self.simd_instruction(
            VexPrefix::Repne,
            VexMap::Map0F,
            false,
            0x5a,
            dst.0,
            upper.0,
            Rm::Reg(src.0),
            0,
        );
    }

    // Integer instructions.

    /// `lzcnt`/`tzcnt`/`popcnt dst, src`.
    pub fn bit_count(&mut self, op: BitCount, width: Width, dst: Gpr, src: Gpr) {
        let features = self.cpu_features();
        if (op == BitCount::Lzcnt && !features.lzcnt) || (op == BitCount::Tzcnt && !features.bmi1) {
            self.count_zeros(op, width, dst, src);
            return;
        }
        self.emit_instruction(
            Prefix::Rep,
            width == Width::W64,
            &[0x0f, op as u8],
            dst.0,
            Rm::Reg(src.0),
            ByteRegisters::No,
            0,
        );
    }

    /// `div divisor`: unsigned divide rdx:rax.
    pub fn div(&mut self, width: Width, divisor: Gpr) {
        self.emit_instruction(
            Prefix::None,
            width == Width::W64,
            &[0xf7],
            6,
            Rm::Reg(divisor.0),
            ByteRegisters::No,
            0,
        );
    }

    /// `shlx`/`sarx`/`shrx dst, src, amount`. The amount is masked to 5 (or 6) bits; the flags are left alone.
    pub fn shiftx(&mut self, op: ShiftX, width: Width, dst: Gpr, src: Gpr, amount: Gpr) {
        let prefix = match op {
            ShiftX::Shl => VexPrefix::OperandSize,
            ShiftX::Sar => VexPrefix::Rep,
            ShiftX::Shr => VexPrefix::Repne,
        };
        self.vex_instruction(
            prefix,
            VexMap::Map0F38,
            width == Width::W64,
            0xf7,
            dst.0,
            amount.0,
            Rm::Reg(src.0),
            0,
        );
    }

    /// `rorx dst, src, amount`: rotates right by an immediate. Without BMI2, uses MOV and ROR, clobbering flags.
    pub fn rorx(&mut self, width: Width, dst: Gpr, src: Gpr, amount: u8) {
        if !self.cpu_features().bmi2 {
            self.mov_rr(width, dst, src);
            self.shift_ri(super::ShiftOp::Ror, width, dst, amount);
            return;
        }
        self.vex_instruction(
            VexPrefix::Repne,
            VexMap::Map0F3A,
            width == Width::W64,
            0xf0,
            dst.0,
            0,
            Rm::Reg(src.0),
            1,
        );
        self.buffer.emit_u8(amount);
    }

    /// `andn dst, inverted, src`: `!inverted & src`.
    pub fn andn(&mut self, width: Width, dst: Gpr, inverted: Gpr, src: Gpr) {
        self.vex_instruction(
            VexPrefix::None,
            VexMap::Map0F38,
            width == Width::W64,
            0xf2,
            dst.0,
            inverted.0,
            Rm::Reg(src.0),
            0,
        );
    }

    // Atomics.

    /// An instruction with a memory operand of `size` bytes (1, 2, 4 or 8), whose byte form has `byte_opcode` and
    /// whose other forms have `opcode`.
    fn sized_mr(&mut self, size: u32, byte_opcode: &[u8], opcode: &[u8], address: &Address, reg: Gpr) {
        let (prefix, w, opcode, byte_registers) = match size {
            1 => (Prefix::None, false, byte_opcode, ByteRegisters::Reg),
            2 => (Prefix::OperandSize, false, opcode, ByteRegisters::No),
            4 => (Prefix::None, false, opcode, ByteRegisters::No),
            _ => (Prefix::None, true, opcode, ByteRegisters::No),
        };
        self.emit_instruction(prefix, w, opcode, reg.0, Rm::Mem(*address), byte_registers, 0);
    }

    /// `lock xadd [address], reg` of `size` bytes: adds reg to memory and puts the old value in reg.
    pub fn lock_xadd(&mut self, size: u32, address: &Address, reg: Gpr) {
        self.buffer.emit_u8(0xf0);
        self.sized_mr(size, &[0x0f, 0xc0], &[0x0f, 0xc1], address, reg);
    }

    /// `lock cmpxchg [address], reg` of `size` bytes: stores reg if memory holds what rax does, else loads it into rax.
    pub fn lock_cmpxchg(&mut self, size: u32, address: &Address, reg: Gpr) {
        self.buffer.emit_u8(0xf0);
        self.sized_mr(size, &[0x0f, 0xb0], &[0x0f, 0xb1], address, reg);
    }

    /// `xchg [address], reg` of `size` bytes, which is locked without a prefix.
    pub fn xchg_mr(&mut self, size: u32, address: &Address, reg: Gpr) {
        self.sized_mr(size, &[0x86], &[0x87], address, reg);
    }

    /// `mfence`.
    pub fn mfence(&mut self) {
        self.buffer.emit_bytes(&[0x0f, 0xae, 0xf0]);
    }
}

#[cfg(test)]
mod tests {
    use super::super::super::Scale;
    use super::super::super::Width::W32;
    use super::super::super::Width::W64;
    use super::super::super::disassembler;
    use super::super::super::disassembler::assert_encodings;
    use super::super::*;
    use super::*;

    #[test]
    fn scalar_float_arithmetic() {
        assert_encodings!(Assembler, disassembler::x86_64, {
            "vaddss xmm0, xmm1, xmm2" => |a| a.vfloat_rrr(FloatOp::Add, Precision::Single, XMM0, XMM1, XMM2);
            "vaddsd xmm8, xmm9, xmm10" => |a| a.vfloat_rrr(FloatOp::Add, Precision::Double, XMM8, XMM9, XMM10);
            "vsubss xmm15, xmm0, xmm14" => |a| a.vfloat_rrr(FloatOp::Sub, Precision::Single, XMM15, XMM0, XMM14);
            "vmulsd xmm1, xmm12, xmm3" => |a| a.vfloat_rrr(FloatOp::Mul, Precision::Double, XMM1, XMM12, XMM3);
            "vdivss xmm1, xmm2, xmm3" => |a| a.vfloat_rrr(FloatOp::Div, Precision::Single, XMM1, XMM2, XMM3);
            "vminsd xmm1, xmm2, xmm3" => |a| a.vfloat_rrr(FloatOp::Min, Precision::Double, XMM1, XMM2, XMM3);
            "vmaxss xmm1, xmm2, xmm3" => |a| a.vfloat_rrr(FloatOp::Max, Precision::Single, XMM1, XMM2, XMM3);
            "vsqrtsd xmm1, xmm2, xmm11" => |a| a.vfloat_rrr(FloatOp::Sqrt, Precision::Double, XMM1, XMM2, XMM11);
            "vroundss xmm1, xmm1, xmm1, 8" => |a| a.vround(Precision::Single, XMM1, XMM1, Rounding::Nearest);
            "vroundsd xmm9, xmm2, xmm2, 0xb" => |a| a.vround(Precision::Double, XMM9, XMM2, Rounding::TowardZero);
            "vaddsd xmm1, xmm2, qword ptr [rbp-0x48]" => |a| a.vfloat_rrm(
                FloatOp::Add,
                Precision::Double,
                XMM1,
                XMM2,
                &Address::new(RBP, -0x48)
            );
            "vmulss xmm9, xmm10, dword ptr [r13+8]" => |a| a.vfloat_rrm(
                FloatOp::Mul,
                Precision::Single,
                XMM9,
                XMM10,
                &Address::new(R13, 8)
            );
            "imul eax, dword ptr [rbp-0x10]" => |a| a.imul_rm(W32, RAX, &Address::new(RBP, -0x10));
            "imul r9, qword ptr [rbp-0x18]" => |a| a.imul_rm(W64, R9, &Address::new(RBP, -0x18));
            "vucomiss xmm1, xmm2" => |a| a.vucomis(Precision::Single, XMM1, XMM2);
            "vucomisd xmm9, xmm12" => |a| a.vucomis(Precision::Double, XMM9, XMM12);
        });
    }

    #[test]
    fn bitwise_and_moves() {
        assert_encodings!(Assembler, disassembler::x86_64, {
            "vandps xmm1, xmm2, xmm3" => |a| a.vbitwise_rrr(BitwiseOp::And, XMM1, XMM2, XMM3);
            "vandnps xmm1, xmm2, xmm13" => |a| a.vbitwise_rrr(BitwiseOp::AndNot, XMM1, XMM2, XMM13);
            "vorps xmm10, xmm2, xmm3" => |a| a.vbitwise_rrr(BitwiseOp::Or, XMM10, XMM2, XMM3);
            "vxorps xmm0, xmm0, xmm0" => |a| a.vbitwise_rrr(BitwiseOp::Xor, XMM0, XMM0, XMM0);
            "vmovaps xmm1, xmm9" => |a| a.vmovaps_rr(XMM1, XMM9);
            "vmovd xmm1, eax" => |a| a.vmov_xr(W32, XMM1, RAX);
            "vmovq xmm9, r10" => |a| a.vmov_xr(W64, XMM9, R10);
            "vmovd r11d, xmm1" => |a| a.vmov_rx(W32, R11, XMM1);
            "vmovq rax, xmm15" => |a| a.vmov_rx(W64, RAX, XMM15);
            "vmovss xmm1, dword ptr [rbp-0x48]" => |a| a.vmovs_rm(Precision::Single, XMM1, &Address::new(RBP, -0x48));
            "vmovsd xmm9, qword ptr [r15+rax+0x10]" => |a| a.vmovs_rm(
                Precision::Double,
                XMM9,
                &Address::indexed(R15, RAX, Scale::One, 0x10)
            );
            "vmovsd qword ptr [r15+r12], xmm2" => |a| a.vmovs_mr(
                Precision::Double,
                &Address::indexed(R15, R12, Scale::One, 0),
                XMM2
            );
            "vmovss dword ptr [rsp+8], xmm14" => |a| a.vmovs_mr(Precision::Single, &Address::new(RSP, 8), XMM14);
        });
    }

    #[test]
    fn rip_relative_operands() {
        let mut assembler = Assembler::new();
        let label = assembler.new_label();
        assembler.vbitwise_r_label(BitwiseOp::And, XMM1, XMM2, label);
        assembler.vmovs_r_label(Precision::Double, XMM3, label);
        assembler.bind(label);
        let code = assembler.finish().unwrap();
        assert_eq!(
            disassembler::x86_64(&code),
            ["vandps xmm1, xmm2, xmmword ptr [0x10]", "vmovsd xmm3, qword ptr [0x10]"]
        );
    }

    #[test]
    fn conversions() {
        assert_encodings!(Assembler, disassembler::x86_64, {
            "vcvtsi2ss xmm1, xmm1, eax" => |a| a.vcvtsi2s(Precision::Single, W32, XMM1, XMM1, RAX);
            "vcvtsi2sd xmm9, xmm2, r12" => |a| a.vcvtsi2s(Precision::Double, W64, XMM9, XMM2, R12);
            "vcvttss2si eax, xmm1" => |a| a.vcvtts2si(Precision::Single, W32, RAX, XMM1);
            "vcvttsd2si r9, xmm10" => |a| a.vcvtts2si(Precision::Double, W64, R9, XMM10);
            "vcvtss2sd xmm1, xmm2, xmm3" => |a| a.vcvtss2sd(XMM1, XMM2, XMM3);
            "vcvtsd2ss xmm11, xmm11, xmm12" => |a| a.vcvtsd2ss(XMM11, XMM11, XMM12);
        });
    }

    #[test]
    fn any_vex_instruction() {
        use VexMap::{Map0F, Map0F3A, Map0F38};
        use VexPrefix::{OperandSize, Rep};
        let vpaddd = VexOpcode::new(OperandSize, Map0F, 0xfe);
        let vpshufb = VexOpcode::new(OperandSize, Map0F38, 0x00);
        let vpinsrq = VexOpcode::new(OperandSize, Map0F3A, 0x22).w1();
        let vpextrd = VexOpcode::new(OperandSize, Map0F3A, 0x16);
        let vmovdqu_load = VexOpcode::new(Rep, Map0F, 0x6f);
        let vpbroadcastb = VexOpcode::new(OperandSize, Map0F38, 0x78);
        assert_encodings!(Assembler, disassembler::x86_64, {
            "vpaddd xmm1, xmm2, xmm3" => |a| a.vex_rrr(vpaddd, XMM1, XMM2, XMM3);
            "vpaddd xmm9, xmm10, xmm11" => |a| a.vex_rrr(vpaddd, XMM9, XMM10, XMM11);
            "vpshufb xmm1, xmm2, xmm13" => |a| a.vex_rrr(vpshufb, XMM1, XMM2, XMM13);
            "vpinsrq xmm1, xmm2, r9, 1" => |a| a.vex(vpinsrq, 1, 2, VexOperand::Register(R9.0), Some(1));
            "vpextrd r10d, xmm3, 2" => |a| a.vex(vpextrd, 3, 0, VexOperand::Register(R10.0), Some(2));
            "vmovdqu xmm12, xmmword ptr [r15+rax+0x10]" => |a| a.vex(
                vmovdqu_load,
                12,
                0,
                VexOperand::Memory(Address::indexed(R15, RAX, Scale::One, 0x10)),
                None
            );
            "vpbroadcastb xmm0, xmm7" => |a| a.vex(vpbroadcastb, 0, 0, VexOperand::Register(7), None);
        });
    }

    #[test]
    fn integer_instructions() {
        assert_encodings!(Assembler, disassembler::x86_64, {
            "lzcnt eax, ecx" => |a| a.bit_count(BitCount::Lzcnt, W32, RAX, RCX);
            "tzcnt r8, r9" => |a| a.bit_count(BitCount::Tzcnt, W64, R8, R9);
            "popcnt rax, r15" => |a| a.bit_count(BitCount::Popcnt, W64, RAX, R15);
            "div ecx" => |a| a.div(W32, RCX);
            "div r10" => |a| a.div(W64, R10);
            "shlx eax, ecx, edx" => |a| a.shiftx(ShiftX::Shl, W32, RAX, RCX, RDX);
            "sarx r8, r9, r10" => |a| a.shiftx(ShiftX::Sar, W64, R8, R9, R10);
            "shrx rax, r12, rcx" => |a| a.shiftx(ShiftX::Shr, W64, RAX, R12, RCX);
            "rorx eax, ecx, 7" => |a| a.rorx(W32, RAX, RCX, 7);
            "rorx r13, rbx, 0x3f" => |a| a.rorx(W64, R13, RBX, 63);
            "andn eax, ecx, edx" => |a| a.andn(W32, RAX, RCX, RDX);
            "andn r9, r10, r11" => |a| a.andn(W64, R9, R10, R11);
        });
    }

    #[test]
    fn atomic_instructions() {
        assert_encodings!(Assembler, disassembler::x86_64, {
            "lock xadd byte ptr [rdi], sil" => |a| a.lock_xadd(1, &Address::new(RDI, 0), RSI);
            "lock xadd word ptr [r8+0x10], ax" => |a| a.lock_xadd(2, &Address::new(R8, 0x10), RAX);
            "lock xadd dword ptr [rax], r9d" => |a| a.lock_xadd(4, &Address::new(RAX, 0), R9);
            "lock xadd qword ptr [r13], rcx" => |a| a.lock_xadd(8, &Address::new(R13, 0), RCX);
            "lock cmpxchg byte ptr [rcx], dl" => |a| a.lock_cmpxchg(1, &Address::new(RCX, 0), RDX);
            "lock cmpxchg word ptr [rcx], r10w" => |a| a.lock_cmpxchg(2, &Address::new(RCX, 0), R10);
            "lock cmpxchg dword ptr [rcx], edx" => |a| a.lock_cmpxchg(4, &Address::new(RCX, 0), RDX);
            "lock cmpxchg qword ptr [r12], rdx" => |a| a.lock_cmpxchg(8, &Address::new(R12, 0), RDX);
            "xchg byte ptr [rax], dil" => |a| a.xchg_mr(1, &Address::new(RAX, 0), RDI);
            "xchg word ptr [rax], cx" => |a| a.xchg_mr(2, &Address::new(RAX, 0), RCX);
            "xchg dword ptr [rax], ecx" => |a| a.xchg_mr(4, &Address::new(RAX, 0), RCX);
            "xchg qword ptr [rax], r15" => |a| a.xchg_mr(8, &Address::new(RAX, 0), R15);
            "mfence" => |a| a.mfence();
        });
    }
}
