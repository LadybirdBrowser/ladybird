/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lowering of the nodes of speculated representations: checks and
//! conversions between tagged and unboxed values, and int32 arithmetic and
//! comparisons, which exit where the speculation fails.

use super::Codegen;
use super::InputValue;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::PortableMacroAssembler;
use crate::code::Repr;
use crate::ir::BinaryOp;
use crate::ir::Comparison;
use crate::ir::NodeId;
use crate::ir::value;

const BOOLEAN_SHIFTED: u64 = (value::BOOLEAN_TAG as u64) << 48;

/// The condition of an int32 comparison.
pub(super) fn int32_condition(comparison: Comparison) -> Condition {
    match comparison {
        Comparison::LessThan => Condition::LessThan,
        Comparison::LessThanEquals => Condition::LessThanOrEqual,
        Comparison::GreaterThan => Condition::GreaterThan,
        Comparison::GreaterThanEquals => Condition::GreaterThanOrEqual,
        Comparison::StrictlyEquals | Comparison::LooselyEquals => Condition::Equal,
        Comparison::StrictlyInequals | Comparison::LooselyInequals => Condition::NotEqual,
    }
}

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    pub(super) fn emit_check_int32(&mut self, node: NodeId) {
        let exit = self.exit_site(node);
        let (input, output, scratch) = (self.input(node, 0), self.output(node), self.pinned.scratch);
        self.branch_on_tag(Condition::NotEqual, input, value::INT32_TAG, scratch, exit);
        self.masm.move32(output, input);
    }

    pub(super) fn emit_check_identity_comparable(&mut self, node: NodeId) {
        let exit = self.exit_site(node);
        let ok = self.masm.new_label();
        let (scratch, temp) = (self.pinned.scratch, Gpr(self.allocation.node(node).temps[0]));
        for input in 0..2 {
            let value = self.input(node, input);
            self.masm.shr64_imm(scratch, value, 48);
            // Objects and symbols only differ in the second lowest tag bit,
            // undefined and null in the lowest.
            self.masm.or32_imm(temp, scratch, 2);
            self.masm
                .branch32_imm(Condition::Equal, temp, i32::from(value::OBJECT_TAG | 2), ok);
            self.masm
                .branch32_imm(Condition::Equal, scratch, i32::from(value::BOOLEAN_TAG), ok);
            self.masm.or32_imm(temp, scratch, 1);
            self.masm
                .branch32_imm(Condition::Equal, temp, i32::from(value::NULL_TAG), ok);
        }
        self.masm.jump(exit);
        self.masm.bind(ok);
    }

    /// `dst` = the tagged value of the int32 in `int32`.
    pub(super) fn emit_box_int32(&mut self, dst: Gpr, int32: Gpr) {
        self.masm.move32(dst, int32);
        self.masm.or64_imm(dst, dst, value::tagged(value::INT32_TAG));
    }

    /// `dst` = the tagged boolean of the 0 or 1 in `boolean`.
    pub(super) fn emit_box_bool(&mut self, dst: Gpr, boolean: Gpr) {
        self.masm.move32(dst, boolean);
        self.masm.or64_imm(dst, dst, BOOLEAN_SHIFTED);
    }

    /// `dst` = the tagged value of the value of representation `repr` in
    /// register number `register` of either class.
    pub(super) fn emit_box_register(&mut self, dst: Gpr, register: u8, repr: Repr) {
        match super::register_of(register) {
            super::Register::General(register) => self.emit_box(dst, register, repr),
            super::Register::Float(register) => self.box_number(dst, register),
        }
    }

    /// `dst` = the tagged value of `value`, whose representation is `repr`.
    /// `dst` may be `value`.
    pub(super) fn emit_box(&mut self, dst: Gpr, value: Gpr, repr: Repr) {
        match repr {
            Repr::Tagged => {
                if dst != value {
                    self.masm.move64(dst, value);
                }
            }
            Repr::Int32 => self.emit_box_int32(dst, value),
            Repr::Bool => self.emit_box_bool(dst, value),
            Repr::Float64 => {
                let number = Self::lowering_fpr(0);
                self.masm.move_gpr_to_double(number, value);
                self.box_number(dst, number);
            }
            Repr::Pointer => unreachable!("cell addresses are never boxed"),
        }
    }

    pub(super) fn emit_int32_binary(&mut self, node: NodeId, op: BinaryOp) {
        if op == BinaryOp::Mod {
            self.emit_int32_remainder(node);
            return;
        }
        let (lhs, output) = (self.input(node, 0), self.output(node));
        let scratch = self.pinned.scratch;
        let rhs = match self.input_value(node, 1) {
            InputValue::Register(rhs) => rhs,
            InputValue::Constant(bits) => {
                let imm = bits as u32;
                match op {
                    BinaryOp::Add => {
                        let exit = self.exit_site(node);
                        self.masm.branch_add32_imm_overflow(output, lhs, imm as i32, exit);
                    }
                    BinaryOp::Sub => {
                        let exit = self.exit_site(node);
                        self.masm.branch_sub32_imm_overflow(output, lhs, imm as i32, exit);
                    }
                    BinaryOp::BitwiseAnd => self.masm.and32_imm(output, lhs, imm),
                    BinaryOp::BitwiseOr => self.masm.or32_imm(output, lhs, imm),
                    BinaryOp::BitwiseXor => self.masm.xor32_imm(output, lhs, imm),
                    // NB: Shifts use the low five bits of the amount.
                    BinaryOp::LeftShift => self.masm.shl32_imm(output, lhs, (imm & 31) as u8),
                    BinaryOp::RightShift => self.masm.sar32_imm(output, lhs, (imm & 31) as u8),
                    BinaryOp::UnsignedRightShift => {
                        let exit = self.exit_site(node);
                        self.masm.shr32_imm(output, lhs, (imm & 31) as u8);
                        self.masm.branch32_imm(Condition::LessThan, output, 0, exit);
                    }
                    BinaryOp::Mul | BinaryOp::Div | BinaryOp::Mod => {
                        unreachable!("only additive, bitwise and shift operations take immediates")
                    }
                }
                return;
            }
        };
        match op {
            BinaryOp::Add => {
                let exit = self.exit_site(node);
                self.masm.branch_add32_overflow(output, lhs, rhs, exit);
            }
            BinaryOp::Sub => {
                let exit = self.exit_site(node);
                self.masm.branch_sub32_overflow(output, lhs, rhs, exit);
            }
            BinaryOp::Mul => {
                let exit = self.exit_site(node);
                self.masm.branch_mul32_overflow(output, lhs, rhs, exit);
                // A zero product is -0 if either operand is negative.
                let nonzero = self.masm.new_label();
                self.masm.branch_test32(Condition::NonZero, output, u32::MAX, nonzero);
                self.masm.or32(scratch, lhs, rhs);
                self.masm.branch32_imm(Condition::LessThan, scratch, 0, exit);
                self.masm.bind(nonzero);
            }
            BinaryOp::BitwiseAnd => self.masm.and32(output, lhs, rhs),
            BinaryOp::BitwiseOr => self.masm.or32(output, lhs, rhs),
            BinaryOp::BitwiseXor => self.masm.xor32(output, lhs, rhs),
            BinaryOp::LeftShift => self.masm.shl32(output, lhs, rhs),
            BinaryOp::RightShift => self.masm.sar32(output, lhs, rhs),
            BinaryOp::UnsignedRightShift => {
                let exit = self.exit_site(node);
                self.masm.shr32(output, lhs, rhs);
                self.masm.branch32_imm(Condition::LessThan, output, 0, exit);
            }
            BinaryOp::Div => unreachable!("division is no int32 operation"),
            BinaryOp::Mod => unreachable!("remainders are lowered on their own"),
        }
    }

    /// The remainder of int32 values, which exits for a zero divisor and a
    /// zero remainder of a negative dividend (-0). Needs a temp.
    ///
    /// NB: There is no portable integer division here, so the quotient is
    ///     the truncated double quotient, which is exact for int32 operands:
    ///     the double division errs by less than 2^-22 / |rhs|, and a
    ///     quotient that is not an integer is at least 1 / |rhs| away from
    ///     one.
    fn emit_int32_remainder(&mut self, node: NodeId) {
        let (lhs, output) = (self.input(node, 0), self.output(node));
        let (scratch, quotient) = (self.pinned.scratch, self.temp(node, 0));
        let exit = self.exit_site(node);
        let rhs = self.input_value(node, 1);
        let (lhs_number, rhs_number) = (Self::lowering_fpr(0), Self::lowering_fpr(1));
        self.masm.convert_int32_to_double(lhs_number, lhs);
        match rhs {
            InputValue::Register(rhs) => {
                self.masm.branch_test32(Condition::Zero, rhs, u32::MAX, exit);
                self.masm.convert_int32_to_double(rhs_number, rhs);
                self.masm.sign_extend32_to_64(scratch, rhs);
            }
            InputValue::Constant(bits) => {
                let divisor = (bits as u32).cast_signed();
                if divisor == 0 {
                    self.masm.jump(exit);
                }
                self.masm.move_double_imm(rhs_number, f64::from(divisor));
                self.masm.move_imm64(scratch, i64::from(divisor).cast_unsigned());
            }
        }
        self.masm.div_double(lhs_number, lhs_number, rhs_number);
        self.masm.truncate_double_to_int64(quotient, lhs_number);
        self.masm.mul64(quotient, quotient, scratch);
        self.masm.sign_extend32_to_64(scratch, lhs);
        self.masm.sub64(scratch, scratch, quotient);
        let done = self.masm.new_label();
        self.masm.branch_test32(Condition::NonZero, scratch, u32::MAX, done);
        self.masm.branch32_imm(Condition::LessThan, lhs, 0, exit);
        self.masm.bind(done);
        self.masm.move32(output, scratch);
    }

    pub(super) fn emit_int32_compare(&mut self, node: NodeId, comparison: Comparison) {
        let (lhs, output) = (self.input(node, 0), self.output(node));
        let condition = int32_condition(comparison);
        match self.input_value(node, 1) {
            InputValue::Register(rhs) => self.masm.compare32_set(condition, output, lhs, rhs),
            InputValue::Constant(bits) => self.masm.compare32_imm_set(condition, output, lhs, bits as i32),
        }
    }
}
