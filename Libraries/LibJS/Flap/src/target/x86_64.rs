/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::{Architecture, CompileOptions, ObjectFormat};
use crate::frontend::layout::KnownLayoutConstant;
use super::description::{BinaryOperation, FloatBinaryOperation, FloatConversion as IntrinsicFloatConversion, FloatUnaryOperation, FloatingPointOperation, IntegerBinaryOperation, IntegerWidth, MemoryOperation, MemoryWidth, Operation, ShiftOperation, SignCondition};
use super::ir::{
    MachineFunction as Handler, MachineInstruction, MachineProgram as Program,
    MachineCondition as Condition, MachineMemoryAddress,
    MachineOperand as Operand, RuntimeConstants,
};
use super::machine_verify::{
    define_machine_opcodes, operands_match,
};
use super::description::OperandKind::{
    FprIn as FR, FuncSymbol as F, GprIn as R, GprInOrImm as RI,
    Imm as I, Label as L, Memory as A, RegisterIn as AR,
};
use super::registers::x86_64 as registers;
use super::emitter::{
    SimpleInstruction, SimpleOperand, emit_file_header, emit_handlers,
    emit_label, emit_program, emit_simple_instruction, integer, native,
    simple, w, word,
};
#[cfg(test)]
use super::emitter::emit_handler_range;
use std::fmt::Write;

pub(crate) mod finalize;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum AluOperation {
    Add,
    Subtract,
    And,
    Or,
    Xor,
}

impl AluOperation {
    pub(crate) fn from_binary(operation: BinaryOperation) -> Option<Self> {
        match operation {
            BinaryOperation::Add => Some(Self::Add),
            BinaryOperation::Subtract => Some(Self::Subtract),
            BinaryOperation::And => Some(Self::And),
            BinaryOperation::Or => Some(Self::Or),
            BinaryOperation::Xor => Some(Self::Xor),
            BinaryOperation::Multiply => None,
        }
    }

    fn mnemonic(self) -> &'static str {
        match self {
            Self::Add => "add",
            Self::Subtract => "sub",
            Self::And => "and",
            Self::Or => "or",
            Self::Xor => "xor",
        }
    }
}

