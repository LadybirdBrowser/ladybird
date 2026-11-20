/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Loop invariant code motion.
//!
//! Nodes in a loop whose inputs are all defined outside of it, which have
//! no effects and read no memory the loop may write, move to the loop's
//! preheader: the one block outside the loop that enters it. Loads of the
//! slots that nothing writes (`Graph::unwritten_slots`) read memory no loop
//! writes, even one that calls. Loops entered
//! from several blocks (like the function entry and an on-stack replacement
//! entry) first get a preheader that merges what those bring.
//!
//! Checks and other nodes that may exit move too, only in loops whose header
//! has a frame state to exit with: the interpreter then resumes at the loop
//! header, with the values the preheader brings into the loop. Those from
//! blocks that not every iteration runs (like the code after a `continue`),
//! but for cold ones, move speculatively: they may exit where the loop would
//! never have run them, and exit with `ExitKind::Hoisted` instead of their
//! own kind. Hoisting a check whose kind of exit was taken at the loop header
//! before is never repeated, nor are speculative hoists where one of them
//! exited there, so a speculation that fails costs one exit, and keeps the
//! other checks moving.

use ak::Zone;
use ak::ZoneVec;

use super::dominators::Dominators;
use super::edit;
use crate::code::ExitKind;
use crate::ir::Block;
use crate::ir::BlockId;
use crate::ir::FrameStateId;
use crate::ir::Graph;
use crate::ir::Locations;
use crate::ir::Op;
use ak::ZoneBitSet;

struct Loop<'w> {
    header: BlockId,
    preheader: BlockId,
    blocks: ZoneBitSet<'w>,
    /// The blocks in the loop that jump back to its header.
    back_edge_sources: ZoneVec<'w, BlockId>,
}

/// Gives each loop header that several earlier blocks outside the loop
/// enter, or one that also leads elsewhere, a preheader: a new block right
/// before the header that they jump to instead, with phis of what they
/// bring.
/// Returns whether it added any.
fn add_preheaders(graph: &mut Graph, dominators: &Dominators, work: &Zone) -> bool {
    let count = graph.blocks.len();
    let mut order = ZoneVec::new_in(work);
    let mut predecessors = ZoneVec::new_in(work);
    let mut back_edge_sources = ZoneVec::new_in(work);
    let mut entries = ZoneVec::new_in(work);
    let mut phis = ZoneVec::new_in(work);
    let mut back_inputs = ZoneVec::new_in(work);
    let mut entry_inputs = ZoneVec::new_in(work);
    for header in (0..count).map(BlockId::from_index) {
        predecessors.assign(&graph.block(header).predecessors);
        back_edge_sources.clear();
        entries.clear();
        for predecessor in predecessors.iter().copied() {
            if dominators.dominates(header, predecessor) {
                back_edge_sources.push(predecessor);
            } else {
                entries.push(predecessor);
            }
        }
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
            predecessors: graph.list(&entries),
            ..graph.new_block()
        });
        for entry in &entries {
            graph.replace_successor(*entry, header, preheader);
        }
        phis.assign(&graph.block(header).phis);
        for phi in phis.iter().copied() {
            back_inputs.clear();
            entry_inputs.clear();
            for (predecessor, input) in predecessors.iter().zip(graph.node(phi).inputs.iter().copied()) {
                if back_edge_sources.contains(predecessor) {
                    back_inputs.push(input);
                } else {
                    entry_inputs.push(input);
                }
            }
            let entering = if entry_inputs.iter().all(|input| *input == entry_inputs[0]) {
                entry_inputs[0]
            } else {
                let merged = graph.add(Op::Phi, &entry_inputs, graph.node(phi).repr, graph.node(phi).pc);
                graph.blocks[preheader.index()].phis.push(merged);
                merged
            };
            let inputs = &mut graph.nodes[phi.index()].inputs;
            inputs.clear();
            inputs.push(entering);
            inputs.extend_from_slice(&back_inputs);
        }
        let jump = graph.add(Op::Jump { target: header }, &[], None, graph.control(header).pc);
        graph.blocks[preheader.index()].control = Some(jump);
        let header_predecessors = &mut graph.blocks[header.index()].predecessors;
        header_predecessors.clear();
        header_predecessors.push(preheader);
        header_predecessors.extend_from_slice(&back_edge_sources);
        order.push(preheader);
        order.push(header);
    }
    if graph.blocks.len() == count {
        return false;
    }
    graph.reorder_blocks(&order, work);
    true
}

