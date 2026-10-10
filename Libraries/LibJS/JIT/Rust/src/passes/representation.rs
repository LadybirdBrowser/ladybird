/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Representation selection: unboxes phis.
//!
//! The builder makes every phi tagged, boxing unboxed values that flow into
//! one. A phi whose inputs are all boxed int32 values, int32 constants or
//! other such phis becomes an int32 phi of the unboxed values instead, like
//! loop counters; likewise for booleans, like the results of comparisons
//! merging before a branch, and for numbers, whose int32 inputs become
//! doubles, like the results of arithmetic that saw doubles. Unboxing only
//! pays off for phis connected (through other phis) to unboxed inputs,
//! unboxing checks or branches.
//!
//! An unboxed phi's int32 checks become the phi itself, and its other users
//! get one boxing of it at the start of its block. Exits take it unboxed.
//! The boxings feeding it are left for dead code elimination.

use super::dominators::Dominators;
use super::edit;
use super::edit::Constants;
use super::edit::Replacements;
use crate::bitset::BitSet;
use crate::code::Repr;
use crate::fast_hash::HashMap;
use crate::ir::BlockId;
use crate::ir::FrameState;
use crate::ir::Graph;
use crate::ir::Node;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::value;

/// An unboxed representation phis can take.
struct Unboxed {
    repr: Repr,
    /// Boxes a value of the representation.
    box_op: Op,
    /// The unboxed bits of a tagged constant of the representation.
    constant: fn(u64) -> Option<u64>,
    /// Unboxes a tagged value, if it is of the representation.
    check_op: Option<Op>,
    /// Whether a user of a phi benefits from it being unboxed.
    wants_unboxed: fn(&Op) -> bool,
    /// Whether int32 values become values of the representation through
    /// `Op::Int32ToFloat64`.
    converts_int32: bool,
}

const UNBOXED: [Unboxed; 3] = [
    Unboxed {
        repr: Repr::Int32,
        box_op: Op::BoxInt32,
        constant: |bits| value::as_int32(bits).map(|integer| u64::from(integer.cast_unsigned())),
        check_op: Some(Op::CheckInt32),
        wants_unboxed: |op| *op == Op::CheckInt32,
        converts_int32: false,
    },
    Unboxed {
        repr: Repr::Bool,
        box_op: Op::BoxBool,
        constant: |bits| match bits {
            value::TRUE => Some(1),
            value::FALSE => Some(0),
            _ => None,
        },
        check_op: None,
        wants_unboxed: |op| matches!(op, Op::BranchTruthy { .. }),
        converts_int32: false,
    },
    Unboxed {
        repr: Repr::Float64,
        box_op: Op::BoxFloat64,
        constant: |bits| {
            if let Some(integer) = value::as_int32(bits) {
                return Some(f64::from(integer).to_bits());
            }
            value::is_double(bits).then_some(bits)
        },
        check_op: Some(Op::CheckNumber),
        wants_unboxed: |op| matches!(op, Op::UnboxDouble | Op::CheckNumber),
        converts_int32: true,
    },
];

pub fn run(graph: &mut Graph) {
    for unboxed in &UNBOXED {
        unbox_phis(graph, unboxed);
    }
}

/// Whether `node` is among the phis being unboxed. Nodes added by the pass
/// are not.
fn is_candidate(candidates: &BitSet, node: NodeId) -> bool {
    node.index() < candidates.capacity() && candidates.contains(node.index())
}

/// What a phi input is unboxed, if it can be one without a check.
enum UnboxedInput {
    Value(NodeId),
    Constant(u64),
    /// The `Repr::Int32` value, which a conversion makes one of the
    /// representation.
    Converted(NodeId),
}

