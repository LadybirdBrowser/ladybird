/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Verification for finalized physical machine IR.

use super::description::OperandKind;
use super::ir::{
    MachineFunction, MachineInstruction, MachineMemoryAddress, MachineOpcode, MachineOperand, MachineProgram,
};
use crate::hash::HashSet;
use crate::{Architecture, CompileError, CompileStage};

pub(crate) fn verify_program(program: &MachineProgram) -> Result<(), CompileError> {
    let architecture = program.target.architecture;
    let mut handler_ids = HashSet::default();
    let mut handler_names = HashSet::default();
    for function in &program.functions {
        if !handler_ids.insert(function.id) {
            return program_error(format!("handler id {:?} is defined more than once", function.id));
        }
        if !handler_names.insert(function.name.as_str()) {
            return program_error(format!("handler '{}' is defined more than once", function.name));
        }
        verify_function(function, architecture)?;
    }
    for handler in program.dispatch_handlers.iter().flatten() {
        if !handler_ids.contains(handler) {
            return program_error(format!("dispatch table references missing handler id {handler:?}"));
        }
    }
    Ok(())
}

pub(crate) fn verify_function(function: &MachineFunction, architecture: Architecture) -> Result<(), CompileError> {
    let instructions = function.hot_instructions.iter().chain(&function.cold_instructions);
    let mut definitions = HashSet::default();
    let mut references = Vec::new();
    for instruction in instructions {
        verify_instruction(function, instruction, architecture)?;
        if instruction.opcode.is_label() {
            let [MachineOperand::Label(label)] = instruction.operands.as_slice() else {
                return function_error(function, "machine label must define exactly one label");
            };
            if !definitions.insert(label.clone()) {
                return function_error(function, format!("machine label '{label}' is defined more than once"));
            }
        } else {
            references.extend(instruction.operands.iter().filter_map(|operand| {
                let MachineOperand::Label(label) = operand else {
                    return None;
                };
                label.starts_with('.').then(|| label.clone())
            }));
        }
    }
    for reference in references {
        if !definitions.contains(&reference) {
            return function_error(
                function,
                format!("machine branch references undefined label '{reference}'"),
            );
        }
    }
    Ok(())
}

fn verify_instruction(
    function: &MachineFunction,
    instruction: &MachineInstruction,
    architecture: Architecture,
) -> Result<(), CompileError> {
    if instruction.opcode.architecture() != architecture {
        return function_error(function, "machine instruction opcode belongs to the wrong architecture");
    }
    if instruction.opcode.is_pseudo() {
        return function_error(
            function,
            format!("target pseudo {:?} remains after finalization", instruction.opcode),
        );
    }
    if !instruction.opcode.operands_are_valid(&instruction.operands) {
        return function_error(
            function,
            format!(
                "machine opcode {:?} has invalid operands {:?}",
                instruction.opcode, instruction.operands
            ),
        );
    }
    if !instruction.opcode.encoding_is_valid(&instruction.operands) {
        return function_error(
            function,
            format!(
                "machine opcode {:?} has unencodable operands {:?}",
                instruction.opcode, instruction.operands
            ),
        );
    }
    for operand in &instruction.operands {
        match operand {
            MachineOperand::PhysicalRegister(register) if register.architecture() != architecture => {
                return function_error(
                    function,
                    format!("machine register '{register}' belongs to the wrong architecture"),
                );
            }
            MachineOperand::Address(address) => {
                verify_address(function, address, architecture)?;
            }
            _ => {}
        }
    }
    Ok(())
}

