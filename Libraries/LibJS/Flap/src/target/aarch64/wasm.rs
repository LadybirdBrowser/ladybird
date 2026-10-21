/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AArch64 expansions of the numeric, vector, and atomic operations that only the WebAssembly profile uses.
//!
//! The vector operations are specified by the x86-64 instructions they map to (see `VectorOperation`), so their
//! expansions here reproduce those instructions' results exactly, including how they treat NaNs, out-of-range
//! conversions, and shift counts. v16 to v19 are scratch vector registers that no handler allocates; x9 and x10 are
//! the usual scratch registers.

use super::{Opcode, Slot};
use crate::intrinsic::{
    AtomicOperation, AtomicOperationKind, NumericOperation, OrderedFloatComparison, VectorOperation,
};
use crate::low_ir::Label;
use crate::target::finalize_support::{AllocatedOperands, Emit, machine_address};
use crate::target::ir::{AllocatedOperand, MachineCondition, MachineInstruction, MachineOpcode, MachineOperand};
use crate::target::machine_verify::emit_machine_instructions as emit;
use crate::target::registers::{
    PhysicalRegister,
    aarch64::{D16, D17, D18, D19, X9, X10},
};
use crate::{CompileError, ObjectFormat};

/// One operand of a generic instruction.
#[derive(Clone, Copy)]
enum Part {
    Register(&'static str, &'static str, PhysicalRegister),
    Immediate(i64),
    HexImmediate(i64),
    Literal(&'static str),
}

fn x(register: PhysicalRegister) -> Part {
    Part::Register("x", "", register)
}

fn w(register: PhysicalRegister) -> Part {
    Part::Register("w", "", register)
}

fn d(register: PhysicalRegister) -> Part {
    Part::Register("d", "", register)
}

fn s(register: PhysicalRegister) -> Part {
    Part::Register("s", "", register)
}

fn b(register: PhysicalRegister) -> Part {
    Part::Register("b", "", register)
}

/// A vector register with an arrangement or lane suffix, like `.4s` or `.d[1]`.
fn v(register: PhysicalRegister, suffix: &'static str) -> Part {
    Part::Register("v", suffix, register)
}

fn table(register: PhysicalRegister) -> Part {
    Part::Register("{v", ".16b}", register)
}

fn instruction(emit: &mut Emit<'_>, mnemonic: &'static str, parts: &[Part]) {
    assert!(
        parts.len() <= 4,
        "a generic AArch64 instruction takes at most four operands"
    );
    let mut slots = [None; 4];
    let mut operands = Vec::with_capacity(parts.len());
    for (slot, part) in slots.iter_mut().zip(parts) {
        *slot = Some(match *part {
            Part::Register(prefix, suffix, register) => {
                operands.push(MachineOperand::PhysicalRegister(register));
                Slot::Register(prefix, suffix)
            }
            Part::Immediate(value) => {
                operands.push(MachineOperand::Immediate(value));
                Slot::Immediate
            }
            Part::HexImmediate(value) => {
                operands.push(MachineOperand::Immediate(value));
                Slot::HexImmediate
            }
            Part::Literal(text) => Slot::Literal(text),
        });
    }
    emit.output.push(MachineInstruction {
        opcode: MachineOpcode::Aarch64(Opcode::Generic(mnemonic, slots)),
        operands,
    });
}

/// Materialize a 64-bit constant with movz and movk.
fn move_constant(emit: &mut Emit<'_>, destination: PhysicalRegister, value: u64) {
    const SHIFTS: [&str; 4] = ["lsl #0", "lsl #16", "lsl #32", "lsl #48"];
    let mut first = true;
    for (index, shift) in SHIFTS.into_iter().enumerate() {
        let chunk = (value >> (index * 16)) & 0xffff;
        if chunk == 0 && (value != 0 || index != 0) {
            continue;
        }
        let mnemonic = if first { "movz" } else { "movk" };
        instruction(
            emit,
            mnemonic,
            &[x(destination), Part::HexImmediate(chunk as i64), Part::Literal(shift)],
        );
        first = false;
    }
}

/// Fill every 32-bit lane of `destination` with `value`.
fn splat32(emit: &mut Emit<'_>, destination: PhysicalRegister, value: u32) {
    move_constant(emit, X9, u64::from(value));
    instruction(emit, "dup", &[v(destination, ".4s"), w(X9)]);
}

/// Fill both 64-bit lanes of `destination` with `value`.
fn splat64(emit: &mut Emit<'_>, destination: PhysicalRegister, value: u64) {
    move_constant(emit, X9, value);
    instruction(emit, "dup", &[v(destination, ".2d"), x(X9)]);
}

fn move_vector(emit: &mut Emit<'_>, destination: PhysicalRegister, source: PhysicalRegister) {
    if destination != source {
        instruction(emit, "mov", &[v(destination, ".16b"), v(source, ".16b")]);
    }
}

/// 32-bit addition, subtraction, and multiplication, which leave the upper half of the destination clear.
pub(crate) fn arithmetic32(
    emit: &mut Emit<'_>,
    operation: crate::intrinsic::BinaryOperation,
    operands: &[AllocatedOperand],
) {
    use crate::intrinsic::BinaryOperation;
    let destination = operands.physical_register(0);
    let lhs = operands.physical_register(1);
    let mnemonic = match operation {
        BinaryOperation::Add => "add",
        BinaryOperation::Subtract => "sub",
        _ => "mul",
    };
    let rhs = match *operands.operand(2) {
        AllocatedOperand::Immediate(value) => {
            let value = value as u32;
            if operation != BinaryOperation::Multiply && value <= 4095 {
                instruction(
                    emit,
                    mnemonic,
                    &[w(destination), w(lhs), Part::Immediate(i64::from(value))],
                );
                return;
            }
            if operation != BinaryOperation::Multiply && value.wrapping_neg() <= 4095 {
                let inverse = if operation == BinaryOperation::Add {
                    "sub"
                } else {
                    "add"
                };
                instruction(
                    emit,
                    inverse,
                    &[w(destination), w(lhs), Part::Immediate(i64::from(value.wrapping_neg()))],
                );
                return;
            }
            move_constant(emit, X9, u64::from(value));
            X9
        }
        _ => operands.physical_register(2),
    };
    instruction(emit, mnemonic, &[w(destination), w(lhs), w(rhs)]);
}

pub(crate) fn branch_ordered_float(
    emit: &mut Emit<'_>,
    lhs: PhysicalRegister,
    rhs: PhysicalRegister,
    comparison: OrderedFloatComparison,
    target: &Label,
) {
    // fcmp sets N, Z, C, and V so that an unordered comparison satisfies none of mi, ls, gt, ge, and eq, and does
    // satisfy ne.
    let condition = match comparison {
        OrderedFloatComparison::Equal => MachineCondition::Equal,
        OrderedFloatComparison::NotEqual => MachineCondition::NotEqual,
        OrderedFloatComparison::Less => MachineCondition::Negative,
        OrderedFloatComparison::LessOrEqual => MachineCondition::UnsignedLessOrEqual,
        OrderedFloatComparison::Greater => MachineCondition::SignedGreater,
        OrderedFloatComparison::GreaterOrEqual => MachineCondition::SignedGreaterOrEqual,
    };
    emit!(emit.output, Aarch64;
        Opcode::CompareDouble => [register lhs, register rhs];
        Opcode::BranchCondition(condition) => [label target.clone()];
    );
}

fn secondary_table_page(prefix: u8, format: ObjectFormat) -> Option<(&'static str, &'static str)> {
    let mach_o = format == ObjectFormat::MachO;
    Some(match (prefix, mach_o) {
        (0xfb, false) => ("asm_dispatch_table_fb", ":lo12:asm_dispatch_table_fb"),
        (0xfc, false) => ("asm_dispatch_table_fc", ":lo12:asm_dispatch_table_fc"),
        (0xfd, false) => ("asm_dispatch_table_fd", ":lo12:asm_dispatch_table_fd"),
        (0xfe, false) => ("asm_dispatch_table_fe", ":lo12:asm_dispatch_table_fe"),
        (0xff, false) => ("asm_dispatch_table_ff", ":lo12:asm_dispatch_table_ff"),
        (0xfb, true) => ("asm_dispatch_table_fb@PAGE", "asm_dispatch_table_fb@PAGEOFF"),
        (0xfc, true) => ("asm_dispatch_table_fc@PAGE", "asm_dispatch_table_fc@PAGEOFF"),
        (0xfd, true) => ("asm_dispatch_table_fd@PAGE", "asm_dispatch_table_fd@PAGEOFF"),
        (0xfe, true) => ("asm_dispatch_table_fe@PAGE", "asm_dispatch_table_fe@PAGEOFF"),
        (0xff, true) => ("asm_dispatch_table_ff@PAGE", "asm_dispatch_table_ff@PAGEOFF"),
        _ => return None,
    })
}

pub(crate) fn dispatch_secondary(emit: &mut Emit<'_>, operands: &[AllocatedOperand]) -> Result<(), CompileError> {
    let prefix = operands.immediate(0);
    let index = operands.physical_register(1);
    let Some((page, offset)) = u8::try_from(prefix)
        .ok()
        .and_then(|prefix| secondary_table_page(prefix, emit.object_format))
    else {
        return Err(crate::target::finalize_support::finalization_error(
            emit.handler,
            "secondary dispatch tables only exist for the 0xfb to 0xff prefixes",
        ));
    };
    instruction(emit, "adrp", &[x(X9), Part::Literal(page)]);
    instruction(emit, "add", &[x(X9), x(X9), Part::Literal(offset)]);
    instruction(
        emit,
        "ldr",
        &[x(X10), Part::Register("[x", "", X9), x(index), Part::Literal("lsl #3]")],
    );
    instruction(emit, "br", &[x(X10)]);
    Ok(())
}

pub(crate) fn finalize_numeric(
    emit: &mut Emit<'_>,
    operation: NumericOperation,
    operands: &[AllocatedOperand],
) -> Result<(), CompileError> {
    use NumericOperation::*;
    let narrow = matches!(
        operation,
        DivideSigned32
            | DivideUnsigned32
            | RemainderSigned32
            | RemainderUnsigned32
            | CountLeadingZeros32
            | CountTrailingZeros32
            | PopulationCount32
            | RotateLeft32
            | RotateRight32
    );
    let gpr = if narrow { w } else { x };
    match operation {
        DivideSigned32 | DivideUnsigned32 | DivideSigned64 | DivideUnsigned64 => {
            let [destination, dividend, divisor] = operands.physical_registers();
            let mnemonic = if matches!(operation, DivideSigned32 | DivideSigned64) {
                "sdiv"
            } else {
                "udiv"
            };
            instruction(emit, mnemonic, &[gpr(destination), gpr(dividend), gpr(divisor)]);
        }
        RemainderSigned32 | RemainderUnsigned32 | RemainderSigned64 | RemainderUnsigned64 => {
            let [destination, dividend, divisor] = operands.physical_registers();
            let mnemonic = if matches!(operation, RemainderSigned32 | RemainderSigned64) {
                "sdiv"
            } else {
                "udiv"
            };
            instruction(emit, mnemonic, &[gpr(X9), gpr(dividend), gpr(divisor)]);
            instruction(emit, "msub", &[gpr(destination), gpr(X9), gpr(divisor), gpr(dividend)]);
        }
        CountLeadingZeros32 | CountLeadingZeros64 => {
            let [destination, source] = operands.physical_registers();
            instruction(emit, "clz", &[gpr(destination), gpr(source)]);
        }
        CountTrailingZeros32 | CountTrailingZeros64 => {
            let [destination, source] = operands.physical_registers();
            instruction(emit, "rbit", &[gpr(X9), gpr(source)]);
            instruction(emit, "clz", &[gpr(destination), gpr(X9)]);
        }
        PopulationCount32 | PopulationCount64 => {
            let [destination, source] = operands.physical_registers();
            if narrow {
                instruction(emit, "fmov", &[s(D16), w(source)]);
            } else {
                instruction(emit, "fmov", &[d(D16), x(source)]);
            }
            instruction(emit, "cnt", &[v(D16, ".8b"), v(D16, ".8b")]);
            instruction(emit, "addv", &[b(D16), v(D16, ".8b")]);
            instruction(emit, "fmov", &[w(destination), s(D16)]);
        }
        RotateLeft32 | RotateLeft64 | RotateRight32 | RotateRight64 => {
            let [destination, count] = operands.physical_registers();
            if matches!(operation, RotateLeft32 | RotateLeft64) {
                instruction(emit, "neg", &[gpr(X9), gpr(count)]);
                instruction(emit, "ror", &[gpr(destination), gpr(destination), gpr(X9)]);
            } else {
                instruction(emit, "ror", &[gpr(destination), gpr(destination), gpr(count)]);
            }
        }
        Float64FromBits => {
            let [destination, source] = operands.physical_registers();
            instruction(emit, "fmov", &[d(destination), x(source)]);
        }
        Float32FromBits => {
            let [destination, source] = operands.physical_registers();
            instruction(emit, "fmov", &[s(destination), w(source)]);
        }
        Float64ToBits => {
            let [destination, source] = operands.physical_registers();
            instruction(emit, "fmov", &[x(destination), d(source)]);
        }
        Float32ToBits => {
            let [destination, source] = operands.physical_registers();
            instruction(emit, "fmov", &[w(destination), s(source)]);
        }
        TruncateFloat64ToSigned64 => {
            // Only in-range values reach this, so saturation never differs from x86-64's result.
            let [destination, source] = operands.physical_registers();
            instruction(emit, "fcvtzs", &[x(destination), d(source)]);
        }
        ConvertSigned64ToFloat32 => {
            let [destination, source] = operands.physical_registers();
            instruction(emit, "scvtf", &[s(destination), x(source)]);
        }
        ConvertSigned64ToFloat64 => {
            let [destination, source] = operands.physical_registers();
            instruction(emit, "scvtf", &[d(destination), x(source)]);
        }
        RoundFloat64ToZero | RoundFloat64ToNearest | RoundFloat64Up | RoundFloat64Down | SquareRootFloat64 => {
            let [destination, source] = operands.physical_registers();
            let mnemonic = match operation {
                RoundFloat64ToZero => "frintz",
                RoundFloat64ToNearest => "frintn",
                RoundFloat64Up => "frintp",
                RoundFloat64Down => "frintm",
                _ => "fsqrt",
            };
            instruction(emit, mnemonic, &[d(destination), d(source)]);
        }
        SquareRootFloat32 => {
            let [destination, source] = operands.physical_registers();
            instruction(emit, "fsqrt", &[s(destination), s(source)]);
        }
        PromoteFloat32 => {
            let [destination, source] = operands.physical_registers();
            instruction(emit, "fcvt", &[d(destination), s(source)]);
        }
        DemoteFloat64 => {
            let [destination, source] = operands.physical_registers();
            instruction(emit, "fcvt", &[s(destination), d(source)]);
        }
        AddFloat32 | SubtractFloat32 | MultiplyFloat32 | DivideFloat32 => {
            let [destination, rhs] = operands.physical_registers();
            let mnemonic = match operation {
                AddFloat32 => "fadd",
                SubtractFloat32 => "fsub",
                MultiplyFloat32 => "fmul",
                _ => "fdiv",
            };
            instruction(emit, mnemonic, &[s(destination), s(destination), s(rhs)]);
        }
    }
    Ok(())
}

pub(crate) fn finalize_atomic(
    emit: &mut Emit<'_>,
    operation: AtomicOperation,
    operands: &[AllocatedOperand],
) -> Result<(), CompileError> {
    use crate::intrinsic::IntegerWidth;
    let width = operation.width();
    let (suffix, gpr): (&'static str, fn(PhysicalRegister) -> Part) = match width {
        IntegerWidth::U8 => ("b", w),
        IntegerWidth::U16 => ("h", w),
        IntegerWidth::U32 => ("", w),
        IntegerWidth::U64 => ("", x),
    };
    if operation.kind() == AtomicOperationKind::Fence {
        instruction(emit, "dmb", &[Part::Literal("ish")]);
        return Ok(());
    }
    let destination = operands.physical_register(0);
    let address = machine_address(operands.operand(1));
    // The atomic instructions only take a base register.
    let base = if address.index.is_none() && address.displacement.unwrap_or(0) == 0 {
        address.base
    } else {
        let mut base = address.base;
        if let Some(index) = address.index {
            let shift = address.scale.unwrap_or(1).trailing_zeros();
            let shift = match shift {
                0 => "lsl #0",
                1 => "lsl #1",
                2 => "lsl #2",
                _ => "lsl #3",
            };
            instruction(emit, "add", &[x(X9), x(base), x(index), Part::Literal(shift)]);
            base = X9;
        }
        let displacement = address.displacement.unwrap_or(0);
        if displacement != 0 {
            if (0..=4095).contains(&displacement) {
                instruction(emit, "add", &[x(X9), x(base), Part::Immediate(displacement)]);
            } else if (-4095..0).contains(&displacement) {
                instruction(emit, "sub", &[x(X9), x(base), Part::Immediate(-displacement)]);
            } else {
                move_constant(emit, X10, displacement as u64);
                instruction(emit, "add", &[x(X9), x(base), x(X10)]);
            }
        }
        X9
    };
    let memory = Part::Register("[x", "]", base);
    // The acquire-release forms make every atomic access sequentially consistent with the others.
    match operation.kind() {
        AtomicOperationKind::Load => {
            let mnemonic = match width {
                IntegerWidth::U8 => "ldarb",
                IntegerWidth::U16 => "ldarh",
                _ => "ldar",
            };
            instruction(emit, mnemonic, &[gpr(destination), memory]);
        }
        AtomicOperationKind::CompareExchange => {
            let replacement = operands.physical_register(2);
            let mnemonic = match suffix {
                "b" => "casalb",
                "h" => "casalh",
                _ => "casal",
            };
            instruction(emit, mnemonic, &[gpr(destination), gpr(replacement), memory]);
        }
        AtomicOperationKind::Exchange | AtomicOperationKind::FetchAdd => {
            let exchange = operation.kind() == AtomicOperationKind::Exchange;
            let mnemonic = match (exchange, suffix) {
                (true, "b") => "swpalb",
                (true, "h") => "swpalh",
                (true, _) => "swpal",
                (false, "b") => "ldaddalb",
                (false, "h") => "ldaddalh",
                (false, _) => "ldaddal",
            };
            instruction(emit, mnemonic, &[gpr(destination), gpr(destination), memory]);
        }
        AtomicOperationKind::Fence => unreachable!(),
    }
    Ok(())
}

pub(crate) fn finalize_vector(
    emit: &mut Emit<'_>,
    operation: VectorOperation,
    operands: &[AllocatedOperand],
) -> Result<(), CompileError> {
    use crate::intrinsic::VectorOperationKind;
    match operation.kind() {
        VectorOperationKind::FromU64Pair => {
            let [destination, low, high] = operands.physical_registers();
            instruction(emit, "fmov", &[d(destination), x(low)]);
            instruction(emit, "mov", &[v(destination, ".d[1]"), x(high)]);
        }
        VectorOperationKind::Binary => {
            let [destination, rhs] = operands.physical_registers();
            vector_binary(emit, operation, destination, rhs);
        }
        VectorOperationKind::Unary => {
            let [destination, source] = operands.physical_registers();
            vector_unary(emit, operation, destination, source);
        }
        VectorOperationKind::ToGeneralPurpose | VectorOperationKind::ToGeneralPurpose32 => {
            let [destination, source] = operands.physical_registers();
            vector_to_general_purpose(emit, operation, destination, source);
        }
    }
    Ok(())
}

/// The mnemonic and arrangement of the binary operations that are one instruction, `destination = op(destination,
/// rhs)`.
fn single_binary(operation: VectorOperation) -> Option<(&'static str, &'static str)> {
    use VectorOperation::*;
    Some(match operation {
        And => ("and", ".16b"),
        Or => ("orr", ".16b"),
        Xor => ("eor", ".16b"),
        AddI8 => ("add", ".16b"),
        SubtractI8 => ("sub", ".16b"),
        EqualI8 => ("cmeq", ".16b"),
        GreaterSignedI8 => ("cmgt", ".16b"),
        AddI16 => ("add", ".8h"),
        SubtractI16 => ("sub", ".8h"),
        EqualI16 => ("cmeq", ".8h"),
        GreaterSignedI16 => ("cmgt", ".8h"),
        AddI32 => ("add", ".4s"),
        SubtractI32 => ("sub", ".4s"),
        EqualI32 => ("cmeq", ".4s"),
        GreaterSignedI32 => ("cmgt", ".4s"),
        AddI64 => ("add", ".2d"),
        SubtractI64 => ("sub", ".2d"),
        EqualI64 => ("cmeq", ".2d"),
        GreaterSignedI64 => ("cmgt", ".2d"),
        AddSaturateSignedI8 => ("sqadd", ".16b"),
        AddSaturateUnsignedI8 => ("uqadd", ".16b"),
        SubtractSaturateSignedI8 => ("sqsub", ".16b"),
        SubtractSaturateUnsignedI8 => ("uqsub", ".16b"),
        AverageUnsignedI8 => ("urhadd", ".16b"),
        AddSaturateSignedI16 => ("sqadd", ".8h"),
        AddSaturateUnsignedI16 => ("uqadd", ".8h"),
        SubtractSaturateSignedI16 => ("sqsub", ".8h"),
        SubtractSaturateUnsignedI16 => ("uqsub", ".8h"),
        AverageUnsignedI16 => ("urhadd", ".8h"),
        MultiplyI16 => ("mul", ".8h"),
        MultiplyI32 => ("mul", ".4s"),
        MinimumSignedI8 => ("smin", ".16b"),
        MinimumUnsignedI8 => ("umin", ".16b"),
        MaximumSignedI8 => ("smax", ".16b"),
        MaximumUnsignedI8 => ("umax", ".16b"),
        MinimumSignedI16 => ("smin", ".8h"),
        MinimumUnsignedI16 => ("umin", ".8h"),
        MaximumSignedI16 => ("smax", ".8h"),
        MaximumUnsignedI16 => ("umax", ".8h"),
        MinimumSignedI32 => ("smin", ".4s"),
        MinimumUnsignedI32 => ("umin", ".4s"),
        MaximumSignedI32 => ("smax", ".4s"),
        MaximumUnsignedI32 => ("umax", ".4s"),
        InterleaveLowI8 => ("zip1", ".16b"),
        InterleaveHighI8 => ("zip2", ".16b"),
        InterleaveLowI16 => ("zip1", ".8h"),
        InterleaveHighI16 => ("zip2", ".8h"),
        InterleaveLowI32 => ("zip1", ".4s"),
        InterleaveHighI32 => ("zip2", ".4s"),
        InterleaveLowI64 => ("zip1", ".2d"),
        InterleaveHighI64 => ("zip2", ".2d"),
        AddF32 => ("fadd", ".4s"),
        SubtractF32 => ("fsub", ".4s"),
        MultiplyF32 => ("fmul", ".4s"),
        DivideF32 => ("fdiv", ".4s"),
        EqualF32 => ("fcmeq", ".4s"),
        AddF64 => ("fadd", ".2d"),
        SubtractF64 => ("fsub", ".2d"),
        MultiplyF64 => ("fmul", ".2d"),
        DivideF64 => ("fdiv", ".2d"),
        EqualF64 => ("fcmeq", ".2d"),
        _ => return None,
    })
}

fn vector_binary(
    emit: &mut Emit<'_>,
    operation: VectorOperation,
    destination: PhysicalRegister,
    rhs: PhysicalRegister,
) {
    use VectorOperation::*;
    if let Some((mnemonic, arrangement)) = single_binary(operation) {
        instruction(
            emit,
            mnemonic,
            &[
                v(destination, arrangement),
                v(destination, arrangement),
                v(rhs, arrangement),
            ],
        );
        return;
    }
    let [lhs, result] = [destination, destination];
    match operation {
        // pandn complements its left operand.
        AndNot => instruction(emit, "bic", &[v(result, ".16b"), v(rhs, ".16b"), v(lhs, ".16b")]),
        // pmuludq multiplies the low halves of the 64-bit lanes.
        MultiplyUnsignedLowI32 => {
            instruction(emit, "xtn", &[v(D16, ".2s"), v(lhs, ".2d")]);
            instruction(emit, "xtn", &[v(D17, ".2s"), v(rhs, ".2d")]);
            instruction(emit, "umull", &[v(result, ".2d"), v(D16, ".2s"), v(D17, ".2s")]);
        }
        MultiplyHighSignedI16 | MultiplyHighUnsignedI16 => {
            let (low, high) = if operation == MultiplyHighSignedI16 {
                ("smull", "smull2")
            } else {
                ("umull", "umull2")
            };
            instruction(emit, low, &[v(D16, ".4s"), v(lhs, ".4h"), v(rhs, ".4h")]);
            instruction(emit, high, &[v(D17, ".4s"), v(lhs, ".8h"), v(rhs, ".8h")]);
            instruction(emit, "uzp2", &[v(result, ".8h"), v(D16, ".8h"), v(D17, ".8h")]);
        }
        // pmulhrsw is (a * b + 0x4000) >> 15, truncated to 16 bits, so -0x8000 * -0x8000 gives -0x8000.
        MultiplyHighRoundScaleI16 => {
            instruction(emit, "smull", &[v(D16, ".4s"), v(lhs, ".4h"), v(rhs, ".4h")]);
            instruction(emit, "smull2", &[v(D17, ".4s"), v(lhs, ".8h"), v(rhs, ".8h")]);
            instruction(emit, "rshrn", &[v(D16, ".4h"), v(D16, ".4s"), Part::Immediate(15)]);
            instruction(emit, "rshrn2", &[v(D16, ".8h"), v(D17, ".4s"), Part::Immediate(15)]);
            move_vector(emit, result, D16);
        }
        // pmaddwd adds adjacent products of signed 16-bit lanes into 32-bit lanes, wrapping.
        MultiplyAddI16 => {
            instruction(emit, "smull", &[v(D16, ".4s"), v(lhs, ".4h"), v(rhs, ".4h")]);
            instruction(emit, "smull2", &[v(D17, ".4s"), v(lhs, ".8h"), v(rhs, ".8h")]);
            instruction(emit, "addp", &[v(result, ".4s"), v(D16, ".4s"), v(D17, ".4s")]);
        }
        // pmaddubsw multiplies unsigned bytes of its left operand with signed bytes of its right one, and adds
        // adjacent products into 16-bit lanes with signed saturation.
        MultiplyAddUnsignedSignedI8 => {
            instruction(emit, "ushll", &[v(D16, ".8h"), v(lhs, ".8b"), Part::Immediate(0)]);
            instruction(emit, "ushll2", &[v(D17, ".8h"), v(lhs, ".16b"), Part::Immediate(0)]);
            instruction(emit, "sshll", &[v(D18, ".8h"), v(rhs, ".8b"), Part::Immediate(0)]);
            instruction(emit, "sshll2", &[v(D19, ".8h"), v(rhs, ".16b"), Part::Immediate(0)]);
            instruction(emit, "mul", &[v(D16, ".8h"), v(D16, ".8h"), v(D18, ".8h")]);
            instruction(emit, "mul", &[v(D17, ".8h"), v(D17, ".8h"), v(D19, ".8h")]);
            instruction(emit, "uzp1", &[v(D18, ".8h"), v(D16, ".8h"), v(D17, ".8h")]);
            instruction(emit, "uzp2", &[v(D19, ".8h"), v(D16, ".8h"), v(D17, ".8h")]);
            instruction(emit, "sqadd", &[v(result, ".8h"), v(D18, ".8h"), v(D19, ".8h")]);
        }
        // The shifts take their count from the low 64 bits of the right operand, and shift every bit out for
        // counts of at least the lane width. ushl and sshl shift right for negative counts, and by the low byte of
        // the count, so the count is clamped to the lane width first.
        ShiftLeftI16
        | ShiftRightUnsignedI16
        | ShiftRightSignedI16
        | ShiftLeftI32
        | ShiftRightUnsignedI32
        | ShiftRightSignedI32
        | ShiftLeftI64
        | ShiftRightUnsignedI64 => {
            let (arrangement, width) = match operation {
                ShiftLeftI16 | ShiftRightUnsignedI16 | ShiftRightSignedI16 => (".8h", 16),
                ShiftLeftI32 | ShiftRightUnsignedI32 | ShiftRightSignedI32 => (".4s", 32),
                _ => (".2d", 64),
            };
            instruction(emit, "fmov", &[x(X9), d(rhs)]);
            instruction(emit, "mov", &[x(X10), Part::Immediate(width)]);
            instruction(emit, "cmp", &[x(X9), x(X10)]);
            instruction(emit, "csel", &[x(X9), x(X9), x(X10), Part::Literal("lo")]);
            if !matches!(operation, ShiftLeftI16 | ShiftLeftI32 | ShiftLeftI64) {
                instruction(emit, "neg", &[x(X9), x(X9)]);
            }
            if width == 64 {
                instruction(emit, "dup", &[v(D16, arrangement), x(X9)]);
            } else {
                instruction(emit, "dup", &[v(D16, arrangement), w(X9)]);
            }
            let mnemonic = if matches!(operation, ShiftRightSignedI16 | ShiftRightSignedI32) {
                "sshl"
            } else {
                "ushl"
            };
            instruction(
                emit,
                mnemonic,
                &[v(result, arrangement), v(lhs, arrangement), v(D16, arrangement)],
            );
        }
        // pshufb picks the byte its index's low four bits select, or zero if the index has its top bit set. tbl
        // gives zero for any index above 15.
        Shuffle8 => {
            instruction(emit, "movi", &[v(D16, ".16b"), Part::HexImmediate(0x8f)]);
            instruction(emit, "and", &[v(D16, ".16b"), v(rhs, ".16b"), v(D16, ".16b")]);
            instruction(emit, "tbl", &[v(result, ".16b"), table(lhs), v(D16, ".16b")]);
        }
        NarrowSignedI16 | NarrowUnsignedI16 | NarrowSignedI32 | NarrowUnsignedI32 => {
            let (low, high) = if matches!(operation, NarrowSignedI16 | NarrowSignedI32) {
                ("sqxtn", "sqxtn2")
            } else {
                ("sqxtun", "sqxtun2")
            };
            let (narrow_low, narrow_high, wide) = if matches!(operation, NarrowSignedI16 | NarrowUnsignedI16) {
                (".8b", ".16b", ".8h")
            } else {
                (".4h", ".8h", ".4s")
            };
            instruction(emit, low, &[v(D16, narrow_low), v(lhs, wide)]);
            instruction(emit, high, &[v(D16, narrow_high), v(rhs, wide)]);
            move_vector(emit, result, D16);
        }
        // minps and maxps give their right operand unless the left one compares strictly smaller or larger, so for
        // NaNs and for zeros of either sign.
        MinimumF32 | MaximumF32 | MinimumF64 | MaximumF64 => {
            let arrangement = if matches!(operation, MinimumF32 | MaximumF32) {
                ".4s"
            } else {
                ".2d"
            };
            if matches!(operation, MinimumF32 | MinimumF64) {
                instruction(
                    emit,
                    "fcmgt",
                    &[v(D16, arrangement), v(rhs, arrangement), v(lhs, arrangement)],
                );
            } else {
                instruction(
                    emit,
                    "fcmgt",
                    &[v(D16, arrangement), v(lhs, arrangement), v(rhs, arrangement)],
                );
            }
            instruction(emit, "bsl", &[v(D16, ".16b"), v(lhs, ".16b"), v(rhs, ".16b")]);
            move_vector(emit, result, D16);
        }
        NotEqualF32 | NotEqualF64 => {
            let arrangement = if operation == NotEqualF32 { ".4s" } else { ".2d" };
            instruction(
                emit,
                "fcmeq",
                &[v(result, arrangement), v(lhs, arrangement), v(rhs, arrangement)],
            );
            instruction(emit, "mvn", &[v(result, ".16b"), v(result, ".16b")]);
        }
        LessF32 | LessF64 => {
            let arrangement = if operation == LessF32 { ".4s" } else { ".2d" };
            instruction(
                emit,
                "fcmgt",
                &[v(result, arrangement), v(rhs, arrangement), v(lhs, arrangement)],
            );
        }
        LessOrEqualF32 | LessOrEqualF64 => {
            let arrangement = if operation == LessOrEqualF32 { ".4s" } else { ".2d" };
            instruction(
                emit,
                "fcmge",
                &[v(result, arrangement), v(rhs, arrangement), v(lhs, arrangement)],
            );
        }
        UnorderedF32 | UnorderedF64 => {
            let arrangement = if operation == UnorderedF32 { ".4s" } else { ".2d" };
            instruction(
                emit,
                "fcmeq",
                &[v(D16, arrangement), v(lhs, arrangement), v(lhs, arrangement)],
            );
            instruction(
                emit,
                "fcmeq",
                &[v(D17, arrangement), v(rhs, arrangement), v(rhs, arrangement)],
            );
            instruction(emit, "and", &[v(D16, ".16b"), v(D16, ".16b"), v(D17, ".16b")]);
            instruction(emit, "mvn", &[v(result, ".16b"), v(D16, ".16b")]);
        }
        _ => unreachable!("'{}' is not a binary vector operation", operation.name()),
    }
}

fn vector_unary(
    emit: &mut Emit<'_>,
    operation: VectorOperation,
    destination: PhysicalRegister,
    source: PhysicalRegister,
) {
    use VectorOperation::*;
    let simple = |mnemonic: &'static str, destination_arrangement: &'static str, source_arrangement: &'static str| {
        (mnemonic, destination_arrangement, source_arrangement)
    };
    let single = match operation {
        SquareRootF32 => Some(simple("fsqrt", ".4s", ".4s")),
        CeilF32 => Some(simple("frintp", ".4s", ".4s")),
        FloorF32 => Some(simple("frintm", ".4s", ".4s")),
        TruncF32 => Some(simple("frintz", ".4s", ".4s")),
        NearestF32 => Some(simple("frintn", ".4s", ".4s")),
        SquareRootF64 => Some(simple("fsqrt", ".2d", ".2d")),
        CeilF64 => Some(simple("frintp", ".2d", ".2d")),
        FloorF64 => Some(simple("frintm", ".2d", ".2d")),
        TruncF64 => Some(simple("frintz", ".2d", ".2d")),
        NearestF64 => Some(simple("frintn", ".2d", ".2d")),
        AbsoluteI8 => Some(simple("abs", ".16b", ".16b")),
        AbsoluteI16 => Some(simple("abs", ".8h", ".8h")),
        AbsoluteI32 => Some(simple("abs", ".4s", ".4s")),
        ConvertSignedI32ToF32 => Some(simple("scvtf", ".4s", ".4s")),
        PromoteLowF32 => Some(simple("fcvtl", ".2d", ".2s")),
        // cvtpd2ps clears the upper half, as does writing a 64-bit arrangement.
        DemoteF64 => Some(simple("fcvtn", ".2s", ".2d")),
        HighHalf => Some(simple("dup", ".2d", ".d[1]")),
        BroadcastLowI32 => Some(simple("dup", ".4s", ".s[0]")),
        _ => None,
    };
    if let Some((mnemonic, destination_arrangement, source_arrangement)) = single {
        instruction(
            emit,
            mnemonic,
            &[v(destination, destination_arrangement), v(source, source_arrangement)],
        );
        return;
    }
    match operation {
        ExtendLowSignedI8 | ExtendLowUnsignedI8 | ExtendLowSignedI16 | ExtendLowUnsignedI16 | ExtendLowSignedI32
        | ExtendLowUnsignedI32 => {
            let mnemonic = if matches!(operation, ExtendLowSignedI8 | ExtendLowSignedI16 | ExtendLowSignedI32) {
                "sshll"
            } else {
                "ushll"
            };
            let (wide, narrow) = match operation {
                ExtendLowSignedI8 | ExtendLowUnsignedI8 => (".8h", ".8b"),
                ExtendLowSignedI16 | ExtendLowUnsignedI16 => (".4s", ".4h"),
                _ => (".2d", ".2s"),
            };
            instruction(
                emit,
                mnemonic,
                &[v(destination, wide), v(source, narrow), Part::Immediate(0)],
            );
        }
        ConvertLowSignedI32ToF64 => {
            instruction(emit, "sshll", &[v(D16, ".2d"), v(source, ".2s"), Part::Immediate(0)]);
            instruction(emit, "scvtf", &[v(destination, ".2d"), v(D16, ".2d")]);
        }
        // cvttps2dq gives 0x80000000 for NaNs and values out of range, where fcvtzs saturates and gives zero for
        // NaNs. Values below the range already give 0x80000000.
        TruncateF32ToSignedI32 => {
            instruction(emit, "fcvtzs", &[v(D16, ".4s"), v(source, ".4s")]);
            splat32(emit, D17, 0x4f00_0000);
            instruction(emit, "fcmgt", &[v(D17, ".4s"), v(D17, ".4s"), v(source, ".4s")]);
            splat32(emit, D18, 0x8000_0000);
            instruction(emit, "bsl", &[v(D17, ".16b"), v(D16, ".16b"), v(D18, ".16b")]);
            move_vector(emit, destination, D17);
        }
        // cvttpd2dq gives 0x80000000 for NaNs and values whose truncation is out of range, and clears the upper
        // half.
        TruncateF64ToSignedI32 => {
            instruction(emit, "fcvtzs", &[v(D16, ".2d"), v(source, ".2d")]);
            splat64(emit, D17, 0x41e0_0000_0000_0000);
            instruction(emit, "fcmgt", &[v(D17, ".2d"), v(D17, ".2d"), v(source, ".2d")]);
            splat64(emit, D18, 0xc1e0_0000_0020_0000);
            instruction(emit, "fcmgt", &[v(D18, ".2d"), v(source, ".2d"), v(D18, ".2d")]);
            instruction(emit, "and", &[v(D17, ".16b"), v(D17, ".16b"), v(D18, ".16b")]);
            instruction(emit, "xtn", &[v(D16, ".2s"), v(D16, ".2d")]);
            instruction(emit, "xtn", &[v(D17, ".2s"), v(D17, ".2d")]);
            splat32(emit, D18, 0x8000_0000);
            instruction(emit, "bsl", &[v(D17, ".8b"), v(D16, ".8b"), v(D18, ".8b")]);
            move_vector(emit, destination, D17);
        }
        _ => unreachable!("'{}' is not a unary vector operation", operation.name()),
    }
}

