/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Memory forwarding and dead-store elimination over proven locations.

use super::effects::{
    AliasResult, EffectDefinition, MemoryAccessLocations, MemoryLocation, alias, instruction_memory_locations,
};
use super::optimize::{InstructionOrder, rebuild_instruction_arena, rewrite_function_uses};
use super::pass::{AnalysisManager};
use super::{Function, InstructionId, Intrinsic, OperandOperation, Operation, ValueId};
use std::collections::{HashMap, HashSet};

pub(super) fn run(function: &mut Function, analyses: &mut AnalysisManager) -> bool {
    let analysis = analyses.get(function);
    let memory = analysis.effects.domain(super::effects::EffectDomain::Memory);
    let mut replacements = HashMap::new();
    let mut eliminated = HashSet::new();
    let mut available_loads = Vec::<(InstructionId, MemoryLocation, ValueId)>::new();
    let mut load_candidates = 0u64;

    for block in analysis.cfg.reverse_postorder() {
        for instruction_id in &function.blocks[block.0].instructions {
            let access = instruction_memory_locations(function, *instruction_id);
            let Some(location) = simple_load(function, *instruction_id, &access) else {
                continue;
            };
            load_candidates += 1;
            let result = function.instructions[instruction_id.0].results[0];
            let result_type = &function.values[result.0].ty;
            let dependencies = memory
                .instruction_access(*instruction_id)
                .map(|access| &access.dependencies);

            let forwarded_store = (!function.instructions[instruction_id.0].effects.may_trap)
                .then_some(dependencies)
                .flatten()
                .and_then(|dependencies| {
                    let mut definitions = dependencies.iter();
                    match (definitions.next(), definitions.next()) {
                        (Some(EffectDefinition::Instruction(store)), None) => Some(*store),
                        _ => None,
                    }
                })
                .and_then(|store| {
                    let access = instruction_memory_locations(function, store);
                    let (stored_location, value) = simple_store(function, store, &access)?;
                    (alias(function, stored_location, location) == AliasResult::Must
                        && function.values[value.0].ty == *result_type)
                        .then_some(value)
                });
            // NB: A load that traps carries an assertion about what it loaded, so it can only reuse
            //     a load that made the same assertion. Reusing a plain load would drop the check.
            let traps = function.instructions[instruction_id.0].effects.may_trap;
            let forwarded_load = available_loads.iter().rev().find_map(|(load, loaded_location, value)| {
                ((!traps || function.instructions[load.0].effects.may_trap)
                    && dominates_instruction(&analysis, *load, *instruction_id)
                    && alias(function, loaded_location, location) == AliasResult::Must
                    && function.values[value.0].ty == *result_type
                    && (memory
                        .instruction_access(*load)
                        .map(|access| &access.dependencies)
                        == dependencies
                        || memory_unchanged_between(
                            function,
                            &analysis,
                            *load,
                            *instruction_id,
                            location,
                        )))
                    .then_some(*value)
            });

            if let Some(value) = forwarded_store.or(forwarded_load) {
                replacements.insert(result, value);
                eliminated.insert(*instruction_id);
            } else {
                available_loads.push((*instruction_id, location.clone(), result));
            }
        }
    }

    let forwarded_loads = replacements.len() as u64;
    analyses.report(
        super::report::OptimizationRemarkKind::Applied,
        "load-forwarding",
        forwarded_loads,
        "reused a value from an equivalent dominating load or store",
    );
    analyses.report(
        super::report::OptimizationRemarkKind::Missed,
        "load-forwarding",
        load_candidates - forwarded_loads,
        "no equivalent dominating load or store was available",
    );

    if replacements.is_empty() && eliminated.is_empty() {
        return false;
    }
    rewrite_function_uses(function, &replacements);
    rebuild_instruction_arena(function, &eliminated, InstructionOrder::ByBlock);
    true
}

