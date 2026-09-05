/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Post-allocation control-flow layout and machine instruction finalization.

use super::backend::{Backend, backend_for};
#[cfg(test)]
use super::description::PairWidth;
use super::description::{
    BinaryOperation, BranchOperation, CallKind, ControlOperation, FloatCondition, FloatConversion,
    FloatingPointOperation, IntegerBinaryOperation, IntegerWidth, MemoryOperation, MemoryWidth, Operation,
    SignCondition,
};
use super::finalize_support::{AllocatedOperands, Emit, finalization_error, verified_register};
use super::ir::{
    AllocatedFunction, AllocatedInstruction, AllocatedOperand, AllocatedProgram, MachineFunction, MachineInstruction,
    MachineOpcode, MachineProgram, RuntimeConstants,
};
use crate::CompileOptions;
#[cfg(test)]
use crate::frontend::layout::KnownLayoutConstant;
#[cfg(test)]
use crate::{Architecture, ObjectFormat};
use crate::{CompileError, CompileStage};

fn finalize_function_for_target(
    mut function: AllocatedFunction,
    runtime: &RuntimeConstants,
    options: &CompileOptions,
) -> Result<MachineFunction, CompileError> {
    let backend = backend_for(options.target.architecture);
    let mut emit = Emit::new(runtime, &function.name, function.size, options);
    let (hot_instructions, mut cold_instructions) = if function.is_cold {
        let instruction = function
            .cfg
            .single_instruction()
            .ok_or_else(|| finalization_error(&function.name, "cold handler must consist solely of call_slow_path"))?;
        if instruction.opcode.operation() != Operation::Call(CallKind::SlowPath) {
            return Err(finalization_error(
                &function.name,
                "cold handler must consist solely of call_slow_path",
            ));
        }
        (
            finalize_instructions(vec![instruction.clone()], backend, &mut emit)?,
            Vec::new(),
        )
    } else {
        let layout = function
            .cfg
            .layout_hot_and_cold()
            .map_err(|message| finalization_error(&function.name, message))?;
        // Nothing reads the graph after this, so it hands its instructions over
        // rather than being copied out of.
        let (hot, cold) = std::mem::take(&mut function.cfg).linearize_hot_and_cold(&layout);
        (
            finalize_instructions(hot, backend, &mut emit)?,
            finalize_instructions(cold, backend, &mut emit)?,
        )
    };

    cold_instructions.extend(emit.cold);
    let machine = MachineFunction {
        id: function.id,
        name: function.name,
        is_cold: function.is_cold,
        hot_instructions,
        cold_instructions,
    };
    Ok(machine)
}

#[cfg(test)]
fn finalize_function(function: AllocatedFunction, runtime: &RuntimeConstants) -> Result<MachineFunction, CompileError> {
    let options = CompileOptions {
        target: crate::Target {
            architecture: Architecture::X86_64,
            object_format: ObjectFormat::Elf,
        },
        has_jscvt: false,
        enable_assertions: false,
    };
    finalize_function_for_target(function, runtime, &options)
}

pub(crate) fn finalize_program(
    program: AllocatedProgram,
    options: &CompileOptions,
) -> Result<MachineProgram, CompileError> {
    let target = options.target;
    if program.architecture != target.architecture {
        return Err(CompileError::new(
            CompileStage::Finalization,
            None,
            "allocated program architecture does not match finalization target",
        ));
    }
    let runtime = program.runtime;
    let functions = program
        .functions
        .into_iter()
        .map(|function| finalize_function_for_target(function, &runtime, options))
        .collect::<Result<_, _>>()?;
    let machine = MachineProgram {
        runtime,
        dispatch_handlers: program.dispatch_handlers,
        target,
        functions,
    };
    super::machine_verify::verify_program(&machine)?;
    Ok(machine)
}

fn finalize_instructions(
    instructions: Vec<AllocatedInstruction>,
    backend: &dyn Backend,
    emit: &mut Emit<'_>,
) -> Result<Vec<MachineInstruction>, CompileError> {
    for instruction in instructions {
        finalize_instruction(instruction, backend, emit)?;
    }
    Ok(emit.take_output())
}

/// A floating-point branch reuses the preceding comparison when it tests the
/// same pair of registers, so a chain of branches only compares once.
fn finalize_float_branch(
    backend: &dyn Backend,
    emit: &mut Emit<'_>,
    condition: FloatCondition,
    operands: &[AllocatedOperand],
) {
    let [lhs, rhs] = [operands.physical_register(0), operands.physical_register(1)];
    let target = operands.label(2);
    let emit_compare = emit.last_fp_compare != Some((lhs, rhs));
    if emit_compare {
        emit.last_fp_compare = Some((lhs, rhs));
    }
    backend.branch_float(emit, lhs, rhs, emit_compare, condition, &target);
}