fn unboxed_input(graph: &Graph, unboxed: &Unboxed, input: NodeId, candidates: &BitSet) -> Option<UnboxedInput> {
    let node = graph.node(input);
    match node.op {
        ref op if *op == unboxed.box_op => Some(UnboxedInput::Value(node.inputs[0])),
        Op::Constant(bits) if node.repr == Some(Repr::Tagged) => (unboxed.constant)(bits).map(UnboxedInput::Constant),
        Op::Phi if is_candidate(candidates, input) => Some(UnboxedInput::Value(input)),
        Op::BoxInt32 if unboxed.converts_int32 => Some(UnboxedInput::Converted(node.inputs[0])),
        _ if unboxed.converts_int32 && node.repr == Some(Repr::Int32) => Some(UnboxedInput::Converted(input)),
        _ => None,
    }
}

/// Marks the phis of `phis` connected to marked ones through their inputs.
fn spread_over_phis(graph: &Graph, phis: &BitSet, marked: &mut BitSet) {
    let mut changed = true;
    while changed {
        changed = false;
        for phi in phis.iter() {
            for input in &graph.nodes[phi].inputs {
                if is_candidate(phis, *input) && marked.contains(phi) != marked.contains(input.index()) {
                    marked.insert(phi);
                    marked.insert(input.index());
                    changed = true;
                }
            }
        }
    }
}

/// The phis of `phis` whose values the code checks with `Unboxed::check_op`
/// whenever they are made: in their block or a block it always jumps on to,
/// or, for loop headers, in a block every iteration of the loop runs.
fn checked_phis(graph: &Graph, unboxed: &Unboxed, phis: &BitSet) -> BitSet {
    let mut checked = BitSet::new(graph.nodes.len());
    let Some(check) = &unboxed.check_op else {
        return checked;
    };
    let mut block_of = HashMap::default();
    let mut checks = Vec::new();
    for (index, block) in graph.blocks.iter().enumerate() {
        let block_id = BlockId::from_index(index);
        for phi in &block.phis {
            block_of.insert(*phi, block_id);
        }
        for node in &block.body {
            let data = graph.node(*node);
            if data.op == *check && is_candidate(phis, data.inputs[0]) {
                checks.push((data.inputs[0], block_id));
            }
        }
    }
    let jumps_on_to = |mut block: BlockId, to: BlockId| {
        for _ in 0..graph.blocks.len() {
            if block == to {
                return true;
            }
            let Op::Jump { target } = graph.control(block).op else {
                return false;
            };
            block = target;
        }
        false
    };
    let mut dominators = None;
    for (phi, check_block) in checks {
        let phi_block = block_of[&phi];
        if jumps_on_to(phi_block, check_block) {
            checked.insert(phi.index());
            continue;
        }
        let dominators = dominators.get_or_insert_with(|| Dominators::compute(graph));
        let back_edge_sources = dominators.back_edge_sources(graph, phi_block);
        if !back_edge_sources.is_empty()
            && dominators.dominates(phi_block, check_block)
            && dominators.runs_every_iteration(check_block, &back_edge_sources)
        {
            checked.insert(phi.index());
        }
    }
    checked
}

/// For each phi of `graph`, its block, and the frame states that checks at
/// the end of the predecessors its inputs come from can exit with, where
/// they can unbox the inputs (see `Unboxed::check_op`): tagged values from
/// predecessors that only jump to the phi's block, which starts a bytecode
/// block that the check never exited at before. Exits start that block over.
/// Only the values of `checked` phis, which the code checks elsewhere, are
/// checked where they enter.
fn checkable_inputs(
    graph: &Graph,
    unboxed: &Unboxed,
    checked: &BitSet,
) -> HashMap<NodeId, (BlockId, Vec<Option<FrameState>>)> {
    let mut checkable = HashMap::default();
    for (index, block) in graph.blocks.iter().enumerate() {
        let block_id = BlockId::from_index(index);
        let gated = match (unboxed.check_op.as_ref(), block.bytecode_start) {
            (Some(check), Some(pc)) => check
                .exit_kind()
                .is_none_or(|kind| graph.exit_sites.contains(&(pc, kind))),
            _ => true,
        };
        for phi in &block.phis {
            let inputs = block
                .predecessors
                .iter()
                .zip(&graph.node(*phi).inputs)
                .map(|(predecessor, input)| {
                    let checkable = !gated
                        && checked.contains(phi.index())
                        && graph.node(*input).repr == Some(Repr::Tagged)
                        && graph.control(*predecessor).op == (Op::Jump { target: block_id });
                    checkable
                        .then(|| edit::frame_state_entering(graph, block_id, *predecessor))
                        .flatten()
                })
                .collect();
            checkable.insert(*phi, (block_id, inputs));
        }
    }
    checkable
}