fn verify_address(
    function: &MachineFunction,
    address: &MachineMemoryAddress,
    architecture: Architecture,
) -> Result<(), CompileError> {
    if address.base.architecture() != architecture
        || address.base.class() != super::registers::RegisterClass::GeneralPurpose
    {
        return function_error(
            function,
            format!(
                "machine address base '{}' is not a target general-purpose register",
                address.base
            ),
        );
    }
    if let Some(index) = address.index
        && (index.architecture() != architecture || index.class() != super::registers::RegisterClass::GeneralPurpose)
    {
        return function_error(
            function,
            format!("machine address index '{index}' is not a target general-purpose register"),
        );
    }
    if let Some(scale) = address.scale {
        if !matches!(scale, 1 | 2 | 4 | 8) {
            return function_error(function, format!("machine address has invalid scale {scale}"));
        }
        if address.index.is_none() {
            return function_error(function, "machine address scale requires an index register");
        }
    }
    match architecture {
        Architecture::X86_64 => {
            if address
                .displacement
                .is_some_and(|displacement| i32::try_from(displacement).is_err())
            {
                return function_error(function, "x86-64 machine address displacement does not fit in 32 bits");
            }
        }
        Architecture::Aarch64 => {
            if address.scale.is_some() {
                return function_error(function, "AArch64 machine address must encode its scale in the opcode");
            }
        }
    }
    Ok(())
}

pub(crate) fn operands_match(operands: &[MachineOperand], expected: &[OperandKind]) -> bool {
    operands.len() == expected.len()
        && operands
            .iter()
            .zip(expected)
            .all(|(operand, expected)| expected.accepts(operand.shape()))
}

macro_rules! define_machine_opcodes {
    (
        $operand_type:ty, $operands:ident;
        encoding ($immediate:ident, $shift_fits:ident) {
            $($encoding_setup:tt)*
        }
        $(
            [$($variant:tt)*] => $pattern:pat => $contract:tt
                $(encoding $encoding:block)?
                $(printing $printing:expr)?;
        )*
    ) => {
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
        pub(crate) enum Opcode {
            $($($variant)*,)*
        }

        impl Opcode {
            #[allow(unused_variables)]
            pub(crate) fn operands_are_valid(
                self,
                $operands: &[$operand_type],
            ) -> bool {
                match self {
                    $(
                        $pattern => {
                            define_machine_opcodes!(
                                @matches $operands, $contract
                            )
                        }
                    )*
                }
            }

            #[allow(unused_variables)]
            pub(crate) fn encoding_is_valid(
                self,
                $operands: &[$operand_type],
            ) -> bool {
                let $immediate = |index| {
                    let Some(super::ir::MachineOperand::Immediate(value)) =
                        $operands.get(index)
                    else {
                        return None;
                    };
                    Some(*value)
                };
                let $shift_fits =
                    |width: super::description::IntegerWidth, value: i64| {
                        let bits = match width {
                            super::description::IntegerWidth::U8 => 8,
                            super::description::IntegerWidth::U16 => 16,
                            super::description::IntegerWidth::U32 => 32,
                            super::description::IntegerWidth::U64 => 64,
                        };
                        (0..bits).contains(&value)
                    };
                $($encoding_setup)*
                match self {
                    $(
                        $(
                            $pattern => $encoding,
                        )?
                    )*
                    _ => true,
                }
            }

            #[allow(unused_variables)]
            fn simple_instruction(self) -> Option<SimpleInstruction> {
                match self {
                    $(
                        $(
                            $pattern => Some($printing),
                        )?
                    )*
                    _ => None,
                }
            }
        }
    };
    (@matches $operands:ident, any) => {
        true
    };
    (@matches $operands:ident, $custom:block) => {
        $custom
    };
    (
        @matches $operands:ident,
        [$([$($kind:ident),*]),+ $(,)?]
    ) => {
        false $(|| operands_match($operands, &[$($kind),*]))+
    };
}

pub(crate) use define_machine_opcodes;

macro_rules! machine_operand {
    (operand $operand:expr) => {
        $operand
    };
    (register $operand:expr) => {
        $crate::target::ir::MachineOperand::PhysicalRegister($operand)
    };
    (relocation $operand:expr) => {
        $crate::target::ir::MachineOperand::Relocation($operand)
    };
    (immediate $operand:expr) => {
        $crate::target::ir::MachineOperand::Immediate($operand)
    };
    (address $operand:expr) => {
        $crate::target::ir::MachineOperand::Address($operand)
    };
    (label $operand:expr) => {
        $crate::target::ir::MachineOperand::Label($operand)
    };
}