pub(crate) fn alu_immediate_fits(width: IntegerWidth, value: i64) -> bool {
    match width {
        IntegerWidth::U64 => i32::try_from(value).is_ok(),
        IntegerWidth::U32 => {
            (i32::MIN as i64..=u32::MAX as i64).contains(&value)
        }
        IntegerWidth::U16 => {
            (i16::MIN as i64..=u16::MAX as i64).contains(&value)
        }
        IntegerWidth::U8 => false,
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum FloatConversion {
    Signed64ToDouble,
    FloatToDouble,
    DoubleToFloat,
    DoubleToSigned32Truncate,
    DoubleToSigned64Truncate,
}

fn load_instruction(width: MemoryWidth, signed: bool) -> SimpleInstruction {
    match (width, signed) {
        (MemoryWidth::DoubleWord, false) => {
            simple!("mov"; native(0), SimpleOperand::Resolved(1, "QWORD PTR "))
        }
        (MemoryWidth::Float, false) => {
            simple!("movss"; native(0), SimpleOperand::Resolved(1, "DWORD PTR "))
        }
        (MemoryWidth::Word, false) => {
            simple!("mov"; word(0), SimpleOperand::Resolved(1, "DWORD PTR "))
        }
        (MemoryWidth::HalfWord, false) => {
            simple!("movzx"; word(0), SimpleOperand::Resolved(1, "WORD PTR "))
        }
        (MemoryWidth::Byte, false) => {
            simple!("movzx"; word(0), SimpleOperand::Resolved(1, "BYTE PTR "))
        }
        (MemoryWidth::HalfWord, true) => {
            simple!("movsx"; word(0), SimpleOperand::Resolved(1, "WORD PTR "))
        }
        (MemoryWidth::Byte, true) => {
            simple!("movsx"; word(0), SimpleOperand::Resolved(1, "BYTE PTR "))
        }
        (MemoryWidth::Word | MemoryWidth::DoubleWord | MemoryWidth::Float, true) => {
            unreachable!("unsupported finalized x86-64 signed load")
        }
    }
}

fn store_instruction(width: MemoryWidth) -> SimpleInstruction {
    match width {
        MemoryWidth::Byte => simple!(
            "mov";
            SimpleOperand::Resolved(0, "BYTE PTR "),
            SimpleOperand::ResolvedInteger(1, IntegerWidth::U8)
        ),
        MemoryWidth::HalfWord => simple!(
            "mov";
            SimpleOperand::Resolved(0, "WORD PTR "),
            SimpleOperand::ResolvedInteger(1, IntegerWidth::U16)
        ),
        MemoryWidth::Word => simple!(
            "mov";
            SimpleOperand::Resolved(0, "DWORD PTR "),
            SimpleOperand::ResolvedInteger(1, IntegerWidth::U32)
        ),
        MemoryWidth::DoubleWord => simple!(
            "mov";
            SimpleOperand::Resolved(0, "QWORD PTR "),
            SimpleOperand::ResolvedInteger(1, IntegerWidth::U64)
        ),
        MemoryWidth::Float => {
            simple!("movss"; SimpleOperand::Resolved(0, "DWORD PTR "), native(1))
        }
    }
}

define_machine_opcodes! {
    Operand, operands;
    encoding (immediate, shift_fits) {}
    [Pseudo] => Self::Pseudo => any;
    [Label] => Self::Label => [[L]];
    [Jump] => Self::Jump => [[L]] printing simple!("jmp"; SimpleOperand::Label(0));
    [JumpMemory] => Self::JumpMemory => [[A]] printing simple!("jmp"; SimpleOperand::Resolved(0, ""));
    [JumpToExit] => Self::JumpToExit => [[]] printing simple!("jmp"; SimpleOperand::Literal(".Lexit"));
    [JumpSign(SignCondition)] => Self::JumpSign(condition) => [[L]] printing simple!(condition.select("js", "jns"); SimpleOperand::Label(0));
    [CallDirect] => Self::CallDirect => [[F]] encoding {
        matches!(
            operands,
            [Operand::Relocation(relocation)]
                if relocation.kind() == crate::low_ir::RelocationKind::FunctionCall
        )
    } printing simple!("call"; SimpleOperand::Relocation(0));
    [CallRegister] => Self::CallRegister => [[R]] printing simple!("call"; native(0));
    [TestRegister(IntegerWidth)] => Self::TestRegister(width) => [[R]] printing simple!("test"; integer(0, width), integer(0, width));
    [JumpNegativeToExit] => Self::JumpNegativeToExit => [[]] printing simple!("js"; SimpleOperand::Literal(".Lexit"));
    [LoadEffectiveAddress] => Self::LoadEffectiveAddress => [[R, A]] printing simple!("lea"; native(0), SimpleOperand::Resolved(1, ""));
    [LoadValuesAddress] => Self::LoadValuesAddress => [[R, A]];
    [Load { width: MemoryWidth, signed: bool }] => opcode @ Self::Load { width, signed } => {
        match opcode {
            Self::Load { width: MemoryWidth::Float, .. } => operands_match(operands, &[FR, A]),
            _ => operands_match(operands, &[R, A]),
        }
    } encoding {
        let Self::Load { width, signed } = opcode else { unreachable!() };
        !signed || matches!(width, MemoryWidth::Byte | MemoryWidth::HalfWord)
    } printing load_instruction(width, signed);
    [LoadExecutionContext { width: MemoryWidth, signed: bool }] => opcode @ Self::LoadExecutionContext { .. } => {
        match opcode {
            Self::LoadExecutionContext { width: MemoryWidth::Float, .. } => operands_match(operands, &[FR, A]),
            _ => operands_match(operands, &[R, A]),
        }
    };
    [LoadAdditiveDisplacement { width: MemoryWidth, signed: bool }] => opcode @ Self::LoadAdditiveDisplacement { width, signed } => {
        match opcode {
            Self::LoadAdditiveDisplacement { width: MemoryWidth::Float, .. } => operands_match(operands, &[FR, A]),
            _ => operands_match(operands, &[R, A]),
        }
    } encoding {
        let Self::LoadAdditiveDisplacement { width, signed } = opcode else { unreachable!() };
        !signed || matches!(width, MemoryWidth::Byte | MemoryWidth::HalfWord)
    } printing load_instruction(width, signed);
    [StoreLargeImmediate(MemoryWidth)] => Self::StoreLargeImmediate(_) => any;
    [Store(MemoryWidth)] => opcode @ Self::Store(width) => {
        match opcode {
            Self::Store(MemoryWidth::Float) => operands_match(operands, &[A, FR]),
            _ => operands_match(operands, &[A, RI]),
        }
    } printing store_instruction(width);
    [StoreAdditiveDisplacement(MemoryWidth)] => opcode @ Self::StoreAdditiveDisplacement(width) => {
        match opcode {
            Self::StoreAdditiveDisplacement(MemoryWidth::Float) => operands_match(operands, &[A, FR]),
            _ => operands_match(operands, &[A, RI]),
        }
    } printing store_instruction(width);
    [Increment32Memory] => Self::Increment32Memory => [[A]] printing simple!("inc"; SimpleOperand::Resolved(0, "DWORD PTR "));
    [SignExtend32To64] => Self::SignExtend32To64 => [[R, R]] printing simple!("movsxd"; native(0), integer(1, IntegerWidth::U32));
    [Move64Register] => Self::Move64Register => [[R, R]] printing simple!("mov"; native(0), native(1));
    [MoveExecutionContext] => Self::MoveExecutionContext => [[R, R]];
    [Move32Register] => Self::Move32Register => [[R, R]] printing simple!("mov"; integer(0, IntegerWidth::U32), integer(1, IntegerWidth::U32));
    [Move64Immediate] => Self::Move64Immediate => [[R, I]] printing simple!("mov"; native(0), SimpleOperand::Immediate(1));
    [Move64ImmediateHex] => Self::Move64ImmediateHex => [[R, I]] printing simple!("mov"; native(0), SimpleOperand::HexImmediate(1));
    [Move32Immediate] => Self::Move32Immediate => [[R, I]] encoding {
        immediate(1).is_some_and(|value| {
            (i32::MIN as i64..=u32::MAX as i64).contains(&value)
        })
    } printing simple!("mov"; integer(0, IntegerWidth::U32), SimpleOperand::Immediate(1));
    [MoveAbsolute64Immediate] => Self::MoveAbsolute64Immediate => [[R, I]] printing simple!("movabs"; native(0), SimpleOperand::Immediate(1));
    [Or64Register] => Self::Or64Register => [[R, R]] printing simple!("or"; native(0), native(1));
    [AluRegister { operation: AluOperation, width: IntegerWidth }] => Self::AluRegister { operation, width } => [[R, R]] printing simple!(operation.mnemonic(); integer(0, width), integer(1, width));
    [AluImmediate { operation: AluOperation, width: IntegerWidth }] => Self::AluImmediate { operation, width } => [[R, I]] encoding {
        immediate(1).is_some_and(|value| alu_immediate_fits(width, value))
    } printing simple!(operation.mnemonic(); integer(0, width), SimpleOperand::Immediate(1));
    [Multiply64Register] => Self::Multiply64Register => [[R, R]] printing simple!("imul"; native(0), native(1));
    [Multiply64Immediate] => Self::Multiply64Immediate => [[R, R, RI]] printing simple!("imul"; native(0), native(1), SimpleOperand::Resolved(2, ""));
    [Multiply32] => Self::Multiply32 => [[R, R]] printing simple!("imul"; integer(0, IntegerWidth::U32), integer(1, IntegerWidth::U32));
    [Increment32Register] => Self::Increment32Register => [[R]] printing simple!("inc"; integer(0, IntegerWidth::U32));
    [Decrement32Register] => Self::Decrement32Register => [[R]] printing simple!("dec"; integer(0, IntegerWidth::U32));
    [Negate32Register] => Self::Negate32Register => [[R]] printing simple!("neg"; integer(0, IntegerWidth::U32));
    [JumpOverflow] => Self::JumpOverflow => [[L]] printing simple!("jo"; SimpleOperand::Label(0));
    [CompareRegister(IntegerWidth)] => Self::CompareRegister(width) => [[R, R]] printing simple!("cmp"; integer(0, width), integer(1, width));
    [CompareImmediate(IntegerWidth)] => Self::CompareImmediate(width) => [[R, I]] encoding {
        immediate(1).is_some_and(|value| alu_immediate_fits(width, value))
    } printing simple!("cmp"; integer(0, width), SimpleOperand::Immediate(1));
    [CompareMemoryRegister(IntegerWidth)] => Self::CompareMemoryRegister(width) => [[A, R]] encoding {
        matches!(width, IntegerWidth::U32 | IntegerWidth::U64)
    } printing match width {
        IntegerWidth::U32 => simple!("cmp"; SimpleOperand::Resolved(0, "DWORD PTR "), integer(1, width)),
        IntegerWidth::U64 => simple!("cmp"; SimpleOperand::Resolved(0, "QWORD PTR "), integer(1, width)),
        IntegerWidth::U8 | IntegerWidth::U16 => unreachable!("unsupported finalized x86-64 memory comparison width"),
    };
    [CompareMemoryImmediate(IntegerWidth)] => Self::CompareMemoryImmediate(width) => [[A, I]] encoding {
        width == IntegerWidth::U8
            && immediate(1).is_some_and(|value| {
                (i8::MIN as i64..=u8::MAX as i64).contains(&value)
            })
    } printing match width {
        IntegerWidth::U8 => simple!("cmp"; SimpleOperand::Resolved(0, "BYTE PTR "), SimpleOperand::Immediate(1)),
        IntegerWidth::U16 | IntegerWidth::U32 | IntegerWidth::U64 => unreachable!("unsupported finalized x86-64 immediate memory comparison width"),
    };
    [Test64Registers] => Self::Test64Registers => [[R, R]] printing simple!("test"; native(0), native(1));
    [Test32Immediate] => Self::Test32Immediate => [[R, I]] encoding {
        immediate(1).is_some_and(|value| {
            (i32::MIN as i64..=u32::MAX as i64).contains(&value)
        })
    } printing simple!("test"; integer(0, IntegerWidth::U32), SimpleOperand::Immediate(1));
    [TestMemoryImmediate(MemoryWidth)] => Self::TestMemoryImmediate(width) => [[A, I]] encoding {
        width == MemoryWidth::Byte
            && immediate(1).is_some_and(|value| {
                (i8::MIN as i64..=u8::MAX as i64).contains(&value)
            })
    } printing match width {
        MemoryWidth::Byte => simple!("test"; SimpleOperand::Resolved(0, "BYTE PTR "), SimpleOperand::Immediate(1)),
        MemoryWidth::HalfWord | MemoryWidth::Word | MemoryWidth::DoubleWord | MemoryWidth::Float => unreachable!("unsupported finalized x86-64 immediate memory test width"),
    };
    [BitTest64Immediate] => Self::BitTest64Immediate => [[R, I]] encoding {
        immediate(1).is_some_and(|value| (0..64).contains(&value))
    } printing simple!("bt"; native(0), SimpleOperand::Immediate(1));
    [JumpCondition(Condition)] => Self::JumpCondition(condition) => [[L]] printing simple!(condition.x86_64_mnemonic(); SimpleOperand::Label(0));
    [UndefinedInstruction] => Self::UndefinedInstruction => [[]] printing simple!("ud2");
    [MoveFloatBits64] => Self::MoveFloatBits64 => [[AR, AR]] printing simple!("movq"; native(0), native(1));
    [CompareDouble] => Self::CompareDouble => [[FR, FR]] printing simple!("ucomisd"; native(0), native(1));
    [JumpParity] => Self::JumpParity => [[L]] printing simple!("jp"; SimpleOperand::Label(0));
    [FloatArithmetic(FloatBinaryOperation)] => Self::FloatArithmetic(operation) => [[FR, FR]] printing simple!(float_arithmetic_mnemonic(operation); native(0), native(1));
    [FloatUnary(FloatUnaryOperation)] => Self::FloatUnary(operation) => [[FR, FR]] printing match operation {
        FloatUnaryOperation::Floor => simple!("roundsd"; native(0), native(1), SimpleOperand::Value(1)),
        FloatUnaryOperation::Ceil => simple!("roundsd"; native(0), native(1), SimpleOperand::Value(2)),
        FloatUnaryOperation::SquareRoot => simple!("sqrtsd"; native(0), native(1)),
    };
    [FloatConversion(FloatConversion)] => Self::FloatConversion(conversion) => {
        match conversion {
            FloatConversion::Signed64ToDouble => operands_match(operands, &[FR, R]),
            FloatConversion::FloatToDouble | FloatConversion::DoubleToFloat => operands_match(operands, &[FR, FR]),
            FloatConversion::DoubleToSigned32Truncate | FloatConversion::DoubleToSigned64Truncate => operands_match(operands, &[R, FR]),
        }
    } printing match conversion {
        FloatConversion::Signed64ToDouble => simple!("cvtsi2sd"; native(0), native(1)),
        FloatConversion::FloatToDouble => simple!("cvtss2sd"; native(0), native(1)),
        FloatConversion::DoubleToFloat => simple!("cvtsd2ss"; native(0), native(1)),
        FloatConversion::DoubleToSigned32Truncate => simple!("cvttsd2si"; integer(0, IntegerWidth::U32), native(1)),
        FloatConversion::DoubleToSigned64Truncate => simple!("cvttsd2si"; native(0), native(1)),
    };
    [ShiftImmediate { operation: ShiftOperation, width: IntegerWidth }] => Self::ShiftImmediate { operation, width } => [[R, I]] encoding {
        immediate(1).is_some_and(|value| shift_fits(width, value))
    } printing simple!(shift_mnemonic(operation); integer(0, width), SimpleOperand::Immediate(1));
    [ShiftByCountRegister { operation: ShiftOperation, width: IntegerWidth }] => Self::ShiftByCountRegister { operation, width } => [[R]] encoding {
        matches!(width, IntegerWidth::U32 | IntegerWidth::U64)
    } printing simple!(shift_mnemonic(operation); integer(0, width), SimpleOperand::Literal("cl"));
    [Negate64] => Self::Negate64 => [[R]] printing simple!("neg"; native(0));
    [BitwiseNot(IntegerWidth)] => Self::BitwiseNot(width) => [[R]] encoding {
        matches!(width, IntegerWidth::U32 | IntegerWidth::U64)
    } printing simple!("not"; integer(0, width));
    [ComplementBit] => Self::ComplementBit => [[R, I]] encoding {
        immediate(1).is_some_and(|value| (0..64).contains(&value))
    } printing simple!("btc"; native(0), SimpleOperand::Immediate(1));
    [ResetBit] => Self::ResetBit => [[R, I]] encoding {
        immediate(1).is_some_and(|value| (0..64).contains(&value))
    } printing simple!("btr"; native(0), SimpleOperand::Immediate(1));
    [SignExtendRaxToRdx] => Self::SignExtendRaxToRdx => [[]] printing simple!("cqo");
    [SignedDivide64Register] => Self::SignedDivide64Register => [[R]] printing simple!("idiv"; native(0));
}

impl Opcode {
    pub(crate) fn select_for_operands(
        operation: super::description::Operation,
        operands: &[super::ir::Operand],
    ) -> Self {
        match operation {
            Operation::LoadEffectiveAddress
                if matches!(
                    operands.first(),
                    Some(super::ir::Operand::InterpreterRegister(
                        crate::types::InterpreterRegister::Values
                    ))
                ) =>
            {
                Self::LoadValuesAddress
            }
            Operation::Move(IntegerWidth::U64)
                if matches!(
                    operands.first(),
                    Some(super::ir::Operand::InterpreterRegister(
                        crate::types::InterpreterRegister::ExecutionContext
                    ))
                ) =>
            {
                Self::MoveExecutionContext
            }
            Operation::Memory(MemoryOperation::Load { width, signed, .. })
                if matches!(
                    operands.first(),
                    Some(super::ir::Operand::InterpreterRegister(
                        crate::types::InterpreterRegister::ExecutionContext
                    ))
                ) =>
            {
                Self::LoadExecutionContext { width, signed }
            }
            Operation::IntegerBinary {
                operation: IntegerBinaryOperation::Binary(operation),
                width,
            }
                if matches!(
                    operation,
                    BinaryOperation::Add | BinaryOperation::Subtract
                ) && width == IntegerWidth::U64 =>
            {
                let operation = AluOperation::from_binary(operation)
                    .expect("add and subtract have x86-64 ALU opcodes");
                if matches!(operands.get(1), Some(super::ir::Operand::Immediate(_))) {
                    Self::AluImmediate { operation, width }
                } else {
                    Self::AluRegister { operation, width }
                }
            }
            Operation::IntegerBinary { .. } => Self::Pseudo,
            Operation::Memory(MemoryOperation::Load { width, signed, .. }) => Self::Load { width, signed },
            Operation::Memory(MemoryOperation::Store(width)) => {
                if width == MemoryWidth::DoubleWord
                    && matches!(
                        operands.get(1),
                        Some(super::ir::Operand::Immediate(value))
                            if !(i32::MIN as i64..=i32::MAX as i64).contains(value)
                    )
                {
                    Self::StoreLargeImmediate(width)
                } else {
                    Self::Store(width)
                }
            }
            Operation::Move(IntegerWidth::U64) => {
                let Some(super::ir::Operand::Immediate(value)) =
                    operands.get(1)
                else {
                    return Self::Move64Register;
                };
                let unsigned_value = *value as u64;
                if unsigned_value <= 0x7fff_ffff
                    || (-0x8000_0000..0).contains(value)
                {
                    Self::Move64Immediate
                } else if unsigned_value <= 0xffff_ffff {
                    Self::Move32Immediate
                } else {
                    Self::MoveAbsolute64Immediate
                }
            }
            Operation::Move(IntegerWidth::U32) => {
                if matches!(operands.get(1), Some(super::ir::Operand::Immediate(_))) {
                    Self::Move32Immediate
                } else {
                    Self::Move32Register
                }
            }
            Operation::FloatMove => Self::MoveFloatBits64,
            Operation::Float(operation) => match operation {
                FloatingPointOperation::Binary(operation) => Self::FloatArithmetic(operation),
                FloatingPointOperation::Unary(operation) => Self::FloatUnary(operation),
                FloatingPointOperation::Convert(
                    IntrinsicFloatConversion::Int32ToFloat64 | IntrinsicFloatConversion::Uint32ToFloat64,
                ) => Self::FloatConversion(FloatConversion::Signed64ToDouble),
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
            Operation::Control(crate::intrinsic::ControlOperation::JumpLabel) => Self::Jump,
            Operation::Control(crate::intrinsic::ControlOperation::Exit) => Self::JumpToExit,
            Operation::LoadEffectiveAddress => Self::LoadEffectiveAddress,
            Operation::UnboxInt32 => Self::SignExtend32To64,
            Operation::Negate => Self::Negate64,
            Operation::Not(width) => Self::BitwiseNot(width),
            Operation::Increment32Memory => Self::Increment32Memory,
            Operation::Memory(MemoryOperation::Load { .. } | MemoryOperation::Store(_))
            | Operation::Move(_)
            | Operation::IntegerBinary { .. } => {
                unreachable!(
                    "operand-sensitive operation must use select_for_operands"
                )
            }
            _ => Self::Pseudo,
        }
    }

    pub(crate) fn is_pseudo(self) -> bool {
        matches!(
            self,
            Self::Pseudo
                | Self::StoreLargeImmediate(_)
                | Self::LoadExecutionContext { .. }
                | Self::LoadValuesAddress
                | Self::MoveExecutionContext
        )
    }
}

fn runtime(program: &Program) -> &RuntimeConstants {
    &program.runtime
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum X86_64Abi {
    SysV,
    Win64,
}

impl X86_64Abi {
    fn for_object_format(object_format: ObjectFormat) -> Self {
        match object_format {
            ObjectFormat::Coff => Self::Win64,
            ObjectFormat::Elf | ObjectFormat::MachO => Self::SysV,
        }
    }

    fn is_win64(self) -> bool {
        matches!(self, Self::Win64)
    }
}

const SYSV_SAVED_REGISTERS: [(&str, i32); 5] = [
    ("rbx", -24),
    ("r12", -32),
    ("r13", -40),
    ("r14", -48),
    ("r15", -56),
];
const WIN64_SAVED_REGISTERS: [&str; 7] = [
    "rbx", "rsi", "rdi", "r12", "r13", "r14", "r15",
];

fn emit_saved_registers(out: &mut String, abi: X86_64Abi) {
    if abi.is_win64() {
        for register in WIN64_SAVED_REGISTERS {
            w!(out, "    push {register}");
            w!(out, "    .seh_pushreg {register}");
        }
    } else {
        for (register, offset) in SYSV_SAVED_REGISTERS {
            w!(out, "    push {register}");
            w!(out, "    .cfi_offset {register}, {offset}");
        }
    }
}

fn emit_restored_registers(out: &mut String, fmt: ObjectFormat, abi: X86_64Abi) {
    let mut restore = |register| {
        w!(out, "    pop {register}");
        if fmt == ObjectFormat::Elf {
            w!(out, "    .cfi_restore {register}");
        }
    };
    if abi.is_win64() {
        WIN64_SAVED_REGISTERS.into_iter().rev().for_each(&mut restore);
    } else {
        SYSV_SAVED_REGISTERS
            .into_iter()
            .rev()
            .map(|(register, _)| register)
            .for_each(restore);
    }
}

const SYSV_VM_SLOT: &str = "[rbp - 48]";
const WIN64_VM_SLOT: &str = "[rbp - 64]";
pub(crate) const WIN64_RAW_NATIVE_RETURN_SLOT: i64 = -80;
pub(crate) const WIN64_RAW_NATIVE_VARIANT_SLOT: i64 = -72;

pub(crate) fn generate(program: &Program, options: &CompileOptions) -> String {
    assert_eq!(program.target, options.target);
    assert_eq!(program.target.architecture, Architecture::X86_64);
    let mut out = String::new();
    let object_format = program.target.object_format;
    let abi = X86_64Abi::for_object_format(object_format);

    emit_file_header(&mut out, "#", true);

    emit_program(
        out,
        program,
        "",
        |out| generate_entry_point(out, program, abi),
        |out| generate_fallback_handler(out, program, abi),
        |out| emit_handlers(
            out,
            program,
            "#",
            |out, cold| w!(out, "{}", if cold { ".p2align 4" } else { ".p2align 6" }),
            |out, instruction, handler| emit_instruction(out, instruction, handler, program),
        ),
        |out| generate_exit_point(out, object_format, abi),
    )
}

fn generate_exit_point(out: &mut String, fmt: ObjectFormat, abi: X86_64Abi) {
    // Shared exit path: restore callee-saved registers and return.
    // Keep this at the end of the proc so its CFI state does not affect
    // later handler PCs. Mach-O's assembler rejects epilogue CFI directives,
    // and x86_64 Windows unwinders recognize epilogues by instruction pattern,
    // so only emit explicit epilogue annotations for ELF.
    w!(out, ".Lexit:");
    if abi.is_win64() {
        w!(out, "    add rsp, 56");
    } else {
        w!(out, "    add rsp, 8");
    }
    emit_restored_registers(out, fmt, abi);
    w!(out, "    pop rbp");
    if matches!(fmt, ObjectFormat::Elf) {
        w!(out, "    .cfi_restore rbp");
        w!(out, "    .cfi_def_cfa rsp, 8");
    }
    w!(out, "    ret");
    w!(out);
}

fn generate_entry_point(out: &mut String, program: &Program, abi: X86_64Abi) {
    w!(out, "    push rbp");
    if abi.is_win64() {
        w!(out, "    .seh_pushreg rbp");
    } else {
        w!(out, "    .cfi_def_cfa_offset 16");
        w!(out, "    .cfi_offset rbp, -16");
    }
    w!(out, "    mov rbp, rsp");
    if abi.is_win64() {
        emit_saved_registers(out, abi);
        w!(out, "    sub rsp, 56");
        w!(out, "    .seh_stackalloc 56");
        w!(out, "    .seh_endprologue");
    } else {
        w!(out, "    .cfi_def_cfa_register rbp");
        emit_saved_registers(out, abi);
        w!(out, "    sub rsp, 8");
    }

    let (bytecode, entry_point, values, vm) = if abi.is_win64() {
        ("rcx", "edx", "r8", "r9")
    } else {
        ("rdi", "esi", "rdx", "rcx")
    };
    w!(out, "    mov r14, {bytecode}          # pb = bytecode base");
    w!(out, "    mov r13d, {entry_point}         # pc = entry_point");
    let values_padding = if abi.is_win64() { "           " } else { "          " };
    w!(out, "    mov rbx, {values}{values_padding}# values");
    let int32_tag_shifted =
        runtime(program)[KnownLayoutConstant::Int32TagShifted];
    let int32_tag_shifted_register = int32_tag_shifted_register();
    w!(out, "    movabs {int32_tag_shifted_register}, {int32_tag_shifted}  # INT32_TAG_SHIFTED");
    w!(out, "    mov QWORD PTR {}, {vm}  # save VM*", vm_slot(abi));
    w!(
        out,
        "    lea r12, [rip + asm_dispatch_table]  # dispatch table"
    );
    emit_dispatch(out);
    w!(out);
}

fn generate_fallback_handler(out: &mut String, program: &Program, abi: X86_64Abi) {
    // The fallback handler calls into C++ for any unhandled instruction.
    // extern "C" i64 asm_fallback_handler(VM* vm, u32 pc, u8 const* instruction);
    // Returns >= 0: new pc to dispatch to. Returns < 0: exit.
    w!(out, ".p2align 4");
    w!(out, "asm_handler_fallback:");
    emit_sync_pc_to_execution_context(out, program);
    emit_vm_pc_instruction_args(out, abi);
    w!(out, "    call CSYM(asm_fallback_handler)");
    // Check for exit (return < 0)
    w!(out, "    test rax, rax");
    w!(out, "    js .Lexit");
    // Reload exec_ctx and pb (exception handling may have unwound inline
    // frames, changing the running execution context). Value slots are
    // addressed directly from exec_ctx.
    emit_state_reload(out, program, abi);
    // New pc from return value
    w!(out, "    mov r13d, eax");
    emit_dispatch(out);
    w!(out, "asm_handler_fallback_hot_end:");
    w!(out, "asm_handler_fallback_cold:");
    w!(out, "asm_handler_fallback_cold_end:");
    w!(out);

    // Shared exit path is emitted at the end of the proc.
    w!(out);
}

/// Emit instructions to reload values (rbx) and pb (r14) from the VM*
/// saved on the stack. Uses rcx as scratch.
fn emit_state_reload(out: &mut String, program: &Program, abi: X86_64Abi) {
    let interp_ctx =
        runtime(program)[KnownLayoutConstant::VmRunningExecutionContext];
    let exec_executable =
        runtime(program)[KnownLayoutConstant::ExecutionContextExecutable];
    let exec_bytecode =
        runtime(program)[KnownLayoutConstant::ExecutableBytecodeData];
    emit_load_vm(out, "rcx", abi);
    w!(out, "    mov rbx, QWORD PTR [rcx + {interp_ctx}]");
    w!(out, "    add rbx, {}", values_offset(program));
    w!(out, "    mov rcx, QWORD PTR [rbx + {}]", exec_executable - values_offset(program));
    w!(out, "    mov r14, QWORD PTR [rcx + {exec_bytecode}]");
}

fn vm_slot(abi: X86_64Abi) -> &'static str {
    if abi.is_win64() {
        WIN64_VM_SLOT
    } else {
        SYSV_VM_SLOT
    }
}

fn emit_load_vm(out: &mut String, dst: &str, abi: X86_64Abi) {
    let slot = vm_slot(abi);
    w!(out, "    mov {dst}, QWORD PTR {slot}");
}

fn emit_vm_pc_args(out: &mut String, abi: X86_64Abi) {
    if abi.is_win64() {
        emit_load_vm(out, "rcx", abi);
        w!(out, "    mov edx, r13d");
    } else {
        emit_load_vm(out, "rdi", abi);
        w!(out, "    mov esi, r13d");
    }
}

fn emit_vm_pc_instruction_args(out: &mut String, abi: X86_64Abi) {
    emit_vm_pc_args(out, abi);
    if abi.is_win64() {
        w!(out, "    lea r8, [r14 + r13]");
    } else {
        w!(out, "    lea rdx, [r14 + r13]");
    }
}

fn emit_sync_pc_to_execution_context(out: &mut String, program: &Program) {
    let program_counter =
        runtime(program)[KnownLayoutConstant::ExecutionContextProgramCounter];
    w!(out, "    mov DWORD PTR [rbx + {}], r13d", program_counter - values_offset(program));
}

fn emit_dispatch(out: &mut String) {
    w!(out, "    movzx eax, BYTE PTR [r14 + r13]");
    w!(out, "    jmp [r12 + rax * 8]");
}

fn resolve_op(op: &Operand, _handler: &Handler, _program: &Program) -> String {
    match op {
        Operand::PhysicalRegister(register) => {
            assert_eq!(register.architecture(), Architecture::X86_64);
            register.to_string()
        }
        Operand::Relocation(symbol) => symbol.as_str().to_string(),
        Operand::Immediate(val) => format!("{val}"),
        Operand::Address(address) => format_x86_machine_address(address),
        Operand::Label(_) => super::emitter::resolve_label(op, _handler),
    }
}

fn values_offset(program: &Program) -> i64 {
    runtime(program)[KnownLayoutConstant::SizeOfExecutionContext]
}

fn int32_tag_shifted_register() -> &'static str {
    registers::R15.as_str()
}

fn format_x86_address(terms: &str, offset: i64) -> String {
    match offset.cmp(&0) {
        std::cmp::Ordering::Less => format!("[{terms} - {}]", offset.unsigned_abs()),
        std::cmp::Ordering::Equal => format!("[{terms}]"),
        std::cmp::Ordering::Greater => format!("[{terms} + {offset}]"),
    }
}

fn format_x86_machine_address(address: &MachineMemoryAddress) -> String {
    let terms = format_x86_machine_address_terms(address);
    format_x86_address(&terms, address.displacement.unwrap_or(0))
}

fn format_x86_machine_address_additive(
    address: &MachineMemoryAddress,
) -> String {
    let terms = format_x86_machine_address_terms(address);
    match address.displacement {
        Some(displacement) => format!("[{terms} + {displacement}]"),
        None => format!("[{terms}]"),
    }
}

fn format_x86_machine_address_terms(
    address: &MachineMemoryAddress,
) -> String {
    match (address.index, address.scale) {
        (None, None) => address.base.to_string(),
        (Some(index), None) => format!("{} + {index}", address.base),
        (Some(index), Some(scale)) => {
            format!("{} + {index} * {scale}", address.base)
        }
        (None, Some(_)) => {
            unreachable!("finalized x86-64 address scale requires an index")
        }
    }
}

fn shift_mnemonic(operation: ShiftOperation) -> &'static str {
    match operation {
        ShiftOperation::Left => "shl",
        ShiftOperation::RightLogical => "shr",
        ShiftOperation::RightArithmetic => "sar",
    }
}

fn float_arithmetic_mnemonic(operation: FloatBinaryOperation) -> &'static str {
    match operation {
        FloatBinaryOperation::Add => "addsd",
        FloatBinaryOperation::Subtract => "subsd",
        FloatBinaryOperation::Multiply => "mulsd",
        FloatBinaryOperation::Divide => "divsd",
    }
}

