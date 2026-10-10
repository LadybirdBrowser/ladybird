/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lowering of number ops: unboxing and boxing of numbers, double
//! arithmetic and comparisons, conversions, and `Math` functions of one
//! double. `Repr::Float64` values are in floating point registers.

use crate::asm::Condition;
use crate::asm::DoubleCondition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::codegen::Codegen;
use crate::codegen::InputValue;
use crate::ir::BinaryOp;
use crate::ir::Comparison;
use crate::ir::Float64UnaryOp;
use crate::ir::NodeId;
use crate::ir::value;

const SIGN_BIT: u64 = 1 << 63;

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// `UnboxDouble`: the double of a tagged double, its bits.
    pub(in crate::codegen) fn emit_unbox_double(&mut self, node: NodeId) {
        let (input, output) = (self.input(node, 0), self.float_output(node));
        self.masm.move_gpr_to_double(output, input);
    }

    /// `CheckNumber`: the double of a tagged number, which exits for other
    /// values.
    pub(in crate::codegen) fn emit_check_number(&mut self, node: NodeId) {
        let exit = self.exit_site(node);
        let (input, output, scratch) = (self.input(node, 0), self.float_output(node), self.pinned.scratch);
        let (int32, double, done) = (self.masm.new_label(), self.masm.new_label(), self.masm.new_label());
        self.branch_on_tag(Condition::Equal, input, value::INT32_TAG, scratch, int32);
        self.emit_double_test(input, double, exit);
        self.masm.bind(double);
        self.masm.move_gpr_to_double(output, input);
        self.masm.jump(done);
        self.masm.bind(int32);
        self.masm.convert_int32_to_double(output, input);
        self.masm.bind(done);
    }

    /// Jumps to `yes` if the tagged `value` is a double, and to `no`
    /// otherwise.
    pub(in crate::codegen) fn emit_double_test(&mut self, value: Gpr, yes: Label, no: Label) {
        let scratch = self.pinned.scratch;
        self.masm.shr64_imm(scratch, value, value::TAG_SHIFT);
        // NB: Canonical NaN has the tag bits other values have set.
        self.masm
            .branch32_imm(Condition::Equal, scratch, i32::from(value::BASE_TAG), yes);
        self.masm.and32_imm(scratch, scratch, u32::from(value::BASE_TAG));
        self.masm
            .branch32_imm(Condition::NotEqual, scratch, i32::from(value::BASE_TAG), yes);
        self.masm.jump(no);
    }

    /// `BoxFloat64`: the tagged number of a double.
    pub(in crate::codegen) fn emit_box_float64(&mut self, node: NodeId) {
        let (input, output) = (self.float_input(node, 0), self.output(node));
        self.box_number(output, input);
    }

    /// `Float64Unary`: `op` of a double. Floor and ceil are machine
    /// instructions. Round rounds up from the floor for fractions of at
    /// least a half, and a zero result of it has the sign of the input,
    /// which makes -0 where `Math` does.
    pub(in crate::codegen) fn emit_float64_unary(&mut self, node: NodeId, op: Float64UnaryOp) {
        let (input, output) = (self.float_input(node, 0), self.float_output(node));
        match op {
            Float64UnaryOp::Abs => self.masm.abs_double(output, input),
            Float64UnaryOp::Negate => self.masm.neg_double(output, input),
            Float64UnaryOp::Sqrt => self.masm.sqrt_double(output, input),
            Float64UnaryOp::Floor => self.masm.floor_double(output, input),
            Float64UnaryOp::Ceil => self.masm.ceil_double(output, input),
            Float64UnaryOp::Round => {
                let (fraction, constant) = (Self::lowering_fpr(0), Self::lowering_fpr(1));
                let (rounded, done) = (self.masm.new_label(), self.masm.new_label());
                let scratch = self.pinned.scratch;
                self.masm.floor_double(output, input);
                // NB: The difference of a double and its floor is exact; it
                //     is NaN for infinities and NaN, which are their own floor.
                self.masm.sub_double(fraction, input, output);
                self.masm.move_double_imm(constant, 0.5);
                self.masm
                    .branch_double(DoubleCondition::LessThanOrUnordered, fraction, constant, rounded);
                self.masm.move_double_imm(constant, 1.0);
                self.masm.add_double(output, output, constant);
                self.masm.bind(rounded);
                // NB: The bits of +0 are zero.
                self.masm.move_double_to_gpr(scratch, output);
                self.masm.branch_test64(Condition::NonZero, scratch, u64::MAX, done);
                self.masm.move_double_to_gpr(scratch, input);
                self.masm.and64_imm(scratch, scratch, SIGN_BIT);
                self.masm.move_gpr_to_double(output, scratch);
                self.masm.bind(done);
            }
        }
    }

    /// `Float64Binary`: `op` of two doubles.
    pub(in crate::codegen) fn emit_float64_binary(&mut self, node: NodeId, op: BinaryOp) {
        let (lhs, rhs, output) = (
            self.float_input(node, 0),
            self.float_input(node, 1),
            self.float_output(node),
        );
        match op {
            BinaryOp::Add => self.masm.add_double(output, lhs, rhs),
            BinaryOp::Sub => self.masm.sub_double(output, lhs, rhs),
            BinaryOp::Mul => self.masm.mul_double(output, lhs, rhs),
            BinaryOp::Div => self.masm.div_double(output, lhs, rhs),
            _ => unreachable!("only Add, Sub, Mul and Div are double operations"),
        }
    }

    /// `Float64Compare`: `comparison` of two doubles, as a boolean. Only
    /// the inequalities hold for NaN.
    pub(in crate::codegen) fn emit_float64_compare(&mut self, node: NodeId, comparison: Comparison) {
        let (lhs, rhs, output) = (self.float_input(node, 0), self.float_input(node, 1), self.output(node));
        self.masm
            .compare_double_set(double_condition(comparison), output, lhs, rhs);
    }

    /// `Float64ToInt32`: ToInt32 of a double. Doubles in the int64 range
    /// truncate to an int64 whose low half it is. For the others, x - floor(x
    /// / 2^32) * 2^32 is exact (they are integers, and every step is), and in
    /// that range; it is NaN for NaN and the infinities, whose truncation has
    /// a zero low half.
    pub(in crate::codegen) fn emit_float64_to_int32(&mut self, node: NodeId) {
        let (input, output) = (self.float_input(node, 0), self.output(node));
        let (large, done) = (self.masm.new_label(), self.masm.new_label());
        // NB: Truncations out of range result in the smallest or (on some
        //     targets) the largest int64.
        self.masm.truncate_double_to_int64(output, input);
        self.masm.branch64_imm(Condition::Equal, output, i64::MIN, large);
        self.masm.branch64_imm(Condition::Equal, output, i64::MAX, large);
        self.masm.move32(output, output);
        self.masm.jump(done);
        self.masm.bind(large);
        let (multiple, constant) = (Self::lowering_fpr(0), Self::lowering_fpr(1));
        self.masm.move_double_imm(constant, 2f64.powi(-32));
        self.masm.mul_double(multiple, input, constant);
        self.masm.floor_double(multiple, multiple);
        self.masm.move_double_imm(constant, 2f64.powi(32));
        self.masm.mul_double(multiple, multiple, constant);
        self.masm.sub_double(multiple, input, multiple);
        self.masm.truncate_double_to_int64(output, multiple);
        self.masm.move32(output, output);
        self.masm.bind(done);
    }

    /// `Uint32ShiftRight`: the bits of the unsigned result of `>>>`, which
    /// shifts by the low five bits of the amount.
    pub(in crate::codegen) fn emit_uint32_shift_right(&mut self, node: NodeId) {
        let (lhs, output) = (self.input(node, 0), self.output(node));
        match self.input_value(node, 1) {
            InputValue::Register(amount) => self.masm.shr32(output, lhs, amount),
            InputValue::Constant(bits) => self.masm.shr32_imm(output, lhs, (bits & 31) as u8),
        }
    }

    /// `Uint32ToFloat64`: the double of the bits of an int32, as an unsigned
    /// integer.
    pub(in crate::codegen) fn emit_uint32_to_float64(&mut self, node: NodeId) {
        let (input, output) = (self.input(node, 0), self.float_output(node));
        let scratch = self.pinned.scratch;
        self.masm.move32(scratch, input);
        self.masm.convert_int64_to_double(output, scratch);
    }
}

/// The condition of a comparison of doubles: false if either is NaN, but
/// for the inequalities.
pub(in crate::codegen) fn double_condition(comparison: Comparison) -> DoubleCondition {
    match comparison {
        Comparison::LessThan => DoubleCondition::LessThan,
        Comparison::LessThanEquals => DoubleCondition::LessThanOrEqual,
        Comparison::GreaterThan => DoubleCondition::GreaterThan,
        Comparison::GreaterThanEquals => DoubleCondition::GreaterThanOrEqual,
        Comparison::StrictlyEquals | Comparison::LooselyEquals => DoubleCondition::Equal,
        Comparison::StrictlyInequals | Comparison::LooselyInequals => DoubleCondition::NotEqualOrUnordered,
    }
}
