/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Vectors: v128 loads, constants, lanes and SIMD arithmetic, as AVX2 instructions on xmm registers.

use super::{CodeGenerator, SCRATCH, SCRATCH_FPR, fpr, gpr};
use crate::asm::x86_64::{Cond, VexMap, VexOpcode, VexOperand, VexPrefix};
use crate::asm::{Address, Width};
use crate::convention::Register;
use crate::ir::{Op, Type, Value};
use libwasm_module::opcode::simd::*;

const fn op(prefix: VexPrefix, map: VexMap, opcode: u8) -> VexOpcode {
    VexOpcode::new(prefix, map, opcode)
}

const fn op66(opcode: u8) -> VexOpcode {
    op(VexPrefix::OperandSize, VexMap::Map0F, opcode)
}

const fn op66_38(opcode: u8) -> VexOpcode {
    op(VexPrefix::OperandSize, VexMap::Map0F38, opcode)
}

const fn op66_3a(opcode: u8) -> VexOpcode {
    op(VexPrefix::OperandSize, VexMap::Map0F3A, opcode)
}

pub(super) const VMOVUPS_LOAD: VexOpcode = op(VexPrefix::None, VexMap::Map0F, 0x10);
pub(super) const VMOVUPS_STORE: VexOpcode = op(VexPrefix::None, VexMap::Map0F, 0x11);
const VMOVD_LOAD: VexOpcode = op66(0x6e);
const VMOVQ_LOAD: VexOpcode = op(VexPrefix::Rep, VexMap::Map0F, 0x7e);
const VPXOR: VexOpcode = op66(0xef);
const VPAND: VexOpcode = op66(0xdb);
const VPANDN: VexOpcode = op66(0xdf);
const VPOR: VexOpcode = op66(0xeb);
const VPCMPEQD: VexOpcode = op66(0x76);
const VPTEST: VexOpcode = op66_38(0x17);
const VPSHUFB: VexOpcode = op66_38(0x00);
const VPADDUSB: VexOpcode = op66(0xdc);
const VPSUBB: VexOpcode = op66(0xf8);
const VPSUBW: VexOpcode = op66(0xf9);
const VPSUBD: VexOpcode = op66(0xfa);
const VPSUBQ: VexOpcode = op66(0xfb);
const VPBROADCASTB: VexOpcode = op66_38(0x78);
const VPBROADCASTW: VexOpcode = op66_38(0x79);
const VPBROADCASTD: VexOpcode = op66_38(0x58);
const VPBROADCASTQ: VexOpcode = op66_38(0x59);
const VBROADCASTSS: VexOpcode = op66_38(0x18);
const VMOVDDUP: VexOpcode = op(VexPrefix::Repne, VexMap::Map0F, 0x12);
const VPEXTRB: VexOpcode = op66_3a(0x14);
const VPEXTRW: VexOpcode = op66_3a(0x15);
const VPEXTRD: VexOpcode = op66_3a(0x16);
const VPINSRB: VexOpcode = op66_3a(0x20);
const VPINSRW: VexOpcode = op66(0xc4);
const VPINSRD: VexOpcode = op66_3a(0x22);
const VINSERTPS: VexOpcode = op66_3a(0x21);
const VPERMILPS: VexOpcode = op66_3a(0x04);
const VPERMILPD: VexOpcode = op66_3a(0x05);
const VMOVSD_MERGE: VexOpcode = op(VexPrefix::Repne, VexMap::Map0F, 0x10);
const VMOVLHPS: VexOpcode = op(VexPrefix::None, VexMap::Map0F, 0x16);
const VPMOVMSKB: VexOpcode = op66(0xd7);
const VMOVMSKPS: VexOpcode = op(VexPrefix::None, VexMap::Map0F, 0x50);
const VMOVMSKPD: VexOpcode = op66(0x50);
const VANDNPS: VexOpcode = op(VexPrefix::None, VexMap::Map0F, 0x55);
const VXORPS: VexOpcode = op(VexPrefix::None, VexMap::Map0F, 0x57);

