/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! x86-64 machine instruction finalization.

use super::Opcode;
use crate::frontend::layout::KnownLayoutConstant;
use crate::low_ir::Label;
use crate::target::backend::{Backend, X86_64Backend};
use crate::target::description::ArchitectureOpcode;
use crate::target::description::{
    AssertionOperation, BinaryOperation, BranchOperation, EqualityCondition, FloatCondition, IntegerWidth, MemoryWidth,
    Operation, OverflowOperation, PairWidth, ScalarBranchCondition, ShiftOperation, SignCondition, TestCondition,
    ZeroCondition,
};
use crate::target::description::{FloatConversion, FloatingPointOperation};
use crate::target::finalize_support::{
    AllocatedOperands, Emit, MemoryBranch, finalization_error, finalize_error,
    indexed_pair_store as decode_indexed_pair_store, interpreter_layout, machine_address, pair_access,
    required_runtime_constant, schedule_parallel_moves, verified_label, verified_register,
};
use crate::target::finalize_support::{memory_branch, push_plain_move};
use crate::target::ir::{AllocatedOperand, MachineInstruction, MachineMemoryAddress, MachineOpcode, MachineOperand};
use crate::target::machine_verify::emit_machine_instructions as emit;
use crate::target::registers::{
    PhysicalRegister,
    x86_64::{R11, R15},
};
use crate::{CompileError, ObjectFormat};

fn machine_instruction(opcode: Opcode, operands: Vec<MachineOperand>) -> MachineInstruction {
    MachineInstruction {
        opcode: MachineOpcode::X86_64(opcode),
        operands,
    }
}

fn branch_bit(emit: &mut Emit<'_>, value: PhysicalRegister, bit: i64, condition: TestCondition, target: &Label) {
    use super::Condition;

    if bit == 31 {
        emit!(emit.output, X86_64; Opcode::TestRegister(IntegerWidth::U32) => [register value];);
        emit!(emit.output, X86_64;
            Opcode::JumpCondition(condition.select(Condition::Negative, Condition::NotNegative)) => [label target.clone()];
        );
    } else {
        emit!(emit.output, X86_64; Opcode::BitTest64Immediate => [register value, immediate bit];);
        emit!(emit.output, X86_64;
            Opcode::JumpCondition(condition.select(Condition::Carry, Condition::NotCarry)) => [label target.clone()];
        );
    }
}

enum MaskBranchError {
    UnencodableImmediate(i64),
}

fn branch_mask_immediate(
    emit: &mut Emit<'_>,
    value: PhysicalRegister,
    mask: i64,
    condition: TestCondition,
    target: &Label,
) -> Result<(), MaskBranchError> {
    use super::Condition;

    let branch_condition = if (mask as u64) >> 32 == 0 {
        emit!(emit.output, X86_64; Opcode::Test32Immediate => [register value, immediate mask];);
        condition.select(Condition::NonZero, Condition::Zero)
    } else if (mask as u64).is_power_of_two() {
        emit!(emit.output, X86_64; Opcode::BitTest64Immediate => [register value, immediate (mask as u64).trailing_zeros() as i64];);
        condition.select(Condition::Carry, Condition::NotCarry)
    } else {
        return Err(MaskBranchError::UnencodableImmediate(mask));
    };
    emit!(emit.output, X86_64; Opcode::JumpCondition(branch_condition) => [label target.clone()];);
    Ok(())
}

fn branch_mask_register(
    emit: &mut Emit<'_>,
    value: PhysicalRegister,
    mask: PhysicalRegister,
    condition: TestCondition,
    target: &Label,
) {
    use super::Condition;

    emit!(emit.output, X86_64; Opcode::Test64Registers => [register value, register mask];);
    emit!(emit.output, X86_64;
        Opcode::JumpCondition(condition.select(Condition::NonZero, Condition::Zero)) => [label target.clone()];
    );
}

#[derive(Debug)]
pub(crate) enum CompareImmediateError {
    Unencodable,
}

pub(crate) fn compare_immediate(
    emit: &mut Emit<'_>,
    lhs: PhysicalRegister,
    rhs: i64,
    width: IntegerWidth,
) -> Result<(), CompareImmediateError> {
    if !compare_immediate_encodable(rhs, width) {
        return Err(CompareImmediateError::Unencodable);
    }
    emit!(emit.output, X86_64; Opcode::CompareImmediate(width) => [register lhs, immediate rhs];);
    Ok(())
}

pub(crate) fn compare_register(emit: &mut Emit<'_>, lhs: PhysicalRegister, rhs: PhysicalRegister, width: IntegerWidth) {
    emit!(emit.output, X86_64; Opcode::CompareRegister(width) => [register lhs, register rhs];);
}

pub(crate) fn compare_immediate_encodable(immediate: i64, width: IntegerWidth) -> bool {
    match width {
        IntegerWidth::U8 => i8::try_from(immediate).is_ok() || u8::try_from(immediate).is_ok(),
        IntegerWidth::U16 => i16::try_from(immediate).is_ok() || u16::try_from(immediate).is_ok(),
        IntegerWidth::U32 => i32::try_from(immediate).is_ok() || u32::try_from(immediate).is_ok(),
        IntegerWidth::U64 => i32::try_from(immediate).is_ok(),
    }
}

fn scalar_compare(
    emit: &mut Emit<'_>,
    lhs: PhysicalRegister,
    rhs: &AllocatedOperand,
    width: IntegerWidth,
) -> Result<(), CompileError> {
    match rhs {
        AllocatedOperand::Immediate(rhs) => {
            if compare_immediate(emit, lhs, *rhs, width).is_err() {
                return finalize_error(
                    emit.handler,
                    format!("x86-64 {width:?} comparison immediate is not encodable"),
                );
            }
        }
        rhs => compare_register(emit, lhs, verified_register(rhs), width),
    }
    Ok(())
}

fn multiply(emit: &mut Emit<'_>, width: IntegerWidth, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
    assert_eq!(width, IntegerWidth::U64);
    match operands {
        [destination, source] => {
            push_multiply(emit, verified_register(destination), verified_register(source));
        }
        [destination, source, AllocatedOperand::Immediate(value)]
            if *value > 0 && (*value as u64).is_power_of_two() =>
        {
            let destination = verified_register(destination);
            let source = verified_register(source);
            if destination != source {
                push_move_register(emit, destination, source, IntegerWidth::U64);
            }
            emit!(emit.output, X86_64;
                Opcode::ShiftImmediate {
                operation: ShiftOperation::Left,
                width: IntegerWidth::U64,
            } => [register destination, immediate (*value as u64).trailing_zeros().into()];
            );
        }
        [destination, source, AllocatedOperand::Immediate(value)] => {
            if i32::try_from(*value).is_err() {
                return finalize_error(emit.handler, "x86-64 multiply immediate must fit in signed 32 bits");
            }
            emit!(emit.output, X86_64; Opcode::Multiply64Immediate => [register verified_register(destination), register verified_register(source), immediate *value];);
        }
        [destination, source, multiplier] => {
            let destination = verified_register(destination);
            let source = verified_register(source);
            let multiplier = verified_register(multiplier);
            if destination == source {
                push_multiply(emit, destination, multiplier);
            } else if destination == multiplier {
                push_multiply(emit, destination, source);
            } else {
                push_move_register(emit, destination, source, IntegerWidth::U64);
                push_multiply(emit, destination, multiplier);
            }
        }
        _ => unreachable!("allocated operation and operands were verified"),
    }
    Ok(())
}

pub(crate) fn move_execution_context(emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
    let destination = operands.physical_register(0);
    let source = operands.physical_register(1);
    if destination != source {
        emit!(emit.output, X86_64;
            Opcode::LoadEffectiveAddress => [register destination, address MachineMemoryAddress::offset(source, crate::target::finalize_support::x86_values_offset(emit.runtime, emit.handler)?)];
        );
    }
    Ok(())
}

fn push_move_register(
    emit: &mut Emit<'_>,
    destination: PhysicalRegister,
    source: PhysicalRegister,
    width: IntegerWidth,
) {
    emit!(emit.output, X86_64;
        match width {
        IntegerWidth::U64 => Opcode::Move64Register,
        IntegerWidth::U32 => Opcode::Move32Register,
        IntegerWidth::U8 | IntegerWidth::U16 => {
            unreachable!("unsupported x86-64 move width")
        }
    } => [register destination, register source];
    );
}

