/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Arithmetic, comparison and update instructions, built as IR from the
//! interpreter's arithmetic feedback.
//!
//! An instruction whose operands were only ever int32 values (and whose
//! results always fit) is built as int32 nodes: its operands are checked to
//! be int32 values once (unless they already are unboxed int32 values), and
//! the operation itself exits where the result does not fit. Frame slots
//! then hold the unboxed int32 result, so the next int32 operation uses it
//! as it is; readers that need a tagged value box it. Comparisons that jump
//! branch on the raw comparison result.
//!
//! Other instructions take one path per kind of operands their feedback
//! saw: int32 values, numbers (as doubles) and strings. A path starts with
//! branches on the kinds of the operands, whose refinements its plain int32,
//! double and string nodes take. Operands of other kinds go on to the next
//! path, and from the last one to the instruction's slow path, in a cold
//! block. The paths join with the instruction's values. An instruction
//! whose feedback saw none of those kinds runs as a generic node.

use super::Flow;
use super::GenericInfo;
use super::GraphBuilder;
use super::checks::SlowPaths;
use crate::CompileFailure;
use crate::bytecode::Instruction;
use crate::bytecode::Label;
use crate::bytecode::Operand;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::ir::BinaryOp;
use crate::ir::BranchCondition;
use crate::ir::Comparison;
use crate::ir::Float64UnaryOp;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::TypeofKind;
use crate::ir::value;
use crate::snapshot::arith_feedback;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnaryOp {
    Plus,
    Minus,
    BitwiseNot,
    ToInt32,
}

/// Whether an update instruction adds or subtracts one.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UpdateOp {
    Increment,
    Decrement,
}

impl UpdateOp {
    /// The operation that applies the update with an operand of one.
    fn binary_op(self) -> BinaryOp {
        match self {
            UpdateOp::Increment => BinaryOp::Add,
            UpdateOp::Decrement => BinaryOp::Sub,
        }
    }
}

/// An operand of an operation: a frame slot, or an int32 immediate from
/// the instruction.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OperationInput {
    Operand(Operand),
    Int32(i32),
}

/// What a comparison does with its result.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    /// Writes it to the operand, as a boolean.
    Value(Operand),
    /// Jumps to the first label if it is true, and to the second otherwise.
    Jump(Label, Label),
}

/// What an arithmetic, comparison or update instruction computes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Operation {
    /// `dst = lhs op rhs`.
    Binary {
        op: BinaryOp,
        dst: Operand,
        lhs: OperationInput,
        rhs: OperationInput,
    },
    /// `dst = op src`.
    Unary { op: UnaryOp, dst: Operand, src: Operand },
    /// `src = src ± 1`, and for the postfix forms first `old_value =
    /// ToNumeric(src)`.
    Update {
        op: UpdateOp,
        src: Operand,
        old_value: Option<Operand>,
    },
    /// `lhs comparison rhs`.
    Compare {
        comparison: Comparison,
        lhs: Operand,
        rhs: OperationInput,
        outcome: Outcome,
    },
}

