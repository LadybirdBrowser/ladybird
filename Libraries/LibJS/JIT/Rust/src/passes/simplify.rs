/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Constant folding and strength reduction.
//!
//! - Conversions and int32 operations of constants become constants, and
//!   conversions of conversions the values they started from.
//! - Int32 operations that leave their left-hand side as it is, like
//!   `x | 0`, become that value, and commutative ones take constants as
//!   their right-hand sides.
//! - Branches on constants become jumps; truthiness branches on boxed
//!   booleans test the unboxed boolean, and on boxed int32 values compare
//!   the unboxed value with 0.

use super::edit;
use super::edit::Constants;
use super::edit::Replacements;
use crate::bitset::BitSet;
use crate::code::Repr;
use crate::inline_vec::InlineVec;
use crate::ir::BinaryOp;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::Comparison;
use crate::ir::Graph;
use crate::ir::Node;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::value;

/// The truthiness of a constant, where it does not depend on the heap.
fn constant_truthiness(bits: u64, repr: Repr) -> Option<bool> {
    match repr {
        Repr::Bool => Some(bits != 0),
        Repr::Int32 => Some(bits as u32 != 0),
        Repr::Float64 | Repr::Pointer => None,
        Repr::Tagged => match value::tag(bits) {
            value::BOOLEAN_TAG | value::INT32_TAG => Some(bits as u32 != 0),
            value::UNDEFINED_TAG | value::NULL_TAG => Some(false),
            _ => None,
        },
    }
}

fn int32_of(graph: &Graph, node: NodeId) -> Option<i32> {
    graph.int32_constant(node)
}

/// The value of `node`, if it is a `Repr::Float64` constant.
fn float64_of(graph: &Graph, node: NodeId) -> Option<f64> {
    let bits = graph.constant_value(node)?;
    (graph.node(graph.unrefined(node)).repr == Some(Repr::Float64)).then(|| f64::from_bits(bits))
}

/// ToInt32 of a number.
fn to_int32(number: f64) -> i32 {
    if !number.is_finite() {
        return 0;
    }
    (number.trunc().rem_euclid(4_294_967_296.0) as u32).cast_signed()
}

/// ToUint8Clamp of a number.
fn to_uint8_clamp(number: f64) -> i32 {
    if number.is_nan() {
        return 0;
    }
    number.clamp(0.0, 255.0).round_ties_even() as i32
}

fn fold_int32_binary(op: BinaryOp, lhs: i32, rhs: i32) -> Option<i32> {
    let shift = (rhs & 31) as u32;
    match op {
        BinaryOp::Add => lhs.checked_add(rhs),
        BinaryOp::Sub => lhs.checked_sub(rhs),
        BinaryOp::Mul => lhs
            .checked_mul(rhs)
            .filter(|product| *product != 0 || (lhs >= 0 && rhs >= 0)),
        BinaryOp::BitwiseAnd => Some(lhs & rhs),
        BinaryOp::BitwiseOr => Some(lhs | rhs),
        BinaryOp::BitwiseXor => Some(lhs ^ rhs),
        BinaryOp::LeftShift => Some(lhs.wrapping_shl(shift)),
        BinaryOp::RightShift => Some(lhs.wrapping_shr(shift)),
        BinaryOp::UnsignedRightShift => i32::try_from(lhs.cast_unsigned().wrapping_shr(shift)).ok(),
        // NB: A zero remainder of a negative dividend is -0.
        BinaryOp::Mod => lhs.checked_rem(rhs).filter(|remainder| *remainder != 0 || lhs >= 0),
        BinaryOp::Div => None,
    }
}

fn fold_int32_wrapping_binary(op: BinaryOp, lhs: i32, rhs: i32) -> i32 {
    match op {
        BinaryOp::Add => lhs.wrapping_add(rhs),
        BinaryOp::Sub => lhs.wrapping_sub(rhs),
        BinaryOp::Mul => lhs.wrapping_mul(rhs),
        BinaryOp::UnsignedRightShift => lhs.cast_unsigned().wrapping_shr((rhs & 31) as u32).cast_signed(),
        _ => unreachable!("only additions, subtractions, multiplications and unsigned right shifts wrap"),
    }
}

