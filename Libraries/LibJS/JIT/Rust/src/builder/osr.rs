/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! On-stack replacement entries.
//!
//! The interpreter counts loop iterations at the loop back edges the bytecode
//! generator marks with the `*Loop` jump opcodes, and once an executable runs
//! out of tier-up budget there, it can continue a frame that is running in a
//! loop in compiled code. It enters at the back edge instruction, with every
//! value in the frame.
//!
//! Each back edge gets an entry block without predecessors that loads the
//! slots live into the back edge instruction from the frame and then runs the
//! instruction itself, or jumps to its block where the instruction starts it
//! (as the tests at the bottom of loops do). Where the instruction jumps to the
//! update step of a for loop, which leads to the loop's header, the entry runs
//! that step itself instead. Its edges are forward edges into the loop and the
//! loop exit, so the values it loaded meet the other paths in phis.
//!
//! Generators and async functions resume the same way: a suspended frame holds
//! every value live at the continuation it resumes at (see `build_suspend()`),
//! so each continuation of a `Yield`, `YieldIteratorResult` or `Await` gets an
//! entry block that loads them and jumps to the continuation's block, which
//! compiled code reaches from nowhere else. The runtime enters there like at
//! an on-stack replacement entry.
//!
//! Exception handlers are entered the same way. The frame state of an
//! instruction that throws lists every value live into its handler. Each
//! handler that caught an exception before the compile gets an entry block
//! after its `Catch`, where compiled code continues after writing that frame
//! state to the frame and running the `Catch` itself, or the interpreter
//! after running the `Catch` (see `ARCHITECTURE.md`, section 4.2).

use super::AbstractFrame;
use super::Flow;
use super::GraphBuilder;
use crate::CompileFailure;
use crate::bytecode::Instruction;
use crate::bytecode::Label;
use crate::bytecode::OpCode;
use crate::bytecode::RESERVED_REGISTER_COUNT;
use crate::bytecode::cfg::BlockIndex;
use crate::bytecode::cfg::Loop;

/// Whether instructions with this opcode are loop back edges, which count
/// loop iterations in the interpreter.
pub(crate) fn is_loop_back_edge(opcode: OpCode) -> bool {
    use OpCode as O;
    matches!(
        opcode,
        O::JumpLoop
            | O::JumpIfLoop
            | O::JumpTrueLoop
            | O::JumpFalseLoop
            | O::JumpLessThanLoop
            | O::JumpGreaterThanLoop
            | O::JumpLessThanEqualsLoop
            | O::JumpGreaterThanEqualsLoop
            | O::JumpLooselyEqualsLoop
            | O::JumpLooselyInequalsLoop
            | O::JumpStrictlyEqualsLoop
            | O::JumpStrictlyInequalsLoop
            | O::JumpLessThanLoopRhsInt32
            | O::JumpLessThanEqualsLoopRhsInt32
            | O::JumpGreaterThanLoopRhsInt32
            | O::JumpGreaterThanEqualsLoopRhsInt32
            | O::JumpStrictlyEqualsLoopRhsInt32
            | O::JumpStrictlyInequalsLoopRhsInt32
            | O::JumpLooselyEqualsLoopRhsInt32
            | O::JumpLooselyInequalsLoopRhsInt32
    )
}

