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
//! below 2^53, so chains like `(a + b + c) | 0` wrap as a whole.
//!
//! Frame states need the exact value, so values they hold do not wrap.
//! The pass runs before simplification removes the `x | 0` that ToInt32 of
//! an int32 builds.

use crate::bitset::BitSet;
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
        Op::Int32WrappingBinary {
            op: BinaryOp::Add | BinaryOp::Sub,
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

pub fn run(graph: &mut Graph) {
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
        let data = &mut graph.nodes[node];
        let Op::Int32Binary { op } = data.op else {
            unreachable!("only int32 operations wrap");
        };
        data.op = Op::Int32WrappingBinary { op };
        data.frame_state = None;
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
