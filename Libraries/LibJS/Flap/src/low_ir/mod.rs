/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Target-independent low-level IR before instruction selection.

pub(crate) mod cfg;
pub(crate) mod lowering;
pub(crate) mod optimize;
pub(crate) mod preallocate;

use crate::ssa as ir;
use crate::bytecode::BytecodeFieldId;
use crate::frontend::layout::{
    KnownLayoutConstant, LayoutConstant, LayoutConstants,
};
use crate::identity::{ExternalSymbol, HandlerId};
use crate::types::{InterpreterRegister, RegisterClass};
use crate::target::registers::PhysicalRegister;
use crate::target::description::{
    InstructionDescription, Operation, SelectedOpcode,
};
use std::cmp::Ordering;
use std::collections::{BTreeSet, HashMap, hash_map::DefaultHasher};
use std::fmt;
use std::hash::{Hash, Hasher};
use std::ops::Deref;
use std::sync::Arc;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum VirtualRegisterOrigin {
    Named,
    Ssa,
    SsaEdgeTemporary,
}

/// The identity of a machine register before register allocation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct VirtualRegisterId {
    function: HandlerId,
    index: u32,
}

/// A virtual register and its non-semantic debug name.
#[derive(Debug, Clone)]
pub(crate) struct VirtualRegister {
    id: Option<VirtualRegisterId>,
    name: Arc<str>,
    class: RegisterClass,
    origin: VirtualRegisterOrigin,
    identity_hash: u64,
}

impl VirtualRegister {
    pub(crate) fn new(name: impl Into<String>) -> Self {
        Self::with_class(name, RegisterClass::GeneralPurpose)
    }

    pub(crate) fn with_class(name: impl Into<String>, class: RegisterClass) -> Self {
        let name = name.into();
        let origin = if name.starts_with("ssa_edge_temporary_") {
            VirtualRegisterOrigin::SsaEdgeTemporary
        } else if name.starts_with("ssa_") {
            VirtualRegisterOrigin::Ssa
        } else {
            VirtualRegisterOrigin::Named
        };
        let mut hasher = DefaultHasher::new();
        name.hash(&mut hasher);
        class.hash(&mut hasher);
        Self {
            id: None,
            name: name.into(),
            class,
            origin,
            identity_hash: hasher.finish(),
        }
    }

    pub(crate) fn class(&self) -> RegisterClass {
        self.class
    }

    pub(crate) fn id(&self) -> Option<VirtualRegisterId> {
        self.id
    }

    pub(crate) fn is_ssa(&self) -> bool {
        matches!(
            self.origin,
            VirtualRegisterOrigin::Ssa | VirtualRegisterOrigin::SsaEdgeTemporary
        )
    }

    pub(crate) fn is_edge_temporary(&self) -> bool {
        self.origin == VirtualRegisterOrigin::SsaEdgeTemporary
    }
}

impl PartialEq for VirtualRegister {
    fn eq(&self, other: &Self) -> bool {
        match (self.id, other.id) {
            (Some(lhs), Some(rhs)) => lhs == rhs,
            (None, None) => {
                self.name == other.name && self.class == other.class
            }
            _ => false,
        }
    }
}

impl Eq for VirtualRegister {}

impl PartialOrd for VirtualRegister {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for VirtualRegister {
    fn cmp(&self, other: &Self) -> Ordering {
        match (self.id, other.id) {
            (Some(lhs), Some(rhs)) => lhs.cmp(&rhs),
            (None, None) => self
                .name
                .cmp(&other.name)
                .then_with(|| self.class.cmp(&other.class)),
            (None, Some(_)) => Ordering::Less,
            (Some(_), None) => Ordering::Greater,
        }
    }
}

impl Hash for VirtualRegister {
    fn hash<H: Hasher>(&self, state: &mut H) {
        match self.id {
            Some(id) => id.hash(state),
            None => self.identity_hash.hash(state),
        }
    }
}

impl From<&str> for VirtualRegister {
    fn from(name: &str) -> Self {
        Self::new(name)
    }
}

impl From<String> for VirtualRegister {
    fn from(name: String) -> Self {
        Self::new(name)
    }
}

impl fmt::Display for VirtualRegister {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.name.fmt(formatter)
    }
}

