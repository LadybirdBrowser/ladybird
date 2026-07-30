/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::description::OperandKind::{
    FprIn as FR, FuncSymbol as F, GprIn as R, Imm as I, Label as L, Memory as A, RegisterIn as AR,
};
use super::description::{
    BinaryOperation, FloatBinaryOperation, FloatConversion as IntrinsicFloatConversion, FloatUnaryOperation,
    FloatingPointOperation, IntegerBinaryOperation, IntegerWidth, MemoryOperation, MemoryWidth, Operation, PairWidth,
    ShiftOperation, SignCondition, TestCondition, ZeroCondition,
};
use super::emitter::{
    SimpleInstruction, SimpleOperand, emit_file_header, emit_handlers, emit_label, emit_program,
    emit_simple_instruction, integer, native, pair, simple, single, w, word,
};
use super::ir::{
    MachineCondition as Condition, MachineFunction as Handler, MachineInstruction, MachineMemoryAddress,
    MachineOperand as Operand, MachineProgram as Program, RuntimeConstants,
};
use super::machine_verify::{define_machine_opcodes, operands_match};
use super::registers::{PhysicalRegister, aarch64 as registers};
use crate::frontend::layout::KnownLayoutConstant;
use crate::{Architecture, CompileOptions, ObjectFormat};
use std::fmt::Write;

