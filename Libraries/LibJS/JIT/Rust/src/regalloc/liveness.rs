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

use ak::Zone;
use ak::ZoneVec;

use super::Size;
use super::check_items;
use crate::CompileFailure;
use crate::codegen::lowering_info::Clobbers as LoweringClobbers;
use crate::codegen::lowering_info::lowering_info;
use crate::ir::BlockId;
use crate::ir::FrameStateId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::tables::Lists;
use crate::tables::Run;

/// A value that nobody uses.
const DEAD_OUTPUT: u8 = 1 << 0;
/// A value that must have a stack slot that holds it from its definition on.
const NEEDS_SPILL: u8 = 1 << 1;

/// In place of the frame state uses of a frame state: they are not listed yet.
const NOT_LISTED: Run = Run::NOT_WRITTEN;

/// The liveness of the values of a graph, in tables in the work zone of a compile.
///
/// No table here is a set that grows. A graph in SSA form makes that possible: a value is live into exactly the blocks
/// that a walk up from its uses passes before it reaches the block of its definition, so one walk for each value
/// gives all the blocks at one time. The values are walked in the order of their indices, so the list of the values
/// that are live into a block is sorted as it is written.
pub(super) struct ValueLiveness<'w> {
    /// The values live into each block and out of each block, sorted.
    pub live_in: Lists<'w, NodeId>,
    live_out: Lists<'w, NodeId>,
    /// For each node, the values whose last use (on this path) it is, as a part of `dying`.
    deaths: ZoneVec<'w, Run>,
    dying: ZoneVec<'w, NodeId>,
    /// For each value, `DEAD_OUTPUT` and `NEEDS_SPILL`.
    flags: ZoneVec<'w, u8>,
    /// The position of each node in linear order.
    pub positions: ZoneVec<'w, u32>,
    /// For each value, the positions of the nodes that use it as an input, in order, as a part of `uses`. A phi input
    /// is used at the control node of its predecessor, where the moves into the phis are.
    use_runs: ZoneVec<'w, Run>,
    uses: ZoneVec<'w, u32>,
    /// The blocks each block continues at.
    pub successors: Lists<'w, BlockId>,

    // NB: Only fill() reads what is below.
    /// The block of each node.
    blocks: ZoneVec<'w, u32>,
    /// The uses of values as inputs, as the value in the high half and the position in the low half.
    use_pairs: ZoneVec<'w, u64>,
    /// Where the walks up start: the value in the high half, then the block, then whether the value is only known to
    /// flow out of the block (to a phi) in the lowest bit.
    seeds: ZoneVec<'w, u64>,
    /// For each block, the value (plus one) that was last found live into it and out of it.
    in_marks: ZoneVec<'w, u32>,
    out_marks: ZoneVec<'w, u32>,
    /// The blocks a walk up has still to pass.
    pending: ZoneVec<'w, u32>,
    /// What the walks found: the block in the high half and the value in the low half, in the order of the values.
    in_pairs: ZoneVec<'w, u64>,
    out_pairs: ZoneVec<'w, u64>,
    /// For each frame state, the allocated values that a node uses through it, as a part of `frame_uses`.
    frame_runs: ZoneVec<'w, Run>,
    frame_uses: ZoneVec<'w, NodeId>,
    /// For each value, the block (plus one) whose backwards walk has it live at the place it is at.
    live: ZoneVec<'w, u32>,
    /// The values that were live at some place of the block that is walked.
    touched: ZoneVec<'w, NodeId>,
    /// The values the node that is walked uses.
    used: ZoneVec<'w, NodeId>,
    /// For each value, the node (plus one) that was last noted as its death.
    last_death: ZoneVec<'w, u32>,
    live_since: ZoneVec<'w, Option<(BlockId, Clobbers)>>,
}

/// Whether a node is a value the register allocator tracks.
pub(super) fn is_allocated_value(graph: &Graph, node: NodeId) -> bool {
    let node = graph.node(node);
    node.repr.is_some() && !matches!(node.op, Op::Constant(_) | Op::VirtualObject { .. })
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
pub(super) fn phi_inputs_from<'g>(
    graph: &'g Graph<'g>,
    block: BlockId,
    successor: BlockId,
) -> impl Iterator<Item = (NodeId, NodeId)> + 'g {
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

fn pair(high: u32, low: u32) -> u64 {
    u64::from(high) << 32 | u64::from(low)
}

fn high(pair: u64) -> u32 {
    (pair >> 32) as u32
}

