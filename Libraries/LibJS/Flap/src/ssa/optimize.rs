/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Target-independent SSA optimizations.

use super::analysis::{ControlFlowGraph, reachable_blocks};
use super::effects::{EffectDefinition, EffectDomain};
use super::pass::{AnalysisManager, FunctionPass, PassManagerOptions, PassRunner};
use super::report::FunctionOptimizationReport;
use super::{BlockId, BlockLayout, Edge, Effects, Function, Instruction, InstructionId, Intrinsic, OperandOperation, Operation, Terminator, ValueDefinition, ValueId, ValueOperation};
use crate::intrinsic::{OperandLoad, OperandStore};
#[cfg(test)]
use super::{BinaryOperation, IntegerBinaryOperation};
use crate::frontend::layout::{LayoutConstants, LayoutValue};
use crate::types::Type;
use crate::{CompileError, CompileStage};
#[cfg(test)]
use crate::identity::ExternalSymbol;
use std::collections::{BTreeSet, HashMap, HashSet};

pub(crate) fn resolve_constants(function: &mut Function, constants: &LayoutConstants) {
    let values = function
        .blocks
        .iter()
        .filter_map(|block| match &block.terminator {
            Some(Terminator::Switch { cases, .. }) => Some(cases),
            _ => None,
        })
        .flat_map(|cases| cases.iter().map(|(pattern, _)| *pattern))
        .chain(
            function
                .instructions
                .iter()
                .filter(|instruction| instruction.operation == Operation::Address)
                .flat_map(|instruction| instruction.inputs.iter().skip(1).copied()),
        )
        .collect::<HashSet<_>>();
    for value_id in values {
        let value = &mut function.values[value_id.0];
        let constant = match &value.definition {
            ValueDefinition::Constant(
                super::ir::Constant::KnownLayout(known),
            ) => constants.known(*known),
            ValueDefinition::Constant(
                super::ir::Constant::LayoutValue(
                    LayoutValue::Constant(constant),
                ),
            ) => Some(*constant),
            ValueDefinition::Constant(
                super::ir::Constant::Symbol(name),
            ) => constants.get(name),
            _ => None,
        };
        let Some(constant) = constant else {
            continue;
        };
        value.definition =
            ValueDefinition::Constant(super::ir::Constant::Integer(constant.value()));
    }
}

pub(crate) fn resolve_layout_constants(
    function: &mut Function,
    constants: &LayoutConstants,
) -> Result<(), CompileError> {
    for value in &mut function.values {
        if let ValueDefinition::Constant(
            super::ir::Constant::KnownLayout(known),
        ) = value.definition
        {
            let constant = constants
                .known(known)
                .ok_or_else(|| {
                    CompileError::new(
                        CompileStage::Ssa,
                        Some(&function.name),
                        format!("unknown constant '{}'", known.name()),
                    )
                })?;
            value.definition = ValueDefinition::Constant(
                super::ir::Constant::Layout(constant),
            );
            continue;
        }
        if let ValueDefinition::Constant(
            super::ir::Constant::LayoutValue(layout_value),
        ) = value.definition
        {
            value.definition = ValueDefinition::Constant(
                match layout_value {
                    LayoutValue::Immediate(value) => {
                        super::ir::Constant::Integer(value)
                    }
                    LayoutValue::Constant(constant) => {
                        super::ir::Constant::Layout(constant)
                    }
                },
            );
            continue;
        }
        let ValueDefinition::Constant(super::ir::Constant::Symbol(name)) =
            &value.definition
        else {
            continue;
        };
        if let Ok(integer) = name.parse() {
            value.definition =
                ValueDefinition::Constant(super::ir::Constant::Integer(integer));
            continue;
        }
        let constant = constants
            .get(name)
            .ok_or_else(|| {
                CompileError::new(
                    CompileStage::Ssa,
                    Some(&function.name),
                    format!("unknown constant '{name}'"),
                )
            })?;
        value.definition =
            ValueDefinition::Constant(super::ir::Constant::Layout(constant));
    }
    Ok(())
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
struct Expression {
    operation: Operation,
    inputs: Vec<ValueId>,
    result_types: Vec<Type>,
    effects: Effects,
    memory_dependencies: BTreeSet<EffectDefinition>,
    machine_state_dependencies: BTreeSet<EffectDefinition>,
}

pub(crate) fn optimize_function(function: &mut Function) {
    run_optimization_pipeline(function, None);
}

pub(crate) fn optimize_function_with_report(
    function: &mut Function,
    options: PassManagerOptions,
) -> FunctionOptimizationReport {
    run_optimization_pipeline(function, Some(options)).expect("reporting was requested")
}

pub(crate) fn fuse_operand_accesses(function: &mut Function) {
    let mut eliminated = HashSet::new();
    for block in &function.blocks {
        for pair in block.instructions.windows(2) {
            let [first_id, second_id] = pair else { unreachable!() };
            if eliminated.contains(first_id) {
                continue;
            }
            let Some((source, result)) = operand_field_load(&function.instructions[first_id.0]) else {
                continue;
            };
            let second = &function.instructions[second_id.0];
            let (operation, inputs, results) = if matches!(
                second.operation,
                Operation::Intrinsic(Intrinsic::Operand(
                    OperandOperation::Store(OperandStore::Field),
                ))
            ) && let [destination, stored] = second.inputs.as_slice()
                && second.results.is_empty()
                && *stored == result
            {
                (OperandOperation::Copy, vec![*destination, source], Vec::new())
            } else if let Some((rhs, rhs_result)) = operand_field_load(second)
                && binary_operand_pair(function, source, rhs)
            {
                (OperandOperation::LoadPair, vec![source, rhs], vec![result, rhs_result])
            } else {
                continue;
            };
            let first = &mut function.instructions[first_id.0];
            first.operation = Operation::Intrinsic(Intrinsic::Operand(operation));
            first.inputs = inputs;
            first.results = results;
            let memory = if operation == OperandOperation::Copy { super::MemoryEffect::ReadWrite } else { super::MemoryEffect::Read };
            first.effects = Effects { memory, ..Effects::PURE };
            if operation == OperandOperation::Copy {
                function.values[result.0].definition = ValueDefinition::Dead;
            }
            eliminated.insert(*second_id);
        }
    }
    if !eliminated.is_empty() {
        rebuild_instruction_arena(function, &eliminated, InstructionOrder::Original);
        recompute_effects(function);
    }
}

fn operand_field_load(instruction: &Instruction) -> Option<(ValueId, ValueId)> {
    match (&instruction.operation, instruction.inputs.as_slice(), instruction.results.as_slice()) {
        (Operation::Intrinsic(Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field))), [input], [result]) => Some((*input, *result)),
        _ => None,
    }
}