fn push_alu_register(
    emit: &mut Emit<'_>,
    operation: super::AluOperation,
    width: IntegerWidth,
    destination: PhysicalRegister,
    source: PhysicalRegister,
) {
    emit!(emit.output, X86_64; Opcode::AluRegister { operation, width } => [register destination, register source];);
}

fn push_alu_memory(
    emit: &mut Emit<'_>,
    operation: super::AluOperation,
    width: IntegerWidth,
    destination: PhysicalRegister,
    source: MachineMemoryAddress,
) {
    emit!(emit.output, X86_64; Opcode::AluMemory { operation, width } => [register destination, address source];);
}

fn push_alu_immediate(
    emit: &mut Emit<'_>,
    operation: super::AluOperation,
    width: IntegerWidth,
    destination: PhysicalRegister,
    value: i64,
) {
    emit!(emit.output, X86_64; Opcode::AluImmediate { operation, width } => [register destination, immediate value];);
}

fn push_multiply(emit: &mut Emit<'_>, destination: PhysicalRegister, source: PhysicalRegister) {
    emit!(emit.output, X86_64; Opcode::Multiply64Register => [register destination, register source];);
}

fn branch_value_representation(
    emit: &mut Emit<'_>,
    value: PhysicalRegister,
    representation: i64,
    scratch: PhysicalRegister,
    condition: EqualityCondition,
    target: &Label,
) {
    use super::Condition;

    debug_assert!(compare_immediate_encodable(representation, IntegerWidth::U16));
    emit!(emit.output, X86_64; Opcode::Move64Register => [register scratch, register value];);
    emit!(emit.output, X86_64;
        Opcode::ShiftImmediate {
        operation: ShiftOperation::RightLogical,
        width: IntegerWidth::U64,
    } => [register scratch, immediate 48];
    );
    compare_immediate(emit, scratch, representation, IntegerWidth::U16).expect("value representation was checked");
    emit!(emit.output, X86_64;
        Opcode::JumpCondition(condition.select(Condition::Equal, Condition::NotEqual)) => [label target.clone()];
    );
}

enum AnyEqualBranchError {
    UnencodableImmediate,
}

fn branch_any_equal(
    emit: &mut Emit<'_>,
    value: PhysicalRegister,
    comparison_values: &[AllocatedOperand],
    target: &Label,
) -> Result<(), AnyEqualBranchError> {
    use super::Condition;

    for comparison_value in comparison_values {
        match comparison_value {
            AllocatedOperand::Immediate(immediate) => {
                compare_immediate(emit, value, *immediate, IntegerWidth::U64)
                    .map_err(|_| AnyEqualBranchError::UnencodableImmediate)?;
            }
            comparison_value => {
                compare_register(emit, value, verified_register(comparison_value), IntegerWidth::U64);
            }
        }
        emit!(emit.output, X86_64; Opcode::JumpCondition(Condition::Equal) => [label target.clone()];);
    }
    Ok(())
}

fn branch_scalar(emit: &mut Emit<'_>, condition: ScalarBranchCondition, target: &Label) {
    use super::Condition;
    let condition = Condition::from_scalar(condition);
    emit!(emit.output, X86_64; Opcode::JumpCondition(condition) => [label target.clone()];);
}

fn immediate_scalar_branch_taken(lhs: i64, rhs: i64, width: IntegerWidth, condition: ScalarBranchCondition) -> bool {
    let mask = match width {
        IntegerWidth::U16 => u16::MAX as u64,
        IntegerWidth::U32 => u32::MAX as u64,
        IntegerWidth::U64 => u64::MAX,
        _ => unreachable!("allocated scalar branch was verified"),
    };
    let lhs = lhs as u64 & mask;
    let rhs = rhs as u64 & mask;
    match condition {
        ScalarBranchCondition::Equal => lhs == rhs,
        ScalarBranchCondition::NotEqual => lhs != rhs,
        ScalarBranchCondition::Unsigned(relation) => ordered_relation_holds(lhs, rhs, relation),
        ScalarBranchCondition::Signed(relation) => {
            let lhs = match width {
                IntegerWidth::U16 => lhs as i16 as i64,
                IntegerWidth::U32 => lhs as i32 as i64,
                IntegerWidth::U64 => lhs as i64,
                _ => unreachable!("allocated scalar branch was verified"),
            };
            let rhs = match width {
                IntegerWidth::U16 => rhs as i16 as i64,
                IntegerWidth::U32 => rhs as i32 as i64,
                IntegerWidth::U64 => rhs as i64,
                _ => unreachable!("allocated scalar branch was verified"),
            };
            ordered_relation_holds(lhs, rhs, relation)
        }
    }
}

fn ordered_relation_holds<T: PartialOrd>(lhs: T, rhs: T, relation: crate::intrinsic::ComparisonRelation) -> bool {
    use crate::intrinsic::ComparisonRelation;

    match relation {
        ComparisonRelation::Less => lhs < rhs,
        ComparisonRelation::LessOrEqual => lhs <= rhs,
        ComparisonRelation::Greater => lhs > rhs,
        ComparisonRelation::GreaterOrEqual => lhs >= rhs,
    }
}

pub(crate) enum StoreSource {
    Immediate(i64),
    Register(PhysicalRegister),
}

pub(crate) fn store(
    emit: &mut Emit<'_>,
    width: MemoryWidth,
    address: MachineMemoryAddress,
    source: StoreSource,
    value_scratch: Option<PhysicalRegister>,
) {
    let source = match source {
        StoreSource::Immediate(value)
            if width == MemoryWidth::DoubleWord && !(i32::MIN as i64..=i32::MAX as i64).contains(&value) =>
        {
            let value_scratch = value_scratch.expect("selected large immediate store has a scratch");
            emit!(emit.output, X86_64;
                if (value as u64) <= u32::MAX as u64 {
                Opcode::Move32Immediate
            } else {
                Opcode::MoveAbsolute64Immediate
            } => [register value_scratch, immediate value];
            );
            MachineOperand::PhysicalRegister(value_scratch)
        }
        StoreSource::Immediate(value) => MachineOperand::Immediate(match width {
            MemoryWidth::Byte => (value as u8).into(),
            MemoryWidth::HalfWord => (value as u16).into(),
            MemoryWidth::Word => (value as u32).into(),
            MemoryWidth::DoubleWord => value,
            MemoryWidth::Float => {
                unreachable!("floating immediate store was validated")
            }
        }),
        StoreSource::Register(register) => MachineOperand::PhysicalRegister(register),
    };
    emit!(emit.output, X86_64; Opcode::Store(width) => [address address, operand source];);
}

pub(crate) fn scalar_store(
    emit: &mut Emit<'_>,
    width: MemoryWidth,
    address: MachineMemoryAddress,
    source: &AllocatedOperand,
    value_scratch: Option<PhysicalRegister>,
) {
    let source = match source {
        AllocatedOperand::Immediate(value) => StoreSource::Immediate(*value),
        source => StoreSource::Register(verified_register(source)),
    };
    store(emit, width, address, source, value_scratch);
}

pub(crate) fn double_to_int32(
    emit: &mut Emit<'_>,
    destination: PhysicalRegister,
    source: PhysicalRegister,
    gpr_scratch: PhysicalRegister,
    fpr_scratch: PhysicalRegister,
    failure: Label,
) {
    use super::{Condition, FloatConversion};

    emit!(emit.output, X86_64;
        Opcode::FloatConversion(FloatConversion::DoubleToSigned32Truncate) => [register destination, register source];
        Opcode::SignExtend32To64 => [register gpr_scratch, register destination];
        Opcode::FloatConversion(FloatConversion::Signed64ToDouble) => [register fpr_scratch, register gpr_scratch];
        Opcode::CompareDouble => [register source, register fpr_scratch];
        Opcode::JumpParity => [label failure.clone()];
        Opcode::JumpCondition(Condition::NotEqual) => [label failure];
    );
}