fn memory_unchanged_between(
    function: &Function,
    analysis: &super::pass::FunctionAnalyses,
    load: InstructionId,
    instruction: InstructionId,
    location: &MemoryLocation,
) -> bool {
    let block = analysis.instruction_layout.block(load);
    if analysis.instruction_layout.block(instruction) != block {
        return false;
    }
    let start = analysis.instruction_layout.position(load) + 1;
    let end = analysis.instruction_layout.position(instruction);
    function.blocks[block.0].instructions[start..end]
        .iter()
        .all(|instruction| {
            let access = instruction_memory_locations(function, *instruction);
            !access.unknown_write
                && access
                    .writes
                    .iter()
                    .all(|write| alias(function, write, location) == AliasResult::No)
        })
}

fn dominates_instruction(
    analysis: &super::pass::FunctionAnalyses,
    dominator: InstructionId,
    instruction: InstructionId,
) -> bool {
    let dominator_block = analysis.instruction_layout.block(dominator);
    let instruction_block = analysis.instruction_layout.block(instruction);
    if dominator_block == instruction_block {
        analysis.instruction_layout.position(dominator) < analysis.instruction_layout.position(instruction)
    } else {
        analysis.dominators.dominates(dominator_block, instruction_block)
    }
}

pub(super) fn simple_load<'a>(
    function: &Function,
    instruction: InstructionId,
    access: &'a MemoryAccessLocations,
) -> Option<&'a MemoryLocation> {
    let instruction = &function.instructions[instruction.0];
    let is_single_load = match &instruction.operation {
        Operation::Intrinsic(Intrinsic::Memory(operation)) => !operation.writes() && operation.address_count() == 1,
        Operation::FieldAccess(access) => !access.kind.writes(),
        Operation::Intrinsic(Intrinsic::Operand(OperandOperation::Load(_))) => true,
        Operation::Intrinsic(Intrinsic::Operand(OperandOperation::LoadPair | OperandOperation::Decode
            | OperandOperation::Store(_) | OperandOperation::Copy)) => false,
        _ => false,
    };
    (instruction.results.len() == 1
        && access.reads.len() == 1
        && access.writes.is_empty()
        && is_single_load)
    .then(|| &access.reads[0])
}

