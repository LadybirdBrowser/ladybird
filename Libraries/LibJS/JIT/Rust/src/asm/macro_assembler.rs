/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The portable interface codegen targets, implemented by
//! `x86_64::MacroAssembler` and `aarch64::MacroAssembler`.

use super::Address;
use super::Architecture;
use super::AsmError;
use super::Condition;
use super::DoubleCondition;
use super::Fpr;
use super::FprSet;
use super::Gpr;
use super::GprSet;
use super::Label;
use super::NegativeZero;

/// The stack frame a JIT function sets up in its prologue.
///
/// After the prologue the stack pointer is 16-byte aligned and points at the
/// locals area `[sp, sp + locals_size)`. Above it are the saved registers,
/// the frame pointer / return address pair, and the caller's frame.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MachineFrame {
    /// Saved in the prologue, restored in the epilogue. Never includes the
    /// stack pointer, frame pointer or link register, which are always saved.
    pub saved_gprs: GprSet,
    pub saved_fprs: FprSet,
    /// Bytes of locals (spill slots), rounded up to keep the stack aligned.
    pub locals_size: u32,
    /// Distance from the stack pointer after the prologue to the stack
    /// pointer at the call into this function, where stack-passed
    /// arguments start.
    pub caller_stack_offset: u32,
}

impl MachineFrame {
    /// The address of a local at `offset` within the locals area.
    pub fn local(&self, stack_pointer: Gpr, offset: u32) -> Address {
        assert!(offset < self.locals_size, "local offset out of range");
        Address::new(stack_pointer, offset as i32)
    }
}

/// Operations codegen can use on every architecture.
///
/// Conventions:
/// - Three-operand forms: `dst` may alias any source.
/// - 32-bit integer operations zero-extend their result into 64 bits.
/// - Moves of immediates never change the flags.
/// - Reserved scratch registers (`SCRATCH_GPRS`, `SCRATCH_FPRS`) are clobbered
///   by any operation and must never be passed as operands.
/// - Shifts by a register amount use the amount modulo the operand width.
/// - On a taken overflow branch, `dst` holds an unspecified value; the source
///   operands are intact unless they alias `dst`.
pub trait PortableMacroAssembler: Sized {
    const ARCHITECTURE: Architecture;
    /// Integer argument registers, in order.
    const ARGUMENT_GPRS: &'static [Gpr];
    /// Floating point argument registers, in order.
    const ARGUMENT_FPRS: &'static [Fpr];
    /// The two integer return registers (`JitResult { value, status }`).
    const RETURN_GPRS: [Gpr; 2];
    const CALLEE_SAVED_GPRS: GprSet;
    const CALLEE_SAVED_FPRS: FprSet;
    const CALLER_SAVED_GPRS: GprSet;
    const CALLER_SAVED_FPRS: FprSet;
    /// Reserved for the macro assembler; never handed to the allocator.
    const SCRATCH_GPRS: GprSet;
    const SCRATCH_FPRS: FprSet;
    /// What the register allocator may use.
    const ALLOCATABLE_GPRS: GprSet;
    const ALLOCATABLE_FPRS: FprSet;
    const STACK_POINTER: Gpr;
    const FRAME_POINTER: Gpr;

    fn new() -> Self;
    fn offset(&self) -> usize;
    fn new_label(&mut self) -> Label;
    fn bind(&mut self, label: Label);
    fn label_offset(&self, label: Label) -> Option<usize>;
    /// Emits no-ops that a jump can replace once the code is installed (see
    /// `jump_patch()`), and returns their offset.
    fn patchable_nop(&mut self) -> usize;
    /// The bytes of a jump from offset `from` to offset `to` of the same
    /// code, which replace the no-ops `patchable_nop()` emitted at `from`.
    fn jump_patch(from: usize, to: usize) -> Vec<u8>;
    /// Returns the code. Fails if a referenced label is unbound or a branch
    /// does not reach.
    fn finish(self) -> Result<Vec<u8>, AsmError>
    where
        Self: Sized,
    {
        Ok(self.finish_with_data_offset()?.0)
    }
    /// Like `finish()`, with the offset where the instructions end and the
    /// data they load (a constant pool) begins.
    fn finish_with_data_offset(self) -> Result<(Vec<u8>, usize), AsmError>;

    /// A guaranteed trap for code that must never run.
    fn unreachable(&mut self);

    // Moves.
    fn move64(&mut self, dst: Gpr, src: Gpr);
    /// Copies the low 32 bits and zero-extends.
    fn move32(&mut self, dst: Gpr, src: Gpr);
    fn move_imm64(&mut self, dst: Gpr, imm: u64);
    /// Zero-extends.
    fn move_imm32(&mut self, dst: Gpr, imm: u32);
    fn sign_extend32_to_64(&mut self, dst: Gpr, src: Gpr);

