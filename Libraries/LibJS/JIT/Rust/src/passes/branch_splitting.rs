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

use ak::Zone;
use ak::ZoneVec;

use super::dominators::Dominators;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::FrameStateId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use ak::ZoneBitSet;

/// A block to split: its phis, the block that branches (itself, or the one
/// it jumps to), and for each side of the branch, the predecessor that takes
/// it and its target. The nodes before that branch, which each side gets a
/// copy of, are in `Lists::moved`.
struct Split {
    block: BlockId,
    branch_block: BlockId,
    sides: [(BlockId, BlockId); 2],
}

/// The lists that the splits use, which each split clears.
struct Lists<'w> {
    zone: &'w Zone,
    /// The nodes that each side gets a copy of.
    moved: ZoneVec<'w, NodeId>,
    /// The phis of the block, and the nodes in `moved`.
    phis: ZoneBitSet<'w>,
    frame_state_sides: ZoneVec<'w, (FrameStateId, usize)>,
    block_phis: ZoneVec<'w, NodeId>,
    predecessors: ZoneVec<'w, BlockId>,
    renames: [ZoneVec<'w, (NodeId, NodeId)>; 2],
    copies: ZoneVec<'w, NodeId>,
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
/// It puts the nodes that each side gets a copy of in `moved`.
fn find_split(graph: &Graph, dominators: &Dominators, block: BlockId, moved: &mut ZoneVec<NodeId>) -> Option<Split> {
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
    moved.assign(&data.body);
    if branch_block != block {
        moved.extend_from_slice(&branch_data.body);
    }
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
        sides: [(sides[0]?, targets[0]), (sides[1]?, targets[1])],
    })
}

/// The side of each frame state that a node or block of one side uses and
/// that holds one of the phis, or `None` if one of them cannot be renamed: a
/// node or block of another place uses it, or the phi is in a frame of its
/// chain other than its own, or in a virtual object. The sides go in `sides`.
fn frame_state_sides(
    graph: &Graph,
    dominators: &Dominators,
    split: &Split,
    phis: &ZoneBitSet,
    sides: &mut ZoneVec<(FrameStateId, usize)>,
    work: &Zone,
) -> Option<()> {
    sides.clear();
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
                    .frame_state_values(id, work)
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
            let chain_holds = graph.frame_state_chain(frame_state).skip(1).any(|id| {
                graph
                    .frame_state(id)
                    .values
                    .iter()
                    .any(|(_, value)| phis.contains(value.index()))
            });
            let all_count = graph.frame_state_values(frame_state, work).count();
            let chain_count: usize = graph
                .frame_state_chain(frame_state)
                .map(|id| graph.frame_state(id).values.len())
                .sum();
            // NB: Virtual objects add values past those of the chain's frames.
            let virtual_holds = all_count != chain_count
                && graph
                    .frame_state_values(frame_state, work)
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
    Some(())
}

/// `value`, or what `renames` renames it to.
fn renamed(renames: &[(NodeId, NodeId)], value: NodeId) -> NodeId {
    renames
        .iter()
        .find(|(from, _)| *from == value)
        .map_or(value, |(_, to)| *to)
}

