/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The dominator tree of a graph's blocks.
//!
//! The entries (block 0 and the on-stack replacement entries) have no
//! immediate dominator, and neither has a block reached from several
//! entries.

use ak::Column;
use ak::ColumnFull;

use crate::bitset::BitSet;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::tables::Lists;
use crate::tables::pop;
use crate::tables::release;
use crate::tables::reset;

/// The dominator tree of a graph, in tables that serve one graph after the other: `compute()` makes no block of the
/// heap.
pub struct Dominators {
    /// The immediate dominator of each block, if it has one.
    idom: Column<Option<BlockId>>,
    /// Whether each block is reachable from an entry.
    reachable: Column<bool>,
    /// When a depth first walk of the dominator tree enters and leaves each reachable block. A block dominates those
    /// it enters and leaves while it is entered.
    tree_intervals: Column<(u32, u32)>,
    /// The children of each block in the dominator tree, in linear order.
    children: Lists<BlockId>,
    /// The reachable blocks in reverse postorder.
    order: Column<BlockId>,
    /// The place of each reachable block in `order`, plus one.
    ranks: Column<u32>,
    /// Whether each block has an immediate dominator or the virtual root above every entry as its dominator so far.
    processed: Column<bool>,
    /// The blocks that a walk still has to enter or leave, and whether it is to leave them.
    pending: Column<(BlockId, bool)>,
}

impl Dominators {
    // NB: The tables do not hold the dominators of a graph with more blocks.
    const MAX_BLOCKS: usize = 1 << 24;

    /// Tables that take no memory before a graph uses them.
    pub fn new() -> Self {
        fn by_block<T: Copy>() -> Column<T> {
            Column::new(Dominators::MAX_BLOCKS, c"JIT dominators")
        }
        Self {
            idom: by_block(),
            reachable: by_block(),
            tree_intervals: by_block(),
            children: Lists::new(Self::MAX_BLOCKS, Self::MAX_BLOCKS, c"JIT dominators"),
            order: by_block(),
            ranks: by_block(),
            processed: by_block(),
            // NB: A block waits to be left one time, and to be entered one time for each edge into it.
            pending: Column::new(Self::MAX_BLOCKS * 16, c"JIT dominators"),
        }
    }

    /// Drops the tables and gives their memory back to the system.
    pub fn release_memory(&mut self) {
        release(&mut self.idom);
        release(&mut self.reachable);
        release(&mut self.tree_intervals);
        self.children.release_memory();
        release(&mut self.order);
        release(&mut self.ranks);
        release(&mut self.processed);
        release(&mut self.pending);
    }

    /// Computes the dominators of `graph` with the iterative algorithm of Cooper,
    /// Harvey and Kennedy, visiting blocks in reverse postorder. The tables then hold the dominators of `graph` as
    /// it is now, and nothing of an earlier graph.
    pub fn compute(&mut self, graph: &Graph) -> Result<(), ColumnFull> {
        let count = graph.blocks.len();
        reset(&mut self.reachable, count, false)?;
        self.order.clear();
        self.pending.clear();
        for entry in graph.entries() {
            self.pending.try_push((entry, false))?;
        }
        while let Some((block, leaving)) = pop(&mut self.pending) {
            if leaving {
                self.order.try_push(block)?;
                continue;
            }
            if self.reachable[block.index()] {
                continue;
            }
            self.reachable[block.index()] = true;
            self.pending.try_push((block, true))?;
            let mut pushed = Ok(());
            graph.control(block).op.for_each_successor(|successor| {
                if pushed.is_ok() {
                    pushed = self.pending.try_push((successor, false));
                }
            });
            pushed?;
        }
        self.order.reverse();
        // NB: Rank zero is the virtual root. Walking to an immediate
        //     dominator decreases the rank, avoiding ancestor-depth scans.
        reset(&mut self.ranks, count, 0)?;
        for (index, block) in self.order.iter().enumerate() {
            self.ranks[block.index()] = index as u32 + 1;
        }

        // NB: `None` with `processed` set means "dominated by the virtual root".
        reset(&mut self.idom, count, None)?;
        reset(&mut self.processed, count, false)?;
        let mut changed = true;
        while changed {
            changed = false;
            for block in self.order.iter().map(|block| block.index()) {
                let predecessors = &graph.blocks[block].predecessors;
                let new_idom = if predecessors.is_empty() {
                    None
                } else {
                    let mut candidates = predecessors
                        .iter()
                        .filter(|predecessor| self.processed[predecessor.index()]);
                    let Some(first) = candidates.next() else {
                        continue;
                    };
                    let mut new_idom = Some(*first);
                    for predecessor in candidates {
                        new_idom = intersect(&self.idom, &self.ranks, new_idom, Some(*predecessor));
                    }
                    new_idom
                };
                if !self.processed[block] || self.idom[block] != new_idom {
                    self.processed[block] = true;
                    self.idom[block] = new_idom;
                    changed = true;
                }
            }
        }
        self.compute_children()?;
        self.compute_tree_intervals()
    }

