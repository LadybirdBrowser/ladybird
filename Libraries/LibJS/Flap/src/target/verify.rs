/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Verification for allocated, target-specific machine IR.

use super::ir::{AllocatedFunction as Handler, AllocatedProgram as Program};
use crate::Architecture;
use crate::hash::HashSet;
use crate::target::description::{OperandKind, Operation};
use crate::target::ir::AllocatedOperand as Operand;
use crate::{CompileError, CompileStage};

pub(crate) fn verify_program(program: &Program) -> Result<(), CompileError> {
    for handler in &program.functions {
        verify_handler(handler, program.architecture)?;
    }
    Ok(())
}

pub(crate) fn verify_handler(handler: &Handler, architecture: Architecture) -> Result<(), CompileError> {
    let mut definitions = HashSet::default();
    let mut references = Vec::new();
    for instruction in handler.cfg.instructions() {
        if instruction.opcode.architecture() != architecture {
            return error(handler, "instruction opcode belongs to the wrong target architecture");
        }
        if instruction.opcode.operation() == Operation::Label {
            let [Operand::Label(label)] = instruction.operands.as_slice() else {
                return error(handler, "label instruction must define exactly one label");
            };
            if !definitions.insert(label.clone()) {
                return error(handler, format!("label '{label}' is defined more than once"));
            }
        }
        let info = instruction.opcode.description();
        if !info.accepts_selected_operand_count(instruction.operands.len(), architecture) {
            return error(
                handler,
                format!(
                    "'{:?}' has {} operands but its description does not accept that arity",
                    instruction.opcode.operation(),
                    instruction.operands.len()
                ),
            );
        }
        for (index, operand) in instruction.operands.iter().enumerate() {
            let kind = info
                .selected_operand_kind(index, instruction.operands.len(), architecture)
                .expect("verified operand index must have a description");
            verify_operand(
                handler,
                operand,
                kind,
                architecture,
                instruction.opcode.operation() == Operation::LoadEffectiveAddress,
            )
            .map_err(|mut error| {
                error.message = format!(
                    "operand {index} of '{:?}': {}",
                    instruction.opcode.operation(),
                    error.message
                );
                error
            })?;
            if kind == OperandKind::Label
                && instruction.opcode.operation() != Operation::Label
                && let Operand::Label(label) = operand
            {
                references.push(label.clone());
            }
        }
        for (scratch_index, scratch) in instruction.operands.iter().enumerate().filter(|(index, _)| {
            info.selected_operand_kind(*index, instruction.operands.len(), architecture)
                .is_some_and(|kind| matches!(kind, OperandKind::GprScratch | OperandKind::FprScratch))
        }) {
            let Operand::PhysicalRegister(scratch) = scratch else {
                unreachable!("scratch operand shape was verified");
            };
            if instruction
                .operands
                .iter()
                .enumerate()
                .any(|(index, operand)| index != scratch_index && operand_references_register(operand, *scratch))
            {
                return error(
                    handler,
                    format!(
                        "scratch operand {scratch_index} of '{:?}' aliases another operand",
                        instruction.opcode.operation()
                    ),
                );
            }
        }
        for &(index, expected) in info.arch(architecture).fixed_operands {
            if matches!(instruction.operands[index], Operand::Immediate(_)) {
                continue;
            }
            let actual = instruction.operands[index].physical_register();
            if actual != Some(expected) {
                return error(
                    handler,
                    format!(
                        "operand {index} of '{:?}' must use register '{expected}'",
                        instruction.opcode.operation()
                    ),
                );
            }
        }
        verify_operation(handler, instruction.opcode.operation(), &instruction.operands)?;
    }
    for reference in references {
        if reference.starts_with('.') && !definitions.contains(&reference) {
            return error(handler, format!("branch references undefined label '{reference}'"));
        }
    }
    Ok(())
}

fn verify_operation(handler: &Handler, operation: Operation, operands: &[Operand]) -> Result<(), CompileError> {
    let immediate = |index| {
        let Operand::Immediate(value) = operands[index] else {
            unreachable!("allocated immediate operand shape was verified")
        };
        value
    };
    match operation {
        Operation::Branch(crate::intrinsic::BranchOperation::Bit(_)) if !(0..64).contains(&immediate(1)) => {
            error(handler, "single-bit branch index must be between 0 and 63")
        }
        Operation::Store64IndexedOffset if !matches!(immediate(2), 1 | 2 | 4 | 8) => {
            error(handler, "indexed offset store scale must be 1, 2, 4, or 8")
        }
        _ => Ok(()),
    }
}

fn verify_operand(
    handler: &Handler,
    operand: &Operand,
    kind: OperandKind,
    architecture: Architecture,
    is_effective_address: bool,
) -> Result<(), CompileError> {
    verify_allocated_operand(handler, operand, architecture, is_effective_address)?;
    if kind.accepts(operand.shape()) {
        Ok(())
    } else {
        error(handler, format!("operand {operand:?} does not satisfy {kind:?}"))
    }
}

