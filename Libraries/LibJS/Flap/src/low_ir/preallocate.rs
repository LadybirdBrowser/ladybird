/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Target-aware instruction combining before register allocation.

use super::{Instruction, Operand, VirtualRegister, visit_virtual_registers, visit_virtual_registers_mut};
use crate::hash::{HashMap, HashSet};
use crate::intrinsic::{BranchOperation, IntegerBinaryOperation, IntegerSignedness};
use crate::target::description::{
    BinaryOperation, EqualityCondition, IntegerWidth, MemoryWidth, OperandKind, Operation, PairWidth,
};
use crate::types::InterpreterRegister;

/// Turn every read of the interpreter's own program counter into a dedicated
/// instruction so each backend can materialize it from whatever state it
/// actually keeps. AArch64 does not hold the program counter in a register: its
/// dispatch register holds `pb + pc` and is the only thing an advancing handler
/// updates, so copying the pinned register would read a stale value.
///
/// A handler that assigns `pc` itself is the exception. Its assignment lands in
/// the pinned register, and the terminator that dispatches on it reads that
/// register back, so uses in between must be left alone.
pub(crate) fn materialize_program_counter_reads(instructions: &mut Vec<Instruction>) -> Result<(), String> {
    let program_counter = Operand::InterpreterRegister(InterpreterRegister::ProgramCounter);
    let mut index = 0;
    let mut reads = 0;
    let mut is_assigned = false;
    while index < instructions.len() {
        let instruction = &instructions[index];
        let operand_count = instruction.operands.len();
        let description = instruction.opcode.description();
        let assigns_program_counter = (0..operand_count).any(|position| {
            instruction.operands[position] == program_counter
                && matches!(
                    description.operand_kind(position, operand_count),
                    Some(OperandKind::GprOut | OperandKind::RegisterOut)
                )
        });
        if is_assigned {
            // The assignment stays live until the terminator dispatches on it.
            is_assigned = !description.terminal;
            index += 1;
            continue;
        }
        if assigns_program_counter {
            is_assigned = true;
            index += 1;
            continue;
        }
        let is_copy_of_program_counter = matches!(
            instruction.opcode.operation(),
            Operation::Move(IntegerWidth::U32 | IntegerWidth::U64)
        ) && operand_count == 2
            && instruction.operands[1] == program_counter
            && instruction.operands[0] != program_counter;
        if is_copy_of_program_counter {
            instructions[index] = Instruction {
                opcode: Operation::LoadProgramCounter,
                operands: vec![instructions[index].operands[0].clone()],
            };
            index += 1;
            continue;
        }

        let mut read_positions = Vec::new();
        for position in 0..operand_count {
            if instruction.operands[position] != program_counter {
                continue;
            }
            match description.operand_kind(position, operand_count) {
                Some(
                    OperandKind::GprIn | OperandKind::RegisterIn | OperandKind::GprInOrImm | OperandKind::GprInOrMemory,
                ) => read_positions.push(position),
                Some(OperandKind::GprInOut) => {
                    return Err(format!(
                        "'{:?}' updates the program counter in place, which no target supports",
                        instruction.opcode.operation()
                    ));
                }
                _ => {}
            }
        }
        if read_positions.is_empty() {
            index += 1;
            continue;
        }

        let temporary = Operand::VirtualRegister(VirtualRegister::from(format!("program_counter_read_{reads}")));
        reads += 1;
        for position in read_positions {
            instructions[index].operands[position] = temporary.clone();
        }
        instructions.insert(
            index,
            Instruction {
                opcode: Operation::LoadProgramCounter,
                operands: vec![temporary],
            },
        );
        index += 2;
    }
    Ok(())
}

