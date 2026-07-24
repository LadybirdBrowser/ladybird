/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Selection of legal target instructions from target-independent LowIR.

use super::ir::{Function, Instruction, Operand, Program};
use crate::Architecture;
use crate::low_ir::{self, AddressDisplacement, Operand as LowOperand};
use crate::low_ir::preallocate::{
    materialize_program_counter_reads, orient_commutative_updates,
    schedule_x86_relational_operand_loads,
};
use crate::target::description::{IntegerWidth, Operation};
use crate::{CompileError, CompileStage};

pub(crate) fn select_program(
    mut program: low_ir::Program,
    architecture: Architecture,
) -> Result<Program, CompileError> {
    let functions = std::mem::take(&mut program.handlers)
        .into_iter()
        .map(|mut handler| -> Result<_, CompileError> {
            materialize_program_counter_reads(&mut handler.instructions)
                .map_err(|message| selection_error(&handler.name, message))?;
            low_ir::intern_virtual_registers(handler.id, &mut handler.instructions);
            if architecture == Architecture::X86_64 {
                schedule_x86_relational_operand_loads(&mut handler.instructions);
            }
            orient_commutative_updates(&mut handler.instructions);
            let mut instructions = Vec::with_capacity(handler.instructions.len());
            for mut instruction in handler.instructions {
                let operation = instruction.opcode;
                for (index, operand) in instruction.operands.iter_mut().enumerate() {
                    legalize_operand(
                        operand,
                        operation,
                        index,
                        architecture,
                        &handler.name,
                    )?;
                }
                let mut operands = instruction.operands;
                if operation == Operation::Move(IntegerWidth::U32)
                    && let Some(Operand::Immediate(value)) = operands.get_mut(1)
                {
                    *value = (*value as u64 & 0xffff_ffff) as i64;
                }
                let opcode = instruction
                    .opcode
                    .select_for_operands(architecture, &operands);
                debug_assert_eq!(opcode.architecture(), architecture);
                if architecture == Architecture::Aarch64
                    && matches!(
                        operation,
                        Operation::IntegerBinary {
                            operation: crate::intrinsic::IntegerBinaryOperation::Binary(crate::target::description::BinaryOperation::Add),
                            width: IntegerWidth::U64,
                        }
                    )
                    && let Some(Operand::Immediate(value)) =
                        operands.get_mut(1)
                {
                    match opcode.aarch64() {
                        crate::target::aarch64::Opcode::AddSubtractImmediate {
                            operation: crate::target::aarch64::AddSubtractOperation::Subtract,
                            ..
                        } => {
                            *value = value.checked_neg().ok_or_else(|| {
                                CompileError::new(
                                    CompileStage::Selection,
                                    Some(&handler.name),
                                    "AArch64 add immediate cannot be negated",
                                )
                            })?;
                        }
                        crate::target::aarch64::Opcode::AddSubtractImmediate {
                            shift: crate::target::aarch64::ImmediateShift::Twelve,
                            ..
                        } => {
                            *value >>= 12;
                        }
                        _ => {}
                    }
                }
                opcode.add_contract_operands(&mut operands);
                instructions.push(Instruction {
                    opcode,
                    operands,
                });
            }
            Ok(Function {
                id: handler.id,
                name: handler.name,
                size: handler.size,
                is_cold: handler.is_cold,
                architecture,
                instructions,
            })
        })
        .collect::<Result<_, _>>()?;
    Ok(Program {
        runtime: program.runtime,
        dispatch_handlers: program.dispatch_handlers,
        architecture,
        functions,
    })
}

fn legalize_operand(
    operand: &mut LowOperand,
    operation: Operation,
    operand_index: usize,
    architecture: Architecture,
    handler: &str,
) -> Result<(), CompileError> {
    match operand {
        LowOperand::VirtualRegister(register) => {
            debug_assert!(register.id().is_some());
        }
        LowOperand::ValueConstant(value) => {
            let compares_value_tag = operand_index == 1
                && (matches!(
                    operation,
                    Operation::Assertion(crate::intrinsic::AssertionOperation::TagEqual | crate::intrinsic::AssertionOperation::TagNotEqual)
                        | Operation::Branch(crate::intrinsic::BranchOperation::Tag(_))
                ) || (matches!(operation, Operation::Branch(crate::intrinsic::BranchOperation::Singleton(_)))
                    && architecture == Architecture::X86_64));
            *operand = LowOperand::Immediate(if compares_value_tag {
                ((*value as u64) >> 48) as i64
            } else {
                *value
            });
        }
        LowOperand::LayoutConstant(constant) => {
            return Err(selection_error(
                handler,
                format!(
                    "unresolved layout constant #{}",
                    constant.id().index()
                ),
            ));
        }
        LowOperand::BytecodeField(field) => {
            return Err(selection_error(
                handler,
                format!(
                    "unresolved bytecode field #{}",
                    field.index()
                ),
            ));
        }
        LowOperand::Address(address) => {
            for register in std::iter::once(&address.base).chain(address.index.iter()) {
                if let low_ir::AddressRegister::Virtual(register) = register {
                    debug_assert!(register.id().is_some());
                }
            }
            if let Some(crate::low_ir::AddressScale::LayoutConstant(constant)) =
                &address.scale
            {
                return Err(selection_error(
                    handler,
                    format!(
                        "unresolved address scale layout constant #{}",
                        constant.id().index()
                    ),
                ));
            }
            if let Some(AddressDisplacement::LayoutConstant(constant)) =
                &address.displacement
            {
                return Err(selection_error(
                    handler,
                    format!(
                        "unresolved address displacement layout constant #{}",
                        constant.id().index()
                    ),
                ));
            }
            if let Some(AddressDisplacement::BytecodeField(field)) =
                &address.displacement
            {
                return Err(selection_error(
                    handler,
                    format!(
                        "unresolved bytecode field #{}",
                        field.index()
                    ),
                ));
            }
        }
        _ => {}
    }
    Ok(())
}

fn selection_error(handler: &str, message: impl Into<String>) -> CompileError {
    CompileError::new(CompileStage::Selection, Some(handler), message)
}
