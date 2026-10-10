/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Assembled bytecode: decoding, control flow and liveness.

pub mod cfg;
mod instruction;
pub mod liveness;

pub use instruction::*;

/// Number of reserved registers at the start of every frame: accumulator,
/// exception, this value, return value and saved lexical environment.
pub const RESERVED_REGISTER_COUNT: u32 = 5;

/// The reserved register holding the frame's `this` value.
pub const THIS_VALUE_REGISTER: u32 = 2;

/// The `kind` of a `CreateArguments` that creates a mapped arguments object
/// (`ArgumentsKind::Mapped` in `Libraries/LibJS/ABI`).
pub const MAPPED_ARGUMENTS: u32 = 0;

/// The `kind` of a `PutByValue` or `PutById` of a plain assignment
/// (`PutKind::Normal` in `Libraries/LibJS/ABI`).
pub const PUT_KIND_NORMAL: u32 = 0;

/// How an executable's flat slot indices are partitioned. Operands index the
/// frame's value array laid out as `[registers | locals | constants | arguments]`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct FrameLayout {
    pub number_of_registers: u32,
    /// Registers plus locals; also the index of the first constant.
    pub registers_and_locals_count: u32,
    pub number_of_constants: u32,
    /// One past the highest argument index the bytecode references.
    pub number_of_arguments: u32,
}

/// What kind of frame slot an operand refers to, with its index within that kind.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SlotKind {
    Register(u32),
    Local(u32),
    Constant(u32),
    Argument(u32),
}

impl FrameLayout {
    pub fn constants_base(&self) -> u32 {
        self.registers_and_locals_count
    }

    pub fn arguments_base(&self) -> u32 {
        self.registers_and_locals_count + self.number_of_constants
    }

    pub fn slot_kind(&self, operand: Operand) -> SlotKind {
        let index = operand.raw();
        if index < self.number_of_registers {
            SlotKind::Register(index)
        } else if index < self.registers_and_locals_count {
            SlotKind::Local(index - self.number_of_registers)
        } else if index < self.arguments_base() {
            SlotKind::Constant(index - self.constants_base())
        } else {
            SlotKind::Argument(index - self.arguments_base())
        }
    }

    /// Number of slots that can hold mutable state: registers, locals and
    /// arguments. Constants are read-only and are not tracked.
    pub fn tracked_slot_count(&self) -> usize {
        (self.registers_and_locals_count + self.number_of_arguments) as usize
    }

    /// Dense index of a mutable slot (registers and locals first, then
    /// arguments), or `None` for a constant.
    pub fn tracked_index(&self, operand: Operand) -> Option<usize> {
        let index = operand.raw();
        if index < self.registers_and_locals_count {
            return Some(index as usize);
        }
        if index < self.arguments_base() {
            return None;
        }
        let argument = index - self.arguments_base();
        assert!(
            argument < self.number_of_arguments,
            "operand {index} is past the frame's arguments"
        );
        Some((self.registers_and_locals_count + argument) as usize)
    }

    /// Inverse of `tracked_index()`.
    pub fn operand_for_tracked_index(&self, tracked_index: usize) -> Operand {
        let index = u32::try_from(tracked_index).expect("tracked index fits in u32");
        if index < self.registers_and_locals_count {
            return Operand::from_raw(index);
        }
        let argument = index - self.registers_and_locals_count;
        assert!(argument < self.number_of_arguments, "tracked index out of range");
        Operand::from_raw(self.arguments_base() + argument)
    }
}

/// An exception handler range of an executable: an exception thrown by an
/// instruction in `start_offset..end_offset` transfers control to `handler_offset`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ExceptionHandler {
    pub start_offset: u32,
    pub end_offset: u32,
    pub handler_offset: u32,
}

#[cfg(test)]
pub(crate) mod test_support {
    use super::cfg::Cfg;
    use super::*;

    /// Eight registers (five reserved), two locals, two constants and one argument.
    pub fn test_layout() -> FrameLayout {
        FrameLayout {
            number_of_registers: 8,
            registers_and_locals_count: 10,
            number_of_constants: 2,
            number_of_arguments: 1,
        }
    }

    pub fn r(index: u32) -> Operand {
        assert!(index < 8);
        Operand::from_raw(index)
    }

    pub fn l(index: u32) -> Operand {
        assert!(index < 2);
        Operand::from_raw(8 + index)
    }

    pub fn c(index: u32) -> Operand {
        assert!(index < 2);
        Operand::from_raw(10 + index)
    }

    pub fn a(index: u32) -> Operand {
        assert!(index < 1);
        Operand::from_raw(12 + index)
    }

    pub struct Program {
        pub bytes: Vec<u8>,
        /// Byte offset of each instruction.
        pub offsets: Vec<u32>,
        pub handlers: Vec<ExceptionHandler>,
    }

    impl Program {
        pub fn instructions(&self) -> Vec<DecodedInstruction> {
            decode_all(&self.bytes).unwrap()
        }

        pub fn cfg(&self) -> Cfg {
            Cfg::new(&self.instructions(), &self.handlers, &test_layout()).unwrap()
        }
    }

    /// Encodes the instructions returned by `build`, which receives a function
    /// that turns an instruction index into a label pointing at that instruction.
    pub fn assemble(build: impl Fn(&dyn Fn(usize) -> Label) -> Vec<Instruction>) -> Program {
        assemble_with_handlers(build, &[])
    }

    /// Like `assemble()`, with exception handlers given as instruction indices
    /// `(start, end, handler)`, where `end` is exclusive.
    pub fn assemble_with_handlers(
        build: impl Fn(&dyn Fn(usize) -> Label) -> Vec<Instruction>,
        handlers: &[(usize, usize, usize)],
    ) -> Program {
        let mut offsets = Vec::new();
        let mut offset = 0;
        for instruction in build(&|_| Label(0)) {
            offsets.push(offset);
            offset += u32::try_from(instruction.encoded_size()).unwrap();
        }
        offsets.push(offset);

        let mut bytes = Vec::new();
        for instruction in build(&|index| Label(offsets[index])) {
            instruction.encode(false, &mut bytes);
        }
        assert_eq!(bytes.len(), offset as usize);

        let handlers = handlers
            .iter()
            .map(|(start, end, handler)| ExceptionHandler {
                start_offset: offsets[*start],
                end_offset: offsets[*end],
                handler_offset: offsets[*handler],
            })
            .collect();
        offsets.pop();
        Program {
            bytes,
            offsets,
            handlers,
        }
    }
}
