/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Branch fusion: a branch on a comparison that nothing else uses compares
//! the comparison's inputs itself, so code generation emits a compare and a
//! conditional jump instead of materializing a boolean and testing it.
//! A constant operand goes on the right, where it is an immediate.
//!
//! A block that only branches on a phi of booleans (where an instruction's
//! paths join, like those of a comparison) hands the branch to its
//! predecessors first: each branches on its own boolean, or jumps where it
//! is a constant, so that its comparison fuses with the branch there.

use super::edit;
use crate::bitset::BitSet;
use crate::code::Repr;
use crate::ir::Block;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::Comparison;
use crate::ir::Graph;
use crate::ir::Node;
use crate::ir::NodeId;
use crate::ir::Op;

/// The comparison of swapped operands that has the same result.
fn mirrored(comparison: Comparison) -> Comparison {
    match comparison {
        Comparison::LessThan => Comparison::GreaterThan,
        Comparison::LessThanEquals => Comparison::GreaterThanEquals,
        Comparison::GreaterThan => Comparison::LessThan,
        Comparison::GreaterThanEquals => Comparison::LessThanEquals,
        Comparison::StrictlyEquals
        | Comparison::StrictlyInequals
        | Comparison::LooselyEquals
        | Comparison::LooselyInequals => comparison,
    }
}

/// How many nodes and frame states use each node.
fn count_uses(graph: &Graph) -> Vec<u32> {
    let mut uses = vec![0u32; graph.nodes.len()];
    for block in &graph.blocks {
        for node in block.nodes() {
            for value in graph.used_values(node) {
                uses[value.index()] += 1;
            }
        }
    }
    uses
}

/// Makes the predecessors of each block that only branches on a phi of
/// booleans, which nothing else uses, branch on their input of the phi
/// themselves (through a new block on each edge, which jumps on), or jump
/// to the target of a constant one.
fn thread_boolean_phis(graph: &mut Graph, uses: &[u32]) {
    let block_count = graph.blocks.len();
    let mut new_blocks: Vec<Vec<BlockId>> = vec![Vec::new(); block_count];
    let mut threaded = BitSet::new(block_count);
    for block in 0..block_count {
        let data = &graph.blocks[block];
        let (Some(control), [phi]) = (data.control, data.phis.as_slice()) else {
            continue;
        };
        let phi = *phi;
        let Op::Branch {
            condition: BranchCondition::Bool,
            if_true,
            if_false,
        } = graph.node(control).op
        else {
            continue;
        };
        let block_id = BlockId::from_index(block);
        let predecessors = data.predecessors.clone();
        let jumps_here = |predecessor: &BlockId| {
            graph.control(*predecessor).op == Op::Jump { target: block_id } && predecessor.index() < block
        };
        if !data.body.is_empty()
            || data.is_loop_header
            || graph.node(control).inputs[0] != phi
            || uses[phi.index()] != 1
            || if_true.index() <= block
            || if_false.index() <= block
            || !predecessors.iter().all(jumps_here)
        {
            continue;
        }
        let pc = graph.node(control).pc;
        let inputs = graph.node(phi).inputs.clone();
        for (predecessor, input) in predecessors.into_iter().zip(inputs) {
            let truth = (graph.node(input).repr == Some(Repr::Bool))
                .then(|| graph.constant_value(input))
                .flatten();
            let predecessor_control = graph.control_id(predecessor);
            if let Some(bits) = truth {
                let target = if bits != 0 { if_true } else { if_false };
                edit::make_jump(graph, predecessor_control, target);
                add_predecessor(graph, target, predecessor, block_id);
                continue;
            }
            let is_cold = graph.block(predecessor).is_cold;
            let edges = [if_true, if_false].map(|target| {
                let edge = graph.add_block(Block {
                    predecessors: vec![predecessor],
                    is_cold,
                    ..Block::default()
                });
                let jump = graph.add_node(Node::new(Op::Jump { target }, Vec::new(), None, pc));
                graph.blocks[edge.index()].control = Some(jump);
                add_predecessor(graph, target, edge, block_id);
                edge
            });
            let data = &mut graph.nodes[predecessor_control.index()];
            data.op = Op::Branch {
                condition: BranchCondition::Bool,
                if_true: edges[0],
                if_false: edges[1],
            };
            data.inputs = vec![input];
            new_blocks[predecessor.index()].extend(edges);
        }
        // NB: Nothing reaches the block anymore.
        for target in [if_true, if_false] {
            remove_predecessor(graph, target, block_id);
        }
        threaded.insert(block);
    }
    if threaded.is_empty() {
        return;
    }
    // NB: The new blocks follow their predecessor, before their targets.
    let order = (0..block_count)
        .filter(|block| !threaded.contains(*block))
        .flat_map(|block| std::iter::once(BlockId::from_index(block)).chain(new_blocks[block].iter().copied()))
        .collect::<Vec<_>>();
    graph.reorder_blocks(&order);
}

