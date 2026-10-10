/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Per-instruction liveness of a frame's registers, locals and arguments.
//!
//! Sets are indexed by `FrameLayout::tracked_index()`. Constants are read-only
//! and never live. The reserved registers (accumulator, exception, this value,
//! return value and saved lexical environment) are always live, since some
//! instructions read or write them implicitly.
//!
//! An instruction inside an exception handler range may throw before writing
//! its outputs, so everything live into the handler is live into, and out of,
//! every such instruction.

use super::DecodedInstruction;
use super::FrameLayout;
use super::RESERVED_REGISTER_COUNT;
use super::cfg::BasicBlock;
use super::cfg::Cfg;
use crate::bitset::BitSet;

#[derive(Debug, Clone)]
pub struct Liveness {
    live_in: Vec<BitSet>,
    live_out: Vec<BitSet>,
}

impl Liveness {
    pub fn compute(instructions: &[DecodedInstruction], cfg: &Cfg, layout: &FrameLayout) -> Self {
        let slot_count = layout.tracked_slot_count();
        let mut always_live = BitSet::new(slot_count);
        always_live.insert_range(0..(RESERVED_REGISTER_COUNT.min(layout.registers_and_locals_count) as usize));

        // Upward exposed uses and definitions of each block.
        let mut block_uses = Vec::with_capacity(cfg.blocks.len());
        let mut block_definitions = Vec::with_capacity(cfg.blocks.len());
        for block in &cfg.blocks {
            let mut uses = BitSet::new(slot_count);
            let mut definitions = BitSet::new(slot_count);
            for instruction in instructions[block.instructions.clone()].iter().rev() {
                for_each_written_slot(instruction, layout, |slot| {
                    uses.remove(slot);
                    definitions.insert(slot);
                });
                for_each_read_slot(instruction, layout, |slot| uses.insert(slot));
            }
            block_uses.push(uses);
            block_definitions.push(definitions);
        }

        // Iterate to a fixed point, visiting blocks in post order so that most
        // successors are processed before their predecessors.
        let mut block_live_in = vec![always_live.clone(); cfg.blocks.len()];
        let post_order = cfg
            .reverse_post_order
            .iter()
            .rev()
            .copied()
            .chain((0..cfg.blocks.len()).filter(|block| cfg.blocks[*block].rpo_number.is_none()))
            .collect::<Vec<_>>();
        let mut changed = true;
        while changed {
            changed = false;
            for &block_index in &post_order {
                let block = &cfg.blocks[block_index];
                let mut live_in = block_live_out(block, &block_live_in, &always_live);
                live_in.subtract(&block_definitions[block_index]);
                live_in.union_with(&block_uses[block_index]);
                if let Some(handler) = block.exception_handler {
                    live_in.union_with(&block_live_in[handler]);
                }
                live_in.union_with(&always_live);
                changed |= block_live_in[block_index].union_with(&live_in);
            }
        }

        // Expand the block results to every instruction.
        let empty = BitSet::new(slot_count);
        let mut live_in = vec![empty.clone(); instructions.len()];
        let mut live_out = vec![empty; instructions.len()];
        for block in &cfg.blocks {
            let handler_live_in = block.exception_handler.map(|handler| &block_live_in[handler]);
            let mut live = block_live_out(block, &block_live_in, &always_live);
            for index in block.instructions.clone().rev() {
                live_out[index] = live.clone();
                for_each_written_slot(&instructions[index], layout, |slot| live.remove(slot));
                for_each_read_slot(&instructions[index], layout, |slot| live.insert(slot));
                if let Some(handler_live_in) = handler_live_in {
                    live.union_with(handler_live_in);
                }
                live.union_with(&always_live);
                live_in[index] = live.clone();
            }
        }

        Self { live_in, live_out }
    }

    /// Slots live before the instruction with the given index executes.
    pub fn live_in(&self, instruction_index: usize) -> &BitSet {
        &self.live_in[instruction_index]
    }

