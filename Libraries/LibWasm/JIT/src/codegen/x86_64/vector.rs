/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Vectors: v128 loads, constants, lanes and SIMD arithmetic on xmm registers. The assembler selects AVX2 or
//! equivalent SSE sequences for the host CPU.
//!
//! The allocator gives a vector node an output register apart from its inputs and temps, so a lowering may write
//! its output before it has read all inputs. Inputs may share a register (`i32x4.add x x`), and are never written.
//! Where the interpreter's sequence decides bits the spec leaves open (NaN payloads, relaxed instructions), the
//! lowering is the same sequence, so that both tiers agree.

use super::{CodeGenerator, SCRATCH, SCRATCH_FPR, fpr, gpr};
use crate::asm::x86_64::{Cond, VexMap, VexOpcode, VexOperand, VexPrefix};
use crate::asm::{Address, Width};
use crate::convention::Register;
use crate::ir::{Func, Op, Type, Value};
use crate::regalloc::{CONSTANT_INPUT, SLOT_INPUT};
use libwasm_module::opcode::simd::*;

const fn op(prefix: VexPrefix, map: VexMap, opcode: u8) -> VexOpcode {
    VexOpcode::new(prefix, map, opcode)
}

const fn op0f(opcode: u8) -> VexOpcode {
    op(VexPrefix::None, VexMap::Map0F, opcode)
}

const fn op66(opcode: u8) -> VexOpcode {
    op(VexPrefix::OperandSize, VexMap::Map0F, opcode)
}

const fn opf3(opcode: u8) -> VexOpcode {
    op(VexPrefix::Rep, VexMap::Map0F, opcode)
}

const fn op66_38(opcode: u8) -> VexOpcode {
    op(VexPrefix::OperandSize, VexMap::Map0F38, opcode)
}

const fn op66_3a(opcode: u8) -> VexOpcode {
    op(VexPrefix::OperandSize, VexMap::Map0F3A, opcode)
}

pub(super) const VMOVUPS_LOAD: VexOpcode = op0f(0x10);
pub(super) const VMOVUPS_STORE: VexOpcode = op0f(0x11);
const VMOVD_LOAD: VexOpcode = op66(0x6e);
const VMOVQ_LOAD: VexOpcode = opf3(0x7e);
const VPXOR: VexOpcode = op66(0xef);
const VPAND: VexOpcode = op66(0xdb);
const VPANDN: VexOpcode = op66(0xdf);
const VPOR: VexOpcode = op66(0xeb);
const VPCMPEQB: VexOpcode = op66(0x74);
const VPCMPEQW: VexOpcode = op66(0x75);
const VPCMPEQD: VexOpcode = op66(0x76);
const VPCMPEQQ: VexOpcode = op66_38(0x29);
const VPCMPGTQ: VexOpcode = op66_38(0x37);
const VPTEST: VexOpcode = op66_38(0x17);
const VPSHUFB: VexOpcode = op66_38(0x00);
const VPSHUFD: VexOpcode = op66(0x70);
const VPADDUSB: VexOpcode = op66(0xdc);
const VPADDB: VexOpcode = op66(0xfc);
const VPADDD: VexOpcode = op66(0xfe);
const VPADDQ: VexOpcode = op66(0xd4);
const VPSUBB: VexOpcode = op66(0xf8);
const VPSUBW: VexOpcode = op66(0xf9);
const VPSUBD: VexOpcode = op66(0xfa);
const VPSUBQ: VexOpcode = op66(0xfb);
const VPMULLW: VexOpcode = op66(0xd5);
const VPMULUDQ: VexOpcode = op66(0xf4);
const VPMADDWD: VexOpcode = op66(0xf5);
const VPMADDUBSW: VexOpcode = op66_38(0x04);
const VPMAXSD: VexOpcode = op66_38(0x3d);
const VPACKSSWB: VexOpcode = op66(0x63);
const VPUNPCKLBW: VexOpcode = op66(0x60);
const VPUNPCKHBW: VexOpcode = op66(0x68);
const VPUNPCKLWD: VexOpcode = op66(0x61);
const VPUNPCKHWD: VexOpcode = op66(0x69);
const VPBLENDW: VexOpcode = op66_3a(0x0e);
const VPBLENDD: VexOpcode = op66_3a(0x02);
const VPALIGNR: VexOpcode = op66_3a(0x0f);
const VPUNPCKLDQ: VexOpcode = op66(0x62);
const VPUNPCKHDQ: VexOpcode = op66(0x6a);
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
const VMOVLHPS: VexOpcode = op0f(0x16);
const VPMOVMSKB: VexOpcode = op66(0xd7);
const VMOVMSKPS: VexOpcode = op0f(0x50);
const VMOVMSKPD: VexOpcode = op66(0x50);
const VANDNPS: VexOpcode = op0f(0x55);
const VXORPS: VexOpcode = op0f(0x57);
const VSHUFPS: VexOpcode = op0f(0xc6);
const VUNPCKLPS: VexOpcode = op0f(0x14);
const VCVTDQ2PS: VexOpcode = op0f(0x5b);
const VCVTTPS2DQ: VexOpcode = opf3(0x5b);

/// The packed float instructions of one precision.
struct Packed {
    add: VexOpcode,
    sub: VexOpcode,
    mul: VexOpcode,
    min: VexOpcode,
    max: VexOpcode,
    and: VexOpcode,
    andn: VexOpcode,
    or: VexOpcode,
    xor: VexOpcode,
    cmp: VexOpcode,
    round: VexOpcode,
    /// The logical right shift by an immediate of lanes of this width: the group opcode and its ModRM.reg digit.
    shift_right: (VexOpcode, u8),
    /// The shift that turns an all-ones lane into the bits a NaN lane keeps of it: 10 or 13, so that what stays set
    /// is the sign, the exponent and the quiet bit.
    nan_payload_shift: u8,
}

const PS: Packed = Packed {
    add: op0f(0x58),
    sub: op0f(0x5c),
    mul: op0f(0x59),
    min: op0f(0x5d),
    max: op0f(0x5f),
    and: op0f(0x54),
    andn: op0f(0x55),
    or: op0f(0x56),
    xor: op0f(0x57),
    cmp: op0f(0xc2),
    round: op66_3a(0x08),
    shift_right: (op66(0x72), 2),
    nan_payload_shift: 10,
};

const PD: Packed = Packed {
    add: op66(0x58),
    sub: op66(0x5c),
    mul: op66(0x59),
    min: op66(0x5d),
    max: op66(0x5f),
    and: op66(0x54),
    andn: op66(0x55),
    or: op66(0x56),
    xor: op66(0x57),
    cmp: op66(0xc2),
    round: op66_3a(0x09),
    shift_right: (op66(0x73), 2),
    nan_payload_shift: 13,
};

/// `vcmpps`/`vcmppd` predicates.
const CMP_EQ: u8 = 0;
const CMP_LE: u8 = 2;
const CMP_UNORD: u8 = 3;

const fn splat8(byte: u8) -> u128 {
    u128::from_le_bytes([byte; 16])
}