fn find_loops<'w>(graph: &Graph, dominators: &Dominators, work: &'w Zone) -> ZoneVec<'w, Loop<'w>> {
    let mut loops = ZoneVec::new_in(work);
    for (index, block) in graph.blocks.iter().enumerate() {
        let header = BlockId::from_index(index);
        let back_edge_sources = dominators.back_edge_sources(graph, header, work);
        if back_edge_sources.is_empty() {
            continue;
        }
        let blocks = dominators.loop_blocks(graph, header, &back_edge_sources, work);
        let mut entries = block
            .predecessors
            .iter()
            .filter(|predecessor| !blocks.contains(predecessor.index()));
        let (Some(preheader), None) = (entries.next(), entries.next()) else {
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

pub fn run(graph: &mut Graph, work: &Zone) {
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
    let mut dominators = Dominators::of(graph, work);
    if add_preheaders(graph, &dominators, work) {
        // NB: The phis of headers that merged their entries in a preheader
        //     only take the value the preheader brings where the loop leaves
        //     it as it is, which is then invariant in the loop.
        edit::remove_trivial_phis(graph, work);
        dominators.compute(graph);
    }
    let dominators = &dominators;
    let loops = find_loops(graph, dominators, work);
    if loops.is_empty() {
        return;
    }
    let mut block_of = ZoneVec::from_elem_in(None, graph.nodes.len(), work);
    let mut hoisted = ZoneVec::new_in(work);
    let mut body = ZoneVec::new_in(work);
    for (index, block) in graph.blocks.iter().enumerate() {
        for node in block.nodes() {
            block_of[node.index()] = Some(index);
        }
    }

    // Inner loops come later in this order; hoisting out of them first lets
    // their invariants move on out of the outer loops.
    for loop_ in loops.iter().rev() {
        // NB: Code that runs may write anything. Calls of runtime helpers
        //     that run no code write what they say they do.
        let mut writes = Locations::NONE;
        for block in loop_.blocks.iter() {
            for node in graph.block_nodes(BlockId::from_index(block)) {
                let properties = graph.node(node).op.properties();
                writes |= if properties.runs_code {
                    Locations::ALL
                } else {
                    properties.writes
                };
            }
        }
        let header_state = graph.block(loop_.header).start_frame_state;
        let mut exit_state: Option<Option<FrameStateId>> = None;
        hoisted.clear();
        for block in loop_.blocks.iter() {
            let block_id = BlockId::from_index(block);
            let runs_every_iteration = dominators.runs_every_iteration(block_id, &loop_.back_edge_sources);
            let is_cold = graph.block(block_id).is_cold;
            body.assign(&graph.block(block_id).body);
            for node in body.iter().copied() {
                let data = graph.node(node);
                let mut properties = data.op.properties();
                // OPTIMIZATION: Nothing writes these slots, so their loads leave even loops that call, like the
                //               reloads the builder puts at loop headers (see `GraphBuilder::merge()`).
                if let Op::LoadSlot { slot } = data.op
                    && graph.is_unwritten_slot(slot)
                {
                    properties.reads = Locations::NONE;
                }
                let invariant_inputs = data
                    .inputs
                    .iter()
                    .all(|input| block_of[input.index()].is_none_or(|block| !loop_.blocks.contains(block)));
                // NB: Refinements belong to the edge into their block. Calls in
                //     cold blocks stay there, where they rarely run, rather
                //     than run whenever the loop is entered.
                let movable = !matches!(data.op, Op::Refine { .. })
                    && !(properties.is_call && is_cold)
                    && !properties.can_lazy_exit
                    && !properties.allocates
                    && properties.writes.is_empty()
                    && !properties.runs_code
                    && !properties.reads.intersects(writes);
                if !invariant_inputs || !movable {
                    continue;
                }
                if let Some(kind) = data.op.exit_kind() {
                    let speculative = !runs_every_iteration || graph.speculatively_hoisted.contains(&node);
                    let exited = |kind| {
                        header_state.is_some_and(|state| {
                            let state = graph.frame_state(state);
                            graph.exit_sites.contains(&(state.executable, state.pc, kind))
                        })
                    };
                    if (speculative && is_cold) || exited(kind) || (speculative && exited(ExitKind::Hoisted)) {
                        continue;
                    }
                    let state = *exit_state.get_or_insert_with(|| {
                        let state = edit::frame_state_entering(graph, loop_.header, loop_.preheader, work)?;
                        Some(graph.add_frame_state(state))
                    });
                    let Some(state) = state else {
                        continue;
                    };
                    graph.nodes[node.index()].frame_state = Some(state);
                    if speculative && !graph.speculatively_hoisted.contains(&node) {
                        graph.speculatively_hoisted.push(node);
                    }
                }
                graph.blocks[block].body.retain(|other| *other != node);
                block_of[node.index()] = Some(loop_.preheader.index());
                hoisted.push(node);
            }
        }
        graph.blocks[loop_.preheader.index()].body.extend_from_slice(&hoisted);
    }
}
