/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SSA inline expansion.

use super::optimize::{InstructionOrder, rebuild_instruction_arena, rewrite_function_uses};
use super::{BlockId, Edge, Function, Intrinsic, MemoryEffect, Operation, Terminator, ValueDefinition, ValueId, ValueOperation};
#[cfg(test)]
use super::{BinaryOperation, IntegerBinaryOperation};
use crate::types::Type;
use crate::{CompileError, CompileStage};
use std::collections::{HashMap, HashSet};

fn inline_error(function: &Function, message: impl Into<String>) -> CompileError {
    CompileError::new(CompileStage::Ssa, Some(&function.name), message)
}

pub(crate) fn inline_calls(function: &mut Function, callees: &[Function]) -> Result<usize, CompileError> {
    let mut count = 0;
    loop {
        if lower_checked_inline_call(function, callees)? {
            continue;
        }
        let Some((block_index, instruction_index, callee)) = find_call(function, callees)? else {
            break;
        };
        inline_call(function, block_index, instruction_index, callee)?;
        count += 1;
    }
    function
        .validate()
        .map_err(|message| inline_error(function, message))?;
    Ok(count)
}

fn lower_checked_inline_call(function: &mut Function, callees: &[Function]) -> Result<bool, CompileError> {
    for block_index in 0..function.blocks.len() {
        let Some(Terminator::CheckedOperation {
            operation: Operation::InlineCall(id),
            inputs,
            results,
            effects,
            success,
            failure,
        }) = function.blocks[block_index].terminator.clone()
        else {
            continue;
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

        let block = BlockId(block_index);
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
        function.blocks[block_index].terminator = Some(Terminator::jump_with_arguments(
            success.block,
            call_results
                .into_iter()
                .chain(success.arguments.into_iter().skip(results.len()))
                .collect(),
        ));
        return Ok(true);
    }
    Ok(false)
}

fn find_call<'a>(
    function: &Function,
    callees: &'a [Function],
) -> Result<Option<(usize, usize, &'a Function)>, CompileError> {
    for (block_index, block) in function.blocks.iter().enumerate() {
        for (instruction_index, instruction_id) in block.instructions.iter().enumerate() {
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
            return Ok(Some((block_index, instruction_index, callee)));
        }
    }
    Ok(None)
}

