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
//! instructions read or write them implicitly. So are the arguments of a
//! function with a legacy `arguments`, which the code it calls can read.
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

/// The live set after every this many instructions of a block, counted from its end, is kept, and the sets of the
/// instructions in between are computed again from it when asked for.
const CHECKPOINT_INTERVAL: usize = 64;

/// The block of instructions in no block.
const NO_BLOCK: u32 = u32::MAX;

#[derive(Debug, Clone)]
pub struct Liveness {
    transfers: Transfers,
    /// The block of each instruction, or `NO_BLOCK`.
    block_of_instruction: Vec<u32>,
    /// For each instruction, the index into `checkpoints` of the nearest one at or after it in its block, which holds
    /// the instruction index and the slots live out of it.
    checkpoint_of_instruction: Vec<u32>,
    checkpoints: Vec<(usize, BitSet)>,
}

/// What turns the slots live out of an instruction into those live into it.
#[derive(Debug, Clone)]
struct Transfers {
    always_live: BitSet,
    block_live_in: Vec<BitSet>,
    /// The exception handler of each block.
    exception_handlers: Vec<Option<usize>>,
    /// The slots each instruction writes and then reads: those of instruction `i` are at
    /// `slots[slot_starts[i].0..slot_starts[i].1]` and `slots[slot_starts[i].1..slot_starts[i + 1].0]`.
    slots: Vec<u32>,
    slot_starts: Vec<(u32, u32)>,
}