fn recompute_effects(function: &mut Function) {
    for index in 0..function.instructions.len() {
        let instruction = &function.instructions[index];
        if matches!(instruction.operation, Operation::Intrinsic(Intrinsic::Operand(OperandOperation::LoadPair | OperandOperation::Copy))) {
            continue;
        }
        let effects = function.effects_for_operation(&instruction.operation, &instruction.inputs);
        function.instructions[index].effects = effects;
    }
}

fn binary_operand_pair(function: &Function, lhs: ValueId, rhs: ValueId) -> bool {
    let parameter_name = |value: ValueId| {
        let ValueDefinition::FunctionParameter(parameter) = function.values[value.0].definition else {
            return None;
        };
        function.parameter_names.get(parameter).map(String::as_str)
    };
    matches!(
        (parameter_name(lhs), parameter_name(rhs)),
        (Some("lhs" | "m_lhs"), Some("rhs" | "m_rhs"))
    )
}

fn run_optimization_pipeline(
    function: &mut Function,
    options: Option<PassManagerOptions>,
) -> Option<FunctionOptimizationReport> {
    let mut runner = PassRunner::new(options);
    let mut report = options.map(|_| FunctionOptimizationReport::new(&function.name));
    macro_rules! run_pass {
        ($name:literal, $pass:expr) => {
            if let (_, Some(pass)) = runner.run(function, $name, $pass) {
                report.as_mut().unwrap().push_pass(&pass);
            }
        };
    }
    run_pass!("sparse-conditional-constant-propagation", super::sccp::run);

    let mut iterations = Vec::new();
    let mut converged = false;
    for _ in 1..=2 {
        let mut passes = Vec::new();
        let mut changed = false;
        for (name, pass) in [
            (
                "eliminate-trivial-block-parameters",
                eliminate_trivial_block_parameters_pass as FunctionPass,
            ),
            ("resolve-block-references", resolve_block_references),
        ] {
            let (pass_changed, report) = runner.run(function, name, pass);
            changed |= pass_changed;
            if let Some(report) = report {
                passes.push(report);
            }
        }
        if options.is_some() {
            iterations.push(passes);
        }
        if !changed {
            converged = true;
            break;
        }
    }
    if let Some(report) = &mut report {
        report.push_fixed_point("block-parameter-cleanup", 2, converged, &iterations);
    }
    run_pass!("memory-optimization", super::memory_optimize::run);
    run_pass!(
        "global-common-subexpression-elimination",
        eliminate_common_subexpressions_pass
    );
    run_pass!("dead-code-elimination", eliminate_dead_code_pass);
    run_pass!("global-code-motion", schedule_global_code_motion_pass);
    report
}

#[cfg(test)]
fn run_single_pass(function: &mut Function, name: &'static str, pass: FunctionPass) {
    PassRunner::new(None).run(function, name, pass);
}

fn resolve_block_references(function: &mut Function, _: &mut AnalysisManager) -> bool {
    let mut changed = false;
    for block_index in 0..function.blocks.len() {
        let Some(Terminator::IndirectJump { target }) = function.blocks[block_index].terminator else {
            continue;
        };
        let ValueDefinition::InstructionResult { instruction, .. } = function.values[target.0].definition else {
            continue;
        };
        let instruction = &function.instructions[instruction.0];
        let Operation::BlockReference(target) = instruction.operation else {
            continue;
        };
        function.blocks[block_index].terminator = Some(Terminator::jump_with_arguments(
            target,
            instruction.inputs.clone(),
        ));
        changed = true;
    }
    changed
}

#[cfg(test)]
pub(crate) fn eliminate_trivial_block_parameters(function: &mut Function) {
    run_single_pass(
        function,
        "eliminate-trivial-block-parameters",
        eliminate_trivial_block_parameters_pass,
    );
}

fn eliminate_trivial_block_parameters_pass(function: &mut Function, _: &mut AnalysisManager) -> bool {
    let mut changed = false;
    loop {
        let mut replacements = HashMap::new();
        for (block_index, block) in function.blocks.iter().enumerate() {
            let block_id = BlockId(block_index);
            for (parameter_index, parameter) in block.parameters.iter().enumerate() {
                let incoming = function
                    .blocks
                    .iter()
                    .flat_map(|predecessor| predecessor.terminator.as_ref().unwrap().successors())
                    .filter(|edge| edge.block == block_id)
                    .map(|edge| edge.arguments[parameter_index])
                    .filter(|argument| argument != parameter)
                    .collect::<Vec<_>>();
                let Some(replacement) = incoming.first().copied() else {
                    continue;
                };
                if matches!(
                    function.values[replacement.0].definition,
                    ValueDefinition::TerminatorResult { .. }
                ) {
                    continue;
                }
                if incoming.iter().all(|incoming| *incoming == replacement) {
                    replacements.insert(*parameter, replacement);
                }
            }
        }

        let candidates = replacements.clone();
        replacements.retain(|parameter, _| {
            let mut value = *parameter;
            let mut visited = HashSet::new();
            while let Some(replacement) = candidates.get(&value) {
                if !visited.insert(value) {
                    return false;
                }
                value = *replacement;
            }
            true
        });
        if replacements.is_empty() {
            break;
        }
        changed = true;

        rewrite_function_uses(function, &replacements);

        let removed = replacements.keys().copied().collect::<HashSet<_>>();
        let removed_indices = function
            .blocks
            .iter()
            .map(|block| {
                block
                    .parameters
                    .iter()
                    .enumerate()
                    .filter_map(|(index, parameter)| removed.contains(parameter).then_some(index))
                    .collect::<HashSet<_>>()
            })
            .collect::<Vec<_>>();
        for block in &mut function.blocks {
            for edge in terminator_edges_mut(block.terminator.as_mut().unwrap()) {
                let indices = &removed_indices[edge.block.0];
                let mut index = 0usize;
                edge.arguments.retain(|_| {
                    let keep = !indices.contains(&index);
                    index += 1;
                    keep
                });
            }
        }
        for instruction in &mut function.instructions {
            let Operation::BlockReference(block) = instruction.operation else {
                continue;
            };
            let indices = &removed_indices[block.0];
            let mut index = 0usize;
            instruction.inputs.retain(|_| {
                let keep = !indices.contains(&index);
                index += 1;
                keep
            });
        }
        for (block_index, block) in function.blocks.iter_mut().enumerate() {
            for parameter in block.parameters.extract_if(.., |parameter| removed.contains(parameter)) {
                function.values[parameter.0].definition = ValueDefinition::Dead;
            }
            for (parameter_index, parameter) in block.parameters.iter().enumerate() {
                function.values[parameter.0].definition = ValueDefinition::BlockParameter {
                    block: BlockId(block_index),
                    index: parameter_index,
                };
            }
        }
    }

    changed
}