/// The semantic use that a future machine-code encoder must relocate.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum RelocationKind {
    FunctionCall,
}

/// A symbolic machine-code reference whose relocation semantics are known.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub(crate) struct Relocation {
    symbol: ExternalSymbol,
    kind: RelocationKind,
}

impl Relocation {
    pub(crate) fn function_call(symbol: ExternalSymbol) -> Self {
        Self {
            symbol,
            kind: RelocationKind::FunctionCall,
        }
    }

    pub(crate) fn kind(&self) -> RelocationKind {
        self.kind
    }

    pub(crate) fn as_str(&self) -> &str {
        self.symbol.as_str()
    }
}

impl fmt::Display for Relocation {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.symbol.fmt(formatter)
    }
}

/// A branch target within a machine function.
#[derive(Debug, Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct Label(String);

impl Label {
    pub(crate) fn new(name: impl Into<String>) -> Self {
        Self(name.into())
    }

    pub(crate) fn as_str(&self) -> &str {
        &self.0
    }
}

impl From<&str> for Label {
    fn from(name: &str) -> Self {
        Self::new(name)
    }
}

impl From<String> for Label {
    fn from(name: String) -> Self {
        Self::new(name)
    }
}

impl fmt::Display for Label {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

impl Deref for Label {
    type Target = str;

