/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SSA inline expansion.

use super::optimize::{InstructionOrder, rebuild_instruction_arena, rewrite_function_uses};
#[cfg(test)]
use super::{BinaryOperation, IntegerBinaryOperation};
use super::{
    BlockId, Edge, Function, Intrinsic, MemoryEffect, Operation, Terminator, ValueDefinition, ValueId, ValueOperation,
};
use crate::hash::{HashMap, HashSet};
use crate::types::Type;
use crate::{CompileError, CompileStage};
use std::cmp::Reverse;
use std::collections::BinaryHeap;

const MAX_INLINED_INSTRUCTION_COUNT: usize = 1_000_000;

fn inline_error(function: &Function, message: impl Into<String>) -> CompileError {
    CompileError::new(CompileStage::Ssa, Some(&function.name), message)
}

pub(crate) fn inline_calls(function: &mut Function, callees: &[Function]) -> Result<usize, CompileError> {
    preflight_expansion(function, callees, MAX_INLINED_INSTRUCTION_COUNT)?;
    inline_calls_with_budget(function, callees, MAX_INLINED_INSTRUCTION_COUNT)
}

fn resolved_inline_callee(
    operation: &Operation,
    bindings: &[Option<super::OperationValue>],
) -> Option<crate::identity::InlineFunctionId> {
    match operation {
        Operation::InlineCall(id) => Some(*id),
        Operation::Parameter(parameter) => {
            let Some(Some(super::OperationValue::InlineFunction(id))) = bindings.get(parameter.index()) else {
                return None;
            };
            Some(*id)
        }
        _ => None,
    }
}

fn operation_binding(
    function: &Function,
    value: ValueId,
    bindings: &[Option<super::OperationValue>],
) -> Option<super::OperationValue> {
    match &function.values[value.0].definition {
        ValueDefinition::Constant(super::ir::Constant::Operation(operation)) => Some(operation.clone()),
        ValueDefinition::FunctionParameter(parameter) => bindings.get(*parameter).cloned().flatten(),
        _ => None,
    }
}

fn callee_bindings(
    function: &Function,
    inputs: &[ValueId],
    callee: &Function,
    bindings: &[Option<super::OperationValue>],
) -> Vec<Option<super::OperationValue>> {
    (0..callee.parameter_types.len())
        .map(|parameter| {
            inputs
                .get(parameter)
                .and_then(|value| operation_binding(function, *value, bindings))
        })
        .collect()
}

fn expanded_instruction_count(
    function: &Function,
    callees: &[Function],
    bindings: &[Option<super::OperationValue>],
    active: &mut Vec<crate::identity::InlineFunctionId>,
    memoized_counts: &mut HashMap<(usize, Vec<Option<super::OperationValue>>), usize>,
    maximum_instruction_count: usize,
) -> Result<usize, CompileError> {
    let mut count = function.instructions.len();
    for instruction in &function.instructions {
        let Some(id) = resolved_inline_callee(&instruction.operation, bindings) else {
            continue;
        };
        if active.contains(&id) {
            return Err(inline_error(
                function,
                format!("recursive inline expansion reached inline function #{}", id.index()),
            ));
        }
        let callee = callees
            .get(id.index())
            .ok_or_else(|| inline_error(function, format!("inline function #{} has no SSA body", id.index())))?;
        let callee_bindings = callee_bindings(function, &instruction.inputs, callee, bindings);
        let key = (id.index(), callee_bindings.clone());
        let callee_count = if let Some(count) = memoized_counts.get(&key) {
            *count
        } else {
            active.push(id);
            let count = expanded_instruction_count(
                callee,
                callees,
                &callee_bindings,
                active,
                memoized_counts,
                maximum_instruction_count,
            )?;
            active.pop();
            memoized_counts.insert(key, count);
            count
        };
        count = count
            .checked_sub(1)
            .and_then(|count| count.checked_add(callee_count))
            .ok_or_else(|| inline_error(function, "inline expansion instruction count overflow"))?;
        if count > maximum_instruction_count {
            return Err(inline_error(
                function,
                format!("inline expansion exceeds the {maximum_instruction_count}-instruction limit"),
            ));
        }
    }
    for block in &function.blocks {
        let Some(Terminator::CheckedOperation { operation, inputs, .. }) = &block.terminator else {
            continue;
        };
        let Some(id) = resolved_inline_callee(operation, bindings) else {
            continue;
        };
        if active.contains(&id) {
            return Err(inline_error(
                function,
                format!("recursive inline expansion reached inline function #{}", id.index()),
            ));
        }
        let callee = callees
            .get(id.index())
            .ok_or_else(|| inline_error(function, format!("inline function #{} has no SSA body", id.index())))?;
        let callee_bindings = callee_bindings(function, inputs, callee, bindings);
        let key = (id.index(), callee_bindings.clone());
        let callee_count = if let Some(count) = memoized_counts.get(&key) {
            *count
        } else {
            active.push(id);
            let count = expanded_instruction_count(
                callee,
                callees,
                &callee_bindings,
                active,
                memoized_counts,
                maximum_instruction_count,
            )?;
            active.pop();
            memoized_counts.insert(key, count);
            count
        };
        count = count
            .checked_add(1)
            .and_then(|count| count.checked_add(callee_count))
            .ok_or_else(|| inline_error(function, "inline expansion instruction count overflow"))?;
        if count > maximum_instruction_count {
            return Err(inline_error(
                function,
                format!("inline expansion exceeds the {maximum_instruction_count}-instruction limit"),
            ));
        }
    }
    Ok(count)
}

