/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Control flow graph of an executable's bytecode.
//!
//! Blocks start at the entry, at jump targets, after terminators, and at the
//! start, end and target of every exception handler range, so every block lies
//! entirely inside or outside each handler range. Edges come in two kinds:
//! normal control flow (jumps and fallthrough) and exceptional control flow
//! from a block inside a handler range to the handler.
//!
//! Generator continuations (the labels of `Await`, `Yield` and
//! `YieldIteratorResult`) are treated as ordinary successors: the frame
//! survives the suspension, so values live into the continuation stay live.

use super::DecodedInstruction;
use super::ExceptionHandler;
use super::FrameLayout;
use super::RESERVED_REGISTER_COUNT;
use crate::bitset::BitSet;
use std::ops::Range;

pub type BlockIndex = usize;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BasicBlock {
    /// Byte offset of the first instruction.
    pub start_pc: u32,
    /// Byte offset one past the last instruction.
    pub end_pc: u32,
    /// Indices into the decoded instruction list.
    pub instructions: Range<usize>,
    /// Normal control flow successors, without duplicates.
    pub successors: Vec<BlockIndex>,
    /// The block that receives exceptions thrown by instructions in this block.
    pub exception_handler: Option<BlockIndex>,
    /// Normal control flow predecessors, without duplicates.
    pub predecessors: Vec<BlockIndex>,
    /// Blocks whose exceptions are handled by this block.
    pub exception_predecessors: Vec<BlockIndex>,
    /// Position in `Cfg::reverse_post_order`, or `None` if unreachable.
    pub rpo_number: Option<u32>,
    /// Index into `Cfg::loops` if this block is a loop header.
    pub loop_index: Option<usize>,
}

impl BasicBlock {
    pub fn predecessor_count(&self) -> usize {
        self.predecessors.len()
    }

    pub fn is_loop_header(&self) -> bool {
        self.loop_index.is_some()
    }
}

/// A loop: a header (the target of one or more back edges) and its body.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Loop {
    pub header: BlockIndex,
    /// Sources of the back edges to the header.
    pub back_edge_sources: Vec<BlockIndex>,
    /// Blocks of the loop, including the header and nested loops, in increasing order.
    pub blocks: Vec<BlockIndex>,
    /// Tracked slots (see `FrameLayout::tracked_index()`) written by any
    /// instruction in the loop. The reserved registers are always included,
    /// since some instructions write them implicitly.
    pub assigned_slots: BitSet,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CfgError {
    /// A jump at `pc` targets an offset that is not an instruction boundary.
    InvalidJumpTarget { pc: u32, target: u32 },
    /// An exception handler range does not start, end, or land on instruction boundaries.
    InvalidExceptionHandler { handler_index: usize },
}

#[derive(Debug, Clone)]
pub struct Cfg {
    /// Blocks in bytecode order. Block 0 is the entry.
    pub blocks: Vec<BasicBlock>,
    /// Reachable blocks in reverse post order, starting with the entry.
    pub reverse_post_order: Vec<BlockIndex>,
    pub loops: Vec<Loop>,
}

impl Cfg {
    pub fn new(
        instructions: &[DecodedInstruction],
        exception_handlers: &[ExceptionHandler],
        layout: &FrameLayout,
    ) -> Result<Self, CfgError> {
        let mut cfg = Self {
            blocks: Vec::new(),
            reverse_post_order: Vec::new(),
            loops: Vec::new(),
        };
        if instructions.is_empty() {
            return Ok(cfg);
        }
        cfg.find_blocks(instructions, exception_handlers)?;
        cfg.connect_blocks(instructions, exception_handlers)?;
        let back_edges = cfg.compute_reverse_post_order();
        cfg.find_loops(instructions, layout, back_edges);
        Ok(cfg)
    }