fn simple_store<'a>(
    function: &Function,
    instruction: InstructionId,
    access: &'a MemoryAccessLocations,
) -> Option<(&'a MemoryLocation, ValueId)> {
    let instruction = &function.instructions[instruction.0];
    let is_single_store = match &instruction.operation {
        Operation::Intrinsic(Intrinsic::Memory(operation)) => operation.writes() && operation.address_count() == 1,
        Operation::FieldAccess(access) => access.kind.writes(),
        Operation::Intrinsic(Intrinsic::Operand(OperandOperation::Store(_))) => true,
        Operation::Intrinsic(Intrinsic::Operand(OperandOperation::Decode | OperandOperation::Load(_)
            | OperandOperation::LoadPair | OperandOperation::Copy)) => false,
        _ => false,
    };
    (instruction.results.is_empty()
        && access.writes.len() == 1
        && access.reads.is_empty()
        && is_single_store
        && instruction.inputs.len() == 2)
        .then(|| (&access.writes[0], instruction.inputs[1]))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::intrinsic::{MemoryOperation, MemoryWidth};
    use crate::types::Type;
    use crate::ssa::{BlockId, BlockLayout, Constant, Effects, MemoryEffect, Terminator};

    fn address(function: &mut Function, base: ValueId, offset: i64) -> ValueId {
        let block = function.entry;
        let offset = function.add_constant(Type::U64, Constant::Integer(offset));
        function.append_instruction(
            block,
            Operation::Address,
            vec![base, offset],
            vec![Type::Memory],
        )[0]
    }

    fn load(function: &mut Function, address: ValueId) -> ValueId {
        let block = function.entry;
        load_in(function, block, address)
    }

    fn load_in(function: &mut Function, block: BlockId, address: ValueId) -> ValueId {
        function.append_instruction_with_effects(
            block,
            Intrinsic::Memory(MemoryOperation::Load { width: MemoryWidth::DoubleWord, signed: false }),
            vec![address],
            vec![Type::U64],
            Effects {
                memory: MemoryEffect::Read,
                ..Effects::PURE
            },
        )[0]
    }

    fn store(function: &mut Function, address: ValueId, value: ValueId) -> InstructionId {
        let block = function.entry;
        store_in(function, block, address, value)
    }

    fn store_in(function: &mut Function, block: BlockId, address: ValueId, value: ValueId) -> InstructionId {
        let instruction = InstructionId(function.instructions.len());
        function.append_instruction_with_effects(
            block,
            Intrinsic::Memory(MemoryOperation::Store(MemoryWidth::DoubleWord)),
            vec![address, value],
            Vec::new(),
            Effects {
                memory: MemoryEffect::Write,
                ..Effects::PURE
            },
        );
        instruction
    }

    fn optimize(function: &mut Function) {
        run(function, &mut AnalysisManager::default());
        function.validate().unwrap();
    }

    #[test]
    fn forwards_values_from_must_alias_stores() {
        let mut function = Function::new("forward", vec![Type::U64, Type::U64], vec![Type::U64]);
        let base = function.parameter(0);
        let stored_value = function.parameter(1);
        let location = address(&mut function, base, 0);
        store(&mut function, location, stored_value);
        let loaded = load(&mut function, location);
        function.set_terminator(function.entry, Terminator::Return(vec![loaded]));

        optimize(&mut function);

        assert_eq!(function.instructions.len(), 2);
        let Terminator::Return(values) = function.blocks[function.entry.0].terminator.as_ref().unwrap() else {
            panic!("expected a return")
        };
        assert_eq!(values, &[function.parameter(1)]);
    }

    #[test]
    fn forwards_stores_into_dominated_blocks() {
        let mut function = Function::new("global-forward", vec![Type::U64, Type::U64], vec![Type::U64]);
        let successor = function.create_empty_block("successor", BlockLayout::Hot);
        let base = function.parameter(0);
        let stored_value = function.parameter(1);
        let location = address(&mut function, base, 0);
        store(&mut function, location, stored_value);
        function.set_terminator(
            function.entry,
            Terminator::jump(successor),
        );
        let loaded = load_in(&mut function, successor, location);
        function.set_terminator(successor, Terminator::Return(vec![loaded]));

        optimize(&mut function);

        assert_eq!(function.instructions.len(), 2);
        let Terminator::Return(values) = function.blocks[successor.0].terminator.as_ref().unwrap() else {
            panic!("expected a return")
        };
        assert_eq!(values, &[function.parameter(1)]);
    }

    #[test]
    fn reuses_loads_across_dominated_blocks() {
        let mut function = Function::new("global-load", vec![Type::U64], vec![Type::U64]);
        let successor = function.create_empty_block("successor", BlockLayout::Hot);
        let base = function.parameter(0);
        let location = address(&mut function, base, 0);
        let first = load(&mut function, location);
        function.set_terminator(
            function.entry,
            Terminator::jump(successor),
        );
        let second = load_in(&mut function, successor, location);
        function.set_terminator(successor, Terminator::Return(vec![second]));

        optimize(&mut function);

        assert_eq!(function.instructions.len(), 2);
        let Terminator::Return(values) = function.blocks[successor.0].terminator.as_ref().unwrap() else {
            panic!("expected a return")
        };
        assert_eq!(values, &[first]);
    }

    #[test]
    fn preserves_loads_across_may_alias_stores() {
        let mut function = Function::new("may-alias", vec![Type::U64, Type::U64], vec![Type::U64]);
        let first_base = function.parameter(0);
        let second_base = function.parameter(1);
        let stored = address(&mut function, first_base, 0);
        let loaded = address(&mut function, second_base, 0);
        store(&mut function, stored, first_base);
        let value = load(&mut function, loaded);
        function.set_terminator(function.entry, Terminator::Return(vec![value]));

        optimize(&mut function);

        assert_eq!(function.instructions.len(), 4);
    }

    #[test]
    fn preserves_stores_observed_through_may_alias_loads() {
        let mut function = Function::new("may-alias-read", vec![Type::U64, Type::U64], vec![Type::U64]);
        let first_base = function.parameter(0);
        let second_base = function.parameter(1);
        let stored = address(&mut function, first_base, 0);
        let observed = address(&mut function, second_base, 0);
        store(&mut function, stored, first_base);
        let value = load(&mut function, observed);
        store(&mut function, stored, second_base);
        function.set_terminator(function.entry, Terminator::Return(vec![value]));

        optimize(&mut function);

        assert_eq!(function.instructions.len(), 5);
    }

    #[test]
    fn preserves_available_loads_across_disjoint_stores() {
        let mut function = Function::new("no-alias", vec![Type::U64, Type::U64], vec![Type::U64]);
        let base = function.parameter(0);
        let stored_value = function.parameter(1);
        let first_location = address(&mut function, base, 0);
        let second_location = address(&mut function, base, 8);
        let first = load(&mut function, first_location);
        store(&mut function, second_location, stored_value);
        let second = load(&mut function, first_location);
        function.set_terminator(function.entry, Terminator::Return(vec![second]));

        optimize(&mut function);

        assert_eq!(function.instructions.len(), 4);
        let Terminator::Return(values) = function.blocks[function.entry.0].terminator.as_ref().unwrap() else {
            panic!("expected a return")
        };
        assert_eq!(values, &[first]);
    }

    #[test]
    fn keeps_a_trapping_load_that_only_follows_a_plain_load() {
        let mut function = Function::new("nonzero-after-plain", vec![Type::U64], vec![Type::U64]);
        let base = function.parameter(0);
        let location = address(&mut function, base, 0);
        load(&mut function, location);
        let checked = function.append_instruction_with_effects(
            function.entry,
            Operation::FieldAccess(crate::intrinsic::FieldAccess {
                kind: crate::intrinsic::FieldAccessKind::Load {
                    width: crate::intrinsic::FieldWidth::U64,
                    nonzero: true,
                },
                pair: None,
            }),
            vec![location],
            vec![Type::U64],
            Effects {
                memory: MemoryEffect::Read,
                may_trap: true,
                ..Effects::PURE
            },
        )[0];
        function.set_terminator(function.entry, Terminator::Return(vec![checked]));

        optimize(&mut function);

        // The plain load made no assertion about what it loaded, so the checked load has to stay.
        assert_eq!(function.instructions.len(), 3);
        let Terminator::Return(values) = function.blocks[function.entry.0].terminator.as_ref().unwrap() else {
            panic!("expected a return")
        };
        assert_eq!(values, &[checked]);
    }

    #[test]
    fn preserves_nonzero_load_assertions() {
        let mut function = Function::new("nonzero", vec![Type::U64, Type::U64], vec![Type::U64]);
        let base = function.parameter(0);
        let stored_value = function.parameter(1);
        let location = address(&mut function, base, 0);
        store(&mut function, location, stored_value);
        let first = function.append_instruction_with_effects(
            function.entry,
            Operation::FieldAccess(crate::intrinsic::FieldAccess {
                kind: crate::intrinsic::FieldAccessKind::Load {
                    width: crate::intrinsic::FieldWidth::U64,
                    nonzero: true,
                },
                pair: None,
            }),
            vec![location],
            vec![Type::U64],
            Effects {
                memory: MemoryEffect::Read,
                may_trap: true,
                ..Effects::PURE
            },
        )[0];
        let second = function.append_instruction_with_effects(
            function.entry,
            function.instructions.last().unwrap().operation.clone(),
            vec![location],
            vec![Type::U64],
            Effects {
                memory: MemoryEffect::Read,
                may_trap: true,
                ..Effects::PURE
            },
        )[0];
        function.set_terminator(function.entry, Terminator::Return(vec![second]));

        optimize(&mut function);

        assert_eq!(function.instructions.len(), 3);
        let Terminator::Return(values) = function.blocks[function.entry.0].terminator.as_ref().unwrap() else {
            panic!("expected a return")
        };
        assert_eq!(values, &[first]);
    }
}
