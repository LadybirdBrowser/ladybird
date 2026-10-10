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

use crate::ir::BlockId;
use crate::ir::Graph;

pub struct Dominators {
    /// The immediate dominator of each block, if it has one.
    idom: Vec<Option<BlockId>>,
    /// Whether each block is reachable from an entry.
    reachable: Vec<bool>,
}

impl Dominators {
    /// Computes the dominators with the iterative algorithm of Cooper,
    /// Harvey and Kennedy, visiting blocks in linear order, where every block
    /// comes after its predecessors except over loop back edges.
    pub fn compute(graph: &Graph) -> Self {
        let count = graph.blocks.len();
        let mut reachable = vec![false; count];
        let mut pending = graph.entries().map(BlockId::index).collect::<Vec<_>>();
        while let Some(block) = pending.pop() {
            if reachable[block] {
                continue;
            }
            reachable[block] = true;
            pending.extend(
                graph
                    .successors(BlockId::from_index(block))
                    .iter()
                    .map(|successor| successor.index()),
            );
        }

        // NB: `None` with `processed` set means "dominated by the virtual root".
        let mut idom: Vec<Option<BlockId>> = vec![None; count];
        let mut processed = vec![false; count];
        let mut changed = true;
        while changed {
            changed = false;
            for block in 0..count {
                if !reachable[block] {
                    continue;
                }
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
                        new_idom = intersect(&idom, new_idom, Some(*predecessor));
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
        Self { idom, reachable }
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

    /// Whether every iteration of a loop with back edges from
    /// `back_edge_sources` that goes around runs `block`.
    pub fn runs_every_iteration(&self, block: BlockId, back_edge_sources: &[BlockId]) -> bool {
        back_edge_sources.iter().all(|source| self.dominates(block, *source))
    }

    /// Whether every path from an entry to `block` goes through `dominator`.
    /// A block dominates itself.
    pub fn dominates(&self, dominator: BlockId, block: BlockId) -> bool {
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
    pub fn children(&self) -> Vec<Vec<BlockId>> {
        let mut children = vec![Vec::new(); self.idom.len()];
        for (block, idom) in self.idom.iter().enumerate() {
            if let Some(idom) = idom
                && self.reachable[block]
            {
                children[idom.index()].push(BlockId::from_index(block));
            }
        }
        children
    }
}

/// The nearest common dominator of two blocks, `None` standing for the
/// virtual root above every entry.
fn intersect(idom: &[Option<BlockId>], a: Option<BlockId>, b: Option<BlockId>) -> Option<BlockId> {
    let depth = |mut block: Option<BlockId>| {
        let mut depth = 0;
        while let Some(current) = block {
            depth += 1;
            block = idom[current.index()];
        }
        depth
    };
    let (mut a, mut b) = (a, b);
    let (mut depth_a, mut depth_b) = (depth(a), depth(b));
    while depth_a > depth_b {
        a = a.and_then(|block| idom[block.index()]);
        depth_a -= 1;
    }
    while depth_b > depth_a {
        b = b.and_then(|block| idom[block.index()]);
        depth_b -= 1;
    }
    while a != b {
        a = a.and_then(|block| idom[block.index()]);
        b = b.and_then(|block| idom[block.index()]);
    }
    a
}
