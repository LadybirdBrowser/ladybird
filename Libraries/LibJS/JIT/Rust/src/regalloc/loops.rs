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

use ak::Column;
use ak::ColumnFull;

use super::liveness::ValueLiveness;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::passes::dominators::Dominators;
use crate::tables::Lists;
use crate::tables::pop;
use crate::tables::release;
use crate::tables::reset;

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

/// The loops of a graph, in tables that serve one compile after the other.
pub(super) struct Loops {
    loops: Column<Loop>,
    /// The blocks of each loop but the cold ones: for each loop, `words_for_each_loop` words with one bit for each
    /// block of the graph.
    blocks: Column<u64>,
    words_for_each_loop: usize,
    /// The innermost loop around each block, as an index into `loops`, or `NO_LOOP`.
    innermost: Column<u32>,
    /// The block of each position.
    position_blocks: Column<u32>,
    /// For each loop, the values live into the header that no node of the loop uses, sorted.
    carried_through: Lists<NodeId>,
    /// The blocks that the back edges of a loop come from.
    back_edge_sources: Column<BlockId>,
    /// The blocks that the walk over a loop still has to visit.
    pending: Column<BlockId>,
    /// The loops, from the one with the most blocks to the one with the fewest.
    by_size: Column<u32>,
}

impl Loops {
    // NB: A compile whose graph needs more of these fails.
    const MAX_BLOCKS: usize = 1 << 24;
    const MAX_NODES: usize = 1 << 24;
    const MAX_ITEMS: usize = 1 << 26;

    /// Tables that take no memory before a compile uses them.
    pub fn new() -> Self {
        Self {
            loops: Column::new(Self::MAX_BLOCKS, c"JIT loops"),
            blocks: Column::new(Self::MAX_ITEMS, c"JIT loops"),
            words_for_each_loop: 0,
            innermost: Column::new(Self::MAX_BLOCKS, c"JIT loops"),
            position_blocks: Column::new(Self::MAX_NODES, c"JIT loops"),
            carried_through: Lists::new(Self::MAX_BLOCKS, Self::MAX_ITEMS, c"JIT loops"),
            back_edge_sources: Column::new(Self::MAX_ITEMS, c"JIT loops"),
            pending: Column::new(Self::MAX_ITEMS, c"JIT loops"),
            by_size: Column::new(Self::MAX_BLOCKS, c"JIT loops"),
        }
    }

    /// Drops the tables and gives their memory back to the system.
    pub fn release_memory(&mut self) {
        release(&mut self.loops);
        release(&mut self.blocks);
        release(&mut self.innermost);
        release(&mut self.position_blocks);
        self.carried_through.release_memory();
        release(&mut self.back_edge_sources);
        release(&mut self.pending);
        release(&mut self.by_size);
    }

    /// Finds the loops of `graph`. `dominators` are tables to compute its dominators in.
    pub fn compute(
        &mut self,
        graph: &Graph,
        liveness: &ValueLiveness,
        dominators: &mut Dominators,
    ) -> Result<(), ColumnFull> {
        let block_count = graph.blocks.len();
        self.position_blocks.clear();
        for (index, block) in graph.blocks.iter().enumerate() {
            let end = self.position_blocks.len() + block.nodes().count();
            self.position_blocks.try_resize(end, index as u32)?;
        }
        self.loops.clear();
        self.blocks.clear();
        self.words_for_each_loop = block_count.div_ceil(64);
        self.carried_through.begin()?;
        reset(&mut self.innermost, block_count, NO_LOOP)?;
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
            return Ok(());
        }

        dominators.compute(graph)?;
        for index in 0..block_count {
            let header = BlockId::from_index(index);
            // The sources of the back edges of the loop that the block heads, if it heads one: its predecessors that
            // it dominates.
            self.back_edge_sources.clear();
            for predecessor in &graph.block(header).predecessors {
                if dominators.dominates(header, *predecessor) && is_hot(*predecessor) {
                    self.back_edge_sources.try_push(*predecessor)?;
                }
            }
            if self.back_edge_sources.is_empty() {
                continue;
            }
            // The blocks of the loop: the header and every reachable block that reaches a source of a back edge
            // without going through the header.
            let row = self.blocks.len();
            self.blocks.try_resize(row + self.words_for_each_loop, 0)?;
            let blocks = &mut self.blocks[row..];
            let contains = |blocks: &[u64], block: usize| blocks[block / 64] & (1 << (block % 64)) != 0;
            blocks[index / 64] |= 1 << (index % 64);
            self.pending.clear();
            self.pending.try_extend_from_slice(&self.back_edge_sources)?;
            while let Some(block) = pop(&mut self.pending) {
                if contains(blocks, block.index()) || !dominators.is_reachable(block) {
                    continue;
                }
                blocks[block.index() / 64] |= 1 << (block.index() % 64);
                self.pending.try_extend_from_slice(&graph.block(block).predecessors)?;
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
                    self.carried_through.try_push(*value)?;
                }
            }
            self.carried_through.end_list()?;
            self.loops.try_push(loop_)?;
        }

        // NB: A loop inside another one has fewer blocks, so the smallest loop around a block is the innermost one.
        //     Of two loops with as many blocks, the later one is taken.
        self.by_size.clear();
        for index in 0..self.loops.len() as u32 {
            self.by_size.try_push(index)?;
        }
        let loops = &self.loops;
        self.by_size
            .sort_unstable_by_key(|index| (std::cmp::Reverse(loops[*index as usize].block_count), *index));
        for index in self.by_size.iter() {
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
    pub fn first_use_in(&self, loop_: &Loop, uses: &[u32], range: std::ops::Range<usize>) -> Option<u32> {
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
        range: std::ops::Range<usize>,
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
