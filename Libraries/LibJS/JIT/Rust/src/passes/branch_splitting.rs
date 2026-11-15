/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Branch splitting: a block that joins two paths only to branch on which of
//! them it came from goes away. Where an instruction's paths join with a
//! boolean phi of constants (like the "done" result of a for-in loop's next
//! key), and the next instruction branches on it, each path jumps straight
//! to its side of the branch instead, and the code there sees the values of
//! its own path rather than phis of both. A for-in loop's body then reads
//! the key the cache holds, not a phi that also holds `undefined`.

use super::dominators::Dominators;
use crate::bitset::BitSet;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::FrameStateId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;

/// A block to split: its phis, the block that branches (itself, or the one
/// it jumps to), the nodes before that branch (which each side gets a copy
/// of), and for each side of the branch, the predecessor that takes it and
/// its target.
struct Split {
    block: BlockId,
    branch_block: BlockId,
    moved: Vec<NodeId>,
    sides: [(BlockId, BlockId); 2],
}

/// Whether a copy of `node` can run at the start of each side instead: an
/// `AssumeValid`, or a node without effects that cannot exit.
fn is_movable(graph: &Graph, node: NodeId) -> bool {
    let data = graph.node(node);
    if data.op == Op::AssumeValid {
        return true;
    }
    let properties = data.op.properties();
    data.frame_state.is_none()
        && data.op.exit_kind().is_none()
        && !data.op.is_control()
        && !properties.can_lazy_exit
        && !properties.is_call
        && !properties.runs_code
        && !properties.allocates
        && properties.reads.is_empty()
        && properties.writes.is_empty()
}

/// Which side's target dominates `block`, if one does.
fn side_of(dominators: &Dominators, split: &Split, block: BlockId) -> Option<usize> {
    (0..2).find(|side| dominators.dominates(split.sides[*side].1, block))
}

/// Whether `block` has the shape of `Split`, and its split if so.
fn find_split(graph: &Graph, dominators: &Dominators, block: BlockId) -> Option<Split> {
    let data = graph.block(block);
    if data.phis.is_empty() || data.is_loop_header || data.predecessors.len() != 2 || !dominators.is_reachable(block) {
        return None;
    }
    let movable = |body: &[NodeId]| body.iter().all(|node| is_movable(graph, *node));
    let branch_block = match graph.control(block).op {
        Op::Jump { target } if target.index() > block.index() => {
            let target_data = graph.block(target);
            if target_data.predecessors != [block] || !target_data.phis.is_empty() || target_data.is_loop_header {
                return None;
            }
            target
        }
        Op::Branch { .. } => block,
        _ => return None,
    };
    let branch_data = graph.block(branch_block);
    if !movable(&data.body) || (branch_block != block && !movable(&branch_data.body)) {
        return None;
    }
    let control = graph.control(branch_block);
    let Op::Branch {
        condition: BranchCondition::Bool,
        if_true,
        if_false,
    } = control.op
    else {
        return None;
    };
    let condition = control.inputs[0];
    if !data.phis.contains(&condition) {
        return None;
    }
    let mut sides = [None, None];
    for (predecessor, input) in data.predecessors.iter().zip(&graph.node(condition).inputs) {
        let side = match graph.constant_value(*input)? {
            0 => 1,
            _ => 0,
        };
        if sides[side].is_some() {
            return None;
        }
        // NB: An edge is redirected by replacing its target, which must not also be the other edge of a branch.
        let control = graph.control(*predecessor);
        if !matches!(control.op, Op::Jump { .. } | Op::Branch { .. })
            || control
                .op
                .successors()
                .iter()
                .filter(|successor| **successor == block)
                .count()
                != 1
        {
            return None;
        }
        sides[side] = Some(*predecessor);
    }
    let targets = [if_true, if_false];
    if if_true == if_false {
        return None;
    }
    for target in targets {
        let target_data = graph.block(target);
        if target_data.predecessors != [branch_block]
            || !target_data.phis.is_empty()
            || target_data.is_loop_header
            || target.index() <= branch_block.index()
        {
            return None;
        }
    }
    let moved: Vec<NodeId> = if branch_block == block {
        data.body.clone()
    } else {
        data.body.iter().chain(&branch_data.body).copied().collect()
    };
    // NB: Refines must stay at the head of their block, so the copies of the moved nodes cannot go before one.
    let starts_with_refine = |target: BlockId| {
        graph
            .block(target)
            .body
            .first()
            .is_some_and(|node| matches!(graph.node(*node).op, Op::Refine { .. }))
    };
    if !moved.is_empty() && targets.into_iter().any(starts_with_refine) {
        return None;
    }
    Some(Split {
        block,
        branch_block,
        moved,
        sides: [(sides[0]?, targets[0]), (sides[1]?, targets[1])],
    })
}