/// Whether `x op rhs` is `x` for every int32 `x`. An unsigned right shift
/// by 0 is not, unless it wraps: it makes negative values large numbers.
fn is_int32_identity(op: BinaryOp, rhs: i32, wrapping: bool) -> bool {
    match op {
        BinaryOp::Add | BinaryOp::Sub | BinaryOp::BitwiseOr | BinaryOp::BitwiseXor => rhs == 0,
        BinaryOp::LeftShift | BinaryOp::RightShift => rhs & 31 == 0,
        BinaryOp::UnsignedRightShift => wrapping && rhs & 31 == 0,
        BinaryOp::BitwiseAnd => rhs == -1,
        BinaryOp::Mul => rhs == 1,
        BinaryOp::Mod | BinaryOp::Div => false,
    }
}

/// Double arithmetic of int32 values, which ToInt32 of its result can do
/// as wrapping int32 arithmetic instead.
enum Int32Term {
    Value(NodeId),
    Constant(i32),
    Binary(BinaryOp, Box<Int32Term>, Box<Int32Term>),
}

/// How many levels of additions and subtractions `int32_term()` looks
/// through: the exact results of at most 2^8 int32 values are below 2^53,
/// so doubles hold them exactly, and ToInt32 of them is what wrapping int32
/// arithmetic computes.
const MAX_INT32_TERM_DEPTH: u32 = 8;

/// `value`, a `Repr::Float64`, as double arithmetic of int32 values, if it
/// is that: conversions of int32 values, int32 constants, and additions and
/// subtractions of such.
fn int32_term(graph: &Graph, value: NodeId, depth: u32) -> Option<Int32Term> {
    let node = graph.node(graph.unrefined(value));
    match node.op {
        Op::Int32ToFloat64 => Some(Int32Term::Value(node.inputs[0])),
        Op::Constant(bits) if node.repr == Some(Repr::Float64) => {
            let number = f64::from_bits(bits);
            let integer = number as i32;
            (f64::from(integer) == number && !(number == 0.0 && number.is_sign_negative()))
                .then_some(Int32Term::Constant(integer))
        }
        Op::Float64Binary {
            op: op @ (BinaryOp::Add | BinaryOp::Sub),
        } if depth < MAX_INT32_TERM_DEPTH => Some(Int32Term::Binary(
            op,
            Box::new(int32_term(graph, node.inputs[0], depth + 1)?),
            Box::new(int32_term(graph, node.inputs[1], depth + 1)?),
        )),
        _ => None,
    }
}

/// Adds the wrapping int32 arithmetic of `term` to `block` before
/// `position`, returning its value and the position after it.
fn build_int32_term(
    graph: &mut Graph,
    constants: &mut Constants,
    term: &Int32Term,
    block: BlockId,
    position: usize,
    pc: u32,
) -> (NodeId, usize) {
    match term {
        Int32Term::Value(value) => (*value, position),
        Int32Term::Constant(integer) => (
            constants.get_typed(graph, u64::from(integer.cast_unsigned()), Repr::Int32),
            position,
        ),
        Int32Term::Binary(op, lhs, rhs) => {
            let (lhs, position) = build_int32_term(graph, constants, lhs, block, position, pc);
            let (rhs, position) = build_int32_term(graph, constants, rhs, block, position, pc);
            let node = graph.add_node(Node::new(
                Op::Int32WrappingBinary { op: *op },
                vec![lhs, rhs],
                Some(Repr::Int32),
                pc,
            ));
            graph.blocks[block.index()].body.insert(position, node);
            (node, position + 1)
        }
    }
}

/// The double `value` checks to be an int32, if it is a `CheckInt32` of a
/// boxed double.
fn checked_double(graph: &Graph, value: NodeId) -> Option<NodeId> {
    let check = graph.node(value);
    if check.op != Op::CheckInt32 {
        return None;
    }
    let boxed = graph.node(graph.unrefined(check.inputs[0]));
    (boxed.op == Op::BoxFloat64).then(|| boxed.inputs[0])
}

/// Whether a fold may replace an operation with `value`, which leaves
/// `value` standing for the operation's result. An int32 check of a boxed
/// double may not, nor one of what may become a boxed double (a phi that
/// representation selection unboxes, or a boxing of a check that goes): only
/// the check's exit makes the double the result (like ToInt32 of a double
/// for `x | 0`, and +0 for `-0 + 0`), while the check goes where its users
/// all take the double (see `dissolvable_checks()`).
fn may_stand_for_result(graph: &Graph, value: NodeId) -> bool {
    let check = graph.node(graph.unrefined(value));
    check.op != Op::CheckInt32
        || !matches!(
            graph.node(graph.unrefined(check.inputs[0])).op,
            Op::BoxFloat64 | Op::Phi | Op::BoxInt32
        )
}

