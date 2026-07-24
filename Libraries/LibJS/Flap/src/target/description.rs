/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Target-machine instruction contracts.
//!
//! Each semantic instruction family owns its operand kinds, control-flow
//! semantics, and per-architecture register footprint (hidden clobbers,
//! implicit inputs/outputs, and hard register requirements). The register
//! allocator for typed virtual registers consumes these contracts.
//!
//! ## What gets recorded here
//!
//! - **Operand kinds** -- the role of each operand position in LowIR
//!   (read-only GPR, written GPR, FPR, immediate, memory, label, ...). The
//!   allocator reads this to derive use/def sets for liveness analysis.
//! - **`terminal`** -- whether control falls through to the next instruction.
//!   Terminal instructions (jmp, exit, dispatch_*, goto_handler,
//!   call_slow_path) end the basic block; the allocator does not extend any
//!   liveness past them.
//! - **`is_call`** -- whether the instruction performs a C++ call that
//!   clobbers every caller-saved register. The allocator must forbid keeping
//!   any virtual register live across such instructions, since no register
//!   survives the call.
//! - **`implicit_inputs` / `implicit_outputs`** -- hidden register operands
//!   not exposed in the LowIR operand list. Examples: `call_helper` reads from
//!   t1 (rcx/x1) and writes to t0 (rax/x0); `divmod` writes its results to
//!   rax/rdx on x86_64 regardless of the named operands.
//! - **`fixed_operands`** -- a constraint that operand position N must
//!   resolve to a specific physical register (e.g., x86 shifts require the
//!   count register to be `rcx`).
//!
//! ## What is *not* recorded
//!
//! Codegen emit logic itself (the assembly text). The codegen functions remain
//! authoritative for the actual emitted bytes; these contracts only describe
//! what the allocator needs to feed them safely.
//!
//! ## Caller-saved register sets
//!
//! These are the registers an `is_call=true` instruction kills (in addition
//! to any explicitly listed clobbers). Pinned registers survive calls
//! because they live in callee-saved slots. AArch64 pins its values base,
//! x21 (ip cache), and x22-x24 (tag constants); x86-64 pins values and the
//! shifted Int32 tag, and derives exec_ctx from values.
//!
//! - x86_64: rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11, xmm0-xmm5
//! - aarch64: x0-x17, d0-d7
//!
//! The AArch64 sets stop where the allocator does. A call also clobbers x18 and
//! d16-d31, but none of those are allocatable or hold interpreter state, so the
//! allocator never has to keep anything alive across a call in them. Adding one
//! of them to `temporaries` or `fp_temporaries` means adding it here too.

use crate::Architecture;
pub(crate) use crate::intrinsic::{
    AssertionOperation, BranchOperation, CallOperation as CallKind, ControlOperation, EqualityCondition, FloatBinaryOperation, FloatConversion,
    FloatUnaryOperation, FloatingPointOperation, IntegerBinaryOperation, SignCondition, MemoryWidth, PairWidth, TestCondition, ZeroCondition,
    IntegerSignedness, IntegerWidth, MemoryOperation,
};
use crate::target::ir::Operand;
use crate::target::{aarch64 as aarch64_target, x86_64 as x86_64_target};
use crate::target::registers::{
    PhysicalRegister, RegisterClass, aarch64, x86_64,
};

/// An architecture-selected target instruction.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub(crate) struct SelectedOpcode {
    operation: Operation,
    opcode: ArchitectureOpcode,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum ArchitectureOpcode {
    X86_64(x86_64_target::Opcode),
    Aarch64(aarch64_target::Opcode),
}

impl ArchitectureOpcode {
    pub(crate) fn architecture(self) -> Architecture {
        match self {
            Self::X86_64(_) => Architecture::X86_64,
            Self::Aarch64(_) => Architecture::Aarch64,
        }
    }

    pub(crate) fn x86_64(self) -> x86_64_target::Opcode {
        let Self::X86_64(opcode) = self else {
            unreachable!("expected an x86-64 opcode")
        };
        opcode
    }

    pub(crate) fn aarch64(self) -> aarch64_target::Opcode {
        let Self::Aarch64(opcode) = self else {
            unreachable!("expected an AArch64 opcode")
        };
        opcode
    }
}

pub(crate) use crate::intrinsic::{BinaryOperation, ShiftOperation};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum OverflowOperation {
    AddWithOverflow,
    Increment,
    Decrement,
    SubtractWithOverflow,
    MultiplyWithOverflow,
    MultiplyCopy,
    Negate,
}

