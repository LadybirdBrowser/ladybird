/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AArch64 machine instruction finalization.

use super::Opcode;
use super::{AddSubtractOperation, AddressIndexShift, ConditionFlags, FlagUpdate, MemoryAddressing};
use crate::CompileError;
use crate::frontend::layout::KnownLayoutConstant;
use crate::low_ir::Label;
use crate::target::backend::{Aarch64Backend, Backend};
use crate::target::description::ArchitectureOpcode;
use crate::target::description::{
    AssertionOperation, BinaryOperation, BranchOperation, EqualityCondition, FloatCondition,
    FloatConversion as IntrinsicFloatConversion, FloatingPointOperation, IntegerWidth, MemoryWidth, Operation,
    OverflowOperation, PairWidth, ScalarBranchCondition, ShiftOperation, SignCondition, TestCondition, ZeroCondition,
};
use crate::target::finalize_support::{
    AllocatedOperands, Emit, MemoryBranch, finalization_error, finalize_error,
    indexed_pair_store as decode_indexed_pair_store, interpreter_layout, machine_address, operand_register,
    pair_access, pair_element_size, verified_label, verified_register,
};
use crate::target::finalize_support::{memory_branch, push_plain_move};
use crate::target::ir::{
    AllocatedOperand, MachineInstruction, MachineMemoryAddress, MachineOpcode, MachineOperand, RuntimeConstants,
};
use crate::target::machine_verify::emit_machine_instructions as emit;
use crate::target::registers::{
    PhysicalRegister,
    aarch64::{X21, X22, X23, X24, X25, X26, XZR},
};

fn machine_instruction(opcode: Opcode, operands: Vec<MachineOperand>) -> MachineInstruction {
    MachineInstruction {
        opcode: MachineOpcode::Aarch64(opcode),
        operands,
    }
}

pub(crate) fn compare(
    emit: &mut Emit<'_>,
    lhs: PhysicalRegister,
    rhs: &AllocatedOperand,
    scratch: PhysicalRegister,
    width: IntegerWidth,
) {
    match rhs {
        AllocatedOperand::Immediate(rhs) => {
            let rhs_unsigned = *rhs as u64;
            if rhs_unsigned <= 4095 {
                emit!(emit.output, Aarch64; Opcode::CompareImmediate(width) => [register lhs, immediate *rhs];);
            } else if let Some(pinned) = pinned_constant(emit.runtime, *rhs) {
                emit!(emit.output, Aarch64; Opcode::CompareRegister(width) => [register lhs, register pinned];);
            } else if let Some(negated) = rhs.checked_neg().filter(|negated| (0..=4095).contains(negated)) {
                emit!(emit.output, Aarch64; Opcode::CompareNegativeImmediate(width) => [register lhs, immediate negated];);
            } else {
                move_immediate(emit, scratch, *rhs, width);
                emit!(emit.output, Aarch64; Opcode::CompareRegister(width) => [register lhs, register scratch];);
            }
        }
        rhs => {
            emit!(emit.output, Aarch64; Opcode::CompareRegister(width) => [register lhs, register verified_register(rhs)];);
        }
    }
}

struct ValueRepresentationBranch {
    value: PhysicalRegister,
    representation: i64,
    scratch: PhysicalRegister,
    condition: EqualityCondition,
    target: Label,
}

fn branch_tag(emit: &mut Emit<'_>, operands: ValueRepresentationBranch, compare_scratch: PhysicalRegister) {
    emit!(emit.output, Aarch64;
        Opcode::ShiftImmediate {
        operation: ShiftOperation::RightLogical,
        width: IntegerWidth::U64,
    } => [register operands.scratch, register operands.value, immediate 48];
    );
    compare(
        emit,
        operands.scratch,
        &AllocatedOperand::Immediate(operands.representation),
        compare_scratch,
        IntegerWidth::U64,
    );
    push_equality_branch(emit, operands.condition, &operands.target);
}

fn branch_singleton(emit: &mut Emit<'_>, operands: ValueRepresentationBranch) {
    move_immediate(emit, operands.scratch, operands.representation, IntegerWidth::U64);
    emit!(emit.output, Aarch64; Opcode::CompareRegister(IntegerWidth::U64) => [register operands.value, register operands.scratch];);
    push_equality_branch(emit, operands.condition, &operands.target);
}

fn push_equality_branch(emit: &mut Emit<'_>, condition: EqualityCondition, target: &Label) {
    use super::Condition;

    emit!(emit.output, Aarch64;
        Opcode::BranchCondition(condition.select(Condition::Equal, Condition::NotEqual)) => [label target.clone()];
    );
}

pub(crate) fn pinned_constant(runtime: &RuntimeConstants, value: i64) -> Option<PhysicalRegister> {
    [
        (runtime.get(KnownLayoutConstant::Int32Tag), X22),
        (runtime.get(KnownLayoutConstant::BooleanTag), X23),
        (runtime.get(KnownLayoutConstant::NanBaseTag), X24),
    ]
    .into_iter()
    .find_map(|(constant, register)| (constant == Some(value)).then_some(register))
}

pub(crate) fn move_immediate(emit: &mut Emit<'_>, destination: PhysicalRegister, value: i64, width: IntegerWidth) {
    for immediate in super::immediate_moves(value, width) {
        let mut operands = vec![
            MachineOperand::PhysicalRegister(destination),
            MachineOperand::Immediate(immediate.value),
        ];
        operands.extend(immediate.shift.map(MachineOperand::Immediate));
        emit.output.push(machine_instruction(immediate.opcode, operands));
    }
}

fn multiply(emit: &mut Emit<'_>, width: IntegerWidth, operands: &[AllocatedOperand]) {
    assert_eq!(width, IntegerWidth::U64);
    match operands {
        [destination, source, scratch] => {
            let destination = verified_register(destination);
            let source = verified_register(source);
            let scratch = verified_register(scratch);
            emit!(emit.output, Aarch64; Opcode::SignedMultiplyLong32 => [register destination, register destination, register source];);
            emit!(emit.output, Aarch64; Opcode::SignExtend32To64 => [register scratch, register destination];);
            emit!(emit.output, Aarch64; Opcode::CompareRegister(IntegerWidth::U64) => [register destination, register scratch];);
            emit!(emit.output, Aarch64; Opcode::ConditionalCompareZeroEqual => [];);
        }
        [destination, source, AllocatedOperand::Immediate(value), _scratch]
            if *value > 0 && (*value as u64).is_power_of_two() =>
        {
            emit!(emit.output, Aarch64;
                Opcode::ShiftImmediate {
                operation: ShiftOperation::Left,
                width: IntegerWidth::U64,
            } => [register verified_register(destination), register verified_register(source), immediate (*value as u64).trailing_zeros().into()];
            );
        }
        [destination, source, AllocatedOperand::Immediate(value), scratch] => {
            let destination = verified_register(destination);
            let source = verified_register(source);
            let scratch = verified_register(scratch);
            move_immediate(emit, scratch, *value, IntegerWidth::U64);
            push_multiply(emit, destination, source, scratch);
        }
        [destination, source, multiplier, _scratch] => {
            let destination = verified_register(destination);
            let source = verified_register(source);
            let multiplier = verified_register(multiplier);
            push_multiply(emit, destination, source, multiplier);
        }
        _ => unreachable!("allocated operation and operands were verified"),
    }
}

struct LogicalOperands<'a> {
    destination: PhysicalRegister,
    lhs: PhysicalRegister,
    rhs: &'a AllocatedOperand,
    scratch: PhysicalRegister,
}

fn logical(emit: &mut Emit<'_>, operation: BinaryOperation, width: IntegerWidth, operands: LogicalOperands<'_>) {
    let LogicalOperands {
        destination,
        lhs,
        rhs,
        scratch,
    } = operands;
    let operation = super::LogicalOperation::from_binary(operation).expect("allocated logical operation was verified");
    match rhs {
        AllocatedOperand::Immediate(value) => {
            let value = match width {
                IntegerWidth::U64 => *value as u64,
                IntegerWidth::U32 => *value as u32 as u64,
                IntegerWidth::U16 => *value as u16 as u64,
                IntegerWidth::U8 => unreachable!("8-bit logical operation reached finalization"),
            };
            if super::is_logical_immediate(width, value) {
                emit!(emit.output, Aarch64; Opcode::LogicalImmediate { operation, width } => [register destination, register lhs, immediate value as i64];);
            } else {
                move_immediate(
                    emit,
                    scratch,
                    value as i64,
                    if width == IntegerWidth::U64 {
                        IntegerWidth::U64
                    } else {
                        IntegerWidth::U32
                    },
                );
                push_logical_register(emit, operation, width, destination, lhs, scratch);
            }
        }
        rhs => {
            let rhs = verified_register(rhs);
            push_logical_register(emit, operation, width, destination, lhs, rhs);
        }
    }
}