    /// Slots live after the instruction with the given index executes,
    /// including slots live into its exception handler.
    pub fn live_out(&self, instruction_index: usize) -> &BitSet {
        &self.live_out[instruction_index]
    }
}

fn block_live_out(block: &BasicBlock, block_live_in: &[BitSet], always_live: &BitSet) -> BitSet {
    let mut live_out = always_live.clone();
    for successor in block.successors.iter().chain(&block.exception_handler) {
        live_out.union_with(&block_live_in[*successor]);
    }
    live_out
}

fn for_each_read_slot(instruction: &DecodedInstruction, layout: &FrameLayout, mut callback: impl FnMut(usize)) {
    instruction.instruction.for_each_operand(|operand, role| {
        if role.is_read()
            && let Some(slot) = layout.tracked_index(operand)
        {
            callback(slot);
        }
    });
}

fn for_each_written_slot(instruction: &DecodedInstruction, layout: &FrameLayout, mut callback: impl FnMut(usize)) {
    instruction.instruction.for_each_operand(|operand, role| {
        if role.is_written()
            && let Some(slot) = layout.tracked_index(operand)
        {
            callback(slot);
        }
    });
}

#[cfg(test)]
mod tests {
    use super::super::test_support::*;
    use super::*;
    use crate::bytecode::Instruction;
    use crate::bytecode::Operand;

    struct Analysis {
        liveness: Liveness,
        layout: FrameLayout,
    }

    impl Analysis {
        fn new(program: &Program) -> Self {
            let layout = test_layout();
            let instructions = program.instructions();
            let cfg = program.cfg();
            Self {
                liveness: Liveness::compute(&instructions, &cfg, &layout),
                layout,
            }
        }

        /// The live in set of an instruction, without the always live reserved registers.
        fn live_in(&self, index: usize) -> Vec<Operand> {
            self.operands(self.liveness.live_in(index))
        }

        fn live_out(&self, index: usize) -> Vec<Operand> {
            self.operands(self.liveness.live_out(index))
        }

        fn operands(&self, set: &BitSet) -> Vec<Operand> {
            for reserved in 0..RESERVED_REGISTER_COUNT as usize {
                assert!(set.contains(reserved), "reserved register {reserved} is not live");
            }
            set.iter()
                .filter(|slot| *slot >= RESERVED_REGISTER_COUNT as usize)
                .map(|slot| self.layout.operand_for_tracked_index(slot))
                .collect()
        }
    }

    #[test]
    fn straight_line() {
        let program = assemble(|_| {
            vec![
                Instruction::Mov { dst: r(5), src: c(0) },
                Instruction::Add {
                    arith_feedback: 0,
                    dst: r(6),
                    lhs: r(5),
                    rhs: a(0),
                },
                Instruction::Mov { dst: r(5), src: c(1) },
                Instruction::Return { value: r(6) },
            ]
        });
        let analysis = Analysis::new(&program);
        assert_eq!(analysis.live_in(0), [a(0)]);
        assert_eq!(analysis.live_out(0), [r(5), a(0)]);
        assert_eq!(analysis.live_in(1), [r(5), a(0)]);
        assert_eq!(analysis.live_out(1), [r(6)]);
        // The second write of r5 is dead.
        assert_eq!(analysis.live_in(2), [r(6)]);
        assert_eq!(analysis.live_out(2), [r(6)]);
        assert_eq!(analysis.live_in(3), [r(6)]);
        assert!(analysis.live_out(3).is_empty());
    }

