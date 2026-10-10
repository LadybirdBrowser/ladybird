/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Graph edits shared by the passes: replacing values, removing nodes, and
//! cleaning up the control flow graph after control nodes lost successors.

use crate::bitset::BitSet;
use crate::code::Repr;
use crate::fast_hash::HashMap;
use crate::ir::BlockId;
use crate::ir::FrameState;
use crate::ir::FrameStateId;
use crate::ir::Graph;
use crate::ir::Node;
use crate::ir::NodeId;
use crate::ir::Op;

/// Values to replace by other values, as a pass decides them.
pub struct Replacements {
    map: Vec<Option<NodeId>>,
}

impl Replacements {
    pub fn new(graph: &Graph) -> Self {
        Self {
            map: vec![None; graph.nodes.len()],
        }
    }

    /// Makes every use of `value` a use of `replacement` instead.
    pub fn replace(&mut self, value: NodeId, replacement: NodeId) {
        debug_assert_ne!(value, replacement);
        if value.index() >= self.map.len() {
            self.map.resize(value.index() + 1, None);
        }
        self.map[value.index()] = Some(replacement);
    }

    /// What a use of `value` uses after the replacements.
    pub fn resolve(&self, mut value: NodeId) -> NodeId {
        while let Some(Some(replacement)) = self.map.get(value.index()) {
            value = *replacement;
        }
        value
    }

    pub fn is_empty(&self) -> bool {
        self.map.iter().all(Option::is_none)
    }

    /// Rewrites every input and frame state value of the graph.
    pub fn apply(&self, graph: &mut Graph) {
        if self.is_empty() {
            return;
        }
        for node in &mut graph.nodes {
            for input in &mut node.inputs {
                *input = self.resolve(*input);
            }
        }
        for frame_state in &mut graph.frame_states {
            for (_, value) in &mut frame_state.values {
                *value = self.resolve(*value);
            }
        }
    }
}

/// Makes every use of a refined value (see `Op::refined_input()`) a use of
/// the value it refines, once the passes no longer move or share nodes:
/// `Refine` nodes go away, and checks no longer have a value, so that
/// refining costs no register.
pub fn remove_refinements(graph: &mut Graph) {
    let mut replacements = Replacements::new(graph);
    let mut removed = BitSet::new(graph.nodes.len());
    let mut checks = Vec::new();
    for block in &graph.blocks {
        for node in &block.body {
            let data = graph.node(*node);
            let Some(index) = data.op.refined_input() else {
                continue;
            };
            if data.repr.is_some() {
                replacements.replace(*node, data.inputs[index]);
            }
            match data.op {
                Op::Refine { .. } => removed.insert(node.index()),
                _ => checks.push(*node),
            }
        }
    }
    replacements.apply(graph);
    remove_nodes(graph, &removed);
    for check in checks {
        graph.nodes[check.index()].repr = None;
    }
}

/// Removes the nodes in `removed` from the blocks they are in. Nodes added
/// after `removed` was made are kept.
pub fn remove_nodes(graph: &mut Graph, removed: &BitSet) {
    let is_removed = |node: &NodeId| node.index() < removed.capacity() && removed.contains(node.index());
    for block in &mut graph.blocks {
        block.body.retain(|node| !is_removed(node));
        block.phis.retain(|node| !is_removed(node));
    }
}

/// The constant nodes of a graph by their bits and representation, for
/// passes that need constants: `get()` and `get_typed()` add those the
/// graph does not have yet.
pub struct Constants {
    nodes: HashMap<(u64, Repr), NodeId>,
}

impl Constants {
    pub fn new(graph: &Graph) -> Self {
        let nodes = graph
            .nodes
            .iter()
            .enumerate()
            .filter_map(|(index, node)| match (&node.op, node.repr) {
                (Op::Constant(bits), Some(repr)) => Some(((*bits, repr), NodeId::from_index(index))),
                _ => None,
            })
            .collect();
        Self { nodes }
    }

    /// The tagged constant node with `bits`.
    pub fn get(&mut self, graph: &mut Graph, bits: u64) -> NodeId {
        self.get_typed(graph, bits, Repr::Tagged)
    }

    /// The constant node with `bits` in representation `repr`.
    pub fn get_typed(&mut self, graph: &mut Graph, bits: u64, repr: Repr) -> NodeId {
        *self
            .nodes
            .entry((bits, repr))
            .or_insert_with(|| graph.add_node(Node::new(Op::Constant(bits), Vec::new(), Some(repr), 0)))
    }
}