/// Splits one block, if any has the shape of `Split` and all uses of its
/// phis are on one side of its branch. Returns whether it split one. The
/// data of the split that `lists` does not keep goes in `step`.
fn split_one(graph: &mut Graph, dominators: &mut Dominators, lists: &mut Lists, step: &Zone) -> bool {
    dominators.compute(graph);
    let dominators = &*dominators;
    // NB: Splits add nodes, so the set grows ahead of them.
    if lists.phis.capacity() < graph.nodes.len() {
        lists.phis = ZoneBitSet::new_in(graph.nodes.len().max(lists.phis.capacity() * 2), lists.zone);
    }
    for index in 0..graph.blocks.len() {
        let Some(split) = find_split(graph, dominators, BlockId::from_index(index), &mut lists.moved) else {
            continue;
        };
        let phis = &mut lists.phis;
        phis.clear();
        for phi in &graph.block(split.block).phis {
            phis.insert(phi.index());
        }
        for node in &lists.moved {
            phis.insert(node.index());
        }
        let phis = &lists.phis;
        // Every use of a phi must be on one side. A phi's input is used at the end of its predecessor.
        let mut uses_ok = true;
        'blocks: for (block_index, block) in graph.blocks.iter().enumerate() {
            let block_id = BlockId::from_index(block_index);
            if !dominators.is_reachable(block_id) || block_id == split.block || block_id == split.branch_block {
                continue;
            }
            for phi in &block.phis {
                for (predecessor, input) in block.predecessors.iter().zip(&graph.node(*phi).inputs) {
                    if phis.contains(input.index()) && side_of(dominators, &split, *predecessor).is_none() {
                        uses_ok = false;
                        break 'blocks;
                    }
                }
            }
            let side = side_of(dominators, &split, block_id);
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
        if frame_state_sides(graph, dominators, &split, phis, &mut lists.frame_state_sides, step).is_none() {
            continue;
        }

        let Lists {
            moved: moved_nodes,
            frame_state_sides,
            block_phis,
            predecessors,
            renames,
            copies,
            ..
        } = lists;
        block_phis.assign(&graph.block(split.block).phis);
        predecessors.assign(&graph.block(split.block).predecessors);
        for (side, side_renames) in renames.iter_mut().enumerate() {
            let predecessor = split.sides[side].0;
            let index = predecessors
                .iter()
                .position(|block| *block == predecessor)
                .expect("the side's predecessor enters the block");
            side_renames.clear();
            side_renames.extend(block_phis.iter().map(|phi| (*phi, graph.node(*phi).inputs[index])));
        }
        // Each side starts with copies of the moved nodes, with its values.
        for (side, side_renames) in renames.iter_mut().enumerate() {
            let target = split.sides[side].1;
            copies.clear();
            for moved in moved_nodes.iter() {
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
            copies.extend_from_slice(body);
            body.assign(copies);
        }
        let rename = |side: usize, value: NodeId| renamed(&renames[side], value);

        // Rename the uses of each side.
        for block_index in 0..graph.blocks.len() {
            let block_id = BlockId::from_index(block_index);
            if !dominators.is_reachable(block_id) || block_id == split.block || block_id == split.branch_block {
                continue;
            }
            // NB: The blocks stay as they are while the inputs of their nodes change.
            let block = &graph.blocks[block_index];
            for phi in &block.phis {
                for (input_index, predecessor) in block.predecessors.iter().enumerate() {
                    if let Some(side) = side_of(dominators, &split, *predecessor) {
                        let input = graph.nodes[phi.index()].inputs[input_index];
                        graph.nodes[phi.index()].inputs[input_index] = rename(side, input);
                    }
                }
            }
            if let Some(side) = side_of(dominators, &split, block_id) {
                for node in block.body.iter().chain(block.control.iter()) {
                    for input in &mut graph.nodes[node.index()].inputs {
                        *input = rename(side, *input);
                    }
                }
            }
        }
        for (frame_state, side) in frame_state_sides.iter().copied() {
            for (_, value) in &mut graph.frame_states[frame_state.0 as usize].values {
                *value = rename(side, *value);
            }
        }

        // Each side's predecessor jumps straight to its target.
        for side in 0..2 {
            let (predecessor, target) = split.sides[side];
            graph.replace_successor(predecessor, split.block, target);
            graph.blocks[target.index()].predecessors.assign(&[predecessor]);
        }
        graph.blocks[split.block.index()].predecessors.clear();
        for phi in block_phis.iter() {
            graph.nodes[phi.index()].inputs.clear();
        }
        super::edit::clean_up_control_flow(graph, step);
        return true;
    }
    false
}

pub fn run(graph: &mut Graph, work: &Zone, steps: &mut [Zone; 2]) {
    let [step, _] = steps;
    let mut lists = Lists {
        zone: work,
        moved: ZoneVec::new_in(work),
        phis: ZoneBitSet::new_in(0, work),
        frame_state_sides: ZoneVec::new_in(work),
        block_phis: ZoneVec::new_in(work),
        predecessors: ZoneVec::new_in(work),
        renames: [ZoneVec::new_in(work), ZoneVec::new_in(work)],
        copies: ZoneVec::new_in(work),
    };
    let mut dominators = Dominators::new_in(work);
    for _ in 0..graph.blocks.len() {
        // NB: Each split cleans up the control flow, with data as large as the graph, which goes when the split ends.
        if !step.scope(|step| split_one(graph, &mut dominators, &mut lists, step)) {
            break;
        }
    }
}