/// The checks of doubles to be int32 values whose users all take the
/// double instead (see `checked_double()`): comparisons, conversions to
/// doubles, boxings and frame states. The values of doubles are numbers,
/// so the checks guard no conversion with effects, and they go before check
/// elimination could rely on what they established.
fn dissolvable_checks(graph: &Graph) -> BitSet {
    let mut checks = BitSet::new(graph.nodes.len());
    let mut blocked = BitSet::new(graph.nodes.len());
    for block in &graph.blocks {
        for node in block.nodes() {
            let data = graph.node(node);
            if checked_double(graph, node).is_some() {
                checks.insert(node.index());
            }
            let takes_double = match data.op {
                Op::Int32Compare { comparison } => !matches!(
                    comparison,
                    Comparison::Below | Comparison::BelowOrEqual | Comparison::Above | Comparison::AboveOrEqual
                ),
                Op::Int32ToFloat64 | Op::BoxInt32 => true,
                _ => false,
            };
            if !takes_double {
                for input in &data.inputs {
                    blocked.insert(input.index());
                }
            }
        }
    }
    checks.subtract(&blocked);
    checks
}

fn fold_int32_compare(comparison: Comparison, lhs: i32, rhs: i32) -> bool {
    match comparison {
        Comparison::LessThan => lhs < rhs,
        Comparison::LessThanEquals => lhs <= rhs,
        Comparison::GreaterThan => lhs > rhs,
        Comparison::GreaterThanEquals => lhs >= rhs,
        Comparison::StrictlyEquals | Comparison::LooselyEquals => lhs == rhs,
        Comparison::StrictlyInequals | Comparison::LooselyInequals => lhs != rhs,
        Comparison::Below => lhs.cast_unsigned() < rhs.cast_unsigned(),
        Comparison::BelowOrEqual => lhs.cast_unsigned() <= rhs.cast_unsigned(),
        Comparison::Above => lhs.cast_unsigned() > rhs.cast_unsigned(),
        Comparison::AboveOrEqual => lhs.cast_unsigned() >= rhs.cast_unsigned(),
    }
}

/// Whether `condition` holds of the tagged constants `input` (and `other`,
/// for comparisons), if they are constants.
fn constant_condition(graph: &Graph, condition: BranchCondition, input: NodeId, other: Option<NodeId>) -> Option<bool> {
    if graph.node(input).repr != Some(Repr::Tagged) {
        return None;
    }
    if condition == BranchCondition::Int32Value && graph.node(input).op == Op::BoxInt32 {
        return Some(true);
    }
    if condition == BranchCondition::Int32OrBoolean && matches!(graph.node(input).op, Op::BoxInt32 | Op::BoxBool) {
        return Some(true);
    }
    let bits = graph.constant_value(input)?;
    let tag = value::tag(bits);
    match condition {
        BranchCondition::TaggedEquals { equal } => {
            let other = other?;
            if graph.node(other).repr != Some(Repr::Tagged) {
                return None;
            }
            Some((bits == graph.constant_value(other)?) == equal)
        }
        BranchCondition::Nullish => Some(bits == value::UNDEFINED || bits == value::NULL),
        BranchCondition::Undefined => Some(bits == value::UNDEFINED),
        BranchCondition::Object => Some(tag == value::OBJECT_TAG),
        BranchCondition::NonNegativeInt32 => Some(value::as_int32(bits).is_some_and(|integer| integer >= 0)),
        BranchCondition::Int32Value => Some(tag == value::INT32_TAG),
        BranchCondition::Int32OrBoolean => Some(tag == value::INT32_TAG || tag == value::BOOLEAN_TAG),
        BranchCondition::Double => Some(value::is_double(bits)),
        BranchCondition::String => Some(tag == value::STRING_TAG),
        // NB: Constant strings are made as such, but strings may become
        //     resolved when the code runs.
        BranchCondition::Bool
        | BranchCondition::Int32(_)
        | BranchCondition::Float64(_)
        | BranchCondition::ResolvedString
        | BranchCondition::Builtin(_)
        | BranchCondition::PropertyIteratorCacheValid
        | BranchCondition::BindingMutable { .. }
        | BranchCondition::NextBindingOfShape { .. }
        | BranchCondition::ElementsKind(_)
        | BranchCondition::IndexInBounds
        | BranchCondition::MagicalLength
        | BranchCondition::Extensible
        | BranchCondition::HoleFillableArray
        | BranchCondition::ArrayIterator
        | BranchCondition::GeneratorObject
        | BranchCondition::PlainArrayPrototypeChain
        | BranchCondition::ArrayWithRealmArrayPrototypeAndNoNamedProperties
        | BranchCondition::HoldsValidWatchpointSet
        | BranchCondition::Shape(_) => None,
    }
}