fn low(pair: u64) -> u32 {
    pair as u32
}

impl<'w> ValueLiveness<'w> {
    /// The liveness of the values of `graph`, which has `size`. A compile fails if the lists of the liveness grow
    /// past their limit (see `check_items()`).
    pub fn compute(graph: &Graph, size: &Size, work: &'w Zone) -> Result<Self, CompileFailure> {
        let mut liveness = Self {
            live_in: Lists::new_in(work),
            live_out: Lists::new_in(work),
            deaths: ZoneVec::from_elem_in(Run::default(), size.nodes, work),
            // NB: Most values die once for each use.
            dying: ZoneVec::with_capacity_in(size.inputs, work),
            flags: ZoneVec::from_elem_in(0, size.nodes, work),
            positions: ZoneVec::from_elem_in(0, size.nodes, work),
            use_runs: ZoneVec::from_elem_in(Run::default(), size.nodes, work),
            uses: ZoneVec::with_capacity_in(size.inputs, work),
            successors: Lists::with_capacity_in(size.blocks, size.edges, work),
            blocks: ZoneVec::from_elem_in(0, size.nodes, work),
            use_pairs: ZoneVec::with_capacity_in(size.inputs, work),
            seeds: ZoneVec::with_capacity_in(size.inputs, work),
            in_marks: ZoneVec::from_elem_in(0, size.blocks, work),
            out_marks: ZoneVec::from_elem_in(0, size.blocks, work),
            // NB: A walk up passes each block one time.
            pending: ZoneVec::with_capacity_in(size.blocks, work),
            in_pairs: ZoneVec::new_in(work),
            out_pairs: ZoneVec::new_in(work),
            frame_runs: ZoneVec::from_elem_in(NOT_LISTED, size.frame_states, work),
            frame_uses: ZoneVec::new_in(work),
            live: ZoneVec::from_elem_in(0, size.nodes, work),
            touched: ZoneVec::new_in(work),
            used: ZoneVec::new_in(work),
            last_death: ZoneVec::from_elem_in(0, size.nodes, work),
            live_since: ZoneVec::from_elem_in(None, size.nodes, work),
        };
        liveness.fill(graph, work)?;
        Ok(liveness)
    }

    pub fn is_live_in(&self, block: BlockId, value: NodeId) -> bool {
        self.live_in.get(block.index()).binary_search(&value).is_ok()
    }

    pub fn is_live_out(&self, block: BlockId, value: NodeId) -> bool {
        self.live_out.get(block.index()).binary_search(&value).is_ok()
    }

    /// The values whose last use (on this path) `node` is.
    pub fn dies_at(&self, node: NodeId) -> &[NodeId] {
        &self.dying[self.deaths[node.index()].range()]
    }

    /// Whether nobody uses `value`.
    pub fn is_dead_output(&self, value: NodeId) -> bool {
        self.flags[value.index()] & DEAD_OUTPUT != 0
    }

    /// Whether `value` must have a stack slot that holds it from its definition on: a value that is live across a
    /// call, or live into a loop header (or a merge without a state predecessor).
    pub fn needs_spill(&self, value: NodeId) -> bool {
        self.flags[value.index()] & NEEDS_SPILL != 0
    }

    /// The positions of the nodes that use `value` as an input, in order.
    pub fn use_positions(&self, value: NodeId) -> &[u32] {
        &self.uses[self.use_runs[value.index()].range()]
    }

    /// Lists the allocated values of the frame state `frame`, if they are not listed yet.
    fn list_frame_uses(&mut self, graph: &Graph, frame: FrameStateId, work: &Zone) -> Result<(), CompileFailure> {
        if self.frame_runs[frame.index()] != NOT_LISTED {
            return Ok(());
        }
        let start = self.frame_uses.len();
        for (_, value) in graph.frame_state_values(frame, work) {
            if is_allocated_value(graph, value) {
                self.frame_uses.push(value);
            }
        }
        self.frame_runs[frame.index()] = Run::new(start, self.frame_uses.len());
        // NB: How many values the frame states of a graph have in all is known only when they are listed.
        check_items(self.frame_uses.len())
    }

    /// Notes that `value` is used in `block`, at a node or (`flows_out`) by a phi of a block that `block` continues
    /// at. Fails the compile if the seeds would be past their limit (see `check_items()`).
    fn add_seed(&mut self, value: NodeId, block: usize, flows_out: bool) -> Result<(), CompileFailure> {
        // NB: A use in the block of the definition makes the value live into no block.
        if !flows_out && self.blocks[value.index()] as usize == block {
            return Ok(());
        }
        // NB: The frame states of the nodes give a block seeds that its inputs do not tell, so the seeds of one
        //     block alone can be past the limit.
        check_items(self.seeds.len() + 1)?;
        self.seeds
            .push(pair(value.0, (block as u32) << 1 | u32::from(flows_out)));
        Ok(())
    }

