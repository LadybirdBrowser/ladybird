/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Explicit dependencies between effectful SSA operations.

use super::analysis::{ControlFlowGraph, DominatorTree, InstructionLayout};
use super::{
    AggregateOperation, BlockId, Effects, Function, InstructionId, Intrinsic, MemoryEffect, OperandOperation,
    Operation, Terminator, ValueDefinition, ValueId,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum EffectDomain {
    Memory,
    MachineState,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(crate) enum EffectDefinition {
    Entry,
    Phi(BlockId),
    Instruction(InstructionId),
    Terminator(BlockId),
}

/// A set of effect definitions.
///
/// These are built and copied once per effectful instruction per domain, and
/// the analysis is recomputed after every pass that changes anything, so the
/// set is held as a sorted vector: copying one is a single allocation rather
/// than a tree walk, and nearly all of them hold a single definition.
#[derive(Debug, Clone, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(crate) struct EffectSet(Vec<EffectDefinition>);

impl EffectSet {
    pub(crate) fn new() -> Self {
        Self(Vec::new())
    }

    pub(crate) fn from_one(definition: EffectDefinition) -> Self {
        Self(vec![definition])
    }

    /// Take a set from definitions already sorted and free of duplicates.
    pub(crate) fn from_sorted(definitions: Vec<EffectDefinition>) -> Self {
        debug_assert!(definitions.windows(2).all(|pair| pair[0] < pair[1]));
        Self(definitions)
    }

    pub(crate) fn as_slice(&self) -> &[EffectDefinition] {
        &self.0
    }

    pub(crate) fn len(&self) -> usize {
        self.0.len()
    }

    pub(crate) fn iter(&self) -> impl Iterator<Item = &EffectDefinition> {
        self.0.iter()
    }
}

impl FromIterator<EffectDefinition> for EffectSet {
    fn from_iter<I: IntoIterator<Item = EffectDefinition>>(definitions: I) -> Self {
        let mut set = Self(definitions.into_iter().collect());
        // A block almost always inherits a single definition, and sorting is
        // the most expensive thing this pass does per block.
        if set.0.len() > 1 {
            set.0.sort_unstable();
            set.0.dedup();
        }
        set
    }
}

impl<const N: usize> From<[EffectDefinition; N]> for EffectSet {
    fn from(definitions: [EffectDefinition; N]) -> Self {
        Self::from_iter(definitions)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct EffectAccess {
    pub(crate) effect: MemoryEffect,
    pub(crate) dependencies: EffectSet,
    pub(crate) definition: Option<EffectDefinition>,
}

#[derive(Debug)]
pub(crate) struct EffectDependencies {
    memory: DomainDependencies,
    machine_state: DomainDependencies,
}

impl EffectDependencies {
    pub(crate) fn compute(function: &Function, cfg: &ControlFlowGraph) -> Self {
        Self {
            memory: DomainDependencies::compute(function, cfg, EffectDomain::Memory),
            machine_state: DomainDependencies::compute(function, cfg, EffectDomain::MachineState),
        }
    }

    pub(crate) fn domain(&self, domain: EffectDomain) -> &DomainDependencies {
        match domain {
            EffectDomain::Memory => &self.memory,
            EffectDomain::MachineState => &self.machine_state,
        }
    }

    pub(crate) fn memory_is_stable_after_instruction(
        &self,
        dominators: &DominatorTree,
        layout: &InstructionLayout,
        instruction: InstructionId,
        target: BlockId,
    ) -> bool {
        let source_block = layout.block(instruction);
        if source_block == target || !dominators.dominates(source_block, target) {
            return false;
        }
        self.memory.instruction_access(instruction).is_some_and(|access| {
            access.effect == MemoryEffect::Read && access.dependencies == *self.memory.block_input(target)
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub(crate) enum MemoryLocationIdentity {
    Address(Vec<ValueId>),
    IndexedOperand(ValueId),
    BytecodeField(ValueId),
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub(crate) struct MemoryLocation {
    pub(crate) identity: MemoryLocationIdentity,
    pub(crate) bytes: u8,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum AliasResult {
    Must,
    May,
    No,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub(crate) struct MemoryAccessLocations {
    pub(crate) reads: Vec<MemoryLocation>,
    pub(crate) writes: Vec<MemoryLocation>,
    pub(crate) unknown_read: bool,
    pub(crate) unknown_write: bool,
}

pub(crate) fn instruction_memory_locations(function: &Function, instruction: InstructionId) -> MemoryAccessLocations {
    let instruction = &function.instructions[instruction.0];
    operation_memory_locations(
        function,
        &instruction.operation,
        &instruction.inputs,
        instruction.effects.memory,
    )
}

fn operation_memory_locations(
    function: &Function,
    operation: &Operation,
    inputs: &[ValueId],
    effect: MemoryEffect,
) -> MemoryAccessLocations {
    if effect == MemoryEffect::None {
        return MemoryAccessLocations::default();
    }
    let mut access = MemoryAccessLocations::default();
    let mut add_location = |input: usize, bytes: u8, write: bool| {
        let Some(value) = inputs.get(input) else {
            return false;
        };
        let Some(location) = address_location(function, *value, bytes) else {
            return false;
        };
        if write {
            access.writes.push(location);
        } else {
            access.reads.push(location);
        }
        true
    };

    if let Operation::Intrinsic(Intrinsic::Memory(operation)) = operation {
        for input in 0..operation.address_count() {
            if !add_location(input, operation.access_width(), operation.writes()) {
                if operation.writes() {
                    access.unknown_write = true;
                } else {
                    access.unknown_read = true;
                }
            }
        }
        return access;
    }

    if let Operation::FieldAccess(field_access) = operation {
        let bytes = field_access.kind.width().bytes();
        if !add_location(0, bytes, field_access.kind.writes()) {
            return unknown_access(effect);
        }
        return access;
    }

    if let Operation::Intrinsic(Intrinsic::Operand(operation)) = operation {
        match operation {
            OperandOperation::Load(_) => {
                let [input] = inputs else {
                    return unknown_access(effect);
                };
                access.reads.push(MemoryLocation {
                    identity: MemoryLocationIdentity::IndexedOperand(*input),
                    bytes: 8,
                });
            }
            OperandOperation::LoadPair => {
                if inputs.len() != 2 {
                    return unknown_access(effect);
                }
                for input in inputs {
                    access.reads.push(MemoryLocation {
                        identity: MemoryLocationIdentity::IndexedOperand(*input),
                        bytes: 8,
                    });
                }
            }
            OperandOperation::Store(_) => {
                let [index, _] = inputs else {
                    return unknown_access(effect);
                };
                access.writes.push(MemoryLocation {
                    identity: MemoryLocationIdentity::IndexedOperand(*index),
                    bytes: 8,
                });
            }
            OperandOperation::Copy => {
                let [destination, source] = inputs else {
                    return unknown_access(effect);
                };
                access.writes.push(MemoryLocation {
                    identity: MemoryLocationIdentity::IndexedOperand(*destination),
                    bytes: 8,
                });
                access.reads.push(MemoryLocation {
                    identity: MemoryLocationIdentity::IndexedOperand(*source),
                    bytes: 8,
                });
            }
            OperandOperation::Decode => return unknown_access(effect),
        }
        return access;
    }

    if *operation == Operation::Intrinsic(Intrinsic::LowLevel(super::LowLevelOperation::Increment32Memory)) {
        if let Some(value) = inputs.first()
            && let Some(location) = address_location(function, *value, 4)
        {
            access.reads.push(location.clone());
            access.writes.push(location);
        } else {
            access.unknown_read = true;
            access.unknown_write = true;
        }
        return access;
    }

    if let Operation::Intrinsic(Intrinsic::Bytecode(operation)) = operation {
        let [field] = inputs else {
            return unknown_access(effect);
        };
        access.reads.push(MemoryLocation {
            identity: MemoryLocationIdentity::BytecodeField(*field),
            bytes: operation.access_width(),
        });
        return access;
    }

    if let Operation::Intrinsic(Intrinsic::Aggregate(operation)) = operation {
        match operation {
            AggregateOperation::Copy16 => {
                if !add_location(0, 16, true) {
                    access.unknown_write = true;
                }
                if !add_location(1, 16, false) {
                    access.unknown_read = true;
                }
            }
            AggregateOperation::Zero16 => {
                if !add_location(0, 16, true) {
                    access.unknown_write = true;
                }
            }
        }
        return access;
    }

    unknown_access(effect)
}

fn unknown_access(effect: MemoryEffect) -> MemoryAccessLocations {
    MemoryAccessLocations {
        unknown_read: effect.reads(),
        unknown_write: effect.writes(),
        ..Default::default()
    }
}

fn address_location(function: &Function, value: ValueId, bytes: u8) -> Option<MemoryLocation> {
    let ValueDefinition::InstructionResult { instruction, .. } = function.values[value.0].definition else {
        return None;
    };
    let instruction = &function.instructions[instruction.0];
    if instruction.operation != Operation::Address {
        return None;
    }
    Some(MemoryLocation {
        identity: MemoryLocationIdentity::Address(instruction.inputs.to_vec()),
        bytes,
    })
}

/// What two locations must have in common before `alias` can call them the
/// same one.
///
/// Grouping the locations a pass is tracking by this key turns "is any of these
/// the same location" from a walk over all of them into a lookup. It has to
/// agree with `alias`: two locations it gives different keys must never be
/// `Must`.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub(crate) enum LocationKey {
    /// An address reached through constants: the same base, offset and width
    /// is the same location however it was spelled.
    ConstantAddress(ValueId, i128, u8),
    /// An operand slot named by a constant index.
    ConstantOperand(i64),
    /// Anything else, where only being spelled identically makes it the same.
    AsSpelled(MemoryLocation),
}

pub(crate) fn location_key(function: &Function, location: &MemoryLocation) -> LocationKey {
    match &location.identity {
        MemoryLocationIdentity::Address(components) => match constant_address(function, components) {
            Some((base, offset)) => LocationKey::ConstantAddress(base, offset, location.bytes),
            None => LocationKey::AsSpelled(location.clone()),
        },
        MemoryLocationIdentity::IndexedOperand(index) => match constant_integer(function, *index) {
            Some(index) => LocationKey::ConstantOperand(index),
            None => LocationKey::AsSpelled(location.clone()),
        },
        MemoryLocationIdentity::BytecodeField(_) => LocationKey::AsSpelled(location.clone()),
    }
}

pub(crate) fn alias(function: &Function, lhs: &MemoryLocation, rhs: &MemoryLocation) -> AliasResult {
    if lhs == rhs {
        return AliasResult::Must;
    }
    match (&lhs.identity, &rhs.identity) {
        (MemoryLocationIdentity::Address(lhs_components), MemoryLocationIdentity::Address(rhs_components)) => {
            let (Some((lhs_base, lhs_offset)), Some((rhs_base, rhs_offset))) = (
                constant_address(function, lhs_components),
                constant_address(function, rhs_components),
            ) else {
                return AliasResult::May;
            };
            if lhs_base != rhs_base {
                return AliasResult::May;
            }
            // Two accesses of the same width off the same base at the same
            // offset are the same location, however they were spelled.
            if lhs_offset == rhs_offset && lhs.bytes == rhs.bytes {
                return AliasResult::Must;
            }
            let lhs_end = lhs_offset.saturating_add(lhs.bytes as i128);
            let rhs_end = rhs_offset.saturating_add(rhs.bytes as i128);
            if lhs_end <= rhs_offset || rhs_end <= lhs_offset {
                AliasResult::No
            } else {
                AliasResult::May
            }
        }
        (MemoryLocationIdentity::IndexedOperand(lhs), MemoryLocationIdentity::IndexedOperand(rhs)) => {
            match (constant_integer(function, *lhs), constant_integer(function, *rhs)) {
                (Some(lhs), Some(rhs)) if lhs == rhs => AliasResult::Must,
                (Some(_), Some(_)) => AliasResult::No,
                _ => AliasResult::May,
            }
        }
        _ => AliasResult::May,
    }
}

fn constant_address(function: &Function, components: &[ValueId]) -> Option<(ValueId, i128)> {
    let (base, components) = components.split_first()?;
    let offset = match components {
        [offset] => constant_integer(function, *offset)? as i128,
        [index, scale] => {
            (constant_integer(function, *index)? as i128).checked_mul(constant_integer(function, *scale)? as i128)?
        }
        _ => return None,
    };
    Some((*base, offset))
}

fn constant_integer(function: &Function, value: ValueId) -> Option<i64> {
    match function.values[value.0].definition {
        ValueDefinition::Constant(super::Constant::Integer(value)) => Some(value),
        _ => None,
    }
}

#[derive(Debug)]
pub(crate) struct DomainDependencies {
    instruction_accesses: Vec<Option<EffectAccess>>,
    #[cfg(test)]
    phi_inputs: Vec<Option<EffectSet>>,
    block_inputs: Vec<EffectSet>,
}

impl DomainDependencies {
    fn compute(function: &Function, cfg: &ControlFlowGraph, domain: EffectDomain) -> Self {
        let (reaching_inputs, _) = compute_domain_block_states(function, cfg, domain, &[]);

        let mut instruction_accesses = vec![None; function.instructions.len()];
        let mut terminator_accesses = vec![None; function.blocks.len()];
        let phi_inputs = reaching_inputs
            .iter()
            .enumerate()
            .map(|(block, inputs)| {
                (inputs.len() > 1 && cfg.predecessors(BlockId(block)).len() > 1).then(|| inputs.clone())
            })
            .collect::<Vec<_>>();
        let mut phi_blocks = phi_inputs.iter().map(Option::is_some).collect::<Vec<_>>();
        for block in 0..function.blocks.len() {
            for (_, failure) in function.guard_exits(BlockId(block)) {
                phi_blocks[failure.0] = true;
            }
        }
        let (block_inputs, mut block_outputs) = compute_domain_block_states(function, cfg, domain, &phi_blocks);

        for block_index in 0..function.blocks.len() {
            let block = BlockId(block_index);
            let mut dependencies = block_inputs[block_index].clone();
            for instruction in &function.blocks[block_index].instructions {
                let effect = effect_for(function.instructions[instruction.0].effects, domain);
                if effect == MemoryEffect::None {
                    continue;
                }
                let definition = effect.writes().then_some(EffectDefinition::Instruction(*instruction));
                instruction_accesses[instruction.0] = Some(EffectAccess {
                    effect,
                    dependencies: dependencies.clone(),
                    definition,
                });
                if let Some(definition) = definition {
                    dependencies = EffectSet::from_one(definition);
                }
            }

            let effect = terminator_effect(function.blocks[block_index].terminator.as_ref(), domain);
            if effect != MemoryEffect::None {
                let definition = effect.writes().then_some(EffectDefinition::Terminator(block));
                terminator_accesses[block_index] = Some(EffectAccess {
                    effect,
                    dependencies: dependencies.clone(),
                    definition,
                });
            }
            block_outputs[block_index] = if let Some(access) = &terminator_accesses[block_index]
                && let Some(definition) = access.definition
            {
                EffectSet::from_one(definition)
            } else {
                dependencies
            };
        }

        Self {
            instruction_accesses,
            #[cfg(test)]
            phi_inputs,
            block_inputs,
        }
    }

    pub(crate) fn instruction_access(&self, instruction: InstructionId) -> Option<&EffectAccess> {
        self.instruction_accesses[instruction.0].as_ref()
    }

    #[cfg(test)]
    pub(crate) fn phi_inputs(&self, block: BlockId) -> Option<&EffectSet> {
        self.phi_inputs[block.0].as_ref()
    }

    pub(crate) fn block_input(&self, block: BlockId) -> &EffectSet {
        &self.block_inputs[block.0]
    }
}

fn compute_domain_block_states(
    function: &Function,
    cfg: &ControlFlowGraph,
    domain: EffectDomain,
    phi_blocks: &[bool],
) -> (Vec<EffectSet>, Vec<EffectSet>) {
    // What a block leaves behind, when the block writes at all, is its own
    // last write and nothing that reached it. That does not depend on the
    // analysis, so it is found once rather than rediscovered by walking every
    // instruction on every sweep.
    let block_writes = function
        .blocks
        .iter()
        .enumerate()
        .map(|(block_index, block)| {
            if terminator_effect(block.terminator.as_ref(), domain).writes() {
                return Some(EffectDefinition::Terminator(BlockId(block_index)));
            }
            block
                .instructions
                .iter()
                .rev()
                .find(|instruction| effect_for(function.instructions[instruction.0].effects, domain).writes())
                .map(|instruction| EffectDefinition::Instruction(*instruction))
        })
        .collect::<Vec<_>>();

    let mut inputs = vec![EffectSet::new(); function.blocks.len()];
    let mut outputs = block_writes
        .iter()
        .map(|write| write.map(EffectSet::from_one).unwrap_or_default())
        .collect::<Vec<_>>();

    // A block's input comes from its predecessors, so visiting predecessors
    // first propagates a change the whole way in one sweep instead of one
    // block per sweep. Blocks the entry cannot reach are visited last, since
    // reverse postorder does not name them.
    let mut order = cfg.reverse_postorder().to_vec();
    order.extend(
        (0..function.blocks.len())
            .map(BlockId)
            .filter(|block| !cfg.is_reachable(*block)),
    );

    // Building each block's input into a scratch buffer and only storing it
    // when it actually moved keeps the later sweeps, where almost nothing
    // changes, from allocating a set per block and throwing it away.
    let mut input = Vec::new();
    loop {
        let mut changed = false;
        for id in order.iter().copied() {
            let block_index = id.0;
            input.clear();
            if id == function.entry || cfg.predecessors(id).is_empty() {
                input.push(EffectDefinition::Entry);
            } else if phi_blocks.get(block_index) == Some(&true) {
                input.push(EffectDefinition::Phi(id));
            } else {
                for predecessor in cfg.predecessors(id) {
                    input.extend(outputs[predecessor.0].iter().copied());
                }
                if input.len() > 1 {
                    input.sort_unstable();
                    input.dedup();
                }
            }
            if input.is_empty() {
                input.push(EffectDefinition::Entry);
            }
            if inputs[block_index].as_slice() != input {
                inputs[block_index] = EffectSet::from_sorted(input.clone());
                changed = true;
                // A block that writes ends the chain, so only one that does
                // not passes what reached it along.
                if block_writes[block_index].is_none() {
                    outputs[block_index] = inputs[block_index].clone();
                }
            }
        }
        if !changed {
            return (inputs, outputs);
        }
    }
}

fn effect_for(effects: Effects, domain: EffectDomain) -> MemoryEffect {
    match domain {
        EffectDomain::Memory => effects.memory,
        EffectDomain::MachineState => effects.machine_state,
    }
}

fn terminator_effect(terminator: Option<&Terminator>, domain: EffectDomain) -> MemoryEffect {
    match terminator {
        Some(Terminator::CheckedOperation { effects, .. }) => effect_for(*effects, domain),
        _ => MemoryEffect::None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::intrinsic::{OperandLoad, OperandOperation, OperandStore};
    use crate::ssa::{BlockLayout, Operation};
    use crate::types::Type;

    fn address(function: &mut Function, base: ValueId, offset: i64) -> ValueId {
        let offset = function.add_constant(Type::U64, super::super::Constant::Integer(offset));
        function.append_instruction(
            function.entry,
            Operation::Address,
            vec![base, offset],
            vec![Type::Memory],
        )[0]
    }

    #[test]
    fn records_reaching_memory_definitions_across_a_loop() {
        let mut function = Function::new("loop", vec![Type::Bool], Vec::new());
        let header = function.create_empty_block("header", BlockLayout::Hot);
        let body = function.create_empty_block("body", BlockLayout::Hot);
        let exit = function.create_empty_block("exit", BlockLayout::Hot);
        function.set_terminator(function.entry, Terminator::jump(header));
        let load = function.append_instruction_with_effects(
            header,
            Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field)),
            Vec::new(),
            vec![Type::I32],
            Effects {
                memory: MemoryEffect::Read,
                ..Effects::PURE
            },
        );
        function.set_terminator(header, Terminator::branch(function.parameter(0), body, exit));
        function.append_instruction_with_effects(
            body,
            Intrinsic::Operand(OperandOperation::Store(OperandStore::Field)),
            load,
            Vec::new(),
            Effects {
                memory: MemoryEffect::Write,
                ..Effects::PURE
            },
        );
        function.set_terminator(body, Terminator::jump(header));
        function.set_terminator(exit, Terminator::Return(Vec::new()));

        let cfg = ControlFlowGraph::compute(&function);
        let dependencies = EffectDependencies::compute(&function, &cfg);
        let memory = dependencies.domain(EffectDomain::Memory);
        let load = function.blocks[header.0].instructions[0];
        let store = function.blocks[body.0].instructions[0];

        assert_eq!(
            memory.instruction_access(load).unwrap().dependencies,
            EffectSet::from([EffectDefinition::Phi(header)])
        );
        assert_eq!(
            memory.instruction_access(store).unwrap().dependencies,
            EffectSet::from([EffectDefinition::Phi(header)])
        );
        assert_eq!(
            memory.phi_inputs(header).unwrap(),
            &EffectSet::from([EffectDefinition::Entry, EffectDefinition::Instruction(store)])
        );
    }

    #[test]
    fn keeps_memory_and_machine_state_dependencies_separate() {
        let mut function = Function::new("domains", Vec::new(), Vec::new());
        function.append_instruction_with_effects(
            function.entry,
            Operation::MachineAssign {
                destination: crate::types::InterpreterRegister::ProgramCounter,
                intrinsic: Intrinsic::LowLevel(crate::ssa::LowLevelOperation::Move),
            },
            Vec::new(),
            Vec::new(),
            Effects {
                machine_state: MemoryEffect::Write,
                ..Effects::PURE
            },
        );
        function.set_terminator(function.entry, Terminator::Return(Vec::new()));

        let cfg = ControlFlowGraph::compute(&function);
        let dependencies = EffectDependencies::compute(&function, &cfg);
        let machine_write = function.blocks[function.entry.0].instructions[0];

        assert!(
            dependencies
                .domain(EffectDomain::Memory)
                .instruction_access(machine_write)
                .is_none()
        );
        assert_eq!(
            dependencies
                .domain(EffectDomain::MachineState)
                .instruction_access(machine_write)
                .unwrap()
                .definition,
            Some(EffectDefinition::Instruction(machine_write))
        );
    }

    #[test]
    fn detects_writes_between_a_load_and_a_dominated_join() {
        let mut function = Function::new("join", vec![Type::Bool], Vec::new());
        let write = function.create_empty_block("write", BlockLayout::Hot);
        let clean = function.create_empty_block("clean", BlockLayout::Hot);
        let join = function.create_empty_block("join", BlockLayout::Hot);
        let load = function.append_instruction_with_effects(
            function.entry,
            Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field)),
            Vec::new(),
            vec![Type::I32],
            Effects {
                memory: MemoryEffect::Read,
                ..Effects::PURE
            },
        )[0];
        function.set_terminator(function.entry, Terminator::branch(function.parameter(0), write, clean));
        function.append_instruction_with_effects(
            write,
            Intrinsic::Operand(OperandOperation::Store(OperandStore::Field)),
            vec![load],
            Vec::new(),
            Effects {
                memory: MemoryEffect::Write,
                ..Effects::PURE
            },
        );
        function.set_terminator(write, Terminator::jump(join));
        function.set_terminator(clean, Terminator::jump(join));
        function.set_terminator(join, Terminator::Return(Vec::new()));

        let cfg = ControlFlowGraph::compute(&function);
        let dominators = DominatorTree::compute(&function, &cfg);
        let layout = InstructionLayout::compute(&function).unwrap();
        let dependencies = EffectDependencies::compute(&function, &cfg);
        let load = match function.values[load.0].definition {
            super::super::ValueDefinition::InstructionResult { instruction, .. } => instruction,
            _ => unreachable!(),
        };

        assert!(!dependencies.memory_is_stable_after_instruction(&dominators, &layout, load, join));
    }

    #[test]
    fn treats_unproven_pointer_relationships_as_may_alias() {
        let function = Function::new("aliases", vec![Type::U64, Type::U64], Vec::new());
        let first = MemoryLocation {
            identity: MemoryLocationIdentity::Address(vec![function.parameter(0)]),
            bytes: 8,
        };
        let second = MemoryLocation {
            identity: MemoryLocationIdentity::Address(vec![function.parameter(1)]),
            bytes: 8,
        };

        assert_eq!(alias(&function, &first, &second), AliasResult::May);
    }

    #[test]
    fn gives_typed_field_accesses_precise_memory_locations() {
        let mut function = Function::new("field", vec![Type::U64], Vec::new());
        let base = function.parameter(0);
        let address = address(&mut function, base, 4);
        let instruction = InstructionId(function.instructions.len());
        function.append_instruction_with_effects(
            function.entry,
            Operation::FieldAccess(crate::intrinsic::FieldAccess {
                kind: crate::intrinsic::FieldAccessKind::Load {
                    width: crate::intrinsic::FieldWidth::U8,
                    nonzero: false,
                },
                pair: None,
            }),
            vec![address],
            vec![Type::Bool],
            Effects {
                memory: MemoryEffect::Read,
                ..Effects::PURE
            },
        );

        let locations = instruction_memory_locations(&function, instruction);
        assert_eq!(locations.reads.len(), 1);
        assert_eq!(locations.reads[0].bytes, 1);
        assert!(!locations.unknown_read);
    }
}
