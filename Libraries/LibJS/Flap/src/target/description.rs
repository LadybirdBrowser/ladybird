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
//! x21 (ip cache), and x22-x23 (tag constants), plus x24 for the heap region
//! base; x86-64 pins values and the heap region base, and derives exec_ctx
//! from values.
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
    AssertionOperation, BranchOperation, CallOperation as CallKind, ControlOperation, EqualityCondition,
    FloatBinaryOperation, FloatConversion, FloatUnaryOperation, FloatingPointOperation, IntegerBinaryOperation,
    IntegerSignedness, IntegerWidth, MemoryOperation, MemoryWidth, PairWidth, SignCondition, TestCondition,
    ZeroCondition,
};
use crate::target::ir::Operand;
use crate::target::registers::{PhysicalRegister, RegisterClass, aarch64, x86_64};
use crate::target::{aarch64 as aarch64_target, x86_64 as x86_64_target};

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
    RecoverAddLhs,
    RecoverSubtractLhs,
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
    IntegerBinary {
        operation: IntegerBinaryOperation,
        width: IntegerWidth,
    },
    Or32Branch(SignCondition),
    Negate,
    Not(IntegerWidth),
    Modulo(IntegerWidth),
    Overflow(OverflowOperation),
    ExtractTag,
    UnboxInt32,
    UnboxObject,
    BoxInt32 {
        clean: bool,
    },
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
                Architecture::X86_64 => {
                    ArchitectureOpcode::X86_64(x86_64_target::Opcode::select_for_operands(operation, operands))
                }
                Architecture::Aarch64 => {
                    ArchitectureOpcode::Aarch64(aarch64_target::Opcode::select_for_operands(operation, operands))
                }
            },
        }
    }

    pub(crate) fn description(&self) -> &'static InstructionDescription {
        lookup_operation(*self)
    }
}

