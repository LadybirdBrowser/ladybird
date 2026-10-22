/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Encoders beyond what libjs_jit's code generation uses, for the WebAssembly JIT: scalar floating point of both
//! precisions, conversions, bit counting, LSE atomics, and Advanced SIMD (NEON).

use super::{Assembler, Cond, Fpr, Gpr, SP, Width, rd, rm, rn, sf, vd, vm, vn};

/// The precision of a scalar floating point instruction (its `ftype` field).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FloatSize {
    Single,
    Double,
}

const fn ftype(size: FloatSize) -> u32 {
    match size {
        FloatSize::Single => 0,
        FloatSize::Double => 1 << 22,
    }
}

/// The scalar floating point instructions of two sources.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FloatBinary {
    Mul = 0b0000,
    Div = 0b0001,
    Add = 0b0010,
    Sub = 0b0011,
    Max = 0b0100,
    Min = 0b0101,
}

/// The scalar floating point instructions of one source.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FloatUnary {
    Mov = 0b000000,
    Abs = 0b000001,
    Neg = 0b000010,
    Sqrt = 0b000011,
    /// To nearest, ties to even.
    RoundNearest = 0b001000,
    /// Toward plus infinity.
    RoundUp = 0b001001,
    /// Toward minus infinity.
    RoundDown = 0b001010,
    /// Toward zero.
    RoundTowardZero = 0b001011,
}

/// The access size of an atomic instruction (its `size` field).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AtomicSize {
    Byte = 0,
    Half = 1,
    Word = 2,
    Double = 3,
}

impl AtomicSize {
    pub const fn from_log2(log2: u32) -> Self {
        match log2 {
            0 => AtomicSize::Byte,
            1 => AtomicSize::Half,
            2 => AtomicSize::Word,
            _ => AtomicSize::Double,
        }
    }
}

/// The LSE read-modify-write instructions, in their sequentially consistent (acquire and release) forms.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AtomicRmw {
    /// `ldaddal`
    Add = 0b0000,
    /// `ldclral`: clears the bits set in the operand.
    Clear = 0b0001,
    /// `ldeoral`
    Xor = 0b0010,
    /// `ldsetal`
    Set = 0b0011,
    /// `swpal`
    Swap = 0b1000,
}

/// The arrangement of an Advanced SIMD register: the lane size and whether all 128 bits are used.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VectorArrangement {
    B8,
    B16,
    H4,
    H8,
    S2,
    S4,
    D2,
}

impl VectorArrangement {
    /// The Q bit, at bit 30.
    pub const fn q(self) -> u32 {
        match self {
            VectorArrangement::B8 | VectorArrangement::H4 | VectorArrangement::S2 => 0,
            _ => 1 << 30,
        }
    }

    /// log2 of the lane size in bytes.
    pub const fn size(self) -> u32 {
        match self {
            VectorArrangement::B8 | VectorArrangement::B16 => 0,
            VectorArrangement::H4 | VectorArrangement::H8 => 1,
            VectorArrangement::S2 | VectorArrangement::S4 => 2,
            VectorArrangement::D2 => 3,
        }
    }
}

impl Assembler {
    /// Makes sure that the next `size` bytes of code go in one piece, without a veneer pool between them.
    pub fn reserve_room(&mut self, size: usize) {
        self.reserve(size);
    }

    // Integers.

    /// `clz rd, rn`.
    pub fn clz(&mut self, width: Width, rd_: Gpr, rn_: Gpr) {
        self.emit(0x5ac0_1000 | sf(width) | rn(rn_) | rd(rd_));
    }

    /// `rbit rd, rn`.
    pub fn rbit(&mut self, width: Width, rd_: Gpr, rn_: Gpr) {
        self.emit(0x5ac0_0000 | sf(width) | rn(rn_) | rd(rd_));
    }

    /// `ror rd, rn, #amount` (`extr rd, rn, rn, #amount`).
    pub fn ror_imm(&mut self, width: Width, rd_: Gpr, rn_: Gpr, amount: u8) {
        assert!(u32::from(amount) < width.bits(), "rotate amount out of range");
        let n = if width == Width::W64 { 1 << 22 } else { 0 };
        self.emit(0x1380_0000 | sf(width) | n | rm(rn_) | (u32::from(amount) << 10) | rn(rn_) | rd(rd_));
    }

