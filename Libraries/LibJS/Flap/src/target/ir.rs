/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Legal target IR before and after physical register allocation.

use crate::Architecture;
use crate::frontend::layout::KnownLayoutConstant;
use crate::identity::HandlerId;
#[cfg(test)]
pub(crate) use crate::low_ir::MemoryAddress;
pub(crate) use crate::low_ir::{AddressRegister, Operand, VirtualRegister, visit_virtual_registers};
use crate::low_ir::{Label, Relocation, cfg::ControlFlowGraph};
use crate::target::description::{ArchitectureOpcode, SelectedOpcode};
use crate::target::registers::{PhysicalRegister, RegisterClass};

macro_rules! machine_conditions {
    ($($variant:ident => $x86_64:literal, $aarch64_suffix:literal, $aarch64:literal;)+) => {
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
        pub(crate) enum MachineCondition {
            $($variant),+
        }

        impl MachineCondition {
            pub(crate) fn x86_64_mnemonic(self) -> &'static str {
                match self {
                    $(Self::$variant => $x86_64),+
                }
            }

            pub(crate) fn aarch64_suffix(self) -> &'static str {
                match self {
                    $(Self::$variant => $aarch64_suffix),+
                }
            }

            pub(crate) fn aarch64_mnemonic(self) -> &'static str {
                match self {
                    $(Self::$variant => $aarch64),+
                }
            }
        }
    };
}

machine_conditions! {
    Equal => "je", "eq", "b.eq";
    NotEqual => "jne", "ne", "b.ne";
    Zero => "jz", "eq", "b.eq";
    NonZero => "jnz", "ne", "b.ne";
    UnsignedLess => "jb", "lo", "b.lo";
    UnsignedLessOrEqual => "jbe", "ls", "b.ls";
    UnsignedGreater => "ja", "hi", "b.hi";
    UnsignedGreaterOrEqual => "jae", "hs", "b.hs";
    SignedLess => "jl", "lt", "b.lt";
    SignedLessOrEqual => "jle", "le", "b.le";
    SignedGreater => "jg", "gt", "b.gt";
    SignedGreaterOrEqual => "jge", "ge", "b.ge";
    Carry => "jc", "hs", "b.hs";
    NotCarry => "jnc", "lo", "b.lo";
    Negative => "js", "mi", "b.mi";
    NotNegative => "jns", "pl", "b.pl";
    FloatUnordered => "jp", "vs", "b.vs";
    FloatEqual => "je", "eq", "b.eq";
    FloatLess => "jb", "mi", "b.mi";
    FloatLessOrEqual => "jbe", "ls", "b.ls";
    FloatGreater => "ja", "gt", "b.gt";
    FloatGreaterOrEqual => "jae", "ge", "b.ge";
}

impl MachineCondition {
    pub(crate) fn from_scalar(condition: crate::target::description::ScalarBranchCondition) -> Self {
        use crate::intrinsic::ComparisonRelation;
        use crate::target::description::ScalarBranchCondition;

        match condition {
            ScalarBranchCondition::Equal => Self::Equal,
            ScalarBranchCondition::NotEqual => Self::NotEqual,
            ScalarBranchCondition::Signed(ComparisonRelation::Less) => Self::SignedLess,
            ScalarBranchCondition::Signed(ComparisonRelation::LessOrEqual) => Self::SignedLessOrEqual,
            ScalarBranchCondition::Signed(ComparisonRelation::Greater) => Self::SignedGreater,
            ScalarBranchCondition::Signed(ComparisonRelation::GreaterOrEqual) => Self::SignedGreaterOrEqual,
            ScalarBranchCondition::Unsigned(ComparisonRelation::Less) => Self::UnsignedLess,
            ScalarBranchCondition::Unsigned(ComparisonRelation::LessOrEqual) => Self::UnsignedLessOrEqual,
            ScalarBranchCondition::Unsigned(ComparisonRelation::Greater) => Self::UnsignedGreater,
            ScalarBranchCondition::Unsigned(ComparisonRelation::GreaterOrEqual) => Self::UnsignedGreaterOrEqual,
        }
    }

    pub(crate) fn from_float(condition: crate::intrinsic::FloatComparison) -> Self {
        use crate::intrinsic::FloatComparison;

        match condition {
            FloatComparison::Unordered => Self::FloatUnordered,
            FloatComparison::Equal => Self::FloatEqual,
            FloatComparison::Less => Self::FloatLess,
            FloatComparison::LessOrEqual => Self::FloatLessOrEqual,
            FloatComparison::Greater => Self::FloatGreater,
            FloatComparison::GreaterOrEqual => Self::FloatGreaterOrEqual,
        }
    }

