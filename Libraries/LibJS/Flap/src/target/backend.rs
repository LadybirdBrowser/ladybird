/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The interface every Flap machine backend implements.
//!
//! Finalization decides *which* operation a verified instruction denotes and
//! leaves the backend to decide *how* to express it. Methods that need
//! architecture-specific scratch registers take the whole allocated operand
//! list and decode it themselves, so shared code never indexes an operand
//! list differently depending on the target.
//!
//! Making this a trait rather than a dispatch macro means the compiler
//! rejects a backend that forgets an operation, instead of failing only for
//! the handlers that happen to use it.

use super::description::ArchitectureOpcode;
use super::description::{
    AssertionOperation, BinaryOperation, FloatCondition, FloatingPointOperation, IntegerWidth, MemoryWidth, Operation,
    OverflowOperation, PairWidth, ShiftOperation, SignCondition, ZeroCondition,
};
use super::finalize_support::Emit;
use super::ir::AllocatedOperand;
use super::registers::PhysicalRegister;
use crate::CompileError;
use crate::low_ir::Label;

pub(crate) trait Backend: Sync {
    fn branch_float(
        &self,
        emit: &mut Emit<'_>,
        lhs: PhysicalRegister,
        rhs: PhysicalRegister,
        emit_compare: bool,
        condition: FloatCondition,
        target: &Label,
    );

    fn branch_zero(
        &self,
        emit: &mut Emit<'_>,
        value: PhysicalRegister,
        width: IntegerWidth,
        condition: ZeroCondition,
        target: &Label,
    );

    fn branch_sign(
        &self,
        emit: &mut Emit<'_>,
        value: PhysicalRegister,
        width: IntegerWidth,
        condition: SignCondition,
        target: &Label,
    );

    fn box_int32(
        &self,
        emit: &mut Emit<'_>,
        destination: PhysicalRegister,
        source: PhysicalRegister,
        clean: bool,
    ) -> Result<(), CompileError>;

    fn update_bit(&self, emit: &mut Emit<'_>, destination: PhysicalRegister, bit: u32, operation: Operation);

    fn extract_tag(&self, emit: &mut Emit<'_>, destination: PhysicalRegister, source: PhysicalRegister);

    fn unbox_object(&self, emit: &mut Emit<'_>, destination: PhysicalRegister, source: PhysicalRegister);

    fn float_operation(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        _operation: FloatingPointOperation,
        operands: &[AllocatedOperand],
    );

    fn checked_float_conversion(
        &self,
        emit: &mut Emit<'_>,
        operation: FloatingPointOperation,
        destination: PhysicalRegister,
        source: PhysicalRegister,
        scratches: &[AllocatedOperand],
        failure: &Label,
    );

    fn helper_call(&self, emit: &mut Emit<'_>, function: crate::low_ir::Relocation);

    fn interpreter_call(&self, emit: &mut Emit<'_>, function: crate::low_ir::Relocation);

    fn raw_native_call(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]);

    fn slow_path_call(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError>;

    fn dispatch_current(&self, emit: &mut Emit<'_>, scratches: &[AllocatedOperand]) -> Result<(), CompileError>;

    fn dispatch_variable(
        &self,
        emit: &mut Emit<'_>,
        size: PhysicalRegister,
        scratches: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn dispatch_next(&self, emit: &mut Emit<'_>, size: u32, scratches: &[AllocatedOperand])
    -> Result<(), CompileError>;

    fn finalize_assertion(
        &self,
        emit: &mut Emit<'_>,
        operation: AssertionOperation,
        operands: &[AllocatedOperand],
        ok_label: Option<Label>,
    ) -> Result<(), CompileError>;

    fn finalize_scalar_compare_branch(
        &self,
        emit: &mut Emit<'_>,
        operation: Operation,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn finalize_bit_branch(
        &self,
        emit: &mut Emit<'_>,
        operation: Operation,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn finalize_value_representation_branch(
        &self,
        emit: &mut Emit<'_>,
        operation: Operation,
        operands: &[AllocatedOperand],
    );

    fn branch_memory(
        &self,
        emit: &mut Emit<'_>,
        operation: Operation,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn finalize_any_equal_branch(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand])
    -> Result<(), CompileError>;

    fn finalize_canonicalize_nan(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand])
    -> Result<(), CompileError>;

    fn move_instruction(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        width: IntegerWidth,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn finalize_effective_address(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn finalize_vm_load(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]);

    /// Materialize the current program counter as a value. Targets that do not
    /// keep it in a register derive it from their dispatch state here.
    fn finalize_program_counter_load(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]);

    fn finalize_operand_store(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError>;

    fn finalize_label_load(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError>;

    fn goto_handler(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError>;

    fn goto_bytecode_target(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError>;

    fn finalize_execution_context_store(
        &self,
        emit: &mut Emit<'_>,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn finalize_indexed_offset_store(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]);

    fn finalize_memory_increment(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand])
    -> Result<(), CompileError>;

    fn finalize_scalar_load(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        width: MemoryWidth,
        signed: bool,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn finalize_scalar_store(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        width: MemoryWidth,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn finalize_pair_load(
        &self,
        emit: &mut Emit<'_>,
        width: PairWidth,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn finalize_pair_store(
        &self,
        emit: &mut Emit<'_>,
        width: PairWidth,
        indexed: bool,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn finalize_or32_branch(
        &self,
        emit: &mut Emit<'_>,
        condition: SignCondition,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn add_subtract(
        &self,
        emit: &mut Emit<'_>,
        opcode: ArchitectureOpcode,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn finalize_binary_operation(
        &self,
        emit: &mut Emit<'_>,
        operation: BinaryOperation,
        width: IntegerWidth,
        operands: &[AllocatedOperand],
    ) -> Result<(), CompileError>;

    fn finalize_overflow_operation(
        &self,
        emit: &mut Emit<'_>,
        operation: OverflowOperation,
        operands: &[AllocatedOperand],
    );

    fn finalize_shift(
        &self,
        emit: &mut Emit<'_>,
        operation: ShiftOperation,
        width: IntegerWidth,
        operands: &[AllocatedOperand],
    );

    fn finalize_divide(&self, emit: &mut Emit<'_>, operands: &[AllocatedOperand]);
}

pub(crate) struct X86_64Backend;
pub(crate) struct Aarch64Backend;

pub(crate) fn backend_for(architecture: crate::Architecture) -> &'static dyn Backend {
    match architecture {
        crate::Architecture::X86_64 => &X86_64Backend,
        crate::Architecture::Aarch64 => &Aarch64Backend,
    }
}