    /// `add rd, rn, #imm`, for any 64-bit `imm`, through `scratch` where it does not fit an immediate. rd and rn
    /// may be SP. SP changes in one instruction: what lies below its final value may be live, and a signal handler
    /// may write below SP at any time.
    pub fn add_any(&mut self, rd_: Gpr, rn_: Gpr, imm: i64, scratch: Gpr) {
        let magnitude = imm.unsigned_abs();
        let negative = imm < 0;
        if magnitude < 4096 {
            if negative {
                self.sub_imm(Width::W64, rd_, rn_, magnitude as u32, false);
            } else {
                self.add_imm(Width::W64, rd_, rn_, magnitude as u32, false);
            }
        } else if magnitude & 0xfff == 0 && magnitude < (1 << 24) {
            if negative {
                self.sub_imm(Width::W64, rd_, rn_, (magnitude >> 12) as u32, true);
            } else {
                self.add_imm(Width::W64, rd_, rn_, (magnitude >> 12) as u32, true);
            }
        } else if magnitude < (1 << 24) {
            if rd_ == SP {
                self.add_any(scratch, rn_, imm, scratch);
                self.mov_sp(SP, scratch);
                return;
            }
            let (high, low) = ((magnitude >> 12) as u32, (magnitude & 0xfff) as u32);
            if negative {
                self.sub_imm(Width::W64, rd_, rn_, high, true);
                self.sub_imm(Width::W64, rd_, rd_, low, false);
            } else {
                self.add_imm(Width::W64, rd_, rn_, high, true);
                self.add_imm(Width::W64, rd_, rd_, low, false);
            }
        } else {
            self.mov_imm(Width::W64, scratch, imm as u64);
            self.add_extended(Width::W64, rd_, rn_, scratch, super::Extend::Uxtx, 0);
        }
    }

    // Scalar floating point.

    /// `fadd`, `fsub`, `fmul`, `fdiv`, `fmax` or `fmin` of either precision.
    pub fn float_binary(&mut self, op: FloatBinary, size: FloatSize, rd_: Fpr, rn_: Fpr, rm_: Fpr) {
        self.emit(0x1e20_0800 | ftype(size) | vm(rm_) | ((op as u32) << 12) | vn(rn_) | vd(rd_));
    }

    /// `fmov`, `fabs`, `fneg`, `fsqrt` or `frint*` of either precision.
    pub fn float_unary(&mut self, op: FloatUnary, size: FloatSize, rd_: Fpr, rn_: Fpr) {
        self.emit(0x1e20_4000 | ftype(size) | ((op as u32) << 15) | vn(rn_) | vd(rd_));
    }

    /// `fcmp sn, sm` or `fcmp dn, dm`: NZCV = unordered 0011, less 1000, equal 0110, greater 0010.
    pub fn float_compare(&mut self, size: FloatSize, rn_: Fpr, rm_: Fpr) {
        self.emit(0x1e20_2000 | ftype(size) | vm(rm_) | vn(rn_));
    }

    /// `fcsel rd, rn, rm, cond`.
    pub fn float_select(&mut self, size: FloatSize, rd_: Fpr, rn_: Fpr, rm_: Fpr, cond: Cond) {
        self.emit(0x1e20_0c00 | ftype(size) | vm(rm_) | ((cond as u32) << 12) | vn(rn_) | vd(rd_));
    }

    /// `fcvt`: from `from` to the other precision.
    pub fn float_convert(&mut self, from: FloatSize, rd_: Fpr, rn_: Fpr) {
        let opc = match from {
            FloatSize::Single => 0b01,
            FloatSize::Double => 0b00,
        };
        self.emit(0x1e22_4000 | ftype(from) | (opc << 15) | vn(rn_) | vd(rd_));
    }

    fn float_integer(&mut self, opcode: u32, width: Width, size: FloatSize, register_d: u32, register_n: u32) {
        self.emit(0x1e20_0000 | sf(width) | ftype(size) | (opcode << 16) | (register_n << 5) | register_d);
    }

