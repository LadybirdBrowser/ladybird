/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Shared helpers for architecture-specific machine finalization.

use super::description::{EqualityCondition, IntegerWidth, PairWidth, TestCondition};
use super::ir::{AllocatedOperand, MachineInstruction, MachineMemoryAddress, RuntimeConstants};
use super::registers::PhysicalRegister;
use crate::frontend::layout::KnownLayoutConstant;
use crate::low_ir::{Label, Relocation};
use crate::{CompileError, CompileOptions, CompileStage, ObjectFormat};

/// Everything a backend needs while finalizing one function: where machine
/// instructions go, the target configuration, and the state that outlives a
/// single instruction.
///
/// Backends receive this instead of an ad-hoc selection of output vector,
/// runtime constants, handler name and target options, so that the two
/// architectures present the same signature for every operation even when
/// only one of them needs a given piece of context.
pub(crate) struct Emit<'a> {
    pub(crate) output: Vec<MachineInstruction>,
    /// Instructions parked outside the hot path, emitted after the function.
    pub(crate) cold: Vec<MachineInstruction>,
    pub(crate) runtime: &'a RuntimeConstants,
    pub(crate) handler: &'a str,
    pub(crate) handler_size: Option<u32>,
    pub(crate) object_format: ObjectFormat,
    pub(crate) enable_assertions: bool,
    pub(crate) has_jscvt: bool,
    /// Distinguishes the local labels a single function generates.
    unique_counter: u64,
    /// The operands of the most recent floating-point comparison, so that a
    /// chain of branches on the same comparison only emits it once.
    pub(crate) last_fp_compare: Option<(PhysicalRegister, PhysicalRegister)>,
}

impl<'a> Emit<'a> {
    pub(crate) fn new(
        runtime: &'a RuntimeConstants,
        handler: &'a str,
        handler_size: Option<u32>,
        options: &CompileOptions,
    ) -> Self {
        Self {
            output: Vec::new(),
            cold: Vec::new(),
            runtime,
            handler,
            handler_size,
            object_format: options.target.object_format,
            enable_assertions: options.enable_assertions,
            has_jscvt: options.has_jscvt,
            unique_counter: 0,
            last_fp_compare: None,
        }
    }

    /// Take the instructions emitted so far, leaving the rest of the state in
    /// place for the next run of instructions.
    pub(crate) fn take_output(&mut self) -> Vec<MachineInstruction> {
        std::mem::take(&mut self.output)
    }

    pub(crate) fn error<T>(&self, message: impl Into<String>) -> Result<T, CompileError> {
        finalize_error(self.handler, message)
    }

    /// A layout constant the handler cannot be finalized without.
    pub(crate) fn constant(&self, constant: KnownLayoutConstant) -> Result<i64, CompileError> {
        required_runtime_constant(self.runtime, constant, self.handler)
    }

    /// A label unique within the function being finalized.
    pub(crate) fn unique_label(&mut self, prefix: &str) -> Label {
        let label = Label::from(format!(".{prefix}_{}", self.unique_counter));
        self.unique_counter += 1;
        label
    }

    /// A unique label and its matching return label, for code that branches
    /// out to a cold fixup and comes back.
    pub(crate) fn unique_label_pair(&mut self, prefix: &str) -> (Label, Label) {
        let counter = self.unique_counter;
        self.unique_counter += 1;
        (
            Label::from(format!(".{prefix}_{counter}")),
            Label::from(format!(".{prefix}_{counter}_ret")),
        )
    }
}

pub(crate) enum MemoryBranch {
    CompareRegister {
        width: IntegerWidth,
        condition: EqualityCondition,
        rhs: PhysicalRegister,
    },
    CompareByte {
        condition: EqualityCondition,
        immediate: i64,
    },
    TestByte {
        condition: TestCondition,
        immediate: i64,
        mask_scratch: Option<PhysicalRegister>,
    },
}

pub(crate) trait AllocatedOperands {
    fn operand(&self, index: usize) -> &AllocatedOperand;
    fn physical_register(&self, index: usize) -> PhysicalRegister;
    fn physical_registers<const N: usize>(&self) -> [PhysicalRegister; N];
    fn immediate(&self, index: usize) -> i64;
    fn label(&self, index: usize) -> Label;
    fn relocation(&self, index: usize) -> Relocation;
}

impl AllocatedOperands for [AllocatedOperand] {
    fn operand(&self, index: usize) -> &AllocatedOperand {
        self.get(index).expect("allocated instruction operands were verified")
    }