pub(crate) use crate::intrinsic::ComparisonRelation as OrderedCondition;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum ScalarBranchCondition {
    Equal,
    NotEqual,
    Signed(OrderedCondition),
    Unsigned(OrderedCondition),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum MemoryBranchKind {
    CompareRegister {
        width: IntegerWidth,
        condition: EqualityCondition,
    },
    CompareByte(EqualityCondition),
    TestByte(TestCondition),
}

pub(crate) use crate::intrinsic::FloatComparison as FloatCondition;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum Operation {
    Cold,
    Label,
    Control(ControlOperation),
    Call(CallKind),
    LoadVm,
    LoadProgramCounter,
    StoreOperand,
    LoadLabel,
    Memory(MemoryOperation),
    StoreExecutionContext,
    Store64IndexedOffset,
    StorePairIndexed(PairWidth),
    Increment32Memory,
    LoadEffectiveAddress,
    Move(IntegerWidth),
    IntegerBinary { operation: IntegerBinaryOperation, width: IntegerWidth },
    Or32Branch(SignCondition),
    Negate,
    Not(IntegerWidth),
    DivMod,
    Overflow(OverflowOperation),
    ExtractTag,
    UnboxInt32,
    UnboxObject,
    BoxInt32 { clean: bool },
    ToggleBit,
    ClearBit,
    Float(FloatingPointOperation),
    FloatMove,
    Branch(BranchOperation),
    Assertion(AssertionOperation),
}

impl Operation {
    pub(crate) fn operation(&self) -> Self {
        *self
    }

    pub(crate) fn select_for_operands(
        self,
        architecture: Architecture,
        operands: &[crate::target::ir::Operand],
    ) -> SelectedOpcode {
        let operation = self;
        SelectedOpcode {
            operation,
            opcode: match architecture {
                Architecture::X86_64 => ArchitectureOpcode::X86_64(
                    x86_64_target::Opcode::select_for_operands(operation, operands),
                ),
                Architecture::Aarch64 => ArchitectureOpcode::Aarch64(
                    aarch64_target::Opcode::select_for_operands(operation, operands),
                ),
            },
        }
    }

    pub(crate) fn description(&self) -> InstructionDescription {
        lookup_operation(*self)
    }
}

impl SelectedOpcode {
    pub(crate) fn for_operation(
        operation: Operation,
        architecture: Architecture,
    ) -> Self {
        Self {
            operation,
            opcode: match architecture {
                Architecture::X86_64 => {
                    ArchitectureOpcode::X86_64(x86_64_target::Opcode::select(operation))
                }
                Architecture::Aarch64 => {
                    ArchitectureOpcode::Aarch64(aarch64_target::Opcode::select(operation))
                }
            },
        }
    }

    pub(crate) fn architecture(&self) -> Architecture {
        self.opcode.architecture()
    }

    pub(crate) fn aarch64(&self) -> aarch64_target::Opcode {
        self.opcode.aarch64()
    }

    pub(crate) fn replacing_operation(&self, operation: Operation) -> Self {
        Self::for_operation(operation, self.architecture())
    }

    pub(crate) fn operation(&self) -> Operation {
        self.operation
    }

    pub(crate) fn machine_opcode(&self) -> ArchitectureOpcode {
        self.opcode
    }

    pub(crate) fn description(&self) -> InstructionDescription {
        match self.opcode {
            ArchitectureOpcode::X86_64(
                x86_64_target::Opcode::StoreLargeImmediate(_),
            ) => X86_64_LARGE_IMMEDIATE_STORE_DESCRIPTION,
            _ => lookup_operation(self.operation),
        }
    }

    pub(crate) fn add_contract_operands(&self, operands: &mut Vec<Operand>) {
        let architecture = self.architecture();
        let operation = self.operation;

        let description = self.description();
        let specification = description.arch(architecture);
        let insertion_index = operands.len() - description.trailing_operands.len();
        insert_registers(
            operands,
            insertion_index,
            specification.scratch_registers_before_trailing_operands,
        );
        append_registers(operands, specification.trailing_scratch_registers);

        if architecture == Architecture::X86_64 && operation == Operation::StoreOperand {
            match operands.get(1).expect("store_operand must have a source") {
                Operand::Immediate(value)
                    if !(i32::MIN as i64..=i32::MAX as i64).contains(value) =>
                {
                    append_registers(operands, &[x86_64::RAX, x86_64::R11]);
                }
                Operand::PhysicalRegister(register) if *register == x86_64::R11 => {
                    append_registers(operands, &[x86_64::RAX]);
                }
                _ => append_registers(operands, &[x86_64::R11]),
            }
        }
    }
}

fn insert_registers(operands: &mut Vec<Operand>, index: usize, registers: &[PhysicalRegister]) {
    operands.splice(index..index, registers.iter().copied().map(Operand::PhysicalRegister));
}

fn append_registers(operands: &mut Vec<Operand>, registers: &[PhysicalRegister]) {
    operands.extend(registers.iter().copied().map(Operand::PhysicalRegister));
}

impl From<PairWidth> for MemoryWidth {
    fn from(width: PairWidth) -> Self {
        match width {
            PairWidth::Word => Self::Word,
            PairWidth::DoubleWord => Self::DoubleWord,
        }
    }
}

impl Operation {
    pub(crate) const fn load(width: MemoryWidth, signed: bool) -> Self {
        Self::Memory(MemoryOperation::Load {
            width,
            signed,
        })
    }

    pub(crate) const fn store(width: MemoryWidth) -> Self {
        Self::Memory(MemoryOperation::Store(width))
    }

    pub(crate) const fn load_pair(width: PairWidth) -> Self {
        Self::Memory(MemoryOperation::LoadPair(width))
    }

    pub(crate) const fn store_pair(width: PairWidth) -> Self {
        Self::Memory(MemoryOperation::StorePair(width))
    }

    pub(crate) const fn branch_equality(width: IntegerWidth, condition: EqualityCondition) -> Self {
        Self::Branch(BranchOperation::Equality { width, condition })
    }

    pub(crate) const fn branch_ordered(
        width: IntegerWidth,
        relation: OrderedCondition,
        signedness: IntegerSignedness,
    ) -> Self {
        Self::Branch(BranchOperation::Ordered {
            width,
            relation,
            signedness,
        })
    }

    pub(crate) const fn branch_zero(width: IntegerWidth, condition: ZeroCondition) -> Self {
        Self::Branch(BranchOperation::Zero { width, condition })
    }

    pub(crate) const fn branch_sign(width: IntegerWidth, condition: SignCondition) -> Self {
        Self::Branch(BranchOperation::Sign { width, condition })
    }

    pub(crate) const fn branch_bits(width: MemoryWidth, condition: TestCondition) -> Self {
        Self::Branch(BranchOperation::Bits { width, condition })
    }

    pub(crate) const fn branch_tag(condition: EqualityCondition) -> Self {
        Self::Branch(BranchOperation::Tag(condition))
    }

    pub(crate) const fn branch_singleton(condition: EqualityCondition) -> Self {
        Self::Branch(BranchOperation::Singleton(condition))
    }

    pub(crate) const fn branch_memory(width: PairWidth, condition: EqualityCondition) -> Self {
        Self::Branch(BranchOperation::Memory { width, condition })
    }

    pub(crate) const fn branch_bit(condition: TestCondition) -> Self {
        Self::Branch(BranchOperation::Bit(condition))
    }

    pub(crate) fn scalar_branch(self) -> Option<(IntegerWidth, ScalarBranchCondition)> {
        Some(match self {
            Self::Branch(BranchOperation::Equality { width, condition }) => (
                width,
                condition.select(ScalarBranchCondition::Equal, ScalarBranchCondition::NotEqual),
            ),
            Self::Branch(BranchOperation::Ordered {
                width,
                relation,
                signedness,
            }) => (
                width,
                match signedness {
                    IntegerSignedness::Signed => ScalarBranchCondition::Signed(relation),
                    IntegerSignedness::Unsigned => ScalarBranchCondition::Unsigned(relation),
                },
            ),
            _ => return None,
        })
    }

    pub(crate) fn memory_branch(self) -> Option<MemoryBranchKind> {
        Some(match self {
            Self::Branch(BranchOperation::Memory { width, condition }) => MemoryBranchKind::CompareRegister {
                width: match width {
                    PairWidth::Word => IntegerWidth::U32,
                    PairWidth::DoubleWord => IntegerWidth::U64,
                },
                condition,
            },
            Self::Branch(BranchOperation::Equality {
                width: IntegerWidth::U8,
                condition,
            }) => MemoryBranchKind::CompareByte(condition),
            Self::Branch(BranchOperation::Bits {
                width: MemoryWidth::Byte,
                condition,
            }) => MemoryBranchKind::TestByte(condition),
            _ => return None,
        })
    }

}

/// Kind of a single LowIR operand position.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum OperandKind {
    /// GPR read as input.
    GprIn,
    /// GPR written; previous contents discarded.
    GprOut,
    /// Target-selected physical GPR scratch that must not alias any other
    /// register operand.
    GprScratch,
    /// Target-selected physical FP scratch that must not alias any other
    /// register operand.
    FprScratch,
    /// GPR both read and written.
    GprInOut,
    /// FP register read as input.
    FprIn,
    /// FP register written; previous contents discarded.
    FprOut,
    /// FP register both read and written.
    FprInOut,
    /// GPR or FP register read as input.
    RegisterIn,
    /// GPR or FP register written; previous contents discarded.
    RegisterOut,
    /// GPR (read) or immediate.
    GprInOrImm,
    /// GPR input or memory address.
    GprInOrMemory,
    /// Compile-time integer (immediate, named constant, or field reference).
    Imm,
    /// Memory operand `[base, ...]`. Reads the GPRs referenced inside the
    /// brackets; the memory itself may be loaded from or stored to depending
    /// on the mnemonic.
    Memory,
    /// Branch-target label (local `.foo` or global symbol).
    Label,
    /// Function symbol used by `call_*` instructions.
    FuncSymbol,
}