    fn compute_children(&mut self) -> Result<(), ColumnFull> {
        let count = self.idom.len();
        self.children.begin_counts(count)?;
        for block in 0..count {
            if let Some(idom) = self.idom[block]
                && self.reachable[block]
            {
                self.children.count(idom.index());
            }
        }
        self.children.end_counts(BlockId(0))?;
        for block in 0..count {
            if let Some(idom) = self.idom[block]
                && self.reachable[block]
            {
                self.children.fill(idom.index(), BlockId::from_index(block));
            }
        }
        self.children.end_fill();
        Ok(())
    }

    fn compute_tree_intervals(&mut self) -> Result<(), ColumnFull> {
        let count = self.idom.len();
        reset(&mut self.tree_intervals, count, (0, 0))?;
        let mut time = 0;
        self.pending.clear();
        for block in 0..count {
            if self.reachable[block] && self.idom[block].is_none() {
                self.pending.try_push((BlockId::from_index(block), false))?;
            }
        }
        while let Some((block, leaving)) = pop(&mut self.pending) {
            time += 1;
            if leaving {
                self.tree_intervals[block.index()].1 = time;
                continue;
            }
            self.tree_intervals[block.index()].0 = time;
            self.pending.try_push((block, true))?;
            for child in self.children.get(block.index()) {
                self.pending.try_push((*child, false))?;
            }
        }
        Ok(())
    }

    pub fn idom(&self, block: BlockId) -> Option<BlockId> {
        self.idom[block.index()]
    }

    pub fn is_reachable(&self, block: BlockId) -> bool {
        self.reachable[block.index()]
    }

    /// The predecessors of `header` that it dominates: the sources of the
    /// back edges of the loop it heads, if it heads one.
    pub fn back_edge_sources(&self, graph: &Graph, header: BlockId) -> Vec<BlockId> {
        graph
            .block(header)
            .predecessors
            .iter()
            .copied()
            .filter(|predecessor| self.dominates(header, *predecessor))
            .collect()
    }

    /// The blocks of the loop that `header` heads, with back edges from
    /// `back_edge_sources`: the header and every reachable block that reaches
    /// one of them without going through it.
    pub fn loop_blocks(&self, graph: &Graph, header: BlockId, back_edge_sources: &[BlockId]) -> BitSet {
        let mut blocks = BitSet::new(graph.blocks.len());
        blocks.insert(header.index());
        let mut pending = back_edge_sources.to_vec();
        while let Some(block) = pending.pop() {
            if blocks.contains(block.index()) || !self.is_reachable(block) {
                continue;
            }
            blocks.insert(block.index());
            pending.extend(graph.block(block).predecessors.iter().copied());
        }
        blocks
    }

    /// Whether every iteration of a loop with back edges from
    /// `back_edge_sources` that goes around runs `block`.
    pub fn runs_every_iteration(&self, block: BlockId, back_edge_sources: &[BlockId]) -> bool {
        back_edge_sources.iter().all(|source| self.dominates(block, *source))
    }