impl Operation {
    /// The operation of an instruction, if it is one.
    pub fn of(instruction: &Instruction) -> Option<Self> {
        use Comparison as C;
        use Instruction as I;
        use OperationInput::Int32;
        use OperationInput::Operand as Slot;
        let binary = |op, dst, lhs, rhs| Some(Operation::Binary { op, dst, lhs, rhs });
        let compare = |comparison, lhs, rhs, outcome| {
            Some(Operation::Compare {
                comparison,
                lhs,
                rhs,
                outcome,
            })
        };
        macro_rules! compare_jumps {
            ($($comparison:ident: $slots:ident, $loop_slots:ident, $int32:ident, $loop_int32:ident;)*) => {
                match *instruction {
                    $(
                        I::$slots { lhs, rhs, true_target, false_target, .. }
                        | I::$loop_slots { lhs, rhs, true_target, false_target, .. } => {
                            return compare(C::$comparison, lhs, Slot(rhs), Outcome::Jump(true_target, false_target));
                        }
                        I::$int32 { lhs, rhs, true_target, false_target, .. }
                        | I::$loop_int32 { lhs, rhs, true_target, false_target, .. } => {
                            return compare(C::$comparison, lhs, Int32(rhs), Outcome::Jump(true_target, false_target));
                        }
                    )*
                    _ => {}
                }
            };
        }
        compare_jumps! {
            LessThan: JumpLessThan, JumpLessThanLoop, JumpLessThanRhsInt32, JumpLessThanLoopRhsInt32;
            LessThanEquals: JumpLessThanEquals, JumpLessThanEqualsLoop, JumpLessThanEqualsRhsInt32,
                JumpLessThanEqualsLoopRhsInt32;
            GreaterThan: JumpGreaterThan, JumpGreaterThanLoop, JumpGreaterThanRhsInt32, JumpGreaterThanLoopRhsInt32;
            GreaterThanEquals: JumpGreaterThanEquals, JumpGreaterThanEqualsLoop, JumpGreaterThanEqualsRhsInt32,
                JumpGreaterThanEqualsLoopRhsInt32;
            StrictlyEquals: JumpStrictlyEquals, JumpStrictlyEqualsLoop, JumpStrictlyEqualsRhsInt32,
                JumpStrictlyEqualsLoopRhsInt32;
            StrictlyInequals: JumpStrictlyInequals, JumpStrictlyInequalsLoop, JumpStrictlyInequalsRhsInt32,
                JumpStrictlyInequalsLoopRhsInt32;
            LooselyEquals: JumpLooselyEquals, JumpLooselyEqualsLoop, JumpLooselyEqualsRhsInt32,
                JumpLooselyEqualsLoopRhsInt32;
            LooselyInequals: JumpLooselyInequals, JumpLooselyInequalsLoop, JumpLooselyInequalsRhsInt32,
                JumpLooselyInequalsLoopRhsInt32;
        }
        macro_rules! compares {
            ($($comparison:ident: $slots:ident, $int32:ident;)*) => {
                match *instruction {
                    $(
                        I::$slots { dst, lhs, rhs, .. } => {
                            return compare(C::$comparison, lhs, Slot(rhs), Outcome::Value(dst));
                        }
                        I::$int32 { dst, lhs, rhs, .. } => {
                            return compare(C::$comparison, lhs, Int32(rhs), Outcome::Value(dst));
                        }
                    )*
                    _ => {}
                }
            };
        }
        compares! {
            LessThan: LessThan, LessThanRhsInt32;
            LessThanEquals: LessThanEquals, LessThanEqualsRhsInt32;
            GreaterThan: GreaterThan, GreaterThanRhsInt32;
            GreaterThanEquals: GreaterThanEquals, GreaterThanEqualsRhsInt32;
            StrictlyEquals: StrictlyEquals, StrictlyEqualsRhsInt32;
            StrictlyInequals: StrictlyInequals, StrictlyInequalsRhsInt32;
            LooselyEquals: LooselyEquals, LooselyEqualsRhsInt32;
            LooselyInequals: LooselyInequals, LooselyInequalsRhsInt32;
        }
        let unary = |op, dst, src| Some(Operation::Unary { op, dst, src });
        let update = |op, src, old_value| Some(Operation::Update { op, src, old_value });
        match *instruction {
            I::Add { dst, lhs, rhs, .. } => binary(BinaryOp::Add, dst, Slot(lhs), Slot(rhs)),
            I::AddLhsInt32 { dst, lhs, rhs, .. } => binary(BinaryOp::Add, dst, Int32(lhs), Slot(rhs)),
            I::AddRhsInt32 { dst, lhs, rhs, .. } => binary(BinaryOp::Add, dst, Slot(lhs), Int32(rhs)),
            I::Sub { dst, lhs, rhs, .. } => binary(BinaryOp::Sub, dst, Slot(lhs), Slot(rhs)),
            I::SubRhsInt32 { dst, lhs, rhs, .. } => binary(BinaryOp::Sub, dst, Slot(lhs), Int32(rhs)),
            I::Mul { dst, lhs, rhs, .. } => binary(BinaryOp::Mul, dst, Slot(lhs), Slot(rhs)),
            I::MulRhsInt32 { dst, lhs, rhs, .. } => binary(BinaryOp::Mul, dst, Slot(lhs), Int32(rhs)),
            I::Div { dst, lhs, rhs, .. } => binary(BinaryOp::Div, dst, Slot(lhs), Slot(rhs)),
            I::DivRhsInt32 { dst, lhs, rhs, .. } => binary(BinaryOp::Div, dst, Slot(lhs), Int32(rhs)),
            I::Mod { dst, lhs, rhs, .. } => binary(BinaryOp::Mod, dst, Slot(lhs), Slot(rhs)),
            I::ModRhsInt32 { dst, lhs, rhs, .. } => binary(BinaryOp::Mod, dst, Slot(lhs), Int32(rhs)),
            I::BitwiseAnd { dst, lhs, rhs, .. } => binary(BinaryOp::BitwiseAnd, dst, Slot(lhs), Slot(rhs)),
            I::BitwiseAndRhsInt32 { dst, lhs, rhs, .. } => binary(BinaryOp::BitwiseAnd, dst, Slot(lhs), Int32(rhs)),
            I::BitwiseOr { dst, lhs, rhs, .. } => binary(BinaryOp::BitwiseOr, dst, Slot(lhs), Slot(rhs)),
            I::BitwiseOrRhsInt32 { dst, lhs, rhs, .. } => binary(BinaryOp::BitwiseOr, dst, Slot(lhs), Int32(rhs)),
            I::BitwiseXor { dst, lhs, rhs, .. } => binary(BinaryOp::BitwiseXor, dst, Slot(lhs), Slot(rhs)),
            I::BitwiseXorRhsInt32 { dst, lhs, rhs, .. } => binary(BinaryOp::BitwiseXor, dst, Slot(lhs), Int32(rhs)),
            I::LeftShift { dst, lhs, rhs, .. } => binary(BinaryOp::LeftShift, dst, Slot(lhs), Slot(rhs)),
            I::LeftShiftRhsInt32 { dst, lhs, rhs, .. } => binary(BinaryOp::LeftShift, dst, Slot(lhs), Int32(rhs)),
            I::RightShift { dst, lhs, rhs, .. } => binary(BinaryOp::RightShift, dst, Slot(lhs), Slot(rhs)),
            I::RightShiftRhsInt32 { dst, lhs, rhs, .. } => binary(BinaryOp::RightShift, dst, Slot(lhs), Int32(rhs)),
            I::UnsignedRightShift { dst, lhs, rhs, .. } => {
                binary(BinaryOp::UnsignedRightShift, dst, Slot(lhs), Slot(rhs))
            }
            I::UnsignedRightShiftRhsInt32 { dst, lhs, rhs, .. } => {
                binary(BinaryOp::UnsignedRightShift, dst, Slot(lhs), Int32(rhs))
            }
            I::UnaryPlus { dst, src, .. } => unary(UnaryOp::Plus, dst, src),
            I::UnaryMinus { dst, src, .. } => unary(UnaryOp::Minus, dst, src),
            I::BitwiseNot { dst, src, .. } => unary(UnaryOp::BitwiseNot, dst, src),
            I::ToInt32 { dst, value, .. } => unary(UnaryOp::ToInt32, dst, value),
            I::Increment { dst, .. } => update(UpdateOp::Increment, dst, None),
            I::Decrement { dst, .. } => update(UpdateOp::Decrement, dst, None),
            I::PostfixIncrement { dst, src, .. } => update(UpdateOp::Increment, src, Some(dst)),
            I::PostfixDecrement { dst, src, .. } => update(UpdateOp::Decrement, src, Some(dst)),
            _ => None,
        }
    }
}