    /// The block containing the instruction at `pc`.
    pub fn block_for_pc(&self, pc: u32) -> Option<BlockIndex> {
        let index = self.blocks.partition_point(|block| block.start_pc <= pc);
        let block = index.checked_sub(1)?;
        (pc < self.blocks[block].end_pc).then_some(block)
    }

    fn find_blocks(
        &mut self,
        instructions: &[DecodedInstruction],
        exception_handlers: &[ExceptionHandler],
    ) -> Result<(), CfgError> {
        let end_pc = instructions
            .last()
            .expect("Cfg::new() handles empty bytecode")
            .next_pc();
        let instruction_index_for_pc = |pc: u32| {
            instructions
                .binary_search_by_key(&pc, |instruction| instruction.pc)
                .ok()
        };

        let mut is_leader = vec![false; instructions.len()];
        is_leader[0] = true;
        for (index, instruction) in instructions.iter().enumerate() {
            let mut result = Ok(());
            instruction.instruction.for_each_jump_target(|target| {
                if let Some(target_index) = instruction_index_for_pc(target.0) {
                    is_leader[target_index] = true;
                } else {
                    result = Err(CfgError::InvalidJumpTarget {
                        pc: instruction.pc,
                        target: target.0,
                    });
                }
            });
            result?;
            if instruction.instruction.is_terminator() && index + 1 < instructions.len() {
                is_leader[index + 1] = true;
            }
        }
        for (handler_index, handler) in exception_handlers.iter().enumerate() {
            let error = CfgError::InvalidExceptionHandler { handler_index };
            if handler.start_offset > handler.end_offset {
                return Err(error);
            }
            for (offset, may_be_end) in [
                (handler.start_offset, false),
                (handler.end_offset, true),
                (handler.handler_offset, false),
            ] {
                if may_be_end && offset == end_pc {
                    continue;
                }
                let index = instruction_index_for_pc(offset).ok_or(error)?;
                is_leader[index] = true;
            }
        }

        let mut start = 0;
        for index in 1..=instructions.len() {
            if index < instructions.len() && !is_leader[index] {
                continue;
            }
            self.blocks.push(BasicBlock {
                start_pc: instructions[start].pc,
                end_pc: instructions[index - 1].next_pc(),
                instructions: start..index,
                successors: Vec::new(),
                exception_handler: None,
                predecessors: Vec::new(),
                exception_predecessors: Vec::new(),
                rpo_number: None,
                loop_index: None,
            });
            start = index;
        }
        Ok(())
    }

    fn connect_blocks(
        &mut self,
        instructions: &[DecodedInstruction],
        exception_handlers: &[ExceptionHandler],
    ) -> Result<(), CfgError> {
        for block_index in 0..self.blocks.len() {
            let block = &self.blocks[block_index];
            let last = &instructions[block.instructions.end - 1];
            let mut successors = Vec::new();
            last.instruction
                .for_each_jump_target(|target| successors.push(target.0));
            if last.instruction.opcode().can_fall_through() && block_index + 1 < self.blocks.len() {
                successors.push(last.next_pc());
            }
            let mut successor_blocks = Vec::with_capacity(successors.len());
            for target in successors {
                let successor = self
                    .block_for_pc(target)
                    .ok_or(CfgError::InvalidJumpTarget { pc: last.pc, target })?;
                if !successor_blocks.contains(&successor) {
                    successor_blocks.push(successor);
                }
            }

            let start_pc = block.start_pc;
            let exception_handler = exception_handlers
                .iter()
                .enumerate()
                .find(|(_, handler)| handler.start_offset <= start_pc && start_pc < handler.end_offset)
                .map(|(handler_index, handler)| {
                    self.block_for_pc(handler.handler_offset)
                        .ok_or(CfgError::InvalidExceptionHandler { handler_index })
                })
                .transpose()?;

            for successor in &successor_blocks {
                self.blocks[*successor].predecessors.push(block_index);
            }
            if let Some(handler) = exception_handler {
                self.blocks[handler].exception_predecessors.push(block_index);
            }
            let block = &mut self.blocks[block_index];
            block.successors = successor_blocks;
            block.exception_handler = exception_handler;
        }
        Ok(())
    }

