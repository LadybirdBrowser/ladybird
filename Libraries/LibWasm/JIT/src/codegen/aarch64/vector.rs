/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Vectors: v128 loads, lanes and SIMD arithmetic, as Advanced SIMD instructions.
//!
//! The allocator gives a vector node an output register apart from its inputs, so a lowering may write its output
//! before it has read all inputs; v30 and v31 are the scratch registers. Inputs may share a register (`i32x4.add x
//! x`), and are never written. Where the interpreter's instruction sequence decides bits the spec leaves open (NaN
//! payloads of min and max, relaxed instructions), the lowering is the same sequence, with each of its operations
//! expanded as the interpreter's AArch64 code expands it, so that both tiers agree bit for bit.

use super::{CodeGenerator, SCRATCH, SCRATCH_FPR, SCRATCH_FPR2, fpr, fpr_access, gpr};
use crate::asm::aarch64::{MemOperand, VectorArrangement};
use crate::asm::{Fpr, Gpr, Width};
use crate::convention::Register;
use crate::ir::{Func, Op, Type, Value};
use crate::regalloc::CONSTANT_INPUT;
use libwasm_module::opcode::simd::*;

/// The extra vector registers the lowering of a vector node takes: none, the scratch registers suffice.
pub(crate) fn vector_temps(_func: &Func, _value: Value) -> usize {
    0
}

/// Whether a binary SIMD operation is a shift by an i32, whose count may be a constant.
pub(crate) fn is_vector_shift(sub_opcode: u32) -> bool {
    matches!(
        sub_opcode,
        I8X16_SHL
            | I8X16_SHR_S
            | I8X16_SHR_U
            | I16X8_SHL
            | I16X8_SHR_S
            | I16X8_SHR_U
            | I32X4_SHL
            | I32X4_SHR_S
            | I32X4_SHR_U
            | I64X2_SHL
            | I64X2_SHR_S
            | I64X2_SHR_U
    )
}

const V31: Fpr = SCRATCH_FPR;
const V30: Fpr = SCRATCH_FPR2;

// The opcodes of the "three same" instructions, with their U bit.
const ADD: (bool, u32) = (false, 0b10000);
const SUB: (bool, u32) = (true, 0b10000);
const CMEQ: (bool, u32) = (true, 0b10001);
const CMGT: (bool, u32) = (false, 0b00110);
const CMHI: (bool, u32) = (true, 0b00110);
const CMGE: (bool, u32) = (false, 0b00111);
const CMHS: (bool, u32) = (true, 0b00111);
const SQADD: (bool, u32) = (false, 0b00001);
const UQADD: (bool, u32) = (true, 0b00001);
const SQSUB: (bool, u32) = (false, 0b00101);
const UQSUB: (bool, u32) = (true, 0b00101);
const URHADD: (bool, u32) = (true, 0b00010);
const MUL: (bool, u32) = (false, 0b10011);
const SMAX: (bool, u32) = (false, 0b01100);
const UMAX: (bool, u32) = (true, 0b01100);
const SMIN: (bool, u32) = (false, 0b01101);
const UMIN: (bool, u32) = (true, 0b01101);
const SSHL: (bool, u32) = (false, 0b01000);
const USHL: (bool, u32) = (true, 0b01000);
const ADDP: (bool, u32) = (false, 0b10111);
const SQRDMULH: (bool, u32) = (true, 0b10110);

/// The bitwise "three same" instructions: U, size and the opcode 00011.
#[derive(Clone, Copy)]
enum Logic {
    And,
    Bic,
    Orr,
    Eor,
    Bsl,
}

impl Logic {
    const fn fields(self) -> (bool, u32) {
        match self {
            Logic::And => (false, 0),
            Logic::Bic => (false, 1),
            Logic::Orr => (false, 2),
            Logic::Eor => (true, 0),
            Logic::Bsl => (true, 1),
        }
    }
}

// The float "three same" instructions: U, the high bit of the size field, and the opcode.
const FADD: (bool, u32, u32) = (false, 0, 0b11010);
const FSUB: (bool, u32, u32) = (false, 1, 0b11010);
const FMUL: (bool, u32, u32) = (true, 0, 0b11011);
const FDIV: (bool, u32, u32) = (true, 0, 0b11111);
const FCMEQ: (bool, u32, u32) = (false, 0, 0b11100);
const FCMGE: (bool, u32, u32) = (true, 0, 0b11100);
const FCMGT: (bool, u32, u32) = (true, 1, 0b11100);

// The "two-register miscellaneous" instructions: U, the opcode, and for float ones the high bit of the size field.
const ABS: (bool, u32) = (false, 0b01011);
const NEG: (bool, u32) = (true, 0b01011);
const CNT: (bool, u32) = (false, 0b00101);
const NOT: (bool, u32) = (true, 0b00101);
const CMEQ_ZERO: (bool, u32) = (false, 0b01001);
const SADDLP: (bool, u32) = (false, 0b00010);
const UADDLP: (bool, u32) = (true, 0b00010);
const XTN: (bool, u32) = (false, 0b10010);
const SQXTN: (bool, u32) = (false, 0b10100);
const UQXTN: (bool, u32) = (true, 0b10100);
const SQXTUN: (bool, u32) = (true, 0b10010);
const REV64: (bool, u32) = (false, 0b00000);
const FCVTN: (bool, u32, u32) = (false, 0, 0b10110);
const FCVTL: (bool, u32, u32) = (false, 0, 0b10111);
const FABS: (bool, u32, u32) = (false, 1, 0b01111);
const FNEG: (bool, u32, u32) = (true, 1, 0b01111);
const FSQRT: (bool, u32, u32) = (true, 1, 0b11111);
const FRINTN: (bool, u32, u32) = (false, 0, 0b11000);
const FRINTM: (bool, u32, u32) = (false, 0, 0b11001);
const FRINTP: (bool, u32, u32) = (false, 1, 0b11000);
const FRINTZ: (bool, u32, u32) = (false, 1, 0b11001);
const SCVTF: (bool, u32, u32) = (false, 0, 0b11101);
const UCVTF: (bool, u32, u32) = (true, 0, 0b11101);
const FCVTZS: (bool, u32, u32) = (false, 1, 0b11011);
const FCVTZU: (bool, u32, u32) = (true, 1, 0b11011);