    /// `scvtf` (signed) or `ucvtf`: the integer in `rn` (wn or xn) to a float.
    pub fn convert_to_float(&mut self, signed: bool, width: Width, size: FloatSize, rd_: Fpr, rn_: Gpr) {
        let opcode = if signed { 0b00_010 } else { 0b00_011 };
        self.float_integer(opcode, width, size, vd(rd_), u32::from(rn_.0 & 31));
    }

    /// `fcvtzs` (signed) or `fcvtzu`: truncates the float in `rn` toward zero, saturating, NaN giving 0.
    pub fn convert_to_integer(&mut self, signed: bool, width: Width, size: FloatSize, rd_: Gpr, rn_: Fpr) {
        let opcode = if signed { 0b11_000 } else { 0b11_001 };
        self.float_integer(opcode, width, size, rd(rd_), u32::from(rn_.0 & 31));
    }

    /// `fmov wd, sn` or `fmov xd, dn`: the bits of a float.
    pub fn float_to_gpr(&mut self, size: FloatSize, rd_: Gpr, rn_: Fpr) {
        let width = if size == FloatSize::Double {
            Width::W64
        } else {
            Width::W32
        };
        self.float_integer(0b00_110, width, size, rd(rd_), u32::from(rn_.0 & 31));
    }

    /// `fmov sd, wn` or `fmov dd, xn`: a float of the bits.
    pub fn gpr_to_float(&mut self, size: FloatSize, rd_: Fpr, rn_: Gpr) {
        let width = if size == FloatSize::Double {
            Width::W64
        } else {
            Width::W32
        };
        self.float_integer(0b00_111, width, size, vd(rd_), u32::from(rn_.0 & 31));
    }

    // Atomics.

    /// `ldar`, `ldarb` or `ldarh`: a load with acquire semantics, zero-extended.
    pub fn load_acquire(&mut self, size: AtomicSize, rt: Gpr, base: Gpr) {
        self.emit(((size as u32) << 30) | 0x08df_fc00 | rn(base) | rd(rt));
    }

    /// `stlr`, `stlrb` or `stlrh`: a store with release semantics.
    pub fn store_release(&mut self, size: AtomicSize, rt: Gpr, base: Gpr) {
        self.emit(((size as u32) << 30) | 0x089f_fc00 | rn(base) | rd(rt));
    }

    /// An LSE read-modify-write: `rt` = the old value at [base] (zero-extended), which becomes old op `rs`.
    pub fn atomic_rmw(&mut self, op: AtomicRmw, size: AtomicSize, rs: Gpr, rt: Gpr, base: Gpr) {
        let o3_opc = op as u32;
        self.emit(((size as u32) << 30) | 0x38e0_0000 | rm(rs) | (o3_opc << 12) | rn(base) | rd(rt));
    }

    /// `casal rs, rt, [base]`: if [base] equals `rs`, stores `rt`; `rs` becomes the old value (zero-extended).
    pub fn compare_and_swap(&mut self, size: AtomicSize, rs: Gpr, rt: Gpr, base: Gpr) {
        self.emit(((size as u32) << 30) | 0x08e0_fc00 | rm(rs) | rn(base) | rd(rt));
    }

    /// `dmb ish`.
    pub fn dmb_ish(&mut self) {
        self.emit(0xd503_3bbf);
    }

    /// Reads the virtual counter, `mrs xt, cntvct_el0`.
    pub fn read_counter(&mut self, rt: Gpr) {
        self.emit(0xd53b_e040 | rd(rt));
    }

    // Advanced SIMD.

    /// `mov vd.16b, vn.16b` (`orr`).
    pub fn mov_vector(&mut self, rd_: Fpr, rn_: Fpr) {
        self.emit(0x4ea0_1c00 | vm(rn_) | vn(rn_) | vd(rd_));
    }

    /// `ins vd.<T>[index], rn` (`mov`): lane `index` of lanes of `1 << size_log2` bytes from a general purpose register.
    pub fn insert_lane_from_gpr(&mut self, rd_: Fpr, size_log2: u32, index: u32, rn_: Gpr) {
        let imm5 = ((index << 1) | 1) << size_log2;
        self.emit(0x4e00_1c00 | (imm5 << 16) | rn(rn_) | vd(rd_));
    }