    /// Fills the tables, which `compute()` made for `graph`.
    fn fill(&mut self, graph: &Graph, work: &Zone) -> Result<(), CompileFailure> {
        let block_count = graph.blocks.len();

        self.successors.begin();
        let mut position = 0;
        for (index, block) in graph.blocks.iter().enumerate() {
            for node in block.nodes() {
                self.positions[node.index()] = position;
                self.blocks[node.index()] = index as u32;
                position += 1;
            }
            graph
                .control(BlockId::from_index(index))
                .op
                .for_each_successor(|successor| self.successors.push(successor));
            self.successors.end_list();
        }

        // The uses of each value: where it is an input, and the blocks it is used in that are not its own.
        for (index, block) in graph.blocks.iter().enumerate() {
            for node_id in block.body.iter().chain(&block.control) {
                let node = graph.node(*node_id);
                let position = self.positions[node_id.index()];
                for input in &node.inputs {
                    if is_allocated_value(graph, *input) {
                        self.use_pairs.push(pair(input.0, position));
                        self.add_seed(*input, index, false)?;
                    }
                }
                if let Some(frame) = node.frame_state {
                    self.list_frame_uses(graph, frame, work)?;
                    for use_index in self.frame_runs[frame.index()].range() {
                        self.add_seed(self.frame_uses[use_index], index, false)?;
                    }
                }
            }
            let block_id = BlockId::from_index(index);
            let end = self.positions[graph.control_id(block_id).index()];
            for successor_index in 0..self.successors.get(index).len() {
                let successor = self.successors.get(index)[successor_index];
                for (_, input) in phi_inputs_from(graph, block_id, successor) {
                    if is_allocated_value(graph, input) {
                        self.use_pairs.push(pair(input.0, end));
                        self.add_seed(input, index, true)?;
                    }
                }
            }
        }

        self.use_pairs.sort_unstable();
        let mut index = 0;
        while index < self.use_pairs.len() {
            let value = high(self.use_pairs[index]);
            let start = index;
            while index < self.use_pairs.len() && high(self.use_pairs[index]) == value {
                self.uses.push(low(self.use_pairs[index]));
                index += 1;
            }
            self.use_runs[value as usize] = Run::new(start, index);
        }

        // Walk up from the uses of each value, as far as the block of its definition.
        self.seeds.sort_unstable();
        let mut index = 0;
        while index < self.seeds.len() {
            let value = high(self.seeds[index]);
            let mark = value + 1;
            let definition = self.blocks[value as usize];
            self.pending.clear();
            while index < self.seeds.len() && high(self.seeds[index]) == value {
                let seed = low(self.seeds[index]);
                let (block, flows_out) = (seed >> 1, seed & 1 != 0);
                index += 1;
                if flows_out {
                    if self.out_marks[block as usize] != mark {
                        self.out_marks[block as usize] = mark;
                        self.out_pairs.push(pair(block, value));
                    }
                    if block == definition {
                        continue;
                    }
                }
                if self.in_marks[block as usize] != mark {
                    self.in_marks[block as usize] = mark;
                    self.in_pairs.push(pair(block, value));
                    self.pending.push(block);
                }
            }
            while let Some(block) = self.pending.pop() {
                for predecessor in &graph.blocks[block as usize].predecessors {
                    let predecessor = predecessor.0;
                    if self.out_marks[predecessor as usize] != mark {
                        self.out_marks[predecessor as usize] = mark;
                        self.out_pairs.push(pair(predecessor, value));
                    }
                    if predecessor != definition && self.in_marks[predecessor as usize] != mark {
                        self.in_marks[predecessor as usize] = mark;
                        self.in_pairs.push(pair(predecessor, value));
                        self.pending.push(predecessor);
                    }
                }
            }
            // NB: How many blocks a value is live into is known only when its walk is done.
            check_items(self.in_pairs.len())?;
            check_items(self.out_pairs.len())?;
        }
        // NB: The pairs are in the order of the values, so the list of each block comes out sorted.
        fn lists_of_blocks(lists: &mut Lists<NodeId>, pairs: &[u64], block_count: usize) {
            lists.begin_counts(block_count);
            for pair in pairs {
                lists.count(high(*pair) as usize);
            }
            lists.end_counts(NodeId(0));
            for pair in pairs {
                lists.fill(high(*pair) as usize, NodeId(low(*pair)));
            }
            lists.end_fill();
        }
        lists_of_blocks(&mut self.live_in, &self.in_pairs, block_count);
        lists_of_blocks(&mut self.live_out, &self.out_pairs, block_count);

        // OPTIMIZATION: Rather than marking every value live across each call or float clobbering slow path there,
        //               which takes time quadratic in the size of code that keeps many values live past many of them
        //               (like a large array literal of arrays), the walk counts those nodes, notes the counts where
        //               each value becomes live, and compares them where the value is defined or the block starts.
        let mut clobbers = Clobbers::default();
        for (index, block) in graph.blocks.iter().enumerate() {
            let block_id = BlockId::from_index(index);
            let block_start = clobbers;
            let since = |live_since: &[Option<(BlockId, Clobbers)>], value: usize| match live_since[value] {
                Some((since_block, since)) if since_block == block_id => since,
                _ => block_start,
            };
            let check_spill = |flags: &mut [u8], since: Clobbers, clobbers: Clobbers, value: usize| {
                let is_float = graph.nodes[value].repr == Some(crate::code::Repr::Float64);
                if clobbers.calls != since.calls || (is_float && clobbers.float_clobbers != since.float_clobbers) {
                    flags[value] |= NEEDS_SPILL;
                }
            };
            // Walk the block backwards from the values live at its end. A value is live where the walk is if `live`
            // has the mark of the block for it.
            let mark = index as u32 + 1;
            self.touched.clear();
            for value in self.live_out.get(index) {
                self.live[value.index()] = mark;
                self.touched.push(*value);
            }
            for node_id in block.body.iter().chain(&block.control).rev() {
                let node = graph.node(*node_id);
                self.used.clear();
                for input in &node.inputs {
                    if is_allocated_value(graph, *input) {
                        self.used.push(*input);
                    }
                }
                let frame_uses = match node.frame_state {
                    Some(frame) => self.frame_runs[frame.index()].range(),
                    None => 0..0,
                };
                self.used.extend_from_slice(&self.frame_uses[frame_uses.clone()]);

                if self.live[node_id.index()] == mark {
                    check_spill(
                        &mut self.flags,
                        since(&self.live_since, node_id.index()),
                        clobbers,
                        node_id.index(),
                    );
                } else if is_allocated_value(graph, *node_id) {
                    self.flags[node_id.index()] |= DEAD_OUTPUT;
                }
                let (is_call, clobbers_floats) = match lowering_info(node).clobbers {
                    LoweringClobbers::CallerSaved => (true, false),
                    // NB: Their slow paths clobber the floating point registers.
                    LoweringClobbers::Floats => (false, true),
                    LoweringClobbers::Nothing => (false, false),
                };
                if is_call || clobbers_floats {
                    clobbers.float_clobbers += 1;
                    clobbers.calls += u32::from(is_call);
                    for value in &self.frame_uses[frame_uses] {
                        let is_float = graph.nodes[value.index()].repr == Some(crate::code::Repr::Float64);
                        if value != node_id && (is_call || is_float) {
                            self.flags[value.index()] |= NEEDS_SPILL;
                        }
                    }
                }
                let deaths = self.dying.len();
                for value in &self.used {
                    if self.live[value.index()] != mark && self.last_death[value.index()] != node_id.0 + 1 {
                        self.last_death[value.index()] = node_id.0 + 1;
                        self.dying.push(*value);
                        self.live_since[value.index()] = Some((block_id, clobbers));
                    }
                }
                self.deaths[node_id.index()] = Run::new(deaths, self.dying.len());

                self.live[node_id.index()] = 0;
                for value in &self.used {
                    if self.live[value.index()] != mark {
                        self.live[value.index()] = mark;
                        self.touched.push(*value);
                    }
                }
            }
            for phi in &block.phis {
                self.live[phi.index()] = 0;
            }
            // NB: `touched` may have a value more than one time, which does no harm here.
            for value in &self.touched {
                if self.live[value.index()] == mark {
                    check_spill(
                        &mut self.flags,
                        since(&self.live_since, value.index()),
                        clobbers,
                        value.index(),
                    );
                }
            }
            // NB: The phis of the block are live from its start if anything uses them.
            for phi in &block.phis {
                let is_live = self.live_since[phi.index()].is_some_and(|(since_block, _)| since_block == block_id)
                    || self.live_out.get(index).binary_search(phi).is_ok();
                if is_live {
                    check_spill(
                        &mut self.flags,
                        since(&self.live_since, phi.index()),
                        clobbers,
                        phi.index(),
                    );
                }
            }
            // NB: Most merges start with the register state of one of their
            //     predecessors (see `state_predecessor()`).
            let predecessors = &block.predecessors;
            if (predecessors.len() > 1 || predecessors.first().is_some_and(|predecessor| *predecessor >= block_id))
                && state_predecessor(graph, block_id).is_none()
            {
                for value in self.live_in.get(index) {
                    self.flags[value.index()] |= NEEDS_SPILL;
                }
            }
            // NB: The frame states of the nodes give a block deaths that its inputs do not tell.
            check_items(self.dying.len())?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::code::{Repr, ResumeMode};
    use crate::ir::{Block, FrameState};

    #[test]
    fn shared_frames_and_duplicate_uses_have_one_last_use_per_value() {
        let mut graph = Graph::default();
        graph.add_block(Block::default());
        let value = graph.add(Op::LoadSlot { slot: 0 }, &[], Some(Repr::Tagged), 0);
        graph.blocks[0].body.push(value);
        let frame = graph.add_frame_state(FrameState {
            executable: 0,
            pc: 0,
            mode: ResumeMode::ResumeAt,
            values: ak::ZoneVec::from_slice_in(&[(0, value), (1, value)], crate::test_zone()),
            in_frame: ak::ZoneVec::new_in(crate::test_zone()),
            parent: None,
            passed_argument_count: None,
            legacy_arguments: 0..0,
        });
        let mut checks = Vec::new();
        for _ in 0..2 {
            let check = graph.add(Op::CheckObject, &[value], Some(Repr::Tagged), 0);
            graph.nodes[check.index()].frame_state = Some(frame);
            graph.blocks[0].body.push(check);
            checks.push(check);
        }
        let constant = graph.add(Op::Constant(0), &[], Some(Repr::Tagged), 0);
        let returned = graph.add(Op::Return, &[constant], None, 0);
        graph.blocks[0].control = Some(returned);
        let size = Size::of(&graph).unwrap();
        let liveness = ValueLiveness::compute(&graph, &size, crate::test_zone()).unwrap();
        assert!(liveness.dies_at(checks[0]).is_empty());
        assert_eq!(liveness.dies_at(checks[1]), [value]);
        assert!(!liveness.is_dead_output(value));
        assert!(liveness.live_in.get(0).is_empty());
    }

    #[test]
    fn many_blocks_with_short_live_ranges() {
        let mut graph = Graph::default();
        let block_count = 8192;
        let mut values = Vec::new();
        for index in 0..block_count {
            let mut block = Block::default();
            let value = if index == 0 {
                graph.add(Op::LoadSlot { slot: 0 }, &[], Some(Repr::Tagged), 0)
            } else {
                block.predecessors.push(BlockId::from_index(index - 1));
                graph.add(Op::CheckObject, &[values[index - 1]], Some(Repr::Tagged), 0)
            };
            values.push(value);
            block.body.push(value);
            block.control = Some(if index + 1 == block_count {
                graph.add(Op::Return, &[value], None, 0)
            } else {
                graph.add(
                    Op::Jump {
                        target: BlockId::from_index(index + 1),
                    },
                    &[],
                    None,
                    0,
                )
            });
            graph.add_block(block);
        }
        let size = Size::of(&graph).unwrap();
        // NB: The second time, the zone holds what the first time left in it.
        for _ in 0..2 {
            let liveness = ValueLiveness::compute(&graph, &size, crate::test_zone()).unwrap();
            assert!(liveness.live_in.get(0).is_empty());
            assert!(liveness.live_out.get(block_count - 1).is_empty());
            for index in 1..block_count {
                assert_eq!(liveness.live_in.get(index), [values[index - 1]]);
                assert_eq!(liveness.live_out.get(index - 1), [values[index - 1]]);
                assert!(liveness.is_live_in(BlockId::from_index(index), values[index - 1]));
                assert!(!liveness.is_live_out(BlockId::from_index(index), values[index - 1]));
                assert_eq!(liveness.dies_at(values[index]), [values[index - 1]]);
                assert!(!liveness.is_dead_output(values[index - 1]));
            }
            assert_eq!(
                liveness.dies_at(graph.blocks[block_count - 1].control.unwrap()),
                [values[block_count - 1]]
            );
        }
    }
}
