/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Target-aware instruction combining before register allocation.

use super::{Instruction, Operand, VirtualRegister};
use crate::intrinsic::{BranchOperation, IntegerBinaryOperation, IntegerSignedness};
use crate::target::description::{
    BinaryOperation, EqualityCondition, IntegerWidth, MemoryWidth, Operation, OperandKind, PairWidth,
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
pub(crate) fn materialize_program_counter_reads(
    instructions: &mut Vec<Instruction>,
) -> Result<(), String> {
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
                    OperandKind::GprIn
                    | OperandKind::RegisterIn
                    | OperandKind::GprInOrImm
                    | OperandKind::GprInOrMemory,
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

        let temporary = Operand::VirtualRegister(VirtualRegister::from(format!(
            "program_counter_read_{reads}"
        )));
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
            || lhs_load.opcode.operation()
                != Operation::load(MemoryWidth::DoubleWord, false)
            || rhs_load.opcode.operation()
                != Operation::load(MemoryWidth::DoubleWord, false)
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
            instruction.opcode.operation()
                == Operation::branch_tag(EqualityCondition::NotEqual)
                && instruction.operands.first() == Some(&lhs_value)
        });
        let rhs_tag_check = hot_tail
            .iter()
            .position(|instruction| {
                instruction.opcode.operation()
                    == Operation::branch_tag(EqualityCondition::NotEqual)
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
    let mut index = 0;
    while index + 1 < instructions.len() {
        let copy = &instructions[index];
        let update = &instructions[index + 1];
        if copy.opcode.operation() != Operation::Move(IntegerWidth::U64)
            || !matches!(
                update.opcode.operation(),
                Operation::IntegerBinary {
                    operation: IntegerBinaryOperation::Binary(
                        BinaryOperation::Add
                            | BinaryOperation::And
                            | BinaryOperation::Or
                            | BinaryOperation::Xor,
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
        let lhs_is_live = instructions[index + 2..]
            .iter()
            .any(|instruction| instruction_references_register(instruction, &lhs));
        let rhs_is_dead = !instructions[index + 2..]
            .iter()
            .any(|instruction| instruction_references_register(instruction, &rhs));
        if lhs_is_live && rhs_is_dead {
            instructions[index].operands[1] = Operand::VirtualRegister(rhs);
            instructions[index + 1].operands[1] = Operand::VirtualRegister(lhs);
        }
        index += 1;
    }
}

fn instruction_references_register(
    instruction: &Instruction,
    register: &VirtualRegister,
) -> bool {
    instruction
        .operands
        .iter()
        .any(|operand| operand.references(register))
}