    // Memory.
    fn load8(&mut self, dst: Gpr, address: &Address);
    fn load8_sign_extend(&mut self, dst: Gpr, address: &Address);
    fn load16(&mut self, dst: Gpr, address: &Address);
    fn load16_sign_extend(&mut self, dst: Gpr, address: &Address);
    fn load32(&mut self, dst: Gpr, address: &Address);
    fn load32_sign_extend(&mut self, dst: Gpr, address: &Address);
    fn load64(&mut self, dst: Gpr, address: &Address);
    fn store8(&mut self, address: &Address, src: Gpr);
    fn store16(&mut self, address: &Address, src: Gpr);
    fn store32(&mut self, address: &Address, src: Gpr);
    fn store64(&mut self, address: &Address, src: Gpr);
    fn store_imm32(&mut self, address: &Address, imm: u32);
    fn store_imm64(&mut self, address: &Address, imm: u64);
    /// Loads the 16 bytes of `imm` (little-endian) into all of `dst`, from
    /// the constant pool.
    fn load_imm128(&mut self, dst: Fpr, imm: u128);
    /// Stores all 16 bytes of `src` at `address`.
    fn store128(&mut self, address: &Address, src: Fpr);
    /// Adds `imm` to the 32-bit value at `address`.
    fn add32_to_memory_imm(&mut self, address: &Address, imm: i32);
    /// Adds `imm` to the 64-bit value at `address`.
    fn add64_to_memory_imm(&mut self, address: &Address, imm: i32);
    fn load_effective_address(&mut self, dst: Gpr, address: &Address);
    fn load_double(&mut self, dst: Fpr, address: &Address);
    fn store_double(&mut self, address: &Address, src: Fpr);
    /// Loads a float and widens it to a double.
    fn load_float_as_double(&mut self, dst: Fpr, address: &Address);
    /// Rounds a double to a float and stores it.
    fn store_double_as_float(&mut self, address: &Address, src: Fpr);