pub(crate) fn js_to_int32(
    emit: &mut Emit<'_>,
    destination: PhysicalRegister,
    source: PhysicalRegister,
    gpr_scratch: PhysicalRegister,
    failure: Label,
) {
    use super::{Condition, FloatConversion};

    emit!(emit.output, X86_64;
        Opcode::FloatConversion(FloatConversion::DoubleToSigned64Truncate) => [register destination, register source];
        Opcode::Move64ImmediateHex => [register gpr_scratch, immediate i64::MIN];
        Opcode::CompareRegister(IntegerWidth::U64) => [register destination, register gpr_scratch];
        Opcode::JumpCondition(Condition::Equal) => [label failure];
        Opcode::Move32Register => [register destination, register destination];
    );
}

fn assertion_compare(
    emit: &mut Emit<'_>,
    lhs: &AllocatedOperand,
    rhs: &AllocatedOperand,
    width: IntegerWidth,
) -> Result<(), CompileError> {
    scalar_compare(emit, verified_register(lhs), rhs, width)
}

fn push_overflow_multiply(emit: &mut Emit<'_>, destination: PhysicalRegister, source: PhysicalRegister) {
    emit!(emit.output, X86_64; Opcode::Multiply32 => [register destination, register source];);
}

fn vm_load(emit: &mut Emit<'_>, destination: PhysicalRegister) {
    use crate::target::registers::x86_64::RBP;

    let displacement = match emit.object_format {
        ObjectFormat::Coff => super::WIN64_VM_SLOT_OFFSET,
        ObjectFormat::Elf | ObjectFormat::MachO => super::SYSV_VM_SLOT_OFFSET,
    };
    emit!(emit.output, X86_64;
        Opcode::Load {
        width: MemoryWidth::DoubleWord,
        signed: false,
    } => [register destination, address MachineMemoryAddress::offset(RBP, i64::from(displacement))];
    );
}

fn operand_store(emit: &mut Emit<'_>, offset: i64, source: &AllocatedOperand, scratches: &[AllocatedOperand]) {
    use crate::target::registers::x86_64::{R13, R14, RBX};

    let (index, source) = match source {
        AllocatedOperand::Immediate(value) if !(i32::MIN as i64..=i32::MAX as i64).contains(value) => {
            let [index, value_scratch] = scratches else {
                unreachable!("allocated scratch arity was verified");
            };
            let index = verified_register(index);
            let value_scratch = verified_register(value_scratch);
            emit!(emit.output, X86_64;
                if (*value as u64) <= u32::MAX as u64 {
                Opcode::Move32Immediate
            } else {
                Opcode::MoveAbsolute64Immediate
            } => [register value_scratch, immediate *value];
            );
            (index, StoreSource::Register(value_scratch))
        }
        AllocatedOperand::Immediate(value) => {
            let [index] = scratches else {
                unreachable!("allocated scratch arity was verified");
            };
            (verified_register(index), StoreSource::Immediate(*value))
        }
        source => {
            let [index] = scratches else {
                unreachable!("allocated scratch arity was verified");
            };
            let index = verified_register(index);
            let source = verified_register(source);
            (index, StoreSource::Register(source))
        }
    };

    emit!(emit.output, X86_64;
        Opcode::Load {
        width: MemoryWidth::Word,
        signed: false,
    } => [register index, address MachineMemoryAddress::indexed_offset(R14, R13, offset)];
    );
    store(
        emit,
        MemoryWidth::DoubleWord,
        MachineMemoryAddress::scaled(RBX, index, 8, 0),
        source,
        None,
    );
}

fn pair_load(
    emit: &mut Emit<'_>,
    width: PairWidth,
    destinations: [PhysicalRegister; 2],
    first_address: MachineMemoryAddress,
) -> Result<(), CompileError> {
    let [first, second] = destinations;
    let aliases_address = |register| first_address.base == register || first_address.index == Some(register);
    let first_aliases_address = aliases_address(first);
    if first_aliases_address && aliases_address(second) {
        return finalize_error(
            emit.handler,
            "x86-64 pair-load destinations may not both alias the address",
        );
    }
    let memory_width = width.into();
    let second_address = pair_second_address(&first_address, width);
    let first_instruction = machine_instruction(
        Opcode::Load {
            width: memory_width,
            signed: false,
        },
        vec![
            MachineOperand::PhysicalRegister(first),
            MachineOperand::Address(first_address),
        ],
    );
    let second_instruction = machine_instruction(
        Opcode::Load {
            width: memory_width,
            signed: false,
        },
        vec![
            MachineOperand::PhysicalRegister(second),
            MachineOperand::Address(second_address),
        ],
    );
    if first_aliases_address {
        emit.output.extend([second_instruction, first_instruction]);
    } else {
        emit.output.extend([first_instruction, second_instruction]);
    }
    Ok(())
}

fn pair_store(
    emit: &mut Emit<'_>,
    width: PairWidth,
    sources: [PhysicalRegister; 2],
    first_address: MachineMemoryAddress,
) {
    let [first, second] = sources;
    let memory_width = width.into();
    let second_address = pair_second_address(&first_address, width);
    store(emit, memory_width, first_address, StoreSource::Register(first), None);
    store(emit, memory_width, second_address, StoreSource::Register(second), None);
}

fn indexed_pair_store(
    emit: &mut Emit<'_>,
    address_registers: [PhysicalRegister; 2],
    scale: i64,
    sources: [PhysicalRegister; 2],
) {
    let [base, index] = address_registers;
    let first_address = MachineMemoryAddress {
        base,
        index: Some(index),
        scale: Some(scale),
        displacement: None,
    };
    pair_store(emit, PairWidth::DoubleWord, sources, first_address);
}

fn pair_second_address(first_address: &MachineMemoryAddress, width: PairWidth) -> MachineMemoryAddress {
    let element_size = match width {
        PairWidth::Word => 4,
        PairWidth::DoubleWord => 8,
    };
    let displacement = first_address.displacement.unwrap_or(0) + element_size;
    MachineMemoryAddress {
        displacement: (displacement != 0).then_some(displacement),
        ..first_address.clone()
    }
}

pub(crate) fn direct_call(emit: &mut Emit<'_>, function: crate::low_ir::Relocation) {
    emit!(emit.output, X86_64; Opcode::CallDirect => [relocation function];);
}

pub(crate) fn indirect_call(emit: &mut Emit<'_>, function: PhysicalRegister) {
    emit!(emit.output, X86_64; Opcode::CallRegister => [register function];);
}

pub(crate) fn interpreter_call_arguments(emit: &mut Emit<'_>) {
    use crate::target::registers::x86_64::{R8, R13, R14, RCX, RDI, RDX, RSI};

    let (vm, pc, instruction) = if emit.object_format == ObjectFormat::Coff {
        (RCX, RDX, R8)
    } else {
        (RDI, RSI, RDX)
    };
    vm_load(emit, vm);
    emit!(emit.output, X86_64;
        Opcode::Move32Register => [register pc, register R13];
        Opcode::LoadEffectiveAddress => [register instruction, address MachineMemoryAddress::indexed(R14, R13)];
    );
}

fn store_slow_path_program_counter(emit: &mut Emit<'_>) -> Result<(), CompileError> {
    use crate::target::registers::x86_64::{R13, RBX};

    let [_, _, program_counter, _, values_offset] = interpreter_layout(emit.runtime, emit.handler)?;
    let program_counter_from_values = program_counter
        .checked_sub(values_offset)
        .ok_or_else(|| finalization_error(emit.handler, "execution-context program-counter offset overflow"))?;
    emit!(emit.output, X86_64; Opcode::StoreAdditiveDisplacement(MemoryWidth::Word) => [address MachineMemoryAddress::offset(RBX, program_counter_from_values), register R13];);
    Ok(())
}