/// In an x86-64 binary relational fast path, start the rhs load before the lhs
/// load. The lhs is commonly the destination of the preceding bytecode
/// instruction, so this gives its store more time to complete. Check the rhs
/// tag early to shorten the dependency chain leading to the comparison.
pub(crate) fn schedule_x86_relational_operand_loads(instructions: &mut Vec<Instruction>) {
    let mut index = 0;
    while index + 2 < instructions.len() {
        let pair = instructions[index].clone();
        let lhs_load = instructions[index + 1].clone();
        let rhs_load = instructions[index + 2].clone();
        if pair.opcode.operation() != Operation::load_pair(PairWidth::Word)
            || lhs_load.opcode.operation() != Operation::load(MemoryWidth::DoubleWord, false)
            || rhs_load.opcode.operation() != Operation::load(MemoryWidth::DoubleWord, false)
            || pair.operands.len() != 4
            || lhs_load.operands.len() != 2
            || rhs_load.operands.len() != 2
        {
            index += 1;
            continue;
        }

        let lhs_value = lhs_load.operands[0].clone();
        let rhs_value = rhs_load.operands[0].clone();
        let hot_tail = &instructions[index + 3..];
        let hot_tail_end = hot_tail
            .iter()
            .position(|instruction| instruction.opcode.operation() == Operation::Cold)
            .unwrap_or(hot_tail.len());
        let hot_tail = &hot_tail[..hot_tail_end];
        let has_lhs_tag_check = hot_tail.iter().any(|instruction| {
            instruction.opcode.operation() == Operation::branch_tag(EqualityCondition::NotEqual)
                && instruction.operands.first() == Some(&lhs_value)
        });
        let rhs_tag_check = hot_tail
            .iter()
            .position(|instruction| {
                instruction.opcode.operation() == Operation::branch_tag(EqualityCondition::NotEqual)
                    && instruction.operands.first() == Some(&rhs_value)
            })
            .map(|position| index + 3 + position);
        let has_relational_compare = hot_tail.iter().any(|instruction| {
            matches!(
                instruction.opcode.operation(),
                Operation::Branch(BranchOperation::Ordered {
                    width: IntegerWidth::U32,
                    signedness: IntegerSignedness::Signed,
                    ..
                })
            ) && instruction.operands.first() == Some(&lhs_value)
                && instruction.operands.get(1) == Some(&rhs_value)
        });
        let Some(rhs_tag_check) = rhs_tag_check else {
            index += 1;
            continue;
        };
        if !has_lhs_tag_check || !has_relational_compare {
            index += 1;
            continue;
        }

        let rhs_tag_branch = &instructions[rhs_tag_check];
        let tag = rhs_tag_branch.operands[1].clone();
        let target = rhs_tag_branch.operands[2].clone();
        let rhs_index = pair.operands[1].clone();
        instructions.splice(
            rhs_tag_check..rhs_tag_check + 1,
            [
                Instruction {
                    opcode: Operation::ExtractTag,
                    operands: vec![rhs_index.clone(), rhs_value.clone()],
                },
                Instruction {
                    opcode: Operation::branch_equality(IntegerWidth::U16, EqualityCondition::NotEqual),
                    operands: vec![rhs_index.clone(), tag, target],
                },
            ],
        );

        instructions.splice(
            index..index + 3,
            [
                Instruction {
                    opcode: Operation::load(MemoryWidth::Word, false),
                    operands: vec![rhs_index, pair.operands[3].clone()],
                },
                rhs_load,
                Instruction {
                    opcode: Operation::load(MemoryWidth::Word, false),
                    operands: vec![pair.operands[0].clone(), pair.operands[2].clone()],
                },
                lhs_load,
            ],
        );
        index += 4;
    }
}

/// Prefer the dying input as the destructive destination of a commutative
/// update. Flap expressions initially lower `result = lhs + rhs` to
/// `mov result, lhs; add result, rhs`. When `lhs` remains live but `rhs` dies
/// at the add, reversing the operands lets the allocator coalesce `result`
/// with `rhs` and removes both an otherwise-required register and the move.
pub(crate) fn orient_commutative_updates(instructions: &mut [Instruction]) {
    // Where each register is named for the last time. Asking whether a value
    // outlives an instruction by scanning the rest of the listing turns this
    // into a quadratic pass over a large function. Reversing a pair of
    // operands only moves a register between the two instructions of the pair,
    // both of which lie before every listing suffix later steps look at, so
    // these positions stay correct as the pass runs.
    let mut last_reference = HashMap::default();
    for (index, instruction) in instructions.iter().enumerate() {
        for operand in &instruction.operands {
            visit_virtual_registers(operand, &mut |register| {
                last_reference.insert(register.clone(), index);
            });
        }
    }
    let referenced_after = |register: &VirtualRegister, position: usize| {
        last_reference.get(register).is_some_and(|last| *last >= position)
    };

    let mut index = 0;
    while index + 1 < instructions.len() {
        let copy = &instructions[index];
        let update = &instructions[index + 1];
        if copy.opcode.operation() != Operation::Move(IntegerWidth::U64)
            || !matches!(
                update.opcode.operation(),
                Operation::IntegerBinary {
                    operation: IntegerBinaryOperation::Binary(
                        BinaryOperation::Add | BinaryOperation::And | BinaryOperation::Or | BinaryOperation::Xor,
                    ),
                    width: IntegerWidth::U64,
                }
            )
            || copy.operands.len() != 2
            || update.operands.len() != 2
        {
            index += 1;
            continue;
        }
        let (lhs, rhs) = {
            let (
                Operand::VirtualRegister(destination),
                Operand::VirtualRegister(lhs),
                Operand::VirtualRegister(update_destination),
                Operand::VirtualRegister(rhs),
            ) = (
                &copy.operands[0],
                &copy.operands[1],
                &update.operands[0],
                &update.operands[1],
            )
            else {
                index += 1;
                continue;
            };
            if destination != update_destination || lhs == rhs || destination == rhs {
                index += 1;
                continue;
            }
            (lhs.clone(), rhs.clone())
        };
        let lhs_is_live = referenced_after(&lhs, index + 2);
        let rhs_is_dead = !referenced_after(&rhs, index + 2);
        if lhs_is_live && rhs_is_dead {
            instructions[index].operands[1] = Operand::VirtualRegister(rhs);
            instructions[index + 1].operands[1] = Operand::VirtualRegister(lhs);
        }
        index += 1;
    }
}