    /// `cnt vd.8b, vn.8b`: the set bits of each byte.
    pub fn vector_count_bits(&mut self, rd_: Fpr, rn_: Fpr) {
        self.emit(0x0e20_5800 | vn(rn_) | vd(rd_));
    }

    /// `addv`: the sum of the lanes of `rn`, in the lowest lane of `rd`.
    pub fn vector_add_across(&mut self, arrangement: VectorArrangement, rd_: Fpr, rn_: Fpr) {
        self.emit(0x0e31_b800 | arrangement.q() | (arrangement.size() << 22) | vn(rn_) | vd(rd_));
    }

    /// An Advanced SIMD "three same" instruction: `0 Q U 01110 size 1 Rm opcode 1 Rn Rd`. Float ones pass their
    /// size field (the high bit selects among operations, the low bit is the precision) as `size`.
    /// `operation` is the U bit and the opcode.
    pub fn vector_three_same(&mut self, q: bool, (u, opcode): (bool, u32), size: u32, rd_: Fpr, rn_: Fpr, rm_: Fpr) {
        self.emit(
            (u32::from(q) << 30)
                | (u32::from(u) << 29)
                | 0x0e20_0400
                | ((size & 3) << 22)
                | vm(rm_)
                | ((opcode & 31) << 11)
                | vn(rn_)
                | vd(rd_),
        );
    }

    /// An Advanced SIMD "three different" instruction (the long, wide and narrow ones): `0 Q U 01110 size 1 Rm opcode
    /// 00 Rn Rd`, where Q selects the second (high) half.
    /// `operation` is the U bit and the opcode.
    pub fn vector_three_different(
        &mut self,
        q: bool,
        (u, opcode): (bool, u32),
        size: u32,
        rd_: Fpr,
        rn_: Fpr,
        rm_: Fpr,
    ) {
        self.emit(
            (u32::from(q) << 30)
                | (u32::from(u) << 29)
                | 0x0e20_0000
                | ((size & 3) << 22)
                | vm(rm_)
                | ((opcode & 15) << 12)
                | vn(rn_)
                | vd(rd_),
        );
    }

    /// An Advanced SIMD "two-register miscellaneous" instruction: `0 Q U 01110 size 10000 opcode 10 Rn Rd`.
    pub fn vector_two_misc(&mut self, q: bool, u: bool, size: u32, opcode: u32, rd_: Fpr, rn_: Fpr) {
        self.emit(
            (u32::from(q) << 30)
                | (u32::from(u) << 29)
                | 0x0e20_0800
                | ((size & 3) << 22)
                | ((opcode & 31) << 12)
                | vn(rn_)
                | vd(rd_),
        );
    }

    /// An Advanced SIMD "across lanes" instruction: `0 Q U 01110 size 11000 opcode 10 Rn Rd`.
    pub fn vector_across(&mut self, q: bool, u: bool, size: u32, opcode: u32, rd_: Fpr, rn_: Fpr) {
        self.emit(
            (u32::from(q) << 30)
                | (u32::from(u) << 29)
                | 0x0e30_0800
                | ((size & 3) << 22)
                | ((opcode & 31) << 12)
                | vn(rn_)
                | vd(rd_),
        );
    }

    /// An Advanced SIMD shift by an immediate: `0 Q U 011110 immh:immb opcode 1 Rn Rd`. `immh_immb` is the 7-bit
    /// field, which encodes the lane size and the shift.
    pub fn vector_shift_immediate(&mut self, q: bool, u: bool, immh_immb: u32, opcode: u32, rd_: Fpr, rn_: Fpr) {
        self.emit(
            (u32::from(q) << 30)
                | (u32::from(u) << 29)
                | 0x0f00_0400
                | ((immh_immb & 0x7f) << 16)
                | ((opcode & 31) << 11)
                | vn(rn_)
                | vd(rd_),
        );
    }

