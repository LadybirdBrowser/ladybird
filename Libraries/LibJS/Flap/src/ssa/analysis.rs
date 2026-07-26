/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Reusable analyses over Flap's SSA intermediate representation.

use super::{BlockId, Function, InstructionId, Operation, ValueId};
use crate::hash::HashSet;

#[derive(Debug)]
pub(crate) struct ControlFlowGraph {
    predecessors: Vec<Vec<BlockId>>,
    successors: Vec<Vec<BlockId>>,
    reachable: Vec<bool>,
    reverse_postorder: Vec<BlockId>,
}

impl ControlFlowGraph {
    pub(crate) fn compute(function: &Function) -> Self {
        let mut predecessors = vec![Vec::new(); function.blocks.len()];
        let mut successors = vec![Vec::new(); function.blocks.len()];
        for (block_index, block) in function.blocks.iter().enumerate() {
            for edge in block.terminator.iter().flat_map(|terminator| terminator.successors()) {
                if edge.block.0 >= function.blocks.len() {
                    continue;
                }
                successors[block_index].push(edge.block);
                predecessors[edge.block.0].push(BlockId(block_index));
            }
            // A guard leaves the block from the middle of it, so its exit is an
            // edge as real as the terminator's.
            for (_, failure) in function.guard_exits(BlockId(block_index)) {
                if failure.0 >= function.blocks.len() {
                    continue;
                }
                successors[block_index].push(failure);
                predecessors[failure.0].push(BlockId(block_index));
            }
        }

        let mut reachable = vec![false; function.blocks.len()];
        let mut postorder = Vec::new();
        let mut worklist = vec![(function.entry, false)];
        while let Some((block, expanded)) = worklist.pop() {
            if expanded {
                postorder.push(block);
                continue;
            }
            if block.0 >= successors.len() || std::mem::replace(&mut reachable[block.0], true) {
                continue;
            }
            worklist.push((block, true));
            worklist.extend(successors[block.0].iter().rev().map(|successor| (*successor, false)));
        }
        postorder.reverse();

        Self {
            predecessors,
            successors,
            reachable,
            reverse_postorder: postorder,
        }
    }

    pub(crate) fn predecessors(&self, block: BlockId) -> &[BlockId] {
        &self.predecessors[block.0]
    }

    pub(crate) fn successors(&self, block: BlockId) -> &[BlockId] {
        &self.successors[block.0]
    }

    pub(crate) fn is_reachable(&self, block: BlockId) -> bool {
        self.reachable[block.0]
    }

    pub(crate) fn reverse_postorder(&self) -> &[BlockId] {
        &self.reverse_postorder
    }
}

#[derive(Debug)]
pub(crate) struct DominatorTree {
    immediate_dominators: Vec<Option<BlockId>>,
    children: Vec<Vec<BlockId>>,
    /// Depth in the dominator tree, with the entry at zero.
    depths: Vec<usize>,
    /// Preorder interval of each block's subtree, which answers a dominance
    /// question by containment rather than by set membership.
    entered: Vec<u32>,
    exited: Vec<u32>,
}

