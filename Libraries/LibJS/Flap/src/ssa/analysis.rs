/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Reusable analyses over Flap's SSA intermediate representation.

use super::{BlockId, Function, InstructionId, Operation, ValueId};
use std::collections::HashSet;

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
    dominators: Vec<HashSet<BlockId>>,
    immediate_dominators: Vec<Option<BlockId>>,
    children: Vec<Vec<BlockId>>,
}

impl DominatorTree {
    pub(crate) fn compute(function: &Function, cfg: &ControlFlowGraph) -> Self {
        let all_reachable = cfg.reverse_postorder().iter().copied().collect::<HashSet<_>>();
        let mut dominators = vec![HashSet::new(); function.blocks.len()];
        for block in cfg.reverse_postorder() {
            dominators[block.0] = all_reachable.clone();
        }
        dominators[function.entry.0] = HashSet::from([function.entry]);

        loop {
            let mut changed = false;
            for block in cfg.reverse_postorder().iter().copied().skip(1) {
                let reachable_predecessors = cfg
                    .predecessors(block)
                    .iter()
                    .copied()
                    .filter(|predecessor| cfg.is_reachable(*predecessor))
                    .collect::<Vec<_>>();
                let mut next = if let Some((first, rest)) = reachable_predecessors.split_first() {
                    let mut intersection = dominators[first.0].clone();
                    for predecessor in rest {
                        intersection.retain(|candidate| dominators[predecessor.0].contains(candidate));
                    }
                    intersection
                } else {
                    HashSet::new()
                };
                next.insert(block);
                if next != dominators[block.0] {
                    dominators[block.0] = next;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }

        let mut immediate_dominators = vec![None; function.blocks.len()];
        let mut children = vec![Vec::new(); function.blocks.len()];
        for block_index in 0..function.blocks.len() {
            let block = BlockId(block_index);
            if block == function.entry || !cfg.is_reachable(block) {
                continue;
            }
            let immediate_dominator = dominators[block.0]
                .iter()
                .copied()
                .filter(|dominator| *dominator != block)
                .max_by_key(|dominator| dominators[dominator.0].len());
            immediate_dominators[block.0] = immediate_dominator;
            if let Some(immediate_dominator) = immediate_dominator {
                children[immediate_dominator.0].push(block);
            }
        }

        Self {
            dominators,
            immediate_dominators,
            children,
        }
    }

    pub(crate) fn dominates(&self, dominator: BlockId, block: BlockId) -> bool {
        self.dominators[block.0].contains(&dominator)
    }

    pub(crate) fn immediate_dominator(&self, block: BlockId) -> Option<BlockId> {
        self.immediate_dominators[block.0]
    }

    pub(crate) fn children(&self, block: BlockId) -> &[BlockId] {
        &self.children[block.0]
    }

    pub(crate) fn depth(&self, block: BlockId) -> usize {
        self.dominators[block.0].len().saturating_sub(1)
    }

    pub(crate) fn nearest_common_dominator(&self, lhs: BlockId, rhs: BlockId) -> Option<BlockId> {
        self.dominators[lhs.0]
            .intersection(&self.dominators[rhs.0])
            .copied()
            .max_by_key(|block| self.depth(*block))
    }
}

#[derive(Debug)]
pub(crate) struct NaturalLoop {
    pub(crate) header: BlockId,
    pub(crate) blocks: HashSet<BlockId>,
}

pub(crate) fn find_natural_loops(function: &Function, cfg: &ControlFlowGraph, dominators: &DominatorTree) -> Vec<NaturalLoop> {
    let mut blocks_by_header = vec![HashSet::new(); function.blocks.len()];
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
    use crate::types::Type;
    use crate::ssa::{BlockLayout, Terminator};

    #[test]
    fn computes_cfg_dominators_and_natural_loops() {
        let mut function = Function::new("loop", vec![Type::Bool], Vec::new());
        let header = function.create_empty_block("header", BlockLayout::Hot);
        let body = function.create_empty_block("body", BlockLayout::Hot);
        let exit = function.create_empty_block("exit", BlockLayout::Hot);
        function.set_terminator(
            function.entry,
            Terminator::jump(header),
        );
        function.set_terminator(
            header,
            Terminator::branch(function.parameter(0), body, exit),
        );
        function.set_terminator(
            body,
            Terminator::jump(header),
        );
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
        assert_eq!(loops[0].blocks, HashSet::from([header, body]));
    }
}