pub(super) fn eliminate_unreachable_blocks(function: &mut Function) -> bool {
    let cfg = ControlFlowGraph::compute(function);
    let reachable = reachable_blocks(function, &cfg);
    if reachable.iter().all(|reachable| *reachable) {
        return false;
    }

    let eliminated_instructions = function
        .blocks
        .iter()
        .enumerate()
        .filter(|(index, _)| !reachable[*index])
        .flat_map(|(_, block)| block.instructions.iter().copied())
        .collect::<HashSet<_>>();
    rebuild_instruction_arena(function, &eliminated_instructions, InstructionOrder::ByBlock);

    let mut block_map = vec![None; function.blocks.len()];
    let mut blocks = Vec::new();
    for (old_index, block) in std::mem::take(&mut function.blocks).into_iter().enumerate() {
        if !reachable[old_index] {
            for parameter in block.parameters {
                function.values[parameter.0].definition = ValueDefinition::Dead;
            }
            if let Some(Terminator::CheckedOperation { results, .. }) = block.terminator {
                for result in results {
                    function.values[result.0].definition = ValueDefinition::Dead;
                }
            }
            continue;
        }
        let new_block = BlockId(blocks.len());
        block_map[old_index] = Some(new_block);
        blocks.push(block);
    }

    for (block_index, block) in blocks.iter_mut().enumerate() {
        let block_id = BlockId(block_index);
        for (parameter_index, parameter) in block.parameters.iter().enumerate() {
            function.values[parameter.0].definition = ValueDefinition::BlockParameter {
                block: block_id,
                index: parameter_index,
            };
        }
        let terminator = block.terminator.as_mut().unwrap();
        if let Terminator::CheckedOperation { results, .. } = terminator {
            for (result_index, result) in results.iter().enumerate() {
                function.values[result.0].definition = ValueDefinition::TerminatorResult {
                    block: block_id,
                    index: result_index,
                };
            }
        }
        for edge in terminator_edges_mut(terminator) {
            edge.block = block_map[edge.block.0].expect("reachable block cannot target an unreachable block");
        }
    }

    for instruction in &mut function.instructions {
        let Operation::BlockReference(target) = &mut instruction.operation else {
            continue;
        };
        *target = block_map[target.0].expect("reachable block reference cannot target an unreachable block");
    }

    function.entry = block_map[function.entry.0].expect("the entry block is always reachable");
    function.blocks = blocks;
    true
}

fn terminator_edges_mut(terminator: &mut Terminator) -> Vec<&mut Edge> {
    match terminator {
        Terminator::Jump(edge) => vec![edge],
        Terminator::Branch {
            then_edge,
            else_edge,
            ..
        } => vec![then_edge, else_edge],
        Terminator::Switch { cases, default, .. } => {
            cases.iter_mut().map(|(_, edge)| edge).chain([default]).collect()
        }
        Terminator::CheckedOperation {
            success, failure, ..
        } => vec![success, failure],
        Terminator::IndirectJump { .. } | Terminator::Return(_) | Terminator::Unreachable => Vec::new(),
    }
}

#[cfg(test)]
pub(crate) fn eliminate_common_subexpressions(function: &mut Function) {
    run_single_pass(
        function,
        "global-common-subexpression-elimination",
        eliminate_common_subexpressions_pass,
    );
}

fn eliminate_common_subexpressions_pass(
    function: &mut Function,
    analyses: &mut AnalysisManager,
) -> bool {
    let mut replacements = HashMap::new();
    let mut eliminated = HashSet::new();
    let analyses = analyses.get(function);
    let dominators = analyses.dominators;
    let mut worklist = vec![(function.entry, HashMap::<Expression, Vec<ValueId>>::new())];
    while let Some((block, mut available)) = worklist.pop() {
        for instruction_id in function.blocks[block.0].instructions.clone() {
            let instruction = &mut function.instructions[instruction_id.0];
            rewrite_values(&mut instruction.inputs, &replacements);
            // The current allocator cannot preserve virtual registers across
            // calls, and sharing across a trapping check increases pressure on
            // paths that may exit. Keep cheap expressions rematerializable on
            // each side of those boundaries.
            if instruction.effects.may_call || instruction.effects.may_trap {
                available.clear();
                continue;
            }
            if !instruction.effects.can_be_eliminated()
                // FIXME: Sharing cold reloads with their hot-path originals
                //        makes the register allocator preserve values across
                //        cold edges, adding copies to hot arithmetic paths.
                //        Remove this exception once allocation uses block
                //        frequency to choose preservation versus reloading.
                || (instruction.effects != Effects::PURE
                    && function.blocks[block.0].layout == BlockLayout::Cold)
                || instruction.operation == Operation::Intrinsic(Intrinsic::LowLevel(super::LowLevelOperation::Move))
                || matches!(instruction.operation, Operation::Intrinsic(Intrinsic::Value(operation))
                    if operation == ValueOperation::Reuse
                        || operation.is_rematerialized())
            {
                continue;
            }
            let effect_dependencies = |domain| {
                analyses
                    .effects
                    .domain(domain)
                    .instruction_access(instruction_id)
                    .map(|access| access.dependencies.clone())
                    .unwrap_or_default()
            };
            let expression = Expression {
                operation: instruction.operation.clone(),
                inputs: instruction.inputs.clone(),
                result_types: instruction
                    .results
                    .iter()
                    .map(|result| function.values[result.0].ty.clone())
                    .collect(),
                effects: instruction.effects,
                memory_dependencies: analyses
                    .effects
                    .domain(EffectDomain::Memory)
                    .instruction_access(instruction_id)
                    .map(|access| access.dependencies.clone())
                    .unwrap_or_default(),
                machine_state_dependencies: effect_dependencies(EffectDomain::MachineState),
            };
            if let Some(existing_results) = available.get(&expression) {
                for (result, existing) in instruction.results.iter().zip(existing_results) {
                    replacements.insert(*result, *existing);
                }
                eliminated.insert(instruction_id);
            } else {
                available.insert(expression, instruction.results.clone());
            }
        }
        for child in dominators.children(block).iter().rev() {
            worklist.push((*child, available.clone()));
        }
    }

    if eliminated.is_empty() {
        return false;
    }

    rewrite_function_uses(function, &replacements);
    rebuild_instruction_arena(function, &eliminated, InstructionOrder::ByBlock);
    true
}

#[derive(Debug, Clone, Copy)]
enum InstructionUse {
    Instruction(InstructionId),
    Terminator(BlockId),
}

#[cfg(test)]
pub(crate) fn schedule_global_code_motion(function: &mut Function) {
    run_single_pass(function, "global-code-motion", schedule_global_code_motion_pass);
}