impl GraphBuilder<'_> {
    /// The arithmetic feedback of the instruction being built.
    fn arith_feedback(&self, instruction: &Instruction) -> Option<u8> {
        let slot = instruction.feedback_slots().arith?;
        self.function.executable.feedback.arith.get(usize::from(slot)).copied()
    }

    /// Builds an arithmetic, comparison or update instruction.
    pub(super) fn build_operation(
        &mut self,
        instruction: &Instruction,
        info: GenericInfo,
        operation: Operation,
    ) -> Result<Flow, CompileFailure> {
        if let Some(flow) = self.try_build_int32(instruction, operation)? {
            return Ok(flow);
        }
        if let Operation::Compare {
            comparison,
            lhs,
            rhs: OperationInput::Operand(rhs),
            outcome,
        } = operation
        {
            if let Some(flow) = self.try_build_typeof_comparison(comparison, lhs, rhs, outcome)? {
                return Ok(flow);
            }
            if let Some(flow) = self.try_build_identity_equality(instruction, comparison, lhs, rhs, outcome)? {
                return Ok(flow);
            }
        }
        let feedback = self.arith_feedback(instruction).unwrap_or(0);
        // NB: The slow paths of arithmetic that only ever saw numbers make
        //     numbers too, which join the other paths' numbers as such.
        let numbers = feedback & (arith_feedback::STRING | arith_feedback::BIG_INT | arith_feedback::OTHER) == 0
            && self.may_speculate(ExitKind::BadType);
        match operation {
            Operation::Binary { op, dst, lhs, rhs } => {
                let (lhs, rhs) = (self.read_input(lhs)?, self.read_input(rhs)?);
                let Some(paths) = self.build_binary_paths(op, lhs, rhs, feedback)? else {
                    return self.build_generic(instruction, info);
                };
                let numbers = numbers && !op.is_bitwise();
                let [result] = self.join_hot_paths_with(paths, |builder, outputs| {
                    builder.slow_path_numbers(numbers, outputs, &[dst])
                })[..] else {
                    unreachable!("binary operations have one value")
                };
                self.write(dst, result)?;
            }
            Operation::Unary { op, dst, src } => {
                let value = self.read_value(src)?;
                let Some(paths) = self.build_unary_paths(op, value, feedback)? else {
                    return self.build_generic(instruction, info);
                };
                let numbers = numbers && matches!(op, UnaryOp::Plus | UnaryOp::Minus);
                let [result] = self.join_hot_paths_with(paths, |builder, outputs| {
                    builder.slow_path_numbers(numbers, outputs, &[dst])
                })[..] else {
                    unreachable!("unary operations have one value")
                };
                self.write(dst, result)?;
            }
            Operation::Update { op, src, old_value } => {
                let value = self.read_value(src)?;
                let Some(paths) = self.build_update_paths(op, value, old_value.is_some(), feedback)? else {
                    return self.build_generic(instruction, info);
                };
                let destinations = old_value.into_iter().chain([src]).collect::<Vec<_>>();
                let values = self.join_hot_paths_with(paths, |builder, outputs| {
                    builder.slow_path_numbers(numbers, outputs, &destinations)
                });
                match (values.as_slice(), old_value) {
                    ([new_value], None) => self.write(src, *new_value)?,
                    ([old, new_value], Some(dst)) => {
                        self.write(dst, *old)?;
                        self.write(src, *new_value)?;
                    }
                    _ => unreachable!("updates have one value, and postfix ones the old value too"),
                }
            }
            Operation::Compare {
                comparison,
                lhs,
                rhs,
                outcome,
            } => {
                let (lhs, rhs) = (self.read_value(lhs)?, self.read_input(rhs)?);
                // NB: A comparison's slow path makes a boolean, a jump's the
                //     pc it jumps to.
                let slow_repr = match outcome {
                    Outcome::Value(_) => Repr::Tagged,
                    Outcome::Jump(..) => Repr::Int32,
                };
                let Some(paths) = self.build_comparison_paths(comparison, lhs, rhs, feedback, slow_repr)? else {
                    return self.build_generic(instruction, info);
                };
                let result = match outcome {
                    Outcome::Value(_) => {
                        let [result] = self.join_hot_paths_with(paths, |builder, outputs| {
                            let truth = builder.constant(value::TRUE);
                            vec![builder.emit(
                                Op::TaggedEquals { equal: true },
                                vec![outputs[0], truth],
                                Some(Repr::Bool),
                            )]
                        })[..] else {
                            unreachable!("comparisons have one value")
                        };
                        result
                    }
                    Outcome::Jump(true_target, _) => {
                        let [result] = self.join_hot_paths_with(paths, |builder, outputs| {
                            let true_pc = builder.int32_constant(true_target.0.cast_signed());
                            vec![builder.emit(
                                Op::Int32Compare {
                                    comparison: Comparison::StrictlyEquals,
                                },
                                vec![outputs[0], true_pc],
                                Some(Repr::Bool),
                            )]
                        })[..] else {
                            unreachable!("comparisons have one value")
                        };
                        result
                    }
                };
                return self.finish_comparison(result, outcome);
            }
        }
        Ok(Flow::Continue)
    }

    /// The values the slow path of an arithmetic instruction writes to
    /// `destinations`, `outputs`, as the numbers its other paths make
    /// (boxed doubles) if `numbers`: the code exits after the instruction
    /// where they are no numbers (like bigints).
    fn slow_path_numbers(&mut self, numbers: bool, outputs: Vec<NodeId>, destinations: &[Operand]) -> Vec<NodeId> {
        if !numbers {
            return outputs;
        }
        // NB: The exit continues after the instruction, which wrote the
        //     outputs.
        let frame = self.frame.clone();
        for (destination, output) in destinations.iter().zip(&outputs) {
            self.write(*destination, *output)
                .expect("the instruction's destinations are in the frame");
        }
        let frame_state = self.resume_after_frame_state(Operand::INVALID);
        self.frame = frame;
        outputs
            .into_iter()
            .map(|output| {
                let number = self.emit(Op::CheckNumber, vec![output], Some(Repr::Float64));
                self.graph.nodes[number.index()].frame_state = Some(frame_state);
                self.emit(Op::BoxFloat64, vec![number], Some(Repr::Tagged))
            })
            .collect()
    }

    // Int32 speculation.

    /// Whether the instruction being built may speculate that its operands
    /// are int32 values, and its result too if `can_overflow`.
    fn may_speculate_int32(&self, instruction: &Instruction, can_overflow: bool) -> bool {
        self.arith_feedback(instruction) == Some(arith_feedback::INT32)
            && self.may_speculate(ExitKind::NotInt32)
            && (!can_overflow || self.may_speculate(ExitKind::Overflow))
    }

    /// The `Repr::Int32` form of `value`, checked to be an int32 if it is
    /// a tagged value.
    pub(super) fn int32(&mut self, value: NodeId) -> NodeId {
        let node = self.graph.node(value);
        match (&node.op, node.repr) {
            (_, Some(Repr::Int32)) => value,
            (Op::BoxInt32, _) => node.inputs[0],
            (Op::Constant(bits), _) if let Some(integer) = value::as_int32(*bits) => self.int32_constant(integer),
            _ => self.emit_checked(Op::CheckInt32, vec![value], Some(Repr::Int32)),
        }
    }

    fn read_int32(&mut self, input: OperationInput) -> Result<NodeId, CompileFailure> {
        match input {
            OperationInput::Operand(operand) => {
                let value = self.read_value(operand)?;
                Ok(self.int32(value))
            }
            OperationInput::Int32(integer) => Ok(self.int32_constant(integer)),
        }
    }

    /// The value of an input, in whatever representation the frame holds
    /// it.
    fn read_input(&mut self, input: OperationInput) -> Result<NodeId, CompileFailure> {
        match input {
            OperationInput::Operand(operand) => self.read_value(operand),
            OperationInput::Int32(integer) => Ok(self.int32_constant(integer)),
        }
    }

    /// `lhs op rhs` of two `Repr::Int32` values, which exits where the
    /// result is not an int32.
    pub(super) fn int32_binary(&mut self, op: BinaryOp, lhs: NodeId, rhs: NodeId) -> NodeId {
        let node = self.emit(Op::Int32Binary { op }, vec![lhs, rhs], Some(Repr::Int32));
        if op.int32_can_overflow() {
            let frame_state = self.eager_frame_state();
            self.graph.nodes[node.index()].frame_state = Some(frame_state);
        }
        node
    }

    /// Builds an operation as int32 operations, if its feedback says it
    /// only ever saw int32 values. Returns `None`, having built nothing,
    /// otherwise.
    fn try_build_int32(
        &mut self,
        instruction: &Instruction,
        operation: Operation,
    ) -> Result<Option<Flow>, CompileFailure> {
        match operation {
            Operation::Binary { op, dst, lhs, rhs } => {
                if op == BinaryOp::Div || !self.may_speculate_int32(instruction, op.int32_can_overflow()) {
                    return Ok(None);
                }
                let lhs = self.read_int32(lhs)?;
                let rhs = self.read_int32(rhs)?;
                let result = self.int32_binary(op, lhs, rhs);
                self.write(dst, result)?;
            }
            Operation::Unary { op, dst, src } => {
                if !self.may_speculate_int32(instruction, op == UnaryOp::Minus) {
                    return Ok(None);
                }
                let value = self.read_int32(OperationInput::Operand(src))?;
                let result = match op {
                    UnaryOp::Plus | UnaryOp::ToInt32 => value,
                    UnaryOp::BitwiseNot => {
                        let all_ones = self.int32_constant(-1);
                        self.int32_binary(BinaryOp::BitwiseXor, value, all_ones)
                    }
                    // NB: The product exits for 0, whose negation is -0.
                    UnaryOp::Minus => {
                        let minus_one = self.int32_constant(-1);
                        self.int32_binary(BinaryOp::Mul, value, minus_one)
                    }
                };
                self.write(dst, result)?;
            }
            Operation::Update { op, src, old_value } => {
                if !self.may_speculate_int32(instruction, true) {
                    return Ok(None);
                }
                // NB: ToNumeric of an int32 is the int32 itself.
                let value = self.read_int32(OperationInput::Operand(src))?;
                let result = self.int32_update(op, value);
                if let Some(dst) = old_value {
                    self.write(dst, value)?;
                }
                self.write(src, result)?;
            }
            Operation::Compare {
                comparison,
                lhs,
                rhs,
                outcome,
            } => {
                if !self.may_speculate_int32(instruction, false) {
                    return Ok(None);
                }
                let lhs = self.read_int32(OperationInput::Operand(lhs))?;
                let rhs = self.read_int32(rhs)?;
                let result = self.emit(Op::Int32Compare { comparison }, vec![lhs, rhs], Some(Repr::Bool));
                return self.finish_comparison(result, outcome).map(Some);
            }
        }
        Ok(Some(Flow::Continue))
    }

    fn int32_update(&mut self, op: UpdateOp, value: NodeId) -> NodeId {
        let one = self.int32_constant(1);
        self.int32_binary(op.binary_op(), value, one)
    }

    // Comparisons that need no paths.

    /// Builds a strict equality where one operand is a constant compared
    /// by identity, or whose feedback only ever saw operands that are no
    /// numbers, strings or bigints (on at least one side) as a check of
    /// that, as a comparison of the operands' bits. Returns `None`, having
    /// built nothing, otherwise.
    fn try_build_identity_equality(
        &mut self,
        instruction: &Instruction,
        comparison: Comparison,
        lhs: Operand,
        rhs: Operand,
        outcome: Outcome,
    ) -> Result<Option<Flow>, CompileFailure> {
        let equal = match comparison {
            Comparison::StrictlyEquals => true,
            Comparison::StrictlyInequals => false,
            _ => return Ok(None),
        };
        let lhs = self.read(lhs)?;
        let rhs = self.read(rhs)?;
        // NB: Constants that are compared by identity need no check.
        let constant_compares_by_identity = [lhs, rhs].iter().any(|value| {
            self.graph.constant_value(*value).is_some_and(|bits| {
                matches!(
                    value::tag(bits),
                    value::BOOLEAN_TAG | value::UNDEFINED_TAG | value::NULL_TAG | value::OBJECT_TAG
                )
            })
        });
        let lhs = if constant_compares_by_identity {
            lhs
        } else {
            if self.arith_feedback(instruction) != Some(arith_feedback::OTHER) || !self.may_speculate(ExitKind::BadType)
            {
                return Ok(None);
            }
            self.emit_checked(Op::CheckIdentityComparable, vec![lhs, rhs], None)
        };
        let result = self.emit(Op::TaggedEquals { equal }, vec![lhs, rhs], Some(Repr::Bool));
        self.finish_comparison(result, outcome).map(Some)
    }

    /// `typeof x === "kind"`, its negation and its loose forms, and their
    /// jumps, where the string is a constant: a test of the kind of `x`
    /// that never makes the string `typeof` results in.
    fn try_build_typeof_comparison(
        &mut self,
        comparison: Comparison,
        lhs: Operand,
        rhs: Operand,
        outcome: Outcome,
    ) -> Result<Option<Flow>, CompileFailure> {
        // NB: `typeof` results in a string, which is loosely equal to a
        //     string exactly when it is strictly equal to it.
        let Some(equal) = comparison.equality() else {
            return Ok(None);
        };
        let strings = self.runtime.layout.typeof_strings;
        if strings.number == 0 {
            return Ok(None);
        }
        let (lhs, rhs) = (self.read(lhs)?, self.read(rhs)?);
        let (value, bits) = match (&self.graph.node(lhs).op, &self.graph.node(rhs).op) {
            (Op::Typeof, _) if let Some(bits) = self.graph.constant_value(rhs) => {
                (self.graph.node(lhs).inputs[0], bits)
            }
            (_, Op::Typeof) if let Some(bits) = self.graph.constant_value(lhs) => {
                (self.graph.node(rhs).inputs[0], bits)
            }
            _ => return Ok(None),
        };
        let kinds = [
            (strings.number, TypeofKind::Number),
            (strings.undefined, TypeofKind::Undefined),
            (strings.object, TypeofKind::Object),
            (strings.string, TypeofKind::String),
            (strings.symbol, TypeofKind::Symbol),
            (strings.boolean, TypeofKind::Boolean),
            (strings.bigint, TypeofKind::Bigint),
            (strings.function, TypeofKind::Function),
        ];
        let Some(&(_, kind)) = kinds.iter().find(|(string, _)| *string == bits) else {
            return Ok(None);
        };
        self.assume_no_htmldda_objects();
        let result = self.emit(Op::TypeofIs { kind, equal }, vec![value], Some(Repr::Bool));
        self.finish_comparison(result, outcome).map(Some)
    }

    /// Ends a comparison built as the `Repr::Bool` `result`: with a branch
    /// to the targets of a jump, or by writing its boolean.
    fn finish_comparison(&mut self, result: NodeId, outcome: Outcome) -> Result<Flow, CompileFailure> {
        match outcome {
            Outcome::Jump(true_target, false_target) => {
                let if_true = self.block_for_label(true_target)?;
                let if_false = self.block_for_label(false_target)?;
                self.end_with_branch(
                    |if_true, if_false| Op::Branch {
                        condition: BranchCondition::Bool,
                        if_true,
                        if_false,
                    },
                    vec![result],
                    if_true,
                    if_false,
                )?;
                Ok(Flow::Ended)
            }
            Outcome::Value(dst) => {
                let boolean = self.emit(Op::BoxBool, vec![result], Some(Repr::Tagged));
                self.write(dst, boolean)?;
                Ok(Flow::Continue)
            }
        }
    }

    // Operands on paths.

    /// The tagged int32 constant `value` is, if it is one.
    fn int32_constant_of(&self, value: NodeId) -> Option<i32> {
        let bits = self.graph.constant_value(value)?;
        if self.graph.node(value).repr != Some(Repr::Tagged) {
            return None;
        }
        value::as_int32(bits)
    }

    /// The bits of the double the tagged constant `value` is, if it is one.
    fn double_constant_of(&self, value: NodeId) -> Option<u64> {
        let bits = self.graph.constant_value(value)?;
        (self.graph.node(value).repr == Some(Repr::Tagged) && value::is_double(bits)).then_some(bits)
    }

    /// Whether `values` may all be int32 values: they are no constants of
    /// other kinds.
    fn may_be_int32(&self, values: &[NodeId]) -> bool {
        values.iter().all(|value| {
            self.graph.constant_value(*value).is_none()
                || self.int32_value_of(*value).is_some()
                || self.int32_constant_of(*value).is_some()
        })
    }

    /// Whether `values` may all be numbers: they are no constants of other
    /// kinds.
    fn may_be_number(&self, values: &[NodeId]) -> bool {
        values
            .iter()
            .all(|value| self.may_be_int32(&[*value]) || self.double_constant_of(*value).is_some())
    }

    /// `value` as a `Repr::Int32`, where it is an int32: a test sends other
    /// values on.
    fn int32_on_path(&mut self, paths: &mut SlowPaths, value: NodeId) -> NodeId {
        if let Some(integer) = self.int32_value_of(value) {
            return integer;
        }
        if let Some(integer) = self.int32_constant_of(value) {
            return self.int32_constant(integer);
        }
        let refined = self.branch_to_next_case(paths, BranchCondition::Int32Value, vec![value], true);
        self.emit(Op::UnboxInt32, vec![refined], Some(Repr::Int32))
    }

    /// `value` as a `Repr::Float64`, where it is a number: tests send other
    /// values on.
    fn float64_on_path(&mut self, paths: &mut SlowPaths, value: NodeId) -> NodeId {
        self.float64_of(value, |builder, condition, inputs| {
            builder.branch_to_next_case(paths, condition, inputs, true)
        })
    }

    /// `value` as a `Repr::Float64`, where it is a number: `otherwise`
    /// branches on what it is (see `on_int32_or_double()`).
    pub(super) fn float64_of(
        &mut self,
        value: NodeId,
        otherwise: impl FnOnce(&mut Self, BranchCondition, Vec<NodeId>) -> NodeId,
    ) -> NodeId {
        if self.graph.node(value).repr == Some(Repr::Float64) {
            return value;
        }
        if let Some(integer) = self.int32_value_of(value) {
            return self.int32_to_float64(integer);
        }
        if let Some(integer) = self.int32_constant_of(value) {
            return self.typed_constant(f64::from(integer).to_bits(), Repr::Float64);
        }
        if let Some(bits) = self.double_constant_of(value) {
            return self.typed_constant(bits, Repr::Float64);
        }
        self.on_int32_or_double(
            value,
            Repr::Float64,
            Self::int32_to_float64,
            |_, number| number,
            otherwise,
        )
    }

    /// ToInt32 of `value`, as a `Repr::Int32`, where it is an int32 or, if
    /// `doubles`, a double: tests send other values on.
    fn int32_of_number_on_path(&mut self, paths: &mut SlowPaths, value: NodeId, doubles: bool) -> NodeId {
        if !doubles || self.int32_value_of(value).is_some() || self.int32_constant_of(value).is_some() {
            return self.int32_on_path(paths, value);
        }
        self.on_int32_or_double(
            value,
            Repr::Int32,
            |_, integer| integer,
            |builder, number| builder.emit(Op::Float64ToInt32, vec![number], Some(Repr::Int32)),
            |builder, condition, inputs| builder.branch_to_next_case(paths, condition, inputs, true),
        )
    }

    /// Whether int32 operations that exit where their results are no int32
    /// fit an instruction with `feedback`.
    fn may_speculate_int32_results(&self, feedback: u8) -> bool {
        feedback & arith_feedback::INT32 != 0
            && feedback & arith_feedback::INT32_OVERFLOW == 0
            && self.may_speculate(ExitKind::Overflow)
    }

    // The paths of each kind of operation.

    fn build_binary_paths(
        &mut self,
        op: BinaryOp,
        lhs: NodeId,
        rhs: NodeId,
        feedback: u8,
    ) -> Result<Option<SlowPaths>, CompileFailure> {
        let int32_results =
            self.may_speculate_int32_results(feedback) && op != BinaryOp::Div && self.may_be_int32(&[lhs, rhs]);
        if op.is_bitwise() {
            let doubles = feedback & arith_feedback::DOUBLE != 0 || !self.may_be_int32(&[lhs, rhs]);
            if feedback & (arith_feedback::SAW_INT32 | arith_feedback::DOUBLE) == 0 || !self.may_be_number(&[lhs, rhs])
            {
                return Ok(None);
            }
            let mut paths = self.start_slow_paths(Some(Repr::Tagged))?;
            let lhs = self.int32_of_number_on_path(&mut paths, lhs, doubles);
            let rhs = self.int32_of_number_on_path(&mut paths, rhs, doubles);
            let result = if op == BinaryOp::UnsignedRightShift && !int32_results {
                let bits = self.emit(Op::Uint32ShiftRight, vec![lhs, rhs], Some(Repr::Int32));
                let number = self.emit(Op::Uint32ToFloat64, vec![bits], Some(Repr::Float64));
                self.emit(Op::BoxFloat64, vec![number], Some(Repr::Tagged))
            } else {
                let result = self.int32_binary(op, lhs, rhs);
                self.tagged(result)
            };
            self.end_hot_path(&mut paths, vec![result]);
            return Ok(Some(paths));
        }
        if op == BinaryOp::Mod {
            if !int32_results {
                return Ok(None);
            }
            let mut paths = self.start_slow_paths(Some(Repr::Tagged))?;
            let (lhs, rhs) = (self.int32_on_path(&mut paths, lhs), self.int32_on_path(&mut paths, rhs));
            let result = self.int32_binary(op, lhs, rhs);
            let result = self.tagged(result);
            self.end_hot_path(&mut paths, vec![result]);
            return Ok(Some(paths));
        }
        let numbers = (feedback & (arith_feedback::DOUBLE | arith_feedback::INT32_OVERFLOW) != 0
            || (feedback & arith_feedback::INT32 != 0 && !int32_results))
            && self.may_be_number(&[lhs, rhs]);
        let strings = op == BinaryOp::Add
            && feedback & arith_feedback::STRING != 0
            && self.runtime.rope_allocation.allocator != 0;
        if !int32_results && !numbers && !strings {
            return Ok(None);
        }
        let mut paths = self.start_slow_paths(Some(Repr::Tagged))?;
        let mut started = false;
        if int32_results {
            let (lhs, rhs) = (self.int32_on_path(&mut paths, lhs), self.int32_on_path(&mut paths, rhs));
            let result = self.int32_binary(op, lhs, rhs);
            let result = self.tagged(result);
            self.end_hot_path(&mut paths, vec![result]);
            started = true;
        }
        if numbers && (!started || self.start_next_case(&mut paths)) {
            let (lhs, rhs) = (
                self.float64_on_path(&mut paths, lhs),
                self.float64_on_path(&mut paths, rhs),
            );
            let result = self.emit(Op::Float64Binary { op }, vec![lhs, rhs], Some(Repr::Float64));
            let result = self.emit(Op::BoxFloat64, vec![result], Some(Repr::Tagged));
            self.end_hot_path(&mut paths, vec![result]);
            started = true;
        }
        if strings && (!started || self.start_next_case(&mut paths)) {
            let integers = feedback & arith_feedback::SAW_INT32 != 0 && self.runtime.layout.numeric_string_cache != 0;
            self.build_concatenation_paths(&mut paths, lhs, rhs, integers);
        }
        Ok(Some(paths))
    }

    /// The paths of an `Add` of strings: two strings, and if `integers`, a
    /// string and an integer whose string the VM has.
    fn build_concatenation_paths(&mut self, paths: &mut SlowPaths, lhs: NodeId, rhs: NodeId, integers: bool) {
        let (lhs, rhs) = (self.tagged(lhs), self.tagged(rhs));
        let string = self.branch_to_next_case(paths, BranchCondition::String, vec![lhs], true);
        let other = self.string_on_path(paths, rhs, integers);
        self.finish_concatenation(paths, string, other);
        if integers && self.start_next_case(paths) {
            let string = self.branch_to_next_case(paths, BranchCondition::String, vec![rhs], true);
            let other = self.string_on_path(paths, lhs, true);
            self.finish_concatenation(paths, other, string);
        }
    }

    /// The string `value` is, or if `integers`, the string of the integer
    /// it is, where the VM has that: tests send other values on.
    fn string_on_path(&mut self, paths: &mut SlowPaths, value: NodeId, integers: bool) -> NodeId {
        if !integers {
            return self.branch_to_next_case(paths, BranchCondition::String, vec![value], true);
        }
        let (string_block, other) = self.branch_both_ways(BranchCondition::String, vec![value]);
        self.block = string_block;
        let string = self.refine(BranchCondition::String, true, vec![value]);
        let string_end = self.block;

        self.block = other;
        let integer = self.int32_on_path(paths, value);
        let converted = self.emit(Op::IntegerToString, vec![integer], Some(Repr::Tagged));
        let converted = self.test_not_empty(paths, converted);
        let converted_end = self.block;

        self.join_blocks(vec![string_end, converted_end]);
        self.add_phi(vec![string, converted])
    }

    /// `value`, which is the empty value where only the slow path can make
    /// it: a test sends that on.
    fn test_not_empty(&mut self, paths: &mut SlowPaths, value: NodeId) -> NodeId {
        let empty = self.constant(value::EMPTY);
        self.branch_to_next_case(
            paths,
            BranchCondition::TaggedEquals { equal: false },
            vec![value, empty],
            true,
        )
    }

    fn finish_concatenation(&mut self, paths: &mut SlowPaths, lhs: NodeId, rhs: NodeId) {
        let result = self.emit(Op::ConcatenateStrings, vec![lhs, rhs], Some(Repr::Tagged));
        let result = self.test_not_empty(paths, result);
        self.end_hot_path(paths, vec![result]);
    }

    fn build_unary_paths(
        &mut self,
        op: UnaryOp,
        value: NodeId,
        feedback: u8,
    ) -> Result<Option<SlowPaths>, CompileFailure> {
        let int32s = feedback & arith_feedback::SAW_INT32 != 0;
        let doubles = feedback & arith_feedback::DOUBLE != 0;
        if !int32s && !doubles {
            return Ok(None);
        }
        let mut paths = self.start_slow_paths(Some(Repr::Tagged))?;
        match op {
            UnaryOp::Plus => {
                // NB: ToNumber of a number is the number itself.
                if self.int32_value_of(value).is_some() || self.graph.node(value).repr == Some(Repr::Float64) {
                    let result = self.tagged(value);
                    self.end_hot_path(&mut paths, vec![result]);
                    return Ok(Some(paths));
                }
                let mut started = false;
                for (seen, condition) in [
                    (int32s, BranchCondition::Int32Value),
                    (doubles, BranchCondition::Double),
                ] {
                    if seen && (!started || self.start_next_case(&mut paths)) {
                        let number = self.branch_to_next_case(&mut paths, condition, vec![value], true);
                        self.end_hot_path(&mut paths, vec![number]);
                        started = true;
                    }
                }
            }
            UnaryOp::Minus => {
                let int32_results = self.may_speculate_int32_results(feedback);
                if int32_results {
                    // NB: The product exits for 0, whose negation is -0.
                    let integer = self.int32_on_path(&mut paths, value);
                    let minus_one = self.int32_constant(-1);
                    let result = self.int32_binary(BinaryOp::Mul, integer, minus_one);
                    let result = self.tagged(result);
                    self.end_hot_path(&mut paths, vec![result]);
                }
                if doubles || !int32_results {
                    if int32_results && !self.start_next_case(&mut paths) {
                        return Ok(Some(paths));
                    }
                    let number = self.float64_on_path(&mut paths, value);
                    let result = self.emit(
                        Op::Float64Unary {
                            op: Float64UnaryOp::Negate,
                        },
                        vec![number],
                        Some(Repr::Float64),
                    );
                    let result = self.emit(Op::BoxFloat64, vec![result], Some(Repr::Tagged));
                    self.end_hot_path(&mut paths, vec![result]);
                }
            }
            UnaryOp::BitwiseNot | UnaryOp::ToInt32 => {
                let integer = self.int32_of_number_on_path(&mut paths, value, doubles);
                let result = if op == UnaryOp::BitwiseNot {
                    let all_ones = self.int32_constant(-1);
                    self.int32_binary(BinaryOp::BitwiseXor, integer, all_ones)
                } else {
                    integer
                };
                let result = self.tagged(result);
                self.end_hot_path(&mut paths, vec![result]);
            }
        }
        Ok(Some(paths))
    }

    /// The paths of an update of `value`, whose values are its new value
    /// and, if `postfix`, first the old one.
    fn build_update_paths(
        &mut self,
        op: UpdateOp,
        value: NodeId,
        postfix: bool,
        feedback: u8,
    ) -> Result<Option<SlowPaths>, CompileFailure> {
        let int32_results = self.may_speculate_int32_results(feedback);
        let numbers = feedback & (arith_feedback::DOUBLE | arith_feedback::INT32_OVERFLOW) != 0
            || (feedback & arith_feedback::INT32 != 0 && !int32_results);
        if !int32_results && !numbers {
            return Ok(None);
        }
        // NB: ToNumeric of a number is the number itself.
        let old_value = self.tagged(value);
        let values = |new_value| {
            if postfix {
                vec![old_value, new_value]
            } else {
                vec![new_value]
            }
        };
        let mut paths = self.start_slow_paths(Some(Repr::Tagged))?;
        if int32_results {
            let integer = self.int32_on_path(&mut paths, value);
            let result = self.int32_update(op, integer);
            let result = self.tagged(result);
            self.end_hot_path(&mut paths, values(result));
        }
        if numbers && (!int32_results || self.start_next_case(&mut paths)) {
            let number = self.float64_on_path(&mut paths, value);
            let one = self.typed_constant(1f64.to_bits(), Repr::Float64);
            let result = self.emit(
                Op::Float64Binary { op: op.binary_op() },
                vec![number, one],
                Some(Repr::Float64),
            );
            let result = self.emit(Op::BoxFloat64, vec![result], Some(Repr::Tagged));
            self.end_hot_path(&mut paths, values(result));
        }
        Ok(Some(paths))
    }

    /// The paths of a comparison, whose value is a `Repr::Bool`.
    fn build_comparison_paths(
        &mut self,
        comparison: Comparison,
        lhs: NodeId,
        rhs: NodeId,
        feedback: u8,
        slow_repr: Repr,
    ) -> Result<Option<SlowPaths>, CompileFailure> {
        let equality = comparison.equality();
        // NB: `x == null`: undefined, null and [[IsHTMLDDA]] objects are loosely
        //     equal to undefined and null, and nothing else is.
        if let Some(equal) = equality
            && comparison.is_loose()
            && self.runtime.no_htmldda_objects
        {
            let nullish = |builder: &Self, value: NodeId| {
                builder
                    .graph
                    .constant_value(value)
                    .is_some_and(|bits| bits == value::UNDEFINED || bits == value::NULL)
            };
            let other = if nullish(self, rhs) {
                Some(lhs)
            } else if nullish(self, lhs) {
                Some(rhs)
            } else {
                None
            };
            if let Some(other) = other {
                self.assume_no_htmldda_objects();
                let other = self.tagged(other);
                let mut paths = self.start_slow_paths(Some(slow_repr))?;
                self.branch_to_next_case(&mut paths, BranchCondition::Nullish, vec![other], true);
                let truth = self.typed_constant(u64::from(equal), Repr::Bool);
                self.end_hot_path(&mut paths, vec![truth]);
                self.start_next_case(&mut paths);
                let truth = self.typed_constant(u64::from(!equal), Repr::Bool);
                self.end_hot_path(&mut paths, vec![truth]);
                return Ok(Some(paths));
            }
        }
        let int32s = feedback & arith_feedback::INT32 != 0 && self.may_be_int32(&[lhs, rhs]);
        let doubles = feedback & arith_feedback::DOUBLE != 0 && self.may_be_number(&[lhs, rhs]);
        let with_int32 = [lhs, rhs]
            .iter()
            .any(|value| self.int32_value_of(*value).is_some() || self.int32_constant_of(*value).is_some());
        let strings = equality.is_some() && feedback & arith_feedback::STRING != 0 && !with_int32;
        // NB: Strict equality with an int32 is false for anything but
        //     numbers.
        let strict_with_int32 = equality.is_some() && !comparison.is_loose() && with_int32;
        if !int32s && !doubles && !strings && !strict_with_int32 {
            return Ok(None);
        }
        let mut paths = self.start_slow_paths(Some(slow_repr))?;
        let mut started = false;
        if int32s || strict_with_int32 {
            let (lhs, rhs) = (self.int32_on_path(&mut paths, lhs), self.int32_on_path(&mut paths, rhs));
            let result = self.emit(Op::Int32Compare { comparison }, vec![lhs, rhs], Some(Repr::Bool));
            self.end_hot_path(&mut paths, vec![result]);
            started = true;
        }
        if (doubles || strict_with_int32) && (!started || self.start_next_case(&mut paths)) {
            let (lhs, rhs) = (
                self.float64_on_path(&mut paths, lhs),
                self.float64_on_path(&mut paths, rhs),
            );
            let result = self.emit(Op::Float64Compare { comparison }, vec![lhs, rhs], Some(Repr::Bool));
            self.end_hot_path(&mut paths, vec![result]);
            started = true;
        }
        if let Some(equal) = equality
            && strings
            && (!started || self.start_next_case(&mut paths))
        {
            let (lhs, rhs) = (self.tagged(lhs), self.tagged(rhs));
            let lhs = self.branch_to_next_case(&mut paths, BranchCondition::String, vec![lhs], true);
            let rhs = self.branch_to_next_case(&mut paths, BranchCondition::String, vec![rhs], true);
            let result = self.emit(Op::StringsEqual, vec![lhs, rhs], Some(Repr::Tagged));
            let result = self.test_not_empty(&mut paths, result);
            let truth = self.constant(value::TRUE);
            let result = self.emit(Op::TaggedEquals { equal }, vec![result, truth], Some(Repr::Bool));
            self.end_hot_path(&mut paths, vec![result]);
        }
        if strict_with_int32 && self.start_next_case(&mut paths) {
            let truth = self.typed_constant(u64::from(equality == Some(false)), Repr::Bool);
            self.end_hot_path(&mut paths, vec![truth]);
        }
        Ok(Some(paths))
    }
}
