/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Integer and float arithmetic, comparisons, selects and conversions.

use super::{ADDRESS, CodeGenerator, SCRATCH, SCRATCH_FPR, condition, float_condition, float_size, fpr, gpr, width};
use crate::asm::Width;
use crate::asm::aarch64::{
    Cond, FloatBinary, FloatSize, FloatUnary, Shift, VectorArrangement, encode_logical_immediate,
};
use crate::convention::Register;
use crate::ir::{FLAG_CANNOT_TRAP, FloatCondition, IntCondition, Op, Type, Value};
use crate::regalloc::CONSTANT_INPUT;
use libwasm_module::abi::{TRAP_INTEGER_DIVIDE_BY_ZERO, TRAP_INTEGER_OVERFLOW, TRAP_INVALID_CONVERSION};

/// The bits of a width.
fn mask(width: Width) -> u64 {
    match width {
        Width::W32 => 0xffff_ffff,
        Width::W64 => u64::MAX,
    }
}

fn float_bits(ty: Type, value: f64) -> u64 {
    if ty == Type::F64 {
        value.to_bits()
    } else {
        u64::from((value as f32).to_bits())
    }
}

impl CodeGenerator<'_> {
    pub(super) fn lower_numeric(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        let record = *self.allocation.node(value);
        let [a, b, c] = record.inputs;
        let output = record.output;
        let ty = node.ty;
        match node.op {
            Op::Add | Op::Sub => self.add_sub(value, node.op == Op::Sub, ty, output, a, b),
            Op::And | Op::Or | Op::Xor => self.logic(value, node.op, ty, output, a, b),
            Op::Mul => {
                let rhs = self.input_or_scratch(value, 1, b, ty);
                self.asm.mul(width(ty), gpr(output), gpr(a), rhs);
            }
            Op::Shl | Op::ShrS | Op::ShrU => {
                let (dst, source) = (gpr(output), gpr(a));
                if b == CONSTANT_INPUT {
                    let amount = (self.immediate(value, 1) as u32 & (if ty == Type::I64 { 63 } else { 31 })) as u8;
                    match node.op {
                        Op::Shl => self.asm.lsl_imm(width(ty), dst, source, amount),
                        Op::ShrS => self.asm.asr_imm(width(ty), dst, source, amount),
                        _ => self.asm.lsr_imm(width(ty), dst, source, amount),
                    }
                } else {
                    // NB: The register forms take the amount modulo the width, as WebAssembly does.
                    match node.op {
                        Op::Shl => self.asm.lsl(width(ty), dst, source, gpr(b)),
                        Op::ShrS => self.asm.asr(width(ty), dst, source, gpr(b)),
                        _ => self.asm.lsr(width(ty), dst, source, gpr(b)),
                    }
                }
            }
            Op::Rotl | Op::Rotr => {
                let (dst, source) = (gpr(output), gpr(a));
                let bits = if ty == Type::I64 { 64 } else { 32 };
                if b == CONSTANT_INPUT {
                    let amount = self.immediate(value, 1) as u32 % bits;
                    let right = if node.op == Op::Rotr {
                        amount
                    } else {
                        (bits - amount) % bits
                    };
                    self.asm.ror_imm(width(ty), dst, source, right as u8);
                } else if node.op == Op::Rotr {
                    self.asm.ror(width(ty), dst, source, gpr(b));
                } else {
                    // A left rotation is a right rotation by the negated amount, modulo the width.
                    self.asm.neg(width(ty), SCRATCH, gpr(b));
                    self.asm.ror(width(ty), dst, source, SCRATCH);
                }
            }
            Op::DivS | Op::DivU | Op::RemS | Op::RemU => self.divide(value, ty, output, a, b),
            Op::Clz => self.asm.clz(width(ty), gpr(output), gpr(a)),
            Op::Ctz => {
                self.asm.rbit(width(ty), SCRATCH, gpr(a));
                self.asm.clz(width(ty), gpr(output), SCRATCH);
            }
            Op::Popcnt => {
                self.asm.fmov_from_gpr(SCRATCH_FPR, gpr(a));
                self.asm.vector_count_bits(SCRATCH_FPR, SCRATCH_FPR);
                self.asm
                    .vector_add_across(VectorArrangement::B8, SCRATCH_FPR, SCRATCH_FPR);
                self.asm.fmov_to_gpr(gpr(output), SCRATCH_FPR);
            }
            Op::Eqz => {
                let input_type = func.node(node.args[0]).ty;
                self.asm.cmp_imm(width(input_type), gpr(a), 0, false);
                self.asm.cset(Width::W32, gpr(output), Cond::Eq);
            }
            Op::Compare => {
                let input_type = func.node(node.args[0]).ty;
                self.compare(value, input_type, a, b, 1);
                self.asm
                    .cset(Width::W32, gpr(output), condition(IntCondition::from_flags(node.flags)));
            }
            Op::Extend8S => self.asm.sxtb(width(ty), gpr(output), gpr(a)),
            Op::Extend16S => self.asm.sxth(width(ty), gpr(output), gpr(a)),
            Op::Extend32S | Op::ExtendI32S => self.asm.sxtw(gpr(output), gpr(a)),
            Op::ExtendI32U | Op::Wrap => self.asm.mov(Width::W32, gpr(output), gpr(a)),
            Op::Select => {
                if ty == Type::V128 {
                    let skip = self.asm.new_label();
                    self.move_vector(fpr(output), fpr(c));
                    self.asm.cbz(Width::W32, gpr(a), skip);
                    self.move_vector(fpr(output), fpr(b));
                    self.asm.bind(skip);
                } else if ty.is_float() {
                    self.asm.cmp_imm(Width::W32, gpr(a), 0, false);
                    self.asm
                        .float_select(FloatSize::Double, fpr(output), fpr(b), fpr(c), Cond::Ne);
                } else {
                    self.asm.cmp_imm(Width::W32, gpr(a), 0, false);
                    self.asm.csel(Width::W64, gpr(output), gpr(b), gpr(c), Cond::Ne);
                }
            }
            Op::FAdd | Op::FSub | Op::FMul | Op::FDiv | Op::FMin | Op::FMax => {
                let op = match node.op {
                    Op::FAdd => FloatBinary::Add,
                    Op::FSub => FloatBinary::Sub,
                    Op::FMul => FloatBinary::Mul,
                    Op::FDiv => FloatBinary::Div,
                    // NB: fmin and fmax propagate NaNs and order -0 below +0, as WebAssembly's min and max do.
                    Op::FMin => FloatBinary::Min,
                    _ => FloatBinary::Max,
                };
                self.asm.float_binary(op, float_size(ty), fpr(output), fpr(a), fpr(b));
            }
            Op::FCopysign => {
                let w = width(ty);
                let sign = if ty == Type::F64 { 1u64 << 63 } else { 1 << 31 };
                let size = float_size(ty);
                self.asm.float_to_gpr(size, SCRATCH, fpr(a));
                self.asm.float_to_gpr(size, ADDRESS, fpr(b));
                self.asm.and_imm(w, SCRATCH, SCRATCH, !sign & mask(w));
                self.asm.and_imm(w, ADDRESS, ADDRESS, sign);
                self.asm.orr(w, SCRATCH, SCRATCH, ADDRESS, Shift::Lsl, 0);
                self.asm.gpr_to_float(size, fpr(output), SCRATCH);
            }
            Op::FAbs | Op::FNeg | Op::FSqrt | Op::FCeil | Op::FFloor | Op::FTrunc | Op::FNearest => {
                let op = match node.op {
                    Op::FAbs => FloatUnary::Abs,
                    Op::FNeg => FloatUnary::Neg,
                    Op::FSqrt => FloatUnary::Sqrt,
                    Op::FCeil => FloatUnary::RoundUp,
                    Op::FFloor => FloatUnary::RoundDown,
                    Op::FTrunc => FloatUnary::RoundTowardZero,
                    _ => FloatUnary::RoundNearest,
                };
                self.asm.float_unary(op, float_size(ty), fpr(output), fpr(a));
            }
            Op::FloatCompare => {
                let input_type = func.node(node.args[0]).ty;
                self.asm.float_compare(float_size(input_type), fpr(a), fpr(b));
                let cond = float_condition(FloatCondition::from_flags(node.flags));
                self.asm.cset(Width::W32, gpr(output), cond);
            }
            Op::TruncS | Op::TruncU => self.truncate(value, ty, output, a),
            Op::TruncSatS | Op::TruncSatU => {
                // NB: The conversions saturate, and give 0 for NaN, as the saturating truncations want.
                let input = func.node(node.args[0]).ty;
                self.asm.convert_to_integer(
                    node.op == Op::TruncSatS,
                    width(ty),
                    float_size(input),
                    gpr(output),
                    fpr(a),
                );
            }
            Op::ConvertS | Op::ConvertU => {
                let input = func.node(node.args[0]).ty;
                self.asm.convert_to_float(
                    node.op == Op::ConvertS,
                    width(input),
                    float_size(ty),
                    fpr(output),
                    gpr(a),
                );
            }
            Op::Demote => self.asm.float_convert(FloatSize::Double, fpr(output), fpr(a)),
            Op::Promote => self.asm.float_convert(FloatSize::Single, fpr(output), fpr(a)),
            Op::Reinterpret => {
                if ty.is_float() {
                    self.asm.gpr_to_float(float_size(ty), fpr(output), gpr(a));
                } else {
                    let input = func.node(node.args[0]).ty;
                    self.asm.float_to_gpr(float_size(input), gpr(output), fpr(a));
                }
            }
            _ => unreachable!("{} is no numeric op", node.op.name()),
        }
    }

    fn add_sub(&mut self, value: Value, subtract: bool, ty: Type, output: Register, a: Register, b: Register) {
        let w = width(ty);
        let (dst, lhs) = (gpr(output), gpr(a));
        if b == CONSTANT_INPUT {
            let bits = self.immediate(value, 1);
            let bits = if ty.is_64() { bits } else { bits & 0xffff_ffff };
            let negated = match w {
                Width::W32 => u64::from((bits as u32).wrapping_neg()),
                Width::W64 => bits.wrapping_neg(),
            };
            // The immediate forms take 12 bits, optionally shifted by 12, and subtracting adds the negation.
            let (add, magnitude) = if bits < (1 << 24) {
                (!subtract, bits)
            } else if negated < (1 << 24) {
                (subtract, negated)
            } else {
                self.asm.mov_imm(Width::W64, SCRATCH, bits);
                if subtract {
                    self.asm.sub(w, dst, lhs, SCRATCH, Shift::Lsl, 0);
                } else {
                    self.asm.add(w, dst, lhs, SCRATCH, Shift::Lsl, 0);
                }
                return;
            };
            let (high, low) = ((magnitude >> 12) as u32, (magnitude & 0xfff) as u32);
            let mut source = lhs;
            if high != 0 {
                if add {
                    self.asm.add_imm(w, dst, source, high, true);
                } else {
                    self.asm.sub_imm(w, dst, source, high, true);
                }
                source = dst;
            }
            if low != 0 || source != dst {
                if add {
                    self.asm.add_imm(w, dst, source, low, false);
                } else {
                    self.asm.sub_imm(w, dst, source, low, false);
                }
            }
            return;
        }
        if subtract {
            self.asm.sub(w, dst, lhs, gpr(b), Shift::Lsl, 0);
        } else {
            self.asm.add(w, dst, lhs, gpr(b), Shift::Lsl, 0);
        }
    }

    fn logic(&mut self, value: Value, op: Op, ty: Type, output: Register, a: Register, b: Register) {
        let w = width(ty);
        let (dst, lhs) = (gpr(output), gpr(a));
        if b == CONSTANT_INPUT {
            let bits = self.immediate(value, 1) & mask(w);
            if encode_logical_immediate(bits, w).is_some() {
                match op {
                    Op::And => self.asm.and_imm(w, dst, lhs, bits),
                    Op::Or => self.asm.orr_imm(w, dst, lhs, bits),
                    _ => self.asm.eor_imm(w, dst, lhs, bits),
                }
                return;
            }
        }
        let rhs = self.input_or_scratch(value, 1, b, ty);
        match op {
            Op::And => self.asm.and(w, dst, lhs, rhs, Shift::Lsl, 0),
            Op::Or => self.asm.orr(w, dst, lhs, rhs, Shift::Lsl, 0),
            _ => self.asm.eor(w, dst, lhs, rhs, Shift::Lsl, 0),
        }
    }

    /// Division and remainder. The hardware gives 0 for a zero divisor and INT_MIN for INT_MIN / -1, so both are
    /// checked; the remainder of INT_MIN and -1 comes out 0 as it should.
    fn divide(&mut self, value: Value, ty: Type, output: Register, a: Register, b: Register) {
        let node = *self.func.node(value);
        let w = width(ty);
        let (dst, dividend, divisor) = (gpr(output), gpr(a), gpr(b));
        let checked = node.flags & FLAG_CANNOT_TRAP == 0;
        if checked {
            let zero = self.trap_label(TRAP_INTEGER_DIVIDE_BY_ZERO);
            self.asm.cbz(w, divisor, zero);
        }
        let signed = matches!(node.op, Op::DivS | Op::RemS);
        if node.op == Op::DivS && checked {
            let divide = self.asm.new_label();
            let overflow = self.trap_label(TRAP_INTEGER_OVERFLOW);
            self.asm.cmn_imm(w, divisor, 1, false);
            self.asm.b_cond(Cond::Ne, divide);
            // INT_MIN - 1 is the only subtraction of 1 that overflows.
            self.asm.cmp_imm(w, dividend, 1, false);
            self.asm.b_cond(Cond::Vs, overflow);
            self.asm.bind(divide);
        }
        let quotient = if matches!(node.op, Op::DivS | Op::DivU) {
            dst
        } else {
            SCRATCH
        };
        if signed {
            self.asm.sdiv(w, quotient, dividend, divisor);
        } else {
            self.asm.udiv(w, quotient, dividend, divisor);
        }
        if quotient == SCRATCH {
            self.asm.msub(w, dst, SCRATCH, divisor, dividend);
        }
    }

    /// Compares `source` with the float constant `bound` of type `input`, with v31 holding the bound.
    fn compare_with_constant(&mut self, input: Type, source: Register, bound: f64) {
        let bits = float_bits(input, bound);
        self.load_vector_constant(u128::from(bits), SCRATCH_FPR, false);
        self.asm.float_compare(float_size(input), fpr(source), SCRATCH_FPR);
    }

    /// The trapping truncations of floats to integers: NaN is invalid, and values out of range overflow.
    fn truncate(&mut self, value: Value, ty: Type, output: Register, source: Register) {
        let node = *self.func.node(value);
        let input = self.func.node(node.args[0]).ty;
        let invalid = self.trap_label(TRAP_INVALID_CONVERSION);
        let overflow = self.trap_label(TRAP_INTEGER_OVERFLOW);
        self.asm.float_compare(float_size(input), fpr(source), fpr(source));
        self.asm.b_cond(Cond::Vs, invalid);
        let signed = node.op == Op::TruncS;
        let bits = if ty == Type::I64 { 64 } else { 32 };
        let limit = 2f64.powi(if signed { bits - 1 } else { bits });
        // The lower bound: values at or below it overflow (or below it, where it is exact).
        let (lower, below_only) = match (signed, bits, input) {
            (true, 32, Type::F64) => (-limit - 1.0, false),
            (true, _, _) => (-limit, true),
            (false, _, _) => (-1.0, false),
        };
        self.compare_with_constant(input, source, lower);
        self.asm.b_cond(if below_only { Cond::Lo } else { Cond::Ls }, overflow);
        self.compare_with_constant(input, source, limit);
        self.asm.b_cond(Cond::Hs, overflow);
        self.asm
            .convert_to_integer(signed, width(ty), float_size(input), gpr(output), fpr(source));
    }
}