fn schedule_global_code_motion_pass(
    function: &mut Function,
    analyses: &mut AnalysisManager,
) -> bool {
    let analyses = analyses.get(function);
    let dominators = &analyses.dominators;
    let instruction_layout = &analyses.instruction_layout;
    let mut loop_depths = vec![0usize; function.blocks.len()];
    for natural_loop in analyses.loops {
        for block in &natural_loop.blocks {
            loop_depths[block.0] += 1;
        }
    }

    let movable = function
        .instructions
        .iter()
        .map(instruction_is_globally_movable)
        .collect::<Vec<_>>();
    let uses = collect_instruction_uses(function);
    let original_placements = (0..function.instructions.len())
        .map(|index| instruction_layout.block(InstructionId(index)))
        .collect::<Vec<_>>();
    let mut placements = original_placements.clone();

    loop {
        let mut changed = false;
        for instruction_index in (0..function.instructions.len()).rev() {
            if !movable[instruction_index] {
                continue;
            }
            let instruction = InstructionId(instruction_index);
            let use_blocks = function.instructions[instruction_index]
                .results
                .iter()
                .flat_map(|result| uses[result.0].iter())
                .map(|use_site| match use_site {
                    InstructionUse::Instruction(user) => placements[user.0],
                    InstructionUse::Terminator(block) => *block,
                });
            let Some(latest) = use_blocks.reduce(|lhs, rhs| {
                dominators
                    .nearest_common_dominator(lhs, rhs)
                    .expect("uses of a valid SSA value have a common dominator")
            }) else {
                continue;
            };
            let source = original_placements[instruction.0];
            if !dominators.dominates(source, latest) {
                continue;
            }
            let placement = cheapest_motion_block(
                function,
                source,
                latest,
                dominators,
                &loop_depths,
            );
            if placements[instruction.0] != placement {
                placements[instruction.0] = placement;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }

    let mut ranks = vec![usize::MAX; function.instructions.len()];
    let mut next_rank = 0;
    for block in &function.blocks {
        for instruction in &block.instructions {
            ranks[instruction.0] = next_rank;
            next_rank += 1;
        }
    }
    let mut assigned = vec![Vec::new(); function.blocks.len()];
    for block in &function.blocks {
        for instruction in &block.instructions {
            assigned[placements[instruction.0].0].push(*instruction);
        }
    }
    for instructions in &mut assigned {
        instructions.sort_by_key(|instruction| ranks[instruction.0]);
    }

    let mut schedules = vec![Vec::new(); function.blocks.len()];
    for block_index in 0..function.blocks.len() {
        let block = BlockId(block_index);
        let mut emitted = HashSet::new();
        for instruction in &assigned[block_index] {
            if movable[instruction.0] {
                continue;
            }
            schedule_value_dependencies(
                function,
                block,
                &function.instructions[instruction.0].inputs,
                &placements,
                &movable,
                &ranks,
                &mut emitted,
                &mut schedules[block_index],
            );
            if emitted.insert(*instruction) {
                schedules[block_index].push(*instruction);
            }
        }
        schedule_value_dependencies(
            function,
            block,
            &function.blocks[block_index]
                .terminator
                .as_ref()
                .expect("valid SSA blocks have terminators")
                .inputs(),
            &placements,
            &movable,
            &ranks,
            &mut emitted,
            &mut schedules[block_index],
        );
        for instruction in &assigned[block_index] {
            schedule_instruction(
                function,
                block,
                *instruction,
                &placements,
                &movable,
                &ranks,
                &mut emitted,
                &mut schedules[block_index],
            );
        }
    }

    let changed = function
        .blocks
        .iter()
        .zip(&schedules)
        .any(|(block, schedule)| block.instructions != *schedule);
    if !changed {
        return false;
    }
    for (block, schedule) in function.blocks.iter_mut().zip(schedules) {
        block.instructions = schedule;
    }
    true
}

fn instruction_is_globally_movable(instruction: &super::Instruction) -> bool {
    instruction.effects == Effects::PURE
        && !instruction.results.is_empty()
        && match &instruction.operation {
            Operation::Address => true,
            Operation::Intrinsic(Intrinsic::Address(_)) => true,
            Operation::FieldAccess(_) => true,
            Operation::Intrinsic(Intrinsic::LowLevel(operation)) => *operation != super::LowLevelOperation::Move,
            Operation::Intrinsic(Intrinsic::Classification(_)) | Operation::Intrinsic(Intrinsic::FloatingPoint(_)) | Operation::Intrinsic(Intrinsic::IntegerBinary(_)) | Operation::Intrinsic(Intrinsic::IntegerComparison(_))
            | Operation::Intrinsic(Intrinsic::Operand(_)) => true,
            Operation::Intrinsic(Intrinsic::Value(operation)) => {
                *operation != ValueOperation::Reuse
                    && !operation.is_rematerialized()
            }
            Operation::Intrinsic(Intrinsic::Aggregate(_)) | Operation::Intrinsic(Intrinsic::Assertion(_)) | Operation::Intrinsic(Intrinsic::Branch(_)) | Operation::Intrinsic(Intrinsic::Bytecode(_)) | Operation::Intrinsic(Intrinsic::Call(_)) | Operation::Intrinsic(Intrinsic::CheckedInteger(_)) | Operation::Intrinsic(Intrinsic::Control(_)) | Operation::Intrinsic(Intrinsic::Memory(_)) | Operation::InlineCall(_) | Operation::Parameter(_)
            | Operation::MachineAssign { .. }
            | Operation::BlockReference(_) => false,
        }
}

fn collect_instruction_uses(function: &Function) -> Vec<Vec<InstructionUse>> {
    let mut uses = vec![Vec::new(); function.values.len()];
    for (instruction_index, instruction) in function.instructions.iter().enumerate() {
        for input in &instruction.inputs {
            uses[input.0].push(InstructionUse::Instruction(InstructionId(instruction_index)));
        }
    }
    for (block_index, block) in function.blocks.iter().enumerate() {
        for input in block
            .terminator
            .as_ref()
            .expect("valid SSA blocks have terminators")
            .inputs()
        {
            uses[input.0].push(InstructionUse::Terminator(BlockId(block_index)));
        }
    }
    uses
}

fn cheapest_motion_block(
    function: &Function,
    source: BlockId,
    latest: BlockId,
    dominators: &super::analysis::DominatorTree,
    loop_depths: &[usize],
) -> BlockId {
    let mut best = source;
    let mut block = latest;
    loop {
        let block_is_better = loop_depths[block.0] < loop_depths[best.0]
            || (loop_depths[block.0] == loop_depths[best.0]
                && ((function.blocks[block.0].layout == BlockLayout::Cold
                    && function.blocks[best.0].layout != BlockLayout::Cold)
                    || ((function.blocks[block.0].layout == BlockLayout::Cold)
                        == (function.blocks[best.0].layout == BlockLayout::Cold)
                        && dominators.depth(block) > dominators.depth(best))));
        if block_is_better {
            best = block;
        }
        if block == source {
            break;
        }
        let Some(parent) = dominators.immediate_dominator(block) else {
            return source;
        };
        block = parent;
    }
    best
}

#[allow(clippy::too_many_arguments)]
fn schedule_value_dependencies(
    function: &Function,
    block: BlockId,
    values: &[ValueId],
    placements: &[BlockId],
    movable: &[bool],
    ranks: &[usize],
    emitted: &mut HashSet<InstructionId>,
    schedule: &mut Vec<InstructionId>,
) {
    let mut dependencies = values
        .iter()
        .filter_map(|value| match function.values[value.0].definition {
            ValueDefinition::InstructionResult { instruction, .. }
                if placements[instruction.0] == block && movable[instruction.0] =>
            {
                Some(instruction)
            }
            _ => None,
        })
        .collect::<Vec<_>>();
    dependencies.sort_by_key(|instruction| ranks[instruction.0]);
    dependencies.dedup();
    for dependency in dependencies {
        schedule_instruction(
            function,
            block,
            dependency,
            placements,
            movable,
            ranks,
            emitted,
            schedule,
        );
    }
}

#[allow(clippy::too_many_arguments)]
fn schedule_instruction(
    function: &Function,
    block: BlockId,
    instruction: InstructionId,
    placements: &[BlockId],
    movable: &[bool],
    ranks: &[usize],
    emitted: &mut HashSet<InstructionId>,
    schedule: &mut Vec<InstructionId>,
) {
    if emitted.contains(&instruction) {
        return;
    }
    schedule_value_dependencies(
        function,
        block,
        &function.instructions[instruction.0].inputs,
        placements,
        movable,
        ranks,
        emitted,
        schedule,
    );
    if emitted.insert(instruction) {
        schedule.push(instruction);
    }
}

#[cfg(test)]
pub(crate) fn eliminate_dead_code(function: &mut Function) {
    run_single_pass(function, "dead-code-elimination", eliminate_dead_code_pass);
}

fn eliminate_dead_code_pass(function: &mut Function, analyses: &mut AnalysisManager) -> bool {
    let mut changed = false;
    loop {
        let uses = analyses.uses(function);

        let eliminated = function
            .instructions
            .iter()
            .enumerate()
            .filter(|(_, instruction)| {
                instruction.effects.can_be_eliminated()
                    && instruction.results.iter().all(|result| uses.count(*result) == 0)
            })
            .map(|(index, _)| InstructionId(index))
            .collect::<HashSet<_>>();
        if eliminated.is_empty() {
            break;
        }
        changed = true;
        rebuild_instruction_arena(function, &eliminated, InstructionOrder::ByBlock);
        analyses.invalidate();
    }

    changed
}

pub(super) fn rewrite_values(values: &mut [ValueId], replacements: &HashMap<ValueId, ValueId>) {
    for value in values {
        *value = resolve_replacement(*value, replacements);
    }
}

pub(super) fn rewrite_function_uses(function: &mut Function, replacements: &HashMap<ValueId, ValueId>) {
    for instruction in &mut function.instructions {
        rewrite_values(&mut instruction.inputs, replacements);
    }
    for block in &mut function.blocks {
        if let Some(terminator) = &mut block.terminator {
            rewrite_terminator(terminator, replacements);
        }
    }
}

fn resolve_replacement(mut value: ValueId, replacements: &HashMap<ValueId, ValueId>) -> ValueId {
    while let Some(replacement) = replacements.get(&value) {
        value = *replacement;
    }
    value
}

fn rewrite_edge(edge: &mut Edge, replacements: &HashMap<ValueId, ValueId>) {
    rewrite_values(&mut edge.arguments, replacements);
}

pub(super) fn rewrite_terminator(terminator: &mut Terminator, replacements: &HashMap<ValueId, ValueId>) {
    match terminator {
        Terminator::Jump(edge) => rewrite_edge(edge, replacements),
        Terminator::Branch {
            condition,
            then_edge,
            else_edge,
        } => {
            *condition = resolve_replacement(*condition, replacements);
            rewrite_edge(then_edge, replacements);
            rewrite_edge(else_edge, replacements);
        }
        Terminator::Switch { value, cases, default } => {
            *value = resolve_replacement(*value, replacements);
            for (_, edge) in cases {
                rewrite_edge(edge, replacements);
            }
            rewrite_edge(default, replacements);
        }
        Terminator::CheckedOperation {
            inputs,
            success,
            failure,
            ..
        } => {
            rewrite_values(inputs, replacements);
            rewrite_edge(success, replacements);
            rewrite_edge(failure, replacements);
        }
        Terminator::IndirectJump { target } => {
            *target = resolve_replacement(*target, replacements);
        }
        Terminator::Return(values) => rewrite_values(values, replacements),
        Terminator::Unreachable => {}
    }
}

pub(super) enum InstructionOrder {
    ByBlock,
    Original,
}

pub(super) fn rebuild_instruction_arena(function: &mut Function, eliminated: &HashSet<InstructionId>, order: InstructionOrder) {
    let old_instructions = std::mem::take(&mut function.instructions);
    function.instructions.reserve(old_instructions.len() - eliminated.len());
    let mut instruction_map = vec![None; old_instructions.len()];
    let order: Vec<_> = if matches!(order, InstructionOrder::Original) {
        (0..old_instructions.len()).map(InstructionId).collect()
    } else {
        function.blocks.iter().flat_map(|block| &block.instructions).copied().collect()
    };
    for old_id in order {
        if eliminated.contains(&old_id) {
            for result in &old_instructions[old_id.0].results {
                function.values[result.0].definition = ValueDefinition::Dead;
            }
            continue;
        }
        instruction_map[old_id.0] = Some(InstructionId(function.instructions.len()));
        function.instructions.push(old_instructions[old_id.0].clone());
    }
    for block in &mut function.blocks {
        block.instructions.retain_mut(|instruction| {
            let Some(mapped) = instruction_map[instruction.0] else {
                return false;
            };
            *instruction = mapped;
            true
        });
    }
    for (instruction_index, instruction) in function.instructions.iter().enumerate() {
        for (result_index, result) in instruction.results.iter().enumerate() {
            function.values[result.0].definition = ValueDefinition::InstructionResult {
                instruction: InstructionId(instruction_index),
                index: result_index,
            };
        }
    }
    debug_assert!(instruction_map.iter().enumerate().all(|(index, mapped)| eliminated.contains(&InstructionId(index)) || mapped.is_some()));
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frontend::parser;
    use crate::hir as typecheck;
    use crate::intrinsic::{
        AssertionOperation, CallOperation, ControlOperation, OperandLoad, OperandOperation,
        OperandStore,
    };
    use crate::ssa::{Constant, LowLevelOperation, MemoryEffect, lowering as ir_lowering};

    fn append_test_operation(
        function: &mut Function,
        block: BlockId,
        operation: impl Into<Operation>,
        inputs: Vec<ValueId>,
        result_types: Vec<Type>,
        effects: Effects,
    ) -> (InstructionId, Vec<ValueId>) {
        let instruction = InstructionId(function.instructions.len());
        let results = function.append_instruction_with_effects(
            block,
            operation,
            inputs,
            result_types,
            effects,
        );
        (instruction, results)
    }

    #[test]
    fn global_code_motion_sinks_pure_work_to_its_cold_use() {
        let mut function = Function::new("sink", vec![Type::Bool, Type::I32], Vec::new());
        let entry = function.entry;
        let condition = function.parameter(0);
        let value = function.parameter(1);
        let cold = function.create_empty_block("cold", BlockLayout::Cold);
        let hot = function.create_empty_block("hot", BlockLayout::Hot);
        let (computation, result) = append_test_operation(
            &mut function,
            entry,
            Intrinsic::LowLevel(LowLevelOperation::Negate),
            vec![value],
            vec![Type::I32],
            Effects::PURE,
        );
        let (consume, _) = append_test_operation(
            &mut function,
            cold,
            Intrinsic::Assertion(AssertionOperation::NonZero),
            result,
            Vec::new(),
            Effects::UNKNOWN,
        );
        function.set_terminator(
            entry,
            Terminator::branch_edges(condition, Edge::new(cold), Edge::new(hot)),
        );
        function.set_terminator(cold, Terminator::Return(Vec::new()));
        function.set_terminator(hot, Terminator::Return(Vec::new()));

        schedule_global_code_motion(&mut function);

        assert!(function.blocks[entry.0].instructions.is_empty());
        assert_eq!(function.blocks[cold.0].instructions, vec![computation, consume]);
        function.validate().unwrap();
    }

    #[test]
    fn global_code_motion_schedules_pure_work_immediately_before_use() {
        let mut function = Function::new("late", vec![Type::I32], Vec::new());
        let entry = function.entry;
        let value = function.parameter(0);
        let (computation, result) = append_test_operation(
            &mut function,
            entry,
            Intrinsic::LowLevel(LowLevelOperation::Negate),
            vec![value],
            vec![Type::I32],
            Effects::PURE,
        );
        let (unrelated, _) = append_test_operation(
            &mut function,
            entry,
            Intrinsic::Call(CallOperation::SlowPath),
            Vec::new(),
            Vec::new(),
            Effects::UNKNOWN,
        );
        let (consume, _) = append_test_operation(
            &mut function,
            entry,
            Intrinsic::Assertion(AssertionOperation::NonZero),
            result,
            Vec::new(),
            Effects::UNKNOWN,
        );
        function.set_terminator(entry, Terminator::Return(Vec::new()));

        schedule_global_code_motion(&mut function);

        assert_eq!(
            function.blocks[entry.0].instructions,
            vec![unrelated, computation, consume]
        );
        function.validate().unwrap();
    }

    #[test]
    fn global_code_motion_does_not_sink_work_into_a_loop() {
        let mut function = Function::new("loop", vec![Type::Bool, Type::I32], Vec::new());
        let entry = function.entry;
        let condition = function.parameter(0);
        let value = function.parameter(1);
        let header = function.create_empty_block("header", BlockLayout::Hot);
        let body = function.create_empty_block("body", BlockLayout::Hot);
        let exit = function.create_empty_block("exit", BlockLayout::Hot);
        let (computation, result) = append_test_operation(
            &mut function,
            entry,
            Intrinsic::LowLevel(LowLevelOperation::Negate),
            vec![value],
            vec![Type::I32],
            Effects::PURE,
        );
        let (consume, _) = append_test_operation(
            &mut function,
            header,
            Intrinsic::Assertion(AssertionOperation::NonZero),
            result,
            Vec::new(),
            Effects::UNKNOWN,
        );
        function.set_terminator(
            entry,
            Terminator::jump(header),
        );
        function.set_terminator(
            header,
            Terminator::branch_edges(condition, Edge::new(body), Edge::new(exit)),
        );
        function.set_terminator(
            body,
            Terminator::jump(header),
        );
        function.set_terminator(exit, Terminator::Return(Vec::new()));

        schedule_global_code_motion(&mut function);

        assert_eq!(function.blocks[entry.0].instructions, vec![computation]);
        assert_eq!(function.blocks[header.0].instructions, vec![consume]);
        function.validate().unwrap();
    }

    #[test]
    fn eliminates_trivial_loop_parameters() {
        let mut function = Function::new("loop", Vec::new(), Vec::new());
        let constant = function.add_constant(
            Type::SlowPath,
            Constant::SlowPath(ExternalSymbol::new("slow")),
        );
        let header = function.create_named_block("header", BlockLayout::Hot, vec![Type::SlowPath]);
        let parameter = function.blocks[header.0].parameters[0];
        let use_instruction = function.append_instruction(
            header,
            Intrinsic::Call(CallOperation::SlowPath),
            vec![parameter],
            Vec::new(),
        );
        assert!(use_instruction.is_empty());
        function.set_terminator(
            function.entry,
            Terminator::jump_with_arguments(header, vec![constant]),
        );
        function.set_terminator(
            header,
            Terminator::jump_with_arguments(header, vec![parameter]),
        );

        eliminate_trivial_block_parameters(&mut function);

        assert!(function.blocks[header.0].parameters.is_empty());
        assert!(matches!(function.values[parameter.0].definition, ValueDefinition::Dead));
        assert_eq!(function.instructions[0].inputs, vec![constant]);
        for block in &function.blocks {
            for edge in block.terminator.as_ref().unwrap().successors() {
                assert!(edge.arguments.is_empty());
            }
        }
    }

    #[test]
    fn shares_repeated_pure_arithmetic() {
        let ast = parser::parse(
            "test.flap",
            r#"
handler Add(lhs: i32, rhs: i32) {
    let first = lhs + rhs;
    let second = lhs + rhs;
    let total = first + second;
    assert_nonzero(total);
    dispatch_next;
}
"#,
        )
        .unwrap();
        let typed = typecheck::check("test.flap", &ast).unwrap();
        let mut function = ir_lowering::lower_handler(&typed.handlers[0]).unwrap();

        eliminate_common_subexpressions(&mut function);

        let entry = &function.blocks[function.entry.0];
        assert_eq!(entry.instructions.len(), 4);
        let first_add = &function.instructions[entry.instructions[0].0];
        let total = &function.instructions[entry.instructions[1].0];
        assert_eq!(total.inputs, vec![first_add.results[0], first_add.results[0]]);
    }

    #[test]
    fn shares_expressions_with_repeated_literal_inputs() {
        let ast = parser::parse(
            "test.flap",
            r#"
handler Add(lhs: i32) {
    let first = lhs + 1;
    let second = lhs + 1;
    let total = first + second;
    assert_nonzero(total);
    dispatch_next;
}
"#,
        )
        .unwrap();
        let typed = typecheck::check("test.flap", &ast).unwrap();
        let mut function = ir_lowering::lower_handler(&typed.handlers[0]).unwrap();

        eliminate_common_subexpressions(&mut function);

        assert_eq!(function.blocks[function.entry.0].instructions.len(), 4);
    }

    #[test]
    fn does_not_merge_effectful_operations() {
        let mut function = Function::new("loads", vec![Type::U64], Vec::new());
        for _ in 0..2 {
            function.append_instruction_with_effects(
                function.entry,
                Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field)),
                vec![function.parameter(0)],
                vec![Type::U64],
                Effects::UNKNOWN,
            );
        }
        function.set_terminator(function.entry, Terminator::Unreachable);

        eliminate_common_subexpressions(&mut function);

        assert_eq!(function.blocks[function.entry.0].instructions.len(), 2);
    }

    #[test]
    fn shares_reads_with_the_same_effect_dependency() {
        let mut function = Function::new("loads", vec![Type::U64], vec![Type::U64]);
        function.append_instruction(
            function.entry,
            Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field)),
            vec![function.parameter(0)],
            vec![Type::U64],
        );
        let second = function.append_instruction(
            function.entry,
            Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field)),
            vec![function.parameter(0)],
            vec![Type::U64],
        )[0];
        function.set_terminator(function.entry, Terminator::Return(vec![second]));

        eliminate_common_subexpressions(&mut function);

        assert_eq!(function.instructions.len(), 1);
    }

    #[test]
    fn does_not_share_reads_across_a_write() {
        let mut function = Function::new("loads", vec![Type::U64], vec![Type::U64]);
        let read_effects = Effects {
            memory: MemoryEffect::Read,
            ..Effects::PURE
        };
        function.append_instruction_with_effects(
            function.entry,
            Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field)),
            vec![function.parameter(0)],
            vec![Type::U64],
            read_effects,
        );
        function.append_instruction_with_effects(
            function.entry,
            Intrinsic::Operand(OperandOperation::Store(OperandStore::Field)),
            vec![function.parameter(0)],
            Vec::new(),
            Effects {
                memory: MemoryEffect::Write,
                ..Effects::PURE
            },
        );
        let second = function.append_instruction_with_effects(
            function.entry,
            Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field)),
            vec![function.parameter(0)],
            vec![Type::U64],
            read_effects,
        )[0];
        function.set_terminator(function.entry, Terminator::Return(vec![second]));

        eliminate_common_subexpressions(&mut function);

        assert_eq!(function.instructions.len(), 3);
    }

    #[test]
    fn keeps_cold_reloads_for_frequency_unaware_allocation() {
        let mut function = Function::new("reload", vec![Type::U64], vec![Type::U64]);
        let effects = Effects {
            memory: MemoryEffect::Read,
            ..Effects::PURE
        };
        function.append_instruction_with_effects(
            function.entry,
            Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field)),
            vec![function.parameter(0)],
            vec![Type::U64],
            effects,
        );
        let cold = function.create_empty_block("cold", BlockLayout::Cold);
        function.set_terminator(function.entry, Terminator::jump(cold));
        let reloaded = function.append_instruction_with_effects(
            cold,
            Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field)),
            vec![function.parameter(0)],
            vec![Type::U64],
            effects,
        )[0];
        function.set_terminator(cold, Terminator::Return(vec![reloaded]));

        eliminate_common_subexpressions(&mut function);

        assert_eq!(function.instructions.len(), 2);
    }

    #[test]
    fn does_not_share_rematerializable_moves() {
        let mut function = Function::new("moves", vec![Type::U64], Vec::new());
        for _ in 0..2 {
            function.append_instruction(
                function.entry,
                Intrinsic::LowLevel(LowLevelOperation::Move),
                vec![function.parameter(0)],
                vec![Type::U64],
            );
        }
        function.set_terminator(function.entry, Terminator::Unreachable);

        eliminate_common_subexpressions(&mut function);

        assert_eq!(function.blocks[function.entry.0].instructions.len(), 2);
    }

    #[test]
    fn shares_pure_expressions_across_dominated_blocks() {
        let mut function = Function::new("dominated", vec![Type::Bool, Type::I32, Type::I32], Vec::new());
        function.append_instruction(
            function.entry,
            Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(BinaryOperation::Add)),
            vec![function.parameter(1), function.parameter(2)],
            vec![Type::I32],
        );
        let child = function.create_empty_block("child", BlockLayout::Hot);
        let exit = function.create_empty_block("exit", BlockLayout::Hot);
        function.set_terminator(
            function.entry,
            Terminator::branch(function.parameter(0), child, exit),
        );
        function.append_instruction(
            child,
            Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(BinaryOperation::Add)),
            vec![function.parameter(1), function.parameter(2)],
            vec![Type::I32],
        );
        function.set_terminator(child, Terminator::Unreachable);
        function.set_terminator(exit, Terminator::Unreachable);

        eliminate_common_subexpressions(&mut function);

        assert_eq!(function.instructions.len(), 1);
    }

    #[test]
    fn does_not_share_expressions_between_sibling_blocks() {
        let mut function = Function::new("siblings", vec![Type::Bool, Type::I32, Type::I32], Vec::new());
        let then_block = function.create_empty_block("then", BlockLayout::Hot);
        let else_block = function.create_empty_block("else", BlockLayout::Hot);
        function.set_terminator(
            function.entry,
            Terminator::branch(function.parameter(0), then_block, else_block),
        );
        for block in [then_block, else_block] {
            function.append_instruction(
                block,
                Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(BinaryOperation::Add)),
                vec![function.parameter(1), function.parameter(2)],
                vec![Type::I32],
            );
            function.set_terminator(block, Terminator::Unreachable);
        }

        eliminate_common_subexpressions(&mut function);

        assert_eq!(function.instructions.len(), 2);
    }

    #[test]
    fn removes_cascading_dead_pure_instructions() {
        let mut function = Function::new("dead", vec![Type::I32], Vec::new());
        let first = function.append_instruction(
            function.entry,
            Intrinsic::Value(ValueOperation::Reuse),
            vec![function.parameter(0)],
            vec![Type::I32],
        );
        function.append_instruction(
            function.entry,
            Intrinsic::Value(ValueOperation::Reuse),
            first,
            vec![Type::I32],
        );
        function.append_instruction(
            function.entry,
            Intrinsic::Call(CallOperation::SlowPath),
            Vec::new(),
            Vec::new(),
        );
        function.set_terminator(function.entry, Terminator::Unreachable);

        eliminate_dead_code(&mut function);

        assert_eq!(function.instructions.len(), 1);
        assert_eq!(
            function.instructions[0].operation,
            Operation::Intrinsic(Intrinsic::Call(CallOperation::SlowPath))
        );
    }

    #[test]
    fn retains_pure_values_used_by_terminators() {
        let mut function = Function::new("result", vec![Type::I32], vec![Type::I32]);
        let result = function.append_instruction(
            function.entry,
            Intrinsic::Value(ValueOperation::Reuse),
            vec![function.parameter(0)],
            vec![Type::I32],
        )[0];
        function.set_terminator(function.entry, Terminator::Return(vec![result]));

        eliminate_dead_code(&mut function);

        assert_eq!(function.instructions.len(), 1);
    }

    #[test]
    fn removes_unused_nontrapping_reads() {
        let mut function = Function::new("dead-load", vec![Type::U64], Vec::new());
        function.append_instruction(
            function.entry,
            Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field)),
            vec![function.parameter(0)],
            vec![Type::U64],
        );
        function.set_terminator(function.entry, Terminator::Unreachable);

        eliminate_dead_code(&mut function);

        assert!(function.instructions.is_empty());
    }

    #[test]
    fn folds_constant_branches_and_removes_unreachable_blocks() {
        let mut function = Function::new("branch", Vec::new(), Vec::new());
        let then_block = function.create_empty_block("then", BlockLayout::Hot);
        let else_block = function.create_empty_block("else", BlockLayout::Cold);
        let condition = function.add_constant(Type::Bool, Constant::Integer(1));
        function.set_terminator(
            function.entry,
            Terminator::branch_edges(condition, Edge::new(then_block), Edge::new(else_block)),
        );
        function.append_instruction(
            then_block,
            Intrinsic::Control(ControlOperation::DispatchNext),
            Vec::new(),
            Vec::new(),
        );
        function.append_instruction(
            else_block,
            Intrinsic::Control(ControlOperation::Exit),
            Vec::new(),
            Vec::new(),
        );
        function.set_terminator(then_block, Terminator::Unreachable);
        function.set_terminator(else_block, Terminator::Unreachable);

        super::super::sccp::run(&mut function, &mut AnalysisManager::default());

        assert_eq!(function.blocks.len(), 2);
        assert_eq!(function.instructions.len(), 1);
        assert_eq!(
            function.instructions[0].operation,
            Operation::Intrinsic(Intrinsic::Control(ControlOperation::DispatchNext))
        );
        let Terminator::Jump(edge) = function.blocks[function.entry.0].terminator.as_ref().unwrap() else {
            panic!("expected a folded jump")
        };
        assert_eq!(edge.block, BlockId(1));
    }

    #[test]
    fn folds_constant_switches() {
        let mut function = Function::new("switch", Vec::new(), Vec::new());
        let selected = function.create_empty_block("selected", BlockLayout::Hot);
        let default = function.create_empty_block("default", BlockLayout::Hot);
        let value = function.add_constant(Type::U32, Constant::Integer(7));
        let case = function.add_constant(Type::U32, Constant::Integer(7));
        function.set_terminator(
            function.entry,
            Terminator::switch(
                value,
                vec![(case, Edge::new(selected))],
                Edge::new(default),
            ),
        );
        function.set_terminator(selected, Terminator::Unreachable);
        function.set_terminator(default, Terminator::Unreachable);

        super::super::sccp::run(&mut function, &mut AnalysisManager::default());

        assert_eq!(function.blocks.len(), 2);
        let Terminator::Jump(edge) = function.blocks[function.entry.0].terminator.as_ref().unwrap() else {
            panic!("expected a folded jump")
        };
        assert_eq!(edge.block, BlockId(1));
    }

    #[test]
    fn resolves_layout_constants_outside_switches() {
        let mut function = Function::new("constants", Vec::new(), Vec::new());
        let base = function.add_constant(Type::U64, Constant::Integer(0));
        let constant = function.add_constant(Type::U64, Constant::Symbol("FIELD_OFFSET".to_string()));
        function.append_instruction(
            function.entry,
            Operation::Address,
            vec![base, constant],
            vec![Type::Memory],
        );
        function.set_terminator(function.entry, Terminator::Return(Vec::new()));

        resolve_constants(
            &mut function,
            &LayoutConstants::from_values([(
                "FIELD_OFFSET".to_string(),
                24,
            )]),
        );

        assert_eq!(
            function.values[constant.0].definition,
            ValueDefinition::Constant(Constant::Integer(24))
        );
    }

    #[test]
    fn preserves_layout_value_resolution_points() {
        let constants = LayoutConstants::from_values([(
            "FIELD_OFFSET".to_string(),
            24,
        )]);
        let mut function = Function::new("constants", Vec::new(), Vec::new());
        let base = function.add_constant(Type::U64, Constant::Integer(0));
        let named = function.add_constant(
            Type::U64,
            Constant::LayoutValue(LayoutValue::Constant(
                constants.get("FIELD_OFFSET").unwrap(),
            )),
        );
        let immediate = function.add_constant(
            Type::U64,
            Constant::LayoutValue(LayoutValue::Immediate(8)),
        );
        function.append_instruction(
            function.entry,
            Operation::Address,
            vec![base, named, immediate],
            vec![Type::Memory],
        );
        function.set_terminator(function.entry, Terminator::Return(Vec::new()));

        resolve_constants(&mut function, &constants);

        assert_eq!(
            function.values[named.0].definition,
            ValueDefinition::Constant(Constant::Integer(24))
        );
        assert_eq!(
            function.values[immediate.0].definition,
            ValueDefinition::Constant(Constant::LayoutValue(
                LayoutValue::Immediate(8)
            ))
        );

        resolve_layout_constants(&mut function, &constants).unwrap();

        assert_eq!(
            function.values[immediate.0].definition,
            ValueDefinition::Constant(Constant::Integer(8))
        );
    }

    #[test]
    fn rejects_unknown_layout_constants_during_resolution() {
        let mut function = Function::new("Missing", Vec::new(), Vec::new());
        function.add_constant(
            Type::U64,
            Constant::Symbol("MISSING".to_string()),
        );

        let error = resolve_layout_constants(
            &mut function,
            &LayoutConstants::default(),
        )
        .unwrap_err();

        assert_eq!(error.stage, CompileStage::Ssa);
        assert_eq!(error.handler.as_deref(), Some("Missing"));
        assert_eq!(error.message, "unknown constant 'MISSING'");
    }
}
