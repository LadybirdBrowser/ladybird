/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Constant folding and strength reduction.
//!
//! - Conversions and int32 operations of constants become constants.
//! - Branches on constants become jumps; truthiness branches on boxed
//!   booleans test the unboxed boolean.

use super::edit;
use super::edit::Constants;
use super::edit::Replacements;
use crate::bitset::BitSet;
use crate::code::Repr;
use crate::ir::BinaryOp;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::Comparison;
use crate::ir::Graph;
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
    let data = graph.node(node);
    match (data.op.clone(), data.repr) {
        (Op::Constant(bits), Some(Repr::Int32)) => Some((bits as u32).cast_signed()),
        _ => None,
    }
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

fn fold_int32_compare(comparison: Comparison, lhs: i32, rhs: i32) -> bool {
    match comparison {
        Comparison::LessThan => lhs < rhs,
        Comparison::LessThanEquals => lhs <= rhs,
        Comparison::GreaterThan => lhs > rhs,
        Comparison::GreaterThanEquals => lhs >= rhs,
        Comparison::StrictlyEquals | Comparison::LooselyEquals => lhs == rhs,
        Comparison::StrictlyInequals | Comparison::LooselyInequals => lhs != rhs,
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
        | BranchCondition::Shape(_) => None,
    }
}

pub fn run(graph: &mut Graph) {
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
            let inputs = graph
                .node(node)
                .inputs
                .iter()
                .map(|input| replacements.resolve(*input))
                .collect::<Vec<_>>();
            let constant_bits = |graph: &Graph, input: usize| graph.constant_value(inputs[input]);
            let boxed = |op: Op, input: NodeId| {
                let unrefined = graph.node(graph.unrefined(input));
                (unrefined.op == op).then(|| unrefined.inputs[0])
            };
            let folded = match graph.node(node).op.clone() {
                // NB: Unboxing a boxed value is the value (where a branch
                //     established the box holds the kind it unboxes).
                Op::UnboxDouble | Op::CheckNumber => boxed(Op::BoxFloat64, inputs[0]),
                Op::UnboxInt32 => boxed(Op::BoxInt32, inputs[0]),
                // NB: Inlined callees return values that representation
                //     selection may have found to be boxes, never empty.
                Op::EmptyToUndefined => graph.is_known_non_empty(inputs[0], 0..0).then_some(inputs[0]),
                Op::Int32ToFloat64 => match graph.node(inputs[0]).op {
                    Op::UnboxInt32 => boxed(Op::BoxFloat64, graph.node(inputs[0]).inputs[0]),
                    _ => None,
                },
                Op::BoxInt32 => int32_of(graph, inputs[0]).map(|integer| constants.get(graph, value::int32(integer))),
                Op::BoxBool => constant_bits(graph, 0).map(|bits| {
                    let boolean = if bits != 0 { value::TRUE } else { value::FALSE };
                    constants.get(graph, boolean)
                }),
                Op::CheckInt32 => constant_bits(graph, 0)
                    .filter(|bits| graph.node(inputs[0]).repr == Some(Repr::Tagged) && value::as_int32(*bits).is_some())
                    .map(|bits| constants.get_typed(graph, bits & u64::from(u32::MAX), Repr::Int32)),
                Op::Int32Binary { op } => match (int32_of(graph, inputs[0]), int32_of(graph, inputs[1])) {
                    (Some(lhs), Some(rhs)) => fold_int32_binary(op, lhs, rhs)
                        .map(|result| constants.get_typed(graph, u64::from(result.cast_unsigned()), Repr::Int32)),
                    _ => None,
                },
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
            if let Some(constant) = folded {
                replacements.replace(node, constant);
                removed.insert(node.index());
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
    if changed_control_flow {
        edit::clean_up_control_flow(graph);
    }
}
