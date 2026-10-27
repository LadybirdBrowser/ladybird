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

use crate::bitset::BitSet;
use crate::ir::BlockId;
use crate::ir::Graph;

pub struct Dominators {
    /// The immediate dominator of each block, if it has one.
    idom: Vec<Option<BlockId>>,
    /// Whether each block is reachable from an entry.
    reachable: Vec<bool>,
    /// When a depth first walk of the dominator tree enters and leaves each reachable block. A block dominates those
    /// it enters and leaves while it is entered.
    tree_intervals: Vec<(u32, u32)>,
}

impl Dominators {
    /// Computes the dominators with the iterative algorithm of Cooper,
    /// Harvey and Kennedy, visiting blocks in reverse postorder.
    pub fn compute(graph: &Graph) -> Self {
        let count = graph.blocks.len();
        let mut reachable = vec![false; count];
        let mut postorder = Vec::with_capacity(count);
        enum Step {
            Enter(BlockId),
            Leave(BlockId),
        }
        let mut pending = graph.entries().map(Step::Enter).collect::<Vec<_>>();
        while let Some(step) = pending.pop() {
            let block = match step {
                Step::Enter(block) => block,
                Step::Leave(block) => {
                    postorder.push(block);
                    continue;
                }
            };
            if reachable[block.index()] {
                continue;
            }
            reachable[block.index()] = true;
            pending.push(Step::Leave(block));
            pending.extend(graph.successors(block).iter().copied().map(Step::Enter));
        }
        postorder.reverse();
        // NB: Rank zero is the virtual root. Walking to an immediate
        //     dominator decreases the rank, avoiding ancestor-depth scans.
        let mut ranks = vec![0; count];
        for (index, block) in postorder.iter().enumerate() {
            ranks[block.index()] = index + 1;
        }

        // NB: `None` with `processed` set means "dominated by the virtual root".
        let mut idom: Vec<Option<BlockId>> = vec![None; count];
        let mut processed = vec![false; count];
        let mut changed = true;
        while changed {
            changed = false;
            for block in postorder.iter().map(|block| block.index()) {
                let predecessors = &graph.blocks[block].predecessors;
                let new_idom = if predecessors.is_empty() {
                    None
                } else {
                    let mut candidates = predecessors.iter().filter(|predecessor| processed[predecessor.index()]);
                    let Some(first) = candidates.next() else {
                        continue;
                    };
                    let mut new_idom = Some(*first);
                    for predecessor in candidates {
                        new_idom = intersect(&idom, &ranks, new_idom, Some(*predecessor));
                    }
                    new_idom
                };
                if !processed[block] || idom[block] != new_idom {
                    processed[block] = true;
                    idom[block] = new_idom;
                    changed = true;
                }
            }
        }
        let mut dominators = Self {
            idom,
            reachable,
            tree_intervals: Vec::new(),
        };
        dominators.tree_intervals = dominators.compute_tree_intervals();
        dominators
    }

    fn compute_tree_intervals(&self) -> Vec<(u32, u32)> {
        let children = self.children();
        let mut intervals = vec![(0, 0); self.idom.len()];
        let mut time = 0;
        let mut pending = (0..self.idom.len())
            .filter(|block| self.reachable[*block] && self.idom[*block].is_none())
            .map(|block| (block, false))
            .collect::<Vec<_>>();
        while let Some((block, leaving)) = pending.pop() {
            time += 1;
            if leaving {
                intervals[block].1 = time;
                continue;
            }
            intervals[block].0 = time;
            pending.push((block, true));
            pending.extend(children[block].iter().map(|child| (child.index(), false)));
        }
        intervals
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

    /// The children of each block in the dominator tree, in linear order.
    pub fn children(&self) -> Children {
        let mut offsets = vec![0; self.idom.len() + 1];
        for (block, idom) in self.idom.iter().enumerate() {
            if let Some(idom) = idom
                && self.reachable[block]
            {
                offsets[idom.index() + 1] += 1;
            }
        }
        for index in 1..offsets.len() {
            offsets[index] += offsets[index - 1];
        }
        let mut blocks = vec![BlockId(0); *offsets.last().unwrap()];
        let mut next = offsets.clone();
        for (block, idom) in self.idom.iter().enumerate() {
            if let Some(idom) = idom
                && self.reachable[block]
            {
                blocks[next[idom.index()]] = BlockId::from_index(block);
                next[idom.index()] += 1;
            }
        }
        Children { offsets, blocks }
    }
}

pub struct Children {
    offsets: Vec<usize>,
    blocks: Vec<BlockId>,
}

impl std::ops::Index<usize> for Children {
    type Output = [BlockId];

    fn index(&self, block: usize) -> &Self::Output {
        &self.blocks[self.offsets[block]..self.offsets[block + 1]]
    }
}

/// The nearest common dominator of two blocks, `None` standing for the
/// virtual root above every entry.
fn intersect(
    idom: &[Option<BlockId>],
    ranks: &[usize],
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
            let dominators = Dominators::compute(&graph);
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
            let children = dominators.children();
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
                    assert!(children[parent.index()].contains(&block));
                }
            }
            for block in 0..count {
                assert!(children[block].windows(2).all(|pair| pair[0].index() < pair[1].index()));
            }
        }
    }
}
