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

use super::liveness::ValueLiveness;
use crate::bitset::BitSet;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::passes::dominators::Dominators;
use crate::sparse_bitset::SparseBitSet;

pub(super) struct Loop {
    pub header: BlockId,
    /// The blocks of the loop but the cold ones.
    blocks: BitSet,
    /// The first and last positions of its blocks.
    pub start: u32,
    end: u32,
    /// The values live into the header that no node of the loop uses.
    carried_through: SparseBitSet,
}

pub(super) struct Loops {
    loops: Vec<Loop>,
    /// The innermost loop around each block, as an index into `loops`.
    innermost: Vec<Option<usize>>,
    /// The block of each position.
    position_blocks: Vec<u32>,
}

impl Loops {
    pub fn compute(graph: &Graph, liveness: &ValueLiveness) -> Self {
        let mut position_blocks = Vec::new();
        for (index, block) in graph.blocks.iter().enumerate() {
            position_blocks.extend(std::iter::repeat_n(index as u32, block.nodes().count()));
        }
        let mut loops = Vec::new();
        let mut innermost = vec![None; graph.blocks.len()];
        // NB: Cold blocks are left out of loops, so only the edges from hot blocks make loops. Cold blocks come after
        //     the hot ones, so their edges back into hot code would otherwise look like loops wherever they rejoin it.
        let is_hot = |block: &BlockId| !graph.blocks[block.index()].is_cold;
        let has_back_edges = graph.blocks.iter().enumerate().any(|(index, block)| {
            block
                .predecessors
                .iter()
                .any(|predecessor| predecessor.index() >= index && is_hot(predecessor))
        });
        if !has_back_edges {
            return Self {
                loops,
                innermost,
                position_blocks,
            };
        }

        let dominators = Dominators::compute(graph);
        for index in 0..graph.blocks.len() {
            let header = BlockId::from_index(index);
            let mut back_edge_sources = dominators.back_edge_sources(graph, header);
            back_edge_sources.retain(is_hot);
            if back_edge_sources.is_empty() {
                continue;
            }
            let mut blocks = dominators.loop_blocks(graph, header, &back_edge_sources);
            for block in blocks.iter().collect::<Vec<_>>() {
                if graph.blocks[block].is_cold {
                    blocks.remove(block);
                }
            }
            let position_of = |node: NodeId| liveness.positions[node.index()];
            let start = position_of(graph.block_nodes(header).next().expect("blocks have a control node"));
            let end = blocks
                .iter()
                .map(|block| position_of(graph.control_id(BlockId::from_index(block))))
                .max()
                .unwrap_or(start);
            let mut loop_ = Loop {
                header,
                blocks,
                start,
                end,
                carried_through: SparseBitSet::new(graph.nodes.len()),
            };
            for value in liveness.live_in[index].iter() {
                let uses = &liveness.use_positions[value];
                if loop_.first_use_in(uses, &position_blocks, 0..uses.len()).is_none() {
                    loop_.carried_through.insert(value);
                }
            }
            loops.push(loop_);
        }

        // NB: A loop inside another one has fewer blocks, so the smallest loop around a block is the innermost one.
        let mut order = (0..loops.len()).collect::<Vec<_>>();
        order.sort_by_key(|index| std::cmp::Reverse(loops[*index].blocks.count()));
        for index in order {
            for block in loops[index].blocks.iter() {
                innermost[block] = Some(index);
            }
        }
        Self {
            loops,
            innermost,
            position_blocks,
        }
    }

    /// The innermost loop around the node at `position`.
    pub fn innermost_at(&self, position: u32) -> Option<&Loop> {
        let block = *self.position_blocks.get(position as usize)?;
        self.innermost[block as usize].map(|index| &self.loops[index])
    }

    /// The loop that `header` heads, if it heads one.
    pub fn headed_by(&self, header: BlockId) -> Option<&Loop> {
        self.innermost[header.index()]
            .map(|index| &self.loops[index])
            .filter(|loop_| loop_.header == header)
    }

    /// The first of the use positions `uses[range]` (in order) that is in `loop_`.
    pub fn first_use_in(&self, loop_: &Loop, uses: &[u32], range: std::ops::Range<usize>) -> Option<u32> {
        loop_.first_use_in(uses, &self.position_blocks, range)
    }
}

impl Loop {
    /// Whether no node of the loop uses `value`, which is live into its header.
    pub fn carries_through(&self, value: usize) -> bool {
        self.carried_through.contains(value)
    }

    fn first_use_in(&self, uses: &[u32], position_blocks: &[u32], range: std::ops::Range<usize>) -> Option<u32> {
        let uses = &uses[range];
        let from = uses.partition_point(|position| *position < self.start);
        let to = uses.partition_point(|position| *position <= self.end);
        uses[from..to.max(from)]
            .iter()
            .copied()
            .find(|position| self.blocks.contains(position_blocks[*position as usize] as usize))
    }
}