    /// An Advanced SIMD permute (`uzp1`, `trn1`, `zip1`, ...): `0 Q 001110 size 0 Rm 0 opcode 10 Rn Rd`.
    pub fn vector_permute(&mut self, arrangement: VectorArrangement, opcode: u32, rd_: Fpr, rn_: Fpr, rm_: Fpr) {
        self.emit(
            arrangement.q()
                | 0x0e00_0800
                | (arrangement.size() << 22)
                | vm(rm_)
                | ((opcode & 7) << 12)
                | vn(rn_)
                | vd(rd_),
        );
    }

    /// `ext vd.16b, vn.16b, vm.16b, #index`: bytes `index` to `index + 15` of rm:rn.
    pub fn vector_extract(&mut self, rd_: Fpr, rn_: Fpr, rm_: Fpr, index: u32) {
        self.emit(0x6e00_0000 | vm(rm_) | ((index & 15) << 11) | vn(rn_) | vd(rd_));
    }

    /// `tbl vd.16b, {vn.16b .. vn+count-1.16b}, vm.16b`: a byte of the table per index, zero for indices past it.
    pub fn vector_table(&mut self, rd_: Fpr, rn_: Fpr, count: u32, rm_: Fpr) {
        self.emit(0x4e00_0000 | vm(rm_) | (((count - 1) & 3) << 13) | vn(rn_) | vd(rd_));
    }

    /// The copy instructions: `0 Q op 01110000 imm5 0 imm4 1 Rn Rd` (`dup`, `ins`, `smov`, `umov`).
    pub fn vector_copy(&mut self, q: bool, op: bool, imm5: u32, imm4: u32, rd_: u8, rn_: u8) {
        self.emit(
            (u32::from(q) << 30)
                | (u32::from(op) << 29)
                | 0x0e00_0400
                | ((imm5 & 31) << 16)
                | ((imm4 & 15) << 11)
                | (u32::from(rn_ & 31) << 5)
                | u32::from(rd_ & 31),
        );
    }

    /// The modified immediate instructions (`movi`, `mvni`, `orr`, `bic` of an immediate): `0 Q op 0111100000 abc
    /// cmode o2 1 defgh Rd`.
    pub fn vector_modified_immediate(&mut self, q: bool, op: bool, cmode: u32, imm8: u8, rd_: Fpr) {
        let imm8 = u32::from(imm8);
        self.emit(
            (u32::from(q) << 30)
                | (u32::from(op) << 29)
                | 0x0f00_0400
                | ((imm8 >> 5) << 16)
                | ((cmode & 15) << 12)
                | ((imm8 & 31) << 5)
                | vd(rd_),
        );
    }

    /// An Advanced SIMD scalar instruction of the "two-register miscellaneous" group, as `vector_two_misc()` with the
    /// scalar bit: `01 U 11110 size 10000 opcode 10 Rn Rd`.
    pub fn scalar_two_misc(&mut self, u: bool, size: u32, opcode: u32, rd_: Fpr, rn_: Fpr) {
        self.emit((u32::from(u) << 29) | 0x5e20_0800 | ((size & 3) << 22) | ((opcode & 31) << 12) | vn(rn_) | vd(rd_));
    }

    /// `ld1r {vt.<T>}, [rn]`: one lane loaded and replicated to all, `1 << size_log2` bytes.
    pub fn load_replicate(&mut self, size_log2: u32, rt: Fpr, rn_: Gpr) {
        self.emit(0x4d40_c000 | ((size_log2 & 3) << 10) | rn(rn_) | vd(rt));
    }

    /// `ld1 {vt.<T>}[index], [rn]`: one lane of `1 << size_log2` bytes loaded into lane `index`.
    pub fn load_lane(&mut self, size_log2: u32, index: u32, rt: Fpr, rn_: Gpr) {
        self.emit(0x0d40_0000 | single_structure_fields(size_log2, index) | rn(rn_) | vd(rt));
    }

    /// `st1 {vt.<T>}[index], [rn]`: lane `index` of `1 << size_log2` bytes stored.
    pub fn store_lane(&mut self, size_log2: u32, index: u32, rt: Fpr, rn_: Gpr) {
        self.emit(0x0d00_0000 | single_structure_fields(size_log2, index) | rn(rn_) | vd(rt));
    }
}