fn push_logical_register(
    emit: &mut Emit<'_>,
    operation: super::LogicalOperation,
    width: IntegerWidth,
    destination: PhysicalRegister,
    lhs: PhysicalRegister,
    rhs: PhysicalRegister,
) {
    emit!(emit.output, Aarch64; Opcode::LogicalRegister { operation, width } => [register destination, register lhs, register rhs];);
}

fn push_multiply(emit: &mut Emit<'_>, destination: PhysicalRegister, lhs: PhysicalRegister, rhs: PhysicalRegister) {
    emit!(emit.output, Aarch64; Opcode::Multiply64 => [register destination, register lhs, register rhs];);
}

pub(crate) enum MemoryAddressError {
    NoScratchRegister,
    UnencodableComponents(MachineMemoryAddress),
}

pub(crate) fn memory_address(
    emit: &mut Emit<'_>,
    width: MemoryWidth,
    address: MachineMemoryAddress,
    forbidden_scratch: &[PhysicalRegister],
    scratch_registers: &[PhysicalRegister],
) -> Result<(MemoryAddressing, MachineMemoryAddress), MemoryAddressError> {
    let base = address.base;
    match (address.index, address.scale, address.displacement) {
        (None, None, None) => Ok((MemoryAddressing::Base, MachineMemoryAddress::base(base))),
        (None, None, Some(offset)) => {
            base_offset_address(emit, width, base, offset, forbidden_scratch, scratch_registers)
        }
        (Some(index), None, None) => Ok((MemoryAddressing::Register, MachineMemoryAddress::indexed(base, index))),
        (Some(index), Some(scale), None) => {
            if let Some(shift) = AddressIndexShift::for_memory(width, scale) {
                return Ok((
                    MemoryAddressing::ShiftedRegister(shift),
                    MachineMemoryAddress::indexed(base, index),
                ));
            }

            let scratch = scratch_register(scratch_registers, &[forbidden_scratch, &[base, index]].concat())?;
            move_immediate(emit, scratch, scale, IntegerWidth::U64);
            emit!(emit.output, Aarch64; Opcode::MultiplyAdd64 => [register scratch, register index, register scratch, register base];);
            Ok((MemoryAddressing::Base, MachineMemoryAddress::base(scratch)))
        }
        (Some(index), None, Some(offset)) => {
            if base == X26 && index == X25 {
                return base_offset_address(emit, width, X21, offset, forbidden_scratch, scratch_registers);
            }

            let address_scratch = scratch_register(scratch_registers, &[forbidden_scratch, &[base, index]].concat())?;
            address_add_register(emit, address_scratch, base, index);
            let mut forbidden = forbidden_scratch.to_vec();
            forbidden.push(address_scratch);
            base_offset_address(emit, width, address_scratch, offset, &forbidden, scratch_registers)
        }
        _ => Err(MemoryAddressError::UnencodableComponents(address)),
    }
}

fn base_offset_address(
    emit: &mut Emit<'_>,
    width: MemoryWidth,
    base: PhysicalRegister,
    offset: i64,
    forbidden_scratch: &[PhysicalRegister],
    scratch_registers: &[PhysicalRegister],
) -> Result<(MemoryAddressing, MachineMemoryAddress), MemoryAddressError> {
    if offset == 0 {
        return Ok((MemoryAddressing::Base, MachineMemoryAddress::base(base)));
    }
    if super::unsigned_memory_offset_fits(width, offset) {
        return Ok((
            MemoryAddressing::UnsignedImmediate,
            machine_offset_address(base, offset),
        ));
    }
    if (-256..=255).contains(&offset) {
        return Ok((
            MemoryAddressing::UnscaledImmediate,
            machine_offset_address(base, offset),
        ));
    }

    let scratch = scratch_register(scratch_registers, &[forbidden_scratch, &[base]].concat())?;
    move_immediate(emit, scratch, offset, IntegerWidth::U64);
    Ok((MemoryAddressing::Register, MachineMemoryAddress::indexed(base, scratch)))
}

pub(crate) fn scratch_register(
    scratch_registers: &[PhysicalRegister],
    forbidden: &[PhysicalRegister],
) -> Result<PhysicalRegister, MemoryAddressError> {
    scratch_registers
        .iter()
        .copied()
        .find(|register| !forbidden.contains(register))
        .ok_or(MemoryAddressError::NoScratchRegister)
}

pub(crate) fn machine_offset_address(base: PhysicalRegister, offset: i64) -> MachineMemoryAddress {
    MachineMemoryAddress {
        displacement: Some(offset),
        ..MachineMemoryAddress::base(base)
    }
}

pub(crate) fn address_add_register(
    emit: &mut Emit<'_>,
    destination: PhysicalRegister,
    lhs: PhysicalRegister,
    rhs: PhysicalRegister,
) {
    emit!(emit.output, Aarch64;
        Opcode::AddSubtractRegister {
        operation: AddSubtractOperation::Add,
        flags: FlagUpdate::Preserve,
    } => [register destination, register lhs, register rhs];
    );
}

struct MemoryBranchOperands<'a> {
    address: MachineMemoryAddress,
    branch: MemoryBranch,
    load_scratch: PhysicalRegister,
    target: &'a Label,
}

fn emit_memory_branch(emit: &mut Emit<'_>, operands: MemoryBranchOperands<'_>) -> Result<(), MemoryAddressError> {
    use super::Condition;

    let memory_width = match operands.branch {
        MemoryBranch::CompareRegister {
            width: IntegerWidth::U32,
            ..
        } => MemoryWidth::Word,
        MemoryBranch::CompareRegister {
            width: IntegerWidth::U64,
            ..
        } => MemoryWidth::DoubleWord,
        MemoryBranch::CompareRegister { .. } => {
            unreachable!("memory comparison width was validated")
        }
        MemoryBranch::CompareByte { .. } | MemoryBranch::TestByte { .. } => MemoryWidth::Byte,
    };
    let (addressing, address) = memory_address(
        emit,
        memory_width,
        operands.address,
        &[],
        std::slice::from_ref(&operands.load_scratch),
    )?;
    emit!(emit.output, Aarch64;
        Opcode::Load {
        width: memory_width,
        signed: false,
        addressing,
    } => [register operands.load_scratch, address address];
    );

    let branch_condition = match operands.branch {
        MemoryBranch::CompareRegister { width, condition, rhs } => {
            emit!(emit.output, Aarch64; Opcode::CompareRegister(width) => [register operands.load_scratch, register rhs];);
            Some(condition.select(Condition::Equal, Condition::NotEqual))
        }
        MemoryBranch::CompareByte { condition, immediate } => {
            if immediate == 0 {
                emit!(emit.output, Aarch64;
                    Opcode::CompareAndBranchZero {
                        width: IntegerWidth::U32,
                        condition: condition.select(ZeroCondition::Zero, ZeroCondition::NonZero),
                } => [register operands.load_scratch, label operands.target.clone()];
                );
                None
            } else {
                emit!(emit.output, Aarch64; Opcode::CompareImmediate(IntegerWidth::U64) => [register operands.load_scratch, immediate immediate];);
                Some(condition.select(Condition::Equal, Condition::NotEqual))
            }
        }
        MemoryBranch::TestByte {
            condition,
            immediate,
            mask_scratch,
        } => {
            let mask_scratch = mask_scratch.expect("AArch64 byte test must have a mask scratch");
            move_immediate(emit, mask_scratch, immediate, IntegerWidth::U64);
            emit!(emit.output, Aarch64; Opcode::Test64Register => [register operands.load_scratch, register mask_scratch];);
            Some(condition.select(Condition::NotEqual, Condition::Equal))
        }
    };
    if let Some(condition) = branch_condition {
        emit!(emit.output, Aarch64; Opcode::BranchCondition(condition) => [label operands.target.clone()];);
    }
    Ok(())
}