impl OperandKind {
    pub(crate) fn accepts(self, shape: super::ir::OperandShape) -> bool {
        use super::ir::OperandShape;
        let register_class = match shape {
            OperandShape::PhysicalRegister(class) => Some(class),
            _ => None,
        };
        match self {
            Self::GprIn | Self::GprOut | Self::GprInOut => {
                register_class == Some(RegisterClass::GeneralPurpose)
            }
            Self::GprScratch => shape == OperandShape::PhysicalRegister(RegisterClass::GeneralPurpose),
            Self::FprIn | Self::FprOut | Self::FprInOut => {
                register_class == Some(RegisterClass::FloatingPoint)
            }
            Self::FprScratch => shape == OperandShape::PhysicalRegister(RegisterClass::FloatingPoint),
            Self::RegisterIn | Self::RegisterOut => register_class.is_some(),
            Self::GprInOrImm => {
                register_class == Some(RegisterClass::GeneralPurpose)
                    || shape == OperandShape::Immediate
            }
            Self::GprInOrMemory => {
                register_class == Some(RegisterClass::GeneralPurpose)
                    || shape == OperandShape::Address
            }
            Self::Imm => shape == OperandShape::Immediate,
            Self::Memory => shape == OperandShape::Address,
            Self::Label => shape == OperandShape::Label,
            Self::FuncSymbol => shape == OperandShape::Relocation(crate::low_ir::RelocationKind::FunctionCall),
        }
    }
}