/// The Q, opcode, S and size fields of a single-structure load or store of lane `index`, `1 << size_log2` bytes.
const fn single_structure_fields(size_log2: u32, index: u32) -> u32 {
    // Q:S:size holds the index, scaled by the lane size; opcode bits 15:13 are 000 for bytes, 010 for halves and 100
    // for words and doublewords.
    let (opcode, bits) = match size_log2 {
        0 => (0b000, index),
        1 => (0b010, index << 1),
        2 => (0b100, index << 2),
        _ => (0b100, (index << 3) | 1),
    };
    let q = (bits >> 3) & 1;
    let s = (bits >> 2) & 1;
    let size = bits & 3;
    (q << 30) | (opcode << 13) | (s << 12) | (size << 10)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::asm::aarch64::{Cond, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15, X17};

    fn v(index: u8) -> Fpr {
        Fpr(index)
    }

    /// SP moves in one instruction even where the immediate takes two.
    #[test]
    fn moves_sp_in_one_instruction() {
        let mut asm = Assembler::new();
        asm.add_any(SP, Gpr(29), -0x12345, X17); // sub x17, x29, #0x12, lsl #12; sub x17, x17, #0x345; mov sp, x17
        asm.add_any(X1, X2, 0x12345, X17); // add x1, x2, #0x12, lsl #12; add x1, x1, #0x345
        let code = asm.finish().unwrap();
        let words: Vec<u32> = code
            .chunks(4)
            .map(|word| u32::from_le_bytes(word.try_into().unwrap()))
            .collect();
        assert_eq!(words, [0xd1404bb1, 0xd10d1631, 0x9100023f, 0x91404841, 0x910d1421]);
    }

    /// The encodings llvm-mc gives these instructions (`llvm-mc -triple=aarch64 -mattr=+lse,+neon,+rdm`).
    #[test]
    fn encodes_like_llvm_mc() {
        let mut asm = Assembler::new();
        asm.clz(Width::W32, X1, X2); // clz w1, w2
        asm.rbit(Width::W64, X3, X4); // rbit x3, x4
        asm.ror_imm(Width::W64, X5, X6, 13); // ror x5, x6, #13
        asm.float_binary(FloatBinary::Add, FloatSize::Single, v(1), v(2), v(3)); // fadd s1, s2, s3
        asm.float_binary(FloatBinary::Div, FloatSize::Double, v(1), v(2), v(3)); // fdiv d1, d2, d3
        asm.float_binary(FloatBinary::Max, FloatSize::Single, v(4), v(5), v(6)); // fmax s4, s5, s6
        asm.float_binary(FloatBinary::Min, FloatSize::Double, v(7), v(8), v(9)); // fmin d7, d8, d9
        asm.float_unary(FloatUnary::Sqrt, FloatSize::Single, v(1), v(2)); // fsqrt s1, s2
        asm.float_unary(FloatUnary::RoundNearest, FloatSize::Double, v(3), v(4)); // frintn d3, d4
        asm.float_unary(FloatUnary::RoundTowardZero, FloatSize::Single, v(5), v(6)); // frintz s5, s6
        asm.float_compare(FloatSize::Single, v(1), v(2)); // fcmp s1, s2
        asm.float_select(FloatSize::Double, v(1), v(2), v(3), Cond::Ne); // fcsel d1, d2, d3, ne
        asm.float_convert(FloatSize::Single, v(1), v(2)); // fcvt d1, s2
        asm.float_convert(FloatSize::Double, v(3), v(4)); // fcvt s3, d4
        asm.convert_to_float(true, Width::W32, FloatSize::Single, v(1), X2); // scvtf s1, w2
        asm.convert_to_float(false, Width::W64, FloatSize::Double, v(3), X4); // ucvtf d3, x4
        asm.convert_to_integer(true, Width::W32, FloatSize::Double, X5, v(6)); // fcvtzs w5, d6
        asm.convert_to_integer(false, Width::W64, FloatSize::Single, X7, v(8)); // fcvtzu x7, s8
        asm.float_to_gpr(FloatSize::Single, X1, v(2)); // fmov w1, s2
        asm.gpr_to_float(FloatSize::Double, v(3), X4); // fmov d3, x4
        asm.load_acquire(AtomicSize::Word, X1, X2); // ldar w1, [x2]
        asm.load_acquire(AtomicSize::Byte, X3, X4); // ldarb w3, [x4]
        asm.store_release(AtomicSize::Double, X5, X6); // stlr x5, [x6]
        asm.atomic_rmw(AtomicRmw::Add, AtomicSize::Word, X1, X2, X3); // ldaddal w1, w2, [x3]
        asm.atomic_rmw(AtomicRmw::Clear, AtomicSize::Double, X4, X5, X6); // ldclral x4, x5, [x6]
        asm.atomic_rmw(AtomicRmw::Xor, AtomicSize::Half, X7, X8, X9); // ldeoralh w7, w8, [x9]
        asm.atomic_rmw(AtomicRmw::Set, AtomicSize::Byte, X10, X11, X12); // ldsetalb w10, w11, [x12]
        asm.atomic_rmw(AtomicRmw::Swap, AtomicSize::Double, X13, X14, X15); // swpal x13, x14, [x15]
        asm.compare_and_swap(AtomicSize::Word, X1, X2, X3); // casal w1, w2, [x3]
        asm.compare_and_swap(AtomicSize::Byte, X4, X5, X6); // casalb w4, w5, [x6]
        asm.dmb_ish(); // dmb ish
        asm.read_counter(X1); // mrs x1, cntvct_el0
        asm.mov_vector(v(1), v(2)); // mov v1.16b, v2.16b
        asm.insert_lane_from_gpr(v(3), 3, 1, X4); // mov v3.d[1], x4
        asm.vector_count_bits(v(5), v(6)); // cnt v5.8b, v6.8b
        asm.vector_add_across(VectorArrangement::B8, v(7), v(8)); // addv b7, v8.8b
        asm.vector_three_same(true, (false, 0b10000), 2, v(1), v(2), v(3)); // add v1.4s, v2.4s, v3.4s
        asm.vector_three_same(true, (true, 0b00110), 0, v(1), v(2), v(3)); // cmhi v1.16b, v2.16b, v3.16b
        asm.vector_three_same(true, (true, 0b10110), 1, v(1), v(2), v(3)); // sqrdmulh v1.8h, v2.8h, v3.8h
        asm.vector_three_same(true, (true, 0b11100), 3, v(1), v(2), v(3)); // fcmgt v1.2d, v2.2d, v3.2d
        asm.vector_three_same(true, (false, 0b11010), 2, v(1), v(2), v(3)); // fsub v1.4s, v2.4s, v3.4s
        asm.vector_three_same(true, (true, 0b00011), 1, v(1), v(2), v(3)); // bsl v1.16b, v2.16b, v3.16b
        asm.vector_three_same(true, (false, 0b00011), 1, v(1), v(2), v(3)); // bic v1.16b, v2.16b, v3.16b
        asm.vector_three_different(false, (false, 0b1100), 1, v(1), v(2), v(3)); // smull v1.4s, v2.4h, v3.4h
        asm.vector_three_different(true, (true, 0b1100), 2, v(1), v(2), v(3)); // umull2 v1.2d, v2.4s, v3.4s
        asm.vector_three_different(false, (true, 0b1000), 2, v(1), v(2), v(3)); // umlal v1.2d, v2.2s, v3.2s
        asm.vector_two_misc(true, false, 3, 0b01011, v(1), v(2)); // abs v1.2d, v2.2d
        asm.vector_two_misc(false, false, 1, 0b10110, v(1), v(2)); // fcvtn v1.2s, v2.2d
        asm.vector_two_misc(false, false, 1, 0b10111, v(1), v(2)); // fcvtl v1.2d, v2.2s
        asm.vector_two_misc(true, false, 0, 0b10100, v(1), v(2)); // sqxtn2 v1.16b, v2.8h
        asm.vector_two_misc(true, true, 2, 0b00010, v(1), v(2)); // uaddlp v1.2d, v2.4s
        asm.vector_shift_immediate(true, true, 16 - 7, 0b00000, v(1), v(2)); // ushr v1.16b, v2.16b, #7
        asm.vector_shift_immediate(true, true, 128 - 28, 0b00010, v(1), v(2)); // usra v1.2d, v2.2d, #28
        asm.vector_shift_immediate(true, false, 64 + 32, 0b01010, v(1), v(2)); // shl v1.2d, v2.2d, #32
        asm.vector_shift_immediate(true, false, 8, 0b10100, v(1), v(2)); // sshll2 v1.8h, v2.16b, #0
        asm.vector_across(true, true, 2, 0b01010, v(1), v(2)); // umaxv s1, v2.4s
        asm.vector_permute(VectorArrangement::H8, 0b101, v(1), v(2), v(3)); // uzp2 v1.8h, v2.8h, v3.8h
        asm.vector_extract(v(1), v(2), v(3), 5); // ext v1.16b, v2.16b, v3.16b, #5
        asm.vector_table(v(1), v(30), 2, v(2)); // tbl v1.16b, {v30.16b, v31.16b}, v2.16b
        asm.vector_copy(true, false, 0b00100, 0b0001, 1, 2); // dup v1.4s, w2
        asm.vector_copy(true, false, 0b01000, 0b0000, 1, 2); // dup v1.2d, v2.d[0]
        asm.vector_copy(false, false, 17, 0b0111, 1, 2); // umov w1, v2.b[8]
        asm.vector_copy(false, false, 14, 0b0101, 1, 2); // smov w1, v2.h[3]
        asm.vector_copy(true, false, 24, 0b0111, 1, 2); // mov x1, v2.d[1]
        asm.vector_copy(true, true, 20, 0, 1, 2); // mov v1.s[2], v2.s[0]
        asm.load_replicate(2, v(1), X2); // ld1r {v1.4s}, [x2]
        asm.load_lane(1, 5, v(1), X2); // ld1 {v1.h}[5], [x2]
        asm.store_lane(3, 1, v(1), X2); // st1 {v1.d}[1], [x2]
        asm.vector_modified_immediate(true, true, 0b1110, 0, v(1)); // movi v1.2d, #0
        let expected: [u32; 69] = [
            0x5ac01041, 0xdac00083, 0x93c634c5, 0x1e232841, 0x1e631841, 0x1e2648a4, 0x1e695907, 0x1e21c041, 0x1e644083,
            0x1e25c0c5, 0x1e222020, 0x1e631c41, 0x1e22c041, 0x1e624083, 0x1e220041, 0x9e630083, 0x1e7800c5, 0x9e390107,
            0x1e260041, 0x9e670083, 0x88dffc41, 0x08dffc83, 0xc89ffcc5, 0xb8e10062, 0xf8e410c5, 0x78e72128, 0x38ea318b,
            0xf8ed81ee, 0x88e1fc62, 0x08e4fcc5, 0xd5033bbf, 0xd53be041, 0x4ea21c41, 0x4e181c83, 0x0e2058c5, 0x0e31b907,
            0x4ea38441, 0x6e233441, 0x6e63b441, 0x6ee3e441, 0x4ea3d441, 0x6e631c41, 0x4e631c41, 0x0e63c041, 0x6ea3c041,
            0x2ea38041, 0x4ee0b841, 0x0e616841, 0x0e617841, 0x4e214841, 0x6ea02841, 0x6f090441, 0x6f641441, 0x4f605441,
            0x4f08a441, 0x6eb0a841, 0x4e435841, 0x6e032841, 0x4e0223c1, 0x4e040c41, 0x4e080441, 0x0e113c41, 0x0e0e2c41,
            0x4e183c41, 0x6e140441, 0x4d40c841, 0x4d404841, 0x4d008441, 0x6f00e401,
        ];
        let code = asm.finish().unwrap();
        let words: Vec<u32> = code
            .chunks(4)
            .map(|word| u32::from_le_bytes(word.try_into().unwrap()))
            .collect();
        for (index, (word, expected)) in words.iter().zip(expected).enumerate() {
            assert_eq!(
                *word, expected,
                "instruction {index}: {word:#010x}, llvm-mc {expected:#010x}"
            );
        }
        assert_eq!(words.len(), expected.len());
    }
}