fn branch_any_equal(
    emit: &mut Emit<'_>,
    value: PhysicalRegister,
    comparison_values: &[AllocatedOperand],
    scratch: PhysicalRegister,
    target: &Label,
) {
    use super::Condition;

    compare(emit, value, &comparison_values[0], scratch, IntegerWidth::U64);

    for comparison_value in &comparison_values[1..] {
        let opcode = match comparison_value {
            AllocatedOperand::Immediate(immediate) if (*immediate as u64) <= 31 => {
                Opcode::ConditionalCompareImmediate {
                    width: IntegerWidth::U64,
                    condition: Condition::NotEqual,
                    fallback: ConditionFlags::Equal,
                }
            }
            AllocatedOperand::Immediate(immediate) => {
                if let Some(pinned) = pinned_constant(emit.runtime, *immediate) {
                    emit!(emit.output, Aarch64;
                        Opcode::ConditionalCompareRegister {
                        width: IntegerWidth::U64,
                        condition: Condition::NotEqual,
                        fallback: ConditionFlags::Equal,
                    } => [register value, register pinned];
                    );
                    continue;
                }
                move_immediate(emit, scratch, *immediate, IntegerWidth::U64);
                Opcode::ConditionalCompareRegister {
                    width: IntegerWidth::U64,
                    condition: Condition::NotEqual,
                    fallback: ConditionFlags::Equal,
                }
            }
            comparison_value => {
                emit!(emit.output, Aarch64;
                    Opcode::ConditionalCompareRegister {
                    width: IntegerWidth::U64,
                    condition: Condition::NotEqual,
                    fallback: ConditionFlags::Equal,
                } => [register value, register verified_register(comparison_value)];
                );
                continue;
            }
        };
        emit!(emit.output, Aarch64;
            opcode => [register value, operand match comparison_value {
                AllocatedOperand::Immediate(immediate) if (*immediate as u64) <= 31 => {
                    MachineOperand::Immediate(*immediate)
                }
                AllocatedOperand::Immediate(_) => MachineOperand::PhysicalRegister(scratch),
                _ => unreachable!(),
            }];
        );
    }
    emit!(emit.output, Aarch64; Opcode::BranchCondition(Condition::Equal) => [label target.clone()];);
}

fn branch_bit(emit: &mut Emit<'_>, value: PhysicalRegister, bit: i64, condition: TestCondition, target: &Label) {
    emit!(emit.output, Aarch64;
        Opcode::TestBitAndBranch {
        width: IntegerWidth::U64,
        condition,
    } => [register value, immediate bit, label target.clone()];
    );
}

fn branch_mask_immediate(
    emit: &mut Emit<'_>,
    value: PhysicalRegister,
    mask: i64,
    scratch: PhysicalRegister,
    condition: TestCondition,
    target: &Label,
) {
    use super::Condition;

    let unsigned_mask = mask as u64;
    if unsigned_mask.is_power_of_two() {
        emit!(emit.output, Aarch64;
            Opcode::TestBitAndBranch {
            width: IntegerWidth::U64,
            condition,
        } => [register value, immediate unsigned_mask.trailing_zeros() as i64, label target.clone()];
        );
        return;
    }
    if super::is_logical_immediate(IntegerWidth::U64, unsigned_mask) {
        emit!(emit.output, Aarch64; Opcode::Test64Immediate => [register value, immediate mask];);
    } else {
        move_immediate(emit, scratch, mask, IntegerWidth::U64);
        emit!(emit.output, Aarch64; Opcode::Test64Register => [register value, register scratch];);
    }
    emit!(emit.output, Aarch64;
        Opcode::BranchCondition(condition.select(Condition::NotEqual, Condition::Equal)) => [label target.clone()];
    );
}

fn branch_mask_register(
    emit: &mut Emit<'_>,
    value: PhysicalRegister,
    mask: PhysicalRegister,
    condition: TestCondition,
    target: &Label,
) {
    use super::Condition;

    emit!(emit.output, Aarch64; Opcode::Test64Register => [register value, register mask];);
    emit!(emit.output, Aarch64;
        Opcode::BranchCondition(condition.select(Condition::NotEqual, Condition::Equal)) => [label target.clone()];
    );
}

fn branch_scalar(emit: &mut Emit<'_>, condition: ScalarBranchCondition, target: &Label) {
    use super::Condition;
    let condition = Condition::from_scalar(condition);
    emit!(emit.output, Aarch64; Opcode::BranchCondition(condition) => [label target.clone()];);
}

pub(crate) fn load(
    emit: &mut Emit<'_>,
    width: MemoryWidth,
    signed: bool,
    destination: PhysicalRegister,
    address: MachineMemoryAddress,
    scratch_registers: &[PhysicalRegister],
) -> Result<(), MemoryAddressError> {
    let (addressing, address) = memory_address(emit, width, address, &[], scratch_registers)?;
    emit!(emit.output, Aarch64;
        Opcode::Load {
        width,
        signed,
        addressing,
    } => [register destination, address address];
    );
    Ok(())
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
    scratch_registers: [PhysicalRegister; 2],
) -> Result<(), MemoryAddressError> {
    let [value_scratch, _address_scratch] = scratch_registers;
    let source = match source {
        StoreSource::Immediate(0) => XZR,
        StoreSource::Immediate(value) => {
            move_immediate(
                emit,
                value_scratch,
                value,
                if width == MemoryWidth::DoubleWord {
                    IntegerWidth::U64
                } else {
                    IntegerWidth::U32
                },
            );
            value_scratch
        }
        StoreSource::Register(register) => register,
    };
    let (addressing, address) = memory_address(emit, width, address, &[source], &scratch_registers)?;
    emit!(emit.output, Aarch64; Opcode::Store { width, addressing } => [address address, register source];);
    Ok(())
}

fn assertion_compare(
    emit: &mut Emit<'_>,
    lhs: PhysicalRegister,
    rhs: &AllocatedOperand,
    scratch: PhysicalRegister,
    width: IntegerWidth,
) {
    compare(emit, lhs, rhs, scratch, width);
}

fn append_assertion_trap(emit: &mut Emit<'_>, ok_label: Label) {
    emit!(emit.output, Aarch64;
        Opcode::Break => [];
        Opcode::Label => [label ok_label];
    );
}

fn push_overflow_add_subtract_register(
    emit: &mut Emit<'_>,
    operation: AddSubtractOperation,
    destination: PhysicalRegister,
    source: PhysicalRegister,
) {
    emit!(emit.output, Aarch64; Opcode::AddSubtract32RegisterWithFlags(operation) => [register destination, register source];);
}

fn overflow_multiply(
    emit: &mut Emit<'_>,
    destination: PhysicalRegister,
    lhs: PhysicalRegister,
    rhs: PhysicalRegister,
    scratch: PhysicalRegister,
) {
    emit!(emit.output, Aarch64;
        Opcode::SignedMultiplyLong32 => [register destination, register lhs, register rhs];
        Opcode::SignExtend32To64 => [register scratch, register destination];
        Opcode::CompareRegister(IntegerWidth::U64) => [register destination, register scratch];
    );
}

fn operand_store(
    emit: &mut Emit<'_>,
    offset: i64,
    source: &AllocatedOperand,
    scratches: &[AllocatedOperand],
) -> Result<(), CompileError> {
    use crate::target::registers::aarch64::{X21, X27};

    let [index, secondary_scratch] = scratches else {
        unreachable!("allocated scratch arity was verified");
    };
    let index = verified_register(index);
    let secondary_scratch = verified_register(secondary_scratch);
    let value_scratch = matches!(source, AllocatedOperand::Immediate(_)).then_some(secondary_scratch);

    load(
        emit,
        MemoryWidth::Word,
        false,
        index,
        MachineMemoryAddress::offset(X21, offset),
        &[index],
    )
    .map_err(|error| memory_address_compile_error(emit.handler, error))?;
    let source = match source {
        AllocatedOperand::Immediate(value) => {
            let value_scratch = value_scratch.expect("immediate store has a value scratch");
            move_immediate(emit, value_scratch, *value, IntegerWidth::U64);
            value_scratch
        }
        source => verified_register(source),
    };
    store(
        emit,
        MemoryWidth::DoubleWord,
        MachineMemoryAddress::scaled(X27, index, 8, 0),
        StoreSource::Register(source),
        [index, secondary_scratch],
    )
    .map_err(|error| memory_address_compile_error(emit.handler, error))
}