pub(crate) mod finalize;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum AddSubtractOperation {
    Add,
    Subtract,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum FlagUpdate {
    Preserve,
    Set,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum LogicalOperation {
    And,
    Or,
    Xor,
}

impl LogicalOperation {
    pub(crate) fn from_binary(operation: BinaryOperation) -> Option<Self> {
        match operation {
            BinaryOperation::And => Some(Self::And),
            BinaryOperation::Or => Some(Self::Or),
            BinaryOperation::Xor => Some(Self::Xor),
            BinaryOperation::Add | BinaryOperation::Subtract | BinaryOperation::Multiply => None,
        }
    }

    fn mnemonic(self) -> &'static str {
        match self {
            Self::And => "and",
            Self::Or => "orr",
            Self::Xor => "eor",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum ImmediateShift {
    None,
    Twelve,
}

impl ImmediateShift {
    fn amount(self) -> Option<u8> {
        match self {
            Self::None => None,
            Self::Twelve => Some(12),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum AddressIndexShift {
    One,
    Two,
    Three,
}

impl AddressIndexShift {
    fn amount(self) -> u8 {
        match self {
            Self::One => 1,
            Self::Two => 2,
            Self::Three => 3,
        }
    }

    pub(crate) fn for_memory(width: MemoryWidth, scale: i64) -> Option<Self> {
        match (width, scale) {
            (MemoryWidth::HalfWord, 2) => Some(Self::One),
            (MemoryWidth::Word | MemoryWidth::Float, 4) => Some(Self::Two),
            (MemoryWidth::DoubleWord, 8) => Some(Self::Three),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum MemoryAddressing {
    Base,
    UnsignedImmediate,
    UnscaledImmediate,
    Register,
    ShiftedRegister(AddressIndexShift),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum ConditionFlags {
    Equal,
}

impl ConditionFlags {
    fn bits(self) -> u8 {
        match self {
            Self::Equal => 4,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum FloatConversion {
    Signed64ToDouble,
    Signed32ToDouble,
    FloatToDouble,
    DoubleToFloat,
    DoubleToSigned32Truncate,
    JavaScriptToSigned32,
}

fn load_instruction(width: MemoryWidth, signed: bool, addressing: MemoryAddressing) -> SimpleInstruction {
    let mnemonic = match (width, signed, addressing) {
        (
            MemoryWidth::DoubleWord | MemoryWidth::Float | MemoryWidth::Word,
            false,
            MemoryAddressing::UnscaledImmediate,
        ) => "ldur",
        (MemoryWidth::Word, true, MemoryAddressing::UnscaledImmediate) => "ldursw",
        (MemoryWidth::HalfWord, false, MemoryAddressing::UnscaledImmediate) => "ldurh",
        (MemoryWidth::HalfWord, true, MemoryAddressing::UnscaledImmediate) => "ldursh",
        (MemoryWidth::Byte, false, MemoryAddressing::UnscaledImmediate) => "ldurb",
        (MemoryWidth::Byte, true, MemoryAddressing::UnscaledImmediate) => "ldursb",
        (MemoryWidth::DoubleWord | MemoryWidth::Float | MemoryWidth::Word, false, _) => "ldr",
        (MemoryWidth::Word, true, _) => "ldrsw",
        (MemoryWidth::HalfWord, false, _) => "ldrh",
        (MemoryWidth::HalfWord, true, _) => "ldrsh",
        (MemoryWidth::Byte, false, _) => "ldrb",
        (MemoryWidth::Byte, true, _) => "ldrsb",
        (MemoryWidth::DoubleWord | MemoryWidth::Float, true, _) => {
            unreachable!("unsupported finalized AArch64 signed load")
        }
    };
    let destination = match width {
        MemoryWidth::DoubleWord => native(0),
        MemoryWidth::Float => single(0),
        MemoryWidth::Word if signed => native(0),
        MemoryWidth::Word | MemoryWidth::HalfWord | MemoryWidth::Byte => word(0),
    };
    simple!(mnemonic; destination, SimpleOperand::Resolved(1, ""))
}

fn store_instruction(width: MemoryWidth, addressing: MemoryAddressing) -> SimpleInstruction {
    let mnemonic = match (width, addressing) {
        (MemoryWidth::DoubleWord | MemoryWidth::Float | MemoryWidth::Word, MemoryAddressing::UnscaledImmediate) => {
            "stur"
        }
        (MemoryWidth::HalfWord, MemoryAddressing::UnscaledImmediate) => "sturh",
        (MemoryWidth::Byte, MemoryAddressing::UnscaledImmediate) => "sturb",
        (MemoryWidth::DoubleWord | MemoryWidth::Float | MemoryWidth::Word, _) => "str",
        (MemoryWidth::HalfWord, _) => "strh",
        (MemoryWidth::Byte, _) => "strb",
    };
    let source = match width {
        MemoryWidth::DoubleWord => native(1),
        MemoryWidth::Float => single(1),
        MemoryWidth::Word | MemoryWidth::HalfWord | MemoryWidth::Byte => word(1),
    };
    simple!(mnemonic; source, SimpleOperand::Resolved(0, ""))
}

fn add_subtract_immediate_instruction(
    operation: AddSubtractOperation,
    shift: ImmediateShift,
    flags: FlagUpdate,
) -> SimpleInstruction {
    let mnemonic = add_subtract_mnemonic(operation, flags);
    if let Some(shift) = shift.amount() {
        simple!(
            mnemonic;
            native(0),
            native(1),
            SimpleOperand::HashImmediate(2, false),
            SimpleOperand::ShiftValue(shift)
        )
    } else {
        simple!(mnemonic; native(0), native(1), SimpleOperand::HashImmediate(2, false))
    }
}

define_machine_opcodes! {
    Operand, operands;
    encoding (immediate, shift_fits) {
        let address = |index| {
            let Some(Operand::Address(address)) = operands.get(index) else {
                return None;
            };
            Some(address)
        };
    }
    [Pseudo] => Self::Pseudo => any;
    [Label] => Self::Label => [[L]];
    [Branch] => Self::Branch => [[L]] printing simple!("b"; SimpleOperand::Label(0));
    [BranchRegister] => Self::BranchRegister => [[R]] printing simple!("br"; native(0));
    [BranchToExit] => Self::BranchToExit => [[]] printing simple!("b"; SimpleOperand::Literal(".Lexit"));
    [BranchOnBit31(SignCondition)] => Self::BranchOnBit31(condition) => [[R, L]] printing simple!(condition.select("tbnz", "tbz"); word(0), SimpleOperand::Literal("#31"), SimpleOperand::Label(1));
    [BranchLink] => Self::BranchLink => [[F]] encoding {
        matches!(
            operands,
            [Operand::Relocation(relocation)]
                if relocation.kind() == crate::low_ir::RelocationKind::FunctionCall
        )
    } printing simple!("bl"; SimpleOperand::Relocation(0));
    [BranchLinkRegister] => Self::BranchLinkRegister => [[R]] printing simple!("blr"; native(0));
    [BranchBitSetToExit(u8)] => Self::BranchBitSetToExit(bit) => [[R]] encoding {
        bit < 64
    };
    [Subtract32Register] => Self::Subtract32Register => [[R, R, R]] printing simple!("sub"; word(0), word(1), word(2));
    [SetInstructionPointer] => Self::SetInstructionPointer => [[R]] printing simple!("add"; SimpleOperand::Literal("x21"), SimpleOperand::Literal("x26"), word(0), SimpleOperand::Literal("uxtw"));
    [AdvanceInstructionPointer] => Self::AdvanceInstructionPointer => [[R]] printing simple!("add"; SimpleOperand::Literal("x21"), SimpleOperand::Literal("x21"), word(0), SimpleOperand::Literal("uxtw"));
    [Load { width: MemoryWidth, signed: bool, addressing: MemoryAddressing }] => opcode @ Self::Load { width, signed, addressing } => {
        match opcode {
            Self::Load { width: MemoryWidth::Float, .. } => operands_match(operands, &[FR, A]),
            _ => operands_match(operands, &[R, A]),
        }
    } encoding {
        let Self::Load { width, signed, addressing } = opcode else { unreachable!() };
        (!signed || !matches!(width, MemoryWidth::DoubleWord | MemoryWidth::Float))
            && address(1).is_some_and(|address| {
                memory_address_is_encodable(width, addressing, address)
            })
    } printing load_instruction(width, signed, addressing);
    [Store { width: MemoryWidth, addressing: MemoryAddressing }] => opcode @ Self::Store { width, addressing } => {
        match opcode {
            Self::Store { width: MemoryWidth::Float, .. } => operands_match(operands, &[A, FR]),
            _ => operands_match(operands, &[A, R]),
        }
    } encoding {
        let Self::Store { width, addressing } = opcode else { unreachable!() };
        address(0).is_some_and(|address| {
            memory_address_is_encodable(width, addressing, address)
        })
    } printing store_instruction(width, addressing);
    [Increment32Register] => Self::Increment32Register => [[R]] printing simple!("add"; word(0), word(0), SimpleOperand::Literal("#1"));
    [LoadPair(PairWidth)] => Self::LoadPair(width) => [[R, R, A]] encoding {
        address(2).is_some_and(|address| pair_address_is_encodable(width, address))
    } printing simple!("ldp"; pair(0, width), pair(1, width), SimpleOperand::Resolved(2, ""));
    [StorePair(PairWidth)] => Self::StorePair(width) => [[A, R, R]] encoding {
        address(0).is_some_and(|address| pair_address_is_encodable(width, address))
    } printing simple!("stp"; pair(1, width), pair(2, width), SimpleOperand::Resolved(0, ""));
    [AddSubtractImmediate { operation: AddSubtractOperation, shift: ImmediateShift, flags: FlagUpdate }] => Self::AddSubtractImmediate { operation, shift, flags } => [[R, R, I]] encoding {
        immediate(2)
            .or_else(|| immediate(1))
            .is_some_and(|value| (0..=4095).contains(&value))
    } printing add_subtract_immediate_instruction(operation, shift, flags);
    [AddSubtractRegister { operation: AddSubtractOperation, flags: FlagUpdate }] => Self::AddSubtractRegister { operation, flags } => [[R, R, R]] printing simple!(add_subtract_mnemonic(operation, flags); native(0), native(1), native(2));
    [AddSubtractZero(AddSubtractOperation)] => Self::AddSubtractZero(_) => any;
    [AddSubtractLargeImmediate { operation: AddSubtractOperation, flags: FlagUpdate }] => Self::AddSubtractLargeImmediate { .. } => any;
    [AddSubtract32ImmediateWithFlags(AddSubtractOperation)] => Self::AddSubtract32ImmediateWithFlags(operation) => [[R, I]] encoding {
        immediate(2)
            .or_else(|| immediate(1))
            .is_some_and(|value| (0..=4095).contains(&value))
    } printing simple!(add_subtract_mnemonic(operation, FlagUpdate::Set); word(0), word(0), SimpleOperand::HashImmediate(1, false));
    [AddSubtract32RegisterWithFlags(AddSubtractOperation)] => Self::AddSubtract32RegisterWithFlags(operation) => [[R, R]] printing simple!(add_subtract_mnemonic(operation, FlagUpdate::Set); word(0), word(0), word(1));
    [AddShiftedRegister { shift: AddressIndexShift }] => Self::AddShiftedRegister { shift } => [[R, R, R]] printing simple!("add"; native(0), native(1), native(2), SimpleOperand::ShiftValue(shift.amount()));
    [MultiplyAdd64] => Self::MultiplyAdd64 => [[R, R, R, R]] printing simple!("madd"; native(0), native(1), native(2), native(3));
    [LogicalImmediate { operation: LogicalOperation, width: IntegerWidth }] => Self::LogicalImmediate { operation, width } => [[R, R, I]] encoding {
        immediate(2).is_some_and(|value| match width {
            IntegerWidth::U8 => false,
            width => is_logical_immediate(width, value as u64),
        })
    } printing simple!(operation.mnemonic(); integer(0, width), integer(1, width), SimpleOperand::HashImmediate(2, true));
    [LogicalRegister { operation: LogicalOperation, width: IntegerWidth }] => Self::LogicalRegister { operation, width } => [[R, R, R]] printing simple!(operation.mnemonic(); integer(0, width), integer(1, width), integer(2, width));
    [Multiply64] => Self::Multiply64 => [[R, R, R]] printing simple!("mul"; native(0), native(1), native(2));
    [SignedMultiplyLong32] => Self::SignedMultiplyLong32 => [[R, R, R]] printing simple!("smull"; native(0), word(1), word(2));
    [CompareRegister(IntegerWidth)] => Self::CompareRegister(width) => [[R, R]] printing simple!("cmp"; integer(0, width), integer(1, width));
    [CompareImmediate(IntegerWidth)] => Self::CompareImmediate(width) => [[R, I]] encoding {
        immediate(1).is_some_and(|value| (0..=4095).contains(&value))
    } printing simple!("cmp"; integer(0, width), SimpleOperand::HashImmediate(1, false));
    [CompareNegativeImmediate(IntegerWidth)] => Self::CompareNegativeImmediate(width) => [[R, I]] encoding {
        immediate(1).is_some_and(|value| (0..=4095).contains(&value))
    } printing simple!("cmn"; integer(0, width), SimpleOperand::HashImmediate(1, false));
    [ConditionalCompareRegister { width: IntegerWidth, condition: Condition, fallback: ConditionFlags }] => Self::ConditionalCompareRegister { width, condition, fallback } => [[R, R]] printing simple!("ccmp"; integer(0, width), integer(1, width), SimpleOperand::HashValue(i64::from(fallback.bits())), SimpleOperand::Literal(condition.aarch64_suffix()));
    [ConditionalCompareImmediate { width: IntegerWidth, condition: Condition, fallback: ConditionFlags }] => Self::ConditionalCompareImmediate { width, condition, fallback } => [[R, I]] encoding {
        immediate(1).is_some_and(|value| (0..=31).contains(&value))
    } printing simple!("ccmp"; integer(0, width), SimpleOperand::HashImmediate(1, false), SimpleOperand::HashValue(i64::from(fallback.bits())), SimpleOperand::Literal(condition.aarch64_suffix()));
    [Test64Immediate] => Self::Test64Immediate => [[R, I]] encoding {
        immediate(1).is_some_and(|value| is_logical_immediate(IntegerWidth::U64, value as u64))
    } printing simple!("tst"; native(0), SimpleOperand::HashImmediate(1, true));
    [Test64Register] => Self::Test64Register => [[R, R]] printing simple!("tst"; native(0), native(1));
    [CompareAndBranchZero { width: IntegerWidth, condition: ZeroCondition }] => Self::CompareAndBranchZero { width, condition } => [[R, L]] printing simple!(condition.select("cbz", "cbnz"); integer(0, width), SimpleOperand::Label(1));
    [TestBitAndBranch { width: IntegerWidth, condition: TestCondition }] => Self::TestBitAndBranch { width, condition } => [[R, I, L]] encoding {
        immediate(1).is_some_and(|value| shift_fits(width, value))
    } printing simple!(condition.select("tbnz", "tbz"); integer(0, width), SimpleOperand::HashImmediate(1, false), SimpleOperand::Label(2));
    [BranchCondition(Condition)] => Self::BranchCondition(condition) => [[L]] printing simple!(condition.aarch64_mnemonic(); SimpleOperand::Label(0));
    [Break] => Self::Break => [[]] printing simple!("brk"; SimpleOperand::Literal("#0"));
    [MoveFloatBits64] => Self::MoveFloatBits64 => [[AR, AR]] printing simple!("fmov"; native(0), native(1));
    [CompareDouble] => Self::CompareDouble => [[FR, FR]] printing simple!("fcmp"; native(0), native(1));
    [FloatArithmetic(FloatBinaryOperation)] => Self::FloatArithmetic(operation) => [[FR, FR, FR]] printing simple!(match operation {
        FloatBinaryOperation::Add => "fadd",
        FloatBinaryOperation::Subtract => "fsub",
        FloatBinaryOperation::Multiply => "fmul",
        FloatBinaryOperation::Divide => "fdiv",
    }; native(0), native(1), native(2));
    [FloatUnary(FloatUnaryOperation)] => Self::FloatUnary(operation) => [[FR, FR]] printing match operation {
        FloatUnaryOperation::Floor => simple!("frintm"; native(0), native(1)),
        FloatUnaryOperation::Ceil => simple!("frintp"; native(0), native(1)),
        FloatUnaryOperation::SquareRoot => simple!("fsqrt"; native(0), native(1)),
    };
    [FloatConversion(FloatConversion)] => Self::FloatConversion(conversion) => {
        match conversion {
            FloatConversion::Signed64ToDouble | FloatConversion::Signed32ToDouble => operands_match(operands, &[FR, R]),
            FloatConversion::FloatToDouble | FloatConversion::DoubleToFloat => operands_match(operands, &[FR, FR]),
            FloatConversion::DoubleToSigned32Truncate | FloatConversion::JavaScriptToSigned32 => operands_match(operands, &[R, FR]),
        }
    } printing match conversion {
        FloatConversion::Signed64ToDouble => simple!("scvtf"; native(0), native(1)),
        FloatConversion::Signed32ToDouble => simple!("scvtf"; native(0), word(1)),
        FloatConversion::FloatToDouble => simple!("fcvt"; native(0), single(1)),
        FloatConversion::DoubleToFloat => simple!("fcvt"; single(0), native(1)),
        FloatConversion::DoubleToSigned32Truncate => simple!("fcvtzs"; word(0), native(1)),
        FloatConversion::JavaScriptToSigned32 => simple!("fjcvtzs"; word(0), native(1)),
    };
    [BranchOverflow] => Self::BranchOverflow => [[L]] printing simple!("b.vs"; SimpleOperand::Label(0));
    [BranchNotEqual] => Self::BranchNotEqual => [[L]] printing simple!("b.ne"; SimpleOperand::Label(0));
    [ConditionalCompareZeroEqual] => Self::ConditionalCompareZeroEqual => [[]] printing simple!("ccmp"; SimpleOperand::Literal("xzr"), SimpleOperand::Literal("xzr"), SimpleOperand::Literal("#1"), SimpleOperand::Literal("eq"));
    [SignExtend32To64] => Self::SignExtend32To64 => [[R, R]] printing simple!("sxtw"; native(0), word(1));
    [ShiftImmediate { operation: ShiftOperation, width: IntegerWidth }] => Self::ShiftImmediate { operation, width } => [[R, R, I]] encoding {
        matches!(width, IntegerWidth::U32 | IntegerWidth::U64)
            && immediate(2).is_some_and(|value| shift_fits(width, value))
    } printing simple!(shift_mnemonic(operation); integer(0, width), integer(1, width), SimpleOperand::HashImmediate(2, false));
    [ShiftRegister { operation: ShiftOperation, width: IntegerWidth }] => Self::ShiftRegister { operation, width } => [[R, R, R]] encoding {
        matches!(width, IntegerWidth::U32 | IntegerWidth::U64)
    } printing simple!(shift_mnemonic(operation); integer(0, width), integer(1, width), integer(2, width));
    [And64Immediate] => Self::And64Immediate => [[R, R, I]] encoding {
        immediate(2).is_some_and(|value| is_logical_immediate(IntegerWidth::U64, value as u64))
    } printing simple!("and"; native(0), native(1), SimpleOperand::HashImmediate(2, true));
    [MoveRegister(IntegerWidth)] => Self::MoveRegister(width) => [[R, R]] printing simple!("mov"; integer(0, width), integer(1, width));
    [MoveImmediateDecimal(IntegerWidth)] => Self::MoveImmediateDecimal(width) => [[R, I]] printing simple!("mov"; integer(0, width), SimpleOperand::HashImmediate(1, false));
    [MoveImmediateHex(IntegerWidth)] => Self::MoveImmediateHex(width) => [[R, I]] printing simple!("mov"; integer(0, width), SimpleOperand::HashImmediate(1, true));
    [MoveWideZero(IntegerWidth)] => Self::MoveWideZero(width) => [[R, I]] encoding {
        immediate(1).is_some_and(|value| (0..=u16::MAX as i64).contains(&value))
    } printing simple!("movz"; integer(0, width), SimpleOperand::HashImmediate(1, true));
    [MoveWideZeroShifted(IntegerWidth)] => Self::MoveWideZeroShifted(width) => [[R, I, I]] encoding {
        immediate(1).is_some_and(|value| (0..=u16::MAX as i64).contains(&value))
            && immediate(2).is_some_and(|shift| match width {
                IntegerWidth::U32 => matches!(shift, 0 | 16),
                IntegerWidth::U64 => matches!(shift, 0 | 16 | 32 | 48),
                IntegerWidth::U8 | IntegerWidth::U16 => false,
            })
    } printing simple!("movz"; integer(0, width), SimpleOperand::HashImmediate(1, true), SimpleOperand::ShiftImmediate(2));
    [MoveWideNot(IntegerWidth)] => Self::MoveWideNot(width) => [[R, I]] encoding {
        matches!(width, IntegerWidth::U32 | IntegerWidth::U64)
            && immediate(1).is_some_and(|value| {
                (0..=u16::MAX as i64).contains(&value)
            })
    } printing simple!("movn"; integer(0, width), SimpleOperand::HashImmediate(1, false));
    [MoveWideKeep(IntegerWidth)] => Self::MoveWideKeep(width) => [[R, I]] encoding {
        immediate(1).is_some_and(|value| (0..=u16::MAX as i64).contains(&value))
    } printing simple!("movk"; integer(0, width), SimpleOperand::HashImmediate(1, true));
    [MoveWideKeepShifted(IntegerWidth)] => Self::MoveWideKeepShifted(width) => [[R, I, I]] encoding {
        immediate(1).is_some_and(|value| (0..=u16::MAX as i64).contains(&value))
            && immediate(2).is_some_and(|shift| match width {
                IntegerWidth::U32 => matches!(shift, 0 | 16),
                IntegerWidth::U64 => matches!(shift, 0 | 16 | 32 | 48),
                IntegerWidth::U8 | IntegerWidth::U16 => false,
            })
    } printing simple!("movk"; integer(0, width), SimpleOperand::HashImmediate(1, true), SimpleOperand::ShiftImmediate(2));
    [Negate64] => Self::Negate64 => [[R]] printing simple!("neg"; native(0), native(0));
    [Negate32WithFlags] => Self::Negate32WithFlags => [[R]] printing simple!("negs"; word(0), word(0));
    [BitwiseNot(IntegerWidth)] => Self::BitwiseNot(width) => [[R]] encoding {
        matches!(width, IntegerWidth::U32 | IntegerWidth::U64)
    } printing simple!("mvn"; integer(0, width), integer(0, width));
    [ExclusiveOr64Immediate] => Self::ExclusiveOr64Immediate => [[R, I]] encoding {
        immediate(1).is_some_and(|value| is_logical_immediate(IntegerWidth::U64, value as u64))
    } printing simple!("eor"; native(0), native(0), SimpleOperand::HashImmediate(1, true));
    [BitClear64Immediate] => Self::BitClear64Immediate => [[R, I]] encoding {
        immediate(1).is_some_and(|value| is_logical_immediate(IntegerWidth::U64, value as u64))
    } printing simple!("bic"; native(0), native(0), SimpleOperand::HashImmediate(1, true));
    [SignedDivide32] => Self::SignedDivide32 => [[R, R, R]] printing simple!("sdiv"; word(0), word(1), word(2));
    [MultiplySubtract32] => Self::MultiplySubtract32 => [[R, R, R, R]] printing simple!("msub"; word(0), word(1), word(2), word(3));
}

pub(crate) struct ImmediateMove {
    pub(crate) opcode: Opcode,
    pub(crate) value: i64,
    pub(crate) shift: Option<i64>,
}

pub(crate) fn immediate_moves(value: i64, width: IntegerWidth) -> Vec<ImmediateMove> {
    let value = value as u64;
    if width == IntegerWidth::U64 && value <= u32::MAX as u64 {
        return immediate_moves(value as i64, IntegerWidth::U32);
    }

    let mut moves = Vec::new();
    let mut push = |opcode, value, shift| {
        let opcode = match (opcode, shift) {
            (Opcode::MoveWideZero(width), Some(_)) => Opcode::MoveWideZeroShifted(width),
            (Opcode::MoveWideKeep(width), Some(_)) => Opcode::MoveWideKeepShifted(width),
            (opcode, _) => opcode,
        };
        moves.push(ImmediateMove { opcode, value, shift });
    };
    match width {
        IntegerWidth::U64 => {
            let inverted = !value;
            if inverted <= u16::MAX as u64 {
                push(Opcode::MoveWideNot(width), inverted as i64, None);
                return moves;
            }
            let mut first = true;
            for shift in [0, 16, 32, 48] {
                let halfword = (value >> shift) & 0xffff;
                if halfword != 0 {
                    push(
                        if std::mem::replace(&mut first, false) {
                            Opcode::MoveWideZero(width)
                        } else {
                            Opcode::MoveWideKeep(width)
                        },
                        halfword as i64,
                        Some(shift),
                    );
                }
            }
        }
        IntegerWidth::U32 => {
            let value = value & u32::MAX as u64;
            if value <= u16::MAX as u64 {
                push(Opcode::MoveImmediateDecimal(width), value as i64, None);
            } else {
                let inverted = (!value) & u32::MAX as u64;
                if inverted <= u16::MAX as u64 {
                    push(Opcode::MoveWideNot(width), inverted as i64, None);
                } else {
                    let [low, high] = [value & 0xffff, value >> 16];
                    match (low, high) {
                        (0, high) => push(Opcode::MoveWideZero(width), high as i64, Some(16)),
                        (low, 0) => push(Opcode::MoveImmediateHex(width), low as i64, None),
                        (low, high) => {
                            push(Opcode::MoveWideZero(width), low as i64, None);
                            push(Opcode::MoveWideKeep(width), high as i64, Some(16));
                        }
                    }
                }
            }
        }
        IntegerWidth::U8 | IntegerWidth::U16 => unreachable!("narrow immediate move reached finalization"),
    }
    moves
}

fn memory_address_is_encodable(
    width: MemoryWidth,
    addressing: MemoryAddressing,
    address: &MachineMemoryAddress,
) -> bool {
    match addressing {
        MemoryAddressing::Base => address.index.is_none() && address.displacement.is_none() && address.scale.is_none(),
        MemoryAddressing::UnsignedImmediate => {
            address.index.is_none()
                && address.scale.is_none()
                && address
                    .displacement
                    .is_some_and(|offset| unsigned_memory_offset_fits(width, offset))
        }
        MemoryAddressing::UnscaledImmediate => {
            address.index.is_none()
                && address.scale.is_none()
                && address
                    .displacement
                    .is_some_and(|offset| (-256..=255).contains(&offset))
        }
        MemoryAddressing::Register => {
            address.index.is_some() && address.scale.is_none() && address.displacement.is_none()
        }
        MemoryAddressing::ShiftedRegister(shift) => {
            AddressIndexShift::for_memory(width, 1 << shift.amount()) == Some(shift)
                && address.index.is_some()
                && address.scale.is_none()
                && address.displacement.is_none()
        }
    }
}

pub(crate) fn unsigned_memory_offset_fits(width: MemoryWidth, offset: i64) -> bool {
    match width {
        MemoryWidth::Byte => (0..=4095).contains(&offset),
        MemoryWidth::HalfWord => (0..=8190).contains(&offset) && offset % 2 == 0,
        MemoryWidth::Word | MemoryWidth::Float => (0..=16380).contains(&offset) && offset % 4 == 0,
        MemoryWidth::DoubleWord => (0..=32760).contains(&offset) && offset % 8 == 0,
    }
}

pub(crate) fn pair_offset_fits(width: PairWidth, offset: i64) -> bool {
    match width {
        PairWidth::Word => (-256..=252).contains(&offset) && offset % 4 == 0,
        PairWidth::DoubleWord => (-512..=504).contains(&offset) && offset % 8 == 0,
    }
}

fn pair_address_is_encodable(width: PairWidth, address: &MachineMemoryAddress) -> bool {
    address.index.is_none()
        && address.scale.is_none()
        && address
            .displacement
            .is_none_or(|offset| pair_offset_fits(width, offset))
}

impl Opcode {
    pub(crate) fn select_for_operands(
        operation: super::description::Operation,
        operands: &[super::ir::Operand],
    ) -> Self {
        match operation {
            Operation::IntegerBinary {
                operation: IntegerBinaryOperation::Binary(operation),
                width: IntegerWidth::U64,
            } if matches!(operation, BinaryOperation::Add | BinaryOperation::Subtract) => {
                let operation = match operation {
                    BinaryOperation::Add => AddSubtractOperation::Add,
                    BinaryOperation::Subtract => AddSubtractOperation::Subtract,
                    _ => unreachable!(),
                };
                match operands.get(1) {
                    Some(super::ir::Operand::Immediate(0)) if operation == AddSubtractOperation::Add => {
                        Self::AddSubtractZero(operation)
                    }
                    Some(super::ir::Operand::Immediate(value))
                        if operation == AddSubtractOperation::Add && (1..=4095).contains(value) =>
                    {
                        Self::AddSubtractImmediate {
                            operation,
                            shift: ImmediateShift::None,
                            flags: FlagUpdate::Preserve,
                        }
                    }
                    Some(super::ir::Operand::Immediate(value))
                        if operation == AddSubtractOperation::Add
                            && *value > 0
                            && *value <= 0xfff000
                            && *value & 0xfff == 0 =>
                    {
                        Self::AddSubtractImmediate {
                            operation,
                            shift: ImmediateShift::Twelve,
                            flags: FlagUpdate::Preserve,
                        }
                    }
                    Some(super::ir::Operand::Immediate(value))
                        if operation == AddSubtractOperation::Add && (-4095..0).contains(value) =>
                    {
                        Self::AddSubtractImmediate {
                            operation: AddSubtractOperation::Subtract,
                            shift: ImmediateShift::None,
                            flags: FlagUpdate::Preserve,
                        }
                    }
                    Some(super::ir::Operand::Immediate(value))
                        if operation == AddSubtractOperation::Subtract && (1..=4095).contains(value) =>
                    {
                        Self::AddSubtractImmediate {
                            operation,
                            shift: ImmediateShift::None,
                            flags: FlagUpdate::Set,
                        }
                    }
                    Some(super::ir::Operand::Immediate(_)) => Self::AddSubtractLargeImmediate {
                        operation,
                        flags: if operation == AddSubtractOperation::Add {
                            FlagUpdate::Preserve
                        } else {
                            FlagUpdate::Set
                        },
                    },
                    _ => Self::AddSubtractRegister {
                        operation,
                        flags: FlagUpdate::Set,
                    },
                }
            }
            Operation::IntegerBinary { .. }
            | Operation::Memory(MemoryOperation::Load { .. } | MemoryOperation::Store(_)) => Self::Pseudo,
            Operation::Move(width) => {
                if matches!(operands.get(1), Some(super::ir::Operand::Immediate(_))) {
                    Self::Pseudo
                } else {
                    Self::MoveRegister(width)
                }
            }
            Operation::FloatMove => {
                let is_floating_point = |operand: &super::ir::Operand| match operand {
                    super::ir::Operand::VirtualRegister(register) => {
                        register.class() == crate::types::RegisterClass::FloatingPoint
                    }
                    super::ir::Operand::PhysicalRegister(register) => {
                        register.class() == crate::target::registers::RegisterClass::FloatingPoint
                    }
                    _ => false,
                };
                if operands.iter().any(is_floating_point) {
                    Self::MoveFloatBits64
                } else {
                    Self::MoveRegister(IntegerWidth::U64)
                }
            }
            Operation::Float(operation) => match operation {
                FloatingPointOperation::Binary(operation) => Self::FloatArithmetic(operation),
                FloatingPointOperation::Unary(operation) => Self::FloatUnary(operation),
                FloatingPointOperation::Convert(IntrinsicFloatConversion::Int32ToFloat64) => {
                    Self::FloatConversion(FloatConversion::Signed32ToDouble)
                }
                FloatingPointOperation::Convert(IntrinsicFloatConversion::Uint32ToFloat64) => {
                    Self::FloatConversion(FloatConversion::Signed64ToDouble)
                }
                FloatingPointOperation::Convert(IntrinsicFloatConversion::Float32ToFloat64) => {
                    Self::FloatConversion(FloatConversion::FloatToDouble)
                }
                FloatingPointOperation::Convert(IntrinsicFloatConversion::Float64ToFloat32) => {
                    Self::FloatConversion(FloatConversion::DoubleToFloat)
                }
                FloatingPointOperation::Convert(
                    IntrinsicFloatConversion::Float64ToInt32 | IntrinsicFloatConversion::JavaScriptToInt32,
                )
                | FloatingPointOperation::CanonicalizeNan => Self::Pseudo,
            },
            _ => Self::select(operation),
        }
    }

    pub(crate) fn select(operation: super::description::Operation) -> Self {
        match operation {
            Operation::Label => Self::Label,
            Operation::Control(crate::intrinsic::ControlOperation::JumpLabel) => Self::Branch,
            Operation::Control(crate::intrinsic::ControlOperation::Exit) => Self::BranchToExit,
            Operation::UnboxInt32 => Self::SignExtend32To64,
            Operation::Negate => Self::Negate64,
            Operation::Not(width) => Self::BitwiseNot(width),
            Operation::Memory(MemoryOperation::Load { .. } | MemoryOperation::Store(_))
            | Operation::Move(_)
            | Operation::IntegerBinary { .. } => {
                unreachable!("operand-sensitive operation must use select_for_operands")
            }
            _ => Self::Pseudo,
        }
    }

    pub(crate) fn is_pseudo(self) -> bool {
        matches!(
            self,
            Self::Pseudo | Self::AddSubtractZero(_) | Self::AddSubtractLargeImmediate { .. }
        )
    }
}

fn runtime(program: &Program) -> &RuntimeConstants {
    &program.runtime
}

const AARCH64_FRAME_SIZE: u32 = 112;
const AARCH64_COFF_FRAME_SIZE: u32 = 128;
pub(crate) const COFF_RAW_NATIVE_RETURN_SLOT: i64 = 112;
pub(crate) const COFF_RAW_NATIVE_VARIANT_SLOT: i64 = 120;

/// Emit `adrp`+`add` for a symbol address, using platform-appropriate relocations.
fn emit_symbol_addr(out: &mut String, dst: &str, symbol: &str, fmt: ObjectFormat) {
    match fmt {
        ObjectFormat::MachO => {
            w!(out, "    adrp {dst}, {symbol}@PAGE");
            w!(out, "    add {dst}, {dst}, {symbol}@PAGEOFF");
        }
        ObjectFormat::Elf => {
            w!(out, "    adrp {dst}, {symbol}");
            w!(out, "    add {dst}, {dst}, :lo12:{symbol}");
        }
        ObjectFormat::Coff => {
            w!(out, "    adrp {dst}, {symbol}");
            w!(out, "    add {dst}, {dst}, :lo12:{symbol}");
        }
    }
}

fn frame_size_for_format(fmt: ObjectFormat) -> u32 {
    if matches!(fmt, ObjectFormat::Coff) {
        AARCH64_COFF_FRAME_SIZE
    } else {
        AARCH64_FRAME_SIZE
    }
}

fn cfi_offset(fmt: ObjectFormat, stack_offset: u32) -> i32 {
    stack_offset as i32 - frame_size_for_format(fmt) as i32
}

const SAVED_REGISTER_PAIRS: [(&str, &str, u32); 5] = [
    ("x25", "x26", 16),
    ("x27", "x28", 32),
    ("x19", "x20", 48),
    ("x21", "x22", 64),
    ("x23", "x24", 80),
];

#[derive(Clone, Copy)]
enum FrameAction {
    Save,
    Restore,
}

fn emit_saved_register_pairs(out: &mut String, fmt: ObjectFormat, action: FrameAction) {
    for (first, second, offset) in SAVED_REGISTER_PAIRS {
        let saving = matches!(action, FrameAction::Save);
        let mnemonic = if saving { "stp" } else { "ldp" };
        w!(out, "    {mnemonic} {first}, {second}, [sp, #{offset}]");
        if fmt == ObjectFormat::Coff {
            w!(out, "    .seh_save_regp {first}, {offset}");
        } else if saving {
            w!(out, "    .cfi_offset {first}, {}", cfi_offset(fmt, offset));
            w!(out, "    .cfi_offset {second}, {}", cfi_offset(fmt, offset + 8));
        } else if fmt == ObjectFormat::Elf {
            w!(out, "    .cfi_restore {first}");
            w!(out, "    .cfi_restore {second}");
        }
    }
}

fn emit_handler_alignment(out: &mut String, fmt: ObjectFormat) {
    // LLVM's ARM64 Windows SEH emitter needs the exact function length before
    // alignments are resolved, so keep align directives out of .seh_proc bodies.
    if !matches!(fmt, ObjectFormat::Coff) {
        w!(out, ".p2align 4");
    }
}

pub(crate) fn generate(program: &Program, options: &CompileOptions) -> String {
    assert_eq!(program.target, options.target);
    assert_eq!(program.target.architecture, Architecture::Aarch64);
    let mut out = String::new();
    let object_format = program.target.object_format;

    emit_file_header(&mut out, "//", false);

    emit_program(
        out,
        program,
        "    ",
        |out| generate_entry_point(out, program, object_format),
        |out| generate_fallback_handler(out, program, object_format),
        |out| {
            emit_handlers(
                out,
                program,
                "//",
                |out, _| emit_handler_alignment(out, object_format),
                emit_instruction,
                |out| {
                    w!(out, ".Lexit_veneer:");
                    w!(out, "    b .Lexit");
                },
            )
        },
        |out| generate_exit_point(out, object_format),
    )
}

fn generate_exit_point(out: &mut String, fmt: ObjectFormat) {
    // Shared exit path: restore callee-saved registers and return.
    // Keep this at the end of the proc so its CFI state does not affect
    // later handler PCs. Mach-O's assembler rejects epilogue CFI directives,
    // so only emit those for ELF.
    let frame_size = frame_size_for_format(fmt);
    w!(out, ".Lexit:");
    if matches!(fmt, ObjectFormat::Coff) {
        w!(out, "    .seh_startepilogue");
    }
    emit_saved_register_pairs(out, fmt, FrameAction::Restore);
    w!(out, "    ldr d8, [sp, #96]");
    if matches!(fmt, ObjectFormat::Elf) {
        w!(out, "    .cfi_restore d8");
    } else if matches!(fmt, ObjectFormat::Coff) {
        w!(out, "    .seh_save_freg d8, 96");
    }
    w!(out, "    ldp x29, x30, [sp], #{frame_size}");
    if matches!(fmt, ObjectFormat::Elf) {
        w!(out, "    .cfi_restore x29");
        w!(out, "    .cfi_restore x30");
        w!(out, "    .cfi_def_cfa sp, 0");
    } else if matches!(fmt, ObjectFormat::Coff) {
        w!(out, "    .seh_save_fplr_x {frame_size}");
        w!(out, "    .seh_endepilogue");
    }
    w!(out, "    ret");
    w!(out);
}

fn generate_entry_point(out: &mut String, program: &Program, fmt: ObjectFormat) {
    // void js_interpreter(u8 const* bytecode, u32 entry_point, Value* values, VM* vm)
    // AAPCS64: x0=bytecode, w1=entry_point, x2=values, x3=vm

    // Save callee-saved registers and link register.
    // Pinned: x19(dispatch), x20(vm), x21(ip), x26(pb), x27(values), x28(exec_ctx)
    // x21 = ip (instruction pointer = pb + pc), the primary dispatch register.
    // x25 is only used when DSL code writes to pc directly (rare).
    // x22 = INT32_TAG, x23 = BOOLEAN_TAG, x24 = NAN_BASE_TAG (pinned constants).
    // d8 is pinned to hold CANON_NAN_BITS (callee-saved FP register).
    let frame_size = frame_size_for_format(fmt);
    w!(out, "    stp x29, x30, [sp, #-{frame_size}]!");
    if matches!(fmt, ObjectFormat::Coff) {
        w!(out, "    .seh_save_fplr_x {frame_size}");
    } else {
        w!(out, "    .cfi_def_cfa_offset {frame_size}");
        w!(out, "    .cfi_offset x29, {}", cfi_offset(fmt, 0));
        w!(out, "    .cfi_offset x30, {}", cfi_offset(fmt, 8));
    }
    w!(out, "    mov x29, sp");
    if matches!(fmt, ObjectFormat::Coff) {
        w!(out, "    .seh_set_fp");
    } else {
        w!(out, "    .cfi_def_cfa_register x29");
    }
    emit_saved_register_pairs(out, fmt, FrameAction::Save);
    w!(out, "    str d8, [sp, #96]");
    if matches!(fmt, ObjectFormat::Coff) {
        w!(out, "    .seh_save_freg d8, 96");
        w!(out, "    .seh_endprologue");
    } else {
        w!(out, "    .cfi_offset d8, {}", cfi_offset(fmt, 96));
    }

    // Set up pinned registers
    // x0=bytecode (pb), w1=entry_point (pc), x2=values, x3=vm
    let runtime = runtime(program);
    let interp_ctx = runtime[KnownLayoutConstant::VmRunningExecutionContext];
    let canon_nan = runtime[KnownLayoutConstant::CanonicalNanBits];
    w!(out, "    mov x26, x0              // pb = bytecode base");
    w!(out, "    mov x27, x2              // values = values array");
    // Store VM* in x20 (callee-saved) for C++ calls, pin exec_ctx in x28
    w!(out, "    mov x20, x3              // vm = VM*");
    emit_ldr64(out, "x28", "x3", interp_ctx);
    w!(out, "    // x28 = exec_ctx");
    let vm_breakpoint_controller = runtime[KnownLayoutConstant::VmBreakpointController];
    emit_ldr64(out, "x9", "x3", vm_breakpoint_controller);
    w!(out, "    cbz x9, .Lnormal_dispatch_table");
    emit_symbol_addr(out, "x19", "asm_debug_dispatch_table", fmt);
    w!(out, "    b .Ldispatch_table_ready");
    w!(out, ".Lnormal_dispatch_table:");
    emit_symbol_addr(out, "x19", "asm_dispatch_table", fmt);
    w!(out, ".Ldispatch_table_ready:");
    w!(out, "    // x19 = dispatch table");
    // Pin canonical NaN bits in d8 (callee-saved FP register).
    // Used by canonicalize_nan to avoid materializing the constant each time.
    emit_mov_imm(out, registers::X9, canon_nan);
    w!(out, "    fmov d8, x9              // d8 = CANON_NAN_BITS");
    // Pin frequently-compared tag constants in callee-saved registers.
    let int32_tag = runtime[KnownLayoutConstant::Int32Tag];
    let boolean_tag = runtime[KnownLayoutConstant::BooleanTag];
    let nan_base_tag = runtime[KnownLayoutConstant::NanBaseTag];
    emit_mov_imm(out, registers::X22, int32_tag);
    w!(out, "    // x22 = INT32_TAG");
    emit_mov_imm(out, registers::X23, boolean_tag);
    w!(out, "    // x23 = BOOLEAN_TAG");
    emit_mov_imm(out, registers::X24, nan_base_tag);
    w!(out, "    // x24 = NAN_BASE_TAG");

    // Dispatch to first instruction (x21 = pb + entry_point)
    w!(out, "    add x21, x26, w1, uxtw   // x21 = pb + entry_point");
    w!(out, "    ldrb w9, [x21]           // w9 = opcode byte");
    w!(out, "    ldr x10, [x19, x9, lsl #3]");
    w!(out, "    br x10");
    w!(out);
}

fn generate_fallback_handler(out: &mut String, program: &Program, object_format: ObjectFormat) {
    emit_handler_alignment(out, object_format);
    w!(out, "asm_debugger_trampoline:");
    w!(out, "    mov x0, x20");
    w!(out, "    sub w1, w21, w26");
    emit_sync_pc_to_execution_context(out, program);
    w!(out, "    bl CSYM(asm_debugger_check_breakpoint)");
    emit_state_reload(out, program);
    // The reload may have picked up a different bytecode base, which would leave the absolute
    // instruction pointer in x21 stale, so recompute it from the synced program counter.
    let program_counter = runtime(program)[KnownLayoutConstant::ExecutionContextProgramCounter];
    emit_ldr32(out, "w9", "x28", program_counter);
    w!(out, "    add x21, x26, w9, uxtw");
    w!(out, "    ldrb w9, [x21]");
    emit_symbol_addr(out, "x10", "asm_dispatch_table", object_format);
    w!(out, "    ldr x10, [x10, x9, lsl #3]");
    w!(out, "    br x10");
    w!(out);

    emit_handler_alignment(out, object_format);
    w!(out, "asm_handler_fallback:");
    // Set up args: x0=vm (x20), w1=pc (ip - pb), x2=instruction (ip)
    w!(out, "    mov x0, x20");
    w!(out, "    sub w1, w21, w26");
    emit_sync_pc_to_execution_context(out, program);
    w!(out, "    mov x2, x21");
    w!(out, "    bl CSYM(asm_fallback_handler)");
    // Check for exit (return < 0)
    emit_branch_bit_set_to_exit(out, registers::X0, 63);
    // Reload exec_ctx, pb, and values
    emit_state_reload(out, program);
    // w0 = new pc (32-bit), x26 = new pb; compute x21 = pb + pc
    w!(out, "    add x21, x26, w0, uxtw");
    emit_dispatch_from_ip(out);
    w!(out, "asm_handler_fallback_hot_end:");
    w!(out, "asm_handler_fallback_cold:");
    w!(out, "asm_handler_fallback_cold_end:");
    w!(out);

    // Shared exit path is emitted at the end of the proc.
    w!(out);
}

/// Emit instructions to reload exec_ctx (x28), pb (x26), and values (x27)
/// from the VM* in x20. Uses x9 as scratch.
fn emit_state_reload(out: &mut String, program: &Program) {
    let runtime = runtime(program);
    let interp_ctx = runtime[KnownLayoutConstant::VmRunningExecutionContext];
    let exec_executable = runtime[KnownLayoutConstant::ExecutionContextExecutable];
    let exec_bytecode = runtime[KnownLayoutConstant::ExecutableBytecodeData];
    let sizeof_execctx = runtime[KnownLayoutConstant::SizeOfExecutionContext];
    emit_ldr64(out, "x28", "x20", interp_ctx);
    emit_ldr64(out, "x9", "x28", exec_executable);
    emit_ldr64(out, "x26", "x9", exec_bytecode);
    emit_add_imm(out, "x27", "x28", sizeof_execctx);
}

fn emit_sync_pc_to_execution_context(out: &mut String, program: &Program) {
    let program_counter = runtime(program)[KnownLayoutConstant::ExecutionContextProgramCounter];
    emit_str32(out, "w1", "x28", program_counter);
}

/// Emit a dispatch sequence from x21 (already pointing at the next instruction).
fn emit_dispatch_from_ip(out: &mut String) {
    w!(out, "    ldrb w9, [x21]"); // w9 = opcode
    emit_dispatch_tail(out);
}

/// Emit the tail of a dispatch: given opcode in w9, look up handler and branch.
fn emit_dispatch_tail(out: &mut String) {
    w!(out, "    ldr x10, [x19, x9, lsl #3]");
    w!(out, "    br x10");
}

fn format_machine_memory_address(address: &MachineMemoryAddress, addressing: MemoryAddressing) -> String {
    match addressing {
        MemoryAddressing::Base => {
            let (None, None, None) = (address.index, address.scale, address.displacement) else {
                unreachable!("finalized AArch64 base address has extra components")
            };
            format!("[{}]", address.base)
        }
        MemoryAddressing::UnsignedImmediate | MemoryAddressing::UnscaledImmediate => {
            let (None, None, Some(offset)) = (address.index, address.scale, address.displacement) else {
                unreachable!("finalized AArch64 immediate address has invalid components")
            };
            format!("[{}, #{offset}]", address.base)
        }
        MemoryAddressing::Register => {
            let (Some(index), None, None) = (address.index, address.scale, address.displacement) else {
                unreachable!("finalized AArch64 register address has invalid components")
            };
            format!("[{}, {index}]", address.base)
        }
        MemoryAddressing::ShiftedRegister(shift) => {
            let (Some(index), None, None) = (address.index, address.scale, address.displacement) else {
                unreachable!("finalized AArch64 shifted address has invalid components")
            };
            format!("[{}, {index}, lsl #{}]", address.base, shift.amount())
        }
    }
}

fn format_pair_memory_address(address: &MachineMemoryAddress) -> String {
    let (None, None) = (address.index, address.scale) else {
        unreachable!("finalized AArch64 pair address must use a base and offset")
    };
    match address.displacement {
        Some(offset) => format!("[{}, #{offset}]", address.base),
        None => format!("[{}]", address.base),
    }
}

/// Emit an ldr (64-bit) from [base + offset].
fn emit_ldr64(out: &mut String, dst: &str, base: &str, offset: i64) {
    if offset == 0 {
        w!(out, "    ldr {dst}, [{base}]");
    } else if unsigned_memory_offset_fits(MemoryWidth::DoubleWord, offset) {
        w!(out, "    ldr {dst}, [{base}, #{offset}]");
    } else if (-256..=255).contains(&offset) {
        w!(out, "    ldur {dst}, [{base}, #{offset}]");
    } else {
        // Need to materialize offset in a scratch register
        emit_mov_imm(out, registers::X9, offset);
        w!(out, "    ldr {dst}, [{base}, x9]");
    }
}

/// Emit an ldr (32-bit) from [base + offset].
fn emit_ldr32(out: &mut String, dst: &str, base: &str, offset: i64) {
    if offset == 0 {
        w!(out, "    ldr {dst}, [{base}]");
    } else if unsigned_memory_offset_fits(MemoryWidth::Word, offset) {
        w!(out, "    ldr {dst}, [{base}, #{offset}]");
    } else if (-256..=255).contains(&offset) {
        w!(out, "    ldur {dst}, [{base}, #{offset}]");
    } else {
        emit_mov_imm(out, registers::X9, offset);
        w!(out, "    ldr {dst}, [{base}, x9]");
    }
}

/// Emit a str (32-bit) to [base + offset].
fn emit_str32(out: &mut String, src: &str, base: &str, offset: i64) {
    if offset == 0 {
        w!(out, "    str {src}, [{base}]");
    } else if unsigned_memory_offset_fits(MemoryWidth::Word, offset) {
        w!(out, "    str {src}, [{base}, #{offset}]");
    } else if (-256..=255).contains(&offset) {
        w!(out, "    stur {src}, [{base}, #{offset}]");
    } else {
        emit_mov_imm(out, registers::X9, offset);
        w!(out, "    str {src}, [{base}, x9]");
    }
}

fn emit_mov_imm(out: &mut String, destination: PhysicalRegister, value: i64) {
    let width = if value as u64 <= u32::MAX as u64 {
        IntegerWidth::U32
    } else {
        IntegerWidth::U64
    };
    for immediate in immediate_moves(value, width) {
        let mut operands = vec![
            Operand::PhysicalRegister(destination),
            Operand::Immediate(immediate.value),
        ];
        operands.extend(immediate.shift.map(Operand::Immediate));
        let instruction = MachineInstruction {
            opcode: super::ir::MachineOpcode::Aarch64(immediate.opcode),
            operands,
        };
        assert!(emit_simple_instruction(
            out,
            &instruction,
            immediate.opcode.simple_instruction(),
            |_| unreachable!("immediate move has no resolved operands"),
        ));
    }
}

/// Whether an immediate can be encoded in an AArch64 logical instruction of the given width.
pub(crate) fn is_logical_immediate(width: IntegerWidth, value: u64) -> bool {
    // A 32-bit logical immediate encodes exactly like its replication to 64 bits, and that
    // replication also rejects the element sizes a narrower operation cannot name.
    let value = match width {
        IntegerWidth::U64 => value,
        _ => {
            let value = value & 0xffff_ffff;
            value | (value << 32)
        }
    };
    is_replicated_rotated_run(value)
}

/// Whether a 64-bit value is a replicated, rotated run of ones, which is what the N/immr/imms
/// fields of a logical immediate can name.
fn is_replicated_rotated_run(value: u64) -> bool {
    if value == 0 || value == u64::MAX {
        return false;
    }
    for size in [2u32, 4, 8, 16, 32, 64] {
        let mask = if size == 64 { u64::MAX } else { (1u64 << size) - 1 };
        let element = value & mask;
        if (size..64)
            .step_by(size as usize)
            .any(|shift| ((value >> shift) & mask) != element)
        {
            continue;
        }
        let ones = element.count_ones();
        if ones == 0 || ones == size {
            continue;
        }
        // The encoding names a run of `ones` low bits rotated right, so try every rotation.
        let run = (1u64 << ones) - 1;
        let mut rotation = element;
        for _ in 0..size {
            if rotation == run {
                return true;
            }
            rotation = ((rotation >> 1) | ((rotation & 1) << (size - 1))) & mask;
        }
    }
    false
}

/// Emit add immediate. Handles large immediates by materializing in x9.
fn emit_add_imm(out: &mut String, dst: &str, src: &str, imm: i64) {
    if imm == 0 {
        if dst != src {
            w!(out, "    mov {dst}, {src}");
        }
    } else if imm > 0 && imm <= 4095 {
        w!(out, "    add {dst}, {src}, #{imm}");
    } else if imm > 0 && imm <= 0xFFF000 && imm & 0xFFF == 0 {
        let shifted = imm >> 12;
        w!(out, "    add {dst}, {src}, #{shifted}, lsl #12");
    } else if (-4095..0).contains(&imm) {
        let neg = -imm;
        w!(out, "    sub {dst}, {src}, #{neg}");
    } else {
        emit_mov_imm(out, registers::X9, imm);
        w!(out, "    add {dst}, {src}, x9");
    }
}

fn add_subtract_mnemonic(operation: AddSubtractOperation, flags: FlagUpdate) -> &'static str {
    match (operation, flags) {
        (AddSubtractOperation::Add, FlagUpdate::Preserve) => "add",
        (AddSubtractOperation::Add, FlagUpdate::Set) => "adds",
        (AddSubtractOperation::Subtract, FlagUpdate::Preserve) => "sub",
        (AddSubtractOperation::Subtract, FlagUpdate::Set) => "subs",
    }
}

fn shift_mnemonic(operation: ShiftOperation) -> &'static str {
    match operation {
        ShiftOperation::Left => "lsl",
        ShiftOperation::RightLogical => "lsr",
        ShiftOperation::RightArithmetic => "asr",
    }
}

fn emit_instruction(out: &mut String, insn: &MachineInstruction, handler: &Handler) {
    let opcode = insn.opcode.aarch64();
    debug_assert!(!opcode.is_pseudo());
    if emit_simple_instruction(out, insn, opcode.simple_instruction(), |operand| match operand {
        Operand::Label(_) => super::emitter::resolve_label(operand, handler),
        Operand::Address(address) => match opcode {
            Opcode::Load { addressing, .. } | Opcode::Store { addressing, .. } => {
                format_machine_memory_address(address, addressing)
            }
            Opcode::LoadPair(_) | Opcode::StorePair(_) => format_pair_memory_address(address),
            _ => unreachable!("AArch64 opcode has no resolved operand"),
        },
        _ => unreachable!("resolved AArch64 operand must be an address or label"),
    }) {
        return;
    }
    match opcode {
        Opcode::BranchBitSetToExit(bit) => {
            emit_branch_bit_set_to_exit(out, insn.physical_register(0), bit);
        }
        Opcode::Label => emit_label(out, insn, handler),
        _ => unreachable!("target pseudo reached the AArch64 machine printer"),
    }
}

fn emit_branch_bit_set_to_exit(out: &mut String, register: PhysicalRegister, bit: u8) {
    // A test-bit branch only has a +/-32 KiB range, while the shared exit point
    // follows all interpreter handlers. Branch to the veneer between the hot
    // and cold regions, which can reach the exit using a wider-range branch.
    w!(out, "    tbnz {register}, #{bit}, .Lexit_veneer");
}

#[cfg(test)]
mod tests {
    use super::super::description::{CallKind, EqualityCondition, IntegerSignedness, OrderedCondition};
    use super::*;
    use crate::low_ir::{
        AddressDisplacement, AddressRegister, Instruction as SourceInstruction, MemoryAddress as SourceMemoryAddress,
        Operand as SourceOperand,
    };
    use crate::low_ir::{Handler as SourceHandler, Program as SourceProgram};
    use crate::target::allocator::allocate_program;
    use crate::target::selection::select_program;

    fn test_options() -> CompileOptions {
        CompileOptions {
            target: crate::Target {
                architecture: Architecture::Aarch64,
                object_format: ObjectFormat::Coff,
            },
            has_jscvt: false,
            enable_assertions: false,
        }
    }

    fn opcode(operation: Operation) -> Operation {
        operation
    }

    fn generate(program: &SourceProgram) -> String {
        let selected = select_program(program.clone(), Architecture::Aarch64).unwrap();
        let allocated = allocate_program(selected).unwrap();
        let options = test_options();
        let machine = crate::target::finalize::finalize_program(allocated, &options).unwrap();
        super::generate(&machine, &options)
    }

    fn coff_program(instructions: Vec<SourceInstruction>) -> SourceProgram {
        SourceProgram {
            runtime: crate::low_ir::RuntimeConstants::from_layout(
                &crate::frontend::layout::LayoutConstants::from_values([
                    ("VM_RUNNING_EXECUTION_CONTEXT".into(), 8),
                    ("VM_BREAKPOINT_CONTROLLER".into(), 16),
                    ("CANON_NAN_BITS".into(), 0x7ff8_0000_0000_0000u64 as i64),
                    ("INT32_TAG".into(), 0xfffau64 as i64),
                    ("BOOLEAN_TAG".into(), 0xfffbu64 as i64),
                    ("NAN_BASE_TAG".into(), 0xfff8u64 as i64),
                    ("EXECUTION_CONTEXT_EXECUTABLE".into(), 16),
                    ("EXECUTION_CONTEXT_PROGRAM_COUNTER".into(), 20),
                    ("EXECUTABLE_BYTECODE_DATA".into(), 24),
                    ("SIZEOF_EXECUTION_CONTEXT".into(), 32),
                ]),
            ),
            handlers: vec![SourceHandler {
                id: crate::identity::HandlerId::new(0),
                name: "Call".into(),
                size: Some(1),
                is_cold: false,
                instructions,
            }],
            dispatch_handlers: vec![Some(crate::identity::HandlerId::new(0))],
        }
    }

    fn machine_coff_program(instructions: Vec<SourceInstruction>) -> Program {
        let selected = select_program(coff_program(instructions), Architecture::Aarch64).unwrap();
        let allocated = allocate_program(selected).unwrap();
        crate::target::finalize::finalize_program(allocated, &test_options()).unwrap()
    }

    #[test]
    fn coff_output_uses_windows_arm64_sections_relocations_and_unwind_directives() {
        let output = generate(&coff_program(Vec::new()));

        assert!(output.contains(".section .rdata,\"dr\""));
        assert!(output.contains(".globl CSYM(js_interpreter_handler_ranges)"));
        assert!(output.contains("CSYM(js_interpreter_handler_ranges):\n    .quad asm_handler_Call\n    .quad asm_handler_Call_hot_end\n    .quad asm_handler_Call_cold\n    .quad asm_handler_Call_cold_end"));
        assert!(output.contains("asm_handler_Call_hot_end:"));
        assert!(output.contains("asm_handler_Call_cold:\nasm_handler_Call_cold_end:"));
        assert!(output.contains("    .seh_proc CSYM(js_interpreter)"));
        assert!(output.contains("    .seh_save_fplr_x 128"));
        assert!(output.contains("    .seh_save_regp x25, 16"));
        assert!(output.contains("    .seh_save_freg d8, 96"));
        assert!(output.contains("    .seh_endprologue"));
        assert!(output.contains("    .seh_startepilogue"));
        assert!(output.contains("    .seh_endepilogue"));
        assert!(output.contains("    .seh_endproc"));
        assert!(output.contains("    adrp x19, asm_dispatch_table"));
        assert!(output.contains("    add x19, x19, :lo12:asm_dispatch_table"));
        assert!(!output.contains(".p2align 4\nasm_handler_fallback:"));
        assert!(!output.contains(".p2align 4\nasm_handler_Call:"));
        assert!(!output.contains(".cfi_"));
    }

    #[test]
    fn mov32_zero_extends_even_when_registers_alias() {
        let program = machine_coff_program(Vec::new());
        let handler = &program.functions[0];
        let instruction = MachineInstruction {
            opcode: super::super::ir::MachineOpcode::Aarch64(Opcode::MoveRegister(IntegerWidth::U32)),
            operands: vec![
                Operand::PhysicalRegister(crate::target::registers::aarch64::X0),
                Operand::PhysicalRegister(crate::target::registers::aarch64::X0),
            ],
        };
        let mut out = String::new();

        emit_instruction(&mut out, &instruction, handler);

        assert!(out.contains("mov w0, w0"));
    }

    #[test]
    fn bit_test_exit_branch_uses_the_shared_veneer() {
        let program = machine_coff_program(Vec::new());
        let handler = &program.functions[0];
        let instruction = MachineInstruction {
            opcode: super::super::ir::MachineOpcode::Aarch64(Opcode::BranchBitSetToExit(63)),
            operands: vec![Operand::PhysicalRegister(crate::target::registers::aarch64::X0)],
        };
        let mut out = String::new();

        emit_instruction(&mut out, &instruction, handler);

        assert_eq!(out, "    tbnz x0, #63, .Lexit_veneer\n");
    }

    #[test]
    fn emits_one_exit_veneer_between_hot_and_cold_handlers() {
        let output = generate(&coff_program(Vec::new()));

        assert_eq!(output.matches(".Lexit_veneer:").count(), 1);
        assert!(output.contains("    tbnz x0, #63, .Lexit_veneer"));
        assert!(output.contains(".Lexit_veneer:\n    b .Lexit\n"));
        assert!(output.find(".Lexit_veneer:") < output.find("// Cold handler paths"));
    }

    #[test]
    fn logical_immediates_are_encodable_at_the_operation_width() {
        // A 32-bit operation names a pattern replicated across 32 bits, so all-ones is not one.
        assert!(is_logical_immediate(IntegerWidth::U64, 0xffff_ffff));
        assert!(!is_logical_immediate(IntegerWidth::U32, 0xffff_ffff));
        // A run of ones that wraps around its element is still a rotated run.
        assert!(is_logical_immediate(IntegerWidth::U64, 0x9999_9999_9999_9999));
        assert!(is_logical_immediate(IntegerWidth::U32, 0x9999_9999));
        assert!(!is_logical_immediate(IntegerWidth::U64, 0b1001));
        assert!(is_logical_immediate(IntegerWidth::U64, 0xffff));
        assert!(!is_logical_immediate(IntegerWidth::U64, 0));
        assert!(!is_logical_immediate(IntegerWidth::U64, u64::MAX));
    }

    #[test]
    fn reading_the_program_counter_subtracts_the_bytecode_base() {
        let output = generate(&coff_program(vec![SourceInstruction {
            opcode: opcode(Operation::Move(IntegerWidth::U64)),
            operands: vec![
                SourceOperand::PhysicalRegister(crate::target::registers::aarch64::X0),
                SourceOperand::InterpreterRegister(crate::types::InterpreterRegister::ProgramCounter),
            ],
        }]));

        assert!(output.contains("    sub w0, w21, w26"));
        assert!(!output.contains("    mov x0, x25"));
    }

    #[test]
    fn dispatching_on_an_assigned_program_counter_reads_it_back() {
        let program_counter = SourceOperand::InterpreterRegister(crate::types::InterpreterRegister::ProgramCounter);
        let output = generate(&coff_program(vec![
            SourceInstruction {
                opcode: opcode(Operation::Move(IntegerWidth::U32)),
                operands: vec![
                    program_counter.clone(),
                    SourceOperand::PhysicalRegister(crate::target::registers::aarch64::X0),
                ],
            },
            SourceInstruction {
                opcode: opcode(Operation::Control(
                    crate::target::description::ControlOperation::GotoHandler,
                )),
                operands: vec![program_counter],
            },
        ]));

        assert!(output.contains("    mov w25, w0"));
        assert!(output.contains("    add x21, x26, w25, uxtw"));
    }

    #[test]
    fn coff_raw_native_call_uses_windows_arm64_sret() {
        let output = generate(&coff_program(vec![SourceInstruction {
            opcode: opcode(Operation::Call(CallKind::RawNative)),
            operands: vec![
                SourceOperand::PhysicalRegister(crate::target::registers::aarch64::X8),
                SourceOperand::PhysicalRegister(crate::target::registers::aarch64::X0),
                SourceOperand::PhysicalRegister(crate::target::registers::aarch64::X1),
            ],
        }]));

        assert!(output.contains("    add x0, sp, #112"));
        assert!(output.contains("    mov x1, x20"));
        assert!(output.contains("    blr x9"));
        assert!(output.contains("    ldr x1, [sp, #120]"));
        assert!(output.contains("    ldr x0, [sp, #112]"));
    }

    #[test]
    fn byte_memory_zero_branch_uses_cbz() {
        let output = generate(&coff_program(vec![
            SourceInstruction {
                opcode: opcode(Operation::branch_equality(IntegerWidth::U8, EqualityCondition::Equal)),
                operands: vec![
                    SourceOperand::Address(SourceMemoryAddress {
                        base: AddressRegister::Physical(crate::target::registers::aarch64::X0),
                        index: None,
                        scale: None,
                        displacement: Some(AddressDisplacement::Immediate(4)),
                    }),
                    SourceOperand::Immediate(0),
                    SourceOperand::Label(".zero".into()),
                ],
            },
            SourceInstruction {
                opcode: opcode(Operation::Label),
                operands: vec![SourceOperand::Label(".zero".into())],
            },
        ]));

        assert!(output.contains("    ldrb w9, [x0, #4]"));
        assert!(output.contains("    cbz w9, .Lasm_Call.zero"));
    }

    #[test]
    fn emits_structured_memory_load() {
        let output = generate(&coff_program(vec![SourceInstruction {
            opcode: opcode(Operation::load(MemoryWidth::DoubleWord, false)),
            operands: vec![
                SourceOperand::PhysicalRegister(crate::target::registers::aarch64::X0),
                SourceOperand::Address(SourceMemoryAddress {
                    base: AddressRegister::Physical(crate::target::registers::aarch64::X1),
                    index: None,
                    scale: None,
                    displacement: Some(AddressDisplacement::Immediate(16)),
                }),
            ],
        }]));

        assert!(output.contains("    ldr x0, [x1, #16]"));
    }

    #[test]
    fn compares_full_range_u32_immediates_at_32_bit_width() {
        let output = generate(&coff_program(vec![
            SourceInstruction {
                opcode: opcode(Operation::branch_equality(IntegerWidth::U32, EqualityCondition::Equal)),
                operands: vec![
                    SourceOperand::PhysicalRegister(crate::target::registers::aarch64::X0),
                    SourceOperand::Immediate(0xffff_fffe),
                    SourceOperand::Label(".failure".into()),
                ],
            },
            SourceInstruction {
                opcode: opcode(Operation::Label),
                operands: vec![SourceOperand::Label(".failure".into())],
            },
        ]));

        assert!(output.contains("    movn w9, #1"));
        assert!(output.contains("    cmp w0, w9"));
        assert!(!output.contains("    cmp w0, x9"));
    }

    #[test]
    fn lowers_narrow_signed_branches_at_32_bit_width() {
        let output = generate(&coff_program(vec![
            SourceInstruction {
                opcode: opcode(Operation::branch_ordered(
                    IntegerWidth::U32,
                    OrderedCondition::Less,
                    IntegerSignedness::Signed,
                )),
                operands: vec![
                    SourceOperand::PhysicalRegister(crate::target::registers::aarch64::X0),
                    SourceOperand::PhysicalRegister(crate::target::registers::aarch64::X1),
                    SourceOperand::Label(".failure".into()),
                ],
            },
            SourceInstruction {
                opcode: opcode(Operation::Label),
                operands: vec![SourceOperand::Label(".failure".into())],
            },
        ]));

        assert!(output.contains("    cmp w0, w1"));
        assert!(output.contains("    b.lt .Lasm_Call.failure"));
    }

    #[test]
    fn emits_finalized_assertion_conditions() {
        let program = machine_coff_program(Vec::new());
        let handler = &program.functions[0];
        let cases = [
            (Condition::NotEqual, "    b.ne "),
            (Condition::UnsignedGreaterOrEqual, "    b.hs "),
            (Condition::Equal, "    b.eq "),
        ];

        for (condition, expected) in cases {
            let instruction = MachineInstruction {
                opcode: super::super::ir::MachineOpcode::Aarch64(Opcode::BranchCondition(condition)),
                operands: vec![Operand::Label(".assert_ok".into())],
            };
            let mut out = String::new();
            emit_instruction(&mut out, &instruction, handler);
            assert!(out.contains(expected), "{condition:?} emitted:\n{out}");
        }
    }

    #[test]
    fn emits_cold_handler_after_hot_region() {
        let mut program = coff_program(vec![SourceInstruction {
            opcode: opcode(Operation::Call(CallKind::SlowPath)),
            operands: vec![SourceOperand::Relocation(crate::low_ir::Relocation::function_call(
                crate::identity::ExternalSymbol::new("slow_path"),
            ))],
        }]);
        program.handlers[0].is_cold = true;

        let output = generate(&program);

        assert!(output.find("asm_cold_handler_paths:").unwrap() < output.find("asm_handler_Call:").unwrap());
    }
}
