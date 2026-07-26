/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Flap's typed static single-assignment intermediate representation.

pub(crate) mod analysis;
pub(crate) mod effects;
pub(crate) mod inline;
pub(crate) mod lowering;
mod memory_optimize;
pub(crate) mod optimize;
pub(crate) mod pass;
pub(crate) mod print;
pub(crate) mod report;
mod sccp;

use crate::hash::{HashMap, HashSet};
use crate::hir as typecheck;
use crate::identity::{ExternalSymbol, InlineFunctionId};
pub(crate) use crate::intrinsic::IntrinsicEffects as Effects;
pub(crate) use crate::intrinsic::ModRef as MemoryEffect;
pub(crate) use crate::intrinsic::{
    AggregateOperation, BinaryOperation, CheckedIntegerOperation, ComparisonDomain, ComparisonRelation, FieldAccess,
    IntegerBinaryOperation, IntegerComparisonOperation, Intrinsic, LowLevelOperation, OperandOperation, OperationValue,
    ShiftOperation, ValueOperation,
};
use crate::ssa as ir;
use crate::types::{BlockTemperature, InterpreterRegister, RegisterReference, Type};
use smallvec::SmallVec;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct BlockId(pub usize);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct InstructionId(pub usize);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct OperationParameterId(pub u32);