fn pair_load(
    emit: &mut Emit<'_>,
    width: PairWidth,
    destinations: [PhysicalRegister; 2],
    first_address: MachineMemoryAddress,
    scratch_registers: [PhysicalRegister; 2],
) -> Result<(), CompileError> {
    let [first, second] = destinations;
    let [base_scratch, address_scratch] = scratch_registers;
    let base = pair_base(
        emit,
        first_address.base,
        first_address.index,
        &[first, second],
        &[base_scratch],
    )?;
    let first_offset = first_address.displacement.unwrap_or(0);
    if super::pair_offset_fits(width, first_offset) {
        emit!(emit.output, Aarch64;
            Opcode::LoadPair(width) => [register first, register second, address MachineMemoryAddress {
                base,
                index: None,
                scale: None,
                displacement: Some(first_offset),
            }];
        );
        return Ok(());
    }

    let first_address = MachineMemoryAddress::offset(base, first_offset);
    let second_offset = first_offset
        .checked_add(pair_element_size(width))
        .ok_or_else(|| finalization_error(emit.handler, "pair-load address offset overflow"))?;
    let second_address = MachineMemoryAddress::offset(base, second_offset);
    let push_load = |emit: &mut Emit<'_>, destination, address| -> Result<(), CompileError> {
        load(emit, width.into(), false, destination, address, &[address_scratch])
            .map_err(|error| memory_address_compile_error(emit.handler, error))
    };
    if first == base {
        push_load(emit, second, second_address)?;
        push_load(emit, first, first_address)?;
    } else {
        push_load(emit, first, first_address)?;
        push_load(emit, second, second_address)?;
    }
    Ok(())
}

fn pair_store(
    emit: &mut Emit<'_>,
    width: PairWidth,
    sources: [PhysicalRegister; 2],
    first_address: MachineMemoryAddress,
    scratch_registers: [PhysicalRegister; 2],
) -> Result<(), CompileError> {
    let [first, second] = sources;
    let base = pair_base(
        emit,
        first_address.base,
        first_address.index,
        &[first, second],
        &scratch_registers,
    )?;
    let first_offset = first_address.displacement.unwrap_or(0);
    if super::pair_offset_fits(width, first_offset) {
        push_pair_store(emit, width, base, Some(first_offset), first, second);
        return Ok(());
    }

    store(
        emit,
        width.into(),
        MachineMemoryAddress::offset(base, first_offset),
        StoreSource::Register(first),
        scratch_registers,
    )
    .map_err(|error| memory_address_compile_error(emit.handler, error))?;
    let second_offset = first_offset
        .checked_add(pair_element_size(width))
        .ok_or_else(|| finalization_error(emit.handler, "pair-store address offset overflow"))?;
    store(
        emit,
        width.into(),
        MachineMemoryAddress::offset(base, second_offset),
        StoreSource::Register(second),
        scratch_registers,
    )
    .map_err(|error| memory_address_compile_error(emit.handler, error))
}

fn indexed_pair_store(
    emit: &mut Emit<'_>,
    address_registers: [PhysicalRegister; 2],
    scale: i64,
    sources: [PhysicalRegister; 2],
    scratch: PhysicalRegister,
) {
    let [base, index] = address_registers;
    let [first, second] = sources;
    if scale == 1 {
        address_add_register(emit, scratch, base, index);
    } else {
        let shift = match scale {
            2 => AddressIndexShift::One,
            4 => AddressIndexShift::Two,
            8 => AddressIndexShift::Three,
            _ => unreachable!("indexed pair-store scale was validated"),
        };
        emit!(emit.output, Aarch64; Opcode::AddShiftedRegister { shift } => [register scratch, register base, register index];);
    }
    push_pair_store(emit, PairWidth::DoubleWord, scratch, None, first, second);
}

fn pair_base(
    emit: &mut Emit<'_>,
    base: PhysicalRegister,
    index: Option<PhysicalRegister>,
    excluded: &[PhysicalRegister],
    scratch_registers: &[PhysicalRegister],
) -> Result<PhysicalRegister, CompileError> {
    use crate::target::registers::aarch64::{X21, X25, X26};

    let Some(index) = index else {
        return Ok(base);
    };
    if base == X26 && index == X25 {
        return Ok(X21);
    }
    let mut forbidden = excluded.to_vec();
    forbidden.extend([base, index]);
    let scratch = scratch_register(scratch_registers, &forbidden)
        .map_err(|error| memory_address_compile_error(emit.handler, error))?;
    address_add_register(emit, scratch, base, index);
    Ok(scratch)
}

fn push_pair_store(
    emit: &mut Emit<'_>,
    width: PairWidth,
    base: PhysicalRegister,
    offset: Option<i64>,
    first: PhysicalRegister,
    second: PhysicalRegister,
) {
    emit!(emit.output, Aarch64;
        Opcode::StorePair(width) => [address MachineMemoryAddress {
            base,
            index: None,
            scale: None,
            displacement: offset,
        }, register first, register second];
    );
}

pub(crate) fn direct_call(emit: &mut Emit<'_>, function: crate::low_ir::Relocation) {
    emit!(emit.output, Aarch64; Opcode::BranchLink => [relocation function];);
}

pub(crate) fn indirect_call(emit: &mut Emit<'_>, function: PhysicalRegister) {
    emit!(emit.output, Aarch64; Opcode::BranchLinkRegister => [register function];);
}

pub(crate) fn interpreter_call_arguments(emit: &mut Emit<'_>) {
    vm_pc_arguments(emit);
    instruction_argument(emit);
}

pub(crate) fn vm_pc_arguments(emit: &mut Emit<'_>) {
    use crate::target::registers::aarch64::{X0, X1, X20, X21, X26};

    emit!(emit.output, Aarch64;
        Opcode::MoveRegister(IntegerWidth::U64) => [register X0, register X20];
        Opcode::Subtract32Register => [register X1, register X21, register X26];
    );
}

pub(crate) fn instruction_argument(emit: &mut Emit<'_>) {
    use crate::target::registers::aarch64::{X2, X21};

    emit!(emit.output, Aarch64; Opcode::MoveRegister(IntegerWidth::U64) => [register X2, register X21];);
}

fn dispatch_from_instruction_pointer(
    emit: &mut Emit<'_>,
    opcode_scratch: PhysicalRegister,
    target_scratch: PhysicalRegister,
) -> Result<(), MemoryAddressError> {
    use crate::target::registers::aarch64::X21;

    load(
        emit,
        MemoryWidth::Byte,
        false,
        opcode_scratch,
        MachineMemoryAddress::base(X21),
        &[opcode_scratch],
    )?;
    dispatch_tail(emit, opcode_scratch, target_scratch)
}

pub(crate) fn dispatch_tail(
    emit: &mut Emit<'_>,
    opcode_scratch: PhysicalRegister,
    target_scratch: PhysicalRegister,
) -> Result<(), MemoryAddressError> {
    use crate::target::registers::aarch64::X19;

    load(
        emit,
        MemoryWidth::DoubleWord,
        false,
        target_scratch,
        MachineMemoryAddress::scaled(X19, opcode_scratch, 8, 0),
        &[],
    )?;
    emit!(emit.output, Aarch64; Opcode::BranchRegister => [register target_scratch];);
    Ok(())
}

fn address_offset(
    emit: &mut Emit<'_>,
    destination: PhysicalRegister,
    source: PhysicalRegister,
    offset: i64,
    scratch: PhysicalRegister,
) {
    if offset == 0 {
        if destination != source {
            emit!(emit.output, Aarch64; Opcode::MoveRegister(IntegerWidth::U64) => [register destination, register source];);
        }
        return;
    }
    let (operation, value, shift) = if (1..=4095).contains(&offset) {
        (AddSubtractOperation::Add, offset, super::ImmediateShift::None)
    } else if offset > 0 && offset <= 0xfff000 && offset & 0xfff == 0 {
        (AddSubtractOperation::Add, offset >> 12, super::ImmediateShift::Twelve)
    } else if (-4095..0).contains(&offset) {
        (AddSubtractOperation::Subtract, -offset, super::ImmediateShift::None)
    } else {
        move_immediate(emit, scratch, offset, IntegerWidth::U64);
        address_add_register(emit, destination, source, scratch);
        return;
    };
    emit!(emit.output, Aarch64;
        Opcode::AddSubtractImmediate {
        operation,
        shift,
        flags: FlagUpdate::Preserve,
    } => [register destination, register source, immediate value];
    );
}

fn memory_address_compile_error(handler: &str, error: MemoryAddressError) -> CompileError {
    match error {
        MemoryAddressError::NoScratchRegister => {
            finalization_error(handler, "scalar memory address has no available scratch register")
        }
        MemoryAddressError::UnencodableComponents(address) => finalization_error(
            handler,
            format!(
                "scalar memory address has unencodable normalized components: {:?}",
                (address.index, address.scale, address.displacement)
            ),
        ),
    }
}