const fn splat16(lane: u16) -> u128 {
    let lane = lane as u128;
    lane * 0x0001_0001_0001_0001_0001_0001_0001_0001
}

const fn splat32(lane: u32) -> u128 {
    let lane = lane as u128;
    lane * 0x0000_0001_0000_0001_0000_0001_0000_0001
}

const fn splat64(lane: u64) -> u128 {
    let lane = lane as u128;
    lane | (lane << 64)
}

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
    let inverted = |opcode, swap| Binary {
        opcode,
        swap,
        invert: true,
        immediate: None,
    };
    let compare = |opcode, swap, immediate| Binary {
        opcode,
        swap,
        invert: false,
        immediate: Some(immediate),
    };
    let ps = PS.cmp;
    let pd = PD.cmp;
    Some(match sub_opcode {
        V128_AND => plain(VPAND),
        V128_OR => plain(VPOR),
        V128_XOR => plain(VPXOR),
        // NB: andnot is a & !b, vpandn computes !lhs & rhs.
        V128_ANDNOT => swapped(VPANDN),
        I8X16_EQ => plain(VPCMPEQB),
        I16X8_EQ => plain(VPCMPEQW),
        I32X4_EQ => plain(VPCMPEQD),
        I64X2_EQ => plain(VPCMPEQQ),
        I8X16_NE => inverted(VPCMPEQB, false),
        I16X8_NE => inverted(VPCMPEQW, false),
        I32X4_NE => inverted(VPCMPEQD, false),
        I64X2_NE => inverted(VPCMPEQQ, false),
        I8X16_GT_S => plain(op66(0x64)),
        I16X8_GT_S => plain(op66(0x65)),
        I32X4_GT_S => plain(op66(0x66)),
        I64X2_GT_S => plain(VPCMPGTQ),
        I8X16_LT_S => swapped(op66(0x64)),
        I16X8_LT_S => swapped(op66(0x65)),
        I32X4_LT_S => swapped(op66(0x66)),
        I64X2_LT_S => swapped(VPCMPGTQ),
        I64X2_LE_S => inverted(VPCMPGTQ, false),
        I64X2_GE_S => inverted(VPCMPGTQ, true),
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
        I8X16_ADD => plain(VPADDB),
        I16X8_ADD => plain(op66(0xfd)),
        I32X4_ADD => plain(VPADDD),
        I64X2_ADD => plain(VPADDQ),
        I8X16_SUB => plain(VPSUBB),
        I16X8_SUB => plain(VPSUBW),
        I32X4_SUB => plain(VPSUBD),
        I64X2_SUB => plain(VPSUBQ),
        I8X16_ADD_SAT_S => plain(op66(0xec)),
        I8X16_ADD_SAT_U => plain(VPADDUSB),
        I8X16_SUB_SAT_S => plain(op66(0xe8)),
        I8X16_SUB_SAT_U => plain(op66(0xd8)),
        I16X8_ADD_SAT_S => plain(op66(0xed)),
        I16X8_ADD_SAT_U => plain(op66(0xdd)),
        I16X8_SUB_SAT_S => plain(op66(0xe9)),
        I16X8_SUB_SAT_U => plain(op66(0xd9)),
        I8X16_AVGR_U => plain(op66(0xe0)),
        I16X8_AVGR_U => plain(op66(0xe3)),
        I16X8_MUL => plain(VPMULLW),
        I32X4_MUL => plain(op66_38(0x40)),
        I32X4_DOT_I16X8_S => plain(VPMADDWD),
        // NB: vpmaddubsw multiplies the unsigned bytes of its first source with the signed bytes of its second.
        I16X8_RELAXED_DOT_I8X16_I7X16_S => swapped(VPMADDUBSW),
        I8X16_NARROW_I16X8_S => plain(VPACKSSWB),
        I8X16_NARROW_I16X8_U => plain(op66(0x67)),
        I16X8_NARROW_I32X4_S => plain(op66(0x6b)),
        I16X8_NARROW_I32X4_U => plain(op66_38(0x2b)),
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
        I32X4_MAX_S => plain(VPMAXSD),
        I32X4_MAX_U => plain(op66_38(0x3f)),
        F32X4_ADD => plain(PS.add),
        F32X4_SUB => plain(PS.sub),
        F32X4_MUL => plain(PS.mul),
        F32X4_DIV => plain(op0f(0x5e)),
        F64X2_ADD => plain(PD.add),
        F64X2_SUB => plain(PD.sub),
        F64X2_MUL => plain(PD.mul),
        F64X2_DIV => plain(op66(0x5e)),
        // pmin is b < a ? b : a, which minps computes with b first.
        F32X4_PMIN => swapped(PS.min),
        F32X4_PMAX => swapped(PS.max),
        F64X2_PMIN => swapped(PD.min),
        F64X2_PMAX => swapped(PD.max),
        _ => return None,
    })
}

/// The 32-bit lanes a shuffle picks, if it moves whole ones: 0 to 3 from the first operand, 4 to 7 from the second.
fn dword_lanes(lanes: &[u8; 16]) -> Option<[u8; 4]> {
    let mut dwords = [0; 4];
    for (index, group) in lanes.as_chunks::<4>().0.iter().enumerate() {
        if group[0] % 4 != 0 || (1..4).any(|byte| group[byte] != group[0] + byte as u8) {
            return None;
        }
        dwords[index] = group[0] / 4;
    }
    Some(dwords)
}

/// The unsigned or signed comparisons that compare a lane with the minimum or maximum of both: the min/max
/// instruction, the equality comparison, and whether the result is inverted.
fn compare_with_bound(sub_opcode: u32) -> Option<(VexOpcode, VexOpcode, bool)> {
    let (min_u8, max_u8, min_s8, max_s8) = (op66(0xda), op66(0xde), op66_38(0x38), op66_38(0x3c));
    let (min_u16, max_u16, min_s16, max_s16) = (op66_38(0x3a), op66_38(0x3e), op66(0xea), op66(0xee));
    let (min_u32, max_u32, min_s32, max_s32) = (op66_38(0x3b), op66_38(0x3f), op66_38(0x39), VPMAXSD);
    // a <= b where min(a, b) == a, a >= b where max(a, b) == a, and the strict ones are the others inverted.
    Some(match sub_opcode {
        I8X16_LT_U => (max_u8, VPCMPEQB, true),
        I8X16_GT_U => (min_u8, VPCMPEQB, true),
        I8X16_LE_U => (min_u8, VPCMPEQB, false),
        I8X16_GE_U => (max_u8, VPCMPEQB, false),
        I8X16_LE_S => (min_s8, VPCMPEQB, false),
        I8X16_GE_S => (max_s8, VPCMPEQB, false),
        I16X8_LT_U => (max_u16, VPCMPEQW, true),
        I16X8_GT_U => (min_u16, VPCMPEQW, true),
        I16X8_LE_U => (min_u16, VPCMPEQW, false),
        I16X8_GE_U => (max_u16, VPCMPEQW, false),
        I16X8_LE_S => (min_s16, VPCMPEQW, false),
        I16X8_GE_S => (max_s16, VPCMPEQW, false),
        I32X4_LT_U => (max_u32, VPCMPEQD, true),
        I32X4_GT_U => (min_u32, VPCMPEQD, true),
        I32X4_LE_U => (min_u32, VPCMPEQD, false),
        I32X4_GE_U => (max_u32, VPCMPEQD, false),
        I32X4_LE_S => (min_s32, VPCMPEQD, false),
        I32X4_GE_S => (max_s32, VPCMPEQD, false),
        _ => return None,
    })
}

