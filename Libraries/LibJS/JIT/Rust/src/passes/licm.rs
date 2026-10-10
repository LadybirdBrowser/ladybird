/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Loop invariant code motion.
//!
//! Nodes in a loop whose inputs are all defined outside of it, which have
//! no effects and read no memory the loop may write, move to the loop's
//! preheader: the one block outside the loop that enters it. Loops entered
//! from several blocks (like the function entry and an on-stack replacement
//! entry) first get a preheader that merges what those bring.
//!
//! Checks and other nodes that may exit move only from blocks every
//! iteration of the loop runs (the header, and blocks on the way to every
//! back edge), so they exit early at most where the loop runs no iteration
//! to its end, and only in loops whose header has a frame state to exit
//! with: the interpreter then resumes at the loop header, with the values
//! the preheader brings into the loop. Hoisting a check whose kind of exit
//! was taken at the loop header before is never repeated.

use super::dominators::Dominators;
use super::edit;
use crate::bitset::BitSet;
use crate::ir::Block;
use crate::ir::BlockId;
use crate::ir::FrameStateId;
use crate::ir::Graph;
use crate::ir::Locations;
use crate::ir::Node;
use crate::ir::Op;

struct Loop {
    header: BlockId,
    preheader: BlockId,
    blocks: BitSet,
    /// The blocks in the loop that jump back to its header.
    back_edge_sources: Vec<BlockId>,
}

/// Gives each loop header that several earlier blocks outside the loop
/// enter, or one that also leads elsewhere, a preheader: a new block right
/// before the header that they jump to instead, with phis of what they
/// bring.
/// Returns whether it added any.
fn add_preheaders(graph: &mut Graph, dominators: &Dominators) -> bool {
    let count = graph.blocks.len();
    let mut order = Vec::new();
    for header in (0..count).map(BlockId::from_index) {
        let predecessors = graph.block(header).predecessors.clone();
        let (back_edge_sources, entries): (Vec<BlockId>, Vec<BlockId>) = predecessors
            .iter()
            .partition(|predecessor| dominators.dominates(header, **predecessor));
        // NB: Edges from later blocks the header does not dominate are
        //     back edges of cycles around the header, which an on-stack
        //     replacement entry inside them reaches without the header.
        if back_edge_sources.is_empty()
            || entries.is_empty()
            || entries.iter().any(|entry| entry.index() > header.index())
            || (entries.len() == 1 && *graph.successors(entries[0]) == [header])
        {
            order.push(header);
            continue;
        }
        let preheader = graph.add_block(Block {
            predecessors: entries.clone(),
            ..Block::default()
        });
        for entry in &entries {
            graph.replace_successor(*entry, header, preheader);
        }
        for phi in graph.block(header).phis.clone() {
            let (back_inputs, entry_inputs): (Vec<_>, Vec<_>) = predecessors
                .iter()
                .copied()
                .zip(graph.node(phi).inputs.iter().copied())
                .partition(|(predecessor, _)| back_edge_sources.contains(predecessor));
            let entry_inputs = entry_inputs.into_iter().map(|(_, input)| input).collect::<Vec<_>>();
            let entering = if entry_inputs.iter().all(|input| *input == entry_inputs[0]) {
                entry_inputs[0]
            } else {
                let merged = graph.add_node(Node::new(
                    Op::Phi,
                    entry_inputs,
                    graph.node(phi).repr,
                    graph.node(phi).pc,
                ));
                graph.blocks[preheader.index()].phis.push(merged);
                merged
            };
            let inputs = std::iter::once(entering)
                .chain(back_inputs.into_iter().map(|(_, input)| input))
                .collect();
            graph.nodes[phi.index()].inputs = inputs;
        }
        let jump = graph.add_node(Node::new(
            Op::Jump { target: header },
            Vec::new(),
            None,
            graph.control(header).pc,
        ));
        graph.blocks[preheader.index()].control = Some(jump);
        graph.blocks[header.index()].predecessors = std::iter::once(preheader).chain(back_edge_sources).collect();
        order.push(preheader);
        order.push(header);
    }
    if graph.blocks.len() == count {
        return false;
    }
    graph.reorder_blocks(&order);
    true
}