fn finish_slow_path_call(
    emit: &mut Emit<'_>,
    result_scratch: PhysicalRegister,
    state_scratch: PhysicalRegister,
) -> Result<(), CompileError> {
    use crate::target::registers::x86_64::{R13, R14, RBX};

    let [execution_context, executable, _, bytecode, values_offset] = interpreter_layout(emit.runtime, emit.handler)?;
    emit!(emit.output, X86_64;
        Opcode::TestRegister(IntegerWidth::U64) => [register result_scratch];
        Opcode::JumpNegativeToExit => [];
    );

    vm_load(emit, state_scratch);
    push_load(
        emit,
        RBX,
        MachineMemoryAddress::offset(state_scratch, execution_context),
    );
    emit!(emit.output, X86_64;
        Opcode::AluImmediate {
        operation: super::AluOperation::Add,
        width: IntegerWidth::U64,
    } => [register RBX, immediate values_offset];
    );
    let executable_from_values = executable
        .checked_sub(values_offset)
        .ok_or_else(|| finalization_error(emit.handler, "execution-context executable offset overflow"))?;
    emit!(emit.output, X86_64;
        Opcode::LoadAdditiveDisplacement {
        width: MemoryWidth::DoubleWord,
        signed: false,
    } => [register state_scratch, address MachineMemoryAddress::offset(RBX, executable_from_values)];
    );
    push_load(emit, R14, MachineMemoryAddress::offset(state_scratch, bytecode));
    emit!(emit.output, X86_64; Opcode::Move32Register => [register R13, register result_scratch];);
    dispatch_from_instruction_pointer(emit, result_scratch);
    Ok(())
}

fn push_register_move(emit: &mut Emit<'_>, destination: PhysicalRegister, source: PhysicalRegister) {
    emit!(emit.output, X86_64; Opcode::Move64Register => [register destination, register source];);
}

fn push_parallel_register_moves(
    emit: &mut Emit<'_>,
    requested_moves: &[(PhysicalRegister, PhysicalRegister)],
    scratch: PhysicalRegister,
) {
    for (destination, source) in schedule_parallel_moves(requested_moves, scratch) {
        push_register_move(emit, destination, source);
    }
}

fn push_load(emit: &mut Emit<'_>, destination: PhysicalRegister, address: MachineMemoryAddress) {
    emit!(emit.output, X86_64;
        Opcode::Load {
        width: MemoryWidth::DoubleWord,
        signed: false,
    } => [register destination, address address];
    );
}

fn push_stack_adjustment(emit: &mut Emit<'_>, operation: super::AluOperation, value: i64) {
    use crate::target::registers::x86_64::RSP;

    emit!(emit.output, X86_64;
        Opcode::AluImmediate {
        operation,
        width: IntegerWidth::U64,
    } => [register RSP, immediate value];
    );
}

fn dispatch_from_instruction_pointer(emit: &mut Emit<'_>, opcode_scratch: PhysicalRegister) {
    use crate::target::registers::x86_64::{R12, R13, R14};

    emit!(emit.output, X86_64;
        Opcode::Load {
            width: MemoryWidth::Byte,
            signed: false,
        } => [register opcode_scratch, address MachineMemoryAddress::indexed(R14, R13)];
        Opcode::JumpMemory => [address MachineMemoryAddress::scaled(R12, opcode_scratch, 8, 0)];
    );
}

impl Backend for X86_64Backend {
    fn branch_float(
        &self,
        emit: &mut Emit<'_>,
        lhs: PhysicalRegister,
        rhs: PhysicalRegister,
        emit_compare: bool,
        condition: FloatCondition,
        target: &Label,
    ) {
        if emit_compare {
            emit!(emit.output, X86_64; Opcode::CompareDouble => [register lhs, register rhs];);
        }
        emit!(emit.output, X86_64;
            Opcode::JumpCondition(crate::target::ir::MachineCondition::from_float(condition)) => [label target.clone()];
        );
    }

    fn branch_zero(
        &self,
        emit: &mut Emit<'_>,
        value: PhysicalRegister,
        width: IntegerWidth,
        condition: ZeroCondition,
        target: &Label,
    ) {
        use super::Condition;

        emit!(emit.output, X86_64;
            Opcode::TestRegister(width) => [register value];
            Opcode::JumpCondition(condition.select(Condition::Zero, Condition::NonZero)) => [label target.clone()];
        );
    }

    fn branch_sign(
        &self,
        emit: &mut Emit<'_>,
        value: PhysicalRegister,
        width: IntegerWidth,
        condition: SignCondition,
        target: &Label,
    ) {
        use super::Condition;

        emit!(emit.output, X86_64;
            Opcode::TestRegister(width) => [register value];
            Opcode::JumpCondition(condition.select(Condition::Negative, Condition::NotNegative)) => [label target.clone()];
        );
    }

    fn box_int32(
        &self,
        emit: &mut Emit<'_>,
        destination: PhysicalRegister,
        source: PhysicalRegister,
        clean: bool,
    ) -> Result<(), CompileError> {
        let int32_tag_shifted = emit.constant(KnownLayoutConstant::Int32TagShifted)?;
        if !clean || destination != source {
            emit!(emit.output, X86_64; if clean { Opcode::Move64Register } else { Opcode::Move32Register } => [register destination, register source];);
        }
        emit!(emit.output, X86_64;
            Opcode::MoveAbsolute64Immediate => [register R11, immediate int32_tag_shifted];
            Opcode::Or64Register => [register destination, register R11];
        );
        Ok(())
    }

    fn update_bit(&self, emit: &mut Emit<'_>, destination: PhysicalRegister, bit: u32, operation: Operation) {
        emit!(emit.output, X86_64; if operation == Operation::ToggleBit { Opcode::ComplementBit } else { Opcode::ResetBit } => [register destination, immediate bit.into()];);
    }

    fn extract_tag(&self, emit: &mut Emit<'_>, destination: PhysicalRegister, source: PhysicalRegister) {
        if destination != source {
            emit!(emit.output, X86_64; Opcode::Move64Register => [register destination, register source];);
        }
        emit!(emit.output, X86_64; Opcode::ShiftImmediate { operation: ShiftOperation::RightLogical, width: IntegerWidth::U64 } => [register destination, immediate 48];);
    }

    fn unbox_object(
        &self,
        emit: &mut Emit<'_>,
        destination: PhysicalRegister,
        source: PhysicalRegister,
    ) -> Result<(), CompileError> {
        if destination != source {
            emit!(emit.output, X86_64; Opcode::Move64Register => [register destination, register source];);
        }
        let heap_region_offset_mask = emit.constant(KnownLayoutConstant::HeapRegionOffsetMask)?;
        emit!(emit.output, X86_64;
            Opcode::MoveAbsolute64Immediate => [register R11, immediate heap_region_offset_mask];
            Opcode::AluRegister { operation: super::AluOperation::And, width: IntegerWidth::U64 } => [register destination, register R11];
        );
        emit!(emit.output, X86_64;
            Opcode::AluRegister { operation: super::AluOperation::Add, width: IntegerWidth::U64 } => [register destination, register R15];
        );
        Ok(())
    }

    fn float_operation(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        _operation: FloatingPointOperation,
        operands: &[AllocatedOperand],
    ) {
        emit.output.push(MachineInstruction {
            opcode,
            operands: operands.to_vec(),
        });
    }

    fn checked_float_conversion(
        &self,
        emit: &mut Emit<'_>,
        operation: FloatingPointOperation,
        destination: PhysicalRegister,
        source: PhysicalRegister,
        scratches: &[AllocatedOperand],
        failure: &Label,
    ) {
        match (operation, scratches) {
            (FloatingPointOperation::Convert(FloatConversion::Float64ToInt64), [fpr_scratch]) => {
                use super::{Condition, FloatConversion};
                emit!(emit.output, X86_64;
                    Opcode::FloatConversion(FloatConversion::DoubleToSigned64Truncate) => [register destination, register source];
                    Opcode::FloatConversion(FloatConversion::Signed64ToDouble) => [register verified_register(fpr_scratch), register destination];
                    Opcode::CompareDouble => [register source, register verified_register(fpr_scratch)];
                    Opcode::JumpParity => [label failure.clone()];
                    Opcode::JumpCondition(Condition::NotEqual) => [label failure.clone()];
                );
            }
            (FloatingPointOperation::Convert(FloatConversion::Float64ToInt32), [gpr_scratch, fpr_scratch]) => {
                double_to_int32(
                    emit,
                    destination,
                    source,
                    verified_register(gpr_scratch),
                    verified_register(fpr_scratch),
                    failure.clone(),
                )
            }
            (FloatingPointOperation::Convert(FloatConversion::JavaScriptToInt32), [gpr_scratch]) => {
                js_to_int32(
                    emit,
                    destination,
                    source,
                    verified_register(gpr_scratch),
                    failure.clone(),
                );
            }
            _ => unreachable!("allocated operand shape was verified"),
        }
    }