fn finalize_zero_or_sign_branch(
    backend: &dyn Backend,
    emit: &mut Emit<'_>,
    operation: Operation,
    operands: &[AllocatedOperand],
) {
    let value = operands.physical_register(0);
    let target = operands.label(1);
    match operation {
        Operation::Branch(BranchOperation::Zero { width, condition }) => {
            backend.branch_zero(emit, value, width, condition, &target);
        }
        Operation::Branch(BranchOperation::Sign { width, condition }) => {
            backend.branch_sign(emit, value, width, condition, &target);
        }
        Operation::Branch(BranchOperation::ValueBitsNegative) => {
            backend.branch_sign(emit, value, IntegerWidth::U64, SignCondition::Negative, &target);
        }
        _ => unreachable!("allocated operation and operands were verified"),
    }
}

fn finalize_value_operation(
    backend: &dyn Backend,
    emit: &mut Emit<'_>,
    operation: Operation,
    operands: &[AllocatedOperand],
) -> Result<(), CompileError> {
    match operation {
        Operation::BoxInt32 { clean } => {
            let [destination, source] = operands.physical_registers();
            backend.box_int32(emit, destination, source, clean)?;
        }
        operation @ (Operation::ToggleBit | Operation::ClearBit) => {
            let Ok(bit) = u32::try_from(operands.immediate(1)).map(|bit| bit.min(64)) else {
                return emit.error("bit update index must be between 0 and 63");
            };
            if bit >= 64 {
                return emit.error("bit update index must be between 0 and 63");
            }
            backend.update_bit(emit, operands.physical_register(0), bit, operation);
        }
        Operation::ExtractTag => {
            let [destination, source] = operands.physical_registers();
            backend.extract_tag(emit, destination, source);
        }
        Operation::UnboxObject => {
            let [destination, source] = operands.physical_registers();
            backend.unbox_object(emit, destination, source)?;
        }
        _ => unreachable!(),
    }
    Ok(())
}

fn finalize_float_operation(
    backend: &dyn Backend,
    emit: &mut Emit<'_>,
    opcode: MachineOpcode,
    operation: FloatingPointOperation,
    operands: &[AllocatedOperand],
) {
    if !matches!(
        operation,
        FloatingPointOperation::Convert(
            FloatConversion::Float64ToInt32 | FloatConversion::Float64ToInt64 | FloatConversion::JavaScriptToInt32
        )
    ) {
        backend.float_operation(emit, opcode, operation, operands);
        return;
    }
    let [destination, source, AllocatedOperand::Label(failure), scratches @ ..] = operands else {
        unreachable!("allocated operand shape was verified");
    };
    let failure = failure.clone();
    backend.checked_float_conversion(
        emit,
        operation,
        verified_register(destination),
        verified_register(source),
        scratches,
        &failure,
    );
}

fn finalize_call(
    backend: &dyn Backend,
    emit: &mut Emit<'_>,
    kind: CallKind,
    operands: &[AllocatedOperand],
) -> Result<(), CompileError> {
    match kind {
        CallKind::Helper => backend.helper_call(emit, operands.relocation(0)),
        CallKind::Interpreter => backend.interpreter_call(emit, operands.relocation(0)),
        CallKind::RawNative => backend.raw_native_call(emit, operands),
        CallKind::SlowPath => backend.slow_path_call(emit, operands)?,
        CallKind::BinarySlowPath => backend.binary_slow_path_call(emit, operands)?,
        CallKind::JumpSlowPath => backend.jump_slow_path_call(emit, operands)?,
    }
    Ok(())
}

fn finalize_dispatch(
    backend: &dyn Backend,
    emit: &mut Emit<'_>,
    kind: ControlOperation,
    operands: &[AllocatedOperand],
) -> Result<(), CompileError> {
    let semantic_count = usize::from(kind == ControlOperation::DispatchVariable);
    let (semantic_operands, scratches) = operands.split_at(semantic_count);
    match kind {
        ControlOperation::Dispatch => backend.dispatch_current(emit, scratches),
        ControlOperation::DispatchVariable => {
            backend.dispatch_variable(emit, verified_register(&semantic_operands[0]), scratches)
        }
        ControlOperation::DispatchNext => {
            let Some(size) = emit.handler_size else {
                return emit.error("variable-length handler must use variable dispatch");
            };
            backend.dispatch_next(emit, size, scratches)
        }
        _ => unreachable!("non-dispatch control operation reached dispatch finalization"),
    }
}