fn preflight_expansion(
    function: &Function,
    callees: &[Function],
    maximum_instruction_count: usize,
) -> Result<(), CompileError> {
    let bindings = vec![None; function.parameter_types.len()];
    expanded_instruction_count(
        function,
        callees,
        &bindings,
        &mut Vec::new(),
        &mut HashMap::default(),
        maximum_instruction_count,
    )?;
    Ok(())
}

fn inline_calls_with_budget(
    function: &mut Function,
    callees: &[Function],
    maximum_instruction_count: usize,
) -> Result<usize, CompileError> {
    let mut count = 0;
    let mut eliminated = HashSet::default();
    let mut worklist = (0..function.blocks.len())
        .map(|index| Reverse(BlockId(index)))
        .collect::<BinaryHeap<_>>();
    while let Some(Reverse(block)) = worklist.pop() {
        if lower_checked_inline_call(function, block, callees, maximum_instruction_count, eliminated.len())? {
            worklist.push(Reverse(block));
            continue;
        }
        let Some((instruction_index, callee)) = find_call(function, block, callees)? else {
            continue;
        };
        ensure_expansion_budget(
            function,
            callee.instructions.len(),
            maximum_instruction_count,
            eliminated.len(),
        )?;
        let first_new_block = function.blocks.len();
        let call = inline_call(function, block.0, instruction_index, callee)?;
        eliminated.insert(call);
        worklist.extend((first_new_block..function.blocks.len()).map(|index| Reverse(BlockId(index))));
        worklist.push(Reverse(block));
        count += 1;
    }
    if !eliminated.is_empty() {
        rebuild_instruction_arena(function, &eliminated, InstructionOrder::ByBlock);
    }
    function.recompute_machine_state_dependencies();
    function.validate().map_err(|message| inline_error(function, message))?;
    Ok(count)
}

fn ensure_expansion_budget(
    function: &Function,
    additional_instruction_count: usize,
    maximum_instruction_count: usize,
    eliminated_instruction_count: usize,
) -> Result<(), CompileError> {
    let live_instruction_count = function.instructions.len() - eliminated_instruction_count;
    if live_instruction_count.saturating_add(additional_instruction_count) > maximum_instruction_count {
        return Err(inline_error(
            function,
            format!("inline expansion exceeds the {maximum_instruction_count}-instruction limit"),
        ));
    }
    Ok(())
}