    /// All successors of a block, normal and exceptional.
    fn all_successors(&self, block: BlockIndex) -> impl Iterator<Item = BlockIndex> + '_ {
        let block = &self.blocks[block];
        block.successors.iter().copied().chain(block.exception_handler)
    }

    /// Computes the reverse post order with a depth first search from the
    /// entry and returns the back edges (edges to a block on the DFS stack).
    fn compute_reverse_post_order(&mut self) -> Vec<(BlockIndex, BlockIndex)> {
        #[derive(Clone, Copy, PartialEq, Eq)]
        enum State {
            Unvisited,
            OnStack,
            Done,
        }
        let mut state = vec![State::Unvisited; self.blocks.len()];
        let mut post_order = Vec::with_capacity(self.blocks.len());
        let mut back_edges = Vec::new();
        // Each stack entry is a block and the index of the next successor to visit.
        let mut stack = vec![(0, 0)];
        state[0] = State::OnStack;
        while let Some((block, next_successor)) = stack.last_mut() {
            let block = *block;
            let successor = self.all_successors(block).nth(*next_successor);
            *next_successor += 1;
            match successor {
                Some(successor) => match state[successor] {
                    State::Unvisited => {
                        state[successor] = State::OnStack;
                        stack.push((successor, 0));
                    }
                    State::OnStack => back_edges.push((block, successor)),
                    State::Done => {}
                },
                None => {
                    state[block] = State::Done;
                    post_order.push(block);
                    stack.pop();
                }
            }
        }
        self.reverse_post_order = post_order.into_iter().rev().collect();
        for (rpo_number, block) in self.reverse_post_order.iter().enumerate() {
            self.blocks[*block].rpo_number = Some(u32::try_from(rpo_number).expect("block count fits in u32"));
        }
        back_edges
    }

    fn find_loops(
        &mut self,
        instructions: &[DecodedInstruction],
        layout: &FrameLayout,
        mut back_edges: Vec<(BlockIndex, BlockIndex)>,
    ) {
        back_edges.sort_by_key(|(source, header)| (self.blocks[*header].rpo_number, *source));
        let mut loops: Vec<Loop> = Vec::new();
        for (source, header) in back_edges {
            if let Some(existing) = loops.last_mut().filter(|existing| existing.header == header) {
                existing.back_edge_sources.push(source);
            } else {
                loops.push(Loop {
                    header,
                    back_edge_sources: vec![source],
                    blocks: Vec::new(),
                    assigned_slots: BitSet::new(layout.tracked_slot_count()),
                });
            }
        }

        let reserved_registers = 0..(RESERVED_REGISTER_COUNT.min(layout.registers_and_locals_count) as usize);
        for (loop_index, current_loop) in loops.iter_mut().enumerate() {
            // The natural loop: the header plus every reachable block that
            // reaches a back edge source without passing through the header.
            let mut in_loop = vec![false; self.blocks.len()];
            in_loop[current_loop.header] = true;
            let mut worklist = current_loop.back_edge_sources.clone();
            while let Some(block) = worklist.pop() {
                if in_loop[block] || self.blocks[block].rpo_number.is_none() {
                    continue;
                }
                in_loop[block] = true;
                let block = &self.blocks[block];
                worklist.extend(block.predecessors.iter().chain(&block.exception_predecessors));
            }
            current_loop.blocks = (0..self.blocks.len()).filter(|block| in_loop[*block]).collect();

            current_loop.assigned_slots.insert_range(reserved_registers.clone());
            for block in &current_loop.blocks {
                for instruction in &instructions[self.blocks[*block].instructions.clone()] {
                    instruction.instruction.for_each_operand(|operand, role| {
                        if role.is_written()
                            && let Some(slot) = layout.tracked_index(operand)
                        {
                            current_loop.assigned_slots.insert(slot);
                        }
                    });
                }
            }
            self.blocks[current_loop.header].loop_index = Some(loop_index);
        }
        self.loops = loops;
    }
}