fn vector_to_general_purpose(
    emit: &mut Emit<'_>,
    operation: VectorOperation,
    destination: PhysicalRegister,
    source: PhysicalRegister,
) {
    use VectorOperation::*;
    match operation {
        Low64 => instruction(emit, "fmov", &[x(destination), d(source)]),
        High64 => instruction(emit, "mov", &[x(destination), v(source, ".d[1]")]),
        // Gather the sign bits of the bytes: each shift-and-accumulate doubles the bits a lane holds, until the low
        // byte of each 64-bit half holds the bits of its eight bytes.
        ByteMask => {
            instruction(emit, "ushr", &[v(D16, ".16b"), v(source, ".16b"), Part::Immediate(7)]);
            instruction(emit, "usra", &[v(D16, ".8h"), v(D16, ".8h"), Part::Immediate(7)]);
            instruction(emit, "usra", &[v(D16, ".4s"), v(D16, ".4s"), Part::Immediate(14)]);
            instruction(emit, "usra", &[v(D16, ".2d"), v(D16, ".2d"), Part::Immediate(28)]);
            instruction(emit, "umov", &[w(X9), v(D16, ".b[0]")]);
            instruction(emit, "umov", &[w(X10), v(D16, ".b[8]")]);
            instruction(emit, "orr", &[w(destination), w(X9), w(X10), Part::Literal("lsl #8")]);
        }
        SignMaskF32 => {
            instruction(emit, "ushr", &[v(D16, ".4s"), v(source, ".4s"), Part::Immediate(31)]);
            instruction(emit, "usra", &[v(D16, ".2d"), v(D16, ".2d"), Part::Immediate(31)]);
            instruction(emit, "mov", &[w(X9), v(D16, ".s[0]")]);
            instruction(emit, "mov", &[w(X10), v(D16, ".s[2]")]);
            instruction(emit, "orr", &[w(destination), w(X9), w(X10), Part::Literal("lsl #2")]);
        }
        SignMaskF64 => {
            instruction(emit, "ushr", &[v(D16, ".2d"), v(source, ".2d"), Part::Immediate(63)]);
            instruction(emit, "mov", &[x(X9), v(D16, ".d[0]")]);
            instruction(emit, "mov", &[x(X10), v(D16, ".d[1]")]);
            instruction(emit, "orr", &[x(destination), x(X9), x(X10), Part::Literal("lsl #1")]);
        }
        _ => unreachable!("'{}' does not give a general-purpose value", operation.name()),
    }
}