fn lower_checked_inline_call(
    function: &mut Function,
    block: BlockId,
    callees: &[Function],
    maximum_instruction_count: usize,
    eliminated_instruction_count: usize,
) -> Result<bool, CompileError> {
    let Some(Terminator::CheckedOperation {
        operation: Operation::InlineCall(id),
        inputs,
        results,
        effects,
        success,
        failure,
    }) = function.blocks[block.0].terminator.clone()
    else {
        return Ok(false);
    };
    let callee = callees
        .get(id.index())
        .ok_or_else(|| inline_error(function, format!("inline function #{} has no SSA body", id.index())))?;
    if callee.parameter_types.len() != inputs.len() + 1
        || callee.parameter_types.last() != Some(&Type::label())
        || callee.return_types.len() != results.len()
    {
        return Err(inline_error(
            function,
            format!(
                "checked call to inline function '{}' does not match its {} parameters and {} results",
                callee.name,
                callee.parameter_types.len(),
                callee.return_types.len()
            ),
        ));
    }
    ensure_expansion_budget(function, 2, maximum_instruction_count, eliminated_instruction_count)?;

    let failure_reference = function.append_instruction(
        block,
        Operation::BlockReference(failure.block),
        failure.arguments,
        vec![Type::label()],
    )[0];
    let call_results = function.append_instruction_with_effects(
        block,
        Operation::InlineCall(id),
        inputs.into_iter().chain([failure_reference]).collect(),
        results
            .iter()
            .map(|result| function.values[result.0].ty.clone())
            .collect(),
        effects,
    );
    let replacements = results.iter().copied().zip(call_results.iter().copied()).collect();
    rewrite_function_uses(function, &replacements);
    for old in &results {
        function.values[old.0].definition = ValueDefinition::Dead;
    }
    function.blocks[block.0].terminator = Some(Terminator::jump_with_arguments(
        success.block,
        call_results
            .into_iter()
            .chain(success.arguments.into_iter().skip(results.len()))
            .collect(),
    ));
    Ok(true)
}

fn find_call<'a>(
    function: &Function,
    block: BlockId,
    callees: &'a [Function],
) -> Result<Option<(usize, &'a Function)>, CompileError> {
    for (instruction_index, instruction_id) in function.blocks[block.0].instructions.iter().enumerate() {
        let Operation::InlineCall(id) = &function.instructions[instruction_id.0].operation else {
            continue;
        };
        let callee = callees
            .get(id.index())
            .ok_or_else(|| inline_error(function, format!("inline function #{} has no SSA body", id.index())))?;
        let call = &function.instructions[instruction_id.0];
        if call.inputs.len() != callee.parameter_types.len() || call.results.len() != callee.return_types.len() {
            continue;
        }
        return Ok(Some((instruction_index, callee)));
    }
    Ok(None)
}