/// A shift of 16-, 32- or 64-bit lanes: the shift by an xmm count, the group opcode and digit of the shift by an
/// immediate, and the bits of the lanes.
fn shift(sub_opcode: u32) -> Option<(VexOpcode, (VexOpcode, u8), u32)> {
    let (words, dwords, qwords) = (op66(0x71), op66(0x72), op66(0x73));
    Some(match sub_opcode {
        I16X8_SHL => (op66(0xf1), (words, 6), 16),
        I16X8_SHR_S => (op66(0xe1), (words, 4), 16),
        I16X8_SHR_U => (op66(0xd1), (words, 2), 16),
        I32X4_SHL => (op66(0xf2), (dwords, 6), 32),
        I32X4_SHR_S => (op66(0xe2), (dwords, 4), 32),
        I32X4_SHR_U => (op66(0xd2), (dwords, 2), 32),
        I64X2_SHL => (op66(0xf3), (qwords, 6), 64),
        I64X2_SHR_U => (op66(0xd3), (qwords, 2), 64),
        _ => return None,
    })
}

const VPSLLW: VexOpcode = op66(0xf1);
const VPSRLW: VexOpcode = op66(0xd1);
const VPSRAW: VexOpcode = op66(0xe1);
const VPSRLQ: VexOpcode = op66(0xd3);
const SHIFT_WORDS: VexOpcode = op66(0x71);
const SHIFT_DWORDS: VexOpcode = op66(0x72);
const SHIFT_QWORDS: VexOpcode = op66(0x73);

/// The sub-opcode of a ternary SIMD operation, from the flags of its `VectorTernary` node: its low byte.
fn ternary_sub_opcode(flags: u8) -> u32 {
    if u32::from(flags) == V128_BITSELECT {
        V128_BITSELECT
    } else {
        0x100 | u32::from(flags)
    }
}

/// Whether a SIMD binary operation is a shift by an i32, whose count may be a constant.
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

/// The inputs of vector node `value` its lowering may read from the constant pool where they are v128 constants, as
/// a mask: those that are the r/m operand of an instruction and read only there.
pub(crate) fn vector_constant_inputs(func: &Func, value: Value) -> u8 {
    let node = func.node(value);
    match node.op {
        Op::VectorBinary => match binary(node.args[2]) {
            // NB: The encoder has no rip-relative operands before an immediate.
            Some(binary) if binary.immediate.is_some() => 0,
            Some(binary) if binary.swap => 0b001,
            Some(_) => 0b010,
            None if compare_with_bound(node.args[2]).is_some() => 0b010,
            None => 0,
        },
        Op::VectorTernary => match ternary_sub_opcode(node.flags) {
            F32X4_RELAXED_MADD | F64X2_RELAXED_MADD => 0b110,
            F32X4_RELAXED_NMADD | F64X2_RELAXED_NMADD => 0b010,
            I32X4_RELAXED_DOT_I8X16_I7X16_ADD_S => 0b101,
            _ => 0,
        },
        _ => 0,
    }
}