/// Per-architecture register footprint of an instruction.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct ArchSpec {
    /// Hidden register inputs not visible in the operand list.
    /// E.g. `call_helper` reads its first argument from rcx (x1).
    pub(crate) implicit_inputs: &'static [PhysicalRegister],
    /// Hidden register outputs not visible in the operand list.
    /// E.g. `call_helper` returns its result in rax (x0).
    pub(crate) implicit_outputs: &'static [PhysicalRegister],
    /// Constraints requiring specific operand positions to resolve to fixed
    /// physical registers. Pairs of `(operand_index, physical_register)`.
    pub(crate) fixed_operands: &'static [(usize, PhysicalRegister)],
    /// Exact number of variadic operands required after target selection.
    /// `None` retains the instruction-wide variadic range.
    pub(crate) selected_variadic_operands: Option<usize>,
    /// Target-selected scratch registers appended after all semantic
    /// operands. Their register classes determine their operand kinds.
    pub(crate) trailing_scratch_registers: &'static [PhysicalRegister],
    /// Target-selected scratch registers inserted immediately before the
    /// instruction's trailing semantic operands.
    pub(crate) scratch_registers_before_trailing_operands: &'static [PhysicalRegister],
}

impl ArchSpec {
    pub(crate) const NONE: ArchSpec = ArchSpec {
        implicit_inputs: &[],
        implicit_outputs: &[],
        fixed_operands: &[],
        selected_variadic_operands: None,
        trailing_scratch_registers: &[],
        scratch_registers_before_trailing_operands: &[],
    };

    const fn inputs(mut self, registers: &'static [PhysicalRegister]) -> Self {
        self.implicit_inputs = registers;
        self
    }

    const fn outputs(mut self, registers: &'static [PhysicalRegister]) -> Self {
        self.implicit_outputs = registers;
        self
    }

    const fn fixed(mut self, operands: &'static [(usize, PhysicalRegister)]) -> Self {
        self.fixed_operands = operands;
        self
    }

    const fn selected_variadic(mut self, count: usize) -> Self {
        self.selected_variadic_operands = Some(count);
        self
    }