    #[test]
    fn if_else() {
        let program = assemble(|label| {
            vec![
                Instruction::JumpIf {
                    condition: r(5),
                    true_target: label(1),
                    false_target: label(3),
                },
                Instruction::Mov { dst: l(0), src: r(6) },
                Instruction::Jump { target: label(4) },
                Instruction::Mov { dst: l(0), src: r(7) },
                Instruction::Add {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: l(0),
                    rhs: l(1),
                },
                Instruction::Return { value: r(5) },
            ]
        });
        let analysis = Analysis::new(&program);
        assert_eq!(analysis.live_in(0), [r(5), r(6), r(7), l(1)]);
        assert_eq!(analysis.live_out(0), [r(6), r(7), l(1)]);
        assert_eq!(analysis.live_in(1), [r(6), l(1)]);
        assert_eq!(analysis.live_out(2), [l(0), l(1)]);
        assert_eq!(analysis.live_in(3), [r(7), l(1)]);
        assert_eq!(analysis.live_in(4), [l(0), l(1)]);
        assert_eq!(analysis.live_out(4), [r(5)]);
    }

    #[test]
    fn loop_carried_values_stay_live_around_the_back_edge() {
        let program = assemble(|label| {
            vec![
                Instruction::Mov { dst: r(5), src: c(0) },
                Instruction::Mov { dst: l(0), src: c(0) },
                Instruction::JumpLessThan {
                    arith_feedback: 0,
                    lhs: r(5),
                    rhs: a(0),
                    true_target: label(3),
                    false_target: label(7),
                },
                Instruction::Add {
                    arith_feedback: 0,
                    dst: r(6),
                    lhs: l(0),
                    rhs: r(5),
                },
                Instruction::Mov { dst: l(0), src: r(6) },
                Instruction::Increment {
                    arith_feedback: 0,
                    dst: r(5),
                },
                Instruction::Jump { target: label(2) },
                Instruction::Return { value: l(0) },
            ]
        });
        let analysis = Analysis::new(&program);
        assert_eq!(analysis.live_in(0), [a(0)]);
        // At the loop header, the counter, the sum and the bound are live.
        assert_eq!(analysis.live_in(2), [r(5), l(0), a(0)]);
        assert_eq!(analysis.live_out(2), [r(5), l(0), a(0)]);
        // The temporary r6 only lives between its definition and use.
        assert_eq!(analysis.live_out(3), [r(5), r(6), a(0)]);
        assert_eq!(analysis.live_in(5), [r(5), l(0), a(0)]);
        assert_eq!(analysis.live_out(6), [r(5), l(0), a(0)]);
        assert_eq!(analysis.live_in(7), [l(0)]);
    }

    #[test]
    fn values_used_by_a_catch_handler_are_live_throughout_the_try_block() {
        let program = assemble_with_handlers(
            |_| {
                vec![
                    Instruction::Mov { dst: r(5), src: c(0) },
                    Instruction::Mov { dst: r(6), src: a(0) },
                    Instruction::Mov { dst: r(5), src: c(1) },
                    Instruction::Throw { src: r(6) },
                    Instruction::Catch { dst: r(7) },
                    Instruction::Add {
                        arith_feedback: 0,
                        dst: r(7),
                        lhs: r(7),
                        rhs: r(5),
                    },
                    Instruction::Return { value: r(7) },
                ]
            },
            &[(1, 4, 4)],
        );
        let analysis = Analysis::new(&program);
        // r5 is live into the handler, so its first value must survive until
        // the second write, and the second until the throw.
        assert_eq!(analysis.live_in(1), [r(5), a(0)]);
        assert_eq!(analysis.live_out(1), [r(5), r(6)]);
        assert_eq!(analysis.live_in(2), [r(5), r(6)]);
        assert_eq!(analysis.live_out(2), [r(5), r(6)]);
        assert_eq!(analysis.live_out(3), [r(5)]);
        assert_eq!(analysis.live_in(4), [r(5)]);
        assert_eq!(analysis.live_in(5), [r(5), r(7)]);
    }

    #[test]
    fn inout_operands_are_both_used_and_defined() {
        let program = assemble(|_| {
            vec![
                Instruction::Increment {
                    arith_feedback: 0,
                    dst: r(5),
                },
                Instruction::Return { value: r(5) },
            ]
        });
        let analysis = Analysis::new(&program);
        assert_eq!(analysis.live_in(0), [r(5)]);
        assert_eq!(analysis.live_out(0), [r(5)]);
    }
}
