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
//! (as the tests at the bottom of loops do). Its edges are forward edges into
//! the loop and the loop exit, so the values it loaded meet the other paths in
//! phis.

use super::AbstractFrame;
use super::Flow;
use super::GraphBuilder;
use crate::CompileFailure;
use crate::bytecode::OpCode;
use crate::bytecode::RESERVED_REGISTER_COUNT;

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
            let live = self.function.liveness.live_in(index).clone();
            for slot in live.iter() {
                if slot >= RESERVED_REGISTER_COUNT as usize {
                    let operand = self.function.layout.operand_for_tracked_index(slot);
                    self.read_tracked(slot, operand);
                }
            }
            // NB: A back edge that starts its block is entered through
            //     that block, so that loops with their test at the bottom
            //     are entered at their header like from the function entry.
            let own_block = self
                .function
                .cfg
                .block_for_pc(pc)
                .filter(|block| self.function.cfg.blocks[*block].start_pc == pc);
            let mut targets = Vec::new();
            if let Some(own_block) = own_block {
                self.end_with_jump(own_block)?;
                targets.push(own_block);
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

            // NB: The loops the entry leads into other than at their header may
            //     only be reached from here over their back edges.
            for target in targets {
                for current_loop in &self.function.cfg.loops {
                    if current_loop.blocks.contains(&target) && current_loop.header != target {
                        self.function.osr_loop_headers[current_loop.header] = true;
                    }
                }
            }
        }
        Ok(())
    }
}