fn find_loops(graph: &Graph, dominators: &Dominators) -> Vec<Loop> {
    let mut loops = Vec::new();
    for (index, block) in graph.blocks.iter().enumerate() {
        let header = BlockId::from_index(index);
        let back_edge_sources = dominators.back_edge_sources(graph, header);
        if back_edge_sources.is_empty() {
            continue;
        }
        let mut blocks = BitSet::new(graph.blocks.len());
        blocks.insert(index);
        let mut pending = back_edge_sources.clone();
        while let Some(block) = pending.pop() {
            if blocks.contains(block.index()) {
                continue;
            }
            blocks.insert(block.index());
            pending.extend(graph.block(block).predecessors.iter().copied());
        }
        let entries = block
            .predecessors
            .iter()
            .copied()
            .filter(|predecessor| !blocks.contains(predecessor.index()))
            .collect::<Vec<_>>();
        let [preheader] = entries.as_slice() else {
            continue;
        };
        if *graph.successors(*preheader) != [header] {
            continue;
        }
        loops.push(Loop {
            header,
            preheader: *preheader,
            blocks,
            back_edge_sources,
        });
    }
    loops
}

pub fn run(graph: &mut Graph) {
    // NB: Loops have a back edge from a later block or the block itself.
    let has_loops = graph.blocks.iter().enumerate().any(|(index, block)| {
        block
            .predecessors
            .iter()
            .any(|predecessor| predecessor.index() >= index)
    });
    if !has_loops {
        return;
    }
    let mut dominators = Dominators::compute(graph);
    if add_preheaders(graph, &dominators) {
        dominators = Dominators::compute(graph);
    }
    let loops = find_loops(graph, &dominators);
    if loops.is_empty() {
        return;
    }
    let mut block_of = vec![None; graph.nodes.len()];
    for (index, block) in graph.blocks.iter().enumerate() {
        for node in block.nodes() {
            block_of[node.index()] = Some(index);
        }
    }

    // Inner loops come later in this order; hoisting out of them first lets
    // their invariants move on out of the outer loops.
    for loop_ in loops.iter().rev() {
        // NB: Calls and other code that runs may write anything.
        let mut writes = Locations::NONE;
        for block in loop_.blocks.iter() {
            for node in graph.block_nodes(BlockId::from_index(block)) {
                let properties = graph.node(node).op.properties();
                writes |= if properties.is_call || properties.runs_code {
                    Locations::ALL
                } else {
                    properties.writes
                };
            }
        }
        let header_pc = graph.block(loop_.header).bytecode_start;
        let mut exit_state: Option<Option<FrameStateId>> = None;
        let mut hoisted = Vec::new();
        for block in loop_.blocks.iter() {
            let block_id = BlockId::from_index(block);
            let runs_every_iteration = dominators.runs_every_iteration(block_id, &loop_.back_edge_sources);
            for node in graph.block(block_id).body.clone() {
                let data = graph.node(node);
                let properties = data.op.properties();
                let invariant_inputs = data
                    .inputs
                    .iter()
                    .all(|input| block_of[input.index()].is_none_or(|block| !loop_.blocks.contains(block)));
                // NB: Refinements belong to the edge into their block.
                let movable = !matches!(data.op, Op::Refine { .. })
                    && !properties.is_call
                    && !properties.can_lazy_exit
                    && !properties.allocates
                    && properties.writes.is_empty()
                    && !properties.runs_code
                    && !properties.reads.intersects(writes);
                if !invariant_inputs || !movable {
                    continue;
                }
                if let Some(kind) = data.op.exit_kind() {
                    let gated = header_pc.is_some_and(|pc| graph.exit_sites.contains(&(pc, kind)));
                    if !runs_every_iteration || gated {
                        continue;
                    }
                    let state = *exit_state.get_or_insert_with(|| {
                        let state = edit::frame_state_entering(graph, loop_.header, loop_.preheader)?;
                        Some(graph.add_frame_state(state))
                    });
                    let Some(state) = state else {
                        continue;
                    };
                    graph.nodes[node.index()].frame_state = Some(state);
                }
                graph.blocks[block].body.retain(|other| *other != node);
                block_of[node.index()] = Some(loop_.preheader.index());
                hoisted.push(node);
            }
        }
        graph.blocks[loop_.preheader.index()].body.extend(hoisted);
    }
}