    fn physical_register(&self, index: usize) -> PhysicalRegister {
        verified_register(self.operand(index))
    }

    fn physical_registers<const N: usize>(&self) -> [PhysicalRegister; N] {
        std::array::from_fn(|index| self.physical_register(index))
    }

    fn immediate(&self, index: usize) -> i64 {
        let AllocatedOperand::Immediate(immediate) = self.operand(index) else {
            unreachable!("allocated immediate operand was verified")
        };
        *immediate
    }

    fn label(&self, index: usize) -> Label {
        verified_label(self.operand(index))
    }

    fn relocation(&self, index: usize) -> Relocation {
        let AllocatedOperand::Relocation(relocation) = self.operand(index) else {
            unreachable!("allocated relocation operand was verified")
        };
        relocation.clone()
    }
}

pub(crate) fn operand_register(operand: &AllocatedOperand) -> Option<PhysicalRegister> {
    operand.physical_register()
}

pub(crate) fn verified_register(operand: &AllocatedOperand) -> PhysicalRegister {
    operand_register(operand).expect("allocated register operand was verified")
}

pub(crate) fn verified_label(operand: &AllocatedOperand) -> Label {
    let AllocatedOperand::Label(label) = operand else {
        unreachable!("allocated label operand was verified");
    };
    label.clone()
}

pub(crate) fn finalization_error(handler: &str, message: impl Into<String>) -> CompileError {
    CompileError::new(CompileStage::Finalization, Some(handler), message)
}

pub(crate) fn finalize_error<T>(handler: &str, message: impl Into<String>) -> Result<T, CompileError> {
    Err(finalization_error(handler, message))
}

pub(crate) fn required_byte_immediate(operand: &AllocatedOperand, handler: &str) -> Result<i64, CompileError> {
    let AllocatedOperand::Immediate(immediate) = operand else {
        return finalize_error(handler, "byte memory branch value must be an immediate");
    };
    if i8::try_from(*immediate).is_err() && u8::try_from(*immediate).is_err() {
        return finalize_error(handler, "byte memory branch immediate must fit in 8 bits");
    }
    Ok(*immediate)
}

pub(crate) fn required_runtime_constant(
    runtime: &RuntimeConstants,
    constant: KnownLayoutConstant,
    handler: &str,
) -> Result<i64, CompileError> {
    runtime
        .get(constant)
        .ok_or_else(|| finalization_error(handler, format!("{} constant is required", constant.name())))
}

pub(crate) fn interpreter_layout(runtime: &RuntimeConstants, handler: &str) -> Result<[i64; 5], CompileError> {
    use KnownLayoutConstant::*;

    Ok([
        required_runtime_constant(runtime, VmRunningExecutionContext, handler)?,
        required_runtime_constant(runtime, ExecutionContextExecutable, handler)?,
        required_runtime_constant(runtime, ExecutionContextProgramCounter, handler)?,
        required_runtime_constant(runtime, ExecutableBytecodeData, handler)?,
        required_runtime_constant(runtime, SizeOfExecutionContext, handler)?,
    ])
}

pub(crate) fn machine_address(operand: &AllocatedOperand) -> MachineMemoryAddress {
    let AllocatedOperand::Address(address) = operand else {
        unreachable!("allocated memory operand was verified");
    };
    address.clone()
}

pub(crate) fn machine_memory_pair_address(
    first: &AllocatedOperand,
    second: &AllocatedOperand,
    element_size: i64,
    handler: &str,
) -> Result<MachineMemoryAddress, CompileError> {
    let first = machine_address(first);
    let second = machine_address(second);
    if first.scale.is_some() || second.scale.is_some() {
        return finalize_error(handler, "paired memory addresses may not use scaled indices");
    }
    if first.base != second.base || first.index != second.index {
        return finalize_error(handler, "paired memory addresses must use the same base and index");
    }
    let first_offset = first.displacement.unwrap_or(0);
    let expected_second = first_offset
        .checked_add(element_size)
        .ok_or_else(|| finalization_error(handler, "paired memory address overflow"))?;
    if second.displacement.unwrap_or(0) != expected_second {
        return finalize_error(
            handler,
            format!("paired memory addresses must be adjacent {element_size}-byte fields in order"),
        );
    }
    Ok(MachineMemoryAddress {
        base: first.base,
        index: first.index,
        scale: None,
        displacement: (first_offset != 0).then_some(first_offset),
    })
}