/// The side of each frame state that a node or block of one side uses and
/// that holds one of the phis, or `None` if one of them cannot be renamed: a
/// node or block of another place uses it, or the phi is in a frame of its
/// chain other than its own, or in a virtual object.
fn frame_state_sides(
    graph: &Graph,
    dominators: &Dominators,
    split: &Split,
    phis: &BitSet,
) -> Option<Vec<(FrameStateId, usize)>> {
    let mut sides: Vec<(FrameStateId, usize)> = Vec::new();
    for (index, block) in graph.blocks.iter().enumerate() {
        let block_id = BlockId::from_index(index);
        if !dominators.is_reachable(block_id) || block_id == split.block || block_id == split.branch_block {
            continue;
        }
        // NB: Start frame states are renamed too: passes after this one still exit with them (see
        //     `edit::frame_state_entering()`).
        let frame_states = block
            .nodes()
            .filter_map(|node| graph.node(node).frame_state)
            .chain(block.start_frame_state);
        for frame_state in frame_states {
            let holds_phi = |id| {
                graph
                    .frame_state_values(id)
                    .any(|(_, value)| phis.contains(value.index()))
            };
            if !holds_phi(frame_state) {
                continue;
            }
            let own_holds = graph
                .frame_state(frame_state)
                .values
                .iter()
                .any(|(_, value)| phis.contains(value.index()));
            let chain_holds = graph.frame_state_chain(frame_state)[1..].iter().any(|id| {
                graph
                    .frame_state(*id)
                    .values
                    .iter()
                    .any(|(_, value)| phis.contains(value.index()))
            });
            let all_count = graph.frame_state_values(frame_state).count();
            let chain_count: usize = graph
                .frame_state_chain(frame_state)
                .iter()
                .map(|id| graph.frame_state(*id).values.len())
                .sum();
            // NB: Virtual objects add values past those of the chain's frames.
            let virtual_holds = all_count != chain_count
                && graph
                    .frame_state_values(frame_state)
                    .skip(chain_count)
                    .any(|(_, value)| phis.contains(value.index()));
            if !own_holds || chain_holds || virtual_holds {
                return None;
            }
            let side = side_of(dominators, split, block_id)?;
            match sides.iter().find(|(id, _)| *id == frame_state) {
                Some((_, existing)) if *existing != side => return None,
                Some(_) => {}
                None => sides.push((frame_state, side)),
            }
        }
    }
    Some(sides)
}

/// `value`, or what `renames` renames it to.
fn renamed(renames: &[(NodeId, NodeId)], value: NodeId) -> NodeId {
    renames
        .iter()
        .find(|(from, _)| *from == value)
        .map_or(value, |(_, to)| *to)
}

