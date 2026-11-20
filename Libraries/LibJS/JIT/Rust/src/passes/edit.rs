/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Graph edits shared by the passes: replacing values, removing nodes, and
//! cleaning up the control flow graph after control nodes lost successors.

use crate::code::Repr;
use crate::ir::BlockId;
use crate::ir::FrameState;
use crate::ir::FrameStateId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use ak::Zone;
use ak::ZoneBitSet;
use ak::ZoneMap;
use ak::ZoneVec;

/// Values to replace by other values, as a pass decides them.
pub struct Replacements<'w> {
    map: ZoneVec<'w, Option<NodeId>>,
    initial_node_count: usize,
}

impl<'w> Replacements<'w> {
    /// No replacements, which keep their table in `work`.
    pub fn new(graph: &Graph, work: &'w Zone) -> Self {
        Self {
            map: ZoneVec::new_in(work),
            initial_node_count: graph.nodes.len(),
        }
    }

    /// Forgets all replacements. The table keeps its memory.
    pub fn clear(&mut self) {
        self.map.clear();
    }

    /// Makes every use of `value` a use of `replacement` instead.
    pub fn replace(&mut self, value: NodeId, replacement: NodeId) {
        debug_assert_ne!(value, replacement);
        if value.index() >= self.map.len() {
            self.map.resize((value.index() + 1).max(self.initial_node_count), None);
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
        self.map.is_empty()
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
pub fn remove_refinements(graph: &mut Graph, work: &Zone) {
    let mut replacements = Replacements::new(graph, work);
    let mut removed = ZoneBitSet::new_in(graph.nodes.len(), work);
    let mut checks = ZoneVec::new_in(work);
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
pub fn remove_nodes(graph: &mut Graph, removed: &ZoneBitSet) {
    let is_removed = |node: &NodeId| node.index() < removed.capacity() && removed.contains(node.index());
    for block in &mut graph.blocks {
        block.body.retain(|node| !is_removed(node));
        block.phis.retain(|node| !is_removed(node));
    }
}

/// Makes the int32 operation `node` wrap around instead of checking its
/// result, which it then never exits for.
pub fn make_wrapping(graph: &mut Graph, node: NodeId) {
    let data = &mut graph.nodes[node.index()];
    let Op::Int32Binary { op } = data.op else {
        unreachable!("only int32 operations wrap");
    };
    data.op = Op::Int32WrappingBinary { op };
    data.frame_state = None;
}

/// The constant nodes of a graph by their bits and representation, for
/// passes that need constants: `get()` and `get_typed()` add those the
/// graph does not have yet.
pub struct Constants<'w> {
    nodes: ZoneMap<'w, (u64, Repr), NodeId>,
}

impl<'w> Constants<'w> {
    /// The constants of `graph`, with their table in `work`.
    pub fn new(graph: &Graph, work: &'w Zone) -> Self {
        let nodes = ZoneMap::from_iter_in(
            graph
                .nodes
                .iter()
                .enumerate()
                .filter_map(|(index, node)| match (&node.op, node.repr) {
                    (Op::Constant(bits), Some(repr)) => Some(((*bits, repr), NodeId::from_index(index))),
                    _ => None,
                }),
            work,
        );
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
            .or_insert_with(|| graph.add(Op::Constant(bits), &[], Some(repr), 0))
    }
}

/// Whether an entry reaches each block over the edges the control nodes
/// have.
fn reachable_blocks<'w>(graph: &Graph, work: &'w Zone) -> ZoneVec<'w, bool> {
    let block_count = graph.blocks.len();
    let mut reachable = ZoneVec::from_elem_in(false, block_count, work);
    // NB: A loop header that only its back edges would reach has no
    //     predecessors if none of them was built, but is no entry.
    let mut pending = ZoneVec::from_iter_in(graph.entries().map(BlockId::index), work);
    while let Some(block) = pending.pop() {
        if core::mem::replace(&mut reachable[block], true) {
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
pub fn remove_unreachable_blocks(graph: &mut Graph, work: &Zone) {
    if reachable_blocks(graph, work).iter().any(|reachable| !reachable) {
        clean_up_control_flow(graph, work);
    }
}

/// Brings predecessor lists and phis in line with the control nodes after
/// passes changed them to branch to fewer blocks, then drops the blocks no
/// entry reaches anymore and the phis left with a single distinct input.
pub fn clean_up_control_flow(graph: &mut Graph, work: &Zone) {
    let block_count = graph.blocks.len();
    let reachable = reachable_blocks(graph, work);

    // Keep the predecessor entries (and their phi inputs) of reachable
    // blocks that still branch here, as many times as they do.
    let mut remaining_edges = ZoneVec::from_iter_in(
        (0..block_count).map(|block| ZoneVec::from_slice_in(&graph.successors(BlockId::from_index(block)), work)),
        work,
    );
    let mut keep = ZoneVec::new_in(work);
    for block in 0..block_count {
        if !reachable[block] {
            continue;
        }
        let predecessors = &graph.blocks[block].predecessors;
        keep.clear();
        keep.extend(predecessors.iter().map(|predecessor| {
            if !reachable[predecessor.index()] {
                return false;
            }
            let successors = &mut remaining_edges[predecessor.index()];
            let Some(index) = successors.iter().position(|successor| successor.index() == block) else {
                return false;
            };
            successors.swap_remove(index);
            true
        }));
        if keep.iter().all(|keep| *keep) {
            continue;
        }
        let mut index = 0;
        graph.blocks[block].predecessors.retain(|_| {
            let retained = keep[index];
            index += 1;
            retained
        });
        for phi in &graph.blocks[block].phis {
            let mut index = 0;
            graph.nodes[phi.index()].inputs.retain(|_| {
                let retained = keep[index];
                index += 1;
                retained
            });
        }
    }

    let order = ZoneVec::from_iter_in(
        (0..block_count)
            .filter(|block| reachable[*block])
            .map(BlockId::from_index),
        work,
    );
    if order.len() != block_count {
        graph.reorder_blocks(&order, work);
    }
    remove_trivial_phis(graph, work);
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
pub fn fold_branches_to_one_target(graph: &mut Graph, work: &Zone) -> bool {
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
        clean_up_control_flow(graph, work);
    }
    changed
}

/// A frame state for exits at the end of `predecessor`, resuming at the
/// start of the bytecode block `block` with the values flowing into it from
/// there, if `block` has a start frame state (see
/// `Block::start_frame_state`).
pub fn frame_state_entering<'z>(
    graph: &Graph<'z>,
    block: BlockId,
    predecessor: BlockId,
    work: &Zone,
) -> Option<FrameState<'z>> {
    let (template, values) = values_entering(graph, block, predecessor, work)?;
    Some(frame_state_with_values(graph, template, values))
}

/// The values of a frame state, with the frame state it is made from (see `values_entering()`).
pub type EnteringValues<'w> = (FrameStateId, &'w [(u32, NodeId)]);

/// The values of the frame state of `frame_state_entering()`, in `work`, with the start frame state of `block` that it
/// is made from.
pub fn values_entering<'w>(
    graph: &Graph,
    block: BlockId,
    predecessor: BlockId,
    work: &'w Zone,
) -> Option<EnteringValues<'w>> {
    let data = graph.block(block);
    let template = data.start_frame_state?;
    let entry_index = data
        .predecessors
        .iter()
        .position(|block| *block == predecessor)
        .expect("the predecessor enters the block");
    let phis = &data.phis;
    let values = &graph.frame_state(template).values;
    // NB: Values made in the block do not exist before it.
    if values
        .iter()
        .any(|(_, value)| !phis.contains(value) && data.body.contains(value))
    {
        return None;
    }
    let values = work.alloc_slice_from_iter(values.iter().map(|(slot, value)| {
        if phis.contains(value) {
            (*slot, graph.node(*value).inputs[entry_index])
        } else {
            (*slot, *value)
        }
    }));
    Some((template, values))
}