/// The instruction of a binary SIMD operation that maps to one, with whether its operands are swapped and whether
/// its result is inverted (for comparisons), and the immediate of `vcmpps` and `vcmppd`.
struct Binary {
    opcode: VexOpcode,
    swap: bool,
    invert: bool,
    immediate: Option<u8>,
}

const fn plain(opcode: VexOpcode) -> Binary {
    Binary {
        opcode,
        swap: false,
        invert: false,
        immediate: None,
    }
}

fn binary(sub_opcode: u32) -> Option<Binary> {
    let swapped = |opcode| Binary {
        opcode,
        swap: true,
        invert: false,
        immediate: None,
    };
    let inverted = |opcode| Binary {
        opcode,
        swap: false,
        invert: true,
        immediate: None,
    };
    let compare = |opcode, swap, immediate| Binary {
        opcode,
        swap,
        invert: false,
        immediate: Some(immediate),
    };
    let ps = op(VexPrefix::None, VexMap::Map0F, 0xc2);
    let pd = op66(0xc2);
    Some(match sub_opcode {
        V128_AND => plain(VPAND),
        V128_OR => plain(VPOR),
        V128_XOR => plain(VPXOR),
        // NB: andnot is a & !b, vpandn computes !lhs & rhs.
        V128_ANDNOT => swapped(VPANDN),
        I8X16_EQ => plain(op66(0x74)),
        I16X8_EQ => plain(op66(0x75)),
        I32X4_EQ => plain(op66(0x76)),
        I64X2_EQ => plain(op66_38(0x29)),
        I8X16_NE => inverted(op66(0x74)),
        I16X8_NE => inverted(op66(0x75)),
        I32X4_NE => inverted(op66(0x76)),
        I64X2_NE => inverted(op66_38(0x29)),
        I8X16_GT_S => plain(op66(0x64)),
        I16X8_GT_S => plain(op66(0x65)),
        I32X4_GT_S => plain(op66(0x66)),
        I64X2_GT_S => plain(op66_38(0x37)),
        I8X16_LT_S => swapped(op66(0x64)),
        I16X8_LT_S => swapped(op66(0x65)),
        I32X4_LT_S => swapped(op66(0x66)),
        I64X2_LT_S => swapped(op66_38(0x37)),
        F32X4_EQ => compare(ps, false, 0),
        F32X4_NE => compare(ps, false, 4),
        F32X4_LT => compare(ps, false, 1),
        F32X4_LE => compare(ps, false, 2),
        F32X4_GT => compare(ps, true, 1),
        F32X4_GE => compare(ps, true, 2),
        F64X2_EQ => compare(pd, false, 0),
        F64X2_NE => compare(pd, false, 4),
        F64X2_LT => compare(pd, false, 1),
        F64X2_LE => compare(pd, false, 2),
        F64X2_GT => compare(pd, true, 1),
        F64X2_GE => compare(pd, true, 2),
        I8X16_ADD => plain(op66(0xfc)),
        I16X8_ADD => plain(op66(0xfd)),
        I32X4_ADD => plain(op66(0xfe)),
        I64X2_ADD => plain(op66(0xd4)),
        I8X16_SUB => plain(VPSUBB),
        I16X8_SUB => plain(VPSUBW),
        I32X4_SUB => plain(VPSUBD),
        I64X2_SUB => plain(VPSUBQ),
        I8X16_ADD_SAT_S => plain(op66(0xec)),
        I8X16_ADD_SAT_U => plain(VPADDUSB),
        I8X16_SUB_SAT_S => plain(op66(0xe8)),
        I8X16_SUB_SAT_U => plain(op66(0xd8)),
        I16X8_MUL => plain(op66(0xd5)),
        I32X4_MUL => plain(op66_38(0x40)),
        I8X16_MIN_S => plain(op66_38(0x38)),
        I8X16_MIN_U => plain(op66(0xda)),
        I8X16_MAX_S => plain(op66_38(0x3c)),
        I8X16_MAX_U => plain(op66(0xde)),
        I16X8_MIN_S => plain(op66(0xea)),
        I16X8_MIN_U => plain(op66_38(0x3a)),
        I16X8_MAX_S => plain(op66(0xee)),
        I16X8_MAX_U => plain(op66_38(0x3e)),
        I32X4_MIN_S => plain(op66_38(0x39)),
        I32X4_MIN_U => plain(op66_38(0x3b)),
        I32X4_MAX_S => plain(op66_38(0x3d)),
        I32X4_MAX_U => plain(op66_38(0x3f)),
        F32X4_ADD => plain(op(VexPrefix::None, VexMap::Map0F, 0x58)),
        F32X4_SUB => plain(op(VexPrefix::None, VexMap::Map0F, 0x5c)),
        F32X4_MUL => plain(op(VexPrefix::None, VexMap::Map0F, 0x59)),
        F32X4_DIV => plain(op(VexPrefix::None, VexMap::Map0F, 0x5e)),
        F64X2_ADD => plain(op66(0x58)),
        F64X2_SUB => plain(op66(0x5c)),
        F64X2_MUL => plain(op66(0x59)),
        F64X2_DIV => plain(op66(0x5e)),
        _ => return None,
    })
}