macro_rules! emit_machine_instructions {
    (
        $output:expr, $architecture:ident;
        $(
            $opcode:expr => [$($kind:ident $operand:expr),* $(,)?];
        )*
    ) => {
        $(
            $output.push($crate::target::ir::MachineInstruction {
                opcode: $crate::target::ir::MachineOpcode::$architecture($opcode),
                operands: vec![$($crate::target::machine_verify::machine_operand!($kind $operand)),*],
            });
        )*
    };
}

pub(crate) use emit_machine_instructions;
pub(crate) use machine_operand;

impl MachineOpcode {
    fn operands_are_valid(self, operands: &[MachineOperand]) -> bool {
        match self {
            MachineOpcode::X86_64(opcode) => opcode.operands_are_valid(operands),
            MachineOpcode::Aarch64(opcode) => opcode.operands_are_valid(operands),
        }
    }

    fn encoding_is_valid(self, operands: &[MachineOperand]) -> bool {
        match self {
            MachineOpcode::X86_64(opcode) => opcode.encoding_is_valid(operands),
            MachineOpcode::Aarch64(opcode) => opcode.encoding_is_valid(operands),
        }
    }

    fn is_pseudo(self) -> bool {
        match self {
            MachineOpcode::X86_64(opcode) => opcode.is_pseudo(),
            MachineOpcode::Aarch64(opcode) => opcode.is_pseudo(),
        }
    }

    fn is_label(self) -> bool {
        matches!(
            self,
            MachineOpcode::X86_64(super::x86_64::Opcode::Label) | MachineOpcode::Aarch64(super::aarch64::Opcode::Label)
        )
    }
}

fn function_error<T>(function: &MachineFunction, message: impl Into<String>) -> Result<T, CompileError> {
    Err(CompileError::new(
        CompileStage::Verification,
        Some(&function.name),
        message,
    ))
}

