/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Truncation: int32 operations whose results only reach users that read
//! their low 32 bits stop checking for overflow.
//!
//! Additions, subtractions and unsigned right shifts of int32 values exit
//! where the result is no int32, so that the interpreter can produce the
//! number. But where every user converts the result with ToInt32 or
//! ToUint32 (`(a + b) | 0`, shifts, other bitwise operations, stores to
//! integer typed arrays), the low 32 bits of the result are all that
//! matters, and the operation wraps instead (`Op::Int32WrappingBinary`).
//!
//! Wrapping additions and subtractions read only the low 32 bits of their
//! operands too: the sum of int32 values that wrapped is congruent modulo
//! 2^32 to the exact sum, which doubles hold exactly as long as it stays
//! below 2^53, so chains like `(a + b + c) | 0` wrap as a whole. So do the
//! wrapping multiplications of `Math.imul`.
//!
//! Frame states need the exact value, so values they hold do not wrap.
//! The pass runs before simplification removes the `x | 0` that ToInt32 of
//! an int32 builds.
//!
//! Comparisons of uint32 values (`(a >>> 0) < (b >>> 0)`, whose results
//! may also be doubles) only read the bits of the shifted values as well:
//! they first become unsigned comparisons of those bits, which leaves the
//! shifts unused.

use super::edit::Constants;
use crate::bitset::BitSet;
use crate::code::Repr;
use crate::ir::BinaryOp;
use crate::ir::ElementsKind;
use crate::ir::FrameStateId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::TypedArrayElement;

/// The operations that wrap where their users only read the low 32 bits.
fn can_wrap(op: &Op) -> bool {
    matches!(
        op,
        Op::Int32Binary {
            op: BinaryOp::Add | BinaryOp::Sub | BinaryOp::UnsignedRightShift
        }
    )
}

/// Whether input `index` of `user` only reads its low 32 bits, given the
/// nodes that are to wrap.
fn reads_low_bits(graph: &Graph, user: NodeId, index: usize, wrapping: &BitSet) -> bool {
    match graph.node(user).op {
        Op::Int32Binary { op } | Op::Int32WrappingBinary { op } if op.is_bitwise() => true,
        Op::Int32Binary {
            op: BinaryOp::Add | BinaryOp::Sub,
        } => wrapping.contains(user.index()),
        // NB: The low 32 bits of a product only depend on the low 32 bits of
        //     its factors.
        Op::Int32WrappingBinary {
            op: BinaryOp::Add | BinaryOp::Sub | BinaryOp::Mul,
        } => true,
        // NB: Clamped elements clamp the value instead.
        Op::StoreElementAt {
            kind: ElementsKind::TypedArray(element),
        } => {
            index == 2
                && !matches!(
                    element,
                    TypedArrayElement::Uint8Clamped | TypedArrayElement::Float32 | TypedArrayElement::Float64
                )
        }
        _ => false,
    }
}

/// An operand of a comparison of uint32 values.
enum Uint32Operand {
    /// The `Repr::Int32` value whose bits are the uint32 value.
    Bits(NodeId),
    Constant(u32),
}

/// What `value`, an operand of an int32 comparison (or a float64 one, if
/// `float64`), is as a uint32 value, if it is one.
fn uint32_operand(graph: &Graph, value: NodeId, float64: bool) -> Option<Uint32Operand> {
    let node = graph.node(graph.unrefined(value));
    let shifted_by_zero = |node: &crate::ir::Node| {
        graph
            .constant_value(node.inputs[1])
            .is_some_and(|amount| amount & 31 == 0)
            .then_some(Uint32Operand::Bits(node.inputs[0]))
    };
    match (&node.op, float64) {
        (Op::Constant(bits), false) => {
            let integer = (*bits as u32).cast_signed();
            (integer >= 0).then_some(Uint32Operand::Constant(integer.cast_unsigned()))
        }
        (Op::Constant(bits), true) => {
            let number = f64::from_bits(*bits);
            (number.fract() == 0.0 && (0.0..4294967296.0).contains(&number))
                .then_some(Uint32Operand::Constant(number as u32))
        }
        (
            Op::Int32Binary {
                op: BinaryOp::UnsignedRightShift,
            }
            | Op::Int32WrappingBinary {
                op: BinaryOp::UnsignedRightShift,
            },
            false,
        ) => shifted_by_zero(node),
        // NB: Simplification has not removed the boxing of operands yet.
        (Op::CheckNumber | Op::UnboxDouble, true) => {
            let boxed = graph.node(graph.unrefined(node.inputs[0]));
            (boxed.op == Op::BoxFloat64)
                .then(|| uint32_operand(graph, boxed.inputs[0], true))
                .flatten()
        }
        (Op::Int32ToFloat64, true) => uint32_operand(graph, node.inputs[0], false),
        (Op::Uint32ToFloat64, true) => {
            let bits = graph.node(node.inputs[0]);
            let shifted = (bits.op == Op::Uint32ShiftRight)
                .then(|| shifted_by_zero(bits))
                .flatten();
            shifted.or(Some(Uint32Operand::Bits(node.inputs[0])))
        }
        _ => None,
    }
}