fn emit_instruction(
    out: &mut String,
    insn: &MachineInstruction,
    handler: &Handler,
    program: &Program,
) {
    let opcode = insn.opcode.x86_64();
    debug_assert!(!opcode.is_pseudo());
    if emit_simple_instruction(
        out,
        insn,
        opcode.simple_instruction(),
        |operand| match (opcode, operand) {
            (
                Opcode::LoadAdditiveDisplacement { .. }
                | Opcode::StoreAdditiveDisplacement(_),
                Operand::Address(address),
            ) => format_x86_machine_address_additive(address),
            _ => resolve_op(operand, handler, program),
        },
    ) {
        return;
    }
    match opcode {
        Opcode::Label => emit_label(out, insn, handler),
        _ => unreachable!("target pseudo reached the x86-64 machine printer"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::target::ir::MachineMemoryAddress as MemoryAddress;
    use crate::target::registers::{
        physical_register_named, resolve_interpreter_register,
    };
    use crate::types::InterpreterRegister;
    use crate::ObjectFormat;

    fn test_program() -> Program {
        Program {
            runtime: RuntimeConstants::from_values([
                (
                    KnownLayoutConstant::Int32TagShifted,
                    0x7ffa_0000_0000_0000u64 as i64,
                ),
                (KnownLayoutConstant::SizeOfExecutionContext, 120),
            ]),
            dispatch_handlers: Vec::new(),
            target: crate::Target {
                architecture: Architecture::X86_64,
                object_format: ObjectFormat::MachO,
            },
            functions: Vec::new(),
        }
    }

    fn call_handler() -> Handler {
        Handler {
            id: crate::identity::HandlerId::new(0),
            name: "Call".into(),
            is_cold: false,
            hot_instructions: Vec::new(),
            cold_instructions: Vec::new(),
        }
    }

    fn memory(
        base: &str,
        index: Option<&str>,
        scale: Option<i64>,
        displacement: Option<i64>,
    ) -> Operand {
        let register = |name: &str| {
            InterpreterRegister::from_name(name)
                .and_then(|identity| {
                    resolve_interpreter_register(identity, Architecture::X86_64)
                })
                .or_else(|| {
                    physical_register_named(name, Architecture::X86_64)
                })
                .expect("test memory address must name a target register")
        };
        Operand::Address(MemoryAddress {
            base: register(base),
            index: index.map(register),
            scale,
            displacement,
        })
    }

    fn instruction(opcode: Opcode, operands: Vec<Operand>) -> MachineInstruction {
        MachineInstruction {
            opcode: super::super::ir::MachineOpcode::X86_64(opcode),
            operands,
        }
    }

    fn emit(instructions: impl IntoIterator<Item = MachineInstruction>) -> String {
        let mut output = String::new();
        for instruction in instructions {
            emit_instruction(
                &mut output,
                &instruction,
                &call_handler(),
                &test_program(),
            );
        }
        output
    }

    #[test]
    fn emits_interpreter_handler_ranges() {
        let mut output = String::new();

        emit_handler_range(&mut output, "Call");

        assert_eq!(
            output,
            "    .quad asm_handler_Call\n    .quad asm_handler_Call_hot_end\n    .quad asm_handler_Call_cold\n    .quad asm_handler_Call_cold_end\n"
        );
    }

    #[test]
    fn preserves_scaled_index_memory_operands() {
        let program = test_program();
        let handler = call_handler();
        // Operands that already resolved to platform registers (post-
        // allocation) flow through resolve_op unchanged.
        let scaled_memory = memory("rax", Some("r11"), Some(8), None);

        assert_eq!(
            resolve_op(&scaled_memory, &handler, &program),
            "[rax + r11 * 8]"
        );
        let offset_memory =
            memory("rax", Some("r11"), Some(8), Some(8));
        assert_eq!(
            resolve_op(&offset_memory, &handler, &program),
            "[rax + r11 * 8 + 8]"
        );
    }

    #[test]
    fn addresses_value_slots_from_the_pinned_values_base() {
        let program = test_program();
        let handler = call_handler();
        let memory = memory("values", Some("rax"), Some(8), None);

        assert_eq!(resolve_op(&memory, &handler, &program), "[rbx + rax * 8]");
    }

    #[test]
    fn formats_normalized_execution_context_field_addresses() {
        let program = test_program();
        let handler = call_handler();
        let memory = memory("rbx", None, None, Some(-40));

        assert_eq!(resolve_op(&memory, &handler, &program), "[rbx - 40]");
    }

    #[test]
    fn boxes_clean_int32_values_with_the_pinned_tag() {
        let output = emit([instruction(
            Opcode::Or64Register,
            vec![
                Operand::PhysicalRegister(crate::target::registers::x86_64::RAX),
                Operand::PhysicalRegister(crate::target::registers::x86_64::R15),
            ],
        )]);

        assert_eq!(output, "    or rax, r15\n");
    }

    #[test]
    fn emits_high_single_bit_branch_mask_with_bt() {
        let output = emit([
            instruction(
                Opcode::BitTest64Immediate,
                vec![
                    Operand::PhysicalRegister(crate::target::registers::x86_64::RDX),
                    Operand::Immediate(32),
                ],
            ),
            instruction(
                Opcode::JumpCondition(Condition::NotCarry),
                vec![Operand::Label(".slow".into())],
            ),
        ]);

        assert!(output.contains("bt rdx, 32"));
        assert!(output.contains("jnc .Lasm_Call.slow"));
        assert!(!output.contains("test rdx, 4294967296"));
    }

    #[test]
    fn emits_bit_31_branches_as_sign_tests() {
        let mut output = String::new();

        for (condition, branch) in [
            (Condition::Negative, "js"),
            (Condition::NotNegative, "jns"),
        ] {
            output += &emit([
                instruction(
                    Opcode::TestRegister(IntegerWidth::U32),
                    vec![
                        Operand::PhysicalRegister(crate::target::registers::x86_64::RAX),
                    ],
                ),
                instruction(
                    Opcode::JumpCondition(condition),
                    vec![
                        Operand::Label(".slow".into()),
                    ],
                ),
            ]);
            assert!(output.contains("test eax, eax"));
            assert!(output.contains(&format!("{branch} .Lasm_Call.slow")));
        }
        assert!(!output.contains("bt "));
    }

    #[test]
    fn lowers_narrow_signed_branches_at_32_bit_width() {
        let output = emit([
            instruction(
                Opcode::CompareRegister(IntegerWidth::U32),
                vec![
                    Operand::PhysicalRegister(crate::target::registers::x86_64::RAX),
                    Operand::PhysicalRegister(crate::target::registers::x86_64::RCX),
                ],
            ),
            instruction(
                Opcode::JumpCondition(Condition::SignedLess),
                vec![Operand::Label(".slow".into())],
            ),
        ]);

        assert_eq!(output, "    cmp eax, ecx\n    jl .Lasm_Call.slow\n");
    }

    #[test]
    fn mov32_zero_extends_even_when_registers_alias() {
        let output = emit([instruction(
            Opcode::Move32Register,
            vec![Operand::PhysicalRegister(crate::target::registers::x86_64::RAX), Operand::PhysicalRegister(crate::target::registers::x86_64::RAX)],
        )]);

        assert!(output.contains("mov eax, eax"));
    }

    #[test]
    fn emits_finalized_float_conversion() {
        let output = emit([instruction(
            Opcode::FloatConversion(
                FloatConversion::DoubleToSigned32Truncate,
            ),
            vec![
                Operand::PhysicalRegister(crate::target::registers::x86_64::RDX),
                Operand::PhysicalRegister(crate::target::registers::x86_64::XMM0),
            ],
        )]);

        assert_eq!(output, "    cvttsd2si edx, xmm0\n");
    }

    #[test]
    fn emits_byte_memory_branch_without_a_register_load() {
        let output = emit([
            instruction(
                Opcode::TestMemoryImmediate(
                    MemoryWidth::Byte,
                ),
                vec![
                    memory("rcx", None, None, Some(4)),
                    Operand::Immediate(2),
                ],
            ),
            instruction(
                Opcode::JumpCondition(Condition::Zero),
                vec![Operand::Label(".slow".into())],
            ),
        ]);

        assert!(output.contains("test BYTE PTR [rcx + 4], 2"));
        assert!(output.contains("jz .Lasm_Call.slow"));
        assert!(!output.contains("movzx"));
    }

    #[test]
    fn compares_full_range_u32_immediates_at_32_bit_width() {
        let output = emit([instruction(
            Opcode::CompareImmediate(IntegerWidth::U32),
            vec![
                Operand::PhysicalRegister(crate::target::registers::x86_64::RAX),
                Operand::Immediate(0xffff_fffe),
            ],
        )]);

        assert!(output.contains("cmp eax, 4294967294"));
        assert!(!output.contains("cmp rax, 4294967294"));
    }

    #[test]
    fn emits_finalized_assertion_conditions() {
        let cases = [
            (Condition::NotEqual, "    jne "),
            (Condition::UnsignedGreaterOrEqual, "    jae "),
            (Condition::NonZero, "    jnz "),
            (Condition::Zero, "    jz "),
        ];

        for (condition, expected) in cases {
            let output = emit([instruction(
                Opcode::JumpCondition(condition),
                vec![Operand::Label(".assert_ok".into())],
            )]);
            assert!(output.contains(expected), "{condition:?} emitted:\n{output}");
        }
    }

    #[test]
    fn win64_exit_omits_arm64_seh_epilogue_markers() {
        let mut out = String::new();

        generate_exit_point(&mut out, ObjectFormat::Coff, X86_64Abi::Win64);

        assert!(out.contains("    add rsp, 56"));
        assert!(out.contains("    ret"));
        assert!(!out.contains(".seh_startepilogue"));
        assert!(!out.contains(".seh_endepilogue"));
    }
}