fn inline_call(
    function: &mut Function,
    block_index: usize,
    instruction_index: usize,
    callee: &Function,
) -> Result<super::InstructionId, CompileError> {
    let call_id = function.blocks[block_index].instructions[instruction_index];
    let call = function.instructions[call_id.0].clone();
    if call.inputs.len() != callee.parameter_types.len() || call.results.len() != callee.return_types.len() {
        return Err(inline_error(
            function,
            format!("call to '{}' has an incompatible SSA signature", callee.name),
        ));
    }

    let caller_block = BlockId(block_index);
    let caller_layout = function.blocks[caller_block.0].layout;
    let continuation_types = call
        .results
        .iter()
        .map(|result| function.values[result.0].ty.clone())
        .collect();
    let continuation = function.create_named_block(
        format!("inline_{}_continuation", callee.name),
        function.blocks[block_index].layout,
        continuation_types,
    );
    let tail = function.blocks[block_index]
        .instructions
        .split_off(instruction_index + 1);
    function.blocks[block_index].instructions.pop();
    function.blocks[continuation.0].instructions = tail;
    function.blocks[continuation.0].terminator = function.blocks[block_index].terminator.take();
    if let Some(Terminator::CheckedOperation { results, .. }) = &function.blocks[continuation.0].terminator {
        for (index, result) in results.iter().enumerate() {
            function.values[result.0].definition = ValueDefinition::TerminatorResult {
                block: continuation,
                index,
            };
        }
    }
    let replacements = call
        .results
        .iter()
        .copied()
        .zip(function.blocks[continuation.0].parameters.iter().copied())
        .collect();
    rewrite_function_uses(function, &replacements);
    for old in &call.results {
        function.values[old.0].definition = ValueDefinition::Dead;
    }
    let mut blocks = HashMap::from_iter([(callee.entry, caller_block)]);
    for (callee_block_index, callee_block) in callee.blocks.iter().enumerate() {
        let callee_block_id = BlockId(callee_block_index);
        if callee_block_id == callee.entry {
            if !callee_block.parameters.is_empty() {
                return Err(inline_error(
                    function,
                    format!("callee '{}' has entry block parameters", callee.name),
                ));
            }
            continue;
        }
        let parameter_types = callee_block
            .parameters
            .iter()
            .map(|parameter| callee.values[parameter.0].ty.clone())
            .collect();
        let cloned = function.create_block(
            callee_block
                .name
                .as_ref()
                .map(|name| format!("inline_{}_{name}", callee.name)),
            if caller_layout == super::ir::BlockLayout::Cold {
                super::ir::BlockLayout::Cold
            } else {
                callee_block.layout
            },
            parameter_types,
        );
        blocks.insert(callee_block_id, cloned);
    }

    let mut values = HashMap::default();
    for (index, input) in call.inputs.iter().enumerate() {
        let input = coerce_value(function, caller_block, *input, &callee.parameter_types[index])?;
        values.insert(callee.parameter(index), input);
    }
    for (index, value) in callee.values.iter().enumerate() {
        if let ValueDefinition::Constant(constant) = &value.definition {
            values.insert(
                ValueId(index),
                function.add_constant(value.ty.clone(), constant.clone()),
            );
        }
    }
    for (callee_block_index, callee_block) in callee.blocks.iter().enumerate() {
        if BlockId(callee_block_index) == callee.entry {
            continue;
        }
        let cloned_block = blocks[&BlockId(callee_block_index)];
        for (old, new) in callee_block
            .parameters
            .iter()
            .zip(function.blocks[cloned_block.0].parameters.clone())
        {
            values.insert(*old, new);
        }
    }

    let mut instruction_blocks = vec![None; callee.instructions.len()];
    for (callee_block_index, block) in callee.blocks.iter().enumerate() {
        for instruction in &block.instructions {
            instruction_blocks[instruction.0] = Some(BlockId(callee_block_index));
        }
    }
    for (instruction_index, instruction) in callee.instructions.iter().enumerate() {
        let callee_block = instruction_blocks[instruction_index]
            .ok_or_else(|| inline_error(function, format!("callee '{}' has an orphan instruction", callee.name)))?;
        let inputs = instruction
            .inputs
            .iter()
            .map(|value| mapped(function, &values, *value, &callee.name))
            .collect::<Result<Vec<_>, _>>()?;
        let result_types = instruction
            .results
            .iter()
            .map(|result| callee.values[result.0].ty.clone())
            .collect();
        let operation = cloned_operation(function, callee, &instruction.operation, &blocks, &values)?;
        let results = function.append_instruction_with_effects(
            blocks[&callee_block],
            operation,
            inputs,
            result_types,
            instruction.base_effects,
        );
        for (old, new) in instruction.results.iter().zip(results) {
            values.insert(*old, new);
        }
    }

    for (callee_block_index, block) in callee.blocks.iter().enumerate() {
        let cloned_block = blocks[&BlockId(callee_block_index)];
        let terminator = block
            .terminator
            .as_ref()
            .ok_or_else(|| inline_error(function, format!("callee '{}' has no terminator", callee.name)))?;
        clone_terminator(
            function,
            callee,
            terminator,
            cloned_block,
            continuation,
            &blocks,
            &mut values,
        )?;
    }
    Ok(call_id)
}

fn cloned_operation(
    function: &Function,
    callee: &Function,
    operation: &Operation,
    blocks: &HashMap<BlockId, BlockId>,
    values: &HashMap<ValueId, ValueId>,
) -> Result<Operation, CompileError> {
    match operation {
        Operation::BlockReference(target) => Ok(Operation::BlockReference(blocks[target])),
        Operation::Guard { failure } => Ok(Operation::Guard {
            failure: blocks[failure],
        }),
        Operation::Parameter(parameter) => {
            let parameter_index = parameter.index();
            let value = mapped(function, values, callee.parameter(parameter_index), &callee.name)?;
            let ValueDefinition::Constant(super::ir::Constant::Operation(operation)) =
                &function.values[value.0].definition
            else {
                return Ok(operation.clone());
            };
            Ok(match operation {
                super::OperationValue::Intrinsic(intrinsic) => Operation::from(*intrinsic),
                super::OperationValue::InlineFunction(id) => Operation::InlineCall(*id),
            })
        }
        _ => Ok(operation.clone()),
    }
}