#[cfg(test)]
mod tests {
    use super::super::test_support::*;
    use super::*;
    use crate::bytecode::Instruction;
    use crate::bytecode::Label;

    #[test]
    fn straight_line_code_is_one_block() {
        let program = assemble(|_| {
            vec![
                Instruction::Mov { dst: r(5), src: c(0) },
                Instruction::Mov { dst: r(6), src: r(5) },
                Instruction::Return { value: r(6) },
            ]
        });
        let cfg = program.cfg();
        assert_eq!(cfg.blocks.len(), 1);
        assert_eq!(cfg.blocks[0].instructions, 0..3);
        assert!(cfg.blocks[0].successors.is_empty());
        assert_eq!(cfg.reverse_post_order, [0]);
        assert!(cfg.loops.is_empty());
    }

    #[test]
    fn if_else_splits_at_targets_and_after_terminators() {
        let program = assemble(|label| {
            vec![
                Instruction::JumpIf {
                    condition: r(5),
                    true_target: label(1),
                    false_target: label(3),
                },
                Instruction::Mov { dst: r(6), src: c(0) },
                Instruction::Jump { target: label(4) },
                Instruction::Mov { dst: r(6), src: c(1) },
                Instruction::Return { value: r(6) },
            ]
        });
        let cfg = program.cfg();
        let instruction_ranges = cfg
            .blocks
            .iter()
            .map(|block| block.instructions.clone())
            .collect::<Vec<_>>();
        assert_eq!(instruction_ranges, [0..1, 1..3, 3..4, 4..5]);
        assert_eq!(cfg.blocks[0].successors, [1, 2]);
        assert_eq!(cfg.blocks[1].successors, [3]);
        // The else block falls through into the join block.
        assert_eq!(cfg.blocks[2].successors, [3]);
        assert_eq!(cfg.blocks[3].predecessor_count(), 2);
        assert_eq!(cfg.blocks[0].predecessor_count(), 0);
        assert_eq!(cfg.reverse_post_order.first(), Some(&0));
        assert_eq!(cfg.reverse_post_order.last(), Some(&3));
        assert_eq!(cfg.reverse_post_order.len(), 4);
        assert!(cfg.loops.is_empty());
        assert_eq!(cfg.block_for_pc(program.offsets[2]), Some(1));
        assert_eq!(cfg.block_for_pc(program.offsets[3]), Some(2));
    }

    #[test]
    fn loop_header_is_target_of_back_edge() {
        let program = assemble(|label| {
            vec![
                Instruction::Mov { dst: r(5), src: c(0) },
                Instruction::JumpLessThan {
                    arith_feedback: 0,
                    lhs: r(5),
                    rhs: c(1),
                    true_target: label(2),
                    false_target: label(5),
                },
                Instruction::Increment {
                    arith_feedback: 0,
                    dst: r(5),
                },
                Instruction::Mov { dst: l(0), src: r(5) },
                Instruction::Jump { target: label(1) },
                Instruction::Return { value: l(1) },
            ]
        });
        let cfg = program.cfg();
        let layout = test_layout();
        assert_eq!(cfg.blocks.len(), 4);
        assert_eq!(cfg.loops.len(), 1);
        let header_loop = &cfg.loops[0];
        assert_eq!(header_loop.header, 1);
        assert!(cfg.blocks[1].is_loop_header());
        assert_eq!(header_loop.back_edge_sources, [2]);
        assert_eq!(header_loop.blocks, [1, 2]);
        assert_eq!(cfg.blocks[1].predecessor_count(), 2);
        let assigned = header_loop.assigned_slots.iter().collect::<Vec<_>>();
        let local0 = layout.tracked_index(l(0)).unwrap();
        assert_eq!(assigned, [0, 1, 2, 3, 4, 5, local0]);
        assert_eq!(cfg.reverse_post_order, [0, 1, 3, 2]);
    }