fn unbox_phis(graph: &mut Graph, unboxed: &Unboxed) {
    let count = graph.nodes.len();
    let mut candidates = BitSet::new(count);
    let phis = graph
        .blocks
        .iter()
        .flat_map(|block| block.phis.iter().copied())
        .filter(|phi| graph.node(*phi).repr == Some(Repr::Tagged))
        .collect::<Vec<_>>();
    for phi in &phis {
        candidates.insert(phi.index());
    }
    // NB: Phis that benefit from being unboxed: those of boxed values, and
    //     those that users unbox.
    let mut benefiting = BitSet::new(count);
    for phi in candidates.iter() {
        if graph.nodes[phi]
            .inputs
            .iter()
            .any(|input| graph.node(*input).op == unboxed.box_op)
        {
            benefiting.insert(phi);
        }
    }
    for block in &graph.blocks {
        for node in block.body.iter().chain(&block.control) {
            let data = graph.node(*node);
            if (unboxed.wants_unboxed)(&data.op)
                && let Some(input) = data.inputs.first()
                && is_candidate(&candidates, *input)
            {
                benefiting.insert(input.index());
            }
        }
    }
    if benefiting.is_empty() {
        return;
    }
    let checked = checked_phis(graph, unboxed, &candidates);
    let checkable = checkable_inputs(graph, unboxed, &checked);
    // Drop phis with an input that cannot be unboxed until none is left.
    let mut changed = true;
    while changed {
        changed = false;
        for phi in &phis {
            if candidates.contains(phi.index())
                && graph.node(*phi).inputs.iter().enumerate().any(|(index, input)| {
                    unboxed_input(graph, unboxed, *input, &candidates).is_none() && checkable[phi].1[index].is_none()
                })
            {
                candidates.remove(phi.index());
                changed = true;
            }
        }
    }

    // Keep the phis connected to something that benefits.
    let mut useful = BitSet::new(count);
    for phi in benefiting.iter().filter(|phi| candidates.contains(*phi)) {
        useful.insert(phi);
    }
    spread_over_phis(graph, &candidates, &mut useful);
    let candidates = useful;
    if candidates.is_empty() {
        return;
    }

    // Unbox the phis in place.
    let mut constants = Constants::new(graph);
    let mut conversions = HashMap::default();
    let mut checks = HashMap::default();
    for phi in candidates.iter() {
        let phi_id = NodeId::from_index(phi);
        let (block, states) = &checkable[&phi_id];
        let inputs = graph.nodes[phi]
            .inputs
            .clone()
            .into_iter()
            .enumerate()
            .map(
                |(index, input)| match unboxed_input(graph, unboxed, input, &candidates) {
                    Some(UnboxedInput::Value(value)) => value,
                    Some(UnboxedInput::Constant(bits)) => constants.get_typed(graph, bits, unboxed.repr),
                    Some(UnboxedInput::Converted(integer)) => match graph.constant_value(integer) {
                        Some(bits) => {
                            let number = f64::from((bits as u32).cast_signed());
                            constants.get_typed(graph, number.to_bits(), unboxed.repr)
                        }
                        None => *conversions
                            .entry(integer)
                            .or_insert_with(|| convert_int32(graph, integer)),
                    },
                    None => {
                        let predecessor = graph.block(*block).predecessors[index];
                        let state = states[index]
                            .clone()
                            .expect("candidate phis only have unboxable inputs");
                        *checks
                            .entry((predecessor, input))
                            .or_insert_with(|| check_at_end(graph, unboxed, predecessor, input, state))
                    }
                },
            )
            .collect();
        let node = &mut graph.nodes[phi];
        node.inputs = inputs;
        node.repr = Some(unboxed.repr);
    }

    // Unboxing checks of the phis are the phis themselves; other users that
    // need a tagged value get one boxing per phi.
    let mut replacements = Replacements::new(graph);
    let mut removed = BitSet::new(graph.nodes.len());
    let mut boxes = HashMap::default();
    for block in 0..graph.blocks.len() {
        let block_data = &graph.blocks[block];
        let nodes = block_data
            .phis
            .iter()
            .chain(&block_data.body)
            .chain(&block_data.control)
            .copied()
            .collect::<Vec<_>>();
        for user in nodes {
            let user_node = graph.node(user);
            if unboxed.check_op.as_ref() == Some(&user_node.op) && is_candidate(&candidates, user_node.inputs[0]) {
                replacements.replace(user, user_node.inputs[0]);
                removed.insert(user.index());
                continue;
            }
            if (user_node.op == Op::Phi && is_candidate(&candidates, user))
                || user_node.op == unboxed.box_op
                || matches!(user_node.op, Op::StoreSlot { .. })
            {
                continue;
            }
            for index in 0..graph.node(user).inputs.len() {
                let input = graph.node(user).inputs[index];
                if !is_candidate(&candidates, input) {
                    continue;
                }
                let boxed = *boxes.entry(input).or_insert_with(|| box_phi(graph, unboxed, input));
                graph.nodes[user.index()].inputs[index] = boxed;
            }
        }
    }
    replacements.apply(graph);
    edit::remove_nodes(graph, &removed);
}

