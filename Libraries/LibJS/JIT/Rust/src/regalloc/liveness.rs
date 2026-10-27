/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Liveness of SSA values, the pre-pass of the register allocator.
//!
//! Constants are never live: they are rematerialized at every use. A value
//! used by a frame state is live at the node carrying it; for calls, frame
//! state uses happen after the call returns.

use crate::bitset::BitSet;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::sparse_bitset::SparseBitSet;
use std::cell::OnceCell;

pub(super) struct ValueLiveness {
    pub live_in: Vec<SparseBitSet>,
    pub live_out: Vec<SparseBitSet>,
    /// For each node, the values whose last use (on this path) it is.
    pub dies_at: Vec<Vec<NodeId>>,
    /// Values nobody uses.
    pub dead_outputs: BitSet,
    /// Values that must have a stack slot holding them from their definition
    /// on: those live across a call and those live into a loop header (or a
    /// merge without a state predecessor).
    pub needs_spill: BitSet,
    /// The position of each node in linear order.
    pub positions: Vec<u32>,
    /// For each value, the positions of the nodes that use it as an input, in order. A phi input is used at the
    /// control node of its predecessor, where the moves into the phis are.
    pub use_positions: Vec<Vec<u32>>,
}

/// Whether a node is a value the register allocator tracks.
pub(super) fn is_allocated_value(graph: &Graph, node: NodeId) -> bool {
    let node = graph.node(node);
    node.repr.is_some() && !matches!(node.op, Op::Constant(_) | Op::VirtualObject { .. })
}

/// The allocated values a node uses through its frame state, cached for both walks.
fn cached_frame_state_uses<'a>(
    graph: &'a Graph,
    frames: &'a [OnceCell<Vec<NodeId>>],
    node: NodeId,
) -> impl Iterator<Item = NodeId> + 'a {
    graph.node(node).frame_state.into_iter().flat_map(move |frame| {
        frames[frame.index()]
            .get_or_init(|| {
                graph
                    .frame_state_values(frame)
                    .map(|(_, value)| value)
                    .filter(|value| is_allocated_value(graph, *value))
                    .collect()
            })
            .iter()
            .copied()
    })
}

/// For a merge or a loop header: the predecessor whose register state the
/// block starts with, the first one in linear order, which comes before it.
/// Every other predecessor (another one before it, a cold one or a back
/// edge after it) jumps to it, and moves its values to match that state at
/// its end. `None` for blocks with one predecessor, and for merges that
/// start with every value in its stack slot.
pub(super) fn state_predecessor(graph: &Graph, block: BlockId) -> Option<BlockId> {
    let data = graph.block(block);
    if data.predecessors.len() < 2 {
        return None;
    }
    let first = *data.predecessors.iter().min()?;
    if first >= block {
        return None;
    }
    let others_conform = data
        .predecessors
        .iter()
        .filter(|predecessor| **predecessor != first)
        .all(|predecessor| matches!(graph.control(*predecessor).op, Op::Jump { .. }));
    others_conform.then_some(first)
}

/// The input of each phi of `successor` that flows in from `block`.
pub(super) fn phi_inputs_from(
    graph: &Graph,
    block: BlockId,
    successor: BlockId,
) -> impl Iterator<Item = (NodeId, NodeId)> + '_ {
    let successor_block = graph.block(successor);
    let index = if successor_block.phis.is_empty() {
        0
    } else {
        successor_block
            .predecessors
            .iter()
            .position(|predecessor| *predecessor == block)
            .expect("successors list their predecessors")
    };
    successor_block
        .phis
        .iter()
        .map(move |phi| (*phi, graph.node(*phi).inputs[index]))
}

/// How many calls, and how many nodes that clobber the floating point registers (calls among them), the backwards walk
/// of the liveness computation passed.
#[derive(Clone, Copy, Default)]
struct Clobbers {
    calls: u32,
    float_clobbers: u32,
}