fn program_error<T>(message: impl Into<String>) -> Result<T, CompileError> {
    Err(CompileError::new(CompileStage::Verification, None, message))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::identity::HandlerId;
    use crate::target::description::MemoryWidth;
    use crate::target::registers::{aarch64, x86_64};

    fn function(instructions: Vec<MachineInstruction>) -> MachineFunction {
        MachineFunction {
            id: HandlerId::new(0),
            name: "Test".into(),
            is_cold: false,
            hot_instructions: instructions,
            cold_instructions: Vec::new(),
        }
    }

    fn verify_x86_64(opcode: super::super::x86_64::Opcode, operands: Vec<MachineOperand>) -> Result<(), CompileError> {
        verify_function(
            &function(vec![MachineInstruction {
                opcode: MachineOpcode::X86_64(opcode),
                operands,
            }]),
            Architecture::X86_64,
        )
    }

    #[test]
    fn rejects_target_pseudos() {
        let error = verify_x86_64(super::super::x86_64::Opcode::Pseudo, Vec::new()).unwrap_err();
        assert_eq!(error.stage, CompileStage::Verification);
        assert_eq!(error.handler.as_deref(), Some("Test"));
        assert!(error.message.contains("remains after finalization"));
    }

    #[test]
    fn rejects_cross_architecture_registers() {
        let error = verify_x86_64(
            super::super::x86_64::Opcode::Move64Register,
            vec![
                MachineOperand::PhysicalRegister(aarch64::X0),
                MachineOperand::PhysicalRegister(x86_64::RAX),
            ],
        )
        .unwrap_err();
        assert!(error.message.contains("wrong architecture"));
    }

    #[test]
    fn rejects_invalid_machine_operand_shapes() {
        let error = verify_x86_64(
            super::super::x86_64::Opcode::Move64Register,
            vec![MachineOperand::PhysicalRegister(x86_64::RAX)],
        )
        .unwrap_err();
        assert!(error.message.contains("has invalid operands"));

        let error = verify_x86_64(
            super::super::x86_64::Opcode::Load {
                width: MemoryWidth::DoubleWord,
                signed: false,
            },
            vec![
                MachineOperand::PhysicalRegister(x86_64::RAX),
                MachineOperand::Immediate(0),
            ],
        )
        .unwrap_err();
        assert!(error.message.contains("has invalid operands"));
    }

    #[test]
    fn rejects_invalid_machine_register_classes() {
        let error = verify_x86_64(
            super::super::x86_64::Opcode::CompareDouble,
            vec![
                MachineOperand::PhysicalRegister(x86_64::RAX),
                MachineOperand::PhysicalRegister(x86_64::RCX),
            ],
        )
        .unwrap_err();
        assert!(error.message.contains("has invalid operands"));
    }

    #[test]
    fn rejects_malformed_machine_addresses() {
        let error = verify_x86_64(
            super::super::x86_64::Opcode::Load {
                width: MemoryWidth::DoubleWord,
                signed: false,
            },
            vec![
                MachineOperand::PhysicalRegister(x86_64::RAX),
                MachineOperand::Address(MachineMemoryAddress::scaled(x86_64::RBP, x86_64::RCX, 3, 0)),
            ],
        )
        .unwrap_err();
        assert_eq!(error.message, "machine address has invalid scale 3");
    }

    #[test]
    fn rejects_unencodable_machine_operands() {
        let error = verify_x86_64(
            super::super::x86_64::Opcode::ShiftImmediate {
                operation: crate::target::description::ShiftOperation::Left,
                width: crate::target::description::IntegerWidth::U64,
            },
            vec![
                MachineOperand::PhysicalRegister(x86_64::RAX),
                MachineOperand::Immediate(64),
            ],
        )
        .unwrap_err();
        assert!(error.message.contains("unencodable operands"));

        let function = function(vec![MachineInstruction {
            opcode: MachineOpcode::Aarch64(super::super::aarch64::Opcode::Load {
                width: MemoryWidth::DoubleWord,
                signed: false,
                addressing: super::super::aarch64::MemoryAddressing::UnsignedImmediate,
            }),
            operands: vec![
                MachineOperand::PhysicalRegister(aarch64::X0),
                MachineOperand::Address(MachineMemoryAddress::offset(aarch64::X1, 3)),
            ],
        }]);
        let error = verify_function(&function, Architecture::Aarch64).unwrap_err();
        assert!(error.message.contains("unencodable operands"));
    }

    #[test]
    fn rejects_undefined_local_labels() {
        let error = verify_x86_64(
            super::super::x86_64::Opcode::Jump,
            vec![MachineOperand::Label(".missing".into())],
        )
        .unwrap_err();
        assert_eq!(error.message, "machine branch references undefined label '.missing'");
    }

    #[test]
    fn accepts_labels_across_hot_and_cold_ranges() {
        let mut function = function(vec![MachineInstruction {
            opcode: MachineOpcode::X86_64(super::super::x86_64::Opcode::Jump),
            operands: vec![MachineOperand::Label(".cold".into())],
        }]);
        function.cold_instructions.push(MachineInstruction {
            opcode: MachineOpcode::X86_64(super::super::x86_64::Opcode::Label),
            operands: vec![MachineOperand::Label(".cold".into())],
        });
        verify_function(&function, Architecture::X86_64).unwrap();
    }

    #[test]
    fn accepts_physical_machine_instructions() {
        verify_x86_64(
            super::super::x86_64::Opcode::Move64Register,
            vec![
                MachineOperand::PhysicalRegister(x86_64::RAX),
                MachineOperand::PhysicalRegister(x86_64::RCX),
            ],
        )
        .unwrap();
    }
}
