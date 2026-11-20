/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Loop rotation.
//!
//! Loops with their test at the bottom start at the test: the code before the loop jumps there, and the test jumps
//! back to the body. An on-stack replacement entry at a test that does more than branch (like `i < this.length`, whose
//! property load starts the test's block) runs the branch itself and enters the body, so the loop has two entries:
//! the test, from before the loop, and the body. Such a loop is irreducible: none of its blocks dominates the others,
//! and loop invariant code motion leaves it alone.
//!
//! This pass rotates such loops: the blocks that the test dominates are copied for the paths that enter the test from
//! before the loop, which then run the copy and enter the body (or leave the loop) from there, like the on-stack
//! replacement entry does. The body becomes the loop's only entry, its header, and the test its latch. Values the
//! test makes only reach the blocks it leads to through their phis, since it dominates none of them, so the copy's
//! edges bring its values to those phis.

use ak::Zone;
use ak::ZoneVec;

use super::dominators::Dominators;
use super::edit;
use crate::ir::Block;
use crate::ir::BlockId;
use crate::ir::FrameStateId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use ak::ZoneBitSet;

/// How many nodes the blocks of a test that gets copied may have at most.
const MAX_COPIED_NODES: usize = 64;

pub fn run(graph: &mut Graph, work: &Zone, steps: &mut [Zone; 2]) {
    let [step, _] = steps;
    if graph.osr_entries.is_empty() {
        return;
    }
    let mut rotated = false;
    let mut dominators = Dominators::new_in(work);
    // NB: Each rotation makes one irreducible loop reducible; a graph has fewer such loops than blocks.
    for _ in 0..graph.blocks.len() {
        // NB: Each rotation makes tables as large as the graph, which go when it ends.
        let rotated_one = step.scope(|step| {
            dominators.compute(graph);
            let Some((test, region, sources)) = find_rotatable_test(graph, &dominators, step) else {
                return false;
            };
            rotate(graph, test, &region, &sources, step);
            true
        });
        if !rotated_one {
            break;
        }
        rotated = true;
    }
    if rotated {
        lay_out_in_reverse_post_order(graph, work);
        edit::remove_trivial_phis(graph, work);
    }
}

/// The blocks a depth-first walk from the entries reaches each block from over a retreating edge: an edge to a block
/// the walk is still in.
fn retreating_edges<'w>(graph: &Graph, work: &'w Zone) -> ZoneVec<'w, ZoneVec<'w, BlockId>> {
    let mut sources = ZoneVec::from_iter_in((0..graph.blocks.len()).map(|_| ZoneVec::new_in(work)), work);
    let mut state = ZoneVec::from_elem_in(0u8, graph.blocks.len(), work);
    const ON_STACK: u8 = 1;
    const DONE: u8 = 2;
    let mut stack = ZoneVec::new_in(work);
    for entry in graph.entries() {
        if state[entry.index()] != 0 {
            continue;
        }
        stack.clear();
        stack.push((entry, 0usize));
        state[entry.index()] = ON_STACK;
        while let Some((block, next)) = stack.last_mut() {
            let block = *block;
            let successors = graph.successors(block);
            let Some(successor) = successors.get(*next).copied() else {
                state[block.index()] = DONE;
                stack.pop();
                continue;
            };
            *next += 1;
            match state[successor.index()] {
                0 => {
                    state[successor.index()] = ON_STACK;
                    stack.push((successor, 0));
                }
                ON_STACK => sources[successor.index()].push(block),
                _ => {}
            }
        }
    }
    sources
}