/// The frame state `template` with `values` in place of its values.
pub fn frame_state_with_values<'z>(
    graph: &Graph<'z>,
    template: FrameStateId,
    values: &[(u32, NodeId)],
) -> FrameState<'z> {
    let template = graph.frame_state(template);
    FrameState {
        executable: template.executable,
        pc: template.pc,
        mode: template.mode,
        values: graph.list(values),
        in_frame: template.in_frame.clone(),
        parent: template.parent,
        passed_argument_count: template.passed_argument_count,
        legacy_arguments: template.legacy_arguments.clone(),
    }
}

/// Moves the cold blocks after all other blocks, keeping the order of
/// each, so that the code of every other block comes first and the cold
/// code is out of the way.
pub fn move_cold_blocks_last(graph: &mut Graph, work: &Zone) {
    if !graph.blocks.iter().any(|block| block.is_cold) {
        return;
    }
    let blocks = || (0..graph.blocks.len()).map(BlockId::from_index);
    let hot = blocks().filter(|block| !graph.block(*block).is_cold);
    let cold = blocks().filter(|block| graph.block(*block).is_cold);
    let order = ZoneVec::from_iter_in(hot.chain(cold), work);
    graph.reorder_blocks(&order, work);
}

/// Replaces phis whose inputs are all one value (or the phi itself) by
/// that value, until none is left.
pub fn remove_trivial_phis(graph: &mut Graph, work: &Zone) {
    // NB: Nothing adds nodes here.
    let mut replacements = Replacements::new(graph, work);
    let mut removed = ZoneBitSet::new_in(graph.nodes.len(), work);
    loop {
        replacements.clear();
        removed.clear();
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
pub fn remove_unused_frame_states(graph: &mut Graph, work: &Zone) {
    // NB: No pass exits to the start of blocks anymore.
    for block in &mut graph.blocks {
        block.start_frame_state = None;
    }
    let count = graph.frame_states.len();
    let mut used = ZoneVec::from_elem_in(false, count, work);
    for block in &graph.blocks {
        for node in block.nodes() {
            let mut frame_state = graph.nodes[node.index()].frame_state;
            while let Some(id) = frame_state {
                if core::mem::replace(&mut used[id.index()], true) {
                    break;
                }
                frame_state = graph.frame_states[id.index()].parent;
            }
        }
    }
    if used.iter().all(|used| *used) {
        return;
    }
    let mut new_index = ZoneVec::from_elem_in(None, count, work);
    let mut next = 0;
    for (index, used) in used.iter().enumerate() {
        if *used {
            new_index[index] = Some(FrameStateId(next));
            next += 1;
        }
    }
    let remap = |id: FrameStateId| new_index[id.index()].expect("used frame states only refer to used ones");
    let mut index = 0;
    graph.frame_states.retain_mut(|frame_state| {
        let used = used[index];
        index += 1;
        if used {
            frame_state.parent = frame_state.parent.map(remap);
        }
        used
    });
    for block in &graph.blocks {
        for node in block.nodes() {
            let node = &mut graph.nodes[node.index()];
            node.frame_state = node.frame_state.map(remap);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn replacements_allocate_lazily_and_follow_updated_chains() {
        let mut graph = Graph::default();
        for _ in 0..4 {
            graph.add(Op::Constant(0), &[], Some(Repr::Tagged), 0);
        }
        let mut replacements = Replacements::new(&graph, crate::test_zone());
        assert_eq!(replacements.map.capacity(), 0);
        assert!(replacements.is_empty());
        assert_eq!(replacements.resolve(NodeId(0)), NodeId(0));
        replacements.replace(NodeId(0), NodeId(1));
        replacements.replace(NodeId(1), NodeId(2));
        assert_eq!(replacements.resolve(NodeId(0)), NodeId(2));
        replacements.replace(NodeId(1), NodeId(3));
        assert_eq!(replacements.resolve(NodeId(0)), NodeId(3));
        let added = graph.add(Op::Constant(0), &[], Some(Repr::Tagged), 0);
        replacements.replace(added, NodeId(0));
        assert_eq!(replacements.resolve(added), NodeId(3));
        assert!(!replacements.is_empty());
    }
}