/// The shift by an xmm count of a SIMD shift, and the bits of its lanes.
fn shift(sub_opcode: u32) -> Option<(VexOpcode, u32)> {
    Some(match sub_opcode {
        I16X8_SHL => (op66(0xf1), 16),
        I16X8_SHR_S => (op66(0xe1), 16),
        I16X8_SHR_U => (op66(0xd1), 16),
        I32X4_SHL => (op66(0xf2), 32),
        I32X4_SHR_S => (op66(0xe2), 32),
        I32X4_SHR_U => (op66(0xd2), 32),
        I64X2_SHL => (op66(0xf3), 64),
        I64X2_SHR_U => (op66(0xd3), 64),
        _ => return None,
    })
}

fn register(register: Register) -> VexOperand {
    VexOperand::Register(register_number(register))
}

/// The encoding of an allocator register, of either class.
fn register_number(register: Register) -> u8 {
    if crate::convention::is_fpr(register) {
        fpr(register).0
    } else {
        gpr(register).0
    }
}

const SCRATCH_XMM: u8 = SCRATCH_FPR.0;

impl CodeGenerator<'_> {
    /// `op dst, lhs, rhs` with allocator registers.
    fn vector_op(&mut self, opcode: VexOpcode, dst: u8, lhs: u8, rhs: VexOperand) {
        self.asm.vex(opcode, dst, lhs, rhs, None);
    }

    /// The label of a 16-byte constant in the constant pool.
    fn vector_label(&mut self, bits: u128) -> crate::asm::Label {
        self.constant_label(bits)
    }

    /// Fills the scratch xmm register with ones.
    fn scratch_ones(&mut self) {
        self.asm.vex(
            VPCMPEQD,
            SCRATCH_XMM,
            SCRATCH_XMM,
            VexOperand::Register(SCRATCH_XMM),
            None,
        );
    }

    pub(super) fn vector_load(&mut self, value: Value, address: &Address) {
        let node = *self.func.node(value);
        let dst = fpr(self.allocation.node(value).output).0;
        let memory = VexOperand::Memory(*address);
        let opcode = match u32::from(node.flags) {
            V128_LOAD => VMOVUPS_LOAD,
            V128_LOAD8X8_S => op66_38(0x20),
            V128_LOAD8X8_U => op66_38(0x30),
            V128_LOAD16X4_S => op66_38(0x23),
            V128_LOAD16X4_U => op66_38(0x33),
            V128_LOAD32X2_S => op66_38(0x25),
            V128_LOAD32X2_U => op66_38(0x35),
            V128_LOAD8_SPLAT => VPBROADCASTB,
            V128_LOAD16_SPLAT => VPBROADCASTW,
            V128_LOAD32_SPLAT => VPBROADCASTD,
            V128_LOAD64_SPLAT => VPBROADCASTQ,
            V128_LOAD32_ZERO => VMOVD_LOAD,
            _ => VMOVQ_LOAD,
        };
        self.asm.vex(opcode, dst, 0, memory, None);
    }

    pub(super) fn lower_vector(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        let record = *self.allocation.node(value);
        let [a, b, c] = record.inputs;
        let output = record.output;
        match node.op {
            Op::VectorUnary => self.vector_unary(node.args[1], node.ty, output, a, record.temps[0]),
            Op::VectorBinary => self.vector_binary(node.args[2], output, a, b),
            Op::VectorTernary => {
                // bitselect(a, b, mask) = (a & mask) | (b & !mask)
                let (dst, a, b, mask) = (fpr(output).0, fpr(a).0, fpr(b).0, fpr(c).0);
                let temp = fpr(record.temps[0]).0;
                self.vector_op(VPAND, temp, a, VexOperand::Register(mask));
                self.vector_op(VPANDN, dst, mask, VexOperand::Register(b));
                self.vector_op(VPOR, dst, dst, VexOperand::Register(temp));
            }
            Op::ExtractLane => self.extract_lane(node.args[1], node.args[2] as u8, output, a),
            Op::ReplaceLane => {
                let sub_opcode = node.args[2] & 0xffff;
                let lane = (node.args[2] >> 16) as u8;
                let (dst, vector) = (fpr(output).0, fpr(a).0);
                match sub_opcode {
                    I8X16_REPLACE_LANE => self.asm.vex(VPINSRB, dst, vector, register(b), Some(lane)),
                    I16X8_REPLACE_LANE => self.asm.vex(VPINSRW, dst, vector, register(b), Some(lane)),
                    I32X4_REPLACE_LANE => self.asm.vex(VPINSRD, dst, vector, register(b), Some(lane)),
                    I64X2_REPLACE_LANE => self.asm.vex(VPINSRD.w1(), dst, vector, register(b), Some(lane)),
                    F32X4_REPLACE_LANE => self.asm.vex(VINSERTPS, dst, vector, register(b), Some(lane << 4)),
                    _ => {
                        let opcode = if lane == 0 { VMOVSD_MERGE } else { VMOVLHPS };
                        self.vector_op(opcode, dst, vector, register(b));
                    }
                }
            }
            Op::Shuffle => {
                let lanes = func.constants[node.args[2] as usize].to_le_bytes();
                let (mut from_a, mut from_b) = ([0x80u8; 16], [0x80u8; 16]);
                for (position, lane) in lanes.iter().enumerate() {
                    if *lane < 16 {
                        from_a[position] = *lane;
                    } else {
                        from_b[position] = lane - 16;
                    }
                }
                let (dst, a, b) = (fpr(output).0, fpr(a).0, fpr(b).0);
                let temp = fpr(record.temps[0]).0;
                let label_a = self.vector_label(u128::from_le_bytes(from_a));
                let label_b = self.vector_label(u128::from_le_bytes(from_b));
                self.asm.vex(VPSHUFB, temp, a, VexOperand::Label(label_a), None);
                self.asm.vex(VPSHUFB, dst, b, VexOperand::Label(label_b), None);
                self.vector_op(VPOR, dst, dst, VexOperand::Register(temp));
            }
            _ => unreachable!("{} is no vector op", node.op.name()),
        }
    }

    fn vector_unary(&mut self, sub_opcode: u32, ty: Type, output: Register, a: Register, temp: Register) {
        match sub_opcode {
            I8X16_SPLAT | I16X8_SPLAT | I32X4_SPLAT | I64X2_SPLAT => {
                let dst = fpr(output).0;
                let width = if sub_opcode == I64X2_SPLAT {
                    Width::W64
                } else {
                    Width::W32
                };
                self.asm.vmov_xr(width, fpr(output), gpr(a));
                let broadcast = match sub_opcode {
                    I8X16_SPLAT => VPBROADCASTB,
                    I16X8_SPLAT => VPBROADCASTW,
                    I32X4_SPLAT => VPBROADCASTD,
                    _ => VPBROADCASTQ,
                };
                self.asm.vex(broadcast, dst, 0, VexOperand::Register(dst), None);
            }
            F32X4_SPLAT => self.asm.vex(VBROADCASTSS, fpr(output).0, 0, register(a), None),
            F64X2_SPLAT => self.asm.vex(VMOVDDUP, fpr(output).0, 0, register(a), None),
            V128_NOT => {
                self.scratch_ones();
                self.vector_op(VPXOR, fpr(output).0, fpr(a).0, VexOperand::Register(SCRATCH_XMM));
            }
            I8X16_ABS | I16X8_ABS | I32X4_ABS => {
                let opcode = match sub_opcode {
                    I8X16_ABS => op66_38(0x1c),
                    I16X8_ABS => op66_38(0x1d),
                    _ => op66_38(0x1e),
                };
                self.asm.vex(opcode, fpr(output).0, 0, register(a), None);
            }
            I8X16_NEG | I16X8_NEG | I32X4_NEG | I64X2_NEG => {
                let subtract = match sub_opcode {
                    I8X16_NEG => VPSUBB,
                    I16X8_NEG => VPSUBW,
                    I32X4_NEG => VPSUBD,
                    _ => VPSUBQ,
                };
                let dst = fpr(output).0;
                self.vector_op(VPXOR, dst, dst, VexOperand::Register(dst));
                self.vector_op(subtract, dst, dst, register(a));
            }
            F32X4_ABS | F32X4_NEG | F64X2_ABS | F64X2_NEG => {
                let sign = if matches!(sub_opcode, F32X4_ABS | F32X4_NEG) {
                    0x8000_0000_8000_0000_8000_0000_8000_0000u128
                } else {
                    0x8000_0000_0000_0000_8000_0000_0000_0000u128
                };
                let label = self.vector_label(sign);
                let negate = matches!(sub_opcode, F32X4_NEG | F64X2_NEG);
                if negate {
                    self.asm
                        .vex(VXORPS, fpr(output).0, fpr(a).0, VexOperand::Label(label), None);
                } else {
                    self.asm
                        .vex(VMOVUPS_LOAD, SCRATCH_XMM, 0, VexOperand::Label(label), None);
                    self.vector_op(VANDNPS, fpr(output).0, SCRATCH_XMM, register(a));
                }
            }
            F32X4_SQRT => self.asm.vex(
                op(VexPrefix::None, VexMap::Map0F, 0x51),
                fpr(output).0,
                0,
                register(a),
                None,
            ),
            F64X2_SQRT => self.asm.vex(op66(0x51), fpr(output).0, 0, register(a), None),
            F32X4_CONVERT_I32X4_S => {
                self.asm.vex(
                    op(VexPrefix::None, VexMap::Map0F, 0x5b),
                    fpr(output).0,
                    0,
                    register(a),
                    None,
                );
            }
            V128_ANY_TRUE => {
                let dst = gpr(output);
                self.asm.xor_rr(Width::W32, dst, dst);
                self.asm.vex(VPTEST, fpr(a).0, 0, register(a), None);
                self.asm.setcc(Cond::NotEqual, dst);
            }
            I8X16_ALL_TRUE | I16X8_ALL_TRUE | I32X4_ALL_TRUE | I64X2_ALL_TRUE => {
                // All lanes are nonzero where no lane equals zero.
                let compare = match sub_opcode {
                    I8X16_ALL_TRUE => op66(0x74),
                    I16X8_ALL_TRUE => op66(0x75),
                    I32X4_ALL_TRUE => op66(0x76),
                    _ => op66_38(0x29),
                };
                let (dst, temp) = (gpr(output), fpr(temp).0);
                self.vector_op(VPXOR, temp, temp, VexOperand::Register(temp));
                self.vector_op(compare, temp, temp, register(a));
                self.asm.xor_rr(Width::W32, dst, dst);
                self.asm.vex(VPTEST, temp, 0, VexOperand::Register(temp), None);
                self.asm.setcc(Cond::Equal, dst);
            }
            I8X16_BITMASK => self.asm.vex(VPMOVMSKB, gpr(output).0, 0, register(a), None),
            I32X4_BITMASK => self.asm.vex(VMOVMSKPS, gpr(output).0, 0, register(a), None),
            I64X2_BITMASK => self.asm.vex(VMOVMSKPD, gpr(output).0, 0, register(a), None),
            _ => unreachable!("the builder does not make SIMD sub-opcode {sub_opcode:#x} a unary op of {ty:?}"),
        }
    }

    fn vector_binary(&mut self, sub_opcode: u32, output: Register, a: Register, b: Register) {
        let dst = fpr(output).0;
        if let Some((opcode, bits)) = shift(sub_opcode) {
            // The count is taken modulo the lane width.
            self.asm.mov_rr(Width::W32, SCRATCH, gpr(b));
            self.asm.and_ri(Width::W32, SCRATCH, bits as i32 - 1);
            self.asm.vmov_xr(Width::W32, SCRATCH_FPR, SCRATCH);
            self.vector_op(opcode, dst, fpr(a).0, VexOperand::Register(SCRATCH_XMM));
            return;
        }
        if sub_opcode == I8X16_SWIZZLE {
            // Indices of 16 and up select zero: adding 0x70 with saturation sets their top bit, which vpshufb zeroes.
            let label = self.vector_label(0x7070_7070_7070_7070_7070_7070_7070_7070);
            self.asm
                .vex(VPADDUSB, SCRATCH_XMM, fpr(b).0, VexOperand::Label(label), None);
            self.vector_op(VPSHUFB, dst, fpr(a).0, VexOperand::Register(SCRATCH_XMM));
            return;
        }
        let binary = binary(sub_opcode).expect("the builder only makes the binary ops code generation knows");
        let (lhs, rhs) = if binary.swap {
            (fpr(b).0, fpr(a).0)
        } else {
            (fpr(a).0, fpr(b).0)
        };
        self.asm
            .vex(binary.opcode, dst, lhs, VexOperand::Register(rhs), binary.immediate);
        if binary.invert {
            self.scratch_ones();
            self.vector_op(VPXOR, dst, dst, VexOperand::Register(SCRATCH_XMM));
        }
    }

    fn extract_lane(&mut self, sub_opcode: u32, lane: u8, output: Register, vector: Register) {
        let source = fpr(vector).0;
        match sub_opcode {
            I8X16_EXTRACT_LANE_S | I8X16_EXTRACT_LANE_U => {
                self.asm.vex(VPEXTRB, source, 0, register(output), Some(lane));
                if sub_opcode == I8X16_EXTRACT_LANE_S {
                    self.asm.movsx8_rr(Width::W32, gpr(output), gpr(output));
                }
            }
            I16X8_EXTRACT_LANE_S | I16X8_EXTRACT_LANE_U => {
                self.asm.vex(VPEXTRW, source, 0, register(output), Some(lane));
                if sub_opcode == I16X8_EXTRACT_LANE_S {
                    self.asm.movsx16_rr(Width::W32, gpr(output), gpr(output));
                }
            }
            I32X4_EXTRACT_LANE => self.asm.vex(VPEXTRD, source, 0, register(output), Some(lane)),
            I64X2_EXTRACT_LANE => self.asm.vex(VPEXTRD.w1(), source, 0, register(output), Some(lane)),
            F32X4_EXTRACT_LANE => self.asm.vex(VPERMILPS, fpr(output).0, 0, register(vector), Some(lane)),
            _ => self.asm.vex(VPERMILPD, fpr(output).0, 0, register(vector), Some(lane)),
        }
    }

    /// Stores the 16 bytes of the vector in `register` at `address`.
    pub(super) fn store_vector(&mut self, register: Register, address: &Address) {
        self.asm
            .vex(VMOVUPS_STORE, fpr(register).0, 0, VexOperand::Memory(*address), None);
    }

    /// Loads 16 bytes at `address` into the xmm register `register` (an encoding).
    pub(super) fn load_vector(&mut self, register: u8, address: &Address) {
        self.asm
            .vex(VMOVUPS_LOAD, register, 0, VexOperand::Memory(*address), None);
    }
}