/// A block that a retreating edge it does not dominate leads to, with the blocks it dominates, if they can be copied
/// for the paths that enter it from elsewhere, and the sources of the retreating edges to it.
fn find_rotatable_test<'w>(
    graph: &Graph,
    dominators: &Dominators,
    work: &'w Zone,
) -> Option<(BlockId, ZoneBitSet<'w>, ZoneVec<'w, BlockId>)> {
    let mut retreating = retreating_edges(graph, work);
    let entries = ZoneVec::from_iter_in(graph.entries(), work);
    let mut region = ZoneBitSet::new_in(graph.blocks.len(), work);
    'blocks: for (index, sources) in retreating.iter().enumerate() {
        let test = BlockId::from_index(index);
        if sources.is_empty()
            || entries.contains(&test)
            || sources.iter().all(|source| dominators.dominates(test, *source))
        {
            continue;
        }
        region.clear();
        let mut node_count = 0;
        for (block, block_sources) in retreating.iter().enumerate() {
            let block_id = BlockId::from_index(block);
            if !dominators.is_reachable(block_id) || !dominators.dominates(test, block_id) {
                continue;
            }
            region.insert(block);
            node_count += graph.block_nodes(block_id).count();
            // NB: Copying a loop inside the region would take more than copying its blocks.
            if node_count > MAX_COPIED_NODES || (block != index && !block_sources.is_empty()) {
                continue 'blocks;
            }
        }
        let enters_from_before = graph
            .block(test)
            .predecessors
            .iter()
            .any(|predecessor| !sources.contains(predecessor));
        // NB: Where the other entry enters an inner loop at its header, the test leads to that header, and rotating
        //     would make it the header of both loops: the inner loop would be no loop of its own anymore.
        let leads_to_loop_header = region.iter().any(|block| {
            graph
                .successors(BlockId::from_index(block))
                .iter()
                .any(|successor| !region.contains(successor.index()) && graph.block(*successor).is_loop_header)
        });
        if sources.iter().any(|source| region.contains(source.index())) || !enters_from_before || leads_to_loop_header {
            continue;
        }
        return Some((test, region, retreating[index].take()));
    }
    None
}