impl ValueLiveness {
    pub fn compute(graph: &Graph) -> Self {
        let value_count = graph.nodes.len();
        let block_count = graph.blocks.len();
        // NB: Both backwards walks and spill analysis use these immutable
        //     dependencies. Flatten each referenced frame only once.
        let frames = (0..graph.frame_states.len())
            .map(|_| OnceCell::new())
            .collect::<Vec<_>>();

        let mut positions = vec![0; value_count];
        let mut use_positions = vec![Vec::new(); value_count];
        let mut position = 0;
        for block in 0..block_count {
            let block = BlockId::from_index(block);
            for node in graph.block_nodes(block) {
                positions[node.index()] = position;
                for input in &graph.node(node).inputs {
                    if graph.node(node).op != Op::Phi && is_allocated_value(graph, *input) {
                        use_positions[input.index()].push(position);
                    }
                }
                position += 1;
            }
            for successor in graph.successors(block) {
                for (_, input) in phi_inputs_from(graph, block, successor) {
                    if is_allocated_value(graph, input) {
                        use_positions[input.index()].push(position - 1);
                    }
                }
            }
        }

        let block_ids = (0..block_count).map(BlockId::from_index).collect::<Vec<_>>();
        // What each block uses before defining it, and what it defines, so
        // that the dataflow below only combines sets.
        let mut used_before_defined = Vec::with_capacity(block_count);
        let mut defined = Vec::with_capacity(block_count);
        let mut used = Vec::new();
        for block in &block_ids {
            let mut live = SparseBitSet::new(value_count);
            Self::transfer_block(graph, &frames, *block, &mut live, &mut used, |_, _, _| {});
            used_before_defined.push(live);
            let mut nodes = SparseBitSet::new(value_count);
            for node in graph.block_nodes(*block) {
                nodes.insert(node.index());
            }
            defined.push(nodes);
        }
        // The inputs of the phis of each block's successors that flow in
        // from it.
        let phi_inputs = block_ids
            .iter()
            .map(|block| {
                graph
                    .successors(*block)
                    .into_iter()
                    .flat_map(|successor| phi_inputs_from(graph, *block, successor))
                    .map(|(_, input)| input)
                    .filter(|input| is_allocated_value(graph, *input))
                    .collect::<Vec<_>>()
            })
            .collect::<Vec<_>>();

        let mut live_in = vec![SparseBitSet::new(value_count); block_count];
        let mut live_out = vec![SparseBitSet::new(value_count); block_count];
        let mut entering = SparseBitSet::new(value_count);
        // Visit backwards initially, then only revisit predecessors whose
        // successor's live-in set changed. Acyclic regions need one visit.
        let mut worklist = block_ids.clone();
        let mut queued = vec![true; block_count];
        while let Some(block) = worklist.pop() {
            queued[block.index()] = false;
            let live = &mut live_out[block.index()];
            for successor in graph.successors(block) {
                live.union_with(&live_in[successor.index()]);
            }
            for input in &phi_inputs[block.index()] {
                live.insert(input.index());
            }
            entering.clone_from(live);
            entering.subtract(&defined[block.index()]);
            entering.union_with(&used_before_defined[block.index()]);
            if live_in[block.index()].union_with(&entering) {
                for predecessor in &graph.block(block).predecessors {
                    if !queued[predecessor.index()] {
                        queued[predecessor.index()] = true;
                        worklist.push(*predecessor);
                    }
                }
            }
        }

        let mut dies_at = vec![Vec::new(); value_count];
        let mut last_death = vec![None; value_count];
        let mut dead_outputs = BitSet::new(value_count);
        let mut needs_spill = BitSet::new(value_count);
        // OPTIMIZATION: Rather than marking every value live across each call or float clobbering slow path there,
        //               which takes time quadratic in the size of code that keeps many values live past many of them
        //               (like a large array literal of arrays), the walk counts those nodes, notes the counts where
        //               each value becomes live, and compares them where the value is defined or the block starts.
        let mut clobbers = Clobbers::default();
        let mut live_since: Vec<Option<(BlockId, Clobbers)>> = vec![None; value_count];
        for block in &block_ids {
            let block_start = clobbers;
            let since = |live_since: &[Option<(BlockId, Clobbers)>], value: usize| match live_since[value] {
                Some((since_block, since)) if since_block == *block => since,
                _ => block_start,
            };
            let check_spill = |needs_spill: &mut BitSet, since: Clobbers, clobbers: Clobbers, value: usize| {
                let is_float = graph.nodes[value].repr == Some(crate::code::Repr::Float64);
                if clobbers.calls != since.calls || (is_float && clobbers.float_clobbers != since.float_clobbers) {
                    needs_spill.insert(value);
                }
            };
            entering.clone_from(&live_out[block.index()]);
            Self::transfer_block(
                graph,
                &frames,
                *block,
                &mut entering,
                &mut used,
                |node, live_after, used| {
                    if live_after.contains(node.index()) {
                        check_spill(
                            &mut needs_spill,
                            since(&live_since, node.index()),
                            clobbers,
                            node.index(),
                        );
                    } else if is_allocated_value(graph, node) {
                        dead_outputs.insert(node.index());
                    }
                    let op = &graph.node(node).op;
                    let is_call = op.properties().is_call;
                    // NB: Their slow paths clobber the floating point registers.
                    let clobbers_floats = super::saves_only_gprs(op);
                    if is_call || clobbers_floats {
                        clobbers.float_clobbers += 1;
                        clobbers.calls += u32::from(is_call);
                        for value in cached_frame_state_uses(graph, &frames, node) {
                            let is_float = graph.nodes[value.index()].repr == Some(crate::code::Repr::Float64);
                            if value != node && (is_call || is_float) {
                                needs_spill.insert(value.index());
                            }
                        }
                    }
                    for value in used {
                        if !live_after.contains(value.index()) && last_death[value.index()] != Some(node) {
                            last_death[value.index()] = Some(node);
                            dies_at[node.index()].push(*value);
                            live_since[value.index()] = Some((*block, clobbers));
                        }
                    }
                },
            );
            // NB: The phis of the block are live from its start if anything uses them.
            let live_phis = graph.block(*block).phis.iter().map(|phi| phi.index()).filter(|phi| {
                live_since[*phi].is_some_and(|(since_block, _)| since_block == *block)
                    || live_out[block.index()].contains(*phi)
            });
            for value in entering.iter().chain(live_phis) {
                check_spill(&mut needs_spill, since(&live_since, value), clobbers, value);
            }
            // NB: Most merges start with the register state of one of their
            //     predecessors (see `state_predecessor()`).
            let predecessors = &graph.block(*block).predecessors;
            if (predecessors.len() > 1 || predecessors.first().is_some_and(|predecessor| predecessor >= block))
                && state_predecessor(graph, *block).is_none()
            {
                for value in live_in[block.index()].iter() {
                    needs_spill.insert(value);
                }
            }
        }

        Self {
            live_in,
            live_out,
            dies_at,
            dead_outputs,
            needs_spill,
            positions,
            use_positions,
        }
    }