    const fn scratches(mut self, registers: &'static [PhysicalRegister]) -> Self {
        self.trailing_scratch_registers = registers;
        self
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct InstructionDescription {
    pub(crate) operands: &'static [OperandKind],
    /// Final operand kind that may repeat (variadic). `None` means fixed
    /// arity.
    pub(crate) variadic_kind: Option<OperandKind>,
    pub(crate) minimum_variadic_operands: usize,
    pub(crate) maximum_variadic_operands: Option<usize>,
    pub(crate) trailing_operands: &'static [OperandKind],
    /// Control does not fall through to the next instruction.
    pub(crate) terminal: bool,
    /// C++ call. Implies all caller-saved virtuals are killed.
    pub(crate) is_call: bool,
    pub(crate) coalescing_pairs: &'static [(usize, usize)],
    pub(crate) identity_if_inputs_alias: bool,
    pub(crate) zeroes_if_inputs_alias: bool,
    pub(crate) x86_64: ArchSpec,
    pub(crate) aarch64: ArchSpec,
}

impl InstructionDescription {
    const fn new(operands: &'static [OperandKind]) -> Self {
        Self {
            operands,
            variadic_kind: None,
            minimum_variadic_operands: 0,
            maximum_variadic_operands: None,
            trailing_operands: &[],
            terminal: false,
            is_call: false,
            coalescing_pairs: &[],
            identity_if_inputs_alias: false,
            zeroes_if_inputs_alias: false,
            x86_64: ArchSpec::NONE,
            aarch64: ArchSpec::NONE,
        }
    }

    const fn terminal(mut self) -> Self {
        self.terminal = true;
        self
    }

    const fn call(mut self) -> Self {
        self.is_call = true;
        self
    }

    const fn coalesces(mut self, pairs: &'static [(usize, usize)]) -> Self {
        self.coalescing_pairs = pairs;
        self
    }

    const fn identity_if_inputs_alias(mut self) -> Self {
        self.identity_if_inputs_alias = true;
        self
    }

    const fn zeroes_if_inputs_alias(mut self) -> Self {
        self.zeroes_if_inputs_alias = true;
        self
    }

    const fn x86_64(mut self, specification: ArchSpec) -> Self {
        self.x86_64 = specification;
        self
    }

    const fn aarch64(mut self, specification: ArchSpec) -> Self {
        self.aarch64 = specification;
        self
    }

    const fn variadic(
        mut self,
        kind: OperandKind,
        minimum: usize,
        maximum: Option<usize>,
        trailing: &'static [OperandKind],
    ) -> Self {
        self.variadic_kind = Some(kind);
        self.minimum_variadic_operands = minimum;
        self.maximum_variadic_operands = maximum;
        self.trailing_operands = trailing;
        self
    }

    const fn trailing(mut self, operands: &'static [OperandKind]) -> Self {
        self.trailing_operands = operands;
        self
    }

    /// Scratch registers appended after every operand, per architecture.
    const fn scratches(
        mut self,
        x86_64: &'static [PhysicalRegister],
        aarch64: &'static [PhysicalRegister],
    ) -> Self {
        self.x86_64.trailing_scratch_registers = x86_64;
        self.aarch64.trailing_scratch_registers = aarch64;
        self
    }

    /// Scratch registers inserted before the trailing semantic operands,
    /// per architecture.
    const fn pre_scratches(
        mut self,
        x86_64: &'static [PhysicalRegister],
        aarch64: &'static [PhysicalRegister],
    ) -> Self {
        self.x86_64.scratch_registers_before_trailing_operands = x86_64;
        self.aarch64.scratch_registers_before_trailing_operands = aarch64;
        self
    }

    /// Look up the per-arch spec for this instruction.
    pub(crate) fn arch(&self, arch: Architecture) -> &ArchSpec {
        match arch {
            Architecture::X86_64 => &self.x86_64,
            Architecture::Aarch64 => &self.aarch64,
        }
    }

    pub(crate) fn accepts_operand_count(&self, count: usize) -> bool {
        let fixed = self.operands.len() + self.trailing_operands.len();
        if self.variadic_kind.is_none() {
            return count == fixed;
        }
        let variadic_count = count.checked_sub(fixed);
        variadic_count.is_some_and(|count| {
            count >= self.minimum_variadic_operands
                && self.maximum_variadic_operands.is_none_or(|maximum| count <= maximum)
        })
    }

    pub(crate) fn accepts_selected_operand_count(
        &self,
        count: usize,
        architecture: Architecture,
    ) -> bool {
        let arch = self.arch(architecture);
        let scratch_count = arch.trailing_scratch_registers.len()
            + arch.scratch_registers_before_trailing_operands.len();
        let Some(count) = count.checked_sub(scratch_count) else {
            return false;
        };
        if let Some(variadic_count) =
            self.arch(architecture).selected_variadic_operands
        {
            return self.variadic_kind.is_some()
                && count
                    == self.operands.len()
                        + variadic_count
                        + self.trailing_operands.len();
        }
        self.accepts_operand_count(count)
    }

    pub(crate) fn operand_kind(&self, index: usize, operand_count: usize) -> Option<OperandKind> {
        if let Some(kind) = self.operands.get(index) {
            return Some(*kind);
        }
        let trailing_start = operand_count.checked_sub(self.trailing_operands.len())?;
        if index >= trailing_start {
            return self.trailing_operands.get(index - trailing_start).copied();
        }
        self.variadic_kind
    }

    pub(crate) fn selected_operand_kind(
        &self,
        index: usize,
        operand_count: usize,
        architecture: Architecture,
    ) -> Option<OperandKind> {
        let scratch_registers =
            self.arch(architecture).trailing_scratch_registers;
        let before_trailing = self
            .arch(architecture)
            .scratch_registers_before_trailing_operands;
        let total_scratch_count =
            scratch_registers.len() + before_trailing.len();
        let semantic_count =
            operand_count.checked_sub(total_scratch_count)?;
        let before_trailing_end = operand_count
            .checked_sub(scratch_registers.len())?
            .checked_sub(self.trailing_operands.len())?;
        let before_trailing_start =
            before_trailing_end.checked_sub(before_trailing.len())?;
        if index >= operand_count - scratch_registers.len() {
            return scratch_registers
                .get(index - (operand_count - scratch_registers.len()))
                .map(|register| match register.class() {
                    RegisterClass::GeneralPurpose => OperandKind::GprScratch,
                    RegisterClass::FloatingPoint => OperandKind::FprScratch,
                });
        }
        if index >= before_trailing_start && index < before_trailing_end {
            return before_trailing
                .get(index - before_trailing_start)
                .map(|register| match register.class() {
                    RegisterClass::GeneralPurpose => OperandKind::GprScratch,
                    RegisterClass::FloatingPoint => OperandKind::FprScratch,
                });
        }
        let semantic_index = if index >= before_trailing_end {
            index - before_trailing.len()
        } else {
            index
        };
        self.operand_kind(semantic_index, semantic_count)
    }
}

/// An instruction taking exactly `operands` and nothing else.
const fn plain(operands: &'static [OperandKind]) -> InstructionDescription {
    InstructionDescription::new(operands)
}

/// An architecture specification with no constraints applied yet.
const fn spec() -> ArchSpec {
    ArchSpec::NONE
}

const fn scalar_compare_branch() -> InstructionDescription {
    plain(&[GprIn, GprInOrImm]).trailing(&[Label]).pre_scratches(&[], &[X9])
}

use OperandKind::*;
use aarch64::{D16, X0, X1, X9, X10};
use x86_64::{R11, RAX, RCX, RDX, XMM3};

const X86_64_LARGE_IMMEDIATE_STORE_DESCRIPTION: InstructionDescription =
    plain(&[Memory, GprInOrImm]).scratches(&[R11], &[]);

fn lookup_operation(operation: Operation) -> InstructionDescription {
    use BinaryOperation::*;
    use IntegerWidth::*;
    use OverflowOperation::*;

    match operation {
        Operation::Cold => plain(&[Label]),
        Operation::Label => plain(&[Label]),
        Operation::Control(ControlOperation::JumpLabel) => plain(&[Label]).terminal(),
        Operation::Control(ControlOperation::Exit) => plain(&[]).terminal(),
        Operation::Control(ControlOperation::DispatchNext | ControlOperation::Dispatch) => plain(&[])
            .terminal().scratches(&[RAX], &[X9, X10]),
        Operation::Control(ControlOperation::DispatchVariable | ControlOperation::GotoHandler) => plain(&[GprIn])
            .terminal().scratches(&[RAX], &[X9, X10]),
        Operation::Control(ControlOperation::JumpBytecode) => plain(&[Imm]).terminal()
            .scratches(&[RAX], &[X0, X9, X10]),
        Operation::Call(CallKind::SlowPath) => plain(&[FuncSymbol]).terminal().call()
            .x86_64(spec().scratches(&[RAX, RCX])).aarch64(spec().scratches(&[X9, X10])),
        Operation::Call(CallKind::Helper) => plain(&[FuncSymbol, GprIn, GprOut]).call()
            .x86_64(spec().inputs(&[RCX]).outputs(&[RAX]).fixed(&[(1, RCX), (2, RAX)]))
            .aarch64(spec().inputs(&[X0]).outputs(&[X0]).fixed(&[(1, X0), (2, X0)])),
        Operation::Call(CallKind::Interpreter) => plain(&[FuncSymbol, GprOut]).call()
            .x86_64(spec().outputs(&[RAX]).fixed(&[(1, RAX)]))
            .aarch64(spec().outputs(&[X0]).fixed(&[(1, X0)])),
        Operation::Call(CallKind::RawNative) => plain(&[GprIn, GprOut, GprOut]).call()
            .x86_64(spec().outputs(&[RAX, RCX]).fixed(&[(1, RAX), (2, RCX)]).scratches(&[R11]))
            .aarch64(spec().outputs(&[X0, X1]).fixed(&[(1, X0), (2, X1)]).scratches(&[X9])),
        Operation::LoadVm => plain(&[GprOut]),
        Operation::LoadProgramCounter => plain(&[GprOut]),
        Operation::StoreOperand => plain(&[Imm, GprInOrImm]).variadic(GprScratch, 1, Some(2), &[])
            .aarch64(spec().selected_variadic(0).scratches(&[X9, X10])),
        Operation::LoadLabel => plain(&[GprOut, Imm]).scratches(&[], &[X9]),
        Operation::Memory(MemoryOperation::Load {
            width: MemoryWidth::Word | MemoryWidth::DoubleWord | MemoryWidth::Float,
            signed: true,
            ..
        }) => unreachable!("unsupported signed load width"),
        Operation::Memory(MemoryOperation::Load {
            width: MemoryWidth::Float,
            signed: false,
        }) => plain(&[FprOut, Memory])
            .pre_scratches(&[], &[X9]),
        Operation::Memory(MemoryOperation::Load { .. }) => plain(&[GprOut, Memory])
            .pre_scratches(&[], &[X9]),
        Operation::Memory(MemoryOperation::Store(
            MemoryWidth::DoubleWord
            | MemoryWidth::Word
            | MemoryWidth::HalfWord
            | MemoryWidth::Byte,
        )) => plain(&[Memory, GprInOrImm])
            .pre_scratches(&[], &[X9, X10]),
        Operation::StoreExecutionContext => plain(&[Memory]).pre_scratches(&[R11], &[X9, X10]),
        Operation::Store64IndexedOffset => plain(&[GprIn, GprIn, Imm, Imm, GprIn])
            .pre_scratches(&[], &[X9]),
        Operation::Memory(MemoryOperation::Store(MemoryWidth::Float)) => plain(&[Memory, FprIn])
            .pre_scratches(&[], &[X9, X10]),
        Operation::Memory(MemoryOperation::LoadPair(_)) => plain(&[GprOut, GprOut, Memory, Memory])
            .pre_scratches(&[], &[X10, X9]),
        Operation::Memory(MemoryOperation::StorePair(_)) => plain(&[Memory, Memory, GprIn, GprIn])
            .pre_scratches(&[], &[X9, X10]),
        Operation::StorePairIndexed(PairWidth::DoubleWord) => plain(&[GprIn, GprIn, Imm, GprIn, GprIn])
            .pre_scratches(&[], &[X10]),
        Operation::Increment32Memory => plain(&[Memory]).pre_scratches(&[], &[X9, X10]),
        Operation::LoadEffectiveAddress => plain(&[GprOut, GprInOrMemory]).pre_scratches(&[], &[X9]),
        Operation::Move(U64) => plain(&[GprOut, GprInOrImm]).coalesces(&[(0, 1)])
            .identity_if_inputs_alias(),
        Operation::Move(U32) => plain(&[GprOut, GprInOrImm]),
        Operation::ExtractTag | Operation::UnboxObject | Operation::BoxInt32 { clean: true } => plain(&[GprOut, GprIn])
            .coalesces(&[(0, 1)]),
        Operation::UnboxInt32 | Operation::BoxInt32 { clean: false } => plain(&[GprOut, GprIn]),
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(Add | Subtract | Or),
            width: U64,
        }
        | Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(And),
            width: U16,
        } => plain(&[GprInOut, GprInOrImm])
            .scratches(&[], &[X9]),
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(Xor),
            width: U64,
        } => plain(&[GprInOut, GprInOrImm])
            .scratches(&[], &[X9]).zeroes_if_inputs_alias(),
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(Multiply),
            width: U64,
        } => plain(&[GprOut, GprIn])
            .variadic(GprInOrImm, 0, Some(1), &[]).aarch64(spec().scratches(&[X9]))
            .coalesces(&[(0, 1), (0, 2)]),
        Operation::Negate | Operation::Not(U64) | Operation::Not(U32) => plain(&[GprInOut]),
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(And),
            width: U64,
        } => plain(&[GprInOut, GprInOrImm])
            .scratches(&[RAX, R11], &[X9]),
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(And | Or | Xor),
            width: U32,
        } => plain(&[GprOut, GprIn, GprInOrImm])
            .scratches(&[], &[X9]).coalesces(&[(0, 1), (0, 2)]),
        Operation::Or32Branch(_) => plain(&[GprOut, GprIn, GprInOrImm]).trailing(&[Label])
            .pre_scratches(&[], &[X9]),
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Shift(_),
            width: U64,
        } => plain(&[GprInOut, GprInOrImm])
            .x86_64(spec().fixed(&[(1, RCX)])),
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Shift(_),
            width: U32,
        } => plain(&[GprOut, GprIn, GprInOrImm])
            .x86_64(spec().fixed(&[(2, RCX)]).scratches(&[R11])).coalesces(&[(0, 1)]),
        Operation::DivMod => plain(&[GprOut, GprOut, GprIn, GprIn])
            .x86_64(spec().outputs(&[RAX, RDX]).fixed(&[(0, RAX), (1, RDX)]))
            .aarch64(spec().scratches(&[X9])),
        Operation::Overflow(AddWithOverflow) | Operation::Overflow(SubtractWithOverflow) => plain(&[GprInOut, GprInOrImm])
            .trailing(&[Label]).pre_scratches(&[], &[X9]),
        Operation::Overflow(Increment)
        | Operation::Overflow(Decrement)
        | Operation::Overflow(Negate) => plain(&[GprInOut, Label]),
        Operation::Overflow(MultiplyWithOverflow) => plain(&[GprInOut, GprIn]).trailing(&[Label])
            .pre_scratches(&[], &[X9]),
        Operation::Overflow(MultiplyCopy) => plain(&[GprOut, GprIn, GprIn]).trailing(&[Label])
            .pre_scratches(&[], &[X9]),
        Operation::ToggleBit | Operation::ClearBit => plain(&[GprInOut, Imm]),
        Operation::Float(FloatingPointOperation::Binary(_)) => plain(&[FprInOut, FprIn]),
        Operation::Float(
            FloatingPointOperation::Unary(_)
            | FloatingPointOperation::Convert(
                FloatConversion::Float32ToFloat64 | FloatConversion::Float64ToFloat32,
            ),
        ) => plain(&[FprOut, FprIn]),
        Operation::FloatMove => plain(&[RegisterOut, RegisterIn]).coalesces(&[(0, 1)])
            .identity_if_inputs_alias(),
        Operation::Float(FloatingPointOperation::Convert(
            FloatConversion::Int32ToFloat64 | FloatConversion::Uint32ToFloat64,
        )) => plain(&[FprOut, GprIn]),
        Operation::Float(FloatingPointOperation::Convert(FloatConversion::Float64ToInt32)) => plain(&[GprOut, FprIn, Label])
            .scratches(&[RCX, XMM3], &[D16]),
        Operation::Float(FloatingPointOperation::Convert(FloatConversion::JavaScriptToInt32)) => plain(&[GprOut, FprIn, Label])
            .scratches(&[RCX], &[D16]),
        Operation::Float(FloatingPointOperation::CanonicalizeNan) => plain(&[GprOut, FprIn]),
        Operation::Branch(BranchOperation::Equality {
            width: U16 | U32 | U64,
            ..
        })
        | Operation::Branch(BranchOperation::Ordered {
            width: U32 | U64,
            ..
        }) => scalar_compare_branch(),
        Operation::Branch(BranchOperation::Tag(_)) => plain(&[GprIn, Imm]).trailing(&[Label])
            .pre_scratches(&[R11], &[X9, X10]),
        Operation::Branch(BranchOperation::Singleton(_)) => plain(&[GprIn, Imm]).trailing(&[Label])
            .pre_scratches(&[R11], &[X9]),
        Operation::Branch(BranchOperation::Memory { .. }) => plain(&[Memory, GprIn])
            .trailing(&[Label]).pre_scratches(&[], &[X9]),
        Operation::Branch(BranchOperation::Equality {
            width: U8,
            ..
        }) => plain(&[Memory, Imm])
            .trailing(&[Label]).pre_scratches(&[], &[X9]),
        Operation::Branch(BranchOperation::Bits {
            width: MemoryWidth::Byte,
            ..
        }) => plain(&[Memory, Imm])
            .trailing(&[Label]).pre_scratches(&[], &[X9, X10]),
        Operation::Branch(BranchOperation::Zero {
            width: U32 | U64,
            ..
        })
        | Operation::Branch(BranchOperation::Sign {
            width: U32 | U64,
            ..
        })
        | Operation::Branch(BranchOperation::ValueBitsNegative) => plain(&[GprIn, Label]),
        Operation::Branch(BranchOperation::Bits {
            width: MemoryWidth::DoubleWord,
            ..
        }) => plain(&[GprIn, GprInOrImm])
            .trailing(&[Label]).pre_scratches(&[], &[X9]),
        Operation::Branch(BranchOperation::Bit(_)) => plain(&[GprIn, Imm, Label]),
        Operation::Branch(BranchOperation::AnyEqual) => plain(&[GprIn])
            .variadic(GprInOrImm, 1, None, &[Label])
            .pre_scratches(&[], &[X9]),
        Operation::Branch(BranchOperation::Float(_)) => plain(&[FprIn, FprIn, Label]),
        Operation::Assertion(AssertionOperation::UnsignedLess | AssertionOperation::UnsignedGreaterOrEqual) => plain(&[GprIn, GprInOrImm])
            .scratches(&[], &[X9]),
        Operation::Assertion(AssertionOperation::NonZero) => plain(&[GprIn]),
        Operation::Assertion(AssertionOperation::TagEqual | AssertionOperation::TagNotEqual) => plain(&[GprIn, GprInOrImm])
            .scratches(&[R11], &[X9, X10]),
        Operation::StorePairIndexed(PairWidth::Word) => unreachable!("indexed 32-bit pair stores are unsupported"),
        Operation::Memory(MemoryOperation::Load64NonZero) => unreachable!("checked loads must be lowered before target selection"),
        Operation::Move(U8 | U16) => unreachable!("narrow moves are unsupported"),
        Operation::IntegerBinary { .. } => unreachable!("unsupported integer operation width"),
        Operation::Not(U8 | U16) => unreachable!("narrow not is unsupported"),
        Operation::Branch(BranchOperation::Ordered {
            width: U8 | U16, ..
        }) => unreachable!("narrow ordered branches are unsupported"),
        Operation::Branch(BranchOperation::Sign {
            width: U8 | U16, ..
        }) => unreachable!("narrow sign branches are unsupported"),
        Operation::Branch(BranchOperation::Zero {
            width: U8 | U16, ..
        }) => unreachable!("narrow zero branches are unsupported"),
        Operation::Branch(BranchOperation::Bits { .. }) => unreachable!("unsupported branch-bits width"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn and_declares_x86_large_immediate_scratch_operands() {
        let info = lookup_operation(Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(BinaryOperation::And),
            width: IntegerWidth::U64,
        });
        assert_eq!(
            info.x86_64.trailing_scratch_registers,
            &[RAX, R11]
        );
    }

    #[test]
    fn direct_value_operations_have_no_scratch_registers() {
        for operation in [
            Operation::BoxInt32 { clean: false },
            Operation::BoxInt32 { clean: true },
            Operation::ToggleBit,
            Operation::ClearBit,
        ] {
            let info = lookup_operation(operation);
            assert!(info.x86_64.trailing_scratch_registers.is_empty());
            assert!(info.aarch64.trailing_scratch_registers.is_empty());
        }
    }

}