fn clone_terminator(
    function: &mut Function,
    callee: &Function,
    terminator: &Terminator,
    block: BlockId,
    continuation: BlockId,
    blocks: &HashMap<BlockId, BlockId>,
    values: &mut HashMap<ValueId, ValueId>,
) -> Result<(), CompileError> {
    let edge = |edge: &Edge| -> Result<Edge, CompileError> {
        Ok(Edge::with_arguments(
            blocks[&edge.block],
            edge.arguments
                .iter()
                .map(|value| mapped(function, values, *value, &callee.name))
                .collect::<Result<_, _>>()?,
        ))
    };
    let cloned = match terminator {
        Terminator::Jump(target) => Terminator::Jump(edge(target)?),
        Terminator::Branch {
            condition,
            then_edge,
            else_edge,
        } => Terminator::branch_edges(
            mapped(function, values, *condition, &callee.name)?,
            edge(then_edge)?,
            edge(else_edge)?,
        ),
        Terminator::Switch { value, cases, default } => Terminator::switch(
            mapped(function, values, *value, &callee.name)?,
            cases
                .iter()
                .map(|(pattern, target)| Ok((mapped(function, values, *pattern, &callee.name)?, edge(target)?)))
                .collect::<Result<_, CompileError>>()?,
            edge(default)?,
        ),
        Terminator::CheckedOperation {
            operation,
            inputs,
            results,
            effects,
            success,
            failure,
        } => {
            let inputs = inputs
                .iter()
                .map(|value| mapped(function, values, *value, &callee.name))
                .collect::<Result<_, _>>()?;
            let failure = edge(failure)?;
            let cloned_operation = cloned_operation(function, callee, operation, blocks, values)?;
            let cloned_effects = if &cloned_operation == operation {
                *effects
            } else if let Operation::Intrinsic(intrinsic) = &cloned_operation {
                let mut cloned_effects = intrinsic.effects();
                if effects.machine_state != MemoryEffect::None {
                    cloned_effects.machine_state = MemoryEffect::Read;
                }
                cloned_effects
            } else {
                *effects
            };
            let result_types = results
                .iter()
                .map(|result| callee.values[result.0].ty.clone())
                .collect();
            let success_block = blocks[&success.block];
            let extra_success_arguments = success.arguments[results.len()..]
                .iter()
                .map(|value| mapped(function, values, *value, &callee.name))
                .collect::<Result<_, _>>()?;
            let cloned_results = function.set_checked_operation(
                block,
                cloned_operation,
                inputs,
                result_types,
                cloned_effects,
                success_block,
                extra_success_arguments,
                failure,
            );
            for (old, new) in results.iter().zip(cloned_results) {
                values.insert(*old, new);
            }
            return Ok(());
        }
        Terminator::IndirectJump { target } => {
            let target = mapped(function, values, *target, &callee.name)?;
            if let Some(edge) = block_reference(function, target) {
                Terminator::Jump(edge)
            } else {
                Terminator::IndirectJump { target }
            }
        }
        Terminator::Return(returned) => {
            let mut arguments = Vec::new();
            for (returned, parameter) in returned.iter().zip(function.blocks[continuation.0].parameters.clone()) {
                let returned = mapped(function, values, *returned, &callee.name)?;
                let parameter_type = function.values[parameter.0].ty.clone();
                arguments.push(coerce_value(function, block, returned, &parameter_type)?);
            }
            Terminator::jump_with_arguments(continuation, arguments)
        }
        Terminator::Unreachable => Terminator::Unreachable,
    };
    function.set_terminator(block, cloned);
    Ok(())
}

fn block_reference(function: &Function, value: ValueId) -> Option<Edge> {
    let ValueDefinition::InstructionResult { instruction, .. } = &function.values[value.0].definition else {
        return None;
    };
    let instruction = &function.instructions[instruction.0];
    let Operation::BlockReference(block) = &instruction.operation else {
        return None;
    };
    Some(Edge::with_arguments(*block, instruction.inputs.to_vec()))
}