    fn helper_call(&self, emit: &mut Emit<'_>, function: crate::low_ir::Relocation) {
        use crate::target::registers::x86_64::{RCX, RDI};

        if emit.object_format != ObjectFormat::Coff {
            emit!(emit.output, X86_64; Opcode::Move64Register => [register RDI, register RCX];);
        }
        direct_call(emit, function);
    }

    fn interpreter_call(&self, emit: &mut Emit<'_>, function: crate::low_ir::Relocation) {
        interpreter_call_arguments(emit);
        direct_call(emit, function);
    }

    fn raw_native_call(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) {
        let [function, scratch] = [operands.physical_register(0), operands.physical_register(3)];
        use crate::target::registers::x86_64::{RAX, RBP, RCX, RDI, RDX, RSI, RSP};

        push_register_move(emit, scratch, function);

        match emit.object_format {
            ObjectFormat::Coff => {
                emit!(emit.output, X86_64; Opcode::LoadEffectiveAddress => [register RCX, address MachineMemoryAddress::offset(RBP, super::WIN64_RAW_NATIVE_RETURN_SLOT)];);
                vm_load(emit, RDX);
                indirect_call(emit, scratch);
                push_load(
                    emit,
                    RAX,
                    MachineMemoryAddress::offset(RBP, super::WIN64_RAW_NATIVE_RETURN_SLOT),
                );
                push_load(
                    emit,
                    RDX,
                    MachineMemoryAddress::offset(RBP, super::WIN64_RAW_NATIVE_VARIANT_SLOT),
                );
            }
            ObjectFormat::MachO => {
                push_stack_adjustment(emit, super::AluOperation::Subtract, 16);
                push_register_move(emit, RDI, RSP);
                vm_load(emit, RSI);
                indirect_call(emit, scratch);
                push_load(emit, RAX, MachineMemoryAddress::offset(RSP, 0));
                push_load(emit, RDX, MachineMemoryAddress::offset(RSP, 8));
                push_stack_adjustment(emit, super::AluOperation::Add, 16);
            }
            ObjectFormat::Elf => {
                vm_load(emit, RDI);
                indirect_call(emit, scratch);
            }
        }
        push_register_move(emit, RCX, RDX);
    }

    fn slow_path_call(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
        let function = operands.relocation(0);
        let [result_scratch, state_scratch] = [operands.physical_register(1), operands.physical_register(2)];

        store_slow_path_program_counter(emit)?;
        interpreter_call_arguments(emit);
        direct_call(emit, function);
        finish_slow_path_call(emit, result_scratch, state_scratch)
    }

    fn binary_slow_path_call(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
        let function = operands.relocation(0);
        let [destination, lhs, rhs] = [
            operands.physical_register(1),
            operands.physical_register(2),
            operands.physical_register(3),
        ];
        let [result_scratch, state_scratch] = [operands.physical_register(4), operands.physical_register(5)];
        use crate::target::registers::x86_64::{R8, R9, R13, RBP, RCX, RDI, RDX, RSI};

        store_slow_path_program_counter(emit)?;
        if emit.object_format == ObjectFormat::Coff {
            store(
                emit,
                MemoryWidth::DoubleWord,
                MachineMemoryAddress::offset(RBP, super::WIN64_RAW_NATIVE_RETURN_SLOT),
                StoreSource::Register(rhs),
                None,
            );
            push_parallel_register_moves(emit, &[(R8, destination), (R9, lhs)], state_scratch);
            vm_load(emit, RCX);
            emit!(emit.output, X86_64; Opcode::Move32Register => [register RDX, register R13];);
        } else {
            push_parallel_register_moves(emit, &[(RDX, destination), (RCX, lhs), (R8, rhs)], state_scratch);
            vm_load(emit, RDI);
            emit!(emit.output, X86_64; Opcode::Move32Register => [register RSI, register R13];);
        }
        direct_call(emit, function);
        finish_slow_path_call(emit, result_scratch, state_scratch)
    }

    fn jump_slow_path_call(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
        let function = operands.relocation(0);
        let [lhs, rhs, true_target, false_target] = [
            operands.physical_register(1),
            operands.physical_register(2),
            operands.physical_register(3),
            operands.physical_register(4),
        ];
        let [result_scratch, state_scratch] = [operands.physical_register(5), operands.physical_register(6)];
        use crate::target::registers::x86_64::{R8, R9, R13, RBP, RCX, RDI, RDX, RSI};

        store_slow_path_program_counter(emit)?;
        if emit.object_format == ObjectFormat::Coff {
            store(
                emit,
                MemoryWidth::DoubleWord,
                MachineMemoryAddress::offset(RBP, super::WIN64_RAW_NATIVE_RETURN_SLOT),
                StoreSource::Register(true_target),
                None,
            );
            store(
                emit,
                MemoryWidth::DoubleWord,
                MachineMemoryAddress::offset(RBP, super::WIN64_RAW_NATIVE_VARIANT_SLOT),
                StoreSource::Register(false_target),
                None,
            );
            push_parallel_register_moves(emit, &[(R8, lhs), (R9, rhs)], state_scratch);
            vm_load(emit, RCX);
            emit!(emit.output, X86_64; Opcode::Move32Register => [register RDX, register R13];);
        } else {
            push_parallel_register_moves(
                emit,
                &[(RDX, lhs), (RCX, rhs), (R8, true_target), (R9, false_target)],
                state_scratch,
            );
            vm_load(emit, RDI);
            emit!(emit.output, X86_64; Opcode::Move32Register => [register RSI, register R13];);
        }
        direct_call(emit, function);
        finish_slow_path_call(emit, result_scratch, state_scratch)
    }

    fn dispatch_current(&self, emit: &mut Emit<'_>, scratches: &[AllocatedOperand]) -> Result<(), CompileError> {
        dispatch_from_instruction_pointer(emit, scratches.physical_register(0));
        Ok(())
    }

    fn dispatch_variable(
        &self,
        emit: &mut Emit<'_>,
        size: PhysicalRegister,
        scratches: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let opcode_scratch = scratches.physical_register(0);
        use crate::target::registers::x86_64::R13;

        emit!(emit.output, X86_64;
            Opcode::AluRegister {
            operation: super::AluOperation::Add,
            width: IntegerWidth::U32,
        } => [register R13, register size];
        );
        dispatch_from_instruction_pointer(emit, opcode_scratch);
        Ok(())
    }

    fn dispatch_next(
        &self,
        emit: &mut Emit<'_>,
        size: u32,
        scratches: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let opcode_scratch = scratches.physical_register(0);
        use crate::target::registers::x86_64::R13;

        emit!(emit.output, X86_64;
            Opcode::AluImmediate {
            operation: super::AluOperation::Add,
            width: IntegerWidth::U32,
        } => [register R13, immediate size.into()];
        );
        dispatch_from_instruction_pointer(emit, opcode_scratch);
        Ok(())
    }

    fn finalize_assertion(
        &self,
        emit: &mut Emit<'_>,
        operation: AssertionOperation,
        operands: &[AllocatedOperand],
        ok_label: Option<Label>,
    ) -> Result<(), CompileError> {
        let Some(ok_label) = ok_label else {
            return Ok(());
        };
        use super::Condition;

        let condition = match operation {
            AssertionOperation::UnsignedLess | AssertionOperation::UnsignedGreaterOrEqual => {
                assertion_compare(emit, operands.operand(0), operands.operand(1), IntegerWidth::U64)?;
                Condition::from_assertion(operation)
            }
            AssertionOperation::NonZero => {
                emit!(emit.output, X86_64; Opcode::TestRegister(IntegerWidth::U64) => [register operands.physical_register(0)];);
                Condition::NonZero
            }
            AssertionOperation::TagEqual | AssertionOperation::TagNotEqual => {
                let value = operands.physical_register(0);
                let tag = operands.operand(1);
                let scratch = operands.physical_register(2);
                emit!(emit.output, X86_64;
                    Opcode::Move64Register => [register scratch, register value];
                    Opcode::ShiftImmediate { operation: ShiftOperation::RightLogical, width: IntegerWidth::U64 } => [register scratch, immediate 48];
                );
                scalar_compare(emit, scratch, tag, IntegerWidth::U64)?;
                Condition::from_assertion(operation)
            }
        };
        emit!(emit.output, X86_64;
            Opcode::JumpCondition(condition) => [label ok_label.clone()];
            Opcode::UndefinedInstruction => [];
            Opcode::Label => [label ok_label];
        );
        Ok(())
    }

