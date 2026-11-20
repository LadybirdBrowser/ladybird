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

use ak::Zone;
use ak::ZoneVec;

use crate::ir::BlockId;
use crate::ir::Graph;
use crate::tables::Lists;
use ak::ZoneBitSet;

/// The dominator tree of a graph, in lists of a work zone. `compute()` makes them anew for the graph as it is then,
/// in the memory they have where it is enough.
pub struct Dominators<'w> {
    /// The immediate dominator of each block, if it has one.
    idom: ZoneVec<'w, Option<BlockId>>,
    /// Whether each block is reachable from an entry.
    reachable: ZoneVec<'w, bool>,
    /// When a depth first walk of the dominator tree enters and leaves each reachable block. A block dominates those
    /// it enters and leaves while it is entered.
    tree_intervals: ZoneVec<'w, (u32, u32)>,
    /// The children of each block in the dominator tree, in linear order.
    children: Lists<'w, BlockId>,
    /// The reachable blocks in reverse postorder.
    order: ZoneVec<'w, BlockId>,
    /// The place of each reachable block in `order`, plus one.
    ranks: ZoneVec<'w, u32>,
    /// Whether each block has an immediate dominator or the virtual root above every entry as its dominator so far.
    processed: ZoneVec<'w, bool>,
    /// The blocks that a walk still has to enter or leave, and whether it is to leave them.
    pending: ZoneVec<'w, (BlockId, bool)>,
}

impl<'w> Dominators<'w> {
    /// The most blocks a graph has whose dominators a compile computes. A compile whose graph has more fails (see
    /// `passes::optimize()`).
    pub const MAX_BLOCKS: usize = 1 << 24;

    /// Lists for the dominators of a graph, which take no memory before `compute()` uses them.
    pub fn new_in(work: &'w Zone) -> Self {
        Self {
            idom: ZoneVec::new_in(work),
            reachable: ZoneVec::new_in(work),
            tree_intervals: ZoneVec::new_in(work),
            children: Lists::new_in(work),
            order: ZoneVec::new_in(work),
            ranks: ZoneVec::new_in(work),
            processed: ZoneVec::new_in(work),
            pending: ZoneVec::new_in(work),
        }
    }

    /// The dominators of `graph` as it is now, in `work`.
    pub fn of(graph: &Graph, work: &'w Zone) -> Self {
        let mut dominators = Self::new_in(work);
        dominators.compute(graph);
        dominators
    }

    /// Computes the dominators of `graph` with the iterative algorithm of Cooper,
    /// Harvey and Kennedy, visiting blocks in reverse postorder. The lists then hold the dominators of `graph` as
    /// it is now, and nothing of an earlier graph.
    pub fn compute(&mut self, graph: &Graph) {
        let count = graph.blocks.len();
        self.reachable.clear();
        self.reachable.resize(count, false);
        self.order.clear();
        self.order.reserve(count);
        self.pending.clear();
        // NB: A block waits to be left one time, and to be entered one time for each edge into it and for each entry
        //     it is.
        let edges = graph.blocks.iter().map(|block| block.predecessors.len()).sum::<usize>();
        self.pending.reserve(count + edges + graph.entries().count());
        for entry in graph.entries() {
            self.pending.push((entry, false));
        }
        while let Some((block, leaving)) = self.pending.pop() {
            if leaving {
                self.order.push(block);
                continue;
            }
            if self.reachable[block.index()] {
                continue;
            }
            self.reachable[block.index()] = true;
            self.pending.push((block, true));
            graph.control(block).op.for_each_successor(|successor| {
                self.pending.push((successor, false));
            });
        }
        self.order.reverse();
        // NB: Rank zero is the virtual root. Walking to an immediate
        //     dominator decreases the rank, avoiding ancestor-depth scans.
        self.ranks.clear();
        self.ranks.resize(count, 0);
        for (index, block) in self.order.iter().enumerate() {
            self.ranks[block.index()] = index as u32 + 1;
        }

        // NB: `None` with `processed` set means "dominated by the virtual root".
        self.idom.clear();
        self.idom.resize(count, None);
        self.processed.clear();
        self.processed.resize(count, false);
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
        self.compute_children();
        self.compute_tree_intervals();
    }

    fn compute_children(&mut self) {
        let count = self.idom.len();
        self.children.begin_counts(count);
        for block in 0..count {
            if let Some(idom) = self.idom[block]
                && self.reachable[block]
            {
                self.children.count(idom.index());
            }
        }
        self.children.end_counts(BlockId(0));
        for block in 0..count {
            if let Some(idom) = self.idom[block]
                && self.reachable[block]
            {
                self.children.fill(idom.index(), BlockId::from_index(block));
            }
        }
        self.children.end_fill();
    }

    fn compute_tree_intervals(&mut self) {
        let count = self.idom.len();
        self.tree_intervals.clear();
        self.tree_intervals.resize(count, (0, 0));
        let mut time = 0;
        self.pending.clear();
        for block in 0..count {
            if self.reachable[block] && self.idom[block].is_none() {
                self.pending.push((BlockId::from_index(block), false));
            }
        }
        while let Some((block, leaving)) = self.pending.pop() {
            time += 1;
            if leaving {
                self.tree_intervals[block.index()].1 = time;
                continue;
            }
            self.tree_intervals[block.index()].0 = time;
            self.pending.push((block, true));
            for child in self.children.get(block.index()) {
                self.pending.push((*child, false));
            }
        }
    }

    pub fn idom(&self, block: BlockId) -> Option<BlockId> {
        self.idom[block.index()]
    }

    pub fn is_reachable(&self, block: BlockId) -> bool {
        self.reachable[block.index()]
    }

    /// The predecessors of `header` that it dominates: the sources of the
    /// back edges of the loop it heads, if it heads one.
    pub fn back_edge_sources<'v>(&self, graph: &Graph, header: BlockId, work: &'v Zone) -> ZoneVec<'v, BlockId> {
        ZoneVec::from_iter_in(
            graph
                .block(header)
                .predecessors
                .iter()
                .copied()
                .filter(|predecessor| self.dominates(header, *predecessor)),
            work,
        )
    }

    /// The blocks of the loop that `header` heads, with back edges from
    /// `back_edge_sources`: the header and every reachable block that reaches
    /// one of them without going through it.
    pub fn loop_blocks<'v>(
        &self,
        graph: &Graph,
        header: BlockId,
        back_edge_sources: &[BlockId],
        work: &'v Zone,
    ) -> ZoneBitSet<'v> {
        let mut blocks = ZoneBitSet::new_in(graph.blocks.len(), work);
        blocks.insert(header.index());
        let mut pending = ZoneVec::from_slice_in(back_edge_sources, work);
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
    use crate::ir::{Block, BranchCondition, Op};

    #[test]
    fn cyclic_graphs_with_multiple_entries_match_dominator_sets() {
        let work = crate::test_work();
        let mut dominators = Dominators::new_in(&work);
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
                let control = graph.add(
                    Op::Branch {
                        condition: BranchCondition::Bool,
                        if_true: a,
                        if_false: b,
                    },
                    &[],
                    None,
                    0,
                );
                graph.blocks[block].control = Some(control);
                for successor in [a, b] {
                    graph.blocks[successor.index()]
                        .predecessors
                        .push(BlockId::from_index(block));
                }
            }
            // NB: The tables of one graph serve the next one.
            dominators.compute(&graph);
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