// The "three different" instructions: U and the opcode.
const SMULL: (bool, u32) = (false, 0b1100);
const UMULL: (bool, u32) = (true, 0b1100);
const UMLAL: (bool, u32) = (true, 0b1000);

// The shifts by an immediate: U and the opcode.
const SHL: (bool, u32) = (false, 0b01010);
const SSHR: (bool, u32) = (false, 0b00000);
const USHR: (bool, u32) = (true, 0b00000);
const USRA: (bool, u32) = (true, 0b00010);
const SSHLL: (bool, u32) = (false, 0b10100);
const USHLL: (bool, u32) = (true, 0b10100);

// The permutes.
const UZP1: u32 = 0b001;
const UZP2: u32 = 0b101;

// The copies: imm4.
const DUP_ELEMENT: u32 = 0b0000;
const DUP_GENERAL: u32 = 0b0001;
const INS_GENERAL: u32 = 0b0011;
const SMOV: u32 = 0b0101;
const UMOV: u32 = 0b0111;

/// The imm5 field of a copy of lane `index` of lanes of `1 << size` bytes.
const fn lane_imm5(size: u32, index: u32) -> u32 {
    ((index << 1) | 1) << size
}

/// The size field of a float operation: the high bit, and the precision.
const fn float_size_field(high: u32, double: bool) -> u32 {
    (high << 1) | double as u32
}

