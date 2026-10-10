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

pub(super) struct ValueLiveness {
    pub live_in: Vec<BitSet>,
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
    /// For each value, the positions of the nodes that use it as an input, in order.
    pub use_positions: Vec<Vec<u32>>,
}

/// Whether a node is a value the register allocator tracks.
pub(super) fn is_allocated_value(graph: &Graph, node: NodeId) -> bool {
    let node = graph.node(node);
    node.repr.is_some() && !matches!(node.op, Op::Constant(_) | Op::VirtualObject { .. })
}

/// The values a node uses through its frame state.
pub(super) fn frame_state_uses(graph: &Graph, node: NodeId) -> impl Iterator<Item = NodeId> + '_ {
    graph
        .node(node)
        .frame_state
        .into_iter()
        .flat_map(|frame_state| graph.frame_state_values(frame_state))
        .map(|(_, value)| value)
        .filter(|value| is_allocated_value(graph, *value))
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
pub(super) fn phi_inputs_from(graph: &Graph, block: BlockId, successor: BlockId) -> Vec<(NodeId, NodeId)> {
    let successor_block = graph.block(successor);
    if successor_block.phis.is_empty() {
        return Vec::new();
    }
    let index = successor_block
        .predecessors
        .iter()
        .position(|predecessor| *predecessor == block)
        .expect("successors list their predecessors");
    successor_block
        .phis
        .iter()
        .map(|phi| (*phi, graph.node(*phi).inputs[index]))
        .collect()
}

impl ValueLiveness {
    pub fn compute(graph: &Graph) -> Self {
        let value_count = graph.nodes.len();
        let block_count = graph.blocks.len();

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
        }

        let block_ids = (0..block_count).map(BlockId::from_index).collect::<Vec<_>>();
        // What each block uses before defining it, and what it defines, so
        // that the dataflow below only combines sets.
        let mut used_before_defined = Vec::with_capacity(block_count);
        let mut defined = Vec::with_capacity(block_count);
        let mut used = Vec::new();
        for block in &block_ids {
            let mut live = BitSet::new(value_count);
            Self::transfer_block(graph, *block, &mut live, &mut used, |_, _, _| {});
            used_before_defined.push(live);
            let mut nodes = BitSet::new(value_count);
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

        let mut live_in = vec![BitSet::new(value_count); block_count];
        let mut live_out = vec![BitSet::new(value_count); block_count];
        let mut changed = true;
        while changed {
            changed = false;
            for block in block_ids.iter().rev() {
                let live = &mut live_out[block.index()];
                for successor in graph.successors(*block) {
                    live.union_with(&live_in[successor.index()]);
                }
                for input in &phi_inputs[block.index()] {
                    live.insert(input.index());
                }
                let mut entering = live.clone();
                entering.subtract(&defined[block.index()]);
                entering.union_with(&used_before_defined[block.index()]);
                changed |= live_in[block.index()].union_with(&entering);
            }
        }

        let mut dies_at = vec![Vec::new(); value_count];
        let mut dead_outputs = BitSet::new(value_count);
        let mut needs_spill = BitSet::new(value_count);
        for block in &block_ids {
            let mut live = live_out[block.index()].clone();
            Self::transfer_block(graph, *block, &mut live, &mut used, |node, live_after, used| {
                if is_allocated_value(graph, node) && !live_after.contains(node.index()) {
                    dead_outputs.insert(node.index());
                }
                let op = &graph.node(node).op;
                if op.properties().is_call {
                    for value in live_after
                        .iter()
                        .chain(frame_state_uses(graph, node).map(NodeId::index))
                    {
                        if value != node.index() {
                            needs_spill.insert(value);
                        }
                    }
                } else if super::saves_only_gprs(op) {
                    // NB: Their slow paths clobber the floating point registers.
                    for value in live_after
                        .iter()
                        .chain(frame_state_uses(graph, node).map(NodeId::index))
                    {
                        let is_float = graph.nodes[value].repr == Some(crate::code::Repr::Float64);
                        if value != node.index() && is_float {
                            needs_spill.insert(value);
                        }
                    }
                }
                for value in used {
                    if !live_after.contains(value.index()) && !dies_at[node.index()].contains(value) {
                        dies_at[node.index()].push(*value);
                    }
                }
            });
            // NB: Most merges start with the register state of one of their
            //     predecessors (see `state_predecessor()`).
            let predecessors = &graph.block(*block).predecessors;
            if (predecessors.len() > 1 || predecessors.first().is_some_and(|predecessor| predecessor >= block))
                && state_predecessor(graph, *block).is_none()
            {
                needs_spill.union_with(&live_in[block.index()]);
            }
        }

        Self {
            live_in,
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
        block: BlockId,
        live: &mut BitSet,
        used: &mut Vec<NodeId>,
        mut visit: impl FnMut(NodeId, &BitSet, &[NodeId]),
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
                    .chain(frame_state_uses(graph, *node)),
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