/// Copies the blocks of `region`, which `test` dominates, for the predecessors of `test` that enter it from before
/// the loop (all but the `sources` of retreating edges), and makes those predecessors branch to the copy of `test`.
fn rotate(graph: &mut Graph, test: BlockId, region: &ZoneBitSet, sources: &[BlockId], work: &Zone) {
    let entering = ZoneVec::from_iter_in(
        graph
            .block(test)
            .predecessors
            .iter()
            .map(|predecessor| !sources.contains(predecessor)),
        work,
    );
    let mut inputs = ZoneVec::new_in(work);
    let mut phis = ZoneVec::new_in(work);

    // The copies of the blocks, and of their nodes, with the inputs and frame states of the originals for now. The
    // copy of the test takes the phi inputs from the predecessors that enter it.
    let mut block_copy = ZoneVec::from_elem_in(None, graph.blocks.len(), work);
    let mut node_copy: ZoneVec<Option<NodeId>> = ZoneVec::from_elem_in(None, graph.nodes.len(), work);
    let blocks = ZoneVec::from_iter_in(region.iter().map(BlockId::from_index), work);
    for &block in &blocks {
        let data = graph.block(block);
        let copy = graph.add_block(Block {
            bytecode_start: data.bytecode_start,
            is_cold: data.is_cold,
            start_frame_state: data.start_frame_state,
            ..graph.new_block()
        });
        block_copy[block.index()] = Some(copy);
    }
    let copied = |block: BlockId, block_copy: &[Option<BlockId>]| block_copy[block.index()].expect("in the region");
    let test_copy = copied(test, &block_copy);
    phis.assign(&graph.block(test).phis);
    for phi in phis.iter().copied() {
        inputs.clear();
        inputs.extend(
            graph
                .node(phi)
                .inputs
                .iter()
                .zip(&entering)
                .filter(|(_, entering)| **entering)
                .map(|(input, _)| *input),
        );
        node_copy[phi.index()] = Some(if inputs.iter().all(|input| *input == inputs[0]) {
            inputs[0]
        } else {
            let data = graph.node(phi);
            let copy = graph.add(Op::Phi, &inputs, data.repr, data.pc);
            graph.blocks[test_copy.index()].phis.push(copy);
            copy
        });
    }
    let mut copies = ZoneVec::new_in(work);
    let mut body = ZoneVec::new_in(work);
    let mut predecessors = ZoneVec::new_in(work);
    for &block in &blocks {
        let copy = copied(block, &block_copy);
        let data = graph.block(block);
        phis.clear();
        if block != test {
            phis.extend_from_slice(&data.phis);
        }
        body.assign(&data.body);
        let control = data.control;
        predecessors.assign(&data.predecessors);
        for (node, is_phi) in phis
            .iter()
            .map(|node| (*node, true))
            .chain(body.iter().chain(&control).map(|node| (*node, false)))
        {
            let new = graph.add_node(graph.node(node).clone());
            node_copy.resize(graph.nodes.len(), None);
            node_copy[node.index()] = Some(new);
            copies.push(new);
            let copy_data = &mut graph.blocks[copy.index()];
            if is_phi {
                copy_data.phis.push(new);
            } else if Some(node) == control {
                copy_data.control = Some(new);
            } else {
                copy_data.body.push(new);
            }
        }
        if block != test {
            let copy_predecessors = &mut graph.blocks[copy.index()].predecessors;
            copy_predecessors.clear();
            copy_predecessors.extend(predecessors.iter().map(|predecessor| copied(*predecessor, &block_copy)));
        }
    }
    let map =
        |node: NodeId, node_copy: &[Option<NodeId>]| node_copy.get(node.index()).copied().flatten().unwrap_or(node);

    // The copies use the copies of the region's values, and exit with copies of the frame states that refer to them.
    // Frame states that refer to none (like those of the callers of inlined calls) stay shared.
    let mut frame_state_copy: ZoneVec<Option<FrameStateId>> =
        ZoneVec::from_elem_in(None, graph.frame_states.len(), work);
    let mut chain = ZoneVec::new_in(work);
    let mut values = ZoneVec::new_in(work);
    let mut copy_frame_state = |graph: &mut Graph, id: FrameStateId| -> FrameStateId {
        chain.clear();
        chain.extend(graph.frame_state_chain(id));
        let mut copied_parent = None;
        for id in chain.iter().rev() {
            if let Some(copy) = frame_state_copy[id.index()] {
                copied_parent = Some(copy);
                continue;
            }
            let original = graph.frame_state(*id);
            values.clear();
            values.extend(
                original
                    .values
                    .iter()
                    .map(|(slot, value)| (*slot, map(*value, &node_copy))),
            );
            let parent = if copied_parent.is_some() {
                copied_parent
            } else {
                original.parent
            };
            let copy = if *original.values == *values && original.parent == parent {
                *id
            } else {
                let mut state = edit::frame_state_with_values(graph, *id, &values);
                state.parent = parent;
                graph.add_frame_state(state)
            };
            frame_state_copy[id.index()] = Some(copy);
            copied_parent = Some(copy);
        }
        copied_parent.expect("the chain is not empty")
    };
    for &copy in &copies {
        for input in &mut graph.nodes[copy.index()].inputs {
            *input = map(*input, &node_copy);
        }
        if let Some(frame_state) = graph.node(copy).frame_state {
            graph.nodes[copy.index()].frame_state = Some(copy_frame_state(graph, frame_state));
        }
    }
    for &block in &blocks {
        let copy = copied(block, &block_copy);
        for successor in graph.successors(copy) {
            if let Some(successor_copy) = block_copy[successor.index()] {
                graph.replace_successor(copy, successor, successor_copy);
            }
        }
        if let Some(frame_state) = graph.block(copy).start_frame_state {
            graph.blocks[copy.index()].start_frame_state = Some(copy_frame_state(graph, frame_state));
        }
    }

    // The edges of the copies out of the region bring the copies of the values the originals bring.
    for &block in &blocks {
        let copy = copied(block, &block_copy);
        for successor in graph.successors(block) {
            if region.contains(successor.index()) {
                continue;
            }
            // NB: A block that branches to the same block twice is its predecessor twice.
            let already = graph
                .block(successor)
                .predecessors
                .iter()
                .filter(|predecessor| **predecessor == copy)
                .count();
            let position = graph
                .block(successor)
                .predecessors
                .iter()
                .enumerate()
                .filter(|(_, predecessor)| **predecessor == block)
                .map(|(position, _)| position)
                .nth(already)
                .expect("each edge from the block has a place among the predecessors");
            graph.blocks[successor.index()].predecessors.push(copy);
            phis.assign(&graph.block(successor).phis);
            for phi in phis.iter().copied() {
                let input = map(graph.node(phi).inputs[position], &node_copy);
                graph.nodes[phi.index()].inputs.push(input);
            }
        }
    }

    // The blocks of the loop that the test leads to are its header now, which a rotation of a loop around it must not
    // make the header of that loop too.
    let mut visited = ZoneBitSet::new_in(graph.blocks.len(), work);
    let mut pending = ZoneVec::new_in(work);
    for &block in &blocks {
        for successor in graph.successors(block) {
            if !region.contains(successor.index())
                && reaches_without(graph, successor, sources, test, (&mut visited, &mut pending))
            {
                graph.blocks[successor.index()].is_loop_header = true;
            }
        }
    }
    graph.blocks[test.index()].is_loop_header = false;

    // The predecessors that entered the test enter its copy instead, and the test keeps the others.
    predecessors.assign(&graph.block(test).predecessors);
    for (predecessor, entering) in predecessors.iter().zip(&entering) {
        if *entering {
            graph.replace_successor(*predecessor, test, test_copy);
        }
    }
    let selected = |keep: bool| {
        predecessors
            .iter()
            .zip(&entering)
            .filter(move |(_, entering)| **entering == keep)
            .map(|(predecessor, _)| *predecessor)
    };
    let test_copy_predecessors = &mut graph.blocks[test_copy.index()].predecessors;
    test_copy_predecessors.clear();
    test_copy_predecessors.extend(selected(true));
    let test_predecessors = &mut graph.blocks[test.index()].predecessors;
    test_predecessors.clear();
    test_predecessors.extend(selected(false));
    phis.assign(&graph.block(test).phis);
    for phi in phis.iter().copied() {
        inputs.clear();
        inputs.extend(
            graph
                .node(phi)
                .inputs
                .iter()
                .zip(&entering)
                .filter(|(_, entering)| !**entering)
                .map(|(input, _)| *input),
        );
        graph.nodes[phi.index()].inputs.assign(&inputs);
    }
    let zone = graph.zone;
    for index in 0..graph.legacy_arguments_guards.len() {
        let (guard, nodes) = &graph.legacy_arguments_guards[index];
        if let Some(copy) = node_copy.get(guard.index()).copied().flatten() {
            let nodes = ZoneVec::from_iter_in(nodes.iter().map(|node| map(*node, &node_copy)), zone);
            graph.legacy_arguments_guards.push((copy, nodes));
        }
    }
    for index in 0..graph.slow_paths_initializing_frame.len() {
        let node = graph.slow_paths_initializing_frame[index];
        if let Some(copy) = node_copy.get(node.index()).copied().flatten() {
            graph.slow_paths_initializing_frame.push(copy);
        }
    }
}