impl GraphBuilder<'_> {
    /// Builds an entry block for every reachable loop back edge of the
    /// compiled function that ran, before any of its bytecode blocks is
    /// started.
    pub(super) fn build_osr_entries(&mut self) -> Result<(), CompileFailure> {
        debug_assert!(!self.function.is_virtual());
        for index in 0..self.function.instructions.len() {
            let pc = self.function.instructions[index].pc;
            if !is_loop_back_edge(self.function.instructions[index].instruction.opcode())
                || !(self.has_run)(pc)
                || (self.snapshot.options.osr_pc != Some(pc) && !self.snapshot.options.stress.osr_at_every_loop)
            {
                continue;
            }
            // NB: Back edges in unreachable code lead to blocks that are
            //     never built.
            let reachable = self
                .function
                .cfg
                .block_for_pc(pc)
                .is_some_and(|block| self.function.cfg.blocks[block].rpo_number.is_some());
            if !reachable {
                continue;
            }
            let block = self.start_new_block(Vec::new());
            self.frame = AbstractFrame::in_memory(self.function.layout.tracked_slot_count());
            self.function.instruction_index = index;
            self.function.pc = pc;
            self.function.eager_frame_state = None;
            let live = self.function.liveness.live_in(index);
            for slot in live.iter() {
                if slot >= RESERVED_REGISTER_COUNT as usize {
                    let operand = self.function.layout.operand_for_tracked_index(slot);
                    self.read_tracked(slot, operand);
                }
            }
            // NB: A back edge that starts its block is entered through
            //     that block, so that loops with their test at the bottom
            //     are entered at their header like from the function entry.
            //     Unconditional back edges jump to their loop's header
            //     directly instead, since entering a block of the loop
            //     other than its header would make the loop irreducible.
            //     Those of loops whose update step has a block of its own
            //     (like for loops with a continue) jump to that step, which
            //     the entry runs itself before it continues at the header.
            let opcode = self.function.instructions[index].instruction.opcode();
            let own_block = self
                .function
                .cfg
                .block_for_pc(pc)
                .filter(|block| opcode != OpCode::JumpLoop && self.function.cfg.blocks[*block].start_pc == pc);
            let mut targets = Vec::new();
            if let Some(own_block) = own_block {
                self.end_with_jump(own_block)?;
                targets.push(own_block);
            } else if let Some(update_block) = self.update_block_of_back_edge(index) {
                self.build_bytecode_block(update_block)?;
                targets.extend(self.function.cfg.blocks[update_block].successors.iter().copied());
            } else {
                match self.build_instruction()? {
                    Flow::Ended => {}
                    Flow::Continue => {
                        return Err(CompileFailure::InvalidBytecode {
                            pc,
                            reason: "loop back edge that does not end its block",
                        });
                    }
                }
                self.function.instructions[index]
                    .instruction
                    .for_each_jump_target(|label| {
                        targets.extend(self.function.cfg.block_for_pc(label.0));
                    });
            }
            self.graph.osr_entries.push((pc, block));
            self.record_osr_loop_back_edges(pc);
            self.note_loops_entered_at(&targets);
        }
        Ok(())
    }

    /// Builds an entry block for every continuation of a suspension of a
    /// generator or async function in the compiled function, after the
    /// on-stack replacement entries, before any of its bytecode blocks is
    /// started.
    pub(super) fn build_resume_entries(&mut self) -> Result<(), CompileFailure> {
        debug_assert!(!self.function.is_virtual());
        for index in 0..self.function.instructions.len() {
            let (Instruction::Yield {
                continuation_label: Some(continuation),
                ..
            }
            | Instruction::YieldIteratorResult {
                continuation_label: continuation,
                ..
            }
            | Instruction::Await {
                continuation_label: continuation,
                ..
            }) = self.function.instructions[index].instruction
            else {
                continue;
            };
            let pc = continuation.0;
            let target = self.block_for_label(continuation)?;
            let target_block = &self.function.cfg.blocks[target];
            if target_block.start_pc != pc {
                return Err(CompileFailure::InvalidBytecode {
                    pc,
                    reason: "continuation that does not start a block",
                });
            }
            if target_block.rpo_number.is_some() {
                self.build_entry_into_block(target)?;
            }
        }
        Ok(())
    }

    /// Builds an entry block for every exception handler of the compiled
    /// function that caught an exception and whose handler range is
    /// reachable, after the resume entries, before any of its bytecode blocks
    /// is started. Handlers start with a `Catch`, which the interpreter runs:
    /// compiled code never builds it, and is entered after it, where the
    /// handler's block continues. Handlers that never caught anything get no
    /// entry, which would merge into the paths after them.
    pub(super) fn build_catch_entries(&mut self) -> Result<(), CompileFailure> {
        debug_assert!(!self.function.is_virtual());
        for handler in 0..self.function.cfg.blocks.len() {
            let handler_block = &self.function.cfg.blocks[handler];
            let reachable = handler_block
                .exception_predecessors
                .iter()
                .any(|predecessor| self.function.cfg.blocks[*predecessor].rpo_number.is_some());
            let catch = &self.function.instructions[handler_block.instructions.start];
            if !reachable || catch.instruction.opcode() != OpCode::Catch {
                continue;
            }
            if !(self.has_run)(catch.pc) {
                self.graph.uncaught_handlers.push(catch.pc);
                continue;
            }
            // NB: The Catch ends its block, which falls through to the block
            //     that continues the handler.
            let Ok(target) = self.block_for_label(Label(catch.next_pc())) else {
                continue;
            };
            self.build_entry_into_block(target)?;
        }
        Ok(())
    }

    /// Builds an entry block for a frame whose memory holds every value live
    /// at the start of `target`: it loads them and jumps there. The runtime
    /// enters it like an on-stack replacement entry.
    fn build_entry_into_block(&mut self, target: BlockIndex) -> Result<(), CompileFailure> {
        let target_block = &self.function.cfg.blocks[target];
        let pc = target_block.start_pc;
        // NB: An on-stack replacement entry at a back edge that starts the
        //     block enters the frame there just the same.
        if self.graph.osr_entries.iter().any(|(entry_pc, _)| *entry_pc == pc) {
            return Ok(());
        }
        let block = self.start_new_block(Vec::new());
        self.frame = AbstractFrame::in_memory(self.function.layout.tracked_slot_count());
        let start = target_block.instructions.start;
        self.function.instruction_index = start;
        self.function.pc = pc;
        self.function.eager_frame_state = None;
        let live = self.function.liveness.live_in(start);
        for slot in live.iter() {
            if slot >= RESERVED_REGISTER_COUNT as usize {
                let operand = self.function.layout.operand_for_tracked_index(slot);
                self.read_tracked(slot, operand);
            }
        }
        self.end_with_jump(target)?;
        self.graph.osr_entries.push((pc, block));
        self.note_loops_entered_at(&[target]);
        Ok(())
    }

    /// Notes the loops that an entry leads into other than at their header,
    /// over its edges to `targets`, which may only be reached from there over
    /// their back edges.
    fn note_loops_entered_at(&mut self, targets: &[BlockIndex]) {
        for &target in targets {
            for current_loop in &self.function.cfg.loops {
                if current_loop.blocks.contains(&target) && current_loop.header != target {
                    self.function.osr_loop_headers[current_loop.header] = true;
                }
            }
        }
    }

    /// The block that the unconditional back edge at instruction `index` jumps
    /// to, if it is no loop header and leads to one and nowhere else.
    fn update_block_of_back_edge(&self, index: usize) -> Option<BlockIndex> {
        let instruction = &self.function.instructions[index].instruction;
        if instruction.opcode() != OpCode::JumpLoop {
            return None;
        }
        let mut target = None;
        instruction.for_each_jump_target(|label| target = self.function.cfg.block_for_pc(label.0));
        let cfg = &self.function.cfg;
        let block = &cfg.blocks[target?];
        match block.successors.as_slice() {
            [successor] if !block.is_loop_header() && cfg.blocks[*successor].is_loop_header() => target,
            _ => None,
        }
    }

    /// Records the loop back edges in the loop of the back edge at `pc`, which
    /// has an entry. A frame running in the interpreter at one of them reaches
    /// the entry once its inner loops finish an iteration. Also records the
    /// other back edges of the loops around that loop, from where a frame may
    /// reach the entry on a later iteration.
    fn record_osr_loop_back_edges(&mut self, pc: u32) {
        let cfg = &self.function.cfg;
        let Some(back_edge_block) = cfg.block_for_pc(pc) else {
            return;
        };
        // NB: The loop of a back edge is the innermost loop it is in. Its
        //     header may be the block of the back edge itself, like the test
        //     at the bottom of a for loop.
        let Some(entered_loop) = cfg
            .loops
            .iter()
            .filter(|current_loop| current_loop.blocks.contains(&back_edge_block))
            .min_by_key(|current_loop| current_loop.blocks.len())
        else {
            return;
        };
        let back_edges_in = |current_loop: &Loop| {
            current_loop
                .blocks
                .iter()
                .flat_map(|&block| &self.function.instructions[cfg.blocks[block].instructions.clone()])
                .filter(|instruction| is_loop_back_edge(instruction.instruction.opcode()))
                .map(|instruction| instruction.pc)
                .collect::<Vec<_>>()
        };
        for back_edge_pc in back_edges_in(entered_loop) {
            if !self.graph.osr_loop_back_edges.contains(&back_edge_pc) {
                self.graph.osr_loop_back_edges.push(back_edge_pc);
            }
        }
        for outer_loop in cfg.loops.iter().filter(|current_loop| {
            current_loop.blocks.contains(&back_edge_block) && current_loop.blocks.len() > entered_loop.blocks.len()
        }) {
            for back_edge_pc in back_edges_in(outer_loop) {
                if !self.graph.osr_outer_loop_back_edges.contains(&back_edge_pc) {
                    self.graph.osr_outer_loop_back_edges.push(back_edge_pc);
                }
            }
        }
    }
}