/// Whether an entry reaches each block over the edges the control nodes
/// have.
fn reachable_blocks(graph: &Graph) -> Vec<bool> {
    let block_count = graph.blocks.len();
    let mut reachable = vec![false; block_count];
    // NB: A loop header that only its back edges would reach has no
    //     predecessors if none of them was built, but is no entry.
    let mut pending = graph.entries().map(BlockId::index).collect::<Vec<_>>();
    while let Some(block) = pending.pop() {
        if std::mem::replace(&mut reachable[block], true) {
            continue;
        }
        pending.extend(
            graph
                .successors(BlockId::from_index(block))
                .iter()
                .map(|successor| successor.index()),
        );
    }
    reachable
}

/// Drops the blocks no entry reaches, if there are any.
pub fn remove_unreachable_blocks(graph: &mut Graph) {
    if reachable_blocks(graph).iter().any(|reachable| !reachable) {
        clean_up_control_flow(graph);
    }
}

/// Brings predecessor lists and phis in line with the control nodes after
/// passes changed them to branch to fewer blocks, then drops the blocks no
/// entry reaches anymore and the phis left with a single distinct input.
pub fn clean_up_control_flow(graph: &mut Graph) {
    let block_count = graph.blocks.len();
    let reachable = reachable_blocks(graph);

    // Keep the predecessor entries (and their phi inputs) of reachable
    // blocks that still branch here, as many times as they do.
    for block in 0..block_count {
        if !reachable[block] {
            continue;
        }
        let predecessors = graph.blocks[block].predecessors.clone();
        let mut remaining_edges: HashMap<BlockId, usize> = HashMap::default();
        for predecessor in &predecessors {
            if reachable[predecessor.index()] {
                let edges = graph
                    .successors(*predecessor)
                    .iter()
                    .filter(|successor| successor.index() == block)
                    .count();
                remaining_edges.insert(*predecessor, edges);
            }
        }
        let keep = predecessors
            .iter()
            .map(|predecessor| match remaining_edges.get_mut(predecessor) {
                Some(count) if *count > 0 => {
                    *count -= 1;
                    true
                }
                _ => false,
            })
            .collect::<Vec<_>>();
        if keep.iter().all(|keep| *keep) {
            continue;
        }
        let filter = |values: &[NodeId]| {
            values
                .iter()
                .zip(&keep)
                .filter(|(_, keep)| **keep)
                .map(|(value, _)| *value)
                .collect::<Vec<_>>()
        };
        graph.blocks[block].predecessors = predecessors
            .iter()
            .zip(&keep)
            .filter(|(_, keep)| **keep)
            .map(|(predecessor, _)| *predecessor)
            .collect();
        for phi in graph.blocks[block].phis.clone() {
            let inputs = filter(&graph.nodes[phi.index()].inputs);
            graph.nodes[phi.index()].inputs = inputs;
        }
    }

    let order = (0..block_count)
        .filter(|block| reachable[*block])
        .map(BlockId::from_index)
        .collect::<Vec<_>>();
    if order.len() != block_count {
        graph.reorder_blocks(&order);
    }
    remove_trivial_phis(graph);
}

/// Makes the control node `control` jump to `target`, without inputs and
/// a frame state, which jumps never have.
pub fn make_jump(graph: &mut Graph, control: NodeId, target: BlockId) {
    let node = &mut graph.nodes[control.index()];
    node.op = Op::Jump { target };
    node.inputs.clear();
    node.frame_state = None;
}

/// Makes branches whose edges both lead to the same place, through blocks
/// that only jump on, jumps: where the target's phis get the same values
/// from both. Returns whether it changed any.
pub fn fold_branches_to_one_target(graph: &mut Graph) -> bool {
    // NB: Where an edge leads, and the block it enters that from.
    // NB: Blocks that only jump on can make a loop, which leads nowhere.
    let end_of = |graph: &Graph, mut block: BlockId, mut from: BlockId| {
        for _ in 0..graph.blocks.len() {
            let data = graph.block(block);
            let Op::Jump { target } = graph.control(block).op else {
                return (block, from);
            };
            if !data.phis.is_empty() || !data.body.is_empty() || target == block {
                return (block, from);
            }
            from = block;
            block = target;
        }
        (block, from)
    };
    let mut changed = false;
    for block in 0..graph.blocks.len() {
        let block_id = BlockId::from_index(block);
        let control = graph.control_id(block_id);
        let Op::Branch { if_true, if_false, .. } = graph.node(control).op else {
            continue;
        };
        let (true_end, true_from) = end_of(graph, if_true, block_id);
        let (false_end, false_from) = end_of(graph, if_false, block_id);
        if true_end != false_end {
            continue;
        }
        let target = graph.block(true_end);
        let input_from = |from: BlockId| target.predecessors.iter().position(|block| *block == from);
        let (Some(true_index), Some(false_index)) = (input_from(true_from), input_from(false_from)) else {
            continue;
        };
        if target
            .phis
            .iter()
            .any(|phi| graph.node(*phi).inputs[true_index] != graph.node(*phi).inputs[false_index])
        {
            continue;
        }
        make_jump(graph, control, if_true);
        changed = true;
    }
    if changed {
        clean_up_control_flow(graph);
    }
    changed
}