fn verify_allocated_operand(
    handler: &Handler,
    operand: &Operand,
    architecture: Architecture,
    is_effective_address: bool,
) -> Result<(), CompileError> {
    match operand {
        Operand::PhysicalRegister(register) if register.architecture() != architecture => error(
            handler,
            format!("register '{register}' belongs to the wrong architecture"),
        ),
        Operand::Address(address) => {
            if address.base.architecture() != architecture
                || address.index.is_some_and(|index| index.architecture() != architecture)
            {
                return error(handler, "address register belongs to the wrong architecture");
            }
            if let Some(scale) = &address.scale {
                if !is_effective_address && !matches!(scale, 1 | 2 | 4 | 8) {
                    return error(handler, format!("address has invalid scale {scale}"));
                }
                if address.index.is_none() {
                    return error(handler, "address scale requires an index register");
                }
            }
            Ok(())
        }
        _ => Ok(()),
    }
}

fn operand_references_register(operand: &Operand, register: super::registers::PhysicalRegister) -> bool {
    match operand {
        Operand::PhysicalRegister(candidate) => *candidate == register,
        Operand::Address(address) => address.base == register || address.index == Some(register),
        _ => false,
    }
}

fn error<T>(handler: &Handler, message: impl Into<String>) -> Result<T, CompileError> {
    Err(CompileError::new(
        CompileStage::Verification,
        Some(&handler.name),
        message,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::low_ir::cfg::ControlFlowGraph;
    use crate::target::description::{
        BinaryOperation, FloatConversion, FloatingPointOperation, IntegerBinaryOperation, IntegerWidth, MemoryWidth,
        Operation, PairWidth,
    };
    use crate::target::ir::{AllocatedInstruction, AllocatedMemoryAddress as MemoryAddress};
    use crate::target::registers::{aarch64, x86_64};

    type Instruction = crate::low_ir::Instruction<Operand>;

    fn verify_instruction(instruction: Instruction) -> Result<(), CompileError> {
        verify_instruction_for_architecture(instruction, Architecture::X86_64)
    }

    fn verify_instruction_for_architecture(
        instruction: Instruction,
        architecture: Architecture,
    ) -> Result<(), CompileError> {
        let operation = instruction.opcode.operation();
        let operands = instruction
            .operands
            .iter()
            .map(Operand::selection_operand)
            .collect::<Vec<_>>();
        let instruction = AllocatedInstruction {
            opcode: operation.select_for_operands(architecture, &operands),
            operands: instruction.operands,
        };
        let handler = Handler {
            id: crate::identity::HandlerId::new(0),
            name: "Test".into(),
            size: None,
            is_cold: false,
            cfg: ControlFlowGraph::from_instructions(vec![instruction]).unwrap(),
        };
        verify_handler(&handler, architecture)
    }

    fn memory(base: crate::target::registers::PhysicalRegister, displacement: Option<i64>) -> Operand {
        Operand::Address(MemoryAddress {
            base,
            index: None,
            scale: None,
            displacement,
        })
    }

    fn assert_invalid_arity(architecture: Architecture, operation: Operation, operands: Vec<Operand>) {
        let error = verify_instruction_for_architecture(
            Instruction {
                opcode: operation,
                operands,
            },
            architecture,
        )
        .unwrap_err();
        assert!(error.message.contains("does not accept that arity"));
    }

    fn assert_invalid_operation(operation: Operation, operands: Vec<Operand>, expected: &str) {
        let error = verify_instruction(Instruction {
            opcode: operation,
            operands,
        })
        .unwrap_err();
        assert!(error.message.contains(expected), "{}", error.message);
    }

    #[test]
    fn rejects_invalid_address_scales() {
        let error = verify_instruction(Instruction {
            opcode: Operation::load(MemoryWidth::DoubleWord, false),
            operands: vec![
                Operand::PhysicalRegister(x86_64::RAX),
                Operand::Address(MemoryAddress {
                    base: x86_64::RAX,
                    index: Some(x86_64::RCX),
                    scale: Some(3),
                    displacement: None,
                }),
            ],
        })
        .unwrap_err();

        assert!(error.message.contains("invalid scale"));

        assert_invalid_operation(
            Operation::branch_bit(crate::target::description::TestCondition::Set),
            vec![
                Operand::PhysicalRegister(x86_64::RAX),
                Operand::Immediate(64),
                Operand::Label(".target".into()),
            ],
            "bit branch index",
        );
        assert_invalid_operation(
            Operation::Store64IndexedOffset,
            vec![
                Operand::PhysicalRegister(x86_64::RAX),
                Operand::PhysicalRegister(x86_64::RCX),
                Operand::Immediate(3),
                Operand::Immediate(0),
                Operand::PhysicalRegister(x86_64::RDX),
            ],
            "indexed offset store scale",
        );
    }

    #[test]
    fn rejects_undefined_local_labels() {
        let error = verify_instruction(Instruction {
            opcode: Operation::Control(crate::intrinsic::ControlOperation::JumpLabel),
            operands: vec![Operand::Label(".missing".into())],
        })
        .unwrap_err();

        assert!(error.message.contains("undefined label"));
    }

    #[test]
    fn rejects_explicit_scratch_aliases() {
        let error = verify_instruction_for_architecture(
            Instruction {
                opcode: Operation::Modulo(IntegerWidth::U32),
                operands: vec![
                    Operand::PhysicalRegister(aarch64::X1),
                    Operand::PhysicalRegister(aarch64::X2),
                    Operand::PhysicalRegister(aarch64::X3),
                    Operand::PhysicalRegister(aarch64::X3),
                ],
            },
            Architecture::Aarch64,
        )
        .unwrap_err();

        assert!(error.message.contains("scratch operand 3"));
        assert!(error.message.contains("aliases another operand"));

        let error = verify_instruction_for_architecture(
            Instruction {
                opcode: Operation::Float(FloatingPointOperation::Convert(FloatConversion::Float64ToInt32)),
                operands: vec![
                    Operand::PhysicalRegister(aarch64::X0),
                    Operand::PhysicalRegister(aarch64::D16),
                    Operand::Label(".failure".into()),
                    Operand::PhysicalRegister(aarch64::D16),
                ],
            },
            Architecture::Aarch64,
        )
        .unwrap_err();

        assert!(error.message.contains("scratch operand 3"));
        assert!(error.message.contains("aliases another operand"));
    }

    #[test]
    fn rejects_architecture_specific_scratch_arities() {
        assert_invalid_arity(
            Architecture::Aarch64,
            Operation::load(MemoryWidth::DoubleWord, false),
            vec![Operand::PhysicalRegister(aarch64::X0), memory(aarch64::X1, None)],
        );
        assert_invalid_arity(
            Architecture::Aarch64,
            Operation::IntegerBinary {
                operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                width: IntegerWidth::U64,
            },
            vec![Operand::PhysicalRegister(aarch64::X0), Operand::Immediate(0x1_0000)],
        );
        assert_invalid_arity(
            Architecture::Aarch64,
            Operation::IntegerBinary {
                operation: IntegerBinaryOperation::Binary(BinaryOperation::Multiply),
                width: IntegerWidth::U64,
            },
            vec![
                Operand::PhysicalRegister(aarch64::X0),
                Operand::PhysicalRegister(aarch64::X1),
            ],
        );
        assert_invalid_arity(
            Architecture::X86_64,
            Operation::IntegerBinary {
                operation: IntegerBinaryOperation::Binary(BinaryOperation::And),
                width: IntegerWidth::U64,
            },
            vec![
                Operand::PhysicalRegister(x86_64::RDX),
                Operand::Immediate(0x3_ffff_ffff),
            ],
        );
        assert_invalid_arity(
            Architecture::Aarch64,
            Operation::load_pair(PairWidth::DoubleWord),
            vec![
                Operand::PhysicalRegister(aarch64::X0),
                Operand::PhysicalRegister(aarch64::X1),
                memory(aarch64::X2, None),
                memory(aarch64::X2, Some(8)),
            ],
        );
        assert_invalid_arity(
            Architecture::Aarch64,
            Operation::StorePairIndexed(PairWidth::DoubleWord),
            vec![
                Operand::PhysicalRegister(aarch64::X0),
                Operand::PhysicalRegister(aarch64::X1),
                Operand::Immediate(8),
                Operand::PhysicalRegister(aarch64::X2),
                Operand::PhysicalRegister(aarch64::X3),
            ],
        );
        assert_invalid_arity(
            Architecture::Aarch64,
            Operation::LoadEffectiveAddress,
            vec![Operand::PhysicalRegister(aarch64::X0), memory(aarch64::X1, None)],
        );
        assert_invalid_arity(
            Architecture::Aarch64,
            Operation::Store64IndexedOffset,
            vec![
                Operand::PhysicalRegister(aarch64::X0),
                Operand::PhysicalRegister(aarch64::X1),
                Operand::Immediate(8),
                Operand::Immediate(16),
                Operand::PhysicalRegister(aarch64::X2),
            ],
        );
        assert_invalid_arity(
            Architecture::Aarch64,
            Operation::store(MemoryWidth::DoubleWord),
            vec![memory(aarch64::X0, None), Operand::Immediate(1)],
        );
        assert_invalid_arity(
            Architecture::Aarch64,
            Operation::Modulo(IntegerWidth::U32),
            [aarch64::X1, aarch64::X2, aarch64::X3]
                .map(Operand::PhysicalRegister)
                .into(),
        );
        assert_invalid_arity(
            Architecture::X86_64,
            Operation::Modulo(IntegerWidth::U32),
            vec![
                Operand::PhysicalRegister(x86_64::RAX),
                Operand::PhysicalRegister(x86_64::RDX),
                Operand::PhysicalRegister(x86_64::RSI),
                Operand::PhysicalRegister(x86_64::RDI),
                Operand::PhysicalRegister(x86_64::R11),
            ],
        );
    }
}
