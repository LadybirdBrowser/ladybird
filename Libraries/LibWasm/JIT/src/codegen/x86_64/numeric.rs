/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Integer and float arithmetic, comparisons, selects and conversions.

use super::{CodeGenerator, SCRATCH, SCRATCH_FPR, condition, fpr, gpr, precision, width};
use crate::asm::x86_64::{AluOp, BitCount, BitwiseOp, Cond, FloatOp, Precision, Rounding, ShiftOp, ShiftX};
use crate::asm::{Address, Fpr, Gpr, Width};
use crate::convention::Register;
use crate::ir::{FLAG_CANNOT_TRAP, FloatCondition, IntCondition, Op, Type, Value};
use crate::regalloc::{CONSTANT_INPUT, SLOT_INPUT};
use libwasm_module::abi::{TRAP_INTEGER_DIVIDE_BY_ZERO, TRAP_INTEGER_OVERFLOW, TRAP_INVALID_CONVERSION};

/// The sign bit of a float type.
fn sign_bit(ty: Type) -> u64 {
    if ty == Type::F64 { 1 << 63 } else { 1 << 31 }
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
            Op::Add => self.add(value, ty, output, a, b),
            Op::Sub => self.sub(value, ty, output, a, b),
            Op::And => self.logic(value, AluOp::And, ty, output, a, b),
            Op::Or => self.logic(value, AluOp::Or, ty, output, a, b),
            Op::Xor => self.logic(value, AluOp::Xor, ty, output, a, b),
            Op::Mul => {
                let (dst, a_register) = (gpr(output), gpr(a));
                if b == SLOT_INPUT {
                    let address = self.input_slot(node.args[1]);
                    if output != a {
                        self.asm.mov_rr(width(ty), dst, a_register);
                    }
                    self.asm.imul_rm(width(ty), dst, &address);
                } else if b == CONSTANT_INPUT {
                    let immediate = self.immediate(value, 1) as i32;
                    self.asm.imul_rri(width(ty), dst, a_register, immediate);
                } else if output == a {
                    self.asm.imul_rr(width(ty), dst, gpr(b));
                } else if output == b {
                    self.asm.imul_rr(width(ty), dst, a_register);
                } else {
                    self.asm.mov_rr(width(ty), dst, a_register);
                    self.asm.imul_rr(width(ty), dst, gpr(b));
                }
            }
            Op::Shl | Op::ShrS | Op::ShrU => {
                let (dst, source) = (gpr(output), gpr(a));
                if b == CONSTANT_INPUT {
                    let amount = (self.immediate(value, 1) as u32 & (if ty == Type::I64 { 63 } else { 31 })) as u8;
                    if output != a {
                        self.asm.mov_rr(width(ty), dst, source);
                    }
                    let op = match node.op {
                        Op::Shl => ShiftOp::Shl,
                        Op::ShrS => ShiftOp::Sar,
                        _ => ShiftOp::Shr,
                    };
                    self.asm.shift_ri(op, width(ty), dst, amount);
                } else if !self.asm.cpu_features().bmi2 {
                    debug_assert_eq!(gpr(b), crate::asm::x86_64::RCX);
                    if output != a {
                        self.asm.mov_rr(width(ty), dst, source);
                    }
                    let op = match node.op {
                        Op::Shl => ShiftOp::Shl,
                        Op::ShrS => ShiftOp::Sar,
                        _ => ShiftOp::Shr,
                    };
                    self.asm.shift_rcl(op, width(ty), dst);
                } else {
                    let op = match node.op {
                        Op::Shl => ShiftX::Shl,
                        Op::ShrS => ShiftX::Sar,
                        _ => ShiftX::Shr,
                    };
                    self.asm.shiftx(op, width(ty), dst, source, gpr(b));
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
                    self.asm.rorx(width(ty), dst, source, right as u8);
                } else {
                    if output != a {
                        self.asm.mov_rr(width(ty), dst, source);
                    }
                    let op = if node.op == Op::Rotl {
                        ShiftOp::Rol
                    } else {
                        ShiftOp::Ror
                    };
                    self.asm.shift_rcl(op, width(ty), dst);
                }
            }
            Op::DivS | Op::DivU | Op::RemS | Op::RemU => self.divide(value, ty, b),
            Op::Clz | Op::Ctz | Op::Popcnt => {
                let count = match node.op {
                    Op::Clz => BitCount::Lzcnt,
                    Op::Ctz => BitCount::Tzcnt,
                    _ => BitCount::Popcnt,
                };
                self.asm.bit_count(count, width(ty), gpr(output), gpr(a));
            }
            Op::Eqz => {
                let input_type = func.node(node.args[0]).ty;
                let dst = gpr(output);
                self.asm.xor_rr(Width::W32, dst, dst);
                self.asm.test_rr(width(input_type), gpr(a), gpr(a));
                self.asm.setcc(Cond::Equal, dst);
            }
            Op::Compare => {
                let input_type = func.node(node.args[0]).ty;
                let dst = gpr(output);
                self.asm.xor_rr(Width::W32, dst, dst);
                self.compare(input_type, a, b, node.args[1]);
                self.asm.setcc(condition(IntCondition::from_flags(node.flags)), dst);
            }
            Op::Extend8S => self.asm.movsx8_rr(width(ty), gpr(output), gpr(a)),
            Op::Extend16S => self.asm.movsx16_rr(width(ty), gpr(output), gpr(a)),
            Op::Extend32S | Op::ExtendI32S => self.asm.movsxd_rr(gpr(output), gpr(a)),
            Op::ExtendI32U | Op::Wrap => self.asm.mov_rr(Width::W32, gpr(output), gpr(a)),
            Op::Select => {
                if ty.uses_xmm() {
                    let skip = self.asm.new_label();
                    self.asm.vmovaps_rr(fpr(output), fpr(c));
                    self.asm.test_rr(Width::W32, gpr(a), gpr(a));
                    self.asm.jcc(Cond::Equal, skip);
                    self.asm.vmovaps_rr(fpr(output), fpr(b));
                    self.asm.bind(skip);
                } else {
                    self.asm.mov_rr(Width::W64, gpr(output), gpr(c));
                    self.asm.test_rr(Width::W32, gpr(a), gpr(a));
                    self.asm.cmovcc(Cond::NotEqual, Width::W64, gpr(output), gpr(b));
                }
            }
            Op::FAdd | Op::FSub | Op::FMul | Op::FDiv => {
                let op = match node.op {
                    Op::FAdd => FloatOp::Add,
                    Op::FSub => FloatOp::Sub,
                    Op::FMul => FloatOp::Mul,
                    _ => FloatOp::Div,
                };
                if b == SLOT_INPUT {
                    let address = self.input_slot(node.args[1]);
                    self.asm.vfloat_rrm(op, precision(ty), fpr(output), fpr(a), &address);
                } else {
                    self.asm.vfloat_rrr(op, precision(ty), fpr(output), fpr(a), fpr(b));
                }
            }
            Op::FSqrt => self
                .asm
                .vfloat_rrr(FloatOp::Sqrt, precision(ty), fpr(output), fpr(a), fpr(a)),
            Op::FMin | Op::FMax => self.min_max(node.op == Op::FMin, ty, output, a, b),
            Op::FCopysign => {
                let temp = fpr(record.temps[0]);
                self.load_float_constant(ty, sign_bit(ty), SCRATCH_FPR);
                self.asm.vbitwise_rrr(BitwiseOp::And, temp, SCRATCH_FPR, fpr(b));
                self.asm
                    .vbitwise_rrr(BitwiseOp::AndNot, fpr(output), SCRATCH_FPR, fpr(a));
                self.asm.vbitwise_rrr(BitwiseOp::Or, fpr(output), fpr(output), temp);
            }
            Op::FAbs => {
                self.load_float_constant(ty, sign_bit(ty), SCRATCH_FPR);
                self.asm
                    .vbitwise_rrr(BitwiseOp::AndNot, fpr(output), SCRATCH_FPR, fpr(a));
            }
            Op::FNeg => {
                self.load_float_constant(ty, sign_bit(ty), SCRATCH_FPR);
                self.asm.vbitwise_rrr(BitwiseOp::Xor, fpr(output), fpr(a), SCRATCH_FPR);
            }
            Op::FCeil | Op::FFloor | Op::FTrunc | Op::FNearest => {
                let rounding = match node.op {
                    Op::FCeil => Rounding::Up,
                    Op::FFloor => Rounding::Down,
                    Op::FTrunc => Rounding::TowardZero,
                    _ => Rounding::Nearest,
                };
                self.asm.vround(precision(ty), fpr(output), fpr(a), rounding);
            }
            Op::FloatCompare => self.float_compare(value, output, a, b, record.temps[0]),
            Op::TruncS | Op::TruncU => self.truncate(value, ty, output, a, record.temps[0]),
            Op::TruncSatS | Op::TruncSatU => self.truncate_saturating(value, ty, output, a, record.temps[0]),
            Op::ConvertS => {
                let input_type = func.node(node.args[0]).ty;
                let dst = fpr(output);
                self.asm.vbitwise_rrr(BitwiseOp::Xor, dst, dst, dst);
                self.asm.vcvtsi2s(precision(ty), width(input_type), dst, dst, gpr(a));
            }
            Op::ConvertU => self.convert_unsigned(value, ty, output, a, record.temps[0]),
            Op::Demote => self.asm.vcvtsd2ss(fpr(output), fpr(a), fpr(a)),
            Op::Promote => self.asm.vcvtss2sd(fpr(output), fpr(a), fpr(a)),
            Op::Reinterpret => {
                if ty.is_float() {
                    self.asm.vmov_xr(width(ty), fpr(output), gpr(a));
                } else {
                    self.asm.vmov_rx(width(ty), gpr(output), fpr(a));
                }
            }
            _ => unreachable!("{} is no numeric op", node.op.name()),
        }
    }

    /// `dst = a op [slot of input 1]`, for an input the allocator left in its slot.
    fn alu_slot(&mut self, value: Value, op: AluOp, ty: Type, output: Register, a: Register) {
        let address = self.input_slot(self.func.inputs(value)[1]);
        if output != a {
            self.asm.mov_rr(width(ty), gpr(output), gpr(a));
        }
        self.asm.alu_rm(op, width(ty), gpr(output), &address);
    }

    fn add(&mut self, value: Value, ty: Type, output: Register, a: Register, b: Register) {
        let (dst, a_register) = (gpr(output), gpr(a));
        if b == SLOT_INPUT {
            self.alu_slot(value, AluOp::Add, ty, output, a);
        } else if b == CONSTANT_INPUT {
            let immediate = self.immediate(value, 1) as i32;
            if output == a {
                self.asm.add_ri(width(ty), dst, immediate);
            } else {
                self.asm.lea(width(ty), dst, &Address::new(a_register, immediate));
            }
        } else if output == a {
            self.asm.add_rr(width(ty), dst, gpr(b));
        } else if output == b {
            self.asm.add_rr(width(ty), dst, a_register);
        } else {
            self.asm.lea(
                width(ty),
                dst,
                &Address::indexed(a_register, gpr(b), crate::asm::Scale::One, 0),
            );
        }
    }

    fn sub(&mut self, value: Value, ty: Type, output: Register, a: Register, b: Register) {
        let (dst, a_register) = (gpr(output), gpr(a));
        if b == SLOT_INPUT {
            self.alu_slot(value, AluOp::Sub, ty, output, a);
        } else if b == CONSTANT_INPUT {
            let immediate = self.immediate(value, 1) as i32;
            if output != a {
                self.asm.mov_rr(width(ty), dst, a_register);
            }
            self.asm.sub_ri(width(ty), dst, immediate);
        } else if output == a {
            self.asm.sub_rr(width(ty), dst, gpr(b));
        } else if output == b {
            self.asm.neg(width(ty), dst);
            self.asm.add_rr(width(ty), dst, a_register);
        } else {
            self.asm.mov_rr(width(ty), dst, a_register);
            self.asm.sub_rr(width(ty), dst, gpr(b));
        }
    }

    fn logic(&mut self, value: Value, op: AluOp, ty: Type, output: Register, a: Register, b: Register) {
        let (dst, a_register) = (gpr(output), gpr(a));
        if b == SLOT_INPUT {
            self.alu_slot(value, op, ty, output, a);
        } else if b == CONSTANT_INPUT {
            let immediate = self.immediate(value, 1) as i32;
            if output != a {
                self.asm.mov_rr(width(ty), dst, a_register);
            }
            self.asm.alu_ri(op, width(ty), dst, immediate);
        } else if output == a {
            self.asm.alu_rr(op, width(ty), dst, gpr(b));
        } else if output == b {
            self.asm.alu_rr(op, width(ty), dst, a_register);
        } else {
            self.asm.mov_rr(width(ty), dst, a_register);
            self.asm.alu_rr(op, width(ty), dst, gpr(b));
        }
    }

    /// Division and remainder: the dividend is in rax, the quotient goes to rax and the remainder to rdx.
    fn divide(&mut self, value: Value, ty: Type, divisor: Register) {
        let node = *self.func.node(value);
        let divisor = gpr(divisor);
        let w = width(ty);
        let rax = Gpr(crate::convention::RAX);
        let rdx = Gpr(crate::convention::RDX);
        let checked = node.flags & FLAG_CANNOT_TRAP == 0;
        if checked {
            let zero = self.trap_label(TRAP_INTEGER_DIVIDE_BY_ZERO);
            self.asm.test_rr(w, divisor, divisor);
            self.asm.jcc(Cond::Equal, zero);
        }
        match node.op {
            Op::DivU | Op::RemU => {
                self.asm.xor_rr(Width::W32, rdx, rdx);
                self.asm.div(w, divisor);
            }
            Op::DivS => {
                if checked {
                    // INT_MIN / -1 overflows.
                    let divide = self.asm.new_label();
                    let overflow = self.trap_label(TRAP_INTEGER_OVERFLOW);
                    self.asm.cmp_ri(w, divisor, -1);
                    self.asm.jcc(Cond::NotEqual, divide);
                    if ty == Type::I64 {
                        self.asm.mov_ri(SCRATCH, 1 << 63);
                        self.asm.cmp_rr(w, rax, SCRATCH);
                    } else {
                        self.asm.cmp_ri(w, rax, i32::MIN);
                    }
                    self.asm.jcc(Cond::Equal, overflow);
                    self.asm.bind(divide);
                }
                self.asm.sign_extend_rax_into_rdx(w);
                self.asm.idiv(w, divisor);
            }
            _ => {
                if checked {
                    // INT_MIN % -1 overflows idiv, but is 0.
                    let divide = self.asm.new_label();
                    let done = self.asm.new_label();
                    self.asm.cmp_ri(w, divisor, -1);
                    self.asm.jcc(Cond::NotEqual, divide);
                    self.asm.xor_rr(Width::W32, rdx, rdx);
                    self.asm.jmp(done);
                    self.asm.bind(divide);
                    self.asm.sign_extend_rax_into_rdx(w);
                    self.asm.idiv(w, divisor);
                    self.asm.bind(done);
                } else {
                    self.asm.sign_extend_rax_into_rdx(w);
                    self.asm.idiv(w, divisor);
                }
            }
        }
    }

    /// Loads a float type's constant `bits` into `register`.
    fn load_float_constant(&mut self, ty: Type, bits: u64, register: Fpr) {
        let label = self.constant_label(u128::from(bits));
        self.asm.vmovs_r_label(precision(ty), register, label);
    }

    fn min_max(&mut self, minimum: bool, ty: Type, output: Register, a: Register, b: Register) {
        let p = precision(ty);
        let (dst, a, b) = (fpr(output), fpr(a), fpr(b));
        let nan = self.asm.new_label();
        let ordinary = self.asm.new_label();
        let done = self.asm.new_label();
        self.asm.vucomis(p, a, b);
        self.asm.jcc(Cond::Parity, nan);
        self.asm.jcc(Cond::NotEqual, ordinary);
        // Equal values: only the signs of zeros may differ, and min takes the negative one.
        let op = if minimum { BitwiseOp::Or } else { BitwiseOp::And };
        self.asm.vbitwise_rrr(op, dst, a, b);
        self.asm.jmp(done);
        self.asm.bind(nan);
        self.asm.vfloat_rrr(FloatOp::Add, p, dst, a, b);
        self.asm.jmp(done);
        self.asm.bind(ordinary);
        let op = if minimum { FloatOp::Min } else { FloatOp::Max };
        self.asm.vfloat_rrr(op, p, dst, a, b);
        self.asm.bind(done);
    }

    fn float_compare(&mut self, value: Value, output: Register, a: Register, b: Register, temp: Register) {
        let node = *self.func.node(value);
        let ty = self.func.node(node.args[0]).ty;
        let p = precision(ty);
        let (dst, a, b) = (gpr(output), fpr(a), fpr(b));
        self.asm.xor_rr(Width::W32, dst, dst);
        match FloatCondition::from_flags(node.flags) {
            FloatCondition::Eq | FloatCondition::Ne => {
                let equal = FloatCondition::from_flags(node.flags) == FloatCondition::Eq;
                let temp = gpr(temp);
                self.asm.xor_rr(Width::W32, temp, temp);
                self.asm.vucomis(p, a, b);
                if equal {
                    self.asm.setcc(Cond::Equal, dst);
                    self.asm.setcc(Cond::NotParity, temp);
                    self.asm.and_rr(Width::W32, dst, temp);
                } else {
                    self.asm.setcc(Cond::NotEqual, dst);
                    self.asm.setcc(Cond::Parity, temp);
                    self.asm.or_rr(Width::W32, dst, temp);
                }
            }
            FloatCondition::Gt => {
                self.asm.vucomis(p, a, b);
                self.asm.setcc(Cond::Above, dst);
            }
            FloatCondition::Ge => {
                self.asm.vucomis(p, a, b);
                self.asm.setcc(Cond::AboveOrEqual, dst);
            }
            FloatCondition::Lt => {
                self.asm.vucomis(p, b, a);
                self.asm.setcc(Cond::Above, dst);
            }
            FloatCondition::Le => {
                self.asm.vucomis(p, b, a);
                self.asm.setcc(Cond::AboveOrEqual, dst);
            }
        }
    }

    /// Compares `source` with the float constant `bound` of type `input` (`vucomis source, bound`).
    fn compare_with_constant(&mut self, input: Type, source: Fpr, bound: f64, temp: Fpr) {
        self.load_float_constant(input, float_bits(input, bound), temp);
        self.asm.vucomis(precision(input), source, temp);
    }

    /// The trapping truncations of floats to integers.
    fn truncate(&mut self, value: Value, ty: Type, output: Register, source: Register, temp: Register) {
        let node = *self.func.node(value);
        let input = self.func.node(node.args[0]).ty;
        let (dst, source, temp) = (gpr(output), fpr(source), fpr(temp));
        let p = precision(input);
        let invalid = self.trap_label(TRAP_INVALID_CONVERSION);
        let overflow = self.trap_label(TRAP_INTEGER_OVERFLOW);
        self.asm.vucomis(p, source, source);
        self.asm.jcc(Cond::Parity, invalid);
        let signed = node.op == Op::TruncS;
        let bits = if ty == Type::I64 { 64 } else { 32 };
        let limit = 2f64.powi(if signed { bits - 1 } else { bits });
        // The lower bound: values at or below it overflow (or below it, where it is exact).
        let (lower, below_only) = match (signed, bits, input) {
            (true, 32, Type::F64) => (-limit - 1.0, false),
            (true, _, _) => (-limit, true),
            (false, _, _) => (-1.0, false),
        };
        self.compare_with_constant(input, source, lower, temp);
        self.asm
            .jcc(if below_only { Cond::Below } else { Cond::BelowOrEqual }, overflow);
        self.compare_with_constant(input, source, limit, temp);
        self.asm.jcc(Cond::AboveOrEqual, overflow);
        if signed {
            self.asm.vcvtts2si(p, width(ty), dst, source);
        } else if ty == Type::I32 {
            self.asm.vcvtts2si(p, Width::W64, dst, source);
        } else {
            self.truncate_u64(p, input, dst, source, temp);
        }
    }

    /// Truncates `source`, which is not negative and below 2^64, to a u64.
    fn truncate_u64(&mut self, p: Precision, input: Type, dst: Gpr, source: Fpr, temp: Fpr) {
        let large = self.asm.new_label();
        let done = self.asm.new_label();
        self.compare_with_constant(input, source, 2f64.powi(63), temp);
        self.asm.jcc(Cond::AboveOrEqual, large);
        self.asm.vcvtts2si(p, Width::W64, dst, source);
        self.asm.jmp(done);
        self.asm.bind(large);
        self.asm.vfloat_rrr(FloatOp::Sub, p, temp, source, temp);
        self.asm.vcvtts2si(p, Width::W64, dst, temp);
        self.asm.mov_ri(SCRATCH, 1 << 63);
        self.asm.xor_rr(Width::W64, dst, SCRATCH);
        self.asm.bind(done);
    }

    /// The saturating truncations of floats to integers.
    fn truncate_saturating(&mut self, value: Value, ty: Type, output: Register, source: Register, temp: Register) {
        let node = *self.func.node(value);
        let input = self.func.node(node.args[0]).ty;
        let (dst, source, temp) = (gpr(output), fpr(source), fpr(temp));
        let p = precision(input);
        let w = width(ty);
        let done = self.asm.new_label();
        let zero = self.asm.new_label();
        let maximum = self.asm.new_label();
        if node.op == Op::TruncSatS {
            // The conversion gives INT_MIN for NaN and out of range values; only some of those want another result.
            self.asm.vcvtts2si(p, w, dst, source);
            if ty == Type::I64 {
                self.asm.mov_ri(SCRATCH, 1 << 63);
                self.asm.cmp_rr(w, dst, SCRATCH);
            } else {
                self.asm.cmp_ri(w, dst, i32::MIN);
            }
            self.asm.jcc(Cond::NotEqual, done);
            self.asm.vucomis(p, source, source);
            self.asm.jcc(Cond::Parity, zero);
            self.asm.vbitwise_rrr(BitwiseOp::Xor, temp, temp, temp);
            self.asm.vucomis(p, source, temp);
            self.asm.jcc(Cond::Below, done);
            self.asm.bind(maximum);
            if ty == Type::I64 {
                self.asm.mov_ri(dst, i64::MAX as u64);
            } else {
                self.asm.mov_ri32(dst, i32::MAX as u32);
            }
            self.asm.jmp(done);
        } else {
            self.asm.vbitwise_rrr(BitwiseOp::Xor, temp, temp, temp);
            self.asm.vucomis(p, source, temp);
            self.asm.jcc(Cond::Parity, zero);
            self.asm.jcc(Cond::Below, zero);
            let bits = if ty == Type::I64 { 64 } else { 32 };
            self.compare_with_constant(input, source, 2f64.powi(bits), temp);
            self.asm.jcc(Cond::AboveOrEqual, maximum);
            if ty == Type::I32 {
                self.asm.vcvtts2si(p, Width::W64, dst, source);
            } else {
                self.truncate_u64(p, input, dst, source, temp);
            }
            self.asm.jmp(done);
            self.asm.bind(maximum);
            if ty == Type::I64 {
                self.asm.mov_ri(dst, u64::MAX);
            } else {
                self.asm.mov_ri32(dst, u32::MAX);
            }
            self.asm.jmp(done);
        }
        self.asm.bind(zero);
        self.asm.xor_rr(Width::W32, dst, dst);
        self.asm.bind(done);
    }

    fn convert_unsigned(&mut self, value: Value, ty: Type, output: Register, source: Register, temp: Register) {
        let node = *self.func.node(value);
        let input = self.func.node(node.args[0]).ty;
        let (dst, source) = (fpr(output), gpr(source));
        let p = precision(ty);
        self.asm.vbitwise_rrr(BitwiseOp::Xor, dst, dst, dst);
        if input == Type::I32 {
            // A zero-extended u32 is a nonnegative i64.
            self.asm.vcvtsi2s(p, Width::W64, dst, dst, source);
            return;
        }
        let temp = gpr(temp);
        let large = self.asm.new_label();
        let done = self.asm.new_label();
        self.asm.test_rr(Width::W64, source, source);
        self.asm.jcc(Cond::Sign, large);
        self.asm.vcvtsi2s(p, Width::W64, dst, dst, source);
        self.asm.jmp(done);
        self.asm.bind(large);
        // Halve, keeping the low bit for rounding, convert, and double.
        self.asm.mov_rr(Width::W64, temp, source);
        self.asm.shr_ri(Width::W64, temp, 1);
        self.asm.mov_rr(Width::W32, SCRATCH, source);
        self.asm.and_ri(Width::W32, SCRATCH, 1);
        self.asm.or_rr(Width::W64, temp, SCRATCH);
        self.asm.vcvtsi2s(p, Width::W64, dst, dst, temp);
        self.asm.vfloat_rrr(FloatOp::Add, p, dst, dst, dst);
        self.asm.bind(done);
    }
}