fn inline_call(function: &mut Function, block_index: usize, instruction_index: usize, callee: &Function) -> Result<(), CompileError> {
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
    let continuation_types = call.results.iter().map(|result| function.values[result.0].ty.clone()).collect();
    let continuation = function.create_named_block(format!("inline_{}_continuation", callee.name), function.blocks[block_index].layout, continuation_types);
    let tail = function.blocks[block_index].instructions.split_off(instruction_index + 1);
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
    rebuild_instruction_arena(function, &HashSet::from([call_id]), InstructionOrder::ByBlock);

    let mut blocks = HashMap::from([(callee.entry, caller_block)]);
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
            callee_block.name.as_ref().map(|name| format!("inline_{}_{name}", callee.name)),
            if caller_layout == super::ir::BlockLayout::Cold {
                super::ir::BlockLayout::Cold
            } else {
                callee_block.layout
            },
            parameter_types,
        );
        blocks.insert(callee_block_id, cloned);
    }

    let mut values = HashMap::new();
    for (index, input) in call.inputs.iter().enumerate() {
        let input = coerce_value(function, caller_block, *input, &callee.parameter_types[index])?;
        values.insert(callee.parameter(index), input);
    }
    for (index, value) in callee.values.iter().enumerate() {
        if let ValueDefinition::Constant(constant) = &value.definition {
            values.insert(ValueId(index), function.add_constant(value.ty.clone(), constant.clone()));
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
            instruction.effects,
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
        clone_terminator(function, callee, terminator, cloned_block, continuation, &blocks, &mut values)?;
    }
    Ok(())
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
            edge
                .arguments
                .iter()
                .map(|value| mapped(function, values, *value, &callee.name))
                .collect::<Result<_, _>>()?,
        ))
    };
    let cloned = match terminator {
        Terminator::Jump(target) => Terminator::Jump(edge(target)?),
        Terminator::Branch { condition, then_edge, else_edge } => Terminator::branch_edges(
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
        Terminator::CheckedOperation { operation, inputs, results, effects, success, failure } => {
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
            let result_types = results.iter().map(|result| callee.values[result.0].ty.clone()).collect();
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
    Some(Edge::with_arguments(*block, instruction.inputs.clone()))
}

fn coerce_value(function: &mut Function, block: BlockId, value: ValueId, target: &Type) -> Result<ValueId, CompileError> {
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
    values.get(&value).copied().ok_or_else(|| {
        inline_error(
            function,
            format!("callee '{callee}' uses unmapped value {value:?}"),
        )
    })
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
        assert!(handler.instructions.iter().any(|instruction| {
            matches!(instruction.operation, Operation::InlineCall(_))
        }));
        assert_eq!(inline_calls(&mut handler, &callees).unwrap(), 1);
        assert!(!handler.instructions.iter().any(|instruction| {
            matches!(instruction.operation, Operation::InlineCall(_))
        }));
        assert!(handler.instructions.iter().any(|instruction| {
            instruction.operation
                == Operation::Intrinsic(Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(BinaryOperation::Add)))
        }));
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
        assert_eq!(handler.blocks[handler.entry.0].terminator, Some(Terminator::Unreachable));
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
        assert!(!handler.instructions.iter().any(|instruction| {
            matches!(instruction.operation, Operation::InlineCall(_))
        }));
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
        assert!(!handler.blocks.iter().any(|block| {
            matches!(block.terminator, Some(Terminator::IndirectJump { .. }))
        }));
        let jump_targets = handler
            .blocks
            .iter()
            .filter_map(|block| match &block.terminator {
                Some(Terminator::Jump(edge)) => Some(edge.block),
                _ => None,
            })
            .collect::<Vec<_>>();
        assert!(jump_targets.iter().any(|block| {
            handler.blocks[block.0].layout == super::super::ir::BlockLayout::Hot
        }));
        assert!(jump_targets.iter().any(|block| {
            handler.blocks[block.0].layout == super::super::ir::BlockLayout::Cold
        }));
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
        assert!(handler.blocks.iter().any(|block| matches!(block.terminator, Some(Terminator::Branch { .. }))));
        assert!(!handler.instructions.iter().any(|instruction| {
            matches!(instruction.operation, Operation::InlineCall(_))
        }));
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
        assert!(handler.blocks.iter().any(|block| {
            matches!(block.terminator, Some(Terminator::CheckedOperation { .. }))
        }));
        assert!(handler.blocks.iter().any(|block| block.layout == super::super::ir::BlockLayout::Cold));
    }

    #[test]
    fn uses_substituted_checked_operation_effects() {
        let (mut handler, callees) = lower(
            r#"
inline fn apply(
    value: i32,
    operation: CheckedBinaryOperation<i32>
) {
    let result = value;
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
        let effects = handler
            .blocks
            .iter()
            .find_map(|block| match &block.terminator {
                Some(Terminator::CheckedOperation {
                    operation: Operation::Intrinsic(Intrinsic::CheckedInteger(crate::ssa::CheckedIntegerOperation::Add)),
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
            .filter(|block| block.name.as_deref().is_some_and(|name| name.starts_with("inline_helper_")))
            .collect::<Vec<_>>();
        assert!(!inlined_blocks.is_empty());
        assert!(inlined_blocks
            .iter()
            .all(|block| block.layout == super::super::ir::BlockLayout::Cold));
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
            instruction.operation
                == Operation::Intrinsic(Intrinsic::Value(ValueOperation::Reuse))
                && handler.values[instruction.results[0].0].ty == Type::I32
        }));
    }
}