pub(crate) fn pair_element_size(width: PairWidth) -> i64 {
    match width {
        PairWidth::Word => 4,
        PairWidth::DoubleWord => 8,
    }
}

pub(crate) fn pair_access<const SCRATCH_COUNT: usize>(
    operands: &[AllocatedOperand],
    width: PairWidth,
    is_load: bool,
    handler: &str,
) -> Result<
    (
        [PhysicalRegister; 2],
        MachineMemoryAddress,
        [PhysicalRegister; SCRATCH_COUNT],
    ),
    CompileError,
> {
    let (registers, addresses) = if is_load {
        (&operands[..2], &operands[2..4])
    } else {
        (&operands[2..4], &operands[..2])
    };
    Ok((
        std::array::from_fn(|index| verified_register(&registers[index])),
        machine_memory_pair_address(&addresses[0], &addresses[1], pair_element_size(width), handler)?,
        std::array::from_fn(|index| verified_register(&operands[4 + index])),
    ))
}

type IndexedPairStore<const SCRATCH_COUNT: usize> = (
    [PhysicalRegister; 2],
    i64,
    [PhysicalRegister; 2],
    [PhysicalRegister; SCRATCH_COUNT],
);

pub(crate) fn indexed_pair_store<const SCRATCH_COUNT: usize>(
    operands: &[AllocatedOperand],
    width: PairWidth,
    handler: &str,
) -> Result<IndexedPairStore<SCRATCH_COUNT>, CompileError> {
    if width != PairWidth::DoubleWord {
        return finalize_error(handler, "indexed pair store must use double-word values");
    }
    let [
        base,
        index,
        AllocatedOperand::Immediate(scale),
        first,
        second,
        scratches @ ..,
    ] = operands
    else {
        return finalize_error(handler, "indexed pair store has invalid target operands");
    };
    if !matches!(scale, 1 | 2 | 4 | 8) {
        return finalize_error(handler, "indexed pair-store scale must be 1, 2, 4, or 8");
    }
    if scratches.len() != SCRATCH_COUNT {
        unreachable!("allocated indexed pair-store scratch count was verified");
    }
    Ok((
        [verified_register(base), verified_register(index)],
        *scale,
        [verified_register(first), verified_register(second)],
        std::array::from_fn(|index| verified_register(&scratches[index])),
    ))
}

pub(crate) fn x86_values_offset(runtime: &RuntimeConstants, handler: &str) -> Result<i64, CompileError> {
    required_runtime_constant(runtime, KnownLayoutConstant::SizeOfExecutionContext, handler)
}

/// Decode a memory-branch instruction's comparison from its operands.
///
/// `mask_scratch` is the operand index holding the scratch register an
/// architecture needs to materialize a test mask, if it needs one at all.
pub(crate) fn memory_branch(
    operation: super::description::Operation,
    operands: &[AllocatedOperand],
    mask_scratch: Option<usize>,
    handler: &str,
) -> Result<MemoryBranch, CompileError> {
    use super::description::MemoryBranchKind;

    let kind = operation.memory_branch().expect("allocated memory branch was verified");
    let rhs = operands.operand(1);
    Ok(match kind {
        MemoryBranchKind::CompareRegister { width, condition } => MemoryBranch::CompareRegister {
            width,
            condition,
            rhs: verified_register(rhs),
        },
        MemoryBranchKind::CompareByte(condition) => MemoryBranch::CompareByte {
            condition,
            immediate: required_byte_immediate(rhs, handler)?,
        },
        MemoryBranchKind::TestByte(condition) => MemoryBranch::TestByte {
            condition,
            immediate: required_byte_immediate(rhs, handler)?,
            mask_scratch: mask_scratch.map(|index| operands.physical_register(index)),
        },
    })
}

/// The default lowering of a move: a register-to-register move whose source
/// and destination were coalesced to the same register is dropped, except at
/// 32 bits where the move also clears the upper half.
pub(crate) fn push_plain_move(
    emit: &mut Emit<'_>,
    opcode: super::ir::MachineOpcode,
    width: IntegerWidth,
    operands: &[AllocatedOperand],
) {
    if width != IntegerWidth::U32
        && matches!(
            operands,
            [
                AllocatedOperand::PhysicalRegister(destination),
                AllocatedOperand::PhysicalRegister(source),
            ] if destination == source
        )
    {
        return;
    }
    emit.output.push(MachineInstruction {
        opcode,
        operands: operands.to_vec(),
    });
}