/// Adds a check that unboxes `value` at the end of `predecessor`, exiting
/// with `state` where it fails.
fn check_at_end(
    graph: &mut Graph,
    unboxed: &Unboxed,
    predecessor: BlockId,
    value: NodeId,
    state: FrameState,
) -> NodeId {
    let pc = state.pc;
    let frame_state = Some(graph.add_frame_state(state));
    let check = graph.add_node(Node {
        op: unboxed
            .check_op
            .clone()
            .expect("only checked representations check inputs"),
        inputs: vec![value],
        repr: Some(unboxed.repr),
        frame_state,
        pc,
    });
    graph.blocks[predecessor.index()].body.push(check);
    check
}

/// Adds the `Repr::Float64` conversion of the `Repr::Int32` `integer` right
/// after it.
fn convert_int32(graph: &mut Graph, integer: NodeId) -> NodeId {
    let (block, position) = graph
        .blocks
        .iter()
        .enumerate()
        .find_map(|(index, block)| {
            if block.phis.contains(&integer) {
                return Some((index, 0));
            }
            block
                .body
                .iter()
                .position(|node| *node == integer)
                .map(|position| (index, position + 1))
        })
        .expect("values are in blocks");
    let converted = graph.add_node(Node::new(
        Op::Int32ToFloat64,
        vec![integer],
        Some(Repr::Float64),
        graph.node(integer).pc,
    ));
    graph.blocks[block].body.insert(position, converted);
    converted
}

/// Adds a boxing of `phi` at the start of its block.
fn box_phi(graph: &mut Graph, unboxed: &Unboxed, phi: NodeId) -> NodeId {
    let block = graph
        .blocks
        .iter()
        .position(|block| block.phis.contains(&phi))
        .expect("phis are in blocks");
    let boxed = graph.add_node(Node::new(
        unboxed.box_op.clone(),
        vec![phi],
        Some(Repr::Tagged),
        graph.node(phi).pc,
    ));
    graph.blocks[block].body.insert(0, boxed);
    boxed
}