/// Split cheap values around calls when their entire live range is local to
/// one machine block. This keeps the AOT interpreter spill-free while letting
/// constants be recreated after caller-saved registers are destroyed.
pub(crate) fn split_rematerializable_live_ranges_across_calls(instructions: &mut Vec<Instruction>) {
    let calls = instructions
        .iter()
        .enumerate()
        .filter_map(|(index, instruction)| instruction.opcode.description().is_call.then_some(index))
        .collect::<Vec<_>>();
    if calls.is_empty() {
        return;
    }

    let instruction_blocks = instruction_blocks(instructions);
    let mut register_blocks = HashMap::<VirtualRegister, Option<usize>>::default();
    let mut registers = HashSet::default();
    for (index, instruction) in instructions.iter().enumerate() {
        for operand in &instruction.operands {
            visit_virtual_registers(operand, &mut |register| {
                registers.insert(register.clone());
                register_blocks
                    .entry(register.clone())
                    .and_modify(|block| {
                        if *block != Some(instruction_blocks[index]) {
                            *block = None;
                        }
                    })
                    .or_insert(Some(instruction_blocks[index]));
            });
        }
    }

    let mut split_index = 0u64;
    for call in calls.into_iter().rev() {
        let block = instruction_blocks[call];
        let start = machine_block_start(instructions, call);
        let end = machine_block_end(instructions, call);
        let mut recipes = rematerialization_recipes(&instructions[start..call])
            .into_iter()
            .collect::<Vec<_>>();
        recipes.sort_by(|(lhs, _), (rhs, _)| lhs.cmp(rhs));
        let mut splits = Vec::new();
        for (register, recipe) in recipes {
            if register_blocks.get(&register) != Some(&Some(block))
                || instruction_defines_register(&instructions[call], &register)
            {
                continue;
            }
            let Some(last_reference) = instructions[call + 1..end]
                .iter()
                .rposition(|instruction| instruction_references_register(instruction, &register))
                .map(|position| call + 1 + position)
            else {
                continue;
            };
            if instructions[call + 1..=last_reference]
                .iter()
                .any(|instruction| instruction.opcode.description().is_call)
            {
                continue;
            }
            let first_reference = instructions[call + 1..=last_reference]
                .iter()
                .find(|instruction| instruction_references_register(instruction, &register))
                .expect("last reference guarantees a first reference");
            if !instruction_reads_register(first_reference, &register) {
                continue;
            }

            let replacement = loop {
                let candidate =
                    VirtualRegister::with_class(format!("{register}_after_call_{split_index}"), register.class());
                split_index += 1;
                if registers.insert(candidate.clone()) {
                    break candidate;
                }
            };
            splits.push((register, replacement, recipe));
        }

        for (register, replacement, _) in &splits {
            for instruction in &mut instructions[call + 1..end] {
                for operand in &mut instruction.operands {
                    visit_virtual_registers_mut(operand, &mut |candidate| {
                        if candidate == register {
                            *candidate = replacement.clone();
                        }
                    });
                }
            }
        }
        let rematerializations = splits
            .into_iter()
            .map(|(_, replacement, mut recipe)| {
                let Operand::VirtualRegister(destination) = &mut recipe.operands[0] else {
                    unreachable!("rematerialization recipe must define a virtual register")
                };
                *destination = replacement;
                recipe
            })
            .collect::<Vec<_>>();
        instructions.splice(call + 1..call + 1, rematerializations);
    }
}