impl Backend for Aarch64Backend {
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
            emit!(emit.output, Aarch64; Opcode::CompareDouble => [register lhs, register rhs];);
        }
        emit!(emit.output, Aarch64;
            Opcode::BranchCondition(crate::target::ir::MachineCondition::from_float(condition)) => [label target.clone()];
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
        emit!(emit.output, Aarch64; Opcode::CompareAndBranchZero { width, condition } => [register value, label target.clone()];);
    }

    fn branch_sign(
        &self,
        emit: &mut Emit<'_>,
        value: PhysicalRegister,
        width: IntegerWidth,
        condition: SignCondition,
        target: &Label,
    ) {
        let bit = match width {
            IntegerWidth::U32 => 31,
            IntegerWidth::U64 => 63,
            _ => unreachable!("sign branch width was validated"),
        };
        emit!(emit.output, Aarch64;
            Opcode::TestBitAndBranch {
                width,
                condition: condition.select(TestCondition::Set, TestCondition::Clear),
        } => [register value, immediate bit, label target.clone()];
        );
    }

    fn box_int32(
        &self,
        emit: &mut Emit<'_>,
        destination: PhysicalRegister,
        source: PhysicalRegister,
        clean: bool,
    ) -> Result<(), CompileError> {
        let tag = emit.constant(KnownLayoutConstant::Int32Tag)?;
        if !clean || destination != source {
            emit!(emit.output, Aarch64;
                Opcode::MoveRegister(if clean {
                IntegerWidth::U64
            } else {
                IntegerWidth::U32
            }) => [register destination, register source];
            );
        }
        emit!(emit.output, Aarch64; Opcode::MoveWideKeepShifted(IntegerWidth::U64) => [register destination, immediate tag, immediate 48];);
        Ok(())
    }

    fn update_bit(&self, emit: &mut Emit<'_>, destination: PhysicalRegister, bit: u32, operation: Operation) {
        emit!(emit.output, Aarch64;
            if operation == Operation::ToggleBit {
            Opcode::ExclusiveOr64Immediate
        } else {
            Opcode::BitClear64Immediate
        } => [register destination, immediate (1_u64 << bit) as i64];
        );
    }

    fn extract_tag(&self, emit: &mut Emit<'_>, destination: PhysicalRegister, source: PhysicalRegister) {
        emit!(emit.output, Aarch64;
            Opcode::ShiftImmediate {
            operation: ShiftOperation::RightLogical,
            width: IntegerWidth::U64,
        } => [register destination, register source, immediate 48];
        );
    }

    fn unbox_object(&self, emit: &mut Emit<'_>, destination: PhysicalRegister, source: PhysicalRegister) {
        emit!(emit.output, Aarch64; Opcode::And64Immediate => [register destination, register source, immediate 0xffff_ffff_ffff];);
    }

    fn float_operation(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        operation: FloatingPointOperation,
        operands: &[AllocatedOperand],
    ) {
        // AArch64 floating-point arithmetic is three-address, so a two-operand
        // binary operation repeats its destination as the first source.
        let operands = if matches!(operation, FloatingPointOperation::Binary(_)) {
            let [destination, source] = operands.physical_registers();
            vec![
                AllocatedOperand::PhysicalRegister(destination),
                AllocatedOperand::PhysicalRegister(destination),
                AllocatedOperand::PhysicalRegister(source),
            ]
        } else {
            operands.to_vec()
        };
        emit.output.push(MachineInstruction { opcode, operands });
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
        use super::{Condition, FloatConversion};

        let [fpr_scratch] = scratches.physical_registers();
        let has_jscvt = emit.has_jscvt;
        let failure = failure.clone();

        if operation == FloatingPointOperation::Convert(IntrinsicFloatConversion::JavaScriptToInt32) && has_jscvt {
            emit!(emit.output, Aarch64; Opcode::FloatConversion(FloatConversion::JavaScriptToSigned32) => [register destination, register source];);
            return;
        }
        emit!(emit.output, Aarch64;
            Opcode::FloatConversion(FloatConversion::DoubleToSigned32Truncate) => [register destination, register source];
            Opcode::FloatConversion(FloatConversion::Signed32ToDouble) => [register fpr_scratch, register destination];
            Opcode::CompareDouble => [register source, register fpr_scratch];
            Opcode::BranchCondition(Condition::NotEqual) => [label failure];
        );
    }

    fn helper_call(&self, emit: &mut Emit<'_>, function: crate::low_ir::Relocation) {
        direct_call(emit, function);
    }

    fn interpreter_call(&self, emit: &mut Emit<'_>, function: crate::low_ir::Relocation) {
        interpreter_call_arguments(emit);
        direct_call(emit, function);
    }

    fn raw_native_call(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) {
        let [function, scratch] = [operands.physical_register(0), operands.physical_register(3)];
        use crate::target::registers::aarch64::{SP, X0, X1, X20};

        emit!(emit.output, Aarch64; Opcode::MoveRegister(IntegerWidth::U64) => [register scratch, register function];);

        if emit.object_format == crate::ObjectFormat::Coff {
            emit!(emit.output, Aarch64;
                Opcode::AddSubtractImmediate {
                    operation: AddSubtractOperation::Add,
                    shift: super::ImmediateShift::None,
                    flags: FlagUpdate::Preserve,
                } => [register X0, register SP, immediate super::COFF_RAW_NATIVE_RETURN_SLOT];
                Opcode::MoveRegister(IntegerWidth::U64) => [register X1, register X20];
            );
            indirect_call(emit, scratch);
            load(
                emit,
                MemoryWidth::DoubleWord,
                false,
                X1,
                MachineMemoryAddress::offset(SP, super::COFF_RAW_NATIVE_VARIANT_SLOT),
                &[],
            )
            .unwrap_or_else(|_| unreachable!("fixed raw-native variant slot is encodable"));
            load(
                emit,
                MemoryWidth::DoubleWord,
                false,
                X0,
                MachineMemoryAddress::offset(SP, super::COFF_RAW_NATIVE_RETURN_SLOT),
                &[],
            )
            .unwrap_or_else(|_| unreachable!("fixed raw-native return slot is encodable"));
        } else {
            emit!(emit.output, Aarch64; Opcode::MoveRegister(IntegerWidth::U64) => [register X0, register X20];);
            indirect_call(emit, scratch);
        }
    }

    fn slow_path_call(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
        let function = operands.relocation(0);
        let [dispatch_register, dispatch_scratch] = [operands.physical_register(1), operands.physical_register(2)];
        use crate::target::registers::aarch64::{X0, X1, X20, X26, X27, X28};

        let [execution_context, executable, program_counter, bytecode, values_offset] =
            interpreter_layout(emit.runtime, emit.handler)?;

        vm_pc_arguments(emit);
        store(
            emit,
            MemoryWidth::Word,
            MachineMemoryAddress::offset(X28, program_counter),
            StoreSource::Register(X1),
            [dispatch_register, dispatch_scratch],
        )
        .map_err(|error| memory_address_compile_error(emit.handler, error))?;
        instruction_argument(emit);
        direct_call(emit, function);
        emit!(emit.output, Aarch64; Opcode::BranchBitSetToExit(63) => [register X0];);

        load(
            emit,
            MemoryWidth::DoubleWord,
            false,
            X28,
            MachineMemoryAddress::offset(X20, execution_context),
            &[dispatch_register, dispatch_scratch],
        )
        .map_err(|error| memory_address_compile_error(emit.handler, error))?;
        load(
            emit,
            MemoryWidth::DoubleWord,
            false,
            dispatch_register,
            MachineMemoryAddress::offset(X28, executable),
            &[dispatch_register, dispatch_scratch],
        )
        .map_err(|error| memory_address_compile_error(emit.handler, error))?;
        load(
            emit,
            MemoryWidth::DoubleWord,
            false,
            X26,
            MachineMemoryAddress::offset(dispatch_register, bytecode),
            &[dispatch_register, dispatch_scratch],
        )
        .map_err(|error| memory_address_compile_error(emit.handler, error))?;
        address_offset(emit, X27, X28, values_offset, dispatch_register);
        emit!(emit.output, Aarch64; Opcode::SetInstructionPointer => [register X0];);
        dispatch_from_instruction_pointer(emit, dispatch_register, dispatch_scratch)
            .map_err(|error| memory_address_compile_error(emit.handler, error))
    }

    fn dispatch_current(&self, emit: &mut Emit<'_>, scratches: &[AllocatedOperand]) -> Result<(), CompileError> {
        let [opcode_scratch, target_scratch] = scratches.physical_registers();
        use crate::target::registers::aarch64::X25;

        emit!(emit.output, Aarch64; Opcode::SetInstructionPointer => [register X25];);
        dispatch_from_instruction_pointer(emit, opcode_scratch, target_scratch)
            .map_err(|error| memory_address_compile_error(emit.handler, error))
    }

    fn dispatch_variable(
        &self,
        emit: &mut Emit<'_>,
        size: PhysicalRegister,
        scratches: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let [opcode_scratch, target_scratch] = scratches.physical_registers();
        emit!(emit.output, Aarch64; Opcode::AdvanceInstructionPointer => [register size];);
        dispatch_from_instruction_pointer(emit, opcode_scratch, target_scratch)
            .map_err(|error| memory_address_compile_error(emit.handler, error))
    }

    fn dispatch_next(
        &self,
        emit: &mut Emit<'_>,
        size: u32,
        scratches: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let [opcode_scratch, target_scratch] = scratches.physical_registers();
        use crate::target::registers::aarch64::X21;

        if size > 4095 {
            return finalize_error(emit.handler, "AArch64 next-dispatch size exceeds its immediate range");
        }
        let size = i64::from(size);
        load(
            emit,
            MemoryWidth::Byte,
            false,
            opcode_scratch,
            MachineMemoryAddress::offset(X21, size),
            &[opcode_scratch],
        )
        .map_err(|error| memory_address_compile_error(emit.handler, error))?;
        emit!(emit.output, Aarch64;
            Opcode::AddSubtractImmediate {
            operation: AddSubtractOperation::Add,
            shift: super::ImmediateShift::None,
            flags: FlagUpdate::Preserve,
        } => [register X21, register X21, immediate size];
        );
        dispatch_tail(emit, opcode_scratch, target_scratch)
            .map_err(|error| memory_address_compile_error(emit.handler, error))
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
                assertion_compare(
                    emit,
                    operands.physical_register(0),
                    operands.operand(1),
                    operands.physical_register(2),
                    IntegerWidth::U64,
                );
                Condition::from_assertion(operation)
            }
            AssertionOperation::NonZero => {
                emit!(emit.output, Aarch64;
                    Opcode::CompareAndBranchZero {
                        width: IntegerWidth::U64,
                        condition: ZeroCondition::NonZero,
                } => [register operands.physical_register(0), label ok_label.clone()];
                );
                append_assertion_trap(emit, ok_label);
                return Ok(());
            }
            AssertionOperation::TagEqual | AssertionOperation::TagNotEqual => {
                let value = operands.physical_register(0);
                let tag = operands.operand(1);
                let tag_scratch = operands.physical_register(2);
                let compare_scratch = operands.physical_register(3);
                emit!(emit.output, Aarch64;
                    Opcode::ShiftImmediate {
                    operation: ShiftOperation::RightLogical,
                    width: IntegerWidth::U64,
                } => [register tag_scratch, register value, immediate 48];
                );
                assertion_compare(emit, tag_scratch, tag, compare_scratch, IntegerWidth::U64);
                Condition::from_assertion(operation)
            }
        };
        emit!(emit.output, Aarch64; Opcode::BranchCondition(condition) => [label ok_label.clone()];);
        append_assertion_trap(emit, ok_label);
        Ok(())
    }

    fn finalize_scalar_compare_branch(
        &self,
        emit: &mut Emit<'_>,
        operation: Operation,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let (width, condition) = operation.scalar_branch().expect("allocated scalar branch was verified");
        let lhs = operands.physical_register(0);
        let rhs = operands.operand(1);
        let scratch = operands.physical_register(2);
        let target = operands.label(3);
        compare(
            emit,
            lhs,
            rhs,
            scratch,
            if width == IntegerWidth::U16 {
                IntegerWidth::U32
            } else {
                width
            },
        );
        branch_scalar(emit, condition, &target);
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
        let (value, mask_or_bit, scratch, target) = match (operation, operands) {
            (
                Operation::Branch(BranchOperation::Bits { .. }),
                [value, mask, scratch, AllocatedOperand::Label(target)],
            ) => (value, mask, Some(scratch), target),
            (_, [value, bit, AllocatedOperand::Label(target)]) => (value, bit, None, target),
            _ => unreachable!("allocated operation and operands were verified"),
        };
        let value = verified_register(value);
        let scratch = scratch.map(verified_register);
        if operation == Operation::branch_bit(condition) {
            branch_bit(emit, value, operands.immediate(1), condition, target);
            return Ok(());
        }
        let scratch = scratch.expect("AArch64 mask branch must have a scratch");
        if let AllocatedOperand::Immediate(mask) = mask_or_bit {
            branch_mask_immediate(emit, value, *mask, scratch, condition, target);
        } else {
            let mask = verified_register(mask_or_bit);
            branch_mask_register(emit, value, mask, condition, target);
        }
        Ok(())
    }

    fn finalize_value_representation_branch(
        &self,
        emit: &mut Emit<'_>,
        operation: Operation,
        operands: &[AllocatedOperand],
    ) {
        let (condition, compare_scratch, target) = match operation {
            Operation::Branch(BranchOperation::Tag(condition)) => {
                (condition, Some(operands.physical_register(3)), operands.label(4))
            }
            Operation::Branch(BranchOperation::Singleton(condition)) => (condition, None, operands.label(3)),
            _ => unreachable!("allocated operation and operands were verified"),
        };
        let operands = ValueRepresentationBranch {
            value: operands.physical_register(0),
            representation: operands.immediate(1),
            scratch: operands.physical_register(2),
            condition,
            target,
        };
        match operation {
            Operation::Branch(BranchOperation::Tag(_)) => branch_tag(
                emit,
                operands,
                compare_scratch.expect("AArch64 tag branch must have two scratches"),
            ),
            Operation::Branch(BranchOperation::Singleton(_)) => branch_singleton(emit, operands),
            _ => unreachable!(),
        }
    }

    fn branch_memory(
        &self,
        emit: &mut Emit<'_>,
        operation: Operation,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        use super::super::description::MemoryBranchKind;

        let kind = operation.memory_branch().expect("allocated memory branch was verified");
        if let MemoryBranchKind::CompareRegister { width, .. } = kind
            && !matches!(width, IntegerWidth::U32 | IntegerWidth::U64)
        {
            return emit.error("AArch64 memory comparison width must be 32 or 64 bits");
        }
        let is_test = matches!(kind, MemoryBranchKind::TestByte(_));
        let address = machine_address(operands.operand(0));
        let branch = memory_branch(operation, operands, is_test.then_some(3), emit.handler)?;
        let load_scratch = operands.physical_register(2);
        let target = &operands.label(if is_test { 4 } else { 3 });
        emit_memory_branch(
            emit,
            MemoryBranchOperands {
                address,
                branch,
                load_scratch,
                target,
            },
        )
        .map_err(|error| memory_address_compile_error(emit.handler, error))
    }

    fn finalize_any_equal_branch(
        &self,
        emit: &mut Emit<'_>,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let [value, comparison_values @ .., scratch, AllocatedOperand::Label(target)] = operands else {
            unreachable!("allocated operand shape was verified");
        };
        branch_any_equal(
            emit,
            verified_register(value),
            comparison_values,
            verified_register(scratch),
            target,
        );
        Ok(())
    }

    fn finalize_canonicalize_nan(
        &self,
        emit: &mut Emit<'_>,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let (fixup_label, return_label) = emit.unique_label_pair("canon_nan");
        use crate::target::registers::aarch64::D8;

        let [destination, source] = operands.physical_registers();

        emit!(emit.output, Aarch64;
            Opcode::MoveFloatBits64 => [register destination, register source];
            Opcode::CompareDouble => [register source, register source];
            Opcode::BranchOverflow => [label fixup_label.clone()];
            Opcode::Label => [label return_label.clone()];
        );
        emit!(emit.cold, Aarch64;
            Opcode::Label => [label fixup_label];
            Opcode::MoveFloatBits64 => [register destination, register D8];
            Opcode::Branch => [label return_label];
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
        if opcode == ArchitectureOpcode::Aarch64(Opcode::Pseudo) {
            move_immediate(emit, operands.physical_register(0), operands.immediate(1), width);
            return Ok(());
        }
        push_plain_move(emit, opcode, width, operands);
        Ok(())
    }

    fn finalize_effective_address(
        &self,
        emit: &mut Emit<'_>,
        _opcode: ArchitectureOpcode,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let destination = operands.physical_register(0);
        let address = machine_address(operands.operand(1));
        let scratch = operands.physical_register(2);
        let base = address.base;
        match (address.index, address.scale, address.displacement) {
            (None, None, None) => {
                emit!(emit.output, Aarch64; Opcode::MoveRegister(IntegerWidth::U64) => [register destination, register base];);
            }
            (None, None, Some(offset)) => {
                address_offset(emit, destination, base, offset, scratch);
            }
            (Some(index), None, None) => {
                if base == X26 && index == X25 {
                    emit!(emit.output, Aarch64; Opcode::MoveRegister(IntegerWidth::U64) => [register destination, register X21];);
                } else {
                    address_add_register(emit, destination, base, index);
                }
            }
            (Some(index), Some(scale), None) => {
                let shift = match scale {
                    1 => Some(None),
                    2 => Some(Some(AddressIndexShift::One)),
                    4 => Some(Some(AddressIndexShift::Two)),
                    8 => Some(Some(AddressIndexShift::Three)),
                    _ => None,
                };
                if let Some(shift) = shift {
                    emit!(emit.output, Aarch64;
                        if let Some(shift) = shift {
                        Opcode::AddShiftedRegister { shift }
                    } else {
                        Opcode::AddSubtractRegister {
                            operation: AddSubtractOperation::Add,
                            flags: FlagUpdate::Preserve,
                        }
                    } => [register destination, register base, register index];
                    );
                } else {
                    move_immediate(emit, scratch, scale, IntegerWidth::U64);
                    emit!(emit.output, Aarch64; Opcode::MultiplyAdd64 => [register destination, register index, register scratch, register base];);
                }
            }
            (Some(index), None, Some(offset)) => {
                if base == X26 && index == X25 {
                    address_offset(emit, destination, X21, offset, scratch);
                } else {
                    address_add_register(emit, destination, base, index);
                    address_offset(emit, destination, destination, offset, scratch);
                }
            }
            components => {
                return finalize_error(
                    emit.handler,
                    format!("effective address has unencodable normalized components: {components:?}"),
                );
            }
        }
        Ok(())
    }

    fn finalize_vm_load(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) {
        use crate::target::registers::aarch64::X20;

        let destination = operands.physical_register(0);
        emit!(emit.output, Aarch64; Opcode::MoveRegister(IntegerWidth::U64) => [register destination, register X20];);
    }

    fn finalize_program_counter_load(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) {
        // The dispatch register holds `pb + pc`, so the program counter is the
        // distance between it and the bytecode base.
        let destination = operands.physical_register(0);
        emit!(emit.output, Aarch64; Opcode::Subtract32Register => [register destination, register X21, register X26];);
    }

    fn finalize_operand_store(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
        operand_store(emit, operands.immediate(0), operands.operand(1), &operands[2..])
    }

    fn finalize_label_load(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
        use crate::target::registers::aarch64::X21;

        let destination = operands.physical_register(0);
        let offset = operands.immediate(1);
        let address_scratch = operands.physical_register(2);
        load(
            emit,
            MemoryWidth::Word,
            false,
            destination,
            MachineMemoryAddress::offset(X21, offset),
            &[address_scratch],
        )
        .map_err(|error| memory_address_compile_error(emit.handler, error))
    }

    fn goto_handler(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
        let [target, opcode_scratch, target_scratch] = operands.physical_registers();
        emit!(emit.output, Aarch64; Opcode::SetInstructionPointer => [register target];);
        dispatch_from_instruction_pointer(emit, opcode_scratch, target_scratch)
            .map_err(|error| memory_address_compile_error(emit.handler, error))
    }

    fn goto_bytecode_target(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
        let offset = operands.immediate(0);
        let [bytecode_target_scratch, opcode_scratch, dispatch_target_scratch] = [
            operands.physical_register(1),
            operands.physical_register(2),
            operands.physical_register(3),
        ];
        use crate::target::registers::aarch64::X21;

        load(
            emit,
            MemoryWidth::Word,
            false,
            bytecode_target_scratch,
            MachineMemoryAddress::offset(X21, offset),
            &[bytecode_target_scratch],
        )
        .map_err(|error| memory_address_compile_error(emit.handler, error))?;
        emit!(emit.output, Aarch64; Opcode::SetInstructionPointer => [register bytecode_target_scratch];);
        dispatch_from_instruction_pointer(emit, opcode_scratch, dispatch_target_scratch)
            .map_err(|error| memory_address_compile_error(emit.handler, error))
    }

    fn finalize_execution_context_store(
        &self,
        emit: &mut Emit<'_>,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        use crate::target::registers::aarch64::X28;

        let address = machine_address(operands.operand(0));
        let value_scratch = operands.physical_register(1);
        let address_scratch = operands.physical_register(2);
        store(
            emit,
            MemoryWidth::DoubleWord,
            address,
            StoreSource::Register(X28),
            [value_scratch, address_scratch],
        )
        .map_err(|error| memory_address_compile_error(emit.handler, error))
    }

    fn finalize_indexed_offset_store(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) {
        let [base, index, source, address_scratch] = [0, 1, 4, 5].map(|index| operands.physical_register(index));
        let [scale, offset] = [2, 3].map(|index| operands.immediate(index));
        address_offset(emit, address_scratch, base, offset, address_scratch);

        let (addressing, address) = match scale {
            1 => (
                MemoryAddressing::Register,
                MachineMemoryAddress::indexed(address_scratch, index),
            ),
            8 => (
                MemoryAddressing::ShiftedRegister(AddressIndexShift::Three),
                MachineMemoryAddress::indexed(address_scratch, index),
            ),
            2 | 4 => {
                emit!(emit.output, Aarch64;
                    Opcode::AddShiftedRegister {
                    shift: if scale == 2 {
                        AddressIndexShift::One
                    } else {
                        AddressIndexShift::Two
                    },
                } => [register address_scratch, register address_scratch, register index];
                );
                (MemoryAddressing::Base, MachineMemoryAddress::base(address_scratch))
            }
            _ => unreachable!("indexed offset store scale was validated"),
        };
        emit!(emit.output, Aarch64;
            Opcode::Store {
            width: MemoryWidth::DoubleWord,
            addressing,
        } => [address address, register source];
        );
    }

    fn finalize_memory_increment(
        &self,
        emit: &mut Emit<'_>,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let address = machine_address(operands.operand(0));
        let value_scratch = operands.physical_register(1);
        let address_scratch = operands.physical_register(2);
        let (addressing, address) =
            memory_address(emit, MemoryWidth::Word, address, &[value_scratch], &[address_scratch])
                .map_err(|error| memory_address_compile_error(emit.handler, error))?;
        emit!(emit.output, Aarch64;
            Opcode::Load {
                width: MemoryWidth::Word,
                signed: false,
                addressing,
            } => [register value_scratch, address address.clone()];
            Opcode::Increment32Register => [register value_scratch];
            Opcode::Store {
                width: MemoryWidth::Word,
                addressing,
            } => [address address, register value_scratch];
        );
        Ok(())
    }

    fn finalize_scalar_load(
        &self,
        emit: &mut Emit<'_>,
        _opcode: ArchitectureOpcode,
        width: MemoryWidth,
        signed: bool,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let destination = operands.physical_register(0);
        let address = machine_address(operands.operand(1));
        let address_scratch = operands.physical_register(2);
        load(emit, width, signed, destination, address, &[address_scratch])
            .map_err(|error| memory_address_compile_error(emit.handler, error))
    }

    fn finalize_scalar_store(
        &self,
        emit: &mut Emit<'_>,
        _opcode: ArchitectureOpcode,
        width: MemoryWidth,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let address = machine_address(operands.operand(0));
        let source = operands.operand(1);
        let value_scratch = operands.physical_register(2);
        let address_scratch = operands.physical_register(3);
        let source = match source {
            AllocatedOperand::Immediate(value) => StoreSource::Immediate(*value),
            source => StoreSource::Register(verified_register(source)),
        };
        store(emit, width, address, source, [value_scratch, address_scratch])
            .map_err(|error| memory_address_compile_error(emit.handler, error))
    }

    fn finalize_pair_load(
        &self,
        emit: &mut Emit<'_>,
        width: PairWidth,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let (destinations, address, scratches) = pair_access::<2>(operands, width, true, emit.handler)?;
        pair_load(emit, width, destinations, address, scratches)
    }

    fn finalize_pair_store(
        &self,
        emit: &mut Emit<'_>,
        width: PairWidth,
        indexed: bool,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        if indexed {
            let (address_registers, scale, sources, [address_scratch]) =
                decode_indexed_pair_store::<1>(operands, width, emit.handler)?;
            indexed_pair_store(emit, address_registers, scale, sources, address_scratch);
            return Ok(());
        }
        let (sources, address, scratches) = pair_access::<2>(operands, width, false, emit.handler)?;
        pair_store(emit, width, sources, address, scratches)
    }

    fn finalize_or32_branch(
        &self,
        emit: &mut Emit<'_>,
        condition: SignCondition,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let [destination, lhs, rhs, scratch, AllocatedOperand::Label(target)] = operands else {
            unreachable!("allocated OR-branch operands were verified");
        };
        self.finalize_binary_operation(
            emit,
            BinaryOperation::Or,
            IntegerWidth::U32,
            &[destination.clone(), lhs.clone(), rhs.clone(), scratch.clone()],
        )?;
        let destination = verified_register(destination);
        emit!(emit.output, Aarch64; Opcode::BranchOnBit31(condition) => [register destination, label target.clone()];);
        Ok(())
    }

    fn add_subtract(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError> {
        let opcode = opcode.aarch64();
        let destination = operands.physical_register(0);
        let rhs = operands.operand(1);
        let scratch = operands.physical_register(2);
        match (opcode, rhs) {
            (Opcode::AddSubtractZero(_), AllocatedOperand::Immediate(0)) => {}
            (selected @ Opcode::AddSubtractImmediate { .. }, AllocatedOperand::Immediate(value)) => {
                emit!(emit.output, Aarch64; selected => [register destination, register destination, immediate *value];);
            }
            (selected @ Opcode::AddSubtractRegister { .. }, rhs) => {
                let rhs = verified_register(rhs);
                emit!(emit.output, Aarch64; selected => [register destination, register destination, register rhs];);
            }
            (Opcode::AddSubtractLargeImmediate { operation, flags }, AllocatedOperand::Immediate(value)) => {
                move_immediate(emit, scratch, *value, IntegerWidth::U64);
                emit!(emit.output, Aarch64; Opcode::AddSubtractRegister { operation, flags } => [register destination, register destination, register scratch];);
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
            multiply(emit, width, operands);
            return Ok(());
        }

        match (operation, width, operands) {
            (
                BinaryOperation::And | BinaryOperation::Or | BinaryOperation::Xor,
                IntegerWidth::U32,
                [destination, lhs, rhs, scratch],
            ) => {
                logical(
                    emit,
                    operation,
                    width,
                    LogicalOperands {
                        destination: verified_register(destination),
                        lhs: verified_register(lhs),
                        rhs,
                        scratch: verified_register(scratch),
                    },
                );
            }
            (BinaryOperation::And, IntegerWidth::U16, [destination, rhs, scratch]) => {
                let destination = verified_register(destination);
                logical(
                    emit,
                    operation,
                    width,
                    LogicalOperands {
                        destination,
                        lhs: destination,
                        rhs,
                        scratch: verified_register(scratch),
                    },
                );
            }
            (
                BinaryOperation::And | BinaryOperation::Or | BinaryOperation::Xor,
                IntegerWidth::U64,
                [destination, rhs, scratch],
            ) => {
                let destination = verified_register(destination);
                let scratch = verified_register(scratch);
                if operation == BinaryOperation::Xor && operand_register(rhs) == Some(destination) {
                    emit!(emit.output, Aarch64; Opcode::MoveImmediateDecimal(IntegerWidth::U64) => [register destination, immediate 0];);
                } else if operation == BinaryOperation::And
                    && matches!(rhs, AllocatedOperand::Immediate(value) if *value as u64 == 0xffff_ffff)
                {
                    emit!(emit.output, Aarch64; Opcode::MoveRegister(IntegerWidth::U32) => [register destination, register destination];);
                } else {
                    logical(
                        emit,
                        operation,
                        width,
                        LogicalOperands {
                            destination,
                            lhs: destination,
                            rhs,
                            scratch,
                        },
                    );
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
        let branch = match (operation, operands) {
            (
                operation @ (OverflowOperation::AddWithOverflow | OverflowOperation::SubtractWithOverflow),
                [destination, rhs, scratch, target_operand],
            ) => {
                let destination = verified_register(destination);
                let scratch = verified_register(scratch);
                let operation = match operation {
                    OverflowOperation::AddWithOverflow => AddSubtractOperation::Add,
                    OverflowOperation::SubtractWithOverflow => AddSubtractOperation::Subtract,
                    _ => unreachable!(),
                };
                match rhs {
                    AllocatedOperand::Immediate(value) if *value > 0 && *value <= 4095 => {
                        emit!(emit.output, Aarch64; Opcode::AddSubtract32ImmediateWithFlags(operation) => [register destination, immediate *value];);
                    }
                    AllocatedOperand::Immediate(value) => {
                        move_immediate(emit, scratch, *value, IntegerWidth::U32);
                        push_overflow_add_subtract_register(emit, operation, destination, scratch);
                    }
                    rhs => push_overflow_add_subtract_register(emit, operation, destination, verified_register(rhs)),
                }
                (Opcode::BranchOverflow, verified_label(target_operand), None)
            }
            (
                operation @ (OverflowOperation::Increment | OverflowOperation::Decrement),
                [destination, target_operand],
            ) => {
                let operation = match operation {
                    OverflowOperation::Increment => AddSubtractOperation::Add,
                    OverflowOperation::Decrement => AddSubtractOperation::Subtract,
                    _ => unreachable!(),
                };
                emit!(emit.output, Aarch64; Opcode::AddSubtract32ImmediateWithFlags(operation) => [register verified_register(destination), immediate 1];);
                (Opcode::BranchOverflow, verified_label(target_operand), None)
            }
            (OverflowOperation::MultiplyWithOverflow, [destination, source, scratch, target_operand]) => {
                let destination = verified_register(destination);
                overflow_multiply(
                    emit,
                    destination,
                    destination,
                    verified_register(source),
                    verified_register(scratch),
                );
                (
                    Opcode::BranchNotEqual,
                    verified_label(target_operand),
                    Some(destination),
                )
            }
            (OverflowOperation::MultiplyCopy, [destination, lhs, rhs, scratch, target_operand]) => {
                let destination = verified_register(destination);
                overflow_multiply(
                    emit,
                    destination,
                    verified_register(lhs),
                    verified_register(rhs),
                    verified_register(scratch),
                );
                (
                    Opcode::BranchNotEqual,
                    verified_label(target_operand),
                    Some(destination),
                )
            }
            (OverflowOperation::Negate, [destination, target_operand]) => {
                emit!(emit.output, Aarch64; Opcode::Negate32WithFlags => [register verified_register(destination)];);
                (Opcode::BranchOverflow, verified_label(target_operand), None)
            }
            _ => unreachable!("allocated operation and operands were verified"),
        };
        emit!(emit.output, Aarch64; branch.0 => [label branch.1];);
        if let Some(destination) = branch.2 {
            emit!(emit.output, Aarch64; Opcode::MoveRegister(IntegerWidth::U32) => [register destination, register destination];);
        }
    }

    fn finalize_shift(
        &self,
        emit: &mut Emit<'_>,
        operation: ShiftOperation,
        width: IntegerWidth,
        operands: &[AllocatedOperand],
    ) {
        match (width, operands) {
            (IntegerWidth::U64, [destination, count]) => {
                let destination = verified_register(destination);
                match count {
                    AllocatedOperand::Immediate(0) => {}
                    AllocatedOperand::Immediate(count) => {
                        emit!(emit.output, Aarch64; Opcode::ShiftImmediate { operation, width } => [register destination, register destination, immediate *count];);
                    }
                    count => {
                        let count = verified_register(count);
                        emit!(emit.output, Aarch64; Opcode::ShiftRegister { operation, width } => [register destination, register destination, register count];);
                    }
                }
            }
            (IntegerWidth::U32, [destination, lhs, count]) => {
                let destination = verified_register(destination);
                let lhs = verified_register(lhs);
                match count {
                    AllocatedOperand::Immediate(0) if destination == lhs => {}
                    AllocatedOperand::Immediate(0) => {
                        emit!(emit.output, Aarch64; Opcode::MoveRegister(width) => [register destination, register lhs];);
                    }
                    AllocatedOperand::Immediate(count) => {
                        emit!(emit.output, Aarch64; Opcode::ShiftImmediate { operation, width } => [register destination, register lhs, immediate *count];);
                    }
                    count => {
                        let count = verified_register(count);
                        emit!(emit.output, Aarch64; Opcode::ShiftRegister { operation, width } => [register destination, register lhs, register count];);
                    }
                }
            }
            _ => unreachable!("allocated operation and operands were verified"),
        }
    }

    fn finalize_divide(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) {
        let [quotient, remainder, dividend, divisor, scratch] = operands.physical_registers();
        emit!(emit.output, Aarch64; Opcode::SignedDivide64 => [register scratch, register dividend, register divisor];);
        emit!(emit.output, Aarch64; Opcode::MultiplySubtract64 => [register remainder, register scratch, register divisor, register dividend];);
        if quotient != remainder {
            emit!(emit.output, Aarch64; Opcode::MoveRegister(IntegerWidth::U64) => [register quotient, register scratch];);
        }
    }
}