fn coerce_value(
    function: &mut Function,
    block: BlockId,
    value: ValueId,
    target: &Type,
) -> Result<ValueId, CompileError> {
    let source = &function.values[value.0].ty;
    if source == target {
        return Ok(value);
    }
    if !can_bridge_representation(source, target) {
        return Err(inline_error(
            function,
            format!("cannot inline a value of type {source} as {target}"),
        ));
    }
    Ok(function.append_instruction(
        block,
        Intrinsic::Value(ValueOperation::Reuse),
        vec![value],
        vec![target.clone()],
    )[0])
}

fn can_bridge_representation(source: &Type, destination: &Type) -> bool {
    matches!(
        (source, destination),
        (Type::Boxed(_), Type::Value)
            | (Type::U8 | Type::U16 | Type::U32, Type::U64)
            | (Type::U8, Type::U16)
            | (Type::U8 | Type::U16, Type::U32)
            | (Type::I8 | Type::I16 | Type::U8 | Type::U16, Type::I32)
    )
}

fn mapped(
    function: &Function,
    values: &HashMap<ValueId, ValueId>,
    value: ValueId,
    callee: &str,
) -> Result<ValueId, CompileError> {
    values
        .get(&value)
        .copied()
        .ok_or_else(|| inline_error(function, format!("callee '{callee}' uses unmapped value {value:?}")))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frontend::parser;
    use crate::hir as typecheck;
    use crate::identity::InlineFunctionId;
    use crate::ssa::{lowering as ir_lowering, optimize};

    fn lower(source: &str) -> (Function, Vec<Function>) {
        let ast = parser::parse("test.flap", source).unwrap();
        let typed = typecheck::check("test.flap", &ast).unwrap();
        let handler = ir_lowering::lower_handler(&typed.handlers[0]).unwrap();
        let callees = typed
            .inline_functions
            .iter()
            .map(|callee| ir_lowering::lower_inline_function(callee).unwrap())
            .collect();
        (handler, callees)
    }

    #[test]
    fn rejects_inline_function_ids_without_a_body() {
        let mut function = Function::new("invalid", Vec::new(), Vec::new());
        function.append_instruction(
            function.entry,
            Operation::InlineCall(InlineFunctionId::new(0)),
            Vec::new(),
            Vec::new(),
        );
        function.set_terminator(function.entry, Terminator::Return(Vec::new()));

        let error = inline_calls(&mut function, &[]).unwrap_err();
        assert_eq!(error.stage, CompileStage::Ssa);
        assert_eq!(error.handler.as_deref(), Some("invalid"));
        assert_eq!(error.message, "inline function #0 has no SSA body");
    }

    #[test]
    fn rejects_inline_expansion_beyond_the_instruction_budget() {
        let (mut handler, callees) = lower(
            r#"
inline fn identity(value: i32) -> i32 {
    value
}

handler Identity(value: i32) {
    let result = identity(value);
    assert_nonzero(result);
    dispatch_next;
}
"#,
        );
        let maximum_instruction_count = handler.instructions.len();
        let error = inline_calls_with_budget(&mut handler, &callees, maximum_instruction_count).unwrap_err();
        assert_eq!(
            error.message,
            format!("inline expansion exceeds the {maximum_instruction_count}-instruction limit")
        );
    }

    #[test]
    fn inlines_straight_line_values_into_ssa_users() {
        let (mut handler, callees) = lower(
            r#"
inline fn double(value: i32) -> i32 {
    value + value
}

handler Double(value: i32) {
    let doubled = double(value);
    assert_nonzero(doubled);
    dispatch_next;
}
"#,
        );
        assert!(
            handler
                .instructions
                .iter()
                .any(|instruction| { matches!(instruction.operation, Operation::InlineCall(_)) })
        );
        assert_eq!(inline_calls(&mut handler, &callees).unwrap(), 1);
        assert!(
            !handler
                .instructions
                .iter()
                .any(|instruction| { matches!(instruction.operation, Operation::InlineCall(_)) })
        );
        assert!(handler.instructions.iter().any(|instruction| {
            instruction.operation
                == Operation::Intrinsic(Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(
                    BinaryOperation::Add,
                )))
        }));
    }

    #[test]
    fn inlines_multiple_calls_without_intermediate_compaction() {
        let (mut handler, callees) = lower(
            r#"
inline fn increment(value: i32) -> i32 {
    value + 1
}

handler IncrementTwice(value: i32) {
    let first = increment(value);
    let second = increment(first);
    assert_nonzero(second);
    dispatch_next;
}
"#,
        );

        assert_eq!(inline_calls(&mut handler, &callees).unwrap(), 2);
        assert!(
            !handler
                .instructions
                .iter()
                .any(|instruction| matches!(instruction.operation, Operation::InlineCall(_)))
        );
    }

    #[test]
    fn inlines_terminal_straight_line_helpers() {
        let (mut handler, callees) = lower(
            r#"
inline fn finish() {
    dispatch_next;
}

handler Finish() {
    finish();
}
"#,
        );
        assert_eq!(inline_calls(&mut handler, &callees).unwrap(), 1);
        assert_eq!(
            handler.blocks[handler.entry.0].terminator,
            Some(Terminator::Unreachable)
        );
        assert!(handler.instructions.iter().any(|instruction| {
            instruction.operation
                == Operation::Intrinsic(Intrinsic::Control(crate::intrinsic::ControlOperation::DispatchNext))
        }));
    }

    #[test]
    fn inlines_output_operands_as_storage_locations() {
        let (mut handler, callees) = lower(
            r#"
inline fn store_and_finish(dst: out Operand, value: Value) {
    store(dst, value);
    dispatch_next;
}

handler Store(dst: out Operand, value: Value) = store_and_finish(dst, value);
"#,
        );
        assert_eq!(inline_calls(&mut handler, &callees).unwrap(), 1);
        assert!(handler.instructions.iter().any(|instruction| {
            instruction.operation
                == Operation::Intrinsic(Intrinsic::Operand(crate::intrinsic::OperandOperation::Store(
                    crate::intrinsic::OperandStore::Field,
                )))
        }));
        assert!(
            !handler
                .instructions
                .iter()
                .any(|instruction| { matches!(instruction.operation, Operation::InlineCall(_)) })
        );
    }

    #[test]
    fn resolves_inlined_indirect_jumps_to_closure_blocks() {
        let (mut handler, callees) = lower(
            r#"
inline fn choose(condition: u32, yes: Label, no: Label) {
    guard condition != 0 else no;
    goto yes;
}

handler Choose(condition: u32) {
    let yes = || {
        dispatch_next;
    };
    let no = || @cold {
        exit;
    };
    choose(condition, yes, no);
}
"#,
        );
        assert_eq!(inline_calls(&mut handler, &callees).unwrap(), 1);
        optimize::optimize_function(&mut handler);
        assert!(
            !handler
                .blocks
                .iter()
                .any(|block| { matches!(block.terminator, Some(Terminator::IndirectJump { .. })) })
        );
        let jump_targets = handler
            .blocks
            .iter()
            .filter_map(|block| match &block.terminator {
                Some(Terminator::Jump(edge)) => Some(edge.block),
                _ => None,
            })
            .collect::<Vec<_>>();
        assert!(
            jump_targets
                .iter()
                .any(|block| { handler.blocks[block.0].layout == super::super::ir::BlockLayout::Hot })
        );
        assert!(
            jump_targets
                .iter()
                .any(|block| { handler.blocks[block.0].layout == super::super::ir::BlockLayout::Cold })
        );
    }

    #[test]
    fn clones_multi_block_control_flow_and_return_edges() {
        let (mut handler, callees) = lower(
            r#"
inline fn select(condition: u32, lhs: i32, rhs: i32) -> i32 {
    if condition != 0 {
        lhs
    } else {
        rhs
    }
}

handler Select(condition: u32, lhs: i32, rhs: i32) {
    let selected = select(condition, lhs, rhs);
    assert_nonzero(selected);
    dispatch_next;
}
"#,
        );
        assert_eq!(inline_calls(&mut handler, &callees).unwrap(), 1);
        assert!(handler.blocks.len() > 2);
        assert!(
            handler
                .blocks
                .iter()
                .any(|block| matches!(block.terminator, Some(Terminator::Branch { .. })))
        );
        assert!(
            !handler
                .instructions
                .iter()
                .any(|instruction| { matches!(instruction.operation, Operation::InlineCall(_)) })
        );
    }

    #[test]
    fn clones_checked_operations_and_cold_failure_blocks() {
        let (mut handler, callees) = lower(
            r#"
inline fn add_or_dispatch(lhs: i32, rhs: i32) -> i32 {
    guard let sum = checked_add(lhs, rhs) else @cold {
        dispatch_next;
    };
    sum
}

handler Add(lhs: i32, rhs: i32) {
    let sum = add_or_dispatch(lhs, rhs);
    assert_nonzero(sum);
    dispatch_next;
}
"#,
        );
        assert_eq!(inline_calls(&mut handler, &callees).unwrap(), 1);
        assert!(
            handler
                .blocks
                .iter()
                .any(|block| { matches!(block.terminator, Some(Terminator::CheckedOperation { .. })) })
        );
        assert!(
            handler
                .blocks
                .iter()
                .any(|block| block.layout == super::super::ir::BlockLayout::Cold)
        );
    }

    #[test]
    fn uses_substituted_checked_operation_effects() {
        let (mut handler, callees) = lower(
            r#"
inline fn apply(
    value: i32,
    operation: CheckedBinaryOperation<i32>
) {
    let mut result = value;
    operation(result, 1) else @cold {
        dispatch_next;
    };
    assert_nonzero(result);
    dispatch_next;
}

handler Apply(value: i32) = apply(value, checked_add);
"#,
        );
        assert!(callees[0].blocks.iter().any(|block| {
            matches!(
                &block.terminator,
                Some(Terminator::CheckedOperation {
                    operation: Operation::Parameter(crate::ssa::OperationParameterId(1)),
                    ..
                })
            )
        }));
        assert_eq!(inline_calls(&mut handler, &callees).unwrap(), 1);
        let effects =
            handler
                .blocks
                .iter()
                .find_map(|block| match &block.terminator {
                    Some(Terminator::CheckedOperation {
                        operation:
                            Operation::Intrinsic(Intrinsic::CheckedInteger(crate::ssa::CheckedIntegerOperation::Add)),
                        effects,
                        ..
                    }) => Some(effects),
                    _ => None,
                })
                .expect("expected substituted checked operation");
        assert_eq!(effects.memory, MemoryEffect::None);
        assert!(!effects.may_call);
        assert!(!effects.may_trap);
    }

    #[test]
    fn keeps_inlined_helper_blocks_cold_at_cold_call_sites() {
        let (mut handler, callees) = lower(
            r#"
inline fn helper(value: u64) {
    if value != 0 {
        dispatch_next;
    }
    dispatch_next;
}

handler Use(value: u64) {
    guard value != 0 else @cold {
        helper(value);
        exit;
    };
    dispatch_next;
}
"#,
        );
        assert_eq!(inline_calls(&mut handler, &callees).unwrap(), 1);
        let inlined_blocks = handler
            .blocks
            .iter()
            .filter(|block| {
                block
                    .name
                    .as_deref()
                    .is_some_and(|name| name.starts_with("inline_helper_"))
            })
            .collect::<Vec<_>>();
        assert!(!inlined_blocks.is_empty());
        assert!(
            inlined_blocks
                .iter()
                .all(|block| block.layout == super::super::ir::BlockLayout::Cold)
        );
    }

    #[test]
    fn preserves_explicit_result_widening() {
        let (mut handler, callees) = lower(
            r#"
inline fn identity(value: u8) -> u8 {
    value
}

handler Widen(value: u8) {
    let widened: i32 = alias(identity(value));
    assert_nonzero(widened);
    dispatch_next;
}
"#,
        );
        assert_eq!(inline_calls(&mut handler, &callees).unwrap(), 1);
        assert!(handler.instructions.iter().any(|instruction| {
            instruction.operation == Operation::Intrinsic(Intrinsic::Value(ValueOperation::Reuse))
                && handler.values[instruction.results[0].0].ty == Type::I32
        }));
    }
}