/// The number of xmm temps the lowering of vector node `value` takes, besides the scratch register.
pub(crate) fn vector_temps(func: &Func, value: Value) -> usize {
    let node = func.node(value);
    match node.op {
        // NB: Only shuffles of two vectors that no single instruction does need one.
        Op::Shuffle => usize::from(node.args[0] != node.args[1]),
        Op::VectorTernary => usize::from(ternary_sub_opcode(node.flags) == V128_BITSELECT),
        Op::VectorUnary => usize::from(matches!(
            node.args[1],
            I8X16_ALL_TRUE | I16X8_ALL_TRUE | I32X4_ALL_TRUE | I64X2_ALL_TRUE | I8X16_POPCNT | I32X4_TRUNC_SAT_F32X4_U
        )),
        Op::VectorBinary => usize::from(matches!(
            node.args[2],
            I64X2_MUL | I8X16_SHL | I8X16_SHR_S | I8X16_SHR_U | I64X2_SHR_S
        )),
        _ => 0,
    }
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
    /// Input `index` of vector node `value` as an operand: its register, or the constant pool entry of a v128 constant
    /// the allocator left where it is.
    fn vector_input(&mut self, value: Value, index: usize, register: Register) -> VexOperand {
        let func = self.func;
        if register == SLOT_INPUT {
            return VexOperand::Memory(self.input_slot(func.inputs(value)[index]));
        }
        if register != CONSTANT_INPUT {
            return VexOperand::Register(fpr(register).0);
        }
        let constant = func.node(func.inputs(value)[index]);
        debug_assert_eq!(constant.op, Op::Const128);
        VexOperand::Label(self.constant_label(func.constants[constant.args[0] as usize]))
    }

    /// `op dst, lhs, rhs` on xmm registers by their encoding.
    fn vector_op(&mut self, opcode: VexOpcode, dst: u8, lhs: u8, rhs: u8) {
        self.asm.vex(opcode, dst, lhs, VexOperand::Register(rhs), None);
    }

    /// `op dst, lhs, [constant]`.
    fn vector_op_constant(&mut self, opcode: VexOpcode, dst: u8, lhs: u8, bits: u128) {
        let label = self.constant_label(bits);
        self.asm.vex(opcode, dst, lhs, VexOperand::Label(label), None);
    }

    /// `op dst, src` (and an immediate) of the instructions without a second source.
    fn vector_unary_op(&mut self, opcode: VexOpcode, dst: u8, src: u8, immediate: Option<u8>) {
        self.asm.vex(opcode, dst, 0, VexOperand::Register(src), immediate);
    }

    /// A shift by an immediate: `group` is the opcode of the lane width, `digit` picks the shift.
    fn vector_shift_immediate(&mut self, (group, digit): (VexOpcode, u8), dst: u8, src: u8, count: u8) {
        self.asm.vex(group, digit, dst, VexOperand::Register(src), Some(count));
    }

    /// Loads a 16-byte constant into the xmm register `dst`.
    fn vector_load_constant(&mut self, dst: u8, bits: u128) {
        let label = self.constant_label(bits);
        self.asm.vex(VMOVUPS_LOAD, dst, 0, VexOperand::Label(label), None);
    }

    /// Fills the xmm register `dst` with ones.
    fn vector_ones(&mut self, dst: u8) {
        self.vector_op(VPCMPEQD, dst, dst, dst);
    }

    fn vector_zero(&mut self, dst: u8) {
        self.vector_op(VPXOR, dst, dst, dst);
    }

    /// Inverts the bits of the xmm register `dst`, through the scratch register.
    fn vector_invert(&mut self, dst: u8) {
        self.vector_ones(SCRATCH_XMM);
        self.vector_op(VPXOR, dst, dst, SCRATCH_XMM);
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
        let temp = record.temps[0];
        match node.op {
            Op::VectorUnary => self.vector_unary(node.args[1], node.ty, output, a, temp),
            Op::VectorBinary => self.vector_binary(value, node.args[2], output, a, b, temp),
            Op::VectorTernary => {
                self.vector_ternary(value, ternary_sub_opcode(node.flags), output, [a, b, c], temp);
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
                        self.asm.vex(opcode, dst, vector, register(b), None);
                    }
                }
            }
            Op::Shuffle => {
                let single = node.args[0] == node.args[1];
                let mut lanes = func.constants[node.args[2] as usize].to_le_bytes();
                if single {
                    // NB: The builder makes the lanes of a shuffle of one vector 0 to 15, but its operands may have
                    //     become the same value since; the patterns below take lanes of one vector.
                    lanes = lanes.map(|lane| lane & 15);
                }
                let temp = if temp == crate::regalloc::NO_REGISTER {
                    0
                } else {
                    fpr(temp).0
                };
                self.shuffle(lanes, single, fpr(output).0, fpr(a).0, fpr(b).0, temp);
            }
            _ => unreachable!("{} is no vector op", node.op.name()),
        }
    }

    /// `i8x16.shuffle`, with the lanes of `a` and `b` (0 to 31). The builder made `a` and `b` the same value where
    /// only one of them is used, and its lanes 0 to 15.
    fn shuffle(&mut self, lanes: [u8; 16], single: bool, dst: u8, a: u8, b: u8, temp: u8) {
        let dwords = dword_lanes(&lanes);
        if single {
            if let Some(d) = dwords {
                let immediate = d[0] | (d[1] << 2) | (d[2] << 4) | (d[3] << 6);
                self.vector_unary_op(VPSHUFD, dst, a, Some(immediate));
                return;
            }
            if let Some(n) = (1..16u8).find(|&n| lanes.iter().enumerate().all(|(i, &lane)| lane == (i as u8 + n) & 15))
            {
                self.asm.vex(VPALIGNR, dst, a, VexOperand::Register(a), Some(n));
                return;
            }
            self.vector_op_constant(VPSHUFB, dst, a, u128::from_le_bytes(lanes));
            return;
        }
        if let Some(d) = dwords {
            match d {
                [0, 4, 1, 5] => return self.vector_op(VPUNPCKLDQ, dst, a, b),
                [2, 6, 3, 7] => return self.vector_op(VPUNPCKHDQ, dst, a, b),
                [4, 0, 5, 1] => return self.vector_op(VPUNPCKLDQ, dst, b, a),
                [6, 2, 7, 3] => return self.vector_op(VPUNPCKHDQ, dst, b, a),
                _ => {}
            }
            let immediate = |d: [u8; 4]| (d[0] & 3) | ((d[1] & 3) << 2) | ((d[2] & 3) << 4) | ((d[3] & 3) << 6);
            if d[0] < 4 && d[1] < 4 && d[2] >= 4 && d[3] >= 4 {
                self.asm
                    .vex(VSHUFPS, dst, a, VexOperand::Register(b), Some(immediate(d)));
                return;
            }
            if d[0] >= 4 && d[1] >= 4 && d[2] < 4 && d[3] < 4 {
                self.asm
                    .vex(VSHUFPS, dst, b, VexOperand::Register(a), Some(immediate(d)));
                return;
            }
            if d.iter().enumerate().all(|(i, &lane)| lane & 3 == i as u8) {
                let mask = d
                    .iter()
                    .enumerate()
                    .fold(0, |mask, (i, &lane)| mask | (u8::from(lane >= 4) << i));
                self.asm.vex(VPBLENDD, dst, a, VexOperand::Register(b), Some(mask));
                return;
            }
        }
        // a and b concatenated, shifted right by n bytes, or b and a.
        for n in 1..16u8 {
            if lanes.iter().enumerate().all(|(i, &lane)| lane == i as u8 + n) {
                self.asm.vex(VPALIGNR, dst, b, VexOperand::Register(a), Some(n));
                return;
            }
            if lanes
                .iter()
                .enumerate()
                .all(|(i, &lane)| lane == (i as u8 + n + 16) & 31)
            {
                self.asm.vex(VPALIGNR, dst, a, VexOperand::Register(b), Some(n));
                return;
            }
        }
        let (mut from_a, mut from_b) = ([0x80u8; 16], [0x80u8; 16]);
        for (position, lane) in lanes.iter().enumerate() {
            if *lane < 16 {
                from_a[position] = *lane;
            } else {
                from_b[position] = lane - 16;
            }
        }
        self.vector_op_constant(VPSHUFB, temp, a, u128::from_le_bytes(from_a));
        self.vector_op_constant(VPSHUFB, dst, b, u128::from_le_bytes(from_b));
        self.vector_op(VPOR, dst, dst, temp);
    }

    fn vector_unary(&mut self, sub_opcode: u32, ty: Type, output: Register, a: Register, temp: Register) {
        if ty == Type::I32 {
            self.vector_test(sub_opcode, output, a, temp);
            return;
        }
        let dst = fpr(output).0;
        match sub_opcode {
            I8X16_SPLAT | I16X8_SPLAT | I32X4_SPLAT | I64X2_SPLAT => {
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
                self.vector_unary_op(broadcast, dst, dst, None);
            }
            F32X4_SPLAT => self.vector_unary_op(VBROADCASTSS, dst, fpr(a).0, None),
            F64X2_SPLAT => self.vector_unary_op(VMOVDDUP, dst, fpr(a).0, None),
            _ => self.vector_lanewise_unary(sub_opcode, dst, fpr(a).0, temp),
        }
    }

    /// The unary operations from a vector to a vector.
    fn vector_lanewise_unary(&mut self, sub_opcode: u32, dst: u8, a: u8, temp: Register) {
        match sub_opcode {
            V128_NOT => {
                self.vector_ones(SCRATCH_XMM);
                self.vector_op(VPXOR, dst, a, SCRATCH_XMM);
            }
            I8X16_ABS => self.vector_unary_op(op66_38(0x1c), dst, a, None),
            I16X8_ABS => self.vector_unary_op(op66_38(0x1d), dst, a, None),
            I32X4_ABS => self.vector_unary_op(op66_38(0x1e), dst, a, None),
            I64X2_ABS => {
                // (a ^ sign) - sign
                self.vector_zero(SCRATCH_XMM);
                self.vector_op(VPCMPGTQ, SCRATCH_XMM, SCRATCH_XMM, a);
                self.vector_op(VPXOR, dst, a, SCRATCH_XMM);
                self.vector_op(VPSUBQ, dst, dst, SCRATCH_XMM);
            }
            I8X16_NEG | I16X8_NEG | I32X4_NEG | I64X2_NEG => {
                let subtract = match sub_opcode {
                    I8X16_NEG => VPSUBB,
                    I16X8_NEG => VPSUBW,
                    I32X4_NEG => VPSUBD,
                    _ => VPSUBQ,
                };
                self.vector_zero(dst);
                self.vector_op(subtract, dst, dst, a);
            }
            I8X16_POPCNT => {
                // The bits of each nibble, looked up in a table of 16.
                const NIBBLE_BITS: u128 = u128::from_le_bytes([0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4]);
                let table = fpr(temp).0;
                self.vector_load_constant(table, NIBBLE_BITS);
                self.vector_op_constant(VPAND, SCRATCH_XMM, a, splat8(0x0f));
                self.vector_op(VPSHUFB, SCRATCH_XMM, table, SCRATCH_XMM);
                self.vector_shift_immediate((SHIFT_WORDS, 2), dst, a, 4);
                self.vector_op_constant(VPAND, dst, dst, splat8(0x0f));
                self.vector_op(VPSHUFB, dst, table, dst);
                self.vector_op(VPADDB, dst, dst, SCRATCH_XMM);
            }
            F32X4_ABS | F32X4_NEG | F64X2_ABS | F64X2_NEG => {
                let sign = if matches!(sub_opcode, F32X4_ABS | F32X4_NEG) {
                    splat32(0x8000_0000)
                } else {
                    splat64(0x8000_0000_0000_0000)
                };
                if matches!(sub_opcode, F32X4_NEG | F64X2_NEG) {
                    self.vector_op_constant(VXORPS, dst, a, sign);
                } else {
                    self.vector_load_constant(SCRATCH_XMM, sign);
                    self.vector_op(VANDNPS, dst, SCRATCH_XMM, a);
                }
            }
            F32X4_SQRT => self.vector_unary_op(op0f(0x51), dst, a, None),
            F64X2_SQRT => self.vector_unary_op(op66(0x51), dst, a, None),
            // roundps immediates: 0 to nearest, 1 down, 2 up, 3 toward zero.
            F32X4_NEAREST => self.vector_unary_op(PS.round, dst, a, Some(0)),
            F32X4_FLOOR => self.vector_unary_op(PS.round, dst, a, Some(1)),
            F32X4_CEIL => self.vector_unary_op(PS.round, dst, a, Some(2)),
            F32X4_TRUNC => self.vector_unary_op(PS.round, dst, a, Some(3)),
            F64X2_NEAREST => self.vector_unary_op(PD.round, dst, a, Some(0)),
            F64X2_FLOOR => self.vector_unary_op(PD.round, dst, a, Some(1)),
            F64X2_CEIL => self.vector_unary_op(PD.round, dst, a, Some(2)),
            F64X2_TRUNC => self.vector_unary_op(PD.round, dst, a, Some(3)),
            I16X8_EXTEND_LOW_I8X16_S
            | I16X8_EXTEND_HIGH_I8X16_S
            | I16X8_EXTEND_LOW_I8X16_U
            | I16X8_EXTEND_HIGH_I8X16_U
            | I32X4_EXTEND_LOW_I16X8_S
            | I32X4_EXTEND_HIGH_I16X8_S
            | I32X4_EXTEND_LOW_I16X8_U
            | I32X4_EXTEND_HIGH_I16X8_U
            | I64X2_EXTEND_LOW_I32X4_S
            | I64X2_EXTEND_HIGH_I32X4_S
            | I64X2_EXTEND_LOW_I32X4_U
            | I64X2_EXTEND_HIGH_I32X4_U => {
                let (extend, high) = extension(sub_opcode);
                let source = if high {
                    // The high half moves down first.
                    self.vector_unary_op(VPSHUFD, dst, a, Some(0xee));
                    dst
                } else {
                    a
                };
                self.vector_unary_op(extend, dst, source, None);
            }
            I16X8_EXTADD_PAIRWISE_I8X16_S => {
                self.vector_load_constant(SCRATCH_XMM, splat8(1));
                self.vector_op(VPMADDUBSW, dst, SCRATCH_XMM, a);
            }
            I16X8_EXTADD_PAIRWISE_I8X16_U => self.vector_op_constant(VPMADDUBSW, dst, a, splat8(1)),
            I32X4_EXTADD_PAIRWISE_I16X8_S => self.vector_op_constant(VPMADDWD, dst, a, splat16(1)),
            I32X4_EXTADD_PAIRWISE_I16X8_U => {
                // Biased to signed lanes, added in pairs, and the bias of two lanes added back.
                self.vector_op_constant(VPXOR, dst, a, splat16(0x8000));
                self.vector_op_constant(VPMADDWD, dst, dst, splat16(1));
                self.vector_op_constant(VPADDD, dst, dst, splat32(0x10000));
            }
            F32X4_CONVERT_I32X4_S => self.vector_unary_op(VCVTDQ2PS, dst, a, None),
            F32X4_CONVERT_I32X4_U => {
                // The low 16 bits and the rest convert exactly on their own; the rest halved, so that it is
                // positive, and doubled again. Adding both rounds once.
                self.vector_zero(SCRATCH_XMM);
                self.asm
                    .vex(VPBLENDW, SCRATCH_XMM, SCRATCH_XMM, VexOperand::Register(a), Some(0x55));
                self.vector_op(VPSUBD, dst, a, SCRATCH_XMM);
                self.vector_unary_op(VCVTDQ2PS, SCRATCH_XMM, SCRATCH_XMM, None);
                self.vector_shift_immediate((SHIFT_DWORDS, 2), dst, dst, 1);
                self.vector_unary_op(VCVTDQ2PS, dst, dst, None);
                self.vector_op(PS.add, dst, dst, dst);
                self.vector_op(PS.add, dst, dst, SCRATCH_XMM);
            }
            I32X4_TRUNC_SAT_F32X4_S => {
                // NaN lanes become 0 first. cvttps2dq makes every lane out of range 0x80000000, which is right for
                // negative ones; positive ones, whose sign flipped, become 0x7fffffff.
                self.asm
                    .vex(PS.cmp, SCRATCH_XMM, a, VexOperand::Register(a), Some(CMP_EQ));
                self.vector_op(PS.and, dst, a, SCRATCH_XMM);
                self.vector_op(VPXOR, SCRATCH_XMM, SCRATCH_XMM, dst);
                self.vector_unary_op(VCVTTPS2DQ, dst, dst, None);
                self.vector_op(VPAND, SCRATCH_XMM, SCRATCH_XMM, dst);
                self.vector_shift_immediate((SHIFT_DWORDS, 4), SCRATCH_XMM, SCRATCH_XMM, 31);
                self.vector_op(VPXOR, dst, dst, SCRATCH_XMM);
            }
            I32X4_TRUNC_SAT_F32X4_U => {
                let temp = fpr(temp).0;
                // NaN and negative lanes become 0.
                self.vector_zero(temp);
                self.vector_op(PS.max, dst, a, temp);
                // 2^31 as a float, and the lanes less that: those of 2^31 and up convert as signed, those of 2^32
                // and up have all bits set, and the others are clamped to 0.
                self.vector_ones(temp);
                self.vector_shift_immediate((SHIFT_DWORDS, 2), temp, temp, 1);
                self.vector_unary_op(VCVTDQ2PS, temp, temp, None);
                self.vector_op(PS.sub, SCRATCH_XMM, dst, temp);
                self.asm
                    .vex(PS.cmp, temp, temp, VexOperand::Register(SCRATCH_XMM), Some(CMP_LE));
                self.vector_unary_op(VCVTTPS2DQ, SCRATCH_XMM, SCRATCH_XMM, None);
                self.vector_op(VPXOR, SCRATCH_XMM, SCRATCH_XMM, temp);
                self.vector_zero(temp);
                self.vector_op(VPMAXSD, SCRATCH_XMM, SCRATCH_XMM, temp);
                // The lanes below 2^31 convert as they are, the others to 0x80000000, plus what they exceed it by.
                self.vector_unary_op(VCVTTPS2DQ, dst, dst, None);
                self.vector_op(VPADDD, dst, dst, SCRATCH_XMM);
            }
            I32X4_TRUNC_SAT_F64X2_S_ZERO => {
                // NaN lanes are clamped to 0, the others to i32::MAX; cvttpd2dq makes the negative lanes out of
                // range 0x80000000.
                self.asm
                    .vex(PD.cmp, SCRATCH_XMM, a, VexOperand::Register(a), Some(CMP_EQ));
                self.vector_op_constant(PD.and, SCRATCH_XMM, SCRATCH_XMM, splat64(2_147_483_647f64.to_bits()));
                self.vector_op(PD.min, dst, a, SCRATCH_XMM);
                self.vector_unary_op(op66(0xe6), dst, dst, None);
            }
            I32X4_TRUNC_SAT_F64X2_U_ZERO => {
                // Clamped to [0, u32::MAX] (NaN to 0), truncated, and the integer read from the mantissa of
                // 2^52 + lane.
                self.vector_zero(SCRATCH_XMM);
                self.vector_op(PD.max, dst, a, SCRATCH_XMM);
                self.vector_op_constant(PD.min, dst, dst, splat64(4_294_967_295f64.to_bits()));
                self.vector_unary_op(PD.round, dst, dst, Some(3));
                self.vector_op_constant(PD.add, dst, dst, splat64(4_503_599_627_370_496f64.to_bits()));
                self.asm
                    .vex(VSHUFPS, dst, dst, VexOperand::Register(SCRATCH_XMM), Some(0x88));
            }
            F64X2_CONVERT_LOW_I32X4_S => self.vector_unary_op(opf3(0xe6), dst, a, None),
            F64X2_CONVERT_LOW_I32X4_U => {
                // Each lane as the low bits of the mantissa of 2^52, less 2^52.
                self.vector_op_constant(VUNPCKLPS, dst, a, splat32(0x4330_0000));
                self.vector_op_constant(PD.sub, dst, dst, splat64(4_503_599_627_370_496f64.to_bits()));
            }
            F32X4_DEMOTE_F64X2_ZERO => self.vector_unary_op(op66(0x5a), dst, a, None),
            F64X2_PROMOTE_LOW_F32X4 => self.vector_unary_op(op0f(0x5a), dst, a, None),
            _ => unreachable!("SIMD sub-opcode {sub_opcode:#x} is no unary vector operation"),
        }
    }

    /// The reductions of a vector to an i32.
    fn vector_test(&mut self, sub_opcode: u32, output: Register, a: Register, temp: Register) {
        let dst = gpr(output);
        match sub_opcode {
            V128_ANY_TRUE => {
                self.asm.xor_rr(Width::W32, dst, dst);
                self.asm.vex(VPTEST, fpr(a).0, 0, register(a), None);
                self.asm.setcc(Cond::NotEqual, dst);
            }
            I8X16_ALL_TRUE | I16X8_ALL_TRUE | I32X4_ALL_TRUE | I64X2_ALL_TRUE => {
                // All lanes are nonzero where no lane equals zero.
                let compare = match sub_opcode {
                    I8X16_ALL_TRUE => VPCMPEQB,
                    I16X8_ALL_TRUE => VPCMPEQW,
                    I32X4_ALL_TRUE => VPCMPEQD,
                    _ => VPCMPEQQ,
                };
                let temp = fpr(temp).0;
                self.vector_zero(temp);
                self.vector_op(compare, temp, temp, fpr(a).0);
                self.asm.xor_rr(Width::W32, dst, dst);
                self.asm.vex(VPTEST, temp, 0, VexOperand::Register(temp), None);
                self.asm.setcc(Cond::Equal, dst);
            }
            I8X16_BITMASK => self.asm.vex(VPMOVMSKB, dst.0, 0, register(a), None),
            I16X8_BITMASK => {
                // Narrowing with saturation keeps the signs.
                self.vector_op(VPACKSSWB, SCRATCH_XMM, fpr(a).0, fpr(a).0);
                self.asm
                    .vex(VPMOVMSKB, dst.0, 0, VexOperand::Register(SCRATCH_XMM), None);
                self.asm.movzx8_rr(dst, dst);
            }
            I32X4_BITMASK => self.asm.vex(VMOVMSKPS, dst.0, 0, register(a), None),
            I64X2_BITMASK => self.asm.vex(VMOVMSKPD, dst.0, 0, register(a), None),
            _ => unreachable!("SIMD sub-opcode {sub_opcode:#x} is no reduction to an i32"),
        }
    }

    fn vector_binary(
        &mut self,
        value: Value,
        sub_opcode: u32,
        output: Register,
        a: Register,
        b: Register,
        temp: Register,
    ) {
        let dst = fpr(output).0;
        if is_vector_shift(sub_opcode) {
            let count = (b == CONSTANT_INPUT).then(|| self.immediate(value, 1) as u32);
            self.vector_shift(sub_opcode, dst, fpr(a).0, b, count, temp);
            return;
        }
        if let Some(binary) = binary(sub_opcode) {
            let (lhs, rhs) = if binary.swap {
                ((b, 1), (a, 0))
            } else {
                ((a, 0), (b, 1))
            };
            let rhs = self.vector_input(value, rhs.1, rhs.0);
            self.asm.vex(binary.opcode, dst, fpr(lhs.0).0, rhs, binary.immediate);
            if binary.invert {
                self.vector_invert(dst);
            }
            return;
        }
        if let Some((bound, equal, invert)) = compare_with_bound(sub_opcode) {
            let rhs = self.vector_input(value, 1, b);
            self.asm.vex(bound, SCRATCH_XMM, fpr(a).0, rhs, None);
            self.vector_op(equal, dst, SCRATCH_XMM, fpr(a).0);
            if invert {
                self.vector_invert(dst);
            }
            return;
        }
        let (a, b) = (fpr(a).0, fpr(b).0);
        match sub_opcode {
            I8X16_SWIZZLE => {
                // Indices of 16 and up select zero: adding 0x70 with saturation sets their top bit, which vpshufb
                // zeroes.
                self.vector_op_constant(VPADDUSB, SCRATCH_XMM, b, splat8(0x70));
                self.vector_op(VPSHUFB, dst, a, SCRATCH_XMM);
            }
            I64X2_MUL => {
                // The low halves' full product, plus the cross products shifted up.
                let temp = fpr(temp).0;
                self.vector_shift_immediate((SHIFT_QWORDS, 2), temp, a, 32);
                self.vector_op(VPMULUDQ, temp, temp, b);
                self.vector_shift_immediate((SHIFT_QWORDS, 2), SCRATCH_XMM, b, 32);
                self.vector_op(VPMULUDQ, SCRATCH_XMM, SCRATCH_XMM, a);
                self.vector_op(VPADDQ, temp, temp, SCRATCH_XMM);
                self.vector_shift_immediate((SHIFT_QWORDS, 6), temp, temp, 32);
                self.vector_op(VPMULUDQ, dst, a, b);
                self.vector_op(VPADDQ, dst, dst, temp);
            }
            I16X8_Q15MULR_SAT_S => {
                // The only product that overflows is -1 * -1, which gives 0x8000 and must saturate to 0x7fff.
                self.vector_op(op66_38(0x0b), dst, a, b);
                self.vector_op_constant(VPCMPEQW, SCRATCH_XMM, dst, splat16(0x8000));
                self.vector_op(VPXOR, dst, dst, SCRATCH_XMM);
            }
            I16X8_EXTMUL_LOW_I8X16_S
            | I16X8_EXTMUL_HIGH_I8X16_S
            | I16X8_EXTMUL_LOW_I8X16_U
            | I16X8_EXTMUL_HIGH_I8X16_U => {
                let (extend, high) = extension(sub_opcode);
                let (a_source, b_source) = if high {
                    self.vector_unary_op(VPSHUFD, SCRATCH_XMM, a, Some(0xee));
                    self.vector_unary_op(VPSHUFD, dst, b, Some(0xee));
                    (SCRATCH_XMM, dst)
                } else {
                    (a, b)
                };
                self.vector_unary_op(extend, SCRATCH_XMM, a_source, None);
                self.vector_unary_op(extend, dst, b_source, None);
                self.vector_op(VPMULLW, dst, dst, SCRATCH_XMM);
            }
            I32X4_EXTMUL_LOW_I16X8_S
            | I32X4_EXTMUL_HIGH_I16X8_S
            | I32X4_EXTMUL_LOW_I16X8_U
            | I32X4_EXTMUL_HIGH_I16X8_U => {
                // The low and high halves of the products, interleaved.
                let signed = matches!(sub_opcode, I32X4_EXTMUL_LOW_I16X8_S | I32X4_EXTMUL_HIGH_I16X8_S);
                let high = matches!(sub_opcode, I32X4_EXTMUL_HIGH_I16X8_S | I32X4_EXTMUL_HIGH_I16X8_U);
                self.vector_op(VPMULLW, SCRATCH_XMM, a, b);
                self.vector_op(if signed { op66(0xe5) } else { op66(0xe4) }, dst, a, b);
                self.vector_op(if high { VPUNPCKHWD } else { VPUNPCKLWD }, dst, SCRATCH_XMM, dst);
            }
            I64X2_EXTMUL_LOW_I32X4_S
            | I64X2_EXTMUL_HIGH_I32X4_S
            | I64X2_EXTMUL_LOW_I32X4_U
            | I64X2_EXTMUL_HIGH_I32X4_U => {
                // The lanes moved to the low halves of 64-bit lanes, where vpmuldq and vpmuludq multiply.
                let signed = matches!(sub_opcode, I64X2_EXTMUL_LOW_I32X4_S | I64X2_EXTMUL_HIGH_I32X4_S);
                let high = matches!(sub_opcode, I64X2_EXTMUL_HIGH_I32X4_S | I64X2_EXTMUL_HIGH_I32X4_U);
                let lanes = if high { 0xfa } else { 0x50 };
                self.vector_unary_op(VPSHUFD, SCRATCH_XMM, a, Some(lanes));
                self.vector_unary_op(VPSHUFD, dst, b, Some(lanes));
                self.vector_op(if signed { op66_38(0x28) } else { VPMULUDQ }, dst, SCRATCH_XMM, dst);
            }
            F32X4_MIN | F64X2_MIN => {
                // Both orders, so that a NaN or -0 in either operand makes it into the result, and NaN lanes
                // canonical.
                let packed = if sub_opcode == F32X4_MIN { &PS } else { &PD };
                self.vector_op(packed.min, SCRATCH_XMM, a, b);
                self.vector_op(packed.min, dst, b, a);
                self.vector_op(packed.or, dst, dst, SCRATCH_XMM);
                self.asm
                    .vex(packed.cmp, SCRATCH_XMM, dst, VexOperand::Register(dst), Some(CMP_UNORD));
                self.vector_op(packed.or, dst, dst, SCRATCH_XMM);
                self.vector_shift_immediate(packed.shift_right, SCRATCH_XMM, SCRATCH_XMM, packed.nan_payload_shift);
                self.vector_op(packed.andn, dst, SCRATCH_XMM, dst);
            }
            F32X4_MAX | F64X2_MAX => {
                // Both orders, so that a NaN in either operand makes it into the result, +0 beats -0, and NaN
                // lanes are canonical.
                let packed = if sub_opcode == F32X4_MAX { &PS } else { &PD };
                self.vector_op(packed.max, SCRATCH_XMM, a, b);
                self.vector_op(packed.max, dst, b, a);
                self.vector_op(packed.xor, SCRATCH_XMM, SCRATCH_XMM, dst);
                self.vector_op(packed.or, dst, dst, SCRATCH_XMM);
                self.vector_op(packed.sub, dst, dst, SCRATCH_XMM);
                self.asm.vex(
                    packed.cmp,
                    SCRATCH_XMM,
                    SCRATCH_XMM,
                    VexOperand::Register(dst),
                    Some(CMP_UNORD),
                );
                self.vector_shift_immediate(packed.shift_right, SCRATCH_XMM, SCRATCH_XMM, packed.nan_payload_shift);
                self.vector_op(packed.andn, dst, SCRATCH_XMM, dst);
            }
            _ => unreachable!("SIMD sub-opcode {sub_opcode:#x} is no binary vector operation"),
        }
    }

    /// A shift of the vector in `a` by the i32 in `count_register`, or by `count` where it is a constant.
    fn vector_shift(
        &mut self,
        sub_opcode: u32,
        dst: u8,
        a: u8,
        count_register: Register,
        count: Option<u32>,
        temp: Register,
    ) {
        let bits = match sub_opcode {
            I8X16_SHL | I8X16_SHR_S | I8X16_SHR_U => 8,
            I64X2_SHR_S => 64,
            _ => shift(sub_opcode).expect("a vector shift").2,
        };
        let count = count.map(|count| (count & (bits - 1)) as u8);
        // The masked count in the scratch xmm register, plus `bias`.
        let count_into_scratch = |this: &mut Self, bias: i32| {
            this.asm.mov_rr(Width::W32, SCRATCH, gpr(count_register));
            this.asm.and_ri(Width::W32, SCRATCH, bits as i32 - 1);
            if bias != 0 {
                this.asm.add_ri(Width::W32, SCRATCH, bias);
            }
            this.asm.vmov_xr(Width::W32, SCRATCH_FPR, SCRATCH);
        };
        match sub_opcode {
            I8X16_SHL | I8X16_SHR_U => {
                // 16-bit lanes shifted, and the bits that crossed into the next byte cleared.
                let left = sub_opcode == I8X16_SHL;
                if let Some(count) = count {
                    let (digit, mask) = if left {
                        (6, 0xffu8 << count)
                    } else {
                        (2, 0xffu8 >> count)
                    };
                    self.vector_shift_immediate((SHIFT_WORDS, digit), dst, a, count);
                    self.vector_op_constant(VPAND, dst, dst, splat8(mask));
                    return;
                }
                let temp = fpr(temp).0;
                count_into_scratch(self, 0);
                let opcode = if left { VPSLLW } else { VPSRLW };
                self.vector_op(opcode, dst, a, SCRATCH_XMM);
                // The mask is the byte of ones shifted the same: the low byte of each shifted 16-bit lane of ones
                // for a left shift, the high one for a right shift, spread over all bytes.
                self.vector_ones(temp);
                self.vector_op(opcode, temp, temp, SCRATCH_XMM);
                if left {
                    self.vector_zero(SCRATCH_XMM);
                    self.vector_op(VPSHUFB, temp, temp, SCRATCH_XMM);
                } else {
                    self.vector_op_constant(VPSHUFB, temp, temp, splat8(1));
                }
                self.vector_op(VPAND, dst, dst, temp);
            }
            I8X16_SHR_S => {
                // Each byte in the high half of a 16-bit lane, shifted arithmetically by 8 more, and narrowed.
                let temp = fpr(temp).0;
                self.vector_op(VPUNPCKHBW, temp, a, a);
                self.vector_op(VPUNPCKLBW, dst, a, a);
                if let Some(count) = count {
                    self.vector_shift_immediate((SHIFT_WORDS, 4), temp, temp, count + 8);
                    self.vector_shift_immediate((SHIFT_WORDS, 4), dst, dst, count + 8);
                } else {
                    count_into_scratch(self, 8);
                    self.vector_op(VPSRAW, temp, temp, SCRATCH_XMM);
                    self.vector_op(VPSRAW, dst, dst, SCRATCH_XMM);
                }
                self.vector_op(VPACKSSWB, dst, dst, temp);
            }
            I64X2_SHR_S => {
                // ((a ^ sign) >>> count) ^ sign
                let temp = fpr(temp).0;
                self.vector_zero(SCRATCH_XMM);
                self.vector_op(VPCMPGTQ, temp, SCRATCH_XMM, a);
                self.vector_op(VPXOR, dst, a, temp);
                if let Some(count) = count {
                    self.vector_shift_immediate((SHIFT_QWORDS, 2), dst, dst, count);
                } else {
                    count_into_scratch(self, 0);
                    self.vector_op(VPSRLQ, dst, dst, SCRATCH_XMM);
                }
                self.vector_op(VPXOR, dst, dst, temp);
            }
            _ => {
                let (opcode, immediate, _) = shift(sub_opcode).expect("a vector shift");
                if let Some(count) = count {
                    self.vector_shift_immediate(immediate, dst, a, count);
                } else {
                    count_into_scratch(self, 0);
                    self.vector_op(opcode, dst, a, SCRATCH_XMM);
                }
            }
        }
    }

    fn vector_ternary(
        &mut self,
        value: Value,
        sub_opcode: u32,
        output: Register,
        inputs: [Register; 3],
        temp: Register,
    ) {
        let dst = fpr(output).0;
        // The inputs `vector_constant_inputs()` allows to be constants are operands, the others registers.
        let operand = |this: &mut Self, index: usize| this.vector_input(value, index, inputs[index]);
        let register = |index: usize| fpr(inputs[index]).0;
        match sub_opcode {
            V128_BITSELECT => {
                // (a & mask) | (b & !mask)
                let [a, b, c] = [0, 1, 2].map(register);
                let temp = fpr(temp).0;
                self.vector_op(VPAND, temp, a, c);
                self.vector_op(VPANDN, dst, c, b);
                self.vector_op(VPOR, dst, dst, temp);
            }
            // The multiply-adds round twice, as the interpreter's do.
            F32X4_RELAXED_MADD | F64X2_RELAXED_MADD => {
                let packed = if sub_opcode == F32X4_RELAXED_MADD { &PS } else { &PD };
                let (b, c) = (operand(self, 1), operand(self, 2));
                self.asm.vex(packed.mul, dst, register(0), b, None);
                self.asm.vex(packed.add, dst, dst, c, None);
            }
            F32X4_RELAXED_NMADD | F64X2_RELAXED_NMADD => {
                let packed = if sub_opcode == F32X4_RELAXED_NMADD { &PS } else { &PD };
                let b = operand(self, 1);
                self.asm.vex(packed.mul, dst, register(0), b, None);
                self.vector_op(packed.sub, dst, register(2), dst);
            }
            I32X4_RELAXED_DOT_I8X16_I7X16_ADD_S => {
                // NB: vpmaddubsw takes b's bytes as unsigned, and its sums of pairs saturate.
                let (a, c) = (operand(self, 0), operand(self, 2));
                self.asm.vex(VPMADDUBSW, dst, register(1), a, None);
                self.vector_op_constant(VPMADDWD, dst, dst, splat16(1));
                self.asm.vex(VPADDD, dst, dst, c, None);
            }
            _ => unreachable!("SIMD sub-opcode {sub_opcode:#x} is no ternary vector operation"),
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

/// The sign or zero extension of an extend or extending multiply, and whether it is of the high half.
fn extension(sub_opcode: u32) -> (VexOpcode, bool) {
    match sub_opcode {
        I16X8_EXTEND_LOW_I8X16_S | I16X8_EXTMUL_LOW_I8X16_S => (op66_38(0x20), false),
        I16X8_EXTEND_HIGH_I8X16_S | I16X8_EXTMUL_HIGH_I8X16_S => (op66_38(0x20), true),
        I16X8_EXTEND_LOW_I8X16_U | I16X8_EXTMUL_LOW_I8X16_U => (op66_38(0x30), false),
        I16X8_EXTEND_HIGH_I8X16_U | I16X8_EXTMUL_HIGH_I8X16_U => (op66_38(0x30), true),
        I32X4_EXTEND_LOW_I16X8_S => (op66_38(0x23), false),
        I32X4_EXTEND_HIGH_I16X8_S => (op66_38(0x23), true),
        I32X4_EXTEND_LOW_I16X8_U => (op66_38(0x33), false),
        I32X4_EXTEND_HIGH_I16X8_U => (op66_38(0x33), true),
        I64X2_EXTEND_LOW_I32X4_S => (op66_38(0x25), false),
        I64X2_EXTEND_HIGH_I32X4_S => (op66_38(0x25), true),
        I64X2_EXTEND_LOW_I32X4_U => (op66_38(0x35), false),
        _ => (op66_38(0x35), true),
    }
}