    #[test]
    fn nested_loops_include_inner_assignments() {
        let program = assemble(|label| {
            vec![
                Instruction::JumpTrue {
                    condition: r(5),
                    target: label(3),
                },
                Instruction::JumpTrue {
                    condition: r(6),
                    target: label(0),
                },
                Instruction::Return { value: r(5) },
                Instruction::Mov { dst: r(7), src: c(0) },
                Instruction::JumpTrue {
                    condition: r(7),
                    target: label(3),
                },
                Instruction::Jump { target: label(0) },
            ]
        });
        let cfg = program.cfg();
        let layout = test_layout();
        // JumpTrue continues with the next instruction when not taken.
        assert_eq!(cfg.blocks[0].successors, [3, 1]);
        assert_eq!(cfg.loops.len(), 2);
        let outer = &cfg.loops[0];
        let inner = &cfg.loops[1];
        assert_eq!(cfg.blocks[outer.header].instructions, 0..1);
        assert_eq!(cfg.blocks[inner.header].instructions, 3..5);
        assert!(outer.blocks.contains(&inner.header));
        assert!(outer.assigned_slots.contains(layout.tracked_index(r(7)).unwrap()));
        assert!(!outer.assigned_slots.contains(layout.tracked_index(r(6)).unwrap()));
    }

    #[test]
    fn exception_handler_ranges_split_blocks_and_add_exception_edges() {
        let program = assemble_with_handlers(
            |label| {
                vec![
                    Instruction::Mov { dst: r(5), src: c(0) },
                    Instruction::Mov { dst: r(6), src: a(0) },
                    Instruction::Throw { src: r(6) },
                    Instruction::Catch { dst: r(7) },
                    Instruction::Return { value: r(5) },
                    Instruction::Jump { target: label(0) },
                ]
            },
            &[(1, 3, 3)],
        );
        let cfg = program.cfg();
        let instruction_ranges = cfg
            .blocks
            .iter()
            .map(|block| block.instructions.clone())
            .collect::<Vec<_>>();
        assert_eq!(instruction_ranges, [0..1, 1..3, 3..5, 5..6]);
        assert_eq!(cfg.blocks[0].successors, [1]);
        assert!(cfg.blocks[1].successors.is_empty());
        assert_eq!(cfg.blocks[1].exception_handler, Some(2));
        assert_eq!(cfg.blocks[2].exception_predecessors, [1]);
        assert_eq!(cfg.blocks[2].predecessor_count(), 0);
        assert_eq!(cfg.reverse_post_order, [0, 1, 2]);
        assert_eq!(cfg.blocks[3].rpo_number, None);
    }

    #[test]
    fn generator_continuations_are_successors() {
        let program = assemble(|label| {
            vec![
                Instruction::Yield {
                    continuation_label: Some(label(1)),
                    value: r(5),
                },
                Instruction::Return { value: r(5) },
            ]
        });
        let cfg = program.cfg();
        assert_eq!(cfg.blocks.len(), 2);
        assert_eq!(cfg.blocks[0].successors, [1]);
    }

    #[test]
    fn rejects_jumps_into_the_middle_of_instructions() {
        let program = assemble(|label| {
            vec![
                Instruction::Jump {
                    target: Label(label(1).0 + 4),
                },
                Instruction::Return { value: r(5) },
            ]
        });
        let instructions = crate::bytecode::decode_all(&program.bytes).unwrap();
        let error = Cfg::new(&instructions, &[], &test_layout()).unwrap_err();
        assert!(matches!(error, CfgError::InvalidJumpTarget { pc: 0, .. }));
    }
}