/// A frame state for exits at the end of `predecessor`, resuming at the
/// start of the bytecode block `block` with the values flowing into it from
/// there, if `block` has a start frame state (see
/// `Block::start_frame_state`).
pub fn frame_state_entering(graph: &Graph, block: BlockId, predecessor: BlockId) -> Option<FrameState> {
    let data = graph.block(block);
    let template = data.start_frame_state?;
    let entry_index = data
        .predecessors
        .iter()
        .position(|block| *block == predecessor)
        .expect("the predecessor enters the block");
    let phis = data.phis.clone();
    let mut state: FrameState = graph.frame_state(template).clone();
    for (_, value) in &mut state.values {
        if phis.contains(value) {
            *value = graph.node(*value).inputs[entry_index];
        } else if data.body.contains(value) {
            // NB: Values made in the block do not exist before it.
            return None;
        }
    }
    Some(state)
}

/// Moves the cold blocks after all other blocks, keeping the order of
/// each, so that the code of every other block comes first and the cold
/// code is out of the way.
pub fn move_cold_blocks_last(graph: &mut Graph) {
    if !graph.blocks.iter().any(|block| block.is_cold) {
        return;
    }
    let (hot, cold): (Vec<_>, Vec<_>) = (0..graph.blocks.len())
        .map(BlockId::from_index)
        .partition(|block| !graph.block(*block).is_cold);
    let order = hot.into_iter().chain(cold).collect::<Vec<_>>();
    graph.reorder_blocks(&order);
}

/// Replaces phis whose inputs are all one value (or the phi itself) by
/// that value, until none is left.
pub fn remove_trivial_phis(graph: &mut Graph) {
    loop {
        let mut replacements = Replacements::new(graph);
        let mut removed = BitSet::new(graph.nodes.len());
        for block in &graph.blocks {
            for phi in &block.phis {
                let mut distinct = graph.nodes[phi.index()]
                    .inputs
                    .iter()
                    .map(|input| replacements.resolve(*input))
                    .filter(|input| input != phi);
                let Some(first) = distinct.next() else {
                    continue;
                };
                if distinct.all(|input| input == first) {
                    replacements.replace(*phi, first);
                    removed.insert(phi.index());
                }
            }
        }
        if replacements.is_empty() {
            return;
        }
        replacements.apply(graph);
        remove_nodes(graph, &removed);
    }
}

/// Drops the frame states no node uses anymore (directly or as the parent
/// of one it uses) and renumbers the others in their order.
pub fn remove_unused_frame_states(graph: &mut Graph) {
    // NB: No pass exits to the start of blocks anymore.
    for block in &mut graph.blocks {
        block.start_frame_state = None;
    }
    let count = graph.frame_states.len();
    let mut used = vec![false; count];
    for block in &graph.blocks {
        for node in block.nodes() {
            let mut frame_state = graph.nodes[node.index()].frame_state;
            while let Some(id) = frame_state {
                if std::mem::replace(&mut used[id.index()], true) {
                    break;
                }
                frame_state = graph.frame_states[id.index()].parent;
            }
        }
    }
    if used.iter().all(|used| *used) {
        return;
    }
    let mut new_index = vec![None; count];
    let mut next = 0;
    for (index, used) in used.iter().enumerate() {
        if *used {
            new_index[index] = Some(FrameStateId(next));
            next += 1;
        }
    }
    let remap = |id: FrameStateId| new_index[id.index()].expect("used frame states only refer to used ones");
    let old = std::mem::take(&mut graph.frame_states);
    graph.frame_states = old
        .into_iter()
        .zip(&used)
        .filter(|(_, used)| **used)
        .map(|(mut frame_state, _)| {
            frame_state.parent = frame_state.parent.map(remap);
            frame_state
        })
        .collect();
    for block in &graph.blocks {
        for node in block.nodes() {
            let node = &mut graph.nodes[node.index()];
            node.frame_state = node.frame_state.map(remap);
        }
    }
}