    /// Whether every path from an entry to `block` goes through `dominator`.
    /// A block dominates itself.
    pub fn dominates(&self, dominator: BlockId, block: BlockId) -> bool {
        // OPTIMIZATION: Walking up from the block would take time proportional to the depth of the dominator tree,
        //               which is large in long straight-line code with many branches, like inlined assertions.
        if self.reachable[dominator.index()] && self.reachable[block.index()] {
            let (enter, leave) = self.tree_intervals[dominator.index()];
            let (block_enter, block_leave) = self.tree_intervals[block.index()];
            return enter <= block_enter && block_leave <= leave;
        }
        let mut current = Some(block);
        while let Some(candidate) = current {
            if candidate == dominator {
                return true;
            }
            current = self.idom[candidate.index()];
        }
        false
    }

    /// The children of `block` in the dominator tree, in linear order.
    pub fn children(&self, block: BlockId) -> &[BlockId] {
        self.children.get(block.index())
    }
}

impl Default for Dominators {
    fn default() -> Self {
        Self::new()
    }
}

/// The nearest common dominator of two blocks, `None` standing for the
/// virtual root above every entry.
fn intersect(
    idom: &[Option<BlockId>],
    ranks: &[u32],
    mut a: Option<BlockId>,
    mut b: Option<BlockId>,
) -> Option<BlockId> {
    let rank = |block: Option<BlockId>| block.map_or(0, |block| ranks[block.index()]);
    while a != b {
        if rank(a) > rank(b) {
            a = a.and_then(|block| idom[block.index()]);
        } else {
            b = b.and_then(|block| idom[block.index()]);
        }
    }
    a
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ir::{Block, BranchCondition, Node, Op};

    #[test]
    fn cyclic_graphs_with_multiple_entries_match_dominator_sets() {
        let mut dominators = Dominators::new();
        for seed in 1..33u64 {
            let mut random = seed;
            let mut graph = Graph::default();
            let count = 24;
            for _ in 0..count {
                graph.add_block(Block::default());
            }
            graph.osr_entries.push((0, BlockId(1)));
            for block in 0..count {
                let mut target = || {
                    random = random.wrapping_mul(6364136223846793005).wrapping_add(1);
                    BlockId::from_index(2 + (random >> 32) as usize % (count - 2))
                };
                let (a, b) = (target(), target());
                let control = graph.add_node(Node::new(
                    Op::Branch {
                        condition: BranchCondition::Bool,
                        if_true: a,
                        if_false: b,
                    },
                    vec![],
                    None,
                    0,
                ));
                graph.blocks[block].control = Some(control);
                for successor in [a, b] {
                    graph.blocks[successor.index()]
                        .predecessors
                        .push(BlockId::from_index(block));
                }
            }
            // NB: The tables of one graph serve the next one.
            dominators.compute(&graph).unwrap();
            // NB: Independently solve the defining set equations: each
            //     block dominates itself and inherits the intersection of
            //     its predecessors' dominators.
            let mut sets = vec![vec![true; count]; count];
            for entry in graph.entries() {
                sets[entry.index()].fill(false);
                sets[entry.index()][entry.index()] = true;
            }
            loop {
                let mut changed = false;
                for block in 2..count {
                    if !dominators.is_reachable(BlockId::from_index(block)) {
                        continue;
                    }
                    let mut next = vec![true; count];
                    for predecessor in &graph.blocks[block].predecessors {
                        if dominators.is_reachable(*predecessor) {
                            for (value, inherited) in next.iter_mut().zip(&sets[predecessor.index()]) {
                                *value &= inherited;
                            }
                        }
                    }
                    next[block] = true;
                    if sets[block] != next {
                        sets[block] = next;
                        changed = true;
                    }
                }
                if !changed {
                    break;
                }
            }
            for (block, expected) in sets.iter().enumerate() {
                let block = BlockId::from_index(block);
                if !dominators.is_reachable(block) {
                    continue;
                }
                for (candidate, expected) in expected.iter().enumerate() {
                    assert_eq!(
                        dominators.dominates(BlockId::from_index(candidate), block),
                        *expected,
                        "seed {seed}, block {block:?}, candidate {candidate}"
                    );
                }
                if let Some(parent) = dominators.idom(block) {
                    assert!(dominators.children(parent).contains(&block));
                }
            }
            for block in 0..count {
                let children = dominators.children(BlockId::from_index(block));
                assert!(children.windows(2).all(|pair| pair[0].index() < pair[1].index()));
            }
        }
    }
}