/// Splits one block, if any has the shape of `Split` and all uses of its
/// phis are on one side of its branch. Returns whether it split one.
fn split_one(graph: &mut Graph) -> bool {
    let dominators = Dominators::compute(graph);
    for index in 0..graph.blocks.len() {
        let Some(split) = find_split(graph, &dominators, BlockId::from_index(index)) else {
            continue;
        };
        let mut phis = BitSet::new(graph.nodes.len());
        for phi in &graph.block(split.block).phis {
            phis.insert(phi.index());
        }
        for node in &split.moved {
            phis.insert(node.index());
        }
        // Every use of a phi must be on one side. A phi's input is used at the end of its predecessor.
        let mut uses_ok = true;
        'blocks: for (block_index, block) in graph.blocks.iter().enumerate() {
            let block_id = BlockId::from_index(block_index);
            if !dominators.is_reachable(block_id) || block_id == split.block || block_id == split.branch_block {
                continue;
            }
            for phi in &block.phis {
                for (predecessor, input) in block.predecessors.iter().zip(&graph.node(*phi).inputs) {
                    if phis.contains(input.index()) && side_of(&dominators, &split, *predecessor).is_none() {
                        uses_ok = false;
                        break 'blocks;
                    }
                }
            }
            let side = side_of(&dominators, &split, block_id);
            for node in block.body.iter().chain(block.control.iter()) {
                if graph
                    .node(*node)
                    .inputs
                    .iter()
                    .any(|input| phis.contains(input.index()))
                    && side.is_none()
                {
                    uses_ok = false;
                    break 'blocks;
                }
            }
        }
        if !uses_ok {
            continue;
        }
        let Some(frame_state_sides) = frame_state_sides(graph, &dominators, &split, &phis) else {
            continue;
        };

        let block_phis = graph.block(split.block).phis.clone();
        let predecessors = graph.block(split.block).predecessors.clone();
        let mut renames: [Vec<(NodeId, NodeId)>; 2] = std::array::from_fn(|side| {
            let predecessor = split.sides[side].0;
            let index = predecessors
                .iter()
                .position(|block| *block == predecessor)
                .expect("the side's predecessor enters the block");
            block_phis
                .iter()
                .map(|phi| (*phi, graph.node(*phi).inputs[index]))
                .collect()
        });
        // Each side starts with copies of the moved nodes, with its values.
        for (side, side_renames) in renames.iter_mut().enumerate() {
            let target = split.sides[side].1;
            let mut copies = Vec::new();
            for moved in &split.moved {
                let mut node = graph.node(*moved).clone();
                for input in &mut node.inputs {
                    *input = renamed(side_renames, *input);
                }
                node.frame_state = node.frame_state.map(|id| {
                    let mut frame_state = graph.frame_state(id).clone();
                    for (_, value) in &mut frame_state.values {
                        *value = renamed(side_renames, *value);
                    }
                    graph.add_frame_state(frame_state)
                });
                let copy = graph.add_node(node);
                side_renames.push((*moved, copy));
                copies.push(copy);
            }
            let body = &mut graph.blocks[target.index()].body;
            copies.append(body);
            *body = copies;
        }
        let rename = |side: usize, value: NodeId| renamed(&renames[side], value);

        // Rename the uses of each side.
        for block_index in 0..graph.blocks.len() {
            let block_id = BlockId::from_index(block_index);
            if !dominators.is_reachable(block_id) || block_id == split.block || block_id == split.branch_block {
                continue;
            }
            let block = graph.block(block_id).clone();
            for phi in &block.phis {
                for (input_index, predecessor) in block.predecessors.iter().enumerate() {
                    if let Some(side) = side_of(&dominators, &split, *predecessor) {
                        let input = graph.nodes[phi.index()].inputs[input_index];
                        graph.nodes[phi.index()].inputs[input_index] = rename(side, input);
                    }
                }
            }
            if let Some(side) = side_of(&dominators, &split, block_id) {
                for node in block.body.iter().chain(block.control.iter()) {
                    for input in &mut graph.nodes[node.index()].inputs {
                        *input = rename(side, *input);
                    }
                }
            }
        }
        for (frame_state, side) in frame_state_sides {
            for (_, value) in &mut graph.frame_states[frame_state.0 as usize].values {
                *value = rename(side, *value);
            }
        }

        // Each side's predecessor jumps straight to its target.
        for side in 0..2 {
            let (predecessor, target) = split.sides[side];
            graph.replace_successor(predecessor, split.block, target);
            graph.blocks[target.index()].predecessors = vec![predecessor];
        }
        graph.blocks[split.block.index()].predecessors.clear();
        for phi in block_phis {
            graph.nodes[phi.index()].inputs.clear();
        }
        super::edit::clean_up_control_flow(graph);
        return true;
    }
    false
}

pub fn run(graph: &mut Graph) {
    for _ in 0..graph.blocks.len() {
        if !split_one(graph) {
            break;
        }
    }
}