fn finalize_instruction(
    instruction: AllocatedInstruction,
    backend: &dyn Backend,
    emit: &mut Emit<'_>,
) -> Result<(), CompileError> {
    let operation = instruction.opcode.operation();

    if let Operation::Assertion(operation) = operation {
        let ok_label = emit.enable_assertions.then(|| emit.unique_label("assert_ok"));
        backend.finalize_assertion(emit, operation, &instruction.operands, ok_label)?;
        if emit.enable_assertions {
            emit.last_fp_compare = None;
        }
        return Ok(());
    }

    if let Operation::Branch(BranchOperation::Float(condition)) = operation {
        finalize_float_branch(backend, emit, condition, &instruction.operands);
        return Ok(());
    }

    emit.last_fp_compare = None;
    let opcode = instruction.opcode.machine_opcode();
    let operands = &instruction.operands;
    match operation {
        Operation::Branch(BranchOperation::Equality {
            width: IntegerWidth::U16 | IntegerWidth::U32 | IntegerWidth::U64,
            ..
        })
        | Operation::Branch(BranchOperation::Ordered { .. }) => {
            backend.finalize_scalar_compare_branch(emit, operation, operands)?;
        }
        Operation::Branch(BranchOperation::Zero {
            width: IntegerWidth::U32 | IntegerWidth::U64,
            ..
        })
        | Operation::Branch(BranchOperation::Sign {
            width: IntegerWidth::U32 | IntegerWidth::U64,
            ..
        })
        | Operation::Branch(BranchOperation::ValueBitsNegative) => {
            finalize_zero_or_sign_branch(backend, emit, operation, operands);
        }
        Operation::Branch(BranchOperation::Bits {
            width: MemoryWidth::DoubleWord,
            ..
        })
        | Operation::Branch(BranchOperation::Bit(_)) => {
            backend.finalize_bit_branch(emit, operation, operands)?;
        }
        Operation::Branch(BranchOperation::Tag(_) | BranchOperation::Singleton(_)) => {
            backend.finalize_value_representation_branch(emit, operation, operands);
        }
        Operation::Branch(BranchOperation::Memory { .. })
        | Operation::Branch(BranchOperation::Equality {
            width: IntegerWidth::U8,
            ..
        })
        | Operation::Branch(BranchOperation::Bits {
            width: MemoryWidth::Byte,
            ..
        }) => backend.branch_memory(emit, operation, operands)?,
        Operation::Branch(BranchOperation::AnyEqual) => {
            backend.finalize_any_equal_branch(emit, operands)?;
        }
        Operation::Float(FloatingPointOperation::CanonicalizeNan) => {
            backend.finalize_canonicalize_nan(emit, operands)?;
        }
        Operation::Float(operation) => {
            finalize_float_operation(backend, emit, opcode, operation, operands);
        }
        Operation::Move(width) => {
            backend.move_instruction(emit, opcode, width, &instruction.operands)?;
        }
        Operation::LoadEffectiveAddress => {
            backend.finalize_effective_address(emit, opcode, operands)?;
        }
        Operation::LoadVm => backend.finalize_vm_load(emit, operands),
        Operation::LoadProgramCounter => backend.finalize_program_counter_load(emit, operands),
        Operation::Call(kind) => finalize_call(backend, emit, kind, operands)?,
        Operation::Control(
            kind @ (ControlOperation::DispatchNext | ControlOperation::Dispatch | ControlOperation::DispatchVariable),
        ) => finalize_dispatch(backend, emit, kind, operands)?,
        Operation::StoreOperand => backend.finalize_operand_store(emit, operands)?,
        Operation::LoadLabel => backend.finalize_label_load(emit, operands)?,
        Operation::Control(ControlOperation::GotoHandler) => {
            backend.goto_handler(emit, operands)?;
        }
        Operation::Control(ControlOperation::JumpBytecode) => {
            backend.goto_bytecode_target(emit, operands)?;
        }
        Operation::StoreExecutionContext => {
            backend.finalize_execution_context_store(emit, operands)?;
        }
        Operation::Store64IndexedOffset => backend.finalize_indexed_offset_store(emit, operands),
        Operation::Increment32Memory => backend.finalize_memory_increment(emit, operands)?,
        Operation::Memory(MemoryOperation::Load { width, signed, .. }) => {
            backend.finalize_scalar_load(emit, opcode, width, signed, operands)?;
        }
        Operation::Memory(MemoryOperation::Store(width)) => {
            backend.finalize_scalar_store(emit, opcode, width, operands)?;
        }
        Operation::Memory(MemoryOperation::LoadPair(width)) => {
            backend.finalize_pair_load(emit, width, operands)?;
        }
        Operation::Memory(MemoryOperation::StorePair(width)) => {
            backend.finalize_pair_store(emit, width, false, operands)?;
        }
        Operation::StorePairIndexed(width) => {
            backend.finalize_pair_store(emit, width, true, operands)?;
        }
        Operation::Or32Branch(condition) => {
            backend.finalize_or32_branch(emit, condition, operands)?;
        }
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(BinaryOperation::Add | BinaryOperation::Subtract),
            width: IntegerWidth::U64,
        } => backend.add_subtract(emit, opcode, operands)?,
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(operation),
            width,
        } => backend.finalize_binary_operation(emit, operation, width, operands)?,
        Operation::Overflow(operation) => {
            backend.finalize_overflow_operation(emit, operation, operands);
        }
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Shift(operation),
            width,
        } => backend.finalize_shift(emit, operation, width, operands),
        Operation::BoxInt32 { .. }
        | Operation::ToggleBit
        | Operation::ClearBit
        | Operation::ExtractTag
        | Operation::UnboxObject => {
            finalize_value_operation(backend, emit, operation, operands)?;
        }
        Operation::Cold => {
            return emit.error("operation Cold was not expanded into legal machine instructions");
        }
        Operation::Modulo(width) => backend.finalize_divide(emit, width, operands),
        _ => emit.output.push(MachineInstruction {
            opcode,
            operands: instruction.operands,
        }),
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Architecture;
    use crate::low_ir::cfg::ControlFlowGraph;
    use crate::target::description::{ControlOperation, Operation};
    use crate::target::ir::{AllocatedInstruction, AllocatedMemoryAddress, AllocatedOperand as Operand};
    use crate::target::registers::x86_64;

    type Instruction = crate::low_ir::Instruction<Operand>;

    fn allocated_function(instructions: Vec<Instruction>, is_cold: bool) -> AllocatedFunction {
        let instructions = instructions
            .into_iter()
            .map(|instruction| AllocatedInstruction {
                opcode: select_allocated_opcode(instruction.opcode, &instruction.operands),
                operands: instruction.operands,
            })
            .collect();
        AllocatedFunction {
            id: crate::identity::HandlerId::new(0),
            name: "Test".into(),
            size: Some(1),
            is_cold,
            cfg: ControlFlowGraph::from_instructions(instructions).unwrap(),
        }
    }

    fn select_allocated_opcode(opcode: Operation, operands: &[Operand]) -> crate::target::description::SelectedOpcode {
        let operands = operands.iter().map(Operand::selection_operand).collect::<Vec<_>>();
        opcode.select_for_operands(Architecture::X86_64, &operands)
    }

    fn runtime() -> RuntimeConstants {
        RuntimeConstants::from_values([(KnownLayoutConstant::SizeOfExecutionContext, 120)])
    }

    fn test_options() -> CompileOptions {
        CompileOptions {
            target: crate::Target {
                architecture: Architecture::X86_64,
                object_format: ObjectFormat::Elf,
            },
            has_jscvt: false,
            enable_assertions: false,
        }
    }

    fn finalize_one(
        operation: Operation,
        operands: Vec<Operand>,
        runtime: &RuntimeConstants,
    ) -> Result<Vec<MachineInstruction>, CompileError> {
        let instruction = Instruction {
            opcode: operation,
            operands,
        };
        let instruction = AllocatedInstruction {
            opcode: select_allocated_opcode(instruction.opcode, &instruction.operands),
            operands: instruction.operands,
        };
        let options = test_options();
        let mut emit = Emit::new(runtime, "Test", None, &options);
        finalize_instruction(instruction, backend_for(Architecture::X86_64), &mut emit)?;
        Ok(emit.take_output())
    }

    fn address(
        base: super::super::registers::PhysicalRegister,
        index: Option<super::super::registers::PhysicalRegister>,
        displacement: Option<i64>,
    ) -> Operand {
        Operand::Address(AllocatedMemoryAddress {
            base,
            index,
            scale: None,
            displacement,
        })
    }

    #[test]
    fn resolves_hot_and_cold_instruction_layout() {
        let function = allocated_function(
            vec![
                Instruction {
                    opcode: Operation::Control(ControlOperation::JumpLabel),
                    operands: vec![Operand::Label(".done".into())],
                },
                Instruction {
                    opcode: Operation::Label,
                    operands: vec![Operand::Label(".done".into())],
                },
                Instruction {
                    opcode: Operation::Control(ControlOperation::DispatchNext),
                    operands: vec![Operand::PhysicalRegister(x86_64::RAX)],
                },
            ],
            false,
        );

        let machine = finalize_function(function, &runtime()).unwrap();
        assert_eq!(machine.hot_instructions.len(), 3);
        assert!(matches!(
            machine.hot_instructions[0].opcode,
            MachineOpcode::X86_64(super::super::x86_64::Opcode::AluImmediate {
                operation: super::super::x86_64::AluOperation::Add,
                width: IntegerWidth::U32,
            })
        ));
        assert!(matches!(
            machine.hot_instructions[1].opcode,
            MachineOpcode::X86_64(super::super::x86_64::Opcode::Load {
                width: MemoryWidth::Byte,
                signed: false,
            })
        ));
        assert!(matches!(
            machine.hot_instructions[2].opcode,
            MachineOpcode::X86_64(super::super::x86_64::Opcode::JumpMemory)
        ));
        assert!(machine.cold_instructions.is_empty());
    }

    #[test]
    fn rejects_non_terminal_cold_handler() {
        let function = allocated_function(
            vec![Instruction {
                opcode: Operation::Control(ControlOperation::DispatchNext),
                operands: vec![Operand::PhysicalRegister(x86_64::RAX)],
            }],
            true,
        );

        let error = finalize_function(function, &runtime()).unwrap_err();
        assert_eq!(error.stage, CompileStage::Finalization);
        assert_eq!(error.handler.as_deref(), Some("Test"));
        assert_eq!(error.message, "cold handler must consist solely of call_slow_path");
    }

    #[test]
    fn rejects_unencodable_x86_64_alu_immediate() {
        let error = finalize_function(
            allocated_function(
                vec![Instruction {
                    opcode: Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                        width: IntegerWidth::U64,
                    },
                    operands: vec![Operand::PhysicalRegister(x86_64::RAX), Operand::Immediate(i64::MAX)],
                }],
                false,
            ),
            &runtime(),
        )
        .unwrap_err();

        assert_eq!(error.stage, CompileStage::Finalization);
        assert_eq!(error.handler.as_deref(), Some("Test"));
        assert!(error.message.contains("immediate is out of range"));
    }

    #[test]
    fn rejects_selected_operations_after_finalization() {
        let error = finalize_one(Operation::Cold, Vec::new(), &runtime()).unwrap_err();
        assert_eq!(error.stage, CompileStage::Finalization);
        assert_eq!(
            error.message,
            "operation Cold was not expanded into legal machine instructions"
        );
    }

    #[test]
    fn rejects_nonadjacent_pair_addresses() {
        let error = finalize_function(
            allocated_function(
                vec![Instruction {
                    opcode: Operation::load_pair(PairWidth::DoubleWord),
                    operands: vec![
                        Operand::PhysicalRegister(x86_64::RAX),
                        Operand::PhysicalRegister(x86_64::RCX),
                        address(x86_64::RDX, None, None),
                        address(x86_64::RDX, None, Some(16)),
                    ],
                }],
                false,
            ),
            &runtime(),
        )
        .unwrap_err();

        assert_eq!(error.stage, CompileStage::Finalization);
        assert_eq!(error.handler.as_deref(), Some("Test"));
        assert_eq!(
            error.message,
            "paired memory addresses must be adjacent 8-byte fields in order"
        );
    }

    #[test]
    fn rejects_pair_loads_that_clobber_the_entire_address() {
        let error = finalize_function(
            allocated_function(
                vec![Instruction {
                    opcode: Operation::load_pair(PairWidth::DoubleWord),
                    operands: vec![
                        Operand::PhysicalRegister(x86_64::RCX),
                        Operand::PhysicalRegister(x86_64::RDX),
                        address(x86_64::RCX, Some(x86_64::RDX), None),
                        address(x86_64::RCX, Some(x86_64::RDX), Some(8)),
                    ],
                }],
                false,
            ),
            &runtime(),
        )
        .unwrap_err();

        assert_eq!(
            error.message,
            "x86-64 pair-load destinations may not both alias the address"
        );
    }
}