    fn finalize_scalar_compare_branch(
        &self,
        emit: &mut Emit<'_>,
        operation: Operation,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let (width, condition) = operation.scalar_branch().expect("allocated scalar branch was verified");
        if let [
            AllocatedOperand::Immediate(lhs),
            AllocatedOperand::Immediate(rhs),
            AllocatedOperand::Label(target),
        ] = operands
        {
            if immediate_scalar_branch_taken(*lhs, *rhs, width, condition) {
                emit!(emit.output, X86_64; Opcode::Jump => [label target.clone()];);
            }
            return Ok(());
        }
        let (lhs, rhs) = if let Some(lhs) = operands.operand(0).physical_register() {
            (lhs, operands.operand(1))
        } else if matches!(operation, Operation::Branch(BranchOperation::Equality { .. }))
            && let Some(rhs) = operands.operand(1).physical_register()
        {
            (rhs, operands.operand(0))
        } else {
            return emit.error("scalar comparison requires at least one register operand");
        };
        scalar_compare(emit, lhs, rhs, width)?;
        branch_scalar(emit, condition, &operands.label(2));
        Ok(())
    }

    fn finalize_bit_branch(
        &self,
        emit: &mut Emit<'_>,
        operation: Operation,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let condition = match operation {
            Operation::Branch(BranchOperation::Bits {
                width: MemoryWidth::DoubleWord,
                condition,
            }) => condition,
            Operation::Branch(BranchOperation::Bit(condition)) => condition,
            _ => unreachable!("allocated operation and operands were verified"),
        };
        let value = operands.physical_register(0);
        let mask_or_bit = operands.operand(1);
        let target = operands.label(2);
        if operation == Operation::branch_bit(condition) {
            branch_bit(emit, value, operands.immediate(1), condition, &target);
            return Ok(());
        }
        match mask_or_bit {
            AllocatedOperand::Immediate(mask) => {
                if let Err(MaskBranchError::UnencodableImmediate(mask)) =
                    branch_mask_immediate(emit, value, *mask, condition, &target)
                {
                    return finalize_error(
                        emit.handler,
                        format!("x86-64 bit mask {:#x} is not encodable", mask as u64),
                    );
                }
            }
            mask => {
                let mask = verified_register(mask);
                branch_mask_register(emit, value, mask, condition, &target);
            }
        }
        Ok(())
    }

    fn finalize_value_representation_branch(
        &self,
        emit: &mut Emit<'_>,
        operation: Operation,
        operands: &[AllocatedOperand],
    ) {
        let condition = match operation {
            Operation::Branch(BranchOperation::Tag(condition) | BranchOperation::Singleton(condition)) => condition,
            _ => unreachable!("allocated operation and operands were verified"),
        };
        let value = operands.physical_register(0);
        let representation = operands.immediate(1);
        let scratch = operands.physical_register(2);
        let target = operands.label(3);
        branch_value_representation(emit, value, representation, scratch, condition, &target);
    }

    fn branch_memory(
        &self,
        emit: &mut Emit<'_>,
        operation: Operation,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        use super::Condition;

        let address = machine_address(operands.operand(0));
        let branch = memory_branch(operation, operands, None, emit.handler)?;
        let target = &operands.label(2);

        let condition = match branch {
            MemoryBranch::CompareRegister { width, condition, rhs } => {
                emit!(emit.output, X86_64; Opcode::CompareMemoryRegister(width) => [address address, register rhs];);
                condition.select(Condition::Equal, Condition::NotEqual)
            }
            MemoryBranch::CompareByte { condition, immediate } => {
                emit!(emit.output, X86_64; Opcode::CompareMemoryImmediate(IntegerWidth::U8) => [address address, immediate immediate];);
                condition.select(Condition::Zero, Condition::NonZero)
            }
            MemoryBranch::TestByte {
                condition, immediate, ..
            } => {
                emit!(emit.output, X86_64; Opcode::TestMemoryImmediate(MemoryWidth::Byte) => [address address, immediate immediate];);
                condition.select(Condition::NonZero, Condition::Zero)
            }
        };
        emit!(emit.output, X86_64; Opcode::JumpCondition(condition) => [label target.clone()];);
        Ok(())
    }