impl SelectedOpcode {
    pub(crate) fn for_operation(operation: Operation, architecture: Architecture) -> Self {
        Self {
            operation,
            opcode: match architecture {
                Architecture::X86_64 => ArchitectureOpcode::X86_64(x86_64_target::Opcode::select(operation)),
                Architecture::Aarch64 => ArchitectureOpcode::Aarch64(aarch64_target::Opcode::select(operation)),
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

    pub(crate) fn description(&self) -> &'static InstructionDescription {
        match self.opcode {
            ArchitectureOpcode::X86_64(x86_64_target::Opcode::StoreLargeImmediate(_)) => {
                &X86_64_LARGE_IMMEDIATE_STORE_DESCRIPTION
            }
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
                Operand::Immediate(value) if !(i32::MIN as i64..=i32::MAX as i64).contains(value) => {
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
        Self::Memory(MemoryOperation::Load { width, signed })
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
    /// GPR input, immediate, or memory address.
    GprInOrImmOrMemory,
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
            Self::GprIn | Self::GprOut | Self::GprInOut => register_class == Some(RegisterClass::GeneralPurpose),
            Self::GprScratch => shape == OperandShape::PhysicalRegister(RegisterClass::GeneralPurpose),
            Self::FprIn | Self::FprOut | Self::FprInOut => register_class == Some(RegisterClass::FloatingPoint),
            Self::FprScratch => shape == OperandShape::PhysicalRegister(RegisterClass::FloatingPoint),
            Self::RegisterIn | Self::RegisterOut => register_class.is_some(),
            Self::GprInOrImm => {
                register_class == Some(RegisterClass::GeneralPurpose) || shape == OperandShape::Immediate
            }
            Self::GprInOrMemory => {
                register_class == Some(RegisterClass::GeneralPurpose) || shape == OperandShape::Address
            }
            Self::GprInOrImmOrMemory => {
                register_class == Some(RegisterClass::GeneralPurpose)
                    || matches!(shape, OperandShape::Immediate | OperandShape::Address)
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
    const fn scratches(mut self, x86_64: &'static [PhysicalRegister], aarch64: &'static [PhysicalRegister]) -> Self {
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

    pub(crate) fn accepts_selected_operand_count(&self, count: usize, architecture: Architecture) -> bool {
        let arch = self.arch(architecture);
        let scratch_count =
            arch.trailing_scratch_registers.len() + arch.scratch_registers_before_trailing_operands.len();
        let Some(count) = count.checked_sub(scratch_count) else {
            return false;
        };
        if let Some(variadic_count) = self.arch(architecture).selected_variadic_operands {
            return self.variadic_kind.is_some()
                && count == self.operands.len() + variadic_count + self.trailing_operands.len();
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
        let scratch_registers = self.arch(architecture).trailing_scratch_registers;
        let before_trailing = self.arch(architecture).scratch_registers_before_trailing_operands;
        let total_scratch_count = scratch_registers.len() + before_trailing.len();
        let semantic_count = operand_count.checked_sub(total_scratch_count)?;
        let before_trailing_end = operand_count
            .checked_sub(scratch_registers.len())?
            .checked_sub(self.trailing_operands.len())?;
        let before_trailing_start = before_trailing_end.checked_sub(before_trailing.len())?;
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

const fn scalar_equality_branch() -> InstructionDescription {
    plain(&[GprInOrImm, GprInOrImm])
        .trailing(&[Label])
        .pre_scratches(&[], &[X9])
}

const fn scalar_ordered_branch() -> InstructionDescription {
    plain(&[GprIn, GprInOrImm]).trailing(&[Label]).pre_scratches(&[], &[X9])
}

use OperandKind::*;
use aarch64::{D16, X0, X1, X9, X10};
use x86_64::{R11, RAX, RCX, RDX, XMM3};

const X86_64_LARGE_IMMEDIATE_STORE_DESCRIPTION: InstructionDescription =
    plain(&[Memory, GprInOrImm]).scratches(&[R11], &[]);

fn lookup_operation(operation: Operation) -> &'static InstructionDescription {
    use BinaryOperation::*;
    use IntegerWidth::*;
    use OverflowOperation::*;

    match operation {
        Operation::Cold => &const { plain(&[Label]) },
        Operation::Label => &const { plain(&[Label]) },
        Operation::Control(ControlOperation::JumpLabel) => &const { plain(&[Label]).terminal() },
        Operation::Control(ControlOperation::Exit) => &const { plain(&[]).terminal() },
        Operation::Control(ControlOperation::DispatchNext | ControlOperation::Dispatch) => {
            &const { plain(&[]).terminal().scratches(&[RAX], &[X9, X10]) }
        }
        Operation::Control(ControlOperation::DispatchVariable | ControlOperation::GotoHandler) => {
            &const { plain(&[GprIn]).terminal().scratches(&[RAX], &[X9, X10]) }
        }
        Operation::Control(ControlOperation::JumpBytecode) => {
            &const { plain(&[Imm]).terminal().scratches(&[RAX], &[X0, X9, X10]) }
        }
        Operation::Call(CallKind::SlowPath) => {
            &const {
                plain(&[FuncSymbol])
                    .terminal()
                    .call()
                    .x86_64(spec().scratches(&[RAX, RCX]))
                    .aarch64(spec().scratches(&[X9, X10]))
            }
        }
        Operation::Call(CallKind::BinarySlowPath) => {
            &const {
                plain(&[FuncSymbol, GprIn, GprIn, GprIn])
                    .terminal()
                    .call()
                    .x86_64(spec().scratches(&[RAX, R11]))
                    .aarch64(spec().scratches(&[X9, X10]))
            }
        }
        Operation::Call(CallKind::JumpSlowPath) => {
            &const {
                plain(&[FuncSymbol, GprIn, GprIn, GprIn, GprIn])
                    .terminal()
                    .call()
                    .x86_64(spec().scratches(&[RAX, R11]))
                    .aarch64(spec().scratches(&[X9, X10]))
            }
        }
        Operation::Call(CallKind::Helper) => {
            &const {
                plain(&[FuncSymbol, GprIn, GprOut])
                    .call()
                    .x86_64(spec().inputs(&[RCX]).outputs(&[RAX]).fixed(&[(1, RCX), (2, RAX)]))
                    .aarch64(spec().inputs(&[X0]).outputs(&[X0]).fixed(&[(1, X0), (2, X0)]))
            }
        }
        Operation::Call(CallKind::Interpreter) => {
            &const {
                plain(&[FuncSymbol, GprOut])
                    .call()
                    .x86_64(spec().outputs(&[RAX]).fixed(&[(1, RAX)]))
                    .aarch64(spec().outputs(&[X0]).fixed(&[(1, X0)]))
            }
        }
        Operation::Call(CallKind::RawNative) => {
            &const {
                plain(&[GprIn, GprOut, GprOut])
                    .call()
                    .x86_64(
                        spec()
                            .outputs(&[RAX, RCX])
                            .fixed(&[(1, RAX), (2, RCX)])
                            .scratches(&[R11]),
                    )
                    .aarch64(spec().outputs(&[X0, X1]).fixed(&[(1, X0), (2, X1)]).scratches(&[X9]))
            }
        }
        Operation::LoadVm => &const { plain(&[GprOut]) },
        Operation::LoadProgramCounter => &const { plain(&[GprOut]) },
        Operation::StoreOperand => {
            &const {
                plain(&[Imm, GprInOrImm])
                    .variadic(GprScratch, 1, Some(2), &[])
                    .aarch64(spec().selected_variadic(0).scratches(&[X9, X10]))
            }
        }
        Operation::LoadLabel => &const { plain(&[GprOut, Imm]).scratches(&[], &[X9]) },
        Operation::Memory(MemoryOperation::Load {
            width: MemoryWidth::Word | MemoryWidth::DoubleWord | MemoryWidth::Float,
            signed: true,
            ..
        }) => unreachable!("unsupported signed load width"),
        Operation::Memory(MemoryOperation::Load {
            width: MemoryWidth::Float,
            signed: false,
        }) => &const { plain(&[FprOut, Memory]).pre_scratches(&[], &[X9]) },
        Operation::Memory(MemoryOperation::Load { .. }) => {
            &const { plain(&[GprOut, Memory]).pre_scratches(&[], &[X9]) }
        }
        Operation::Memory(MemoryOperation::Store(
            MemoryWidth::DoubleWord | MemoryWidth::Word | MemoryWidth::HalfWord | MemoryWidth::Byte,
        )) => &const { plain(&[Memory, GprInOrImm]).pre_scratches(&[], &[X9, X10]) },
        Operation::StoreExecutionContext => &const { plain(&[Memory]).pre_scratches(&[R11], &[X9, X10]) },
        Operation::Store64IndexedOffset => &const { plain(&[GprIn, GprIn, Imm, Imm, GprIn]).pre_scratches(&[], &[X9]) },
        Operation::Memory(MemoryOperation::Store(MemoryWidth::Float)) => {
            &const { plain(&[Memory, FprIn]).pre_scratches(&[], &[X9, X10]) }
        }
        Operation::Memory(MemoryOperation::LoadPair(_)) => {
            &const { plain(&[GprOut, GprOut, Memory, Memory]).pre_scratches(&[], &[X10, X9]) }
        }
        Operation::Memory(MemoryOperation::StorePair(_)) => {
            &const { plain(&[Memory, Memory, GprIn, GprIn]).pre_scratches(&[], &[X9, X10]) }
        }
        Operation::StorePairIndexed(PairWidth::DoubleWord) => {
            &const { plain(&[GprIn, GprIn, Imm, GprIn, GprIn]).pre_scratches(&[], &[X10]) }
        }
        Operation::Increment32Memory => &const { plain(&[Memory]).pre_scratches(&[], &[X9, X10]) },
        Operation::LoadEffectiveAddress => &const { plain(&[GprOut, GprInOrMemory]).pre_scratches(&[], &[X9]) },
        Operation::Move(U64) => {
            &const {
                plain(&[GprOut, GprInOrImm])
                    .coalesces(&[(0, 1)])
                    .identity_if_inputs_alias()
            }
        }
        Operation::Move(U32) => &const { plain(&[GprOut, GprInOrImm]) },
        Operation::ExtractTag => &const { plain(&[GprOut, GprIn]).coalesces(&[(0, 1)]) },
        Operation::BoxInt32 { clean: true } => {
            &const { plain(&[GprOut, GprIn]).coalesces(&[(0, 1)]).scratches(&[R11], &[]) }
        }
        Operation::UnboxObject => &const { plain(&[GprOut, GprIn]).coalesces(&[(0, 1)]).scratches(&[R11], &[]) },
        Operation::UnboxInt32 => &const { plain(&[GprOut, GprIn]) },
        Operation::BoxInt32 { clean: false } => &const { plain(&[GprOut, GprIn]).scratches(&[R11], &[]) },
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(Add | Subtract | Or),
            width: U64,
        }
        | Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(And),
            width: U16,
        } => &const { plain(&[GprInOut, GprInOrImm]).scratches(&[], &[X9]) },
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(Xor),
            width: U64,
        } => {
            &const {
                plain(&[GprInOut, GprInOrImm])
                    .scratches(&[], &[X9])
                    .zeroes_if_inputs_alias()
            }
        }
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(Multiply),
            width: U64,
        } => {
            &const {
                plain(&[GprOut, GprIn])
                    .variadic(GprInOrImm, 0, Some(1), &[])
                    .aarch64(spec().scratches(&[X9]))
                    .coalesces(&[(0, 1), (0, 2)])
            }
        }
        Operation::Negate | Operation::Not(U64) | Operation::Not(U32) => &const { plain(&[GprInOut]) },
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(And),
            width: U64,
        } => &const { plain(&[GprInOut, GprInOrImm]).scratches(&[RAX, R11], &[X9]) },
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(And | Or | Xor),
            width: U32,
        } => {
            &const {
                plain(&[GprOut, GprIn, GprInOrImm])
                    .scratches(&[], &[X9])
                    .coalesces(&[(0, 1), (0, 2)])
            }
        }
        Operation::Or32Branch(_) => {
            &const {
                plain(&[GprOut, GprIn, GprInOrImm])
                    .trailing(&[Label])
                    .pre_scratches(&[], &[X9])
            }
        }
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Shift(_),
            width: U64,
        } => &const { plain(&[GprInOut, GprInOrImm]).x86_64(spec().fixed(&[(1, RCX)])) },
        Operation::IntegerBinary {
            operation: IntegerBinaryOperation::Shift(_),
            width: U32,
        } => {
            &const {
                plain(&[GprOut, GprIn, GprInOrImm])
                    .x86_64(spec().fixed(&[(2, RCX)]).scratches(&[R11]))
                    .coalesces(&[(0, 1)])
            }
        }
        Operation::Modulo(_) => {
            &const {
                plain(&[GprOut, GprIn, GprIn])
                    .x86_64(spec().outputs(&[RDX]).fixed(&[(0, RDX)]).scratches(&[RAX]))
                    .aarch64(spec().scratches(&[X9]))
            }
        }
        Operation::Overflow(AddWithOverflow) | Operation::Overflow(SubtractWithOverflow) => {
            &const {
                plain(&[GprInOut, GprInOrImmOrMemory])
                    .trailing(&[Label])
                    .pre_scratches(&[], &[X9])
            }
        }
        Operation::Overflow(RecoverAddLhs) | Operation::Overflow(RecoverSubtractLhs) => {
            &const { plain(&[GprInOut, GprInOrImmOrMemory]).pre_scratches(&[], &[X9]) }
        }
        Operation::Overflow(Increment) | Operation::Overflow(Decrement) | Operation::Overflow(Negate) => {
            &const { plain(&[GprInOut, Label]) }
        }
        Operation::Overflow(MultiplyWithOverflow) => {
            &const { plain(&[GprInOut, GprIn]).trailing(&[Label]).pre_scratches(&[], &[X9]) }
        }
        Operation::Overflow(MultiplyCopy) => {
            &const {
                plain(&[GprOut, GprIn, GprIn])
                    .trailing(&[Label])
                    .pre_scratches(&[], &[X9])
            }
        }
        Operation::ToggleBit | Operation::ClearBit => &const { plain(&[GprInOut, Imm]) },
        Operation::Float(FloatingPointOperation::Binary(_)) => &const { plain(&[FprInOut, FprIn]) },
        Operation::Float(
            FloatingPointOperation::Unary(_)
            | FloatingPointOperation::Convert(FloatConversion::Float32ToFloat64 | FloatConversion::Float64ToFloat32),
        ) => &const { plain(&[FprOut, FprIn]) },
        Operation::FloatMove => {
            &const {
                plain(&[RegisterOut, RegisterIn])
                    .coalesces(&[(0, 1)])
                    .identity_if_inputs_alias()
            }
        }
        Operation::Float(FloatingPointOperation::Convert(
            FloatConversion::Int32ToFloat64 | FloatConversion::Int64ToFloat64 | FloatConversion::Uint32ToFloat64,
        )) => &const { plain(&[FprOut, GprIn]) },
        Operation::Float(FloatingPointOperation::Convert(FloatConversion::Float64ToInt64)) => {
            &const { plain(&[GprOut, FprIn, Label]).scratches(&[XMM3], &[D16]) }
        }
        Operation::Float(FloatingPointOperation::Convert(FloatConversion::Float64ToInt32)) => {
            &const { plain(&[GprOut, FprIn, Label]).scratches(&[RCX, XMM3], &[D16]) }
        }
        Operation::Float(FloatingPointOperation::Convert(FloatConversion::JavaScriptToInt32)) => {
            &const { plain(&[GprOut, FprIn, Label]).scratches(&[RCX], &[D16]) }
        }
        Operation::Float(FloatingPointOperation::CanonicalizeNan) => &const { plain(&[GprOut, FprIn]) },
        Operation::Branch(BranchOperation::Equality {
            width: U16 | U32 | U64, ..
        }) => &const { scalar_equality_branch() },
        Operation::Branch(BranchOperation::Ordered { width: U32 | U64, .. }) => &const { scalar_ordered_branch() },
        Operation::Branch(BranchOperation::Tag(_)) => {
            &const {
                plain(&[GprIn, Imm])
                    .trailing(&[Label])
                    .pre_scratches(&[R11], &[X9, X10])
            }
        }
        Operation::Branch(BranchOperation::Singleton(_)) => {
            &const { plain(&[GprIn, Imm]).trailing(&[Label]).pre_scratches(&[R11], &[X9]) }
        }
        Operation::Branch(BranchOperation::Memory { .. }) => {
            &const { plain(&[Memory, GprIn]).trailing(&[Label]).pre_scratches(&[], &[X9]) }
        }
        Operation::Branch(BranchOperation::Equality { width: U8, .. }) => {
            &const { plain(&[Memory, Imm]).trailing(&[Label]).pre_scratches(&[], &[X9]) }
        }
        Operation::Branch(BranchOperation::Bits {
            width: MemoryWidth::Byte,
            ..
        }) => &const { plain(&[Memory, Imm]).trailing(&[Label]).pre_scratches(&[], &[X9, X10]) },
        Operation::Branch(BranchOperation::Zero { width: U32 | U64, .. })
        | Operation::Branch(BranchOperation::Sign { width: U32 | U64, .. })
        | Operation::Branch(BranchOperation::ValueBitsNegative) => &const { plain(&[GprIn, Label]) },
        Operation::Branch(BranchOperation::Bits {
            width: MemoryWidth::DoubleWord,
            ..
        }) => &const { plain(&[GprIn, GprInOrImm]).trailing(&[Label]).pre_scratches(&[], &[X9]) },
        Operation::Branch(BranchOperation::Bit(_)) => &const { plain(&[GprIn, Imm, Label]) },
        Operation::Branch(BranchOperation::AnyEqual) => {
            &const {
                plain(&[GprIn])
                    .variadic(GprInOrImm, 1, None, &[Label])
                    .pre_scratches(&[], &[X9])
            }
        }
        Operation::Branch(BranchOperation::Float(_)) => &const { plain(&[FprIn, FprIn, Label]) },
        Operation::Assertion(AssertionOperation::UnsignedLess | AssertionOperation::UnsignedGreaterOrEqual) => {
            &const { plain(&[GprIn, GprInOrImm]).scratches(&[], &[X9]) }
        }
        Operation::Assertion(AssertionOperation::NonZero) => &const { plain(&[GprIn]) },
        Operation::Assertion(AssertionOperation::TagEqual | AssertionOperation::TagNotEqual) => {
            &const { plain(&[GprIn, GprInOrImm]).scratches(&[R11], &[X9, X10]) }
        }
        Operation::StorePairIndexed(PairWidth::Word) => unreachable!("indexed 32-bit pair stores are unsupported"),
        Operation::Memory(
            MemoryOperation::Load64NonZero | MemoryOperation::LoadCellPointer | MemoryOperation::LoadNonnullCellPointer,
        ) => {
            unreachable!("specialized loads must be lowered before target selection")
        }
        Operation::Move(U8 | U16) => unreachable!("narrow moves are unsupported"),
        Operation::IntegerBinary { .. } => unreachable!("unsupported integer operation width"),
        Operation::Not(U8 | U16) => unreachable!("narrow not is unsupported"),
        Operation::Branch(BranchOperation::Ordered { width: U8 | U16, .. }) => {
            unreachable!("narrow ordered branches are unsupported")
        }
        Operation::Branch(BranchOperation::Sign { width: U8 | U16, .. }) => {
            unreachable!("narrow sign branches are unsupported")
        }
        Operation::Branch(BranchOperation::Zero { width: U8 | U16, .. }) => {
            unreachable!("narrow zero branches are unsupported")
        }
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
        assert_eq!(info.x86_64.trailing_scratch_registers, &[RAX, R11]);
    }

    #[test]
    fn direct_value_operations_declare_their_scratch_registers() {
        for operation in [
            Operation::BoxInt32 { clean: false },
            Operation::BoxInt32 { clean: true },
        ] {
            let info = lookup_operation(operation);
            assert_eq!(info.x86_64.trailing_scratch_registers, &[R11]);
            assert!(info.aarch64.trailing_scratch_registers.is_empty());
        }

        for operation in [Operation::ToggleBit, Operation::ClearBit] {
            let info = lookup_operation(operation);
            assert!(info.x86_64.trailing_scratch_registers.is_empty());
            assert!(info.aarch64.trailing_scratch_registers.is_empty());
        }
    }

    #[test]
    fn aarch64_slow_path_dispatch_preserves_the_return_register() {
        for kind in [CallKind::BinarySlowPath, CallKind::JumpSlowPath] {
            let info = lookup_operation(Operation::Call(kind));
            assert_eq!(info.aarch64.trailing_scratch_registers, &[X9, X10]);
            assert!(!info.aarch64.trailing_scratch_registers.contains(&X0));
        }
    }
}