    fn deref(&self) -> &Self::Target {
        self.as_str()
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum AddressRegister<
    V = VirtualRegister,
    I = InterpreterRegister,
> {
    Virtual(V),
    Interpreter(I),
    Physical(PhysicalRegister),
}

impl AddressRegister<VirtualRegister> {
    #[cfg(test)]
    pub(crate) fn named(name: impl Into<String>) -> Self {
        let name = name.into();
        InterpreterRegister::from_name(&name)
            .map(Self::Interpreter)
            .unwrap_or_else(|| Self::Virtual(VirtualRegister::new(name)))
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum AddressScale {
    Immediate(i64),
    LayoutConstant(LayoutConstant),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum AddressDisplacement {
    Immediate(i64),
    LayoutConstant(LayoutConstant),
    BytecodeField(BytecodeFieldId),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct MemoryAddress {
    pub(crate) base: AddressRegister,
    pub(crate) index: Option<AddressRegister>,
    pub(crate) scale: Option<AddressScale>,
    pub(crate) displacement: Option<AddressDisplacement>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum Operand {
    VirtualRegister(VirtualRegister),
    InterpreterRegister(InterpreterRegister),
    PhysicalRegister(PhysicalRegister),
    Relocation(Relocation),
    Immediate(i64),
    ValueConstant(i64),
    LayoutConstant(LayoutConstant),
    Address(MemoryAddress),
    Label(Label),
    BytecodeField(BytecodeFieldId),
}

impl Operand {
    pub(crate) fn references(&self, register: &VirtualRegister) -> bool {
        match self {
            Self::VirtualRegister(candidate) => candidate == register,
            Self::Address(address) => std::iter::once(&address.base)
                .chain(&address.index)
                .any(|candidate| matches!(candidate, AddressRegister::Virtual(candidate) if candidate == register)),
            _ => false,
        }
    }
}

pub(crate) trait ControlFlowOperand: Clone {
    fn label(&self) -> Option<&Label>;
    fn label_mut(&mut self) -> Option<&mut Label>;
    fn from_label(label: Label) -> Self;
}

pub(crate) trait ControlFlowOpcode: Clone + fmt::Debug {
    fn operation(&self) -> Operation;
    fn description(&self) -> InstructionDescription;
    fn replacing_operation(&self, operation: Operation) -> Self;
}

impl ControlFlowOpcode for Operation {
    fn operation(&self) -> Operation {
        *self
    }

    fn description(&self) -> InstructionDescription {
        self.description()
    }

    fn replacing_operation(&self, operation: Operation) -> Self {
        operation
    }
}

impl ControlFlowOpcode for SelectedOpcode {
    fn operation(&self) -> Operation {
        self.operation()
    }

    fn description(&self) -> InstructionDescription {
        self.description()
    }

    fn replacing_operation(&self, operation: Operation) -> Self {
        self.replacing_operation(operation)
    }
}

macro_rules! impl_control_flow_operand {
    ($operand:ty) => {
        impl $crate::low_ir::ControlFlowOperand for $operand {
            fn label(&self) -> Option<&$crate::low_ir::Label> {
                match self {
                    Self::Label(label) => Some(label),
                    _ => None,
                }
            }

            fn label_mut(&mut self) -> Option<&mut $crate::low_ir::Label> {
                match self {
                    Self::Label(label) => Some(label),
                    _ => None,
                }
            }

            fn from_label(label: $crate::low_ir::Label) -> Self {
                Self::Label(label)
            }
        }
    };
}

impl_control_flow_operand!(Operand);
pub(crate) use impl_control_flow_operand;

#[derive(Debug, Clone)]
pub(crate) struct Instruction<O = Operand, C = Operation> {
    pub(crate) opcode: C,
    pub(crate) operands: Vec<O>,
}

macro_rules! emit_instructions {
    (
        $output:expr;
        $(
            $opcode:expr => [$($operand:expr),* $(,)?];
        )*
    ) => {
        $(
            $output.push($crate::low_ir::Instruction {
                opcode: $opcode.into(),
                operands: vec![$($operand),*],
            });
        )*
    };
}

pub(crate) use emit_instructions;

/// Count appearances of virtual registers in the instruction operand stream.
pub(crate) fn count_virtual_register_uses<C>(
    instructions: &[Instruction<Operand, C>],
) -> HashMap<VirtualRegister, u32> {
    let mut counts = HashMap::new();
    for instruction in instructions {
        for operand in &instruction.operands {
            visit_virtual_registers(operand, &mut |register| {
                *counts.entry(register.clone()).or_insert(0) += 1;
            });
        }
    }
    counts
}

pub(crate) fn visit_virtual_registers(
    operand: &Operand,
    visit: &mut impl FnMut(&VirtualRegister),
) {
    match operand {
        Operand::VirtualRegister(register) => visit(register),
        Operand::Address(address) => {
            for register in std::iter::once(&address.base).chain(address.index.iter()) {
                if let AddressRegister::Virtual(register) = register {
                    visit(register);
                }
            }
        }
        _ => {}
    }
}

fn visit_virtual_registers_mut(
    operand: &mut Operand,
    visit: &mut impl FnMut(&mut VirtualRegister),
) {
    match operand {
        Operand::VirtualRegister(register) => visit(register),
        Operand::Address(address) => {
            for register in std::iter::once(&mut address.base)
                .chain(address.index.iter_mut())
            {
                if let AddressRegister::Virtual(register) = register {
                    visit(register);
                }
            }
        }
        _ => {}
    }
}

pub(crate) fn intern_virtual_registers(
    function: HandlerId,
    instructions: &mut [Instruction],
) {
    for instruction in instructions.iter_mut() {
        for operand in &mut instruction.operands {
            visit_virtual_registers_mut(operand, &mut |register| {
                register.id = None;
            });
        }
    }

    let mut registers = BTreeSet::new();
    for instruction in instructions.iter() {
        for operand in &instruction.operands {
            visit_virtual_registers(operand, &mut |register| {
                registers.insert(register.clone());
            });
        }
    }
    let ids = registers
        .into_iter()
        .enumerate()
        .map(|(index, register)| {
            let id = VirtualRegisterId {
                function,
                index: u32::try_from(index)
                    .expect("one handler cannot contain more than u32::MAX virtual registers"),
            };
            (register, id)
        })
        .collect::<HashMap<_, _>>();

    for instruction in instructions {
        for operand in &mut instruction.operands {
            visit_virtual_registers_mut(operand, &mut |register| {
                register.id = Some(ids[register]);
            });
        }
    }
}

#[derive(Debug, Clone)]
pub(crate) struct Handler {
    pub(crate) id: HandlerId,
    pub(crate) name: String,
    pub(crate) size: Option<u32>,
    pub(crate) is_cold: bool,
    pub(crate) instructions: Vec<Instruction>,
}

#[derive(Clone, Default)]
pub(crate) struct RuntimeConstants {
    values: [Option<i64>; KnownLayoutConstant::COUNT],
}

impl RuntimeConstants {
    pub(crate) fn from_layout(constants: &LayoutConstants) -> Self {
        Self {
            values: KnownLayoutConstant::ALL
                .map(|constant| constants.known(constant).map(|value| value.value())),
        }
    }

    pub(crate) fn get(&self, constant: KnownLayoutConstant) -> Option<i64> {
        self.values[constant as usize]
    }

    #[cfg(test)]
    pub(crate) fn from_values(
        values: impl IntoIterator<Item = (KnownLayoutConstant, i64)>,
    ) -> Self {
        let mut constants = Self::default();
        for (constant, value) in values {
            constants.values[constant as usize] = Some(value);
        }
        constants
    }
}

impl std::ops::Index<KnownLayoutConstant> for RuntimeConstants {
    type Output = i64;

    fn index(&self, constant: KnownLayoutConstant) -> &Self::Output {
        self.values[constant as usize]
            .as_ref()
            .expect("required runtime constants were verified before emission")
    }
}

#[derive(Clone)]
pub struct Program {
    pub(crate) runtime: RuntimeConstants,
    pub(crate) handlers: Vec<Handler>,
    pub(crate) dispatch_handlers: Vec<Option<HandlerId>>,
}

impl Program {
    pub(crate) fn new() -> Self {
        Self {
            runtime: RuntimeConstants::default(),
            handlers: Vec::new(),
            dispatch_handlers: Vec::new(),
        }
    }

}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::target::description::{
        IntegerWidth, Operation as MachineOperation,
    };

    #[test]
    fn interns_virtual_registers_per_handler() {
        let value = VirtualRegister::new("ssa_0");
        let other = VirtualRegister::new("ssa_1");
        let mut instructions = vec![Instruction {
            opcode: MachineOperation::Move(IntegerWidth::U64),
            operands: vec![
                Operand::VirtualRegister(value.clone()),
                Operand::Address(MemoryAddress {
                    base: AddressRegister::Virtual(value),
                    index: Some(AddressRegister::Virtual(other)),
                    scale: None,
                    displacement: None,
                }),
            ],
        }];

        intern_virtual_registers(HandlerId::new(3), &mut instructions);

        let Operand::VirtualRegister(value) = &instructions[0].operands[0]
        else {
            unreachable!()
        };
        let Operand::Address(address) = &instructions[0].operands[1] else {
            unreachable!()
        };
        let AddressRegister::Virtual(base) = &address.base else {
            unreachable!()
        };
        let Some(AddressRegister::Virtual(index)) = &address.index else {
            unreachable!()
        };
        assert_eq!(value.id(), base.id());
        assert_ne!(value.id(), index.id());
        assert!(value.id().is_some());

        let original = value.id();
        intern_virtual_registers(HandlerId::new(3), &mut instructions);
        let Operand::VirtualRegister(value) = &instructions[0].operands[0]
        else {
            unreachable!()
        };
        assert_eq!(value.id(), original);
    }
}