    // Integer arithmetic and logic.
    fn add32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn add32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: i32);
    fn add64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn add64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: i64);
    fn sub32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn sub32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: i32);
    fn sub64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn sub64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: i64);
    fn and32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn and32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u32);
    fn and64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn and64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u64);
    fn or32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn or32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u32);
    fn or64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn or64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u64);
    fn xor32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn xor32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u32);
    fn xor64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn xor64_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u64);
    fn mul32(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn mul32_imm(&mut self, dst: Gpr, lhs: Gpr, imm: u32);
    fn mul64(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn neg32(&mut self, dst: Gpr, src: Gpr);
    fn neg64(&mut self, dst: Gpr, src: Gpr);
    fn not32(&mut self, dst: Gpr, src: Gpr);
    fn not64(&mut self, dst: Gpr, src: Gpr);
    fn shl32(&mut self, dst: Gpr, src: Gpr, amount: Gpr);
    fn shl32_imm(&mut self, dst: Gpr, src: Gpr, amount: u8);
    fn shl64(&mut self, dst: Gpr, src: Gpr, amount: Gpr);
    fn shl64_imm(&mut self, dst: Gpr, src: Gpr, amount: u8);
    /// Logical (unsigned) right shift.
    fn shr32(&mut self, dst: Gpr, src: Gpr, amount: Gpr);
    fn shr32_imm(&mut self, dst: Gpr, src: Gpr, amount: u8);
    fn shr64(&mut self, dst: Gpr, src: Gpr, amount: Gpr);
    fn shr64_imm(&mut self, dst: Gpr, src: Gpr, amount: u8);
    /// Arithmetic (signed) right shift.
    fn sar32(&mut self, dst: Gpr, src: Gpr, amount: Gpr);
    fn sar32_imm(&mut self, dst: Gpr, src: Gpr, amount: u8);
    fn sar64(&mut self, dst: Gpr, src: Gpr, amount: Gpr);
    fn sar64_imm(&mut self, dst: Gpr, src: Gpr, amount: u8);

    // Arithmetic that branches to `overflow` on signed overflow.
    fn branch_add32_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label);
    fn branch_add32_imm_overflow(&mut self, dst: Gpr, lhs: Gpr, imm: i32, overflow: Label);
    fn branch_sub32_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label);
    fn branch_sub32_imm_overflow(&mut self, dst: Gpr, lhs: Gpr, imm: i32, overflow: Label);
    fn branch_mul32_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label);
    fn branch_neg32_overflow(&mut self, dst: Gpr, src: Gpr, overflow: Label);
    fn branch_add64_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label);
    fn branch_sub64_overflow(&mut self, dst: Gpr, lhs: Gpr, rhs: Gpr, overflow: Label);

    // Compares, tests and branches.
    fn branch32(&mut self, condition: Condition, lhs: Gpr, rhs: Gpr, target: Label);
    fn branch32_imm(&mut self, condition: Condition, lhs: Gpr, imm: i32, target: Label);
    fn branch64(&mut self, condition: Condition, lhs: Gpr, rhs: Gpr, target: Label);
    /// Branches to `target` if the stack pointer is below the 64-bit limit
    /// at `limit` (unsigned).
    fn branch_if_stack_pointer_below(&mut self, limit: &Address, target: Label);
    fn branch64_imm(&mut self, condition: Condition, lhs: Gpr, imm: i64, target: Label);
    /// Compares the 32-bit value at `address` with `imm`.
    fn branch32_memory_imm(&mut self, condition: Condition, address: &Address, imm: i32, target: Label);
    /// Compares the 16-bit value at `address`, zero-extended, with `imm`.
    fn branch16_memory_imm(&mut self, condition: Condition, address: &Address, imm: u16, target: Label);
    /// Compares the 64-bit value at `address` with `rhs`.
    fn branch64_memory(&mut self, condition: Condition, address: &Address, rhs: Gpr, target: Label);
    /// Compares the 64-bit value at `address` with `imm`.
    fn branch64_memory_imm(&mut self, condition: Condition, address: &Address, imm: i64, target: Label);
    /// Branches on `value & mask`; `condition` is one of Zero, NonZero,
    /// Negative or PositiveOrZero.
    fn branch_test32(&mut self, condition: Condition, value: Gpr, mask: u32, target: Label);
    fn branch_test64(&mut self, condition: Condition, value: Gpr, mask: u64, target: Label);
    /// Sets `dst` to 1 if `lhs condition rhs`, else 0.
    fn compare32_set(&mut self, condition: Condition, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn compare32_imm_set(&mut self, condition: Condition, dst: Gpr, lhs: Gpr, imm: i32);
    fn compare64_set(&mut self, condition: Condition, dst: Gpr, lhs: Gpr, rhs: Gpr);
    fn compare64_imm_set(&mut self, condition: Condition, dst: Gpr, lhs: Gpr, imm: i64);
    /// `dst = (lhs condition rhs) ? if_true : if_false`, comparing 64 bits.
    fn select64(&mut self, condition: Condition, lhs: Gpr, rhs: Gpr, dst: Gpr, if_true: Gpr, if_false: Gpr);

    // Control flow.
    fn jump(&mut self, target: Label);
    /// Tail jump to an absolute address (through a scratch register).
    fn jump_absolute(&mut self, address: u64);
    fn call(&mut self, target: Label);
    fn call_register(&mut self, target: Gpr);
    /// Calls an absolute address (through a scratch register).
    fn call_absolute(&mut self, address: u64);
    fn ret(&mut self);

    // Doubles.
    fn move_double(&mut self, dst: Fpr, src: Fpr);
    fn move_double_imm(&mut self, dst: Fpr, value: f64);
    /// Bit-casts a GPR into a double register.
    fn move_gpr_to_double(&mut self, dst: Fpr, src: Gpr);
    /// Bit-casts a double register into a GPR.
    fn move_double_to_gpr(&mut self, dst: Gpr, src: Fpr);
    fn add_double(&mut self, dst: Fpr, lhs: Fpr, rhs: Fpr);
    fn sub_double(&mut self, dst: Fpr, lhs: Fpr, rhs: Fpr);
    fn mul_double(&mut self, dst: Fpr, lhs: Fpr, rhs: Fpr);
    fn div_double(&mut self, dst: Fpr, lhs: Fpr, rhs: Fpr);
    fn sqrt_double(&mut self, dst: Fpr, src: Fpr);
    /// The largest integral double not above `src`.
    fn floor_double(&mut self, dst: Fpr, src: Fpr);
    /// The smallest integral double not below `src`.
    fn ceil_double(&mut self, dst: Fpr, src: Fpr);
    fn neg_double(&mut self, dst: Fpr, src: Fpr);
    fn abs_double(&mut self, dst: Fpr, src: Fpr);
    fn convert_int32_to_double(&mut self, dst: Fpr, src: Gpr);
    fn convert_int64_to_double(&mut self, dst: Fpr, src: Gpr);
    /// Converts `src` to an int32 if it is exactly representable as one, and
    /// branches to `fail` otherwise (fractions, NaN, out of range, and -0 when
    /// `negative_zero` is `Fail`).
    fn branch_convert_double_to_int32(&mut self, dst: Gpr, src: Fpr, fail: Label, negative_zero: NegativeZero);
    /// Truncates toward zero. The result for NaN and out of range inputs is
    /// unspecified.
    fn truncate_double_to_int64(&mut self, dst: Gpr, src: Fpr);
    fn branch_double(&mut self, condition: DoubleCondition, lhs: Fpr, rhs: Fpr, target: Label);
    fn compare_double_set(&mut self, condition: DoubleCondition, dst: Gpr, lhs: Fpr, rhs: Fpr);

    // Frames.
    /// Computes the frame for saving `saved_gprs`/`saved_fprs` (the stack
    /// pointer, frame pointer and link register are filtered out) with
    /// `locals_size` bytes of locals.
    fn frame(saved_gprs: GprSet, saved_fprs: FprSet, locals_size: u32) -> MachineFrame;
    /// Sets up `frame`: links a frame pointer, saves registers, reserves locals.
    fn emit_prologue(&mut self, frame: &MachineFrame);
    /// Tears down `frame` and restores saved registers. Does not return.
    fn emit_epilogue(&mut self, frame: &MachineFrame);
}