    /// Walks a block backwards from the values live at its end, calling
    /// `visit(node, live_after, used)` for every non-phi node, and leaves
    /// the values live into the block (phis excluded) in `live`.
    fn transfer_block(
        graph: &Graph,
        frames: &[OnceCell<Vec<NodeId>>],
        block: BlockId,
        live: &mut SparseBitSet,
        used: &mut Vec<NodeId>,
        mut visit: impl FnMut(NodeId, &SparseBitSet, &[NodeId]),
    ) {
        let block_data = graph.block(block);
        for node in block_data.body.iter().chain(&block_data.control).rev() {
            used.clear();
            used.extend(
                graph
                    .node(*node)
                    .inputs
                    .iter()
                    .copied()
                    .filter(|input| is_allocated_value(graph, *input))
                    .chain(cached_frame_state_uses(graph, frames, *node)),
            );
            visit(*node, live, used);
            live.remove(node.index());
            for value in used.iter() {
                live.insert(value.index());
            }
        }
        for phi in &block_data.phis {
            live.remove(phi.index());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::code::{Repr, ResumeMode};
    use crate::ir::{Block, FrameState, Node};

    #[test]
    fn shared_frames_and_duplicate_uses_have_one_last_use_per_value() {
        let mut graph = Graph::default();
        graph.add_block(Block::default());
        let value = graph.add_node(Node::new(Op::LoadSlot { slot: 0 }, vec![], Some(Repr::Tagged), 0));
        graph.blocks[0].body.push(value);
        let frame = graph.add_frame_state(FrameState {
            executable: 0,
            pc: 0,
            mode: ResumeMode::ResumeAt,
            values: vec![(0, value), (1, value)],
            in_frame: vec![],
            parent: None,
            passed_argument_count: None,
            legacy_arguments: 0..0,
        });
        let mut checks = Vec::new();
        for _ in 0..2 {
            let check = graph.add_node(Node::new(Op::CheckObject, vec![value], Some(Repr::Tagged), 0));
            graph.nodes[check.index()].frame_state = Some(frame);
            graph.blocks[0].body.push(check);
            checks.push(check);
        }
        let constant = graph.add_node(Node::new(Op::Constant(0), vec![], Some(Repr::Tagged), 0));
        let returned = graph.add_node(Node::new(Op::Return, vec![constant], None, 0));
        graph.blocks[0].control = Some(returned);
        let liveness = ValueLiveness::compute(&graph);
        assert!(liveness.dies_at[checks[0].index()].is_empty());
        assert_eq!(liveness.dies_at[checks[1].index()], [value]);
        assert!(!liveness.dead_outputs.contains(value.index()));
        assert!(liveness.live_in[0].is_empty());
    }

    #[test]
    fn many_blocks_with_short_live_ranges() {
        let mut graph = Graph::default();
        let block_count = 8192;
        let mut values = Vec::new();
        for index in 0..block_count {
            let mut block = Block::default();
            let value = if index == 0 {
                graph.add_node(Node::new(Op::LoadSlot { slot: 0 }, vec![], Some(Repr::Tagged), 0))
            } else {
                block.predecessors.push(BlockId::from_index(index - 1));
                graph.add_node(Node::new(
                    Op::CheckObject,
                    vec![values[index - 1]],
                    Some(Repr::Tagged),
                    0,
                ))
            };
            values.push(value);
            block.body.push(value);
            block.control = Some(if index + 1 == block_count {
                graph.add_node(Node::new(Op::Return, vec![value], None, 0))
            } else {
                graph.add_node(Node::new(
                    Op::Jump {
                        target: BlockId::from_index(index + 1),
                    },
                    vec![],
                    None,
                    0,
                ))
            });
            graph.add_block(block);
        }
        let liveness = ValueLiveness::compute(&graph);
        assert!(liveness.live_in[0].is_empty());
        assert!(liveness.live_out[block_count - 1].is_empty());
        for index in 1..block_count {
            assert_eq!(
                liveness.live_in[index].iter().collect::<Vec<_>>(),
                [values[index - 1].index()]
            );
            assert_eq!(
                liveness.live_out[index - 1].iter().collect::<Vec<_>>(),
                [values[index - 1].index()]
            );
            assert_eq!(liveness.dies_at[values[index].index()], [values[index - 1]]);
            assert!(!liveness.dead_outputs.contains(values[index - 1].index()));
        }
        assert_eq!(
            liveness.dies_at[graph.blocks[block_count - 1].control.unwrap().index()],
            [values[block_count - 1]]
        );
    }
}