fn instruction_blocks(instructions: &[Instruction]) -> Vec<usize> {
    let mut blocks = Vec::with_capacity(instructions.len());
    let mut block = 0usize;
    for (index, instruction) in instructions.iter().enumerate() {
        if index > 0
            && (instruction.opcode.operation() == Operation::Label
                || machine_instruction_ends_block(&instructions[index - 1]))
        {
            block += 1;
        }
        blocks.push(block);
    }
    blocks
}

fn machine_block_start(instructions: &[Instruction], instruction: usize) -> usize {
    (0..=instruction)
        .rev()
        .find(|index| {
            *index == 0
                || instructions[*index].opcode.operation() == Operation::Label
                || machine_instruction_ends_block(&instructions[*index - 1])
        })
        .unwrap()
}

fn machine_block_end(instructions: &[Instruction], instruction: usize) -> usize {
    (instruction..instructions.len())
        .find_map(|index| {
            (machine_instruction_ends_block(&instructions[index])
                || instructions
                    .get(index + 1)
                    .is_some_and(|instruction| instruction.opcode.operation() == Operation::Label))
            .then_some(index + 1)
        })
        .unwrap_or(instructions.len())
}

fn machine_instruction_ends_block(instruction: &Instruction) -> bool {
    instruction.opcode.description().terminal
        || (instruction.opcode.operation() != Operation::Label
            && instruction
                .operands
                .iter()
                .any(|operand| matches!(operand, Operand::Label(_))))
}

fn rematerialization_recipes(instructions: &[Instruction]) -> HashMap<VirtualRegister, Instruction> {
    let mut seen = HashSet::default();
    let mut recipes = HashMap::default();
    for instruction in instructions {
        let recipe = rematerialization_recipe(instruction);
        let mut referenced = Vec::new();
        for operand in &instruction.operands {
            visit_virtual_registers(operand, &mut |register| {
                if !referenced.contains(register) {
                    referenced.push(register.clone());
                }
            });
        }
        for register in referenced {
            let first_reference = seen.insert(register.clone());
            if recipe
                .as_ref()
                .is_some_and(|(recipe_register, _)| *recipe_register == register)
            {
                if first_reference {
                    recipes.insert(register, recipe.as_ref().unwrap().1.clone());
                } else {
                    recipes.remove(&register);
                }
            } else if instruction_defines_register(instruction, &register) {
                recipes.remove(&register);
            }
        }
    }
    recipes
}

fn rematerialization_recipe(instruction: &Instruction) -> Option<(VirtualRegister, Instruction)> {
    if !matches!(
        instruction.opcode.operation(),
        Operation::Move(IntegerWidth::U32 | IntegerWidth::U64)
    ) {
        return None;
    }
    let [
        Operand::VirtualRegister(register),
        source @ (Operand::Immediate(_) | Operand::ValueConstant(_)),
    ] = instruction.operands.as_slice()
    else {
        return None;
    };
    Some((
        register.clone(),
        Instruction {
            opcode: instruction.opcode,
            operands: vec![Operand::VirtualRegister(register.clone()), source.clone()],
        },
    ))
}

fn instruction_references_register(instruction: &Instruction, register: &VirtualRegister) -> bool {
    instruction.operands.iter().any(|operand| {
        let mut references = false;
        visit_virtual_registers(operand, &mut |candidate| references |= candidate == register);
        references
    })
}

fn instruction_reads_register(instruction: &Instruction, register: &VirtualRegister) -> bool {
    let description = instruction.opcode.description();
    instruction.operands.iter().enumerate().any(|(index, operand)| {
        if matches!(operand, Operand::Address(_)) {
            let mut reads = false;
            visit_virtual_registers(operand, &mut |candidate| reads |= candidate == register);
            return reads;
        }
        let Operand::VirtualRegister(candidate) = operand else {
            return false;
        };
        candidate == register
            && matches!(
                description.operand_kind(index, instruction.operands.len()),
                Some(
                    OperandKind::GprIn
                        | OperandKind::GprInOut
                        | OperandKind::FprIn
                        | OperandKind::FprInOut
                        | OperandKind::RegisterIn
                        | OperandKind::GprInOrImm
                        | OperandKind::GprInOrMemory
                )
            )
    })
}

fn instruction_defines_register(instruction: &Instruction, register: &VirtualRegister) -> bool {
    let description = instruction.opcode.description();
    instruction.operands.iter().enumerate().any(|(index, operand)| {
        matches!(operand, Operand::VirtualRegister(candidate) if candidate == register)
            && matches!(
                description.operand_kind(index, instruction.operands.len()),
                Some(
                    OperandKind::GprOut
                        | OperandKind::GprInOut
                        | OperandKind::FprOut
                        | OperandKind::FprInOut
                        | OperandKind::RegisterOut
                )
            )
    })
}