/// Whether a path that does not go through `avoided` leads from `start` to one of `targets`. `visited` and `pending`
/// are lists for the walk, which it empties first.
fn reaches_without(
    graph: &Graph,
    start: BlockId,
    targets: &[BlockId],
    avoided: BlockId,
    (visited, pending): (&mut ZoneBitSet, &mut ZoneVec<BlockId>),
) -> bool {
    visited.clear();
    pending.clear();
    pending.push(start);
    while let Some(block) = pending.pop() {
        if targets.contains(&block) {
            return true;
        }
        if block == avoided || visited.contains(block.index()) {
            continue;
        }
        visited.insert(block.index());
        pending.extend(graph.successors(block).iter().copied());
    }
    false
}

/// Orders the blocks so that each comes after its predecessors but those that reach it over a retreating edge, and
/// marks the blocks such edges lead to as loop headers too.
fn lay_out_in_reverse_post_order(graph: &mut Graph, work: &Zone) {
    let mut post_order = ZoneVec::with_capacity_in(graph.blocks.len(), work);
    let mut visited = ZoneVec::from_elem_in(false, graph.blocks.len(), work);
    // NB: The walk starts at the on-stack replacement entries, so that block 0 comes first.
    let entries = ZoneVec::from_iter_in(graph.entries(), work);
    let mut stack = ZoneVec::new_in(work);
    for &entry in entries.iter().rev() {
        if visited[entry.index()] {
            continue;
        }
        visited[entry.index()] = true;
        stack.clear();
        stack.push((entry, graph.successors(entry).len()));
        while let Some((block, remaining)) = stack.last_mut() {
            let block = *block;
            if *remaining == 0 {
                post_order.push(block);
                stack.pop();
                continue;
            }
            *remaining -= 1;
            // NB: Successors are visited last to first, so that the first comes right after its block.
            let successor = graph.successors(block)[*remaining];
            if !visited[successor.index()] {
                visited[successor.index()] = true;
                stack.push((successor, graph.successors(successor).len()));
            }
        }
    }
    post_order.reverse();
    graph.reorder_blocks(&post_order, work);
    for index in 0..graph.blocks.len() {
        let is_cold = graph.blocks[index].is_cold;
        let reached_from_later = graph.blocks[index]
            .predecessors
            .iter()
            .any(|predecessor| predecessor.index() >= index && !(graph.block(*predecessor).is_cold && !is_cold));
        graph.blocks[index].is_loop_header |= reached_from_later;
    }
}