impl OperationParameterId {
    pub(crate) fn index(self) -> usize {
        self.0 as usize
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct ValueId(pub usize);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum BlockLayout {
    Hot,
    Preferred,
    Cold,
}

impl From<BlockTemperature> for BlockLayout {
    fn from(temperature: BlockTemperature) -> Self {
        match temperature {
            BlockTemperature::Default => Self::Hot,
            BlockTemperature::Hot => Self::Preferred,
            BlockTemperature::Cold => Self::Cold,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub(crate) enum Operation {
    Parameter(OperationParameterId),
    Intrinsic(Intrinsic),
    FieldAccess(FieldAccess),
    InlineCall(InlineFunctionId),
    MachineAssign {
        destination: InterpreterRegister,
        intrinsic: Intrinsic,
    },
    BlockReference(BlockId),
    Address,
    /// A conditional side exit: control leaves the block for `failure` when the
    /// condition is false, and falls through to the next instruction otherwise.
    ///
    /// A handler is mostly checks, and spelling each one as a branch terminator
    /// costs two blocks -- the one the check continues into and the one its
    /// join needs -- for code that is a single conditional jump. Keeping the
    /// check inside the block leaves the path that passes it straight-line,
    /// which is what every later phase is indexed by.
    Guard {
        failure: BlockId,
    },
}

impl From<Intrinsic> for Operation {
    fn from(intrinsic: Intrinsic) -> Self {
        Self::Intrinsic(intrinsic)
    }
}

impl Operation {
    fn effects(&self) -> Effects {
        match self {
            Self::Intrinsic(intrinsic) => intrinsic.effects(),
            Self::FieldAccess(field_access) => field_access.effects(),
            Self::Parameter(_) | Self::InlineCall(_) => Effects::UNKNOWN,
            Self::MachineAssign { intrinsic, .. } => {
                let mut effects = intrinsic.effects();
                effects.machine_state = if effects.machine_state.reads() {
                    MemoryEffect::ReadWrite
                } else {
                    MemoryEffect::Write
                };
                effects
            }
            Self::BlockReference(_) | Self::Address | Self::Guard { .. } => Effects::PURE,
        }
    }

    /// Where control goes when this operation decides not to continue.
    pub(crate) fn guard_failure(&self) -> Option<BlockId> {
        match self {
            Self::Guard { failure } => Some(*failure),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Instruction {
    pub(crate) operation: Operation,
    pub(crate) inputs: SmallVec<[ValueId; 2]>,
    pub(crate) results: SmallVec<[ValueId; 2]>,
    base_effects: Effects,
    pub(crate) effects: Effects,
}

impl Instruction {
    pub(crate) fn can_be_eliminated(&self) -> bool {
        self.effects.can_be_eliminated() && self.operation.guard_failure().is_none()
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub(crate) enum Constant {
    Integer(i64),
    KnownLayout(crate::frontend::layout::KnownLayoutConstant),
    LayoutValue(crate::frontend::layout::LayoutValue),
    Symbol(String),
    Layout(crate::frontend::layout::LayoutConstant),
    SlowPath(ExternalSymbol),
    FunctionSymbol(ExternalSymbol),
    Operation(OperationValue),
    MachineRegister(RegisterReference),
}

impl Constant {
    fn symbol(name: impl Into<String>) -> Self {
        let name = name.into();
        crate::frontend::layout::KnownLayoutConstant::from_name(&name).map_or(Self::Symbol(name), Self::KnownLayout)
    }

    fn layout_value(value: crate::frontend::layout::LayoutValue) -> Self {
        match value {
            crate::frontend::layout::LayoutValue::Constant(constant) => {
                constant.known().map_or(Self::LayoutValue(value), Self::KnownLayout)
            }
            crate::frontend::layout::LayoutValue::Immediate(_) => Self::LayoutValue(value),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum ValueDefinition {
    Dead,
    FunctionParameter(usize),
    BlockParameter { block: BlockId, index: usize },
    InstructionResult { instruction: InstructionId, index: usize },
    TerminatorResult { block: BlockId, index: usize },
    Constant(Constant),
}

struct UseAnalyses<'a> {
    cfg: &'a analysis::ControlFlowGraph,
    dominators: &'a analysis::DominatorTree,
    instruction_layout: &'a analysis::InstructionLayout,
    guards: &'a analysis::GuardExits,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Value {
    pub(crate) ty: Type,
    pub(crate) definition: ValueDefinition,
    depends_on_machine_state: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Edge {
    pub(crate) block: BlockId,
    pub(crate) arguments: Vec<ValueId>,
}

impl Edge {
    pub(crate) fn new(block: BlockId) -> Self {
        Self {
            block,
            arguments: Vec::new(),
        }
    }

    pub(crate) fn with_arguments(block: BlockId, arguments: Vec<ValueId>) -> Self {
        Self { block, arguments }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum Terminator {
    Jump(Edge),
    Branch {
        condition: ValueId,
        then_edge: Edge,
        else_edge: Edge,
    },
    Switch {
        value: ValueId,
        cases: Vec<(ValueId, Edge)>,
        default: Edge,
    },
    CheckedOperation {
        operation: Operation,
        inputs: Vec<ValueId>,
        results: Vec<ValueId>,
        effects: Effects,
        success: Edge,
        failure: Edge,
    },
    IndirectJump {
        target: ValueId,
    },
    Return(Vec<ValueId>),
    Unreachable,
}

impl Terminator {
    #[cfg(test)]
    pub(crate) fn jump(block: BlockId) -> Self {
        Self::Jump(Edge::new(block))
    }

    pub(crate) fn jump_with_arguments(block: BlockId, arguments: Vec<ValueId>) -> Self {
        Self::Jump(Edge::with_arguments(block, arguments))
    }

    #[cfg(test)]
    pub(crate) fn branch(condition: ValueId, then_block: BlockId, else_block: BlockId) -> Self {
        Self::branch_edges(condition, Edge::new(then_block), Edge::new(else_block))
    }

    pub(crate) fn branch_edges(condition: ValueId, then_edge: Edge, else_edge: Edge) -> Self {
        Self::Branch {
            condition,
            then_edge,
            else_edge,
        }
    }

    pub(crate) fn switch(value: ValueId, cases: Vec<(ValueId, Edge)>, default: Edge) -> Self {
        Self::Switch { value, cases, default }
    }

    pub(crate) fn successor_count(&self) -> usize {
        match self {
            Self::Jump(_) => 1,
            Self::Branch { .. } | Self::CheckedOperation { .. } => 2,
            Self::Switch { cases, .. } => cases.len() + 1,
            Self::IndirectJump { .. } | Self::Return(_) | Self::Unreachable => 0,
        }
    }

    pub(crate) fn successor(&self, index: usize) -> &Edge {
        match self {
            Self::Jump(edge) if index == 0 => edge,
            Self::Branch { then_edge, .. } if index == 0 => then_edge,
            Self::Branch { else_edge, .. } if index == 1 => else_edge,
            Self::CheckedOperation { success, .. } if index == 0 => success,
            Self::CheckedOperation { failure, .. } if index == 1 => failure,
            Self::Switch { cases, default, .. } => cases.get(index).map(|(_, edge)| edge).unwrap_or_else(|| {
                if index == cases.len() {
                    default
                } else {
                    panic!("no successor {index}")
                }
            }),
            _ => panic!("no successor {index}"),
        }
    }

    pub(crate) fn successors(&self) -> Successors<'_> {
        Successors {
            terminator: self,
            front: 0,
            back: self.successor_count(),
        }
    }

    pub(crate) fn inputs(&self) -> Vec<ValueId> {
        match self {
            Self::Jump(edge) => edge.arguments.clone(),
            Self::Branch {
                condition,
                then_edge,
                else_edge,
            } => std::iter::once(*condition)
                .chain(then_edge.arguments.iter().copied())
                .chain(else_edge.arguments.iter().copied())
                .collect(),
            Self::Switch { value, cases, default } => std::iter::once(*value)
                .chain(cases.iter().flat_map(|(_, edge)| edge.arguments.iter().copied()))
                .chain(default.arguments.iter().copied())
                .collect(),
            Self::CheckedOperation {
                inputs,
                results,
                success,
                failure,
                ..
            } => inputs
                .iter()
                .copied()
                .chain(
                    success
                        .arguments
                        .iter()
                        .copied()
                        .filter(|argument| !results.contains(argument)),
                )
                .chain(failure.arguments.iter().copied())
                .collect(),
            Self::IndirectJump { target } => vec![*target],
            Self::Return(values) => values.clone(),
            Self::Unreachable => Vec::new(),
        }
    }
}

#[derive(Clone)]
pub(crate) struct Successors<'a> {
    terminator: &'a Terminator,
    front: usize,
    back: usize,
}

impl<'a> Iterator for Successors<'a> {
    type Item = &'a Edge;

    fn next(&mut self) -> Option<Self::Item> {
        if self.front == self.back {
            return None;
        }
        let edge = self.terminator.successor(self.front);
        self.front += 1;
        Some(edge)
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let remaining = self.back - self.front;
        (remaining, Some(remaining))
    }
}

impl DoubleEndedIterator for Successors<'_> {
    fn next_back(&mut self) -> Option<Self::Item> {
        if self.front == self.back {
            return None;
        }
        self.back -= 1;
        Some(self.terminator.successor(self.back))
    }
}

impl ExactSizeIterator for Successors<'_> {}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Block {
    pub(crate) name: Option<String>,
    pub(crate) layout: BlockLayout,
    pub(crate) parameters: Vec<ValueId>,
    pub(crate) instructions: Vec<InstructionId>,
    pub(crate) terminator: Option<Terminator>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Function {
    pub(crate) name: String,
    pub(crate) parameter_names: Vec<String>,
    pub(crate) parameter_types: Vec<Type>,
    pub(crate) return_types: Vec<Type>,
    pub(crate) blocks: Vec<Block>,
    pub(crate) instructions: Vec<Instruction>,
    pub(crate) values: Vec<Value>,
    pub(crate) entry: BlockId,
    constant_values: HashMap<(Type, Constant), ValueId>,
}

impl Function {
    pub(crate) fn new(name: impl Into<String>, parameter_types: Vec<Type>, return_types: Vec<Type>) -> Self {
        let parameter_names = (0..parameter_types.len())
            .map(|index| format!("parameter_{index}"))
            .collect();
        let values = parameter_types
            .iter()
            .cloned()
            .enumerate()
            .map(|(index, ty)| Value {
                ty,
                definition: ValueDefinition::FunctionParameter(index),
                depends_on_machine_state: false,
            })
            .collect();
        Self {
            name: name.into(),
            parameter_names,
            parameter_types,
            return_types,
            blocks: vec![Block {
                name: Some("entry".to_string()),
                layout: BlockLayout::Hot,
                parameters: Vec::new(),
                instructions: Vec::new(),
                terminator: None,
            }],
            instructions: Vec::new(),
            values,
            entry: BlockId(0),
            constant_values: HashMap::default(),
        }
    }

    pub(crate) fn parameter(&self, index: usize) -> ValueId {
        ValueId(index)
    }

    pub(crate) fn set_parameter_name(&mut self, index: usize, name: impl Into<String>) {
        self.parameter_names[index] = name.into();
    }

    pub(crate) fn add_constant(&mut self, ty: Type, constant: Constant) -> ValueId {
        if let Some(value) = self.constant_values.get(&(ty.clone(), constant.clone())) {
            return *value;
        }
        let id = ValueId(self.values.len());
        self.values.push(Value {
            ty: ty.clone(),
            definition: ValueDefinition::Constant(constant.clone()),
            depends_on_machine_state: matches!(constant, Constant::MachineRegister(_)),
        });
        self.constant_values.insert((ty, constant), id);
        id
    }

    pub(crate) fn create_block(
        &mut self,
        name: Option<String>,
        layout: BlockLayout,
        parameter_types: Vec<Type>,
    ) -> BlockId {
        let block = BlockId(self.blocks.len());
        let parameters = parameter_types
            .into_iter()
            .enumerate()
            .map(|(index, ty)| {
                let value = ValueId(self.values.len());
                self.values.push(Value {
                    ty,
                    definition: ValueDefinition::BlockParameter { block, index },
                    depends_on_machine_state: false,
                });
                value
            })
            .collect();
        self.blocks.push(Block {
            name,
            layout,
            parameters,
            instructions: Vec::new(),
            terminator: None,
        });
        block
    }

    pub(crate) fn create_empty_block(&mut self, name: impl Into<String>, layout: BlockLayout) -> BlockId {
        self.create_block(Some(name.into()), layout, Vec::new())
    }

    pub(crate) fn create_named_block(
        &mut self,
        name: impl Into<String>,
        layout: BlockLayout,
        parameter_types: Vec<Type>,
    ) -> BlockId {
        self.create_block(Some(name.into()), layout, parameter_types)
    }

    pub(crate) fn append_instruction(
        &mut self,
        block: BlockId,
        operation: impl Into<Operation>,
        inputs: Vec<ValueId>,
        result_types: Vec<Type>,
    ) -> Vec<ValueId> {
        let operation = operation.into();
        let effects = operation.effects();
        self.append_instruction_with_effects(block, operation, inputs, result_types, effects)
    }

    pub(super) fn append_instruction_with_effects(
        &mut self,
        block: BlockId,
        operation: impl Into<Operation>,
        inputs: Vec<ValueId>,
        result_types: Vec<Type>,
        effects: Effects,
    ) -> Vec<ValueId> {
        let operation = operation.into();
        let base_effects = effects;
        let effects = self.effects_for_inputs(base_effects, &inputs);
        let depends_on_machine_state = effects.machine_state != MemoryEffect::None;
        let instruction = InstructionId(self.instructions.len());
        let results = result_types
            .into_iter()
            .enumerate()
            .map(|(index, ty)| {
                let value = ValueId(self.values.len());
                self.values.push(Value {
                    ty,
                    definition: ValueDefinition::InstructionResult { instruction, index },
                    depends_on_machine_state,
                });
                value
            })
            .collect::<Vec<_>>();
        self.instructions.push(Instruction {
            operation,
            inputs: inputs.into(),
            results: results.clone().into(),
            base_effects,
            effects,
        });
        self.blocks[block.0].instructions.push(instruction);
        results
    }

    fn effects_for_operation(&self, operation: &Operation, inputs: &[ValueId]) -> Effects {
        self.effects_for_inputs(operation.effects(), inputs)
    }

    fn effects_for_inputs(&self, mut effects: Effects, inputs: &[ValueId]) -> Effects {
        if inputs.iter().any(|input| self.value_depends_on_machine_state(*input)) {
            effects.machine_state = if effects.machine_state.writes() {
                MemoryEffect::ReadWrite
            } else {
                MemoryEffect::Read
            };
        }
        effects
    }

    fn value_depends_on_machine_state(&self, value: ValueId) -> bool {
        self.values[value.0].depends_on_machine_state
    }

    fn recompute_machine_state_dependencies(&mut self) {
        let mut dependent_values = vec![false; self.values.len()];
        let mut dependents = vec![Vec::new(); self.values.len()];
        let mut worklist = Vec::new();

        for (index, value) in self.values.iter().enumerate() {
            if matches!(
                value.definition,
                ValueDefinition::Constant(Constant::MachineRegister(_))
            ) {
                dependent_values[index] = true;
                worklist.push(ValueId(index));
            }
        }
        for instruction in &self.instructions {
            for input in &instruction.inputs {
                dependents[input.0].extend(instruction.results.iter().copied());
            }
            if instruction.base_effects.machine_state != MemoryEffect::None {
                for result in &instruction.results {
                    if !dependent_values[result.0] {
                        dependent_values[result.0] = true;
                        worklist.push(*result);
                    }
                }
            }
        }
        for block in &self.blocks {
            let terminator = block
                .terminator
                .as_ref()
                .expect("machine-state dependencies require complete SSA");
            if let Terminator::CheckedOperation {
                inputs,
                results,
                effects,
                ..
            } = terminator
            {
                for input in inputs {
                    dependents[input.0].extend(results.iter().copied());
                }
                if effects.machine_state != MemoryEffect::None {
                    for result in results {
                        if !dependent_values[result.0] {
                            dependent_values[result.0] = true;
                            worklist.push(*result);
                        }
                    }
                }
            }
            for edge in terminator.successors() {
                for (argument, parameter) in edge.arguments.iter().zip(&self.blocks[edge.block.0].parameters) {
                    dependents[argument.0].push(*parameter);
                }
            }
        }

        while let Some(value) = worklist.pop() {
            for dependent in &dependents[value.0] {
                if !dependent_values[dependent.0] {
                    dependent_values[dependent.0] = true;
                    worklist.push(*dependent);
                }
            }
        }

        for (value, dependent) in self.values.iter_mut().zip(&dependent_values) {
            value.depends_on_machine_state = *dependent;
        }
        let effects = self
            .instructions
            .iter()
            .map(|instruction| self.effects_for_inputs(instruction.base_effects, &instruction.inputs))
            .collect::<Vec<_>>();
        for (instruction, effects) in self.instructions.iter_mut().zip(effects) {
            instruction.effects = effects;
        }
    }

    /// The blocks a guard in `block` can exit to, with where the guard sits.
    ///
    /// A guard exit is an edge the terminator knows nothing about, so anything
    /// that walks control flow has to ask for these as well.
    pub(crate) fn guard_exits(&self, block: BlockId) -> impl Iterator<Item = (usize, BlockId)> + '_ {
        self.blocks[block.0]
            .instructions
            .iter()
            .enumerate()
            .filter_map(|(position, instruction)| {
                self.instructions[instruction.0]
                    .operation
                    .guard_failure()
                    .map(|failure| (position, failure))
            })
    }

    /// Where the first guard in `block` sits, if it has one.
    pub(crate) fn first_guard_position(&self, block: BlockId) -> Option<usize> {
        self.guard_exits(block).next().map(|(position, _)| position)
    }

    pub(crate) fn set_terminator(&mut self, block: BlockId, terminator: Terminator) {
        assert!(self.blocks[block.0].terminator.is_none());
        self.blocks[block.0].terminator = Some(terminator);
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn set_checked_operation(
        &mut self,
        block: BlockId,
        operation: impl Into<Operation>,
        inputs: Vec<ValueId>,
        result_types: Vec<Type>,
        effects: Effects,
        success_block: BlockId,
        success_arguments: Vec<ValueId>,
        failure: Edge,
    ) -> Vec<ValueId> {
        let operation = operation.into();
        let results = result_types
            .into_iter()
            .enumerate()
            .map(|(index, ty)| {
                let value = ValueId(self.values.len());
                self.values.push(Value {
                    ty,
                    definition: ValueDefinition::TerminatorResult { block, index },
                    depends_on_machine_state: false,
                });
                value
            })
            .collect::<Vec<_>>();
        self.set_terminator(
            block,
            Terminator::CheckedOperation {
                operation,
                inputs,
                results: results.clone(),
                effects,
                success: Edge::with_arguments(
                    success_block,
                    results.iter().copied().chain(success_arguments).collect(),
                ),
                failure,
            },
        );
        results
    }

    pub(crate) fn validate(&self) -> Result<(), String> {
        if self.entry.0 >= self.blocks.len() {
            return Err(format!("function '{}' has an invalid entry block", self.name));
        }
        for (index, parameter_type) in self.parameter_types.iter().enumerate() {
            let value = self
                .values
                .get(index)
                .ok_or_else(|| format!("function '{}' is missing parameter {index}", self.name))?;
            if value.definition != ValueDefinition::FunctionParameter(index) || &value.ty != parameter_type {
                return Err(format!("function '{}' has an invalid parameter {index}", self.name));
            }
        }
        for (index, block) in self.blocks.iter().enumerate() {
            let block_id = BlockId(index);
            for (parameter_index, parameter) in block.parameters.iter().enumerate() {
                let value = self
                    .values
                    .get(parameter.0)
                    .ok_or_else(|| format!("block {block_id:?} has invalid parameter {parameter:?}"))?;
                if value.definition
                    != (ValueDefinition::BlockParameter {
                        block: block_id,
                        index: parameter_index,
                    })
                {
                    return Err(format!("block {block_id:?} does not own parameter {parameter:?}"));
                }
            }
            let terminator = block
                .terminator
                .as_ref()
                .ok_or_else(|| format!("block {block_id:?} has no terminator"))?;
            self.validate_terminator(block_id, terminator)?;
            for edge in terminator.successors() {
                self.validate_edge(block_id, edge)?;
            }
        }
        for (index, instruction) in self.instructions.iter().enumerate() {
            let instruction_id = InstructionId(index);
            self.validate_instruction(instruction_id, instruction)?;
            for (result_index, result) in instruction.results.iter().enumerate() {
                let Some(value) = self.values.get(result.0) else {
                    return Err(format!("instruction {instruction_id:?} has an invalid result"));
                };
                if value.definition
                    != (ValueDefinition::InstructionResult {
                        instruction: instruction_id,
                        index: result_index,
                    })
                {
                    return Err(format!("instruction {instruction_id:?} does not own result {result:?}"));
                }
            }
        }
        self.validate_uses()
    }

    fn validate_instruction(&self, instruction_id: InstructionId, instruction: &Instruction) -> Result<(), String> {
        self.validate_operation(&instruction.operation)?;
        if !matches!(instruction.operation, Operation::Guard { .. }) {
            return Ok(());
        }
        let [condition] = instruction.inputs.as_slice() else {
            return Err(format!(
                "guard instruction {instruction_id:?} in '{}' must have exactly one input",
                self.name
            ));
        };
        let condition_type = &self
            .values
            .get(condition.0)
            .ok_or_else(|| format!("guard instruction {instruction_id:?} uses invalid value {condition:?}"))?
            .ty;
        if condition_type != &Type::Bool {
            return Err(format!(
                "guard instruction {instruction_id:?} in '{}' uses {condition_type:?}, not Bool",
                self.name
            ));
        }
        Ok(())
    }

    fn validate_operation(&self, operation: &Operation) -> Result<(), String> {
        if let Operation::Guard { failure } = operation {
            let Some(target) = self.blocks.get(failure.0) else {
                return Err(format!("a guard in '{}' exits to invalid block {failure:?}", self.name));
            };
            // A guard exit carries no arguments, so nothing can supply the
            // parameters of the block it leaves for.
            if !target.parameters.is_empty() {
                return Err(format!(
                    "a guard in '{}' exits to {failure:?}, which takes parameters",
                    self.name
                ));
            }
            return Ok(());
        }
        let Operation::Parameter(parameter) = operation else {
            return Ok(());
        };
        let Some(parameter_type) = self.parameter_types.get(parameter.index()) else {
            return Err(format!(
                "function '{}' references invalid operation parameter {}",
                self.name, parameter.0
            ));
        };
        if !parameter_type.is_operation() {
            return Err(format!(
                "function '{}' references non-operation parameter {} as an operation",
                self.name, parameter.0
            ));
        }
        Ok(())
    }

    fn validate_terminator(&self, block: BlockId, terminator: &Terminator) -> Result<(), String> {
        match terminator {
            Terminator::Branch { condition, .. } => {
                let condition_type = self
                    .values
                    .get(condition.0)
                    .ok_or_else(|| format!("block {block:?} branches on invalid value {condition:?}"))?
                    .ty
                    .clone();
                if condition_type != Type::Bool {
                    return Err(format!("block {block:?} branches on {condition_type:?}, not Bool"));
                }
            }
            Terminator::Switch { value, cases, .. } => {
                let switch_type = self
                    .values
                    .get(value.0)
                    .ok_or_else(|| format!("block {block:?} switches on invalid value {value:?}"))?
                    .ty
                    .clone();
                let mut seen = HashSet::default();
                for (pattern, _) in cases {
                    let pattern_type = self
                        .values
                        .get(pattern.0)
                        .ok_or_else(|| format!("block {block:?} has an invalid switch pattern"))?;
                    if !matches!(pattern_type.definition, ValueDefinition::Constant(_)) {
                        return Err(format!("block {block:?} has a non-constant switch pattern"));
                    }
                    if pattern_type.ty != switch_type {
                        return Err(format!("block {block:?} has a mistyped switch pattern"));
                    }
                    if !seen.insert(*pattern) {
                        return Err(format!("block {block:?} has duplicate switch case {pattern:?}"));
                    }
                }
            }
            Terminator::CheckedOperation {
                operation,
                results,
                success,
                failure,
                ..
            } => {
                self.validate_operation(operation)?;
                for (index, result) in results.iter().enumerate() {
                    let Some(value) = self.values.get(result.0) else {
                        return Err(format!("checked operation in {block:?} has an invalid result"));
                    };
                    if value.definition != (ValueDefinition::TerminatorResult { block, index }) {
                        return Err(format!("checked operation in {block:?} does not own result {result:?}"));
                    }
                    if success.arguments.get(index) != Some(result) {
                        return Err(format!(
                            "checked operation result {result:?} is not passed to its success block"
                        ));
                    }
                    if success
                        .arguments
                        .get(results.len()..)
                        .is_some_and(|arguments| arguments.contains(result))
                    {
                        return Err(format!("checked operation result {result:?} is passed more than once"));
                    }
                    if failure.arguments.contains(result) {
                        return Err(format!(
                            "checked operation result {result:?} is available on its failure edge"
                        ));
                    }
                }
            }
            Terminator::IndirectJump { target } => {
                let target_type = self
                    .values
                    .get(target.0)
                    .ok_or_else(|| format!("block {block:?} jumps through invalid value {target:?}"))?
                    .ty
                    .clone();
                if target_type != Type::label() {
                    return Err(format!("block {block:?} jumps through {target_type:?}, not Label"));
                }
            }
            Terminator::Return(values) => {
                if values.len() != self.return_types.len() {
                    return Err(format!(
                        "block {block:?} returns {} values from a function with {} results",
                        values.len(),
                        self.return_types.len()
                    ));
                }
                for (value, return_type) in values.iter().zip(&self.return_types) {
                    let value_type = self
                        .values
                        .get(value.0)
                        .ok_or_else(|| format!("block {block:?} returns invalid value {value:?}"))?
                        .ty
                        .clone();
                    if &value_type != return_type {
                        return Err(format!(
                            "block {block:?} returns {value_type:?} from a {return_type:?} result"
                        ));
                    }
                }
            }
            Terminator::Jump(_) | Terminator::Unreachable => {}
        }
        Ok(())
    }

    fn validate_edge(&self, source: BlockId, edge: &Edge) -> Result<(), String> {
        let Some(target) = self.blocks.get(edge.block.0) else {
            return Err(format!("block {source:?} targets invalid block {:?}", edge.block));
        };
        if edge.arguments.len() != target.parameters.len() {
            return Err(format!(
                "edge {source:?} -> {:?} passes {} arguments to {} parameters",
                edge.block,
                edge.arguments.len(),
                target.parameters.len()
            ));
        }
        for (argument, parameter) in edge.arguments.iter().zip(&target.parameters) {
            let argument_type = self
                .values
                .get(argument.0)
                .ok_or_else(|| format!("edge {source:?} uses invalid value {argument:?}"))?
                .ty
                .clone();
            let parameter_type = &self
                .values
                .get(parameter.0)
                .ok_or_else(|| format!("block {:?} has invalid parameter {parameter:?}", edge.block))?
                .ty;
            if &argument_type != parameter_type {
                return Err(format!(
                    "edge {source:?} -> {:?} passes {argument_type:?} to {parameter_type:?}",
                    edge.block
                ));
            }
        }
        Ok(())
    }

    fn validate_uses(&self) -> Result<(), String> {
        let cfg = analysis::ControlFlowGraph::compute(self);
        let dominators = analysis::DominatorTree::compute(self, &cfg);
        let instruction_layout = analysis::InstructionLayout::compute(self)?;
        let guards = analysis::GuardExits::compute(self, &cfg);
        let analyses = UseAnalyses {
            cfg: &cfg,
            dominators: &dominators,
            instruction_layout: &instruction_layout,
            guards: &guards,
        };

        for (instruction_index, instruction) in self.instructions.iter().enumerate() {
            let instruction_id = InstructionId(instruction_index);
            let block = instruction_layout.block(instruction_id);
            for input in &instruction.inputs {
                self.validate_use(
                    *input,
                    block,
                    Some(instruction_layout.position(instruction_id)),
                    &analyses,
                )?;
            }
        }
        for (block_index, block) in self.blocks.iter().enumerate() {
            for input in block.terminator.as_ref().unwrap().inputs() {
                self.validate_use(input, BlockId(block_index), None, &analyses)?;
            }
        }
        Ok(())
    }

    fn validate_use(
        &self,
        value: ValueId,
        use_block: BlockId,
        use_position: Option<usize>,
        analyses: &UseAnalyses<'_>,
    ) -> Result<(), String> {
        let UseAnalyses {
            cfg,
            dominators,
            instruction_layout,
            guards,
        } = analyses;
        let definition = self
            .values
            .get(value.0)
            .ok_or_else(|| format!("block {use_block:?} uses invalid value {value:?}"))?;
        if definition.definition == ValueDefinition::Dead {
            return Err(format!("block {use_block:?} uses dead value {value:?}"));
        }
        if matches!(definition.definition, ValueDefinition::TerminatorResult { .. }) {
            return Err(format!(
                "terminator result {value:?} must enter its success block through a block parameter"
            ));
        }
        let (definition_block, definition_position) = match definition.definition {
            ValueDefinition::Dead => unreachable!(),
            ValueDefinition::FunctionParameter(_) | ValueDefinition::Constant(_) => return Ok(()),
            ValueDefinition::BlockParameter { block, .. } => (block, None),
            ValueDefinition::InstructionResult { instruction, .. } => (
                instruction_layout.block(instruction),
                Some(instruction_layout.position(instruction)),
            ),
            ValueDefinition::TerminatorResult { .. } => unreachable!(),
        };
        if definition_block == use_block {
            if let (Some(definition_position), Some(use_position)) = (definition_position, use_position)
                && definition_position >= use_position
            {
                return Err(format!("value {value:?} is used before its definition"));
            }
            return Ok(());
        }
        if !dominators.dominates(definition_block, use_block) {
            return Err(format!("value {value:?} does not dominate its use in {use_block:?}"));
        }
        let position = definition_position.unwrap_or(0);
        if !guards.definition_reaches(definition_block, position, use_block)
            && guards.definition_escapes(self, cfg, definition_block, position, use_block)
        {
            return Err(format!(
                "value {value:?} is defined past a guard in {definition_block:?} and cannot reach {use_block:?}"
            ));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::intrinsic::{CallOperation, ControlOperation, MemoryOperation, MemoryWidth, PairWidth};

    #[test]
    fn exposes_intrinsic_properties_to_ssa() {
        assert_eq!(
            MemoryOperation::Load {
                width: MemoryWidth::HalfWord,
                signed: true
            }
            .access_width(),
            2
        );
        assert_eq!(MemoryOperation::StorePair(PairWidth::DoubleWord).address_count(), 2);
        assert!(MemoryOperation::StorePair(PairWidth::DoubleWord).writes());
        assert!(!MemoryOperation::LoadPair(PairWidth::DoubleWord).writes());
        assert!(IntegerBinaryOperation::Binary(BinaryOperation::And).supports_narrow_result());
        assert!(!IntegerBinaryOperation::Binary(BinaryOperation::Add).supports_narrow_result());
        assert_eq!(
            CheckedIntegerOperation::Multiply.unchecked(),
            Some(IntegerBinaryOperation::Binary(BinaryOperation::Multiply))
        );
        assert_eq!(CheckedIntegerOperation::Negate.unchecked(), None);
        assert!(ControlOperation::DispatchNext.is_terminal());
        assert!(ControlOperation::JumpBytecode.writes_machine_state());
        assert_eq!(CallOperation::SlowPath.result_count(), 0);
        assert_eq!(CallOperation::RawNative.result_count(), 2);
    }

    #[test]
    fn validates_typed_block_arguments_at_a_control_flow_join() {
        let mut function = Function::new("choose", vec![Type::Bool, Type::I32, Type::I32], vec![Type::I32]);
        let then_block = function.create_empty_block("then", BlockLayout::Hot);
        let else_block = function.create_empty_block("else", BlockLayout::Cold);
        let join = function.create_named_block("join", BlockLayout::Hot, vec![Type::I32]);
        function.set_terminator(
            function.entry,
            Terminator::branch(function.parameter(0), then_block, else_block),
        );
        function.set_terminator(
            then_block,
            Terminator::jump_with_arguments(join, vec![function.parameter(1)]),
        );
        function.set_terminator(
            else_block,
            Terminator::jump_with_arguments(join, vec![function.parameter(2)]),
        );
        function.set_terminator(join, Terminator::Return(vec![function.blocks[join.0].parameters[0]]));

        function.validate().unwrap();
    }

    #[test]
    fn rejects_values_that_do_not_dominate_their_use() {
        let mut function = Function::new("bad", vec![Type::Bool, Type::I32], vec![Type::I32]);
        let then_block = function.create_block(None, BlockLayout::Hot, Vec::new());
        let else_block = function.create_block(None, BlockLayout::Hot, Vec::new());
        let join = function.create_block(None, BlockLayout::Hot, Vec::new());
        function.set_terminator(
            function.entry,
            Terminator::branch(function.parameter(0), then_block, else_block),
        );
        let value = function.append_instruction(
            then_block,
            Intrinsic::Value(crate::intrinsic::ValueOperation::Reuse),
            vec![function.parameter(1)],
            vec![Type::I32],
        )[0];
        function.set_terminator(then_block, Terminator::jump(join));
        function.set_terminator(else_block, Terminator::jump(join));
        function.set_terminator(join, Terminator::Return(vec![value]));

        assert!(function.validate().unwrap_err().contains("does not dominate"));
    }

    #[test]
    fn exposes_checked_results_only_through_the_success_block() {
        let mut function = Function::new("checked_add", vec![Type::I32, Type::I32], vec![Type::I32]);
        let success = function.create_named_block("success", BlockLayout::Hot, vec![Type::I32]);
        let failure = function.create_empty_block("overflow", BlockLayout::Cold);
        function.set_checked_operation(
            function.entry,
            Intrinsic::CheckedInteger(CheckedIntegerOperation::Add),
            vec![function.parameter(0), function.parameter(1)],
            vec![Type::I32],
            Effects::PURE,
            success,
            Vec::new(),
            Edge::new(failure),
        );
        function.set_terminator(
            success,
            Terminator::Return(vec![function.blocks[success.0].parameters[0]]),
        );
        function.set_terminator(failure, Terminator::Unreachable);

        function.validate().unwrap();
    }

    #[test]
    fn validates_effectful_operations_and_constants() {
        let mut function = Function::new("load", vec![Type::U64], vec![Type::U32]);
        let offset = function.add_constant(Type::U64, Constant::Integer(8));
        let symbol = function.add_constant(Type::U64, Constant::Symbol("CAGE_MASK".to_string()));
        let value = function.append_instruction_with_effects(
            function.entry,
            Intrinsic::Memory(MemoryOperation::Load {
                width: MemoryWidth::Word,
                signed: false,
            }),
            vec![function.parameter(0), offset, symbol],
            vec![Type::U32],
            Effects {
                memory: MemoryEffect::Read,
                machine_state: MemoryEffect::None,
                may_call: false,
                may_trap: false,
            },
        )[0];
        function.set_terminator(function.entry, Terminator::Return(vec![value]));

        function.validate().unwrap();
    }

    #[test]
    fn tracks_machine_state_dependencies_in_linear_time() {
        let mut function = Function::new("machine-state-dag", Vec::new(), Vec::new());
        let machine_state = function.add_constant(
            Type::U64,
            Constant::MachineRegister(RegisterReference::Interpreter(InterpreterRegister::ProgramBase)),
        );
        let one = function.add_constant(Type::U64, Constant::Integer(1));
        let mut previous = machine_state;
        let mut current = one;
        for _ in 0..100 {
            let next = function.append_instruction(
                function.entry,
                Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(BinaryOperation::Add)),
                vec![previous, current],
                vec![Type::U64],
            )[0];
            previous = current;
            current = next;
        }
        function.set_terminator(function.entry, Terminator::Return(vec![current]));

        assert_eq!(
            function.instructions.last().unwrap().effects.machine_state,
            MemoryEffect::Read
        );

        function.instructions[0].inputs[0] = one;
        function.recompute_machine_state_dependencies();
        assert_eq!(
            function.instructions.last().unwrap().effects.machine_state,
            MemoryEffect::None
        );
    }

    #[test]
    fn tracks_machine_state_through_checked_results_and_block_parameters() {
        let mut function = Function::new("checked-machine-state", Vec::new(), Vec::new());
        let machine_state = function.add_constant(
            Type::U64,
            Constant::MachineRegister(RegisterReference::Interpreter(InterpreterRegister::ProgramBase)),
        );
        let one = function.add_constant(Type::U64, Constant::Integer(1));
        let success = function.create_named_block("success", BlockLayout::Hot, vec![Type::U64]);
        let failure = function.create_empty_block("failure", BlockLayout::Cold);
        let results = function.set_checked_operation(
            function.entry,
            Intrinsic::CheckedInteger(CheckedIntegerOperation::Add),
            vec![machine_state, one],
            vec![Type::U64],
            Effects::PURE,
            success,
            Vec::new(),
            Edge::new(failure),
        );
        let parameter = function.blocks[success.0].parameters[0];
        function.append_instruction(
            success,
            Intrinsic::LowLevel(LowLevelOperation::Negate),
            vec![parameter],
            vec![Type::U64],
        );
        function.set_terminator(success, Terminator::Return(Vec::new()));
        function.set_terminator(failure, Terminator::Unreachable);

        function.recompute_machine_state_dependencies();
        assert!(function.values[results[0].0].depends_on_machine_state);
        assert!(function.values[parameter.0].depends_on_machine_state);
        assert_eq!(function.instructions[0].effects.machine_state, MemoryEffect::Read);

        let Terminator::CheckedOperation { inputs, .. } =
            function.blocks[function.entry.0].terminator.as_mut().unwrap()
        else {
            unreachable!()
        };
        inputs[0] = one;
        function.recompute_machine_state_dependencies();
        assert!(!function.values[results[0].0].depends_on_machine_state);
        assert!(!function.values[parameter.0].depends_on_machine_state);
        assert_eq!(function.instructions[0].effects.machine_state, MemoryEffect::None);
    }

    #[test]
    fn validates_typed_operation_parameters() {
        let mut function = Function::new(
            "apply",
            vec![Type::CheckedBinaryOperation(Box::new(Type::I32))],
            Vec::new(),
        );
        function.append_instruction(
            function.entry,
            Operation::Parameter(OperationParameterId(0)),
            Vec::new(),
            Vec::new(),
        );
        function.set_terminator(function.entry, Terminator::Return(Vec::new()));
        function.validate().unwrap();

        let mut scalar_function = Function::new("apply", vec![Type::I32], Vec::new());
        scalar_function.append_instruction(
            scalar_function.entry,
            Operation::Parameter(OperationParameterId(0)),
            Vec::new(),
            Vec::new(),
        );
        scalar_function.set_terminator(scalar_function.entry, Terminator::Return(Vec::new()));
        assert!(
            scalar_function
                .validate()
                .unwrap_err()
                .contains("references non-operation parameter 0")
        );

        function.instructions[0].operation = Operation::Parameter(OperationParameterId(1));
        assert!(
            function
                .validate()
                .unwrap_err()
                .contains("references invalid operation parameter 1")
        );
    }

    #[test]
    fn interns_typed_constants() {
        let mut function = Function::new("constants", Vec::new(), Vec::new());
        let first = function.add_constant(Type::I32, Constant::Integer(42));
        let repeated = function.add_constant(Type::I32, Constant::Integer(42));
        let differently_typed = function.add_constant(Type::U32, Constant::Integer(42));

        assert_eq!(first, repeated);
        assert_ne!(first, differently_typed);
        assert_eq!(function.values.len(), 2);
    }

    fn guarded_function() -> (Function, BlockId, ValueId) {
        let mut function = Function::new("guarded", vec![Type::Bool, Type::I32], vec![Type::I32]);
        let failure = function.create_empty_block("failure", BlockLayout::Cold);
        function.append_instruction(
            function.entry,
            Operation::Guard { failure },
            vec![function.parameter(0)],
            Vec::new(),
        );
        let value = function.append_instruction(
            function.entry,
            Intrinsic::Value(ValueOperation::Reuse),
            vec![function.parameter(1)],
            vec![Type::I32],
        )[0];
        function.set_terminator(function.entry, Terminator::Return(vec![value]));
        (function, failure, value)
    }

    #[test]
    fn a_guard_exit_is_an_edge_out_of_the_middle_of_its_block() {
        let (mut function, failure, _) = guarded_function();
        function.set_terminator(failure, Terminator::Return(vec![function.parameter(1)]));

        let cfg = analysis::ControlFlowGraph::compute(&function);

        assert_eq!(cfg.successors(function.entry), [failure]);
        assert_eq!(cfg.predecessors(failure), [function.entry]);
        assert!(cfg.is_reachable(failure));
        function.validate().unwrap();
    }

    #[test]
    fn rejects_guards_without_one_boolean_condition() {
        let (mut function, failure, _) = guarded_function();
        function.set_terminator(failure, Terminator::Unreachable);
        let guard = function.blocks[function.entry.0].instructions[0];
        function.instructions[guard.0].inputs.clear();
        assert!(function.validate().unwrap_err().contains("must have exactly one input"));

        let non_boolean = function.parameter(1);
        function.instructions[guard.0].inputs.push(non_boolean);
        assert!(function.validate().unwrap_err().contains("uses I32, not Bool"));
    }

    #[test]
    fn rejects_a_value_defined_past_a_guard_where_the_guard_leaves_for() {
        let (mut function, failure, value) = guarded_function();
        function.set_terminator(failure, Terminator::Return(vec![value]));

        assert!(
            function
                .validate()
                .unwrap_err()
                .contains("is defined past a guard in BlockId(0)")
        );
    }

    #[test]
    fn accepts_a_value_defined_past_a_guard_no_exit_of_its_own_reaches() {
        let (mut function, failure, value) = guarded_function();
        let reader = function.create_empty_block("reader", BlockLayout::Hot);
        let elsewhere = function.create_empty_block("elsewhere", BlockLayout::Hot);
        function.blocks[function.entry.0].terminator = Some(Terminator::jump(elsewhere));
        function.append_instruction(
            elsewhere,
            Operation::Guard { failure: reader },
            vec![function.parameter(0)],
            Vec::new(),
        );
        function.set_terminator(elsewhere, Terminator::jump(reader));
        function.set_terminator(reader, Terminator::Return(vec![value]));
        function.set_terminator(failure, Terminator::Unreachable);

        function.validate().unwrap();
    }

    #[test]
    fn unifies_known_source_and_layout_constants() {
        let constants =
            crate::frontend::layout::LayoutConstants::from_values([("EMPTY_VALUE".to_string(), 0x7ffb_0000_0000_0000)]);

        assert_eq!(
            Constant::symbol("EMPTY_VALUE"),
            Constant::layout_value(crate::frontend::layout::LayoutValue::Constant(
                constants.get("EMPTY_VALUE").unwrap(),
            ))
        );
    }
}