impl DominatorTree {
    /// Compute immediate dominators by the Cooper-Harvey-Kennedy algorithm.
    ///
    /// Only the immediate dominator of each block is stored. The full dominator
    /// set of a block is its path to the entry, so keeping the sets themselves
    /// costs quadratic space and turns every recomputation into set cloning and
    /// intersection.
    pub(crate) fn compute(function: &Function, cfg: &ControlFlowGraph) -> Self {
        let block_count = function.blocks.len();
        let reverse_postorder = cfg.reverse_postorder();

        // Intersection walks two blocks up the tree until they meet, comparing
        // by postorder number, so a block deeper in the traversal is the one
        // that moves.
        let mut postorder_number = vec![0usize; block_count];
        for (index, block) in reverse_postorder.iter().enumerate() {
            postorder_number[block.0] = reverse_postorder.len() - index;
        }

        // A block is its own dominator here so the fixed point has a starting
        // point to intersect against; the entry keeps that value and every
        // other block is refined away from it.
        let mut immediate_dominators = vec![None; block_count];
        immediate_dominators[function.entry.0] = Some(function.entry);

        let intersect = |immediate_dominators: &[Option<BlockId>], mut lhs: BlockId, mut rhs: BlockId| {
            while lhs != rhs {
                while postorder_number[lhs.0] < postorder_number[rhs.0] {
                    lhs = immediate_dominators[lhs.0].expect("a processed block has an immediate dominator");
                }
                while postorder_number[rhs.0] < postorder_number[lhs.0] {
                    rhs = immediate_dominators[rhs.0].expect("a processed block has an immediate dominator");
                }
            }
            lhs
        };

        loop {
            let mut changed = false;
            for block in reverse_postorder.iter().copied().skip(1) {
                let mut new_immediate_dominator = None;
                for predecessor in cfg.predecessors(block).iter().copied() {
                    if !cfg.is_reachable(predecessor) || immediate_dominators[predecessor.0].is_none() {
                        continue;
                    }
                    new_immediate_dominator = Some(match new_immediate_dominator {
                        None => predecessor,
                        Some(current) => intersect(&immediate_dominators, predecessor, current),
                    });
                }
                if new_immediate_dominator.is_some() && immediate_dominators[block.0] != new_immediate_dominator {
                    immediate_dominators[block.0] = new_immediate_dominator;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }

        // The entry dominates itself but is nobody's child, and an unreachable
        // block has no dominator at all.
        immediate_dominators[function.entry.0] = None;
        let mut children = vec![Vec::new(); block_count];
        for (block_index, immediate_dominator) in immediate_dominators.iter().copied().enumerate() {
            if let Some(immediate_dominator) = immediate_dominator {
                children[immediate_dominator.0].push(BlockId(block_index));
            }
        }

        // Numbering the tree once turns dominance into an interval containment
        // test, which is what the passes ask for over and over.
        let mut depths = vec![0usize; block_count];
        let mut entered = vec![0u32; block_count];
        let mut exited = vec![0u32; block_count];
        let mut clock = 0u32;
        let mut worklist = vec![(function.entry, false)];
        while let Some((block, finished)) = worklist.pop() {
            if finished {
                exited[block.0] = clock;
                continue;
            }
            clock += 1;
            entered[block.0] = clock;
            worklist.push((block, true));
            for child in children[block.0].iter().copied() {
                depths[child.0] = depths[block.0] + 1;
                worklist.push((child, false));
            }
        }

        Self {
            immediate_dominators,
            children,
            depths,
            entered,
            exited,
        }
    }

    pub(crate) fn dominates(&self, dominator: BlockId, block: BlockId) -> bool {
        // An unvisited block was never entered, so it dominates nothing and is
        // dominated by nothing, which is what the set-based answer used to be.
        self.entered[block.0] != 0
            && self.entered[dominator.0] != 0
            && self.entered[dominator.0] <= self.entered[block.0]
            && self.exited[block.0] <= self.exited[dominator.0]
    }

    pub(crate) fn immediate_dominator(&self, block: BlockId) -> Option<BlockId> {
        self.immediate_dominators[block.0]
    }

    pub(crate) fn children(&self, block: BlockId) -> &[BlockId] {
        &self.children[block.0]
    }

    pub(crate) fn depth(&self, block: BlockId) -> usize {
        self.depths[block.0]
    }

    pub(crate) fn nearest_common_dominator(&self, mut lhs: BlockId, mut rhs: BlockId) -> Option<BlockId> {
        if self.entered[lhs.0] == 0 || self.entered[rhs.0] == 0 {
            return None;
        }
        while self.depths[lhs.0] > self.depths[rhs.0] {
            lhs = self.immediate_dominators[lhs.0]?;
        }
        while self.depths[rhs.0] > self.depths[lhs.0] {
            rhs = self.immediate_dominators[rhs.0]?;
        }
        while lhs != rhs {
            lhs = self.immediate_dominators[lhs.0]?;
            rhs = self.immediate_dominators[rhs.0]?;
        }
        Some(lhs)
    }
}

/// Where guards sit, and which blocks a guard can reach around them.
///
/// A block containing a guard is entered before the guard and left again from
/// the middle of it, so the values it defines after a guard are not available
/// everywhere it dominates: a path that took the exit skipped them. Dominance
/// alone cannot say that, so every pass that moves or shares a value across
/// blocks asks this instead.
#[derive(Debug, Default)]
pub(crate) struct GuardExits {
    any: bool,
    first_guard: Vec<Option<usize>>,
    reachable_from_exit: Vec<bool>,
}

impl GuardExits {
    pub(crate) fn compute(function: &Function, cfg: &ControlFlowGraph) -> Self {
        let first_guard = (0..function.blocks.len())
            .map(|index| function.first_guard_position(BlockId(index)))
            .collect::<Vec<_>>();
        if first_guard.iter().all(Option::is_none) {
            return Self {
                any: false,
                first_guard,
                reachable_from_exit: vec![false; function.blocks.len()],
            };
        }
        let mut reachable_from_exit = vec![false; function.blocks.len()];
        let mut worklist = (0..function.blocks.len())
            .flat_map(|index| function.guard_exits(BlockId(index)))
            .map(|(_, failure)| failure)
            .collect::<Vec<_>>();
        while let Some(block) = worklist.pop() {
            if std::mem::replace(&mut reachable_from_exit[block.0], true) {
                continue;
            }
            worklist.extend(cfg.successors(block).iter().copied());
        }
        Self {
            any: true,
            first_guard,
            reachable_from_exit,
        }
    }

    /// Whether a value defined at `position` in `block` is available in `user`.
    ///
    /// The caller has already established that `block` dominates `user`. The
    /// answer is conservative: it asks whether *any* guard exit reaches `user`
    /// rather than whether one of this block's does, because the exact question
    /// costs a walk per block. [`Self::definition_escapes`] asks it exactly.
    pub(crate) fn definition_reaches(&self, block: BlockId, position: usize, user: BlockId) -> bool {
        if !self.any || block == user {
            return true;
        }
        match self.first_guard[block.0] {
            Some(guard) if position > guard => !self.reachable_from_exit[user.0],
            _ => true,
        }
    }

    /// Whether a guard really carries control from before `position` in `block`
    /// to `user`, skipping the definition that sits between them.
    ///
    /// This walks out of each exit, so it costs a pass over the function. Only
    /// validation asks it, and only where the cheap answer was already "maybe".
    pub(crate) fn definition_escapes(
        &self,
        function: &Function,
        cfg: &ControlFlowGraph,
        block: BlockId,
        position: usize,
        user: BlockId,
    ) -> bool {
        // Coming back around to the block runs the definition after all, so
        // only paths that stay out of it skip it.
        let mut reached = vec![false; function.blocks.len()];
        reached[block.0] = true;
        let mut worklist = function
            .guard_exits(block)
            .filter(|(guard, _)| *guard < position)
            .map(|(_, failure)| failure)
            .collect::<Vec<_>>();
        while let Some(reached_block) = worklist.pop() {
            if reached_block == user {
                return true;
            }
            if std::mem::replace(&mut reached[reached_block.0], true) {
                continue;
            }
            worklist.extend(cfg.successors(reached_block).iter().copied());
        }
        false
    }

    /// Whether control can leave `block` before its terminator.
    pub(crate) fn has_guard(&self, block: BlockId) -> bool {
        self.any && self.first_guard[block.0].is_some()
    }

    pub(crate) fn is_reachable_from_exit(&self, block: BlockId) -> bool {
        self.reachable_from_exit[block.0]
    }
}

#[derive(Debug)]
pub(crate) struct NaturalLoop {
    pub(crate) header: BlockId,
    pub(crate) blocks: HashSet<BlockId>,
}

pub(crate) fn find_natural_loops(
    function: &Function,
    cfg: &ControlFlowGraph,
    dominators: &DominatorTree,
) -> Vec<NaturalLoop> {
    let mut blocks_by_header = vec![HashSet::default(); function.blocks.len()];
    for source in cfg.reverse_postorder().iter().copied() {
        for header in cfg.successors(source) {
            if !dominators.dominates(*header, source) {
                continue;
            }
            let blocks = &mut blocks_by_header[header.0];
            blocks.insert(*header);
            let mut worklist = vec![source];
            while let Some(block) = worklist.pop() {
                if !blocks.insert(block) || block == *header {
                    continue;
                }
                worklist.extend(cfg.predecessors(block).iter().copied());
            }
        }
    }

    let mut loops = blocks_by_header
        .into_iter()
        .enumerate()
        .filter(|(_, blocks)| !blocks.is_empty())
        .map(|(header, blocks)| {
            let header = BlockId(header);
            NaturalLoop { header, blocks }
        })
        .collect::<Vec<_>>();
    loops.sort_by_key(|natural_loop| (natural_loop.blocks.len(), natural_loop.header.0));
    loops
}

#[derive(Debug)]
pub(crate) struct InstructionLayout {
    blocks: Vec<BlockId>,
    positions: Vec<usize>,
}

impl InstructionLayout {
    pub(crate) fn compute(function: &Function) -> Result<Self, String> {
        let mut blocks = vec![BlockId(usize::MAX); function.instructions.len()];
        let mut positions = vec![usize::MAX; function.instructions.len()];
        for (block_index, block) in function.blocks.iter().enumerate() {
            for (position, instruction) in block.instructions.iter().copied().enumerate() {
                if instruction.0 >= function.instructions.len() {
                    return Err(format!(
                        "block {block_index} contains invalid instruction {instruction:?}"
                    ));
                }
                if blocks[instruction.0].0 != usize::MAX {
                    return Err(format!("instruction {instruction:?} belongs to more than one block"));
                }
                blocks[instruction.0] = BlockId(block_index);
                positions[instruction.0] = position;
            }
        }
        for (instruction_index, block) in blocks.iter().enumerate() {
            if block.0 == usize::MAX {
                return Err(format!("instruction {instruction_index} does not belong to a block"));
            }
        }
        Ok(Self { blocks, positions })
    }

    pub(crate) fn block(&self, instruction: InstructionId) -> BlockId {
        self.blocks[instruction.0]
    }

    pub(crate) fn position(&self, instruction: InstructionId) -> usize {
        self.positions[instruction.0]
    }
}

#[derive(Debug)]
pub(crate) struct ValueUses {
    counts: Vec<u32>,
}

impl ValueUses {
    pub(crate) fn compute(function: &Function) -> Self {
        let mut counts = vec![0; function.values.len()];
        for instruction in &function.instructions {
            for input in &instruction.inputs {
                counts[input.0] += 1;
            }
        }
        for block in &function.blocks {
            for input in block.terminator.as_ref().unwrap().inputs() {
                counts[input.0] += 1;
            }
        }
        Self { counts }
    }

    pub(crate) fn count(&self, value: ValueId) -> u32 {
        self.counts[value.0]
    }

    pub(crate) fn remove_use(&mut self, value: ValueId) {
        self.counts[value.0] = self.counts[value.0]
            .checked_sub(1)
            .expect("only recorded SSA uses can be removed");
    }
}

pub(crate) fn reachable_blocks(function: &Function, cfg: &ControlFlowGraph) -> Vec<bool> {
    let mut reachable = vec![false; function.blocks.len()];
    let mut worklist = vec![function.entry];
    while let Some(block) = worklist.pop() {
        if std::mem::replace(&mut reachable[block.0], true) {
            continue;
        }
        worklist.extend(cfg.successors(block).iter().copied());
        worklist.extend(function.blocks[block.0].instructions.iter().filter_map(|instruction| {
            match function.instructions[instruction.0].operation {
                Operation::BlockReference(target) => Some(target),
                _ => None,
            }
        }));
    }
    reachable
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ssa::{BlockLayout, Terminator};
    use crate::types::Type;

    #[test]
    fn computes_cfg_dominators_and_natural_loops() {
        let mut function = Function::new("loop", vec![Type::Bool], Vec::new());
        let header = function.create_empty_block("header", BlockLayout::Hot);
        let body = function.create_empty_block("body", BlockLayout::Hot);
        let exit = function.create_empty_block("exit", BlockLayout::Hot);
        function.set_terminator(function.entry, Terminator::jump(header));
        function.set_terminator(header, Terminator::branch(function.parameter(0), body, exit));
        function.set_terminator(body, Terminator::jump(header));
        function.set_terminator(exit, Terminator::Return(Vec::new()));

        let cfg = ControlFlowGraph::compute(&function);
        let dominators = DominatorTree::compute(&function, &cfg);
        let loops = find_natural_loops(&function, &cfg, &dominators);

        assert_eq!(cfg.predecessors(header), [function.entry, body]);
        assert!(dominators.dominates(header, body));
        assert_eq!(dominators.immediate_dominator(body), Some(header));
        assert_eq!(dominators.children(header), [body, exit]);
        assert_eq!(loops.len(), 1);
        assert_eq!(loops[0].header, header);
        assert_eq!(loops[0].blocks, HashSet::from_iter([header, body]));
    }
}