    fn finalize_any_equal_branch(
        &self,
        emit: &mut Emit<'_>,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let [value, comparison_values @ .., AllocatedOperand::Label(target)] = operands else {
            unreachable!("allocated operand shape was verified");
        };
        match branch_any_equal(emit, verified_register(value), comparison_values, target) {
            Ok(()) => Ok(()),
            Err(AnyEqualBranchError::UnencodableImmediate) => {
                finalize_error(emit.handler, "x86-64 U64 comparison immediate is not encodable")
            }
        }
    }

    fn finalize_canonicalize_nan(
        &self,
        emit: &mut Emit<'_>,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let (fixup_label, return_label) = emit.unique_label_pair("canon_nan");
        let [destination, source] = operands.physical_registers();
        let canonical_nan =
            required_runtime_constant(emit.runtime, KnownLayoutConstant::CanonicalNanBits, emit.handler)?;
        emit!(emit.output, X86_64;
            Opcode::MoveFloatBits64 => [register destination, register source];
            Opcode::CompareDouble => [register source, register source];
            Opcode::JumpParity => [label fixup_label.clone()];
            Opcode::Label => [label return_label.clone()];
        );
        emit!(emit.cold, X86_64;
            Opcode::Label => [label fixup_label];
            Opcode::MoveAbsolute64Immediate => [register destination, immediate canonical_nan];
            Opcode::Jump => [label return_label];
        );
        Ok(())
    }

    fn move_instruction(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        width: IntegerWidth,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        if opcode == ArchitectureOpcode::X86_64(Opcode::MoveExecutionContext) {
            return move_execution_context(emit, operands);
        }
        push_plain_move(emit, opcode, width, operands);
        Ok(())
    }

    fn finalize_effective_address(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let opcode = opcode.x86_64();
        if opcode == Opcode::LoadValuesAddress {
            return Ok(());
        }
        let destination = operands.physical_register(0);
        let address = machine_address(operands.operand(1));
        emit!(emit.output, X86_64; opcode => [register destination, address address];);
        Ok(())
    }

    fn finalize_vm_load(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) {
        let destination = operands.physical_register(0);
        vm_load(emit, destination);
    }

    fn finalize_program_counter_load(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) {
        use crate::target::registers::x86_64::R13;

        let destination = operands.physical_register(0);
        emit!(emit.output, X86_64; Opcode::Move32Register => [register destination, register R13];);
    }

    fn finalize_operand_store(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
        operand_store(emit, operands.immediate(0), operands.operand(1), &operands[2..]);
        Ok(())
    }

    fn finalize_label_load(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
        use crate::target::registers::x86_64::{R13, R14};

        let destination = operands.physical_register(0);
        let offset = operands.immediate(1);
        emit!(emit.output, X86_64;
            Opcode::Load {
            width: MemoryWidth::Word,
            signed: false,
        } => [register destination, address MachineMemoryAddress::indexed_offset(R14, R13, offset)];
        );
        Ok(())
    }

    fn goto_handler(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
        let [target, opcode_scratch] = [operands.physical_register(0), operands.physical_register(1)];
        use crate::target::registers::x86_64::R13;

        if target != R13 {
            emit!(emit.output, X86_64; Opcode::Move32Register => [register R13, register target];);
        }
        dispatch_from_instruction_pointer(emit, opcode_scratch);
        Ok(())
    }

    fn goto_bytecode_target(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
        let offset = operands.immediate(0);
        let opcode_scratch = operands.physical_register(1);
        use crate::target::registers::x86_64::{R13, R14};

        emit!(emit.output, X86_64;
            Opcode::Load {
            width: MemoryWidth::Word,
            signed: false,
        } => [register R13, address MachineMemoryAddress::indexed_offset(R14, R13, offset)];
        );
        dispatch_from_instruction_pointer(emit, opcode_scratch);
        Ok(())
    }

    fn finalize_execution_context_store(
        &self,
        emit: &mut Emit<'_>,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        use crate::target::registers::x86_64::RBX;

        let scratch = operands.physical_register(1);
        let address = machine_address(operands.operand(0));
        let offset =
            required_runtime_constant(emit.runtime, KnownLayoutConstant::SizeOfExecutionContext, emit.handler)?
                .checked_neg()
                .ok_or_else(|| finalization_error(emit.handler, "execution-context offset overflow"))?;
        emit!(emit.output, X86_64;
            Opcode::LoadEffectiveAddress => [register scratch, address MachineMemoryAddress::offset(RBX, offset)];
        );
        store(
            emit,
            MemoryWidth::DoubleWord,
            address,
            StoreSource::Register(scratch),
            None,
        );
        Ok(())
    }

    fn finalize_indexed_offset_store(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) {
        let [base, index, source] = [0, 1, 4].map(|index| operands.physical_register(index));
        let [scale, offset] = [2, 3].map(|index| operands.immediate(index));
        store(
            emit,
            MemoryWidth::DoubleWord,
            MachineMemoryAddress {
                base,
                index: Some(index),
                scale: Some(scale),
                displacement: (offset != 0).then_some(offset),
            },
            StoreSource::Register(source),
            None,
        );
    }

    fn finalize_memory_increment(
        &self,
        emit: &mut Emit<'_>,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let address = machine_address(operands.operand(0));
        emit!(emit.output, X86_64; Opcode::Increment32Memory => [address address];);
        Ok(())
    }

    fn finalize_scalar_load(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        width: MemoryWidth,
        signed: bool,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let opcode = opcode.x86_64();
        let add_execution_context_offset = matches!(opcode, Opcode::LoadExecutionContext { .. });
        let destination = operands.physical_register(0);
        let address = machine_address(operands.operand(1));
        emit!(emit.output, X86_64; Opcode::Load { width, signed } => [register destination, address address];);
        if add_execution_context_offset {
            let offset =
                required_runtime_constant(emit.runtime, KnownLayoutConstant::SizeOfExecutionContext, emit.handler)?;
            emit!(emit.output, X86_64;
                Opcode::AluImmediate {
                operation: super::AluOperation::Add,
                width: IntegerWidth::U64,
            } => [register destination, immediate offset];
            );
        }
        Ok(())
    }

    fn finalize_scalar_store(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        width: MemoryWidth,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let opcode = opcode.x86_64();
        let (address, source, value_scratch) = match (opcode, operands) {
            (Opcode::Store(selected_width), [address, source]) if selected_width == width => (address, source, None),
            (Opcode::StoreLargeImmediate(selected_width), [address, source, value_scratch])
                if selected_width == width =>
            {
                (address, source, Some(verified_register(value_scratch)))
            }
            _ => unreachable!("allocated operation and operands were verified"),
        };
        let address = machine_address(address);
        scalar_store(emit, width, address, source, value_scratch);
        Ok(())
    }

    fn finalize_pair_load(
        &self,
        emit: &mut Emit<'_>,
        width: PairWidth,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let (destinations, address, []) = pair_access::<0>(operands, width, true, emit.handler)?;
        pair_load(emit, width, destinations, address)
    }

    fn finalize_pair_store(
        &self,
        emit: &mut Emit<'_>,
        width: PairWidth,
        indexed: bool,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        if indexed {
            let (address_registers, scale, sources, []) =
                decode_indexed_pair_store::<0>(operands, width, emit.handler)?;
            indexed_pair_store(emit, address_registers, scale, sources);
            return Ok(());
        }
        let (sources, address, []) = pair_access::<0>(operands, width, false, emit.handler)?;
        pair_store(emit, width, sources, address);
        Ok(())
    }

    fn finalize_or32_branch(
        &self,
        emit: &mut Emit<'_>,
        condition: SignCondition,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let [_, _, _, AllocatedOperand::Label(target)] = operands else {
            unreachable!("allocated OR-branch operands were verified");
        };
        self.finalize_binary_operation(emit, BinaryOperation::Or, IntegerWidth::U32, &operands[..3])?;
        emit!(emit.output, X86_64; Opcode::JumpSign(condition) => [label target.clone()];);
        Ok(())
    }

    fn add_subtract(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let opcode = opcode.x86_64();
        let destination = operands.physical_register(0);
        let rhs = operands.operand(1);
        match (opcode, rhs) {
            (
                selected @ Opcode::AluImmediate {
                    width: IntegerWidth::U64,
                    ..
                },
                AllocatedOperand::Immediate(value),
            ) => {
                if !super::alu_immediate_fits(IntegerWidth::U64, *value) {
                    return finalize_error(emit.handler, "x86-64 ALU immediate is out of range");
                }
                emit!(emit.output, X86_64; selected => [register destination, immediate *value];);
            }
            (
                selected @ Opcode::AluRegister {
                    width: IntegerWidth::U64,
                    ..
                },
                rhs,
            ) => {
                let rhs = verified_register(rhs);
                emit!(emit.output, X86_64; selected => [register destination, register rhs];);
            }
            _ => unreachable!("allocated operation and operands were verified"),
        }
        Ok(())
    }

    fn finalize_binary_operation(
        &self,
        emit: &mut Emit<'_>,
        operation: BinaryOperation,
        width: IntegerWidth,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        if operation == BinaryOperation::Multiply {
            return multiply(emit, width, operands);
        }

        let alu_operation =
            super::AluOperation::from_binary(operation).expect("allocated x86-64 ALU operation was verified");
        match (operation, width, operands) {
            (BinaryOperation::And, IntegerWidth::U64, [destination, rhs, scratch, _reserved_scratch]) => {
                let destination = verified_register(destination);
                let scratch = verified_register(scratch);
                match rhs {
                    AllocatedOperand::Immediate(value) if *value as u64 == 0xffff_ffff => {
                        push_move_register(emit, destination, destination, IntegerWidth::U32);
                    }
                    AllocatedOperand::Immediate(value) if i32::try_from(*value).is_ok() => {
                        push_alu_immediate(emit, alu_operation, width, destination, *value);
                    }
                    AllocatedOperand::Immediate(value) => {
                        emit!(emit.output, X86_64; Opcode::MoveAbsolute64Immediate => [register scratch, immediate *value];);
                        push_alu_register(emit, alu_operation, width, destination, scratch);
                    }
                    rhs => {
                        push_alu_register(emit, alu_operation, width, destination, verified_register(rhs));
                    }
                }
            }
            (
                BinaryOperation::And | BinaryOperation::Or | BinaryOperation::Xor,
                IntegerWidth::U64,
                [destination, rhs],
            )
            | (BinaryOperation::And, IntegerWidth::U16, [destination, rhs]) => {
                let destination = verified_register(destination);
                match rhs {
                    AllocatedOperand::Immediate(value) => {
                        if !super::alu_immediate_fits(width, *value) {
                            return finalize_error(emit.handler, "x86-64 ALU immediate is out of range");
                        }
                        push_alu_immediate(emit, alu_operation, width, destination, *value);
                    }
                    rhs => {
                        push_alu_register(emit, alu_operation, width, destination, verified_register(rhs));
                    }
                }
            }
            (
                BinaryOperation::And | BinaryOperation::Or | BinaryOperation::Xor,
                IntegerWidth::U32,
                [destination, lhs, rhs],
            ) => {
                let destination = verified_register(destination);
                let lhs = verified_register(lhs);
                match rhs {
                    AllocatedOperand::Immediate(value) => {
                        if !super::alu_immediate_fits(width, *value) {
                            return finalize_error(emit.handler, "x86-64 ALU immediate is out of range");
                        }
                        if destination != lhs {
                            push_move_register(emit, destination, lhs, IntegerWidth::U32);
                        }
                        push_alu_immediate(emit, alu_operation, width, destination, *value);
                    }
                    rhs => {
                        let rhs = verified_register(rhs);
                        if destination == lhs {
                            push_alu_register(emit, alu_operation, width, destination, rhs);
                        } else if destination == rhs {
                            push_alu_register(emit, alu_operation, width, destination, lhs);
                        } else {
                            push_move_register(emit, destination, lhs, IntegerWidth::U32);
                            push_alu_register(emit, alu_operation, width, destination, rhs);
                        }
                    }
                }
            }
            _ => unreachable!("allocated operation and operands were verified"),
        }
        Ok(())
    }

    fn finalize_overflow_operation(
        &self,
        emit: &mut Emit<'_>,
        operation: OverflowOperation,
        operands: &[AllocatedOperand],
    ) {
        let target = match (operation, operands) {
            (
                operation @ (OverflowOperation::AddWithOverflow | OverflowOperation::SubtractWithOverflow),
                [destination, rhs, target_operand],
            ) => {
                let destination = verified_register(destination);
                let operation = match operation {
                    OverflowOperation::AddWithOverflow => super::AluOperation::Add,
                    OverflowOperation::SubtractWithOverflow => super::AluOperation::Subtract,
                    _ => unreachable!(),
                };
                match rhs {
                    AllocatedOperand::Immediate(value) => {
                        debug_assert!(super::alu_immediate_fits(IntegerWidth::U32, *value));
                        emit!(emit.output, X86_64;
                            Opcode::AluImmediate {
                            operation,
                            width: IntegerWidth::U32,
                        } => [register destination, immediate *value];
                        );
                    }
                    AllocatedOperand::Address(address) => {
                        push_alu_memory(emit, operation, IntegerWidth::U32, destination, address.clone());
                    }
                    rhs => emit.output.push(machine_instruction(
                        Opcode::AluRegister {
                            operation,
                            width: IntegerWidth::U32,
                        },
                        vec![
                            MachineOperand::PhysicalRegister(destination),
                            MachineOperand::PhysicalRegister(verified_register(rhs)),
                        ],
                    )),
                }
                verified_label(target_operand)
            }
            (
                operation @ (OverflowOperation::RecoverAddLhs | OverflowOperation::RecoverSubtractLhs),
                [destination, rhs],
            ) => {
                let destination = verified_register(destination);
                let operation = match operation {
                    OverflowOperation::RecoverAddLhs => super::AluOperation::Subtract,
                    OverflowOperation::RecoverSubtractLhs => super::AluOperation::Add,
                    _ => unreachable!(),
                };
                match rhs {
                    AllocatedOperand::Immediate(value) => {
                        debug_assert!(super::alu_immediate_fits(IntegerWidth::U32, *value));
                        push_alu_immediate(emit, operation, IntegerWidth::U32, destination, *value);
                    }
                    AllocatedOperand::Address(address) => {
                        push_alu_memory(emit, operation, IntegerWidth::U32, destination, address.clone());
                    }
                    rhs => {
                        push_alu_register(emit, operation, IntegerWidth::U32, destination, verified_register(rhs));
                    }
                }
                return;
            }
            (OverflowOperation::MultiplyWithOverflow, [destination, source, target_operand]) => {
                push_overflow_multiply(emit, verified_register(destination), verified_register(source));
                verified_label(target_operand)
            }
            (OverflowOperation::MultiplyCopy, [destination, lhs, rhs, target_operand]) => {
                let destination = verified_register(destination);
                let lhs = verified_register(lhs);
                let rhs = verified_register(rhs);
                if destination == lhs {
                    push_overflow_multiply(emit, destination, rhs);
                } else if destination == rhs {
                    push_overflow_multiply(emit, destination, lhs);
                } else {
                    emit!(emit.output, X86_64; Opcode::Move32Register => [register destination, register lhs];);
                    push_overflow_multiply(emit, destination, rhs);
                }
                verified_label(target_operand)
            }
            (
                operation @ (OverflowOperation::Increment | OverflowOperation::Decrement | OverflowOperation::Negate),
                [destination, target_operand],
            ) => {
                let opcode = match operation {
                    OverflowOperation::Increment => Opcode::Increment32Register,
                    OverflowOperation::Decrement => Opcode::Decrement32Register,
                    OverflowOperation::Negate => Opcode::Negate32Register,
                    _ => unreachable!(),
                };
                emit!(emit.output, X86_64; opcode => [register verified_register(destination)];);
                verified_label(target_operand)
            }
            _ => unreachable!("allocated operation and operands were verified"),
        };
        emit!(emit.output, X86_64; Opcode::JumpOverflow => [label target];);
    }

    fn finalize_shift(
        &self,
        emit: &mut Emit<'_>,
        operation: ShiftOperation,
        width: IntegerWidth,
        operands: &[AllocatedOperand],
    ) {
        use crate::target::registers::x86_64::RCX;

        match (width, operands) {
            (IntegerWidth::U64, [destination, count]) => {
                let destination = verified_register(destination);
                match count {
                    AllocatedOperand::Immediate(0) => {}
                    AllocatedOperand::Immediate(count) => {
                        emit!(emit.output, X86_64; Opcode::ShiftImmediate { operation, width } => [register destination, immediate *count];);
                    }
                    count => {
                        let count = verified_register(count);
                        if count != RCX {
                            emit!(emit.output, X86_64; Opcode::Move64Register => [register RCX, register count];);
                        }
                        emit!(emit.output, X86_64; Opcode::ShiftByCountRegister { operation, width } => [register destination];);
                    }
                }
            }
            (IntegerWidth::U32, [destination, lhs, count, scratch]) => {
                let destination = verified_register(destination);
                let lhs = verified_register(lhs);
                let scratch = verified_register(scratch);
                match count {
                    AllocatedOperand::Immediate(count) => {
                        if destination != lhs {
                            push_move_register(emit, destination, lhs, IntegerWidth::U32);
                        }
                        if *count != 0 {
                            emit!(emit.output, X86_64; Opcode::ShiftImmediate { operation, width } => [register destination, immediate *count];);
                        }
                    }
                    _count => {
                        if destination == RCX && lhs != destination {
                            push_move_register(emit, scratch, lhs, IntegerWidth::U32);
                            emit!(emit.output, X86_64; Opcode::ShiftByCountRegister { operation, width } => [register scratch];);
                            push_move_register(emit, destination, scratch, IntegerWidth::U32);
                        } else {
                            if destination != lhs {
                                push_move_register(emit, destination, lhs, IntegerWidth::U32);
                            }
                            emit!(emit.output, X86_64; Opcode::ShiftByCountRegister { operation, width } => [register destination];);
                        }
                    }
                }
            }
            _ => unreachable!("allocated operation and operands were verified"),
        }
    }

    fn finalize_divide(&self, emit: &mut Emit<'_>, width: IntegerWidth, operands: &[AllocatedOperand]) {
        let [_, dividend, divisor, accumulator] = operands.physical_registers();

        let (move_dividend, sign_extend, divide) = match width {
            IntegerWidth::U32 => (
                Opcode::Move32Register,
                Opcode::SignExtendEaxToEdx,
                Opcode::SignedDivide32Register,
            ),
            IntegerWidth::U64 => (
                Opcode::Move64Register,
                Opcode::SignExtendRaxToRdx,
                Opcode::SignedDivide64Register,
            ),
            _ => unreachable!("remainder requires a 32-bit or 64-bit width"),
        };
        emit!(emit.output, X86_64;
            move_dividend => [register accumulator, register dividend];
            sign_extend => [];
            divide => [register divisor];
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::intrinsic::ComparisonRelation;

    #[test]
    fn evaluates_ordered_immediate_branches() {
        assert!(immediate_scalar_branch_taken(
            -1,
            1,
            IntegerWidth::U32,
            ScalarBranchCondition::Signed(ComparisonRelation::Less),
        ));
        assert!(immediate_scalar_branch_taken(
            -1,
            1,
            IntegerWidth::U32,
            ScalarBranchCondition::Unsigned(ComparisonRelation::Greater),
        ));
        assert!(!immediate_scalar_branch_taken(
            2,
            1,
            IntegerWidth::U16,
            ScalarBranchCondition::Signed(ComparisonRelation::LessOrEqual),
        ));
        assert!(immediate_scalar_branch_taken(
            2,
            1,
            IntegerWidth::U64,
            ScalarBranchCondition::Unsigned(ComparisonRelation::GreaterOrEqual),
        ));
    }
}