/// Makes comparisons of uint32 values unsigned int32 comparisons of their
/// bits.
fn compare_uint32_bits(graph: &mut Graph) {
    let mut constants = Constants::new(graph);
    for block in 0..graph.blocks.len() {
        for index in 0..graph.blocks[block].body.len() {
            let node = graph.blocks[block].body[index];
            let (comparison, float64) = match graph.node(node).op {
                Op::Int32Compare { comparison } => (comparison, false),
                Op::Float64Compare { comparison } => (comparison, true),
                _ => continue,
            };
            let inputs = &graph.node(node).inputs;
            let (Some(lhs), Some(rhs)) = (
                uint32_operand(graph, inputs[0], float64),
                uint32_operand(graph, inputs[1], float64),
            ) else {
                continue;
            };
            if matches!((&lhs, &rhs), (Uint32Operand::Constant(_), Uint32Operand::Constant(_))) {
                continue;
            }
            let inputs = [lhs, rhs].map(|operand| match operand {
                Uint32Operand::Bits(value) => value,
                Uint32Operand::Constant(integer) => constants.get_typed(graph, u64::from(integer), Repr::Int32),
            });
            let data = &mut graph.nodes[node.index()];
            data.op = Op::Int32Compare {
                comparison: comparison.unsigned(),
            };
            data.inputs = inputs.to_vec();
        }
    }
}

pub fn run(graph: &mut Graph) {
    compare_uint32_bits(graph);
    let count = graph.nodes.len();
    let mut wrapping = BitSet::new(count);
    let mut users: Vec<Vec<(NodeId, usize)>> = vec![Vec::new(); count];
    // The node each frame state belongs to alone, if any: those of nodes
    // that wrap go away with their exits.
    let mut owners = vec![Owner::None; graph.frame_states.len()];
    for block in &graph.blocks {
        if let Some(frame_state) = block.start_frame_state {
            share(&mut owners, frame_state, Owner::Shared);
        }
        for node in block.nodes() {
            let data = graph.node(node);
            if can_wrap(&data.op) {
                wrapping.insert(node.index());
            }
            if let Some(frame_state) = data.frame_state {
                share(&mut owners, frame_state, Owner::Node(node));
            }
            for (index, input) in data.inputs.iter().enumerate() {
                users[input.index()].push((node, index));
            }
        }
    }
    if wrapping.is_empty() {
        return;
    }
    for frame_state in &graph.frame_states {
        if let Some(parent) = frame_state.parent {
            share(&mut owners, parent, Owner::Shared);
        }
    }

    // Start from every candidate wrapping, and drop those with users that
    // need more than the low 32 bits until none is left. Values of frame
    // states that stay need all their bits too.
    loop {
        let mut in_frame_state = BitSet::new(count);
        for (frame_state, owner) in graph.frame_states.iter().zip(&owners) {
            if matches!(owner, Owner::Node(node) if wrapping.contains(node.index())) {
                continue;
            }
            for (_, value) in &frame_state.values {
                in_frame_state.insert(value.index());
            }
        }
        // NB: Boxings nothing uses are left for dead code elimination; the
        //     builder boxes every result it writes to a slot.
        let is_unused_boxing = |node: NodeId| {
            graph.node(node).op == Op::BoxInt32
                && users[node.index()].is_empty()
                && !in_frame_state.contains(node.index())
        };
        let mut changed = false;
        for node in wrapping.iter().collect::<Vec<_>>() {
            let keeps_checking = in_frame_state.contains(node)
                || users[node]
                    .iter()
                    .any(|(user, index)| !is_unused_boxing(*user) && !reads_low_bits(graph, *user, *index, &wrapping));
            if keeps_checking {
                wrapping.remove(node);
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }

    for node in wrapping.iter() {
        super::edit::make_wrapping(graph, NodeId::from_index(node));
    }
}

#[derive(Clone, Copy)]
enum Owner {
    None,
    Node(NodeId),
    Shared,
}

fn share(owners: &mut [Owner], frame_state: FrameStateId, owner: Owner) {
    let current = &mut owners[frame_state.index()];
    *current = match *current {
        Owner::None => owner,
        _ => Owner::Shared,
    };
}