/// The number a tagged constant is, if it is one.
fn constant_number(graph: &Graph, node: NodeId) -> Option<f64> {
    if graph.node(node).repr != Some(Repr::Tagged) {
        return None;
    }
    let bits = graph.constant_value(node)?;
    match value::as_int32(bits) {
        Some(integer) => Some(f64::from(integer)),
        None => value::is_double(bits).then(|| f64::from_bits(bits)),
    }
}

pub fn run(graph: &mut Graph) {
    simplify(graph, true);
}

/// Simplification after check elimination, which keeps every int32 check of
/// a double: check elimination may have relied on what one established, like
/// the int32 that an unboxing in a branch it decided takes.
pub fn run_after_check_elimination(graph: &mut Graph) {
    simplify(graph, false);
}

fn simplify(graph: &mut Graph, dissolve_checks: bool) {
    let dissolvable = if dissolve_checks {
        dissolvable_checks(graph)
    } else {
        BitSet::new(0)
    };
    let dissolved_double = |graph: &Graph, value: NodeId| {
        // NB: Nodes the pass adds are no checks.
        (value.index() < dissolvable.capacity() && dissolvable.contains(value.index()))
            .then(|| checked_double(graph, value))
            .flatten()
    };
    let mut constants = Constants::new(graph);
    let mut replacements = Replacements::new(graph);
    let mut removed = BitSet::new(graph.nodes.len());
    let mut changed_control_flow = false;
    for block in 0..graph.blocks.len() {
        let block = BlockId::from_index(block);
        let mut index = 0;
        while index < graph.block(block).body.len() {
            let node = graph.block(block).body[index];
            index += 1;
            if !matches!(
                graph.node(node).op,
                Op::CheckNumber
                    | Op::UnboxDouble
                    | Op::UnboxInt32
                    | Op::EmptyToUndefined
                    | Op::CheckNonNegativeInt32
                    | Op::CheckBounds
                    | Op::Int32ToFloat64
                    | Op::Float64ToInt32
                    | Op::Float64ToUint8Clamped
                    | Op::BoxInt32
                    | Op::BoxBool
                    | Op::CheckInt32
                    | Op::Int32Binary { .. }
                    | Op::Int32WrappingBinary { .. }
                    | Op::Int32Min
                    | Op::Int32Max
                    | Op::Uint32Min
                    | Op::Int32Compare { .. }
            ) {
                continue;
            }
            let inputs = graph
                .node(node)
                .inputs
                .iter()
                .map(|input| replacements.resolve(*input))
                .collect::<InlineVec<NodeId, 4>>();
            // NB: A number check of a boxed int32 value converts the value.
            let converted = (graph.node(node).op == Op::CheckNumber)
                .then(|| graph.node(graph.unrefined(inputs[0])))
                .filter(|unrefined| unrefined.op == Op::BoxInt32)
                .map(|unrefined| replacements.resolve(unrefined.inputs[0]));
            // NB: ToInt32 of double arithmetic of int32 values is wrapping
            //     int32 arithmetic.
            let wrapping = (graph.node(node).op == Op::Float64ToInt32)
                .then(|| int32_term(graph, inputs[0], 0))
                .flatten()
                .filter(|term| matches!(term, Int32Term::Binary(..)));
            // NB: An int32 comparison of a double checked to be an int32
            //     compares the doubles, which needs no check.
            let double_compare = match graph.node(node).op {
                Op::Int32Compare { comparison }
                    if !matches!(
                        comparison,
                        Comparison::Below | Comparison::BelowOrEqual | Comparison::Above | Comparison::AboveOrEqual
                    ) =>
                {
                    let doubles = [dissolved_double(graph, inputs[0]), dissolved_double(graph, inputs[1])];
                    doubles.iter().any(Option::is_some).then_some((comparison, doubles))
                }
                _ => None,
            };
            let constant_bits = |graph: &Graph, input: usize| graph.constant_value(inputs[input]);
            let boxed = |op: Op, input: NodeId| {
                let unrefined = graph.node(graph.unrefined(input));
                (unrefined.op == op).then(|| unrefined.inputs[0])
            };
            let folded = match graph.node(node).op {
                // NB: Numeric constants are numbers.
                Op::CheckNumber if let Some(number) = constant_number(graph, inputs[0]) => {
                    Some(constants.get_typed(graph, number.to_bits(), Repr::Float64))
                }
                // NB: Unboxing a boxed value is the value (where a branch
                //     established the box holds the kind it unboxes).
                Op::UnboxDouble | Op::CheckNumber => boxed(Op::BoxFloat64, inputs[0]),
                Op::UnboxInt32 => boxed(Op::BoxInt32, inputs[0]),
                // NB: Inlined callees return values that representation
                //     selection may have found to be boxes, never empty.
                Op::EmptyToUndefined => graph.is_known_non_empty(inputs[0], 0..0).then_some(inputs[0]),
                Op::CheckNonNegativeInt32 => int32_of(graph, inputs[0])
                    .filter(|value| *value >= 0)
                    .map(|_| inputs[0]),
                Op::CheckBounds => match (int32_of(graph, inputs[0]), int32_of(graph, inputs[1])) {
                    (Some(index), Some(count)) if (index as u32) < count as u32 => Some(inputs[0]),
                    _ => None,
                },
                // NB: The double of a double checked to be an int32 is the
                //     double, which needs no check. A CheckFloat64Int32 stays
                //     where it is, since it may exit.
                Op::Int32ToFloat64 => match graph.node(inputs[0]).op {
                    Op::UnboxInt32 => boxed(Op::BoxFloat64, graph.node(inputs[0]).inputs[0]),
                    Op::CheckFloat64Int32 => Some(graph.node(inputs[0]).inputs[0]),
                    _ => dissolved_double(graph, inputs[0]),
                },
                Op::Float64ToInt32 if let Some(number) = float64_of(graph, inputs[0]) => {
                    Some(constants.get_typed(graph, u64::from(to_int32(number).cast_unsigned()), Repr::Int32))
                }
                Op::Float64ToUint8Clamped => float64_of(graph, inputs[0]).map(|number| {
                    constants.get_typed(graph, u64::from(to_uint8_clamp(number).cast_unsigned()), Repr::Int32)
                }),
                // NB: Both signed and unsigned int32 values are exactly
                //     representable as doubles. ToInt32 recovers their bits.
                Op::Float64ToInt32 => match graph.node(inputs[0]).op {
                    Op::Int32ToFloat64 | Op::Uint32ToFloat64 => {
                        Some(graph.node(inputs[0]).inputs[0]).filter(|integer| may_stand_for_result(graph, *integer))
                    }
                    _ => None,
                },
                // NB: A boxed double checked to be an int32 is the boxing of the
                //     int32, which needs no check.
                Op::BoxInt32 => int32_of(graph, inputs[0])
                    .map(|integer| constants.get(graph, value::int32(integer)))
                    .or_else(|| dissolved_double(graph, inputs[0]).map(|_| graph.node(inputs[0]).inputs[0])),
                Op::BoxBool => constant_bits(graph, 0).map(|bits| {
                    let boolean = if bits != 0 { value::TRUE } else { value::FALSE };
                    constants.get(graph, boolean)
                }),
                // NB: Boxing the double of an int32 boxes the int32.
                Op::CheckInt32
                    if let Some(double) = boxed(Op::BoxFloat64, inputs[0])
                        && graph.node(double).op == Op::Int32ToFloat64 =>
                {
                    Some(graph.node(double).inputs[0])
                }
                Op::CheckInt32 if let Some(integer) = boxed(Op::BoxInt32, inputs[0]) => Some(integer),
                Op::CheckInt32 => constant_bits(graph, 0)
                    .filter(|bits| graph.node(inputs[0]).repr == Some(Repr::Tagged) && value::as_int32(*bits).is_some())
                    .map(|bits| constants.get_typed(graph, bits & u64::from(u32::MAX), Repr::Int32)),
                Op::Int32Binary { op } => match (int32_of(graph, inputs[0]), int32_of(graph, inputs[1])) {
                    (Some(lhs), Some(rhs)) => fold_int32_binary(op, lhs, rhs)
                        .map(|result| constants.get_typed(graph, u64::from(result.cast_unsigned()), Repr::Int32)),
                    (None, Some(rhs))
                        if is_int32_identity(op, rhs, false) && may_stand_for_result(graph, inputs[0]) =>
                    {
                        Some(inputs[0])
                    }
                    _ => None,
                },
                Op::Int32WrappingBinary { op } => match (int32_of(graph, inputs[0]), int32_of(graph, inputs[1])) {
                    (Some(lhs), Some(rhs)) => {
                        let result = fold_int32_wrapping_binary(op, lhs, rhs);
                        Some(constants.get_typed(graph, u64::from(result.cast_unsigned()), Repr::Int32))
                    }
                    (None, Some(rhs)) if is_int32_identity(op, rhs, true) && may_stand_for_result(graph, inputs[0]) => {
                        Some(inputs[0])
                    }
                    _ => None,
                },
                Op::Int32Min | Op::Int32Max | Op::Uint32Min if inputs[0] == inputs[1] => Some(inputs[0]),
                Op::Int32Min | Op::Int32Max | Op::Uint32Min => {
                    match (int32_of(graph, inputs[0]), int32_of(graph, inputs[1])) {
                        (Some(lhs), Some(rhs)) => {
                            let result = match graph.node(node).op {
                                Op::Int32Min => lhs.min(rhs),
                                Op::Int32Max => lhs.max(rhs),
                                _ => lhs.cast_unsigned().min(rhs.cast_unsigned()).cast_signed(),
                            };
                            Some(constants.get_typed(graph, u64::from(result.cast_unsigned()), Repr::Int32))
                        }
                        _ => None,
                    }
                }
                Op::Int32Compare { comparison } => match (int32_of(graph, inputs[0]), int32_of(graph, inputs[1])) {
                    (Some(lhs), Some(rhs)) => Some(constants.get_typed(
                        graph,
                        u64::from(fold_int32_compare(comparison, lhs, rhs)),
                        Repr::Bool,
                    )),
                    _ => None,
                },
                _ => None,
            };
            let folded = folded.or_else(|| {
                let (comparison, doubles) = double_compare?;
                let pc = graph.node(node).pc;
                let mut operands = Vec::new();
                for (input, double) in inputs.iter().zip(doubles) {
                    operands.push(double.unwrap_or_else(|| {
                        let conversion =
                            graph.add_node(Node::new(Op::Int32ToFloat64, vec![*input], Some(Repr::Float64), pc));
                        graph.blocks[block.index()].body.insert(index - 1, conversion);
                        index += 1;
                        conversion
                    }));
                }
                let compare = graph.add_node(Node::new(
                    Op::Float64Compare { comparison },
                    operands,
                    Some(Repr::Bool),
                    pc,
                ));
                graph.blocks[block.index()].body.insert(index - 1, compare);
                index += 1;
                Some(compare)
            });
            let folded = folded.or_else(|| {
                let term = wrapping?;
                let pc = graph.node(node).pc;
                let (result, position) = build_int32_term(graph, &mut constants, &term, block, index - 1, pc);
                index = position + 1;
                Some(result)
            });
            let folded = folded.or_else(|| {
                let integer = converted?;
                if let Some(double) = dissolved_double(graph, integer) {
                    return Some(double);
                }
                let pc = graph.node(node).pc;
                let conversion = graph.add_node(Node::new(Op::Int32ToFloat64, vec![integer], Some(Repr::Float64), pc));
                graph.blocks[block.index()].body.insert(index - 1, conversion);
                index += 1;
                Some(conversion)
            });
            if let Some(constant) = folded {
                replacements.replace(node, constant);
                removed.insert(node.index());
                continue;
            }
            // NB: Once no check of a double goes anymore, an int32 check of a
            //     boxed double checks the double itself, so that only users
            //     that need the boxing keep it.
            if !dissolve_checks
                && graph.node(node).op == Op::CheckInt32
                && let boxing = graph.node(graph.unrefined(inputs[0]))
                && boxing.op == Op::BoxFloat64
            {
                let double = boxing.inputs[0];
                let data = &mut graph.nodes[node.index()];
                data.op = Op::CheckFloat64Int32;
                data.inputs = vec![double];
                continue;
            }
            // NB: Int32 operations take constant right-hand sides as
            //     immediates, so commutative ones get their constants there.
            if let Op::Int32Binary { op } | Op::Int32WrappingBinary { op } = graph.node(node).op
                && op.is_commutative_on_int32()
                && int32_of(graph, inputs[0]).is_some()
                && int32_of(graph, inputs[1]).is_none()
            {
                graph.nodes[node.index()].inputs = vec![inputs[1], inputs[0]];
            }
        }

        // Branches.
        let control = graph.control_id(block);
        let input = graph
            .node(control)
            .inputs
            .first()
            .map(|input| replacements.resolve(*input));
        let new_op = match (graph.node(control).op.clone(), input) {
            (
                Op::BranchTruthy { if_true, if_false, .. }
                | Op::Branch {
                    condition: BranchCondition::Bool,
                    if_true,
                    if_false,
                },
                Some(input),
            ) if graph.constant_value(input).is_some() => {
                let repr = graph.node(input).repr.unwrap_or(Repr::Tagged);
                constant_truthiness(graph.constant_value(input).expect("a constant"), repr).map(|truthy| {
                    (
                        Op::Jump {
                            target: if truthy { if_true } else { if_false },
                        },
                        Vec::new(),
                    )
                })
            }
            (
                Op::Branch {
                    condition,
                    if_true,
                    if_false,
                },
                Some(input),
            ) => constant_condition(
                graph,
                condition,
                input,
                graph
                    .node(control)
                    .inputs
                    .get(1)
                    .map(|input| replacements.resolve(*input)),
            )
            .map(|holds| {
                (
                    Op::Jump {
                        target: if holds { if_true } else { if_false },
                    },
                    Vec::new(),
                )
            }),
            (Op::BranchTruthy { if_true, if_false, .. }, Some(input)) if graph.node(input).op == Op::BoxBool => Some((
                Op::Branch {
                    condition: BranchCondition::Bool,
                    if_true,
                    if_false,
                },
                vec![graph.node(input).inputs[0]],
            )),
            (Op::BranchTruthy { if_true, if_false, .. }, Some(input)) if graph.node(input).op == Op::BoxInt32 => {
                Some((
                    Op::Branch {
                        condition: BranchCondition::Int32(Comparison::StrictlyInequals),
                        if_true,
                        if_false,
                    },
                    vec![graph.node(input).inputs[0], constants.get_typed(graph, 0, Repr::Int32)],
                ))
            }
            _ => None,
        };
        if let Some((op, inputs)) = new_op {
            let data = &mut graph.nodes[control.index()];
            data.op = op;
            data.inputs = inputs;
            changed_control_flow = true;
        }
    }
    replacements.apply(graph);
    edit::remove_nodes(graph, &removed);
    // NB: Folds can give a check users that need the int32, like the
    //     unboxing of a boxing of it, and the loop never visits the nodes
    //     that folds add before the node they replace. A check that any node
    //     still uses stays, and frame states take the doubles of the others.
    if dissolvable.is_empty() {
        if changed_control_flow {
            edit::clean_up_control_flow(graph);
        }
        return;
    }
    let mut dissolvable = dissolvable;
    dissolvable.subtract(&removed);
    for block in &graph.blocks {
        for node in block.nodes() {
            for input in &graph.node(node).inputs {
                if input.index() < dissolvable.capacity() {
                    dissolvable.remove(input.index());
                }
            }
        }
    }
    let mut replacements = Replacements::new(graph);
    let mut removed = BitSet::new(graph.nodes.len());
    for check in dissolvable.iter() {
        let check = NodeId::from_index(check);
        let double = checked_double(graph, check).expect("dissolvable checks check doubles");
        replacements.replace(check, double);
        removed.insert(check.index());
    }
    replacements.apply(graph);
    edit::remove_nodes(graph, &removed);
    if changed_control_flow {
        edit::clean_up_control_flow(graph);
    }
}
