/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The loops of a graph as the register allocator sees them: which blocks
//! each one runs, and which values it only carries through.
//!
//! Linear order does not keep a loop together (its exits may come before its
//! back edges), so loops are sets of blocks. Their cold blocks are left out:
//! those take values from where they are, and cost the loop nothing.

use ak::Zone;
use ak::ZoneVec;

use super::check_items;
use super::liveness::ValueLiveness;
use crate::CompileFailure;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::passes::dominators::Dominators;
use crate::tables::Lists;

#[derive(Clone, Copy)]
pub(super) struct Loop {
    pub header: BlockId,
    /// The first and last positions of its blocks.
    pub start: u32,
    end: u32,
    /// Which loop of the graph it is, in the order of the headers.
    index: u32,
    /// How many blocks it has, without the cold ones.
    block_count: u32,
}

/// In place of the loop around a block that is in no loop.
const NO_LOOP: u32 = u32::MAX;

/// The loops of a graph, in tables in the work zone of a compile.
pub(super) struct Loops<'w> {
    loops: ZoneVec<'w, Loop>,
    /// The blocks of each loop but the cold ones: for each loop, `words_for_each_loop` words with one bit for each
    /// block of the graph.
    blocks: ZoneVec<'w, u64>,
    words_for_each_loop: usize,
    /// The innermost loop around each block, as an index into `loops`, or `NO_LOOP`.
    innermost: ZoneVec<'w, u32>,
    /// The block of each position.
    position_blocks: ZoneVec<'w, u32>,
    /// For each loop, the values live into the header that no node of the loop uses, sorted.
    carried_through: Lists<'w, NodeId>,
    /// The blocks that the back edges of a loop come from.
    back_edge_sources: ZoneVec<'w, BlockId>,
    /// The blocks that the walk over a loop still has to visit.
    pending: ZoneVec<'w, BlockId>,
    /// The loops, from the one with the most blocks to the one with the fewest.
    by_size: ZoneVec<'w, u32>,
}

impl<'w> Loops<'w> {
    /// Finds the loops of `graph`.
    pub fn compute(graph: &Graph, liveness: &ValueLiveness, work: &'w Zone) -> Result<Self, CompileFailure> {
        let block_count = graph.blocks.len();
        let mut loops = Self {
            loops: ZoneVec::new_in(work),
            blocks: ZoneVec::new_in(work),
            words_for_each_loop: block_count.div_ceil(64),
            innermost: ZoneVec::from_elem_in(NO_LOOP, block_count, work),
            position_blocks: ZoneVec::with_capacity_in(graph.nodes.len(), work),
            carried_through: Lists::new_in(work),
            back_edge_sources: ZoneVec::new_in(work),
            pending: ZoneVec::new_in(work),
            by_size: ZoneVec::new_in(work),
        };
        for (index, block) in graph.blocks.iter().enumerate() {
            let end = loops.position_blocks.len() + block.nodes().count();
            loops.position_blocks.resize(end, index as u32);
        }
        // NB: Cold blocks are left out of loops, so only the edges from hot blocks make loops. Cold blocks come after
        //     the hot ones, so their edges back into hot code would otherwise look like loops wherever they rejoin it.
        let is_hot = |block: BlockId| !graph.blocks[block.index()].is_cold;
        let has_back_edges = graph.blocks.iter().enumerate().any(|(index, block)| {
            block
                .predecessors
                .iter()
                .any(|predecessor| predecessor.index() >= index && is_hot(*predecessor))
        });
        if !has_back_edges {
            return Ok(loops);
        }
        let dominators = Dominators::of(graph, work);
        loops.find(graph, liveness, &dominators, work)?;
        Ok(loops)
    }