impl Liveness {
    pub fn compute(
        instructions: &[DecodedInstruction],
        cfg: &Cfg,
        layout: &FrameLayout,
        has_legacy_arguments: bool,
    ) -> Self {
        let slot_count = layout.tracked_slot_count();
        let mut always_live = BitSet::new(slot_count);
        always_live.insert_range(0..(RESERVED_REGISTER_COUNT.min(layout.registers_and_locals_count) as usize));
        if has_legacy_arguments {
            always_live.insert_range(layout.registers_and_locals_count as usize..slot_count);
        }

        let mut slots = Vec::new();
        let mut slot_starts = Vec::with_capacity(instructions.len() + 1);
        for instruction in instructions {
            let start = slots.len() as u32;
            for_each_written_slot(instruction, layout, |slot| slots.push(slot as u32));
            let reads_start = slots.len() as u32;
            for_each_read_slot(instruction, layout, |slot| slots.push(slot as u32));
            slot_starts.push((start, reads_start));
        }
        slot_starts.push((slots.len() as u32, slots.len() as u32));

        // Upward exposed uses and definitions of each block.
        let mut block_uses = Vec::with_capacity(cfg.blocks.len());
        let mut block_definitions = Vec::with_capacity(cfg.blocks.len());
        for block in &cfg.blocks {
            let mut uses = BitSet::new(slot_count);
            let mut definitions = BitSet::new(slot_count);
            for index in block.instructions.clone().rev() {
                let (written, read) = instruction_slots(&slots, &slot_starts, index);
                for slot in written {
                    uses.remove(*slot as usize);
                    definitions.insert(*slot as usize);
                }
                for slot in read {
                    uses.insert(*slot as usize);
                }
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

        // NB: Keeping the sets of every instruction would take memory (and time) quadratic in the size of code that
        //     keeps many slots live over many instructions, like a large array literal of arrays.
        let transfers = Transfers {
            always_live,
            block_live_in,
            exception_handlers: cfg.blocks.iter().map(|block| block.exception_handler).collect(),
            slots,
            slot_starts,
        };
        let mut block_of_instruction = vec![NO_BLOCK; instructions.len()];
        let mut checkpoint_of_instruction = vec![0; instructions.len()];
        let mut checkpoints = Vec::new();
        for (block_index, block) in cfg.blocks.iter().enumerate() {
            let mut live = block_live_out(block, &transfers.block_live_in, &transfers.always_live);
            for (offset, index) in block.instructions.clone().rev().enumerate() {
                block_of_instruction[index] = block_index as u32;
                if offset % CHECKPOINT_INTERVAL == 0 {
                    checkpoints.push((index, live.clone()));
                }
                checkpoint_of_instruction[index] = (checkpoints.len() - 1) as u32;
                transfers.transfer(index, block_index, &mut live);
            }
        }
        Self {
            transfers,
            block_of_instruction,
            checkpoint_of_instruction,
            checkpoints,
        }
    }

    /// Slots live before the instruction with the given index executes.
    pub fn live_in(&self, instruction_index: usize) -> BitSet {
        let mut live = self.live_out(instruction_index);
        let block = self.block_of_instruction[instruction_index];
        if block != NO_BLOCK {
            self.transfers.transfer(instruction_index, block as usize, &mut live);
        }
        live
    }

    /// Slots live after the instruction with the given index executes,
    /// including slots live into its exception handler.
    pub fn live_out(&self, instruction_index: usize) -> BitSet {
        let block = self.block_of_instruction[instruction_index];
        if block == NO_BLOCK {
            return BitSet::new(self.transfers.always_live.capacity());
        }
        let (checkpoint_index, checkpoint) =
            &self.checkpoints[self.checkpoint_of_instruction[instruction_index] as usize];
        let mut live = checkpoint.clone();
        if instruction_index < *checkpoint_index {
            for index in (instruction_index + 1..=*checkpoint_index).rev() {
                self.transfers.transfer_slots(index, &mut live);
            }
            self.transfers.add_slots_live_throughout(block as usize, &mut live);
        }
        live
    }
}

impl Transfers {
    /// Turns the slots live out of an instruction into those live into it.
    fn transfer(&self, index: usize, block: usize, live: &mut BitSet) {
        self.transfer_slots(index, live);
        self.add_slots_live_throughout(block, live);
    }

    fn transfer_slots(&self, index: usize, live: &mut BitSet) {
        let (written, read) = instruction_slots(&self.slots, &self.slot_starts, index);
        for slot in written {
            live.remove(*slot as usize);
        }
        for slot in read {
            live.insert(*slot as usize);
        }
    }

    /// Adds the slots live into and out of every instruction of the block: those always live and those live into its
    /// exception handler. Transfers of several instructions in a row need to add them only once, at the end.
    fn add_slots_live_throughout(&self, block: usize, live: &mut BitSet) {
        if let Some(handler) = self.exception_handlers[block] {
            live.union_with(&self.block_live_in[handler]);
        }
        live.union_with(&self.always_live);
    }
}

fn instruction_slots<'a>(slots: &'a [u32], slot_starts: &[(u32, u32)], index: usize) -> (&'a [u32], &'a [u32]) {
    let (start, reads_start) = slot_starts[index];
    let end = slot_starts[index + 1].0;
    (
        &slots[start as usize..reads_start as usize],
        &slots[reads_start as usize..end as usize],
    )
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
                liveness: Liveness::compute(&instructions, &cfg, &layout, false),
                layout,
            }
        }

        /// The live in set of an instruction, without the always live reserved registers.
        fn live_in(&self, index: usize) -> Vec<Operand> {
            self.operands(&self.liveness.live_in(index))
        }

        fn live_out(&self, index: usize) -> Vec<Operand> {
            self.operands(&self.liveness.live_out(index))
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
    fn long_blocks_match_a_walk_over_every_instruction() {
        // NB: The sets of most instructions of a block this long are computed again from a checkpoint after them.
        let slots = [r(5), r(6), r(7), l(0), l(1)];
        let program = assemble(|_| {
            (0..3 * CHECKPOINT_INTERVAL + 5)
                .map(|index| Instruction::Add {
                    arith_feedback: 0,
                    dst: slots[index % 5],
                    lhs: slots[(index * 3 + 1) % 5],
                    rhs: if index % 7 == 0 {
                        a(0)
                    } else {
                        slots[(index * 2 + 3) % 5]
                    },
                })
                .chain([Instruction::Return { value: r(5) }])
                .collect()
        });
        let analysis = Analysis::new(&program);
        let instructions = program.instructions();
        let mut live = BitSet::new(analysis.layout.tracked_slot_count());
        live.insert_range(0..RESERVED_REGISTER_COUNT as usize);
        for index in (0..instructions.len()).rev() {
            assert_eq!(
                analysis.live_out(index),
                analysis.operands(&live),
                "live out of {index}"
            );
            for_each_written_slot(&instructions[index], &analysis.layout, |slot| live.remove(slot));
            for_each_read_slot(&instructions[index], &analysis.layout, |slot| live.insert(slot));
            assert_eq!(analysis.live_in(index), analysis.operands(&live), "live in of {index}");
        }
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
                    Instruction::Catch {
                        value_feedback: 0,
                        dst: r(7),
                    },
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