/// Removes `predecessor` from the predecessors of `target`, and its inputs
/// from the phis of `target`.
fn remove_predecessor(graph: &mut Graph, target: BlockId, predecessor: BlockId) {
    let index = graph
        .block(target)
        .predecessors
        .iter()
        .position(|block| *block == predecessor)
        .expect("the threaded block is a predecessor of its targets");
    graph.blocks[target.index()].predecessors.remove(index);
    for phi in graph.blocks[target.index()].phis.clone() {
        graph.nodes[phi.index()].inputs.remove(index);
    }
}

/// Makes `predecessor` a predecessor of `target`, with the inputs to its
/// phis that `like` (a predecessor of it already) gives them.
fn add_predecessor(graph: &mut Graph, target: BlockId, predecessor: BlockId, like: BlockId) {
    let index = graph
        .block(target)
        .predecessors
        .iter()
        .position(|block| *block == like)
        .expect("the target is a successor of the threaded block");
    graph.blocks[target.index()].predecessors.push(predecessor);
    for phi in graph.blocks[target.index()].phis.clone() {
        let input: NodeId = graph.nodes[phi.index()].inputs[index];
        graph.nodes[phi.index()].inputs.push(input);
    }
}

pub fn run(graph: &mut Graph) {
    // NB: Threading moves the uses of each phi's inputs to the branches.
    let uses = count_uses(graph);
    thread_boolean_phis(graph, &uses);
    let count = graph.nodes.len();
    let mut removed = BitSet::new(count);
    for block in 0..graph.blocks.len() {
        let control = graph.control_id(BlockId::from_index(block));
        let Op::Branch {
            condition: BranchCondition::Bool,
            if_true,
            if_false,
        } = graph.node(control).op
        else {
            continue;
        };
        let comparison = graph.node(control).inputs[0];
        // NB: The comparison must be in the branch's block, so that fusing
        //     it does not keep its inputs alive any longer than to its end.
        if uses[comparison.index()] != 1 || !graph.blocks[block].body.contains(&comparison) {
            continue;
        }
        let condition = match graph.node(comparison).op {
            Op::Int32Compare { comparison } => BranchCondition::Int32(comparison),
            Op::Float64Compare { comparison } => BranchCondition::Float64(comparison),
            Op::TaggedEquals { equal } => BranchCondition::TaggedEquals { equal },
            _ => continue,
        };
        let mut inputs = graph.node(comparison).inputs.clone();
        // NB: Code generation takes a constant on the right as an immediate.
        let constant = |input| graph.constant_value(input).is_some();
        let condition = if constant(inputs[0]) && !constant(inputs[1]) {
            inputs.swap(0, 1);
            match condition {
                BranchCondition::Int32(comparison) => BranchCondition::Int32(mirrored(comparison)),
                BranchCondition::Float64(comparison) => BranchCondition::Float64(mirrored(comparison)),
                condition => condition,
            }
        } else {
            condition
        };
        let data = &mut graph.nodes[control.index()];
        data.op = Op::Branch {
            condition,
            if_true,
            if_false,
        };
        data.inputs = inputs;
        removed.insert(comparison.index());
    }
    edit::remove_nodes(graph, &removed);
}