    pub(crate) fn from_assertion(operation: crate::intrinsic::AssertionOperation) -> Self {
        use crate::intrinsic::AssertionOperation;

        match operation {
            AssertionOperation::UnsignedLess => Self::UnsignedLess,
            AssertionOperation::UnsignedGreaterOrEqual => Self::UnsignedGreaterOrEqual,
            AssertionOperation::NonZero => Self::NonZero,
            AssertionOperation::TagEqual => Self::Equal,
            AssertionOperation::TagNotEqual => Self::NotEqual,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct PhysicalMemoryAddress {
    pub(crate) base: PhysicalRegister,
    pub(crate) index: Option<PhysicalRegister>,
    pub(crate) scale: Option<i64>,
    pub(crate) displacement: Option<i64>,
}

impl PhysicalMemoryAddress {
    /// `[base]`
    pub(crate) fn base(base: PhysicalRegister) -> Self {
        Self {
            base,
            index: None,
            scale: None,
            displacement: None,
        }
    }

    /// `[base + displacement]`, folding a zero displacement away.
    pub(crate) fn offset(base: PhysicalRegister, displacement: i64) -> Self {
        Self {
            displacement: (displacement != 0).then_some(displacement),
            ..Self::base(base)
        }
    }

    /// `[base + index]`
    pub(crate) fn indexed(base: PhysicalRegister, index: PhysicalRegister) -> Self {
        Self {
            index: Some(index),
            ..Self::base(base)
        }
    }

    /// `[base + index * scale + displacement]`, folding a zero displacement away.
    pub(crate) fn scaled(base: PhysicalRegister, index: PhysicalRegister, scale: i64, displacement: i64) -> Self {
        Self {
            scale: Some(scale),
            ..Self::indexed_offset(base, index, displacement)
        }
    }

    /// `[base + index + displacement]`, folding a zero displacement away.
    pub(crate) fn indexed_offset(base: PhysicalRegister, index: PhysicalRegister, displacement: i64) -> Self {
        Self {
            index: Some(index),
            ..Self::offset(base, displacement)
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum PhysicalOperand {
    PhysicalRegister(PhysicalRegister),
    Relocation(Relocation),
    Immediate(i64),
    Address(PhysicalMemoryAddress),
    Label(Label),
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum OperandShape {
    PhysicalRegister(RegisterClass),
    Immediate,
    Address,
    Label,
    Relocation(crate::low_ir::RelocationKind),
}

crate::low_ir::impl_control_flow_operand!(PhysicalOperand);

pub(crate) type Instruction = crate::low_ir::Instruction<Operand, SelectedOpcode>;

#[cfg(test)]
pub(crate) type AllocatedMemoryAddress = PhysicalMemoryAddress;
pub(crate) type AllocatedOperand = PhysicalOperand;
pub(crate) type MachineMemoryAddress = PhysicalMemoryAddress;
pub(crate) type MachineOperand = PhysicalOperand;

impl PhysicalOperand {
    pub(crate) fn physical_register(&self) -> Option<PhysicalRegister> {
        match self {
            Self::PhysicalRegister(register) => Some(*register),
            _ => None,
        }
    }

    pub(crate) fn shape(&self) -> OperandShape {
        match self {
            Self::PhysicalRegister(register) => OperandShape::PhysicalRegister(register.class()),
            Self::Relocation(relocation) => OperandShape::Relocation(relocation.kind()),
            Self::Immediate(_) => OperandShape::Immediate,
            Self::Address(_) => OperandShape::Address,
            Self::Label(_) => OperandShape::Label,
        }
    }

    #[cfg(test)]
    pub(crate) fn register_name(&self) -> Option<&str> {
        match self {
            Self::PhysicalRegister(register) => Some(register.as_str()),
            _ => None,
        }
    }

    #[cfg(test)]
    pub(crate) fn selection_operand(&self) -> Operand {
        match self {
            Self::PhysicalRegister(register) => Operand::PhysicalRegister(*register),
            Self::Relocation(relocation) => Operand::Relocation(relocation.clone()),
            Self::Immediate(value) => Operand::Immediate(*value),
            Self::Label(label) => Operand::Label(label.clone()),
            Self::Address(address) => Operand::Address(MemoryAddress {
                base: AddressRegister::Physical(address.base),
                index: address.index.map(AddressRegister::Physical),
                scale: address.scale.map(crate::low_ir::AddressScale::Immediate),
                displacement: address.displacement.map(crate::low_ir::AddressDisplacement::Immediate),
            }),
        }
    }
}

pub(crate) type AllocatedInstruction = crate::low_ir::Instruction<AllocatedOperand, SelectedOpcode>;

pub(crate) use crate::low_ir::RuntimeConstants;

/// A legal target function before physical register allocation.
pub(crate) struct Function {
    pub(crate) id: HandlerId,
    pub(crate) name: String,
    pub(crate) size: Option<u32>,
    pub(crate) is_cold: bool,
    pub(crate) architecture: Architecture,
    pub(crate) virtual_registers: Vec<VirtualRegister>,
    pub(crate) instructions: Vec<Instruction>,
}

/// A target program whose instruction selection is complete.
pub struct Program {
    pub(crate) runtime: RuntimeConstants,
    pub(crate) dispatch_handlers: Vec<Option<HandlerId>>,
    pub(crate) architecture: Architecture,
    pub(crate) functions: Vec<Function>,
}

/// One machine function whose virtual registers and CFG are finalized.
pub(crate) struct AllocatedFunction {
    pub(crate) id: HandlerId,
    pub(crate) name: String,
    pub(crate) size: Option<u32>,
    pub(crate) is_cold: bool,
    pub(crate) cfg: ControlFlowGraph<AllocatedOperand, SelectedOpcode>,
}

/// An allocated machine program ready for post-allocation finalization.
pub struct AllocatedProgram {
    pub(crate) runtime: RuntimeConstants,
    pub(crate) dispatch_handlers: Vec<Option<HandlerId>>,
    pub(crate) architecture: Architecture,
    pub(crate) functions: Vec<AllocatedFunction>,
}

/// The architecture-owned opcode of a finalized machine instruction.
pub(crate) type MachineOpcode = ArchitectureOpcode;

/// A finalized physical machine instruction.
pub(crate) type MachineInstruction = crate::low_ir::Instruction<MachineOperand, MachineOpcode>;

impl crate::low_ir::Instruction<MachineOperand, MachineOpcode> {
    pub(crate) fn operand(&self, index: usize) -> &MachineOperand {
        self.operands
            .get(index)
            .expect("machine instruction operands were verified")
    }

    pub(crate) fn physical_register(&self, index: usize) -> PhysicalRegister {
        let MachineOperand::PhysicalRegister(register) = self.operand(index) else {
            unreachable!("machine instruction operand kind was verified")
        };
        *register
    }

    pub(crate) fn relocation(&self, index: usize) -> &Relocation {
        let MachineOperand::Relocation(relocation) = self.operand(index) else {
            unreachable!("machine instruction operand kind was verified")
        };
        relocation
    }

    pub(crate) fn immediate(&self, index: usize) -> i64 {
        let MachineOperand::Immediate(immediate) = self.operand(index) else {
            unreachable!("machine instruction operand kind was verified")
        };
        *immediate
    }
}

/// A finalized physical machine function with resolved hot and cold layout.
#[derive(Debug)]
pub(crate) struct MachineFunction {
    pub(crate) id: HandlerId,
    pub(crate) name: String,
    pub(crate) is_cold: bool,
    pub(crate) hot_instructions: Vec<MachineInstruction>,
    pub(crate) cold_instructions: Vec<MachineInstruction>,
}

/// A finalized physical machine program ready for textual printing.
pub struct MachineProgram {
    pub(crate) runtime: RuntimeConstants,
    pub(crate) dispatch_handlers: Vec<Option<HandlerId>>,
    pub(crate) target: crate::Target,
    pub(crate) functions: Vec<MachineFunction>,
}

impl MachineProgram {
    pub(crate) fn missing_required_runtime_constant(&self) -> Option<&'static str> {
        use KnownLayoutConstant::*;

        let architecture_constants: &[KnownLayoutConstant] = match self.target.architecture {
            Architecture::X86_64 => &[],
            Architecture::Aarch64 => &[Int32Tag, BooleanTag],
        };
        architecture_constants
            .iter()
            .chain(
                [
                    VmRunningExecutionContext,
                    VmBreakpointController,
                    VmHeapRegionBase,
                    ExecutionContextExecutable,
                    ExecutionContextProgramCounter,
                    ExecutableBytecodeData,
                    SizeOfExecutionContext,
                    CanonicalNanBits,
                ]
                .iter(),
            )
            .find_map(|constant| self.runtime.get(*constant).is_none().then_some(constant.name()))
    }
}

impl Program {
    pub fn architecture(&self) -> Architecture {
        self.architecture
    }
}

impl AllocatedProgram {
    pub fn architecture(&self) -> Architecture {
        self.architecture
    }
}

impl MachineProgram {
    pub fn architecture(&self) -> Architecture {
        self.target.architecture
    }
}