impl CodeGenerator<'_> {
    // Instructions.

    fn three(&mut self, (u, opcode): (bool, u32), size: u32, d: Fpr, n: Fpr, m: Fpr) {
        self.asm.vector_three_same(true, (u, opcode), size, d, n, m);
    }

    fn bitwise(&mut self, op: Logic, d: Fpr, n: Fpr, m: Fpr) {
        let (u, size) = op.fields();
        self.asm.vector_three_same(true, (u, 0b00011), size, d, n, m);
    }

    fn float_three(&mut self, (u, high, opcode): (bool, u32, u32), double: bool, d: Fpr, n: Fpr, m: Fpr) {
        self.asm
            .vector_three_same(true, (u, opcode), float_size_field(high, double), d, n, m);
    }

    fn misc(&mut self, (u, opcode): (bool, u32), size: u32, d: Fpr, n: Fpr) {
        self.asm.vector_two_misc(true, u, size, opcode, d, n);
    }

    fn float_misc(&mut self, (u, high, opcode): (bool, u32, u32), double: bool, d: Fpr, n: Fpr) {
        self.asm
            .vector_two_misc(true, u, float_size_field(high, double), opcode, d, n);
    }

    /// A shift left by `shift` of lanes of `1 << size` bytes.
    fn shift_left(&mut self, (u, opcode): (bool, u32), size: u32, shift: u32, d: Fpr, n: Fpr) {
        let bits = 8 << size;
        self.asm.vector_shift_immediate(true, u, bits + shift, opcode, d, n);
    }

    /// A shift right by `shift` (1 to the lane width) of lanes of `1 << size` bytes.
    fn shift_right(&mut self, (u, opcode): (bool, u32), size: u32, shift: u32, d: Fpr, n: Fpr) {
        let bits = 8 << size;
        self.asm.vector_shift_immediate(true, u, 2 * bits - shift, opcode, d, n);
    }

    /// `sshll`/`ushll #0` (`high`: `sshll2`/`ushll2`): the low or high half's lanes of `1 << size` bytes, extended.
    fn extend(&mut self, signed: bool, high: bool, size: u32, d: Fpr, n: Fpr) {
        let (u, opcode) = if signed { SSHLL } else { USHLL };
        self.asm.vector_shift_immediate(high, u, 8 << size, opcode, d, n);
    }

    /// A long multiply (`smull`, `umull`, `umlal` and their high forms) of lanes of `1 << size` bytes.
    fn long(&mut self, (u, opcode): (bool, u32), high: bool, size: u32, d: Fpr, n: Fpr, m: Fpr) {
        self.asm.vector_three_different(high, (u, opcode), size, d, n, m);
    }

    /// A narrowing (`xtn`, `sqxtn`, `uqxtn`, `sqxtun`, or with `high` their second forms) to lanes of `1 << size`
    /// bytes.
    fn narrow(&mut self, (u, opcode): (bool, u32), high: bool, size: u32, d: Fpr, n: Fpr) {
        self.asm.vector_two_misc(high, u, size, opcode, d, n);
    }

    fn not(&mut self, d: Fpr, n: Fpr) {
        self.misc(NOT, 0, d, n);
    }

    /// Fills `d` with the constant `bits`.
    fn vector_constant(&mut self, d: Fpr, bits: u128) {
        self.load_vector_constant(bits, d, true);
    }

    /// Fills every lane of `1 << size` bytes of `d` with the general purpose register `n`.
    fn dup_general(&mut self, size: u32, d: Fpr, n: Gpr) {
        self.asm
            .vector_copy(true, false, lane_imm5(size, 0), DUP_GENERAL, d.0, n.0);
    }

    // Lowerings.

    /// A `VectorLoad`, whose operand `operand_of` makes for an access of `1 << size_log2` bytes.
    pub(super) fn vector_load(&mut self, value: Value, operand_of: impl Fn(&mut Self, u32) -> MemOperand) {
        let node = *self.func.node(value);
        let dst = fpr(self.allocation.node(value).output);
        let sub_opcode = u32::from(node.flags);
        let load = |generator: &mut Self, size_log2: u32| {
            let operand = operand_of(generator, size_log2);
            generator.asm.load_store(fpr_access(size_log2, true), dst.0, operand);
        };
        match sub_opcode {
            V128_LOAD => load(self, 4),
            V128_LOAD8X8_S | V128_LOAD8X8_U | V128_LOAD16X4_S | V128_LOAD16X4_U | V128_LOAD32X2_S | V128_LOAD32X2_U => {
                load(self, 3);
                let (size, signed) = match sub_opcode {
                    V128_LOAD8X8_S => (0, true),
                    V128_LOAD8X8_U => (0, false),
                    V128_LOAD16X4_S => (1, true),
                    V128_LOAD16X4_U => (1, false),
                    V128_LOAD32X2_S => (2, true),
                    _ => (2, false),
                };
                self.extend(signed, false, size, dst, dst);
            }
            V128_LOAD8_SPLAT | V128_LOAD16_SPLAT | V128_LOAD32_SPLAT | V128_LOAD64_SPLAT => {
                let size = match sub_opcode {
                    V128_LOAD8_SPLAT => 0,
                    V128_LOAD16_SPLAT => 1,
                    V128_LOAD32_SPLAT => 2,
                    _ => 3,
                };
                let operand = operand_of(self, size);
                let address = self.address_register(operand);
                self.asm.load_replicate(size, dst, address);
            }
            // NB: Loads of 32 or 64 bits clear the rest of the register.
            V128_LOAD32_ZERO => load(self, 2),
            _ => load(self, 3),
        }
    }

    pub(super) fn lower_vector(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        let record = *self.allocation.node(value);
        let [a, b, c] = record.inputs;
        let output = record.output;
        match node.op {
            Op::VectorUnary => self.vector_unary(node.args[1], node.ty, output, a),
            Op::VectorBinary => self.vector_binary(value, node.args[2], output, a, b),
            Op::VectorTernary => {
                let sub_opcode = if u32::from(node.flags) == V128_BITSELECT {
                    V128_BITSELECT
                } else {
                    0x100 | u32::from(node.flags)
                };
                self.vector_ternary(sub_opcode, fpr(output), [fpr(a), fpr(b), fpr(c)]);
            }
            Op::ExtractLane => self.extract_lane(node.args[1], node.args[2], output, fpr(a)),
            Op::ReplaceLane => {
                let sub_opcode = node.args[2] & 0xffff;
                let lane = node.args[2] >> 16;
                let dst = fpr(output);
                self.move_vector(dst, fpr(a));
                let size = match sub_opcode {
                    I8X16_REPLACE_LANE => 0,
                    I16X8_REPLACE_LANE => 1,
                    I32X4_REPLACE_LANE | F32X4_REPLACE_LANE => 2,
                    _ => 3,
                };
                if matches!(sub_opcode, F32X4_REPLACE_LANE | F64X2_REPLACE_LANE) {
                    // `ins vd.<T>[lane], vn.<T>[0]`
                    self.asm
                        .vector_copy(true, true, lane_imm5(size, lane), 0, dst.0, fpr(b).0);
                } else {
                    self.asm
                        .vector_copy(true, false, lane_imm5(size, lane), INS_GENERAL, dst.0, gpr(b).0);
                }
            }
            Op::Shuffle => {
                let lanes = func.constants[node.args[2] as usize];
                let dst = fpr(output);
                if node.args[0] == node.args[1] {
                    // NB: The builder makes the lanes of a shuffle of one vector 0 to 15, but its operands may have
                    //     become the same value since. A table of one register gives zero for lanes of 16 and up.
                    self.vector_constant(dst, lanes & u128::from_le_bytes([15; 16]));
                    self.asm.vector_table(dst, fpr(a), 1, dst);
                } else {
                    // The table of two registers has to be in consecutive ones.
                    self.move_vector(V30, fpr(a));
                    self.move_vector(V31, fpr(b));
                    self.vector_constant(dst, lanes);
                    self.asm.vector_table(dst, V30, 2, dst);
                }
            }
            _ => unreachable!("{} is no vector op", node.op.name()),
        }
    }

    fn vector_unary(&mut self, sub_opcode: u32, ty: Type, output: Register, a: Register) {
        if ty == Type::I32 {
            self.vector_test(sub_opcode, gpr(output), fpr(a));
            return;
        }
        let dst = fpr(output);
        match sub_opcode {
            I8X16_SPLAT => self.dup_general(0, dst, gpr(a)),
            I16X8_SPLAT => self.dup_general(1, dst, gpr(a)),
            I32X4_SPLAT => self.dup_general(2, dst, gpr(a)),
            I64X2_SPLAT => self.dup_general(3, dst, gpr(a)),
            F32X4_SPLAT => self
                .asm
                .vector_copy(true, false, lane_imm5(2, 0), DUP_ELEMENT, dst.0, fpr(a).0),
            F64X2_SPLAT => self
                .asm
                .vector_copy(true, false, lane_imm5(3, 0), DUP_ELEMENT, dst.0, fpr(a).0),
            _ => self.vector_lanewise_unary(sub_opcode, dst, fpr(a)),
        }
    }

    /// The unary operations from a vector to a vector.
    fn vector_lanewise_unary(&mut self, sub_opcode: u32, dst: Fpr, a: Fpr) {
        match sub_opcode {
            V128_NOT => self.not(dst, a),
            I8X16_ABS => self.misc(ABS, 0, dst, a),
            I16X8_ABS => self.misc(ABS, 1, dst, a),
            I32X4_ABS => self.misc(ABS, 2, dst, a),
            I64X2_ABS => self.misc(ABS, 3, dst, a),
            I8X16_NEG => self.misc(NEG, 0, dst, a),
            I16X8_NEG => self.misc(NEG, 1, dst, a),
            I32X4_NEG => self.misc(NEG, 2, dst, a),
            I64X2_NEG => self.misc(NEG, 3, dst, a),
            I8X16_POPCNT => self.misc(CNT, 0, dst, a),
            // NB: These only change the sign bit, as the interpreter's masks do.
            F32X4_ABS => self.float_misc(FABS, false, dst, a),
            F32X4_NEG => self.float_misc(FNEG, false, dst, a),
            F64X2_ABS => self.float_misc(FABS, true, dst, a),
            F64X2_NEG => self.float_misc(FNEG, true, dst, a),
            F32X4_SQRT => self.float_misc(FSQRT, false, dst, a),
            F64X2_SQRT => self.float_misc(FSQRT, true, dst, a),
            F32X4_NEAREST => self.float_misc(FRINTN, false, dst, a),
            F32X4_FLOOR => self.float_misc(FRINTM, false, dst, a),
            F32X4_CEIL => self.float_misc(FRINTP, false, dst, a),
            F32X4_TRUNC => self.float_misc(FRINTZ, false, dst, a),
            F64X2_NEAREST => self.float_misc(FRINTN, true, dst, a),
            F64X2_FLOOR => self.float_misc(FRINTM, true, dst, a),
            F64X2_CEIL => self.float_misc(FRINTP, true, dst, a),
            F64X2_TRUNC => self.float_misc(FRINTZ, true, dst, a),
            I16X8_EXTEND_LOW_I8X16_S => self.extend(true, false, 0, dst, a),
            I16X8_EXTEND_HIGH_I8X16_S => self.extend(true, true, 0, dst, a),
            I16X8_EXTEND_LOW_I8X16_U => self.extend(false, false, 0, dst, a),
            I16X8_EXTEND_HIGH_I8X16_U => self.extend(false, true, 0, dst, a),
            I32X4_EXTEND_LOW_I16X8_S => self.extend(true, false, 1, dst, a),
            I32X4_EXTEND_HIGH_I16X8_S => self.extend(true, true, 1, dst, a),
            I32X4_EXTEND_LOW_I16X8_U => self.extend(false, false, 1, dst, a),
            I32X4_EXTEND_HIGH_I16X8_U => self.extend(false, true, 1, dst, a),
            I64X2_EXTEND_LOW_I32X4_S => self.extend(true, false, 2, dst, a),
            I64X2_EXTEND_HIGH_I32X4_S => self.extend(true, true, 2, dst, a),
            I64X2_EXTEND_LOW_I32X4_U => self.extend(false, false, 2, dst, a),
            I64X2_EXTEND_HIGH_I32X4_U => self.extend(false, true, 2, dst, a),
            I16X8_EXTADD_PAIRWISE_I8X16_S => self.misc(SADDLP, 0, dst, a),
            I16X8_EXTADD_PAIRWISE_I8X16_U => self.misc(UADDLP, 0, dst, a),
            I32X4_EXTADD_PAIRWISE_I16X8_S => self.misc(SADDLP, 1, dst, a),
            I32X4_EXTADD_PAIRWISE_I16X8_U => self.misc(UADDLP, 1, dst, a),
            // NB: The conversions from integers round once, as the spec wants, so any exact sequence agrees.
            F32X4_CONVERT_I32X4_S => self.float_misc(SCVTF, false, dst, a),
            F32X4_CONVERT_I32X4_U => self.float_misc(UCVTF, false, dst, a),
            F64X2_CONVERT_LOW_I32X4_S => {
                self.extend(true, false, 2, dst, a);
                self.float_misc(SCVTF, true, dst, dst);
            }
            F64X2_CONVERT_LOW_I32X4_U => {
                self.extend(false, false, 2, dst, a);
                self.float_misc(UCVTF, true, dst, dst);
            }
            // NB: The truncations saturate and give 0 for NaN, as the saturating ones want.
            I32X4_TRUNC_SAT_F32X4_S => self.float_misc(FCVTZS, false, dst, a),
            I32X4_TRUNC_SAT_F32X4_U => self.float_misc(FCVTZU, false, dst, a),
            I32X4_TRUNC_SAT_F64X2_S_ZERO => {
                self.float_misc(FCVTZS, true, dst, a);
                self.narrow(SQXTN, false, 2, dst, dst);
            }
            I32X4_TRUNC_SAT_F64X2_U_ZERO => {
                self.float_misc(FCVTZU, true, dst, a);
                self.narrow(UQXTN, false, 2, dst, dst);
            }
            // The interpreter's demotion and promotion are these instructions.
            F32X4_DEMOTE_F64X2_ZERO => self.asm.vector_two_misc(false, FCVTN.0, 1, FCVTN.2, dst, a),
            F64X2_PROMOTE_LOW_F32X4 => self.asm.vector_two_misc(false, FCVTL.0, 1, FCVTL.2, dst, a),
            _ => unreachable!("SIMD sub-opcode {sub_opcode:#x} is no unary vector operation"),
        }
    }

    /// The reductions of a vector to an i32.
    fn vector_test(&mut self, sub_opcode: u32, dst: Gpr, a: Fpr) {
        match sub_opcode {
            V128_ANY_TRUE => {
                // umaxv s31, a.4s
                self.asm.vector_across(true, true, 2, 0b01010, V31, a);
                self.asm.fmov_to_gpr(dst, V31);
                self.asm.cmp_imm(Width::W64, dst, 0, false);
                self.asm.cset(Width::W32, dst, crate::asm::aarch64::Cond::Ne);
            }
            I8X16_ALL_TRUE | I16X8_ALL_TRUE | I32X4_ALL_TRUE | I64X2_ALL_TRUE => {
                // All lanes are nonzero where no lane equals zero.
                let size = match sub_opcode {
                    I8X16_ALL_TRUE => 0,
                    I16X8_ALL_TRUE => 1,
                    I32X4_ALL_TRUE => 2,
                    _ => 3,
                };
                self.misc(CMEQ_ZERO, size, V31, a);
                self.asm.vector_across(true, true, 2, 0b01010, V31, V31);
                self.asm.fmov_to_gpr(dst, V31);
                self.asm.cmp_imm(Width::W64, dst, 0, false);
                self.asm.cset(Width::W32, dst, crate::asm::aarch64::Cond::Eq);
            }
            I8X16_BITMASK | I16X8_BITMASK => {
                // The sign bits gathered: each shift-and-accumulate doubles the bits a lane holds, until the low
                // byte of each 64-bit half holds those of its half.
                let (size, half_bits) = if sub_opcode == I8X16_BITMASK { (0u32, 8) } else { (1, 4) };
                let bits = 8u32 << size;
                self.shift_right(USHR, size, bits - 1, V31, a);
                // Each doubling of the lane width moves the upper half's bits next to the lower half's.
                let mut gathered = 1u32;
                for wider in size + 1..4 {
                    self.shift_right(USRA, wider, (8 << (wider - 1)) - gathered, V31, V31);
                    gathered *= 2;
                }
                self.asm
                    .vector_copy(false, false, lane_imm5(0, 0), UMOV, SCRATCH.0, V31.0);
                self.asm
                    .vector_copy(false, false, lane_imm5(0, 8), UMOV, super::ADDRESS.0, V31.0);
                self.asm.orr(
                    Width::W32,
                    dst,
                    SCRATCH,
                    super::ADDRESS,
                    crate::asm::aarch64::Shift::Lsl,
                    half_bits,
                );
            }
            I32X4_BITMASK => {
                self.shift_right(USHR, 2, 31, V31, a);
                self.shift_right(USRA, 3, 31, V31, V31);
                self.asm
                    .vector_copy(false, false, lane_imm5(2, 0), UMOV, SCRATCH.0, V31.0);
                self.asm
                    .vector_copy(false, false, lane_imm5(2, 2), UMOV, super::ADDRESS.0, V31.0);
                self.asm.orr(
                    Width::W32,
                    dst,
                    SCRATCH,
                    super::ADDRESS,
                    crate::asm::aarch64::Shift::Lsl,
                    2,
                );
            }
            I64X2_BITMASK => {
                self.shift_right(USHR, 3, 63, V31, a);
                self.asm
                    .vector_copy(true, false, lane_imm5(3, 0), UMOV, SCRATCH.0, V31.0);
                self.asm
                    .vector_copy(true, false, lane_imm5(3, 1), UMOV, super::ADDRESS.0, V31.0);
                self.asm.orr(
                    Width::W32,
                    dst,
                    SCRATCH,
                    super::ADDRESS,
                    crate::asm::aarch64::Shift::Lsl,
                    1,
                );
            }
            _ => unreachable!("SIMD sub-opcode {sub_opcode:#x} is no reduction to an i32"),
        }
    }

    /// The interpreter's `v128_min_f*(x, y)`: `y > x ? x : y`, lane by lane.
    fn interpreter_min(&mut self, double: bool, d: Fpr, x: Fpr, y: Fpr) {
        self.float_three(FCMGT, double, d, y, x);
        self.bitwise(Logic::Bsl, d, x, y);
    }

    /// The interpreter's `v128_max_f*(x, y)`: `x > y ? x : y`, lane by lane.
    fn interpreter_max(&mut self, double: bool, d: Fpr, x: Fpr, y: Fpr) {
        self.float_three(FCMGT, double, d, x, y);
        self.bitwise(Logic::Bsl, d, x, y);
    }

    /// The interpreter's `v128_unord_f*(x, y)`, into `d`, through v30.
    fn interpreter_unordered(&mut self, double: bool, d: Fpr, x: Fpr, y: Fpr) {
        self.float_three(FCMEQ, double, V30, x, x);
        self.float_three(FCMEQ, double, d, y, y);
        self.bitwise(Logic::And, d, V30, d);
        self.not(d, d);
    }

    fn vector_binary(&mut self, value: Value, sub_opcode: u32, output: Register, a: Register, b: Register) {
        let dst = fpr(output);
        if is_vector_shift(sub_opcode) {
            let count = (b == CONSTANT_INPUT).then(|| self.immediate(value, 1) as u32);
            self.vector_shift(sub_opcode, dst, fpr(a), b, count);
            return;
        }
        let (a, b) = (fpr(a), fpr(b));
        let integer = |op: (bool, u32), size: u32| (op, size, false);
        let swapped = |op: (bool, u32), size: u32| (op, size, true);
        let simple = match sub_opcode {
            I8X16_EQ => Some(integer(CMEQ, 0)),
            I16X8_EQ => Some(integer(CMEQ, 1)),
            I32X4_EQ => Some(integer(CMEQ, 2)),
            I64X2_EQ => Some(integer(CMEQ, 3)),
            I8X16_GT_S => Some(integer(CMGT, 0)),
            I16X8_GT_S => Some(integer(CMGT, 1)),
            I32X4_GT_S => Some(integer(CMGT, 2)),
            I64X2_GT_S => Some(integer(CMGT, 3)),
            I8X16_LT_S => Some(swapped(CMGT, 0)),
            I16X8_LT_S => Some(swapped(CMGT, 1)),
            I32X4_LT_S => Some(swapped(CMGT, 2)),
            I64X2_LT_S => Some(swapped(CMGT, 3)),
            I8X16_GE_S => Some(integer(CMGE, 0)),
            I16X8_GE_S => Some(integer(CMGE, 1)),
            I32X4_GE_S => Some(integer(CMGE, 2)),
            I64X2_GE_S => Some(integer(CMGE, 3)),
            I8X16_LE_S => Some(swapped(CMGE, 0)),
            I16X8_LE_S => Some(swapped(CMGE, 1)),
            I32X4_LE_S => Some(swapped(CMGE, 2)),
            I64X2_LE_S => Some(swapped(CMGE, 3)),
            I8X16_GT_U => Some(integer(CMHI, 0)),
            I16X8_GT_U => Some(integer(CMHI, 1)),
            I32X4_GT_U => Some(integer(CMHI, 2)),
            I8X16_LT_U => Some(swapped(CMHI, 0)),
            I16X8_LT_U => Some(swapped(CMHI, 1)),
            I32X4_LT_U => Some(swapped(CMHI, 2)),
            I8X16_GE_U => Some(integer(CMHS, 0)),
            I16X8_GE_U => Some(integer(CMHS, 1)),
            I32X4_GE_U => Some(integer(CMHS, 2)),
            I8X16_LE_U => Some(swapped(CMHS, 0)),
            I16X8_LE_U => Some(swapped(CMHS, 1)),
            I32X4_LE_U => Some(swapped(CMHS, 2)),
            I8X16_ADD => Some(integer(ADD, 0)),
            I16X8_ADD => Some(integer(ADD, 1)),
            I32X4_ADD => Some(integer(ADD, 2)),
            I64X2_ADD => Some(integer(ADD, 3)),
            I8X16_SUB => Some(integer(SUB, 0)),
            I16X8_SUB => Some(integer(SUB, 1)),
            I32X4_SUB => Some(integer(SUB, 2)),
            I64X2_SUB => Some(integer(SUB, 3)),
            I8X16_ADD_SAT_S => Some(integer(SQADD, 0)),
            I8X16_ADD_SAT_U => Some(integer(UQADD, 0)),
            I8X16_SUB_SAT_S => Some(integer(SQSUB, 0)),
            I8X16_SUB_SAT_U => Some(integer(UQSUB, 0)),
            I16X8_ADD_SAT_S => Some(integer(SQADD, 1)),
            I16X8_ADD_SAT_U => Some(integer(UQADD, 1)),
            I16X8_SUB_SAT_S => Some(integer(SQSUB, 1)),
            I16X8_SUB_SAT_U => Some(integer(UQSUB, 1)),
            I8X16_AVGR_U => Some(integer(URHADD, 0)),
            I16X8_AVGR_U => Some(integer(URHADD, 1)),
            I16X8_MUL => Some(integer(MUL, 1)),
            I32X4_MUL => Some(integer(MUL, 2)),
            I8X16_MIN_S => Some(integer(SMIN, 0)),
            I8X16_MIN_U => Some(integer(UMIN, 0)),
            I8X16_MAX_S => Some(integer(SMAX, 0)),
            I8X16_MAX_U => Some(integer(UMAX, 0)),
            I16X8_MIN_S => Some(integer(SMIN, 1)),
            I16X8_MIN_U => Some(integer(UMIN, 1)),
            I16X8_MAX_S => Some(integer(SMAX, 1)),
            I16X8_MAX_U => Some(integer(UMAX, 1)),
            I32X4_MIN_S => Some(integer(SMIN, 2)),
            I32X4_MIN_U => Some(integer(UMIN, 2)),
            I32X4_MAX_S => Some(integer(SMAX, 2)),
            I32X4_MAX_U => Some(integer(UMAX, 2)),
            // NB: The rounding doubling multiply saturates the one product that overflows, -1 * -1.
            I16X8_Q15MULR_SAT_S => Some(integer(SQRDMULH, 1)),
            _ => None,
        };
        if let Some((op, size, swap)) = simple {
            let (lhs, rhs) = if swap { (b, a) } else { (a, b) };
            self.three(op, size, dst, lhs, rhs);
            return;
        }
        match sub_opcode {
            V128_AND => self.bitwise(Logic::And, dst, a, b),
            V128_OR => self.bitwise(Logic::Orr, dst, a, b),
            V128_XOR => self.bitwise(Logic::Eor, dst, a, b),
            V128_ANDNOT => self.bitwise(Logic::Bic, dst, a, b),
            I8X16_NE | I16X8_NE | I32X4_NE | I64X2_NE => {
                let size = match sub_opcode {
                    I8X16_NE => 0,
                    I16X8_NE => 1,
                    I32X4_NE => 2,
                    _ => 3,
                };
                self.three(CMEQ, size, dst, a, b);
                self.not(dst, dst);
            }
            F32X4_EQ | F64X2_EQ => self.float_three(FCMEQ, sub_opcode == F64X2_EQ, dst, a, b),
            F32X4_NE | F64X2_NE => {
                self.float_three(FCMEQ, sub_opcode == F64X2_NE, dst, a, b);
                self.not(dst, dst);
            }
            F32X4_LT | F64X2_LT => self.float_three(FCMGT, sub_opcode == F64X2_LT, dst, b, a),
            F32X4_LE | F64X2_LE => self.float_three(FCMGE, sub_opcode == F64X2_LE, dst, b, a),
            F32X4_GT | F64X2_GT => self.float_three(FCMGT, sub_opcode == F64X2_GT, dst, a, b),
            F32X4_GE | F64X2_GE => self.float_three(FCMGE, sub_opcode == F64X2_GE, dst, a, b),
            F32X4_ADD | F64X2_ADD => self.float_three(FADD, sub_opcode == F64X2_ADD, dst, a, b),
            F32X4_SUB | F64X2_SUB => self.float_three(FSUB, sub_opcode == F64X2_SUB, dst, a, b),
            F32X4_MUL | F64X2_MUL => self.float_three(FMUL, sub_opcode == F64X2_MUL, dst, a, b),
            F32X4_DIV | F64X2_DIV => self.float_three(FDIV, sub_opcode == F64X2_DIV, dst, a, b),
            // pmin is b < a ? b : a, the interpreter's min of b and a.
            F32X4_PMIN | F64X2_PMIN => self.interpreter_min(sub_opcode == F64X2_PMIN, dst, b, a),
            F32X4_PMAX | F64X2_PMAX => self.interpreter_max(sub_opcode == F64X2_PMAX, dst, b, a),
            F32X4_MIN | F64X2_MIN => {
                // Both orders, so that a NaN or -0 in either operand makes it into the result, and NaN lanes
                // canonical.
                let double = sub_opcode == F64X2_MIN;
                self.interpreter_min(double, V31, a, b);
                self.interpreter_min(double, dst, b, a);
                self.bitwise(Logic::Orr, dst, dst, V31);
                self.interpreter_unordered(double, V31, dst, dst);
                self.bitwise(Logic::Orr, dst, dst, V31);
                self.nan_payload_shift(double, V31);
                self.bitwise(Logic::Bic, dst, dst, V31);
            }
            F32X4_MAX | F64X2_MAX => {
                // Both orders, so that a NaN in either operand makes it into the result, +0 beats -0, and NaN
                // lanes are canonical.
                let double = sub_opcode == F64X2_MAX;
                self.interpreter_max(double, V31, a, b);
                self.interpreter_max(double, dst, b, a);
                self.bitwise(Logic::Eor, V31, V31, dst);
                self.bitwise(Logic::Orr, dst, dst, V31);
                self.float_three(FSUB, double, dst, dst, V31);
                self.interpreter_unordered(double, V31, V31, dst);
                self.nan_payload_shift(double, V31);
                self.bitwise(Logic::Bic, dst, dst, V31);
            }
            I8X16_SWIZZLE => {
                // NB: tbl picks zero for indices of 16 and up.
                self.asm.vector_table(dst, a, 1, b);
            }
            I8X16_NARROW_I16X8_S | I8X16_NARROW_I16X8_U | I16X8_NARROW_I32X4_S | I16X8_NARROW_I32X4_U => {
                let op = if matches!(sub_opcode, I8X16_NARROW_I16X8_S | I16X8_NARROW_I32X4_S) {
                    SQXTN
                } else {
                    SQXTUN
                };
                let size = if matches!(sub_opcode, I8X16_NARROW_I16X8_S | I8X16_NARROW_I16X8_U) {
                    0
                } else {
                    1
                };
                self.narrow(op, false, size, dst, a);
                self.narrow(op, true, size, dst, b);
            }
            I32X4_DOT_I16X8_S => {
                self.long(SMULL, false, 1, V31, a, b);
                self.long(SMULL, true, 1, V30, a, b);
                self.three(ADDP, 2, dst, V31, V30);
            }
            I64X2_MUL => {
                // The cross products of the halves added, shifted up, plus the low halves' full product.
                self.misc(REV64, 2, V31, b);
                self.three(MUL, 2, V31, V31, a);
                self.misc(UADDLP, 2, dst, V31);
                self.shift_left(SHL, 3, 32, dst, dst);
                self.narrow(XTN, false, 2, V31, a);
                self.narrow(XTN, false, 2, V30, b);
                self.long(UMLAL, false, 2, dst, V31, V30);
            }
            I16X8_EXTMUL_LOW_I8X16_S => self.long(SMULL, false, 0, dst, a, b),
            I16X8_EXTMUL_HIGH_I8X16_S => self.long(SMULL, true, 0, dst, a, b),
            I16X8_EXTMUL_LOW_I8X16_U => self.long(UMULL, false, 0, dst, a, b),
            I16X8_EXTMUL_HIGH_I8X16_U => self.long(UMULL, true, 0, dst, a, b),
            I32X4_EXTMUL_LOW_I16X8_S => self.long(SMULL, false, 1, dst, a, b),
            I32X4_EXTMUL_HIGH_I16X8_S => self.long(SMULL, true, 1, dst, a, b),
            I32X4_EXTMUL_LOW_I16X8_U => self.long(UMULL, false, 1, dst, a, b),
            I32X4_EXTMUL_HIGH_I16X8_U => self.long(UMULL, true, 1, dst, a, b),
            I64X2_EXTMUL_LOW_I32X4_S => self.long(SMULL, false, 2, dst, a, b),
            I64X2_EXTMUL_HIGH_I32X4_S => self.long(SMULL, true, 2, dst, a, b),
            I64X2_EXTMUL_LOW_I32X4_U => self.long(UMULL, false, 2, dst, a, b),
            I64X2_EXTMUL_HIGH_I32X4_U => self.long(UMULL, true, 2, dst, a, b),
            I16X8_RELAXED_DOT_I8X16_I7X16_S => self.multiply_add_unsigned_signed(dst, b, a),
            _ => unreachable!("SIMD sub-opcode {sub_opcode:#x} is no binary vector operation"),
        }
    }

    /// The bits a NaN lane of an all-ones mask in `d` keeps of a value, shifted into place: the interpreter shifts
    /// the mask right by 10 for floats and 13 for doubles, so that the sign, the exponent and the quiet bit remain.
    fn nan_payload_shift(&mut self, double: bool, d: Fpr) {
        if double {
            self.shift_right(USHR, 3, 13, d, d);
        } else {
            self.shift_right(USHR, 2, 10, d, d);
        }
    }

    /// The interpreter's `v128_madd_u8_s8(u, s)`: the unsigned bytes of `u` times the signed bytes of `s`, adjacent
    /// products added into 16-bit lanes with signed saturation.
    fn multiply_add_unsigned_signed(&mut self, dst: Fpr, u: Fpr, s: Fpr) {
        self.extend(false, false, 0, V31, u);
        self.extend(true, false, 0, V30, s);
        self.three(MUL, 1, V31, V31, V30);
        self.extend(false, true, 0, dst, u);
        self.extend(true, true, 0, V30, s);
        self.three(MUL, 1, dst, dst, V30);
        self.asm.vector_permute(VectorArrangement::H8, UZP1, V30, V31, dst);
        self.asm.vector_permute(VectorArrangement::H8, UZP2, dst, V31, dst);
        self.three(SQADD, 1, dst, V30, dst);
    }

    /// A shift of the vector in `a` by the i32 in `count_register`, or by `count` where it is a constant.
    fn vector_shift(&mut self, sub_opcode: u32, dst: Fpr, a: Fpr, count_register: Register, count: Option<u32>) {
        let size = match sub_opcode {
            I8X16_SHL | I8X16_SHR_S | I8X16_SHR_U => 0,
            I16X8_SHL | I16X8_SHR_S | I16X8_SHR_U => 1,
            I32X4_SHL | I32X4_SHR_S | I32X4_SHR_U => 2,
            _ => 3,
        };
        let bits = 8u32 << size;
        let left = matches!(sub_opcode, I8X16_SHL | I16X8_SHL | I32X4_SHL | I64X2_SHL);
        let signed = matches!(sub_opcode, I8X16_SHR_S | I16X8_SHR_S | I32X4_SHR_S | I64X2_SHR_S);
        if let Some(count) = count {
            let count = count & (bits - 1);
            if count == 0 {
                self.move_vector(dst, a);
            } else if left {
                self.shift_left(SHL, size, count, dst, a);
            } else {
                self.shift_right(if signed { SSHR } else { USHR }, size, count, dst, a);
            }
            return;
        }
        // The count masked, negated for right shifts (sshl and ushl shift right by negative counts), in every lane.
        self.asm
            .and_imm(Width::W64, SCRATCH, gpr(count_register), u64::from(bits - 1));
        if !left {
            self.asm.neg(Width::W64, SCRATCH, SCRATCH);
        }
        self.dup_general(size, V31, SCRATCH);
        self.three(if signed { SSHL } else { USHL }, size, dst, a, V31);
    }

    fn vector_ternary(&mut self, sub_opcode: u32, dst: Fpr, [a, b, c]: [Fpr; 3]) {
        match sub_opcode {
            V128_BITSELECT => {
                // (a & c) | (b & !c)
                self.move_vector(dst, c);
                self.bitwise(Logic::Bsl, dst, a, b);
            }
            // The multiply-adds round twice, as the interpreter's do.
            F32X4_RELAXED_MADD | F64X2_RELAXED_MADD => {
                let double = sub_opcode == F64X2_RELAXED_MADD;
                self.float_three(FMUL, double, dst, a, b);
                self.float_three(FADD, double, dst, dst, c);
            }
            F32X4_RELAXED_NMADD | F64X2_RELAXED_NMADD => {
                let double = sub_opcode == F64X2_RELAXED_NMADD;
                self.float_three(FMUL, double, dst, a, b);
                self.float_three(FSUB, double, dst, c, dst);
            }
            I32X4_RELAXED_DOT_I8X16_I7X16_ADD_S => {
                // Pairs as the 16-bit dot product makes them, then added in pairs again, and to c.
                self.multiply_add_unsigned_signed(dst, b, a);
                self.misc(SADDLP, 1, dst, dst);
                self.three(ADD, 2, dst, dst, c);
            }
            _ => unreachable!("SIMD sub-opcode {sub_opcode:#x} is no ternary vector operation"),
        }
    }

    fn extract_lane(&mut self, sub_opcode: u32, lane: u32, output: Register, vector: Fpr) {
        // `smov`, `umov` (Q for a 64-bit lane), or for floats `dup sd/dd, vn.<T>[lane]` (scalar), which clears the
        // rest of the register.
        let (size, copy) = match sub_opcode {
            I8X16_EXTRACT_LANE_S => (0, Some((false, SMOV))),
            I8X16_EXTRACT_LANE_U => (0, Some((false, UMOV))),
            I16X8_EXTRACT_LANE_S => (1, Some((false, SMOV))),
            I16X8_EXTRACT_LANE_U => (1, Some((false, UMOV))),
            I32X4_EXTRACT_LANE => (2, Some((false, UMOV))),
            I64X2_EXTRACT_LANE => (3, Some((true, UMOV))),
            F32X4_EXTRACT_LANE => (2, None),
            _ => (3, None),
        };
        if let Some((q, imm4)) = copy {
            self.asm
                .vector_copy(q, false, lane_imm5(size, lane), imm4, gpr(output).0, vector.0);
        } else {
            let destination = fpr(output);
            self.asm.emit(
                0x5e00_0400 | (lane_imm5(size, lane) << 16) | (u32::from(vector.0) << 5) | u32::from(destination.0),
            );
        }
    }
}