    /// Finds the loops of `graph`, which has back edges.
    fn find(
        &mut self,
        graph: &Graph,
        liveness: &ValueLiveness,
        dominators: &Dominators,
        work: &'w Zone,
    ) -> Result<(), CompileFailure> {
        let block_count = graph.blocks.len();
        let is_hot = |block: BlockId| !graph.blocks[block.index()].is_cold;
        // The sources of the back edges of the loop that a block heads, if it heads one: its predecessors that it
        // dominates.
        let is_back_edge =
            |header: BlockId, predecessor: BlockId| dominators.dominates(header, predecessor) && is_hot(predecessor);
        let mut loop_count = 0;
        let mut header_live_values = 0;
        let mut edges = 0;
        for (index, block) in graph.blocks.iter().enumerate() {
            let header = BlockId::from_index(index);
            edges += block.predecessors.len();
            if block
                .predecessors
                .iter()
                .any(|predecessor| is_back_edge(header, *predecessor))
            {
                loop_count += 1;
                header_live_values += liveness.live_in.get(index).len();
            }
        }
        check_items(loop_count * self.words_for_each_loop)?;
        self.loops = ZoneVec::with_capacity_in(loop_count, work);
        self.blocks = ZoneVec::with_capacity_in(loop_count * self.words_for_each_loop, work);
        self.carried_through = Lists::with_capacity_in(loop_count, header_live_values, work);
        self.carried_through.begin();
        self.pending = ZoneVec::with_capacity_in(edges, work);
        self.by_size = ZoneVec::with_capacity_in(loop_count, work);
        for index in 0..block_count {
            let header = BlockId::from_index(index);
            self.back_edge_sources.clear();
            for predecessor in &graph.block(header).predecessors {
                if is_back_edge(header, *predecessor) {
                    self.back_edge_sources.push(*predecessor);
                }
            }
            if self.back_edge_sources.is_empty() {
                continue;
            }
            // The blocks of the loop: the header and every reachable block that reaches a source of a back edge
            // without going through the header.
            let row = self.blocks.len();
            self.blocks.resize(row + self.words_for_each_loop, 0);
            let blocks = &mut self.blocks[row..];
            let contains = |blocks: &[u64], block: usize| blocks[block / 64] & (1 << (block % 64)) != 0;
            blocks[index / 64] |= 1 << (index % 64);
            self.pending.clear();
            self.pending.extend_from_slice(&self.back_edge_sources);
            while let Some(block) = self.pending.pop() {
                if contains(blocks, block.index()) || !dominators.is_reachable(block) {
                    continue;
                }
                blocks[block.index() / 64] |= 1 << (block.index() % 64);
                self.pending.extend_from_slice(&graph.block(block).predecessors);
            }
            let position_of = |node: NodeId| liveness.positions[node.index()];
            let start = position_of(graph.block_nodes(header).next().expect("blocks have a control node"));
            let mut end = None;
            let mut count = 0;
            for (word_index, word) in blocks.iter_mut().enumerate() {
                let mut bits = *word;
                while bits != 0 {
                    let bit = bits.trailing_zeros();
                    bits &= bits - 1;
                    let block = word_index * 64 + bit as usize;
                    if graph.blocks[block].is_cold {
                        *word &= !(1 << bit);
                        continue;
                    }
                    count += 1;
                    end = end.max(Some(position_of(graph.control_id(BlockId::from_index(block)))));
                }
            }
            let loop_ = Loop {
                header,
                start,
                end: end.unwrap_or(start),
                index: self.loops.len() as u32,
                block_count: count,
            };
            // NB: The values live into a block are in the order of their numbers, so this list is in that order too.
            for value in liveness.live_in.get(index) {
                let uses = liveness.use_positions(*value);
                if loop_
                    .first_use_in(blocks, uses, &self.position_blocks, 0..uses.len())
                    .is_none()
                {
                    self.carried_through.push(*value);
                }
            }
            self.carried_through.end_list();
            self.loops.push(loop_);
        }

        // NB: A loop inside another one has fewer blocks, so the smallest loop around a block is the innermost one.
        //     Of two loops with as many blocks, the later one is taken.
        for index in 0..self.loops.len() as u32 {
            self.by_size.push(index);
        }
        let loops = &self.loops;
        self.by_size
            .sort_unstable_by_key(|index| (core::cmp::Reverse(loops[*index as usize].block_count), *index));
        for index in &self.by_size {
            let row = *index as usize * self.words_for_each_loop;
            let blocks = &self.blocks[row..row + self.words_for_each_loop];
            for (word_index, word) in blocks.iter().enumerate() {
                let mut word = *word;
                while word != 0 {
                    let block = word_index * 64 + word.trailing_zeros() as usize;
                    self.innermost[block] = *index;
                    word &= word - 1;
                }
            }
        }
        Ok(())
    }

    /// The bits for the blocks of `loop_`.
    fn blocks_of(&self, loop_: &Loop) -> &[u64] {
        let start = loop_.index as usize * self.words_for_each_loop;
        &self.blocks[start..start + self.words_for_each_loop]
    }

    fn loop_around(&self, block: usize) -> Option<&Loop> {
        let index = self.innermost[block];
        (index != NO_LOOP).then(|| &self.loops[index as usize])
    }

    /// The innermost loop around the node at `position`.
    pub fn innermost_at(&self, position: u32) -> Option<&Loop> {
        let block = *self.position_blocks.get(position as usize)?;
        self.loop_around(block as usize)
    }

    /// The loop that `header` heads, if it heads one.
    pub fn headed_by(&self, header: BlockId) -> Option<&Loop> {
        self.loop_around(header.index()).filter(|loop_| loop_.header == header)
    }

    /// The first of the use positions `uses[range]` (in order) that is in `loop_`.
    pub fn first_use_in(&self, loop_: &Loop, uses: &[u32], range: core::ops::Range<usize>) -> Option<u32> {
        loop_.first_use_in(self.blocks_of(loop_), uses, &self.position_blocks, range)
    }

    /// Whether no node of `loop_` uses `value`, which is live into its header.
    pub fn carries_through(&self, loop_: &Loop, value: NodeId) -> bool {
        self.carried_through
            .get(loop_.index as usize)
            .binary_search(&value)
            .is_ok()
    }
}

impl Loop {
    /// The first of the use positions `uses[range]` (in order) that is in the loop, which has the blocks with a bit
    /// in `blocks`.
    fn first_use_in(
        &self,
        blocks: &[u64],
        uses: &[u32],
        position_blocks: &[u32],
        range: core::ops::Range<usize>,
    ) -> Option<u32> {
        let uses = &uses[range];
        let from = uses.partition_point(|position| *position < self.start);
        let to = uses.partition_point(|position| *position <= self.end);
        uses[from..to.max(from)].iter().copied().find(|position| {
            let block = position_blocks[*position as usize] as usize;
            blocks[block / 64] & (1 << (block % 64)) != 0
        })
    }
}
