/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lower typed HIR into SSA.

use super::{
    BlockId, BlockLayout, Constant, Edge, Effects, Function, Operation, OperationParameterId, Terminator,
    ValueDefinition, ValueId,
};
use crate::frontend::ast::ParameterMode;
use crate::hash::HashMap;
use crate::hir as typecheck;
use crate::hir::{
    CallTarget, Condition, Handler, InlineFunction, ScalarMatchArmIr, StatementKindIr, TagMask, Value, ValueKind,
    ValueMatchArm, ValueMatchArmKind, ValueMatchFallbackIr, VariableId,
};
use crate::intrinsic::{
    BinaryOperation, ClassificationOperation, ComparisonDomain, ControlOperation, IntegerBinaryOperation,
    IntegerComparisonOperation, Intrinsic, LowLevelOperation, OperandLoad, OperandOperation, OperandStore,
    ValueOperation,
};
use crate::types::{BlockTemperature, Type};
use crate::{CompileError, CompileStage};

pub(crate) fn lower_handler(handler: &Handler) -> Result<Function, CompileError> {
    lower_body(&handler.name, &handler.body, &handler.parameters, &[])
        .map_err(|message| CompileError::new(CompileStage::Ssa, Some(&handler.name), message))
}

pub(crate) fn lower_inline_function(function: &InlineFunction) -> Result<Function, CompileError> {
    let explicit_result_count = match &function.return_type {
        Some(Type::Tuple(elements)) => elements.len(),
        Some(_) => 1,
        None => 0,
    };
    let explicit_results = &function.parameters[..explicit_result_count];
    let parameters = &function.parameters[explicit_result_count..];
    let input_parameters = parameters.to_vec();
    let output_parameters = parameters
        .iter()
        .filter(|parameter| {
            let variable = &function.body.variables[**parameter];
            match variable.parameter_mode {
                Some(ParameterMode::Out) => variable.ty != Type::Operand,
                Some(ParameterMode::InOut) => variable.ty != Type::Operand,
                _ => false,
            }
        })
        .copied()
        .collect::<Vec<_>>();
    let result_variables = explicit_results
        .iter()
        .chain(&output_parameters)
        .copied()
        .collect::<Vec<_>>();
    lower_body(&function.name, &function.body, &input_parameters, &result_variables)
        .map_err(|message| CompileError::new(CompileStage::Ssa, Some(&function.name), message))
}

fn lower_body(
    name: &str,
    body: &typecheck::Body,
    parameters: &[VariableId],
    results: &[VariableId],
) -> Result<Function, String> {
    let parameter_types = parameters
        .iter()
        .map(|parameter| body.variables[*parameter].ty.clone())
        .collect();
    let result_types = results
        .iter()
        .map(|result| body.variables[*result].ty.clone())
        .collect();
    let mut lowered = Function::new(name, parameter_types, result_types);
    let mut bindings = HashMap::default();
    for (index, parameter) in parameters.iter().enumerate() {
        lowered.set_parameter_name(index, &body.variables[*parameter].name);
        bindings.insert(*parameter, lowered.parameter(index));
    }
    let mut continuations = Vec::new();
    let mut continuation_indices = HashMap::default();
    declare_continuations(
        &body.statements,
        body,
        &mut lowered,
        &mut continuations,
        &mut continuation_indices,
    )?;
    let mut lowerer = Lowerer {
        current_block: Some(lowered.entry),
        function: lowered,
        bindings: bindings.clone(),
        entry_bindings: bindings,
        body,
        continuations,
        continuation_indices,
        indirect_targets: HashMap::default(),
        int32_value_sources: HashMap::default(),
    };
    lowerer.lower_statements(&body.statements)?;
    if let Some(block) = lowerer.current_block.take() {
        let results = results
            .iter()
            .map(|result| lowerer.binding(*result))
            .collect::<Result<Vec<_>, _>>()?;
        lowerer.function.set_terminator(block, Terminator::Return(results));
    }
    lowerer.lower_continuations()?;
    super::optimize::fuse_operand_accesses(&mut lowerer.function);
    lowerer.function.validate()?;
    Ok(lowerer.function)
}

#[derive(Clone)]
struct Continuation {
    name: String,
    block: BlockId,
    parameters: Vec<VariableId>,
    captures: Vec<VariableId>,
    body: Vec<super::typecheck::Statement>,
}

fn declare_continuations(
    statements: &[super::typecheck::Statement],
    body: &super::typecheck::Body,
    function: &mut Function,
    continuations: &mut Vec<Continuation>,
    continuation_indices: &mut HashMap<String, usize>,
) -> Result<(), String> {
    for statement in statements {
        if let StatementKindIr::Continuation {
            name,
            temperature,
            parameters,
            captures,
            body: continuation_body,
        } = &statement.kind
        {
            let parameter_types = parameters
                .iter()
                .chain(captures)
                .map(|variable| {
                    body.variables
                        .get(*variable)
                        .map(|variable| variable.ty.clone())
                        .ok_or_else(|| format!("continuation '{name}' uses invalid variable {variable}"))
                })
                .collect::<Result<Vec<_>, _>>()?;
            let block = function.create_named_block(name.clone(), (*temperature).into(), parameter_types);
            let index = continuations.len();
            if continuation_indices.insert(name.clone(), index).is_some() {
                return Err(format!("duplicate continuation '{name}'"));
            }
            continuations.push(Continuation {
                name: name.clone(),
                block,
                parameters: parameters.clone(),
                captures: captures.clone(),
                body: continuation_body.clone(),
            });
        }
        let mut result = Ok(());
        statement.kind.for_each_nested_body(|nested| {
            if result.is_ok() {
                result = declare_continuations(nested, body, function, continuations, continuation_indices);
            }
        });
        result?;
    }
    Ok(())
}

struct Lowerer<'a> {
    function: Function,
    current_block: Option<BlockId>,
    bindings: HashMap<VariableId, ValueId>,
    entry_bindings: HashMap<VariableId, ValueId>,
    body: &'a super::typecheck::Body,
    continuations: Vec<Continuation>,
    continuation_indices: HashMap<String, usize>,
    indirect_targets: HashMap<String, (BlockId, VariableId)>,
    int32_value_sources: HashMap<ValueId, ValueId>,
}

fn field_load_base(call: &typecheck::Call) -> Option<VariableId> {
    let ValueKind::Memory(components) = &call.arguments.first()?.kind else {
        return None;
    };
    let ValueKind::Variable(base) = components.first()?.kind else {
        return None;
    };
    Some(base)
}

fn paired_field_call(call: &typecheck::Call) -> Option<(u32, u8)> {
    let pair = call.field_access()?.pair?;
    Some((pair.id, pair.order))
}

fn schedule_statement_indices(statements: &[typecheck::Statement]) -> Vec<usize> {
    let mut scheduled = Vec::with_capacity(statements.len());
    let mut index = 0;
    while index < statements.len() {
        let paired_guard_load = (|| {
            let StatementKindIr::Call(destinations, call) = &statements.get(index)?.kind else {
                return None;
            };
            let [destination] = destinations.as_slice() else {
                return None;
            };
            let (pair, order) = paired_field_call(call)?;
            let base = field_load_base(call)?;
            let mut guard_uses_first = false;
            for (second_index, statement) in statements.iter().enumerate().skip(index + 1) {
                if let StatementKindIr::Call(_, call) = &statement.kind
                    && guard_uses_first
                    && paired_field_call(call)
                        .is_some_and(|(other_pair, other_order)| other_pair == pair && other_order != order)
                    && field_load_base(call) == Some(base)
                {
                    return Some((second_index, order));
                }
                match &statement.kind {
                    StatementKindIr::Guard { condition, .. } => {
                        guard_uses_first |= typecheck::condition_uses_and_defs(condition).0.contains(destination);
                    }
                    StatementKindIr::Call(_, call) if call.is_load() && call.return_type.is_some() => {}
                    _ => break,
                }
            }
            None
        })();
        let Some((second_index, first_order)) = paired_guard_load else {
            scheduled.push(index);
            index += 1;
            continue;
        };
        if first_order == 0 {
            scheduled.extend([index, second_index]);
        } else {
            scheduled.extend([second_index, index]);
        }
        scheduled.extend(index + 1..second_index);
        index = second_index + 1;
    }
    scheduled
}

fn direct_jump_target(statements: &[typecheck::Statement]) -> Option<&str> {
    let [
        typecheck::Statement {
            kind: StatementKindIr::Call(destinations, call),
            ..
        },
    ] = statements
    else {
        return None;
    };
    (destinations.is_empty()
        && call.terminal
        && call.intrinsic() == Some(Intrinsic::Control(ControlOperation::JumpLabel)))
    .then_some(call.branch_target.as_deref())
    .flatten()
}

impl Lowerer<'_> {
    fn lower_statements(&mut self, statements: &[super::typecheck::Statement]) -> Result<(), String> {
        let schedule = schedule_statement_indices(statements);
        let mut position = 0;
        while let Some(index) = schedule.get(position).copied() {
            position += 1;
            let statement = &statements[index];
            if matches!(statement.kind, StatementKindIr::Continuation { .. }) {
                continue;
            }
            if self.current_block.is_none() {
                return Err(format!(
                    "handler '{}' contains a statement after terminal control flow",
                    self.function.name
                ));
            }
            match &statement.kind {
                StatementKindIr::Call(destinations, call) => {
                    if let Some(failure) = &call.failure {
                        self.lower_fallible_call(destinations, call, &failure.captures, &failure.body)?;
                    } else if call.intrinsic() == Some(Intrinsic::Control(ControlOperation::JumpLabel))
                        && let Some(target) = call
                            .branch_target
                            .as_ref()
                            .filter(|target| self.continuation_indices.contains_key(*target))
                    {
                        self.lower_continuation_jump(target)?;
                    } else {
                        let result_types = if destinations.is_empty() {
                            call.return_type.clone().into_iter().collect()
                        } else {
                            destinations
                                .iter()
                                .map(|destination| self.variable_type(*destination))
                                .collect::<Result<Vec<_>, _>>()?
                        };
                        let results = self.lower_call(call, result_types)?;
                        for (destination, result) in destinations.iter().zip(results) {
                            self.bindings.insert(*destination, result);
                        }
                    }
                    if call.terminal && self.current_block.is_some() {
                        let block = self.current()?;
                        self.function.set_terminator(block, Terminator::Unreachable);
                        self.current_block = None;
                    }
                }
                StatementKindIr::MachineAssign {
                    destination,
                    intrinsic,
                    arguments,
                } => {
                    self.lower_machine_assign(*destination, *intrinsic, arguments)?;
                }
                StatementKindIr::If {
                    destination,
                    condition,
                    temperature,
                    terminal,
                    captures,
                    then_body,
                    else_body,
                    ..
                } => {
                    if let Some(else_body) = else_body {
                        self.lower_if_else(*destination, condition, captures, then_body, else_body)?;
                    } else {
                        self.lower_if(condition, *temperature, *terminal, captures, then_body)?;
                    }
                }
                StatementKindIr::Guard { condition, failure } => self.lower_guard(condition, failure)?,
                StatementKindIr::While {
                    condition_setup,
                    condition,
                    captures,
                    body,
                    post_tested,
                    ..
                } => self.lower_while(condition_setup, condition, captures, body, *post_tested)?,
                StatementKindIr::ValueMatch {
                    destination,
                    value,
                    tag,
                    captures,
                    arms,
                    fallback,
                } => {
                    if let Some(destination) = destination {
                        self.lower_value_value_match(*destination, value, captures, arms, fallback)?;
                    } else {
                        self.lower_value_match(
                            value,
                            tag.expect("control value match must bind its tag"),
                            arms,
                            fallback,
                        )?;
                    }
                }
                StatementKindIr::ScalarMatch {
                    value, arms, fallback, ..
                } => {
                    self.lower_scalar_match(value, arms, fallback)?;
                }
                StatementKindIr::ValueRefinement {
                    binding,
                    value,
                    tag,
                    expected_tag,
                    payload_operation,
                    failure,
                } => self.lower_refinement(
                    *binding,
                    value,
                    *tag,
                    expected_tag.as_deref(),
                    *payload_operation,
                    failure,
                )?,
                unsupported => {
                    return Err(format!(
                        "handler '{}' contains unsupported structured statement {unsupported:?}",
                        self.function.name
                    ));
                }
            }
        }
        Ok(())
    }

    fn lower_continuations(&mut self) -> Result<(), String> {
        for continuation in self.continuations.clone() {
            self.current_block = Some(continuation.block);
            self.bindings = self.entry_bindings.clone();
            for (variable, parameter) in continuation
                .parameters
                .iter()
                .chain(&continuation.captures)
                .zip(&self.function.blocks[continuation.block.0].parameters)
            {
                self.bindings.insert(*variable, *parameter);
            }
            self.lower_statements(&continuation.body)?;
            if self.current_block.is_some() {
                return Err(format!("continuation '{}' does not terminate", continuation.name));
            }
        }
        Ok(())
    }

    fn lower_scalar_match(
        &mut self,
        value: &Value,
        arms: &[ScalarMatchArmIr],
        fallback: &[super::typecheck::Statement],
    ) -> Result<(), String> {
        let source = self.current()?;
        let value = self.lower_value(value)?;
        let original_bindings = self.bindings.clone();
        let mut cases = Vec::new();
        let mut arm_blocks = Vec::with_capacity(arms.len());
        for arm in arms {
            let (edge, block) = if let Some(target) = direct_jump_target(&arm.body) {
                (self.continuation_edge(target)?, None)
            } else {
                let block = self.function.create_empty_block("scalar_match_arm", arm.layout.into());
                (Edge::new(block), Some(block))
            };
            self.lower_scalar_pattern(&arm.pattern, edge, &mut cases)?;
            arm_blocks.push(block);
        }
        let (default, fallback_block) = if let Some(target) = direct_jump_target(fallback) {
            (self.continuation_edge(target)?, None)
        } else {
            let block = self
                .function
                .create_empty_block("scalar_match_fallback", BlockLayout::Hot);
            (Edge::new(block), Some(block))
        };
        self.function
            .set_terminator(source, Terminator::switch(value, cases, default));
        self.current_block = None;
        for (arm, block) in arms.iter().zip(arm_blocks) {
            let Some(block) = block else {
                continue;
            };
            self.current_block = Some(block);
            self.bindings = original_bindings.clone();
            self.lower_statements(&arm.body)?;
            if self.current_block.is_some() {
                return Err("scalar match arm does not terminate".to_string());
            }
        }
        if let Some(block) = fallback_block {
            self.current_block = Some(block);
            self.bindings = original_bindings;
            self.lower_statements(fallback)?;
            if self.current_block.is_some() {
                return Err("scalar match fallback does not terminate".to_string());
            }
        }
        Ok(())
    }

    fn lower_scalar_pattern(
        &mut self,
        pattern: &[Value],
        edge: Edge,
        cases: &mut Vec<(ValueId, Edge)>,
    ) -> Result<(), String> {
        for pattern in pattern {
            cases.push((self.lower_value(pattern)?, edge.clone()));
        }
        Ok(())
    }

    fn lower_continuation_jump(&mut self, target: &str) -> Result<(), String> {
        let source = self.current()?;
        let edge = self.continuation_edge(target)?;
        self.function.set_terminator(source, Terminator::Jump(edge));
        self.current_block = None;
        Ok(())
    }

    fn lower_guard(&mut self, condition: &Condition, failure: &str) -> Result<(), String> {
        let condition = self.lower_condition(condition)?;
        let source = self.current()?;
        let failure = self.continuation_edge(failure)?;
        if failure.arguments.is_empty() {
            self.function.append_instruction(
                source,
                Operation::Guard { failure: failure.block },
                vec![condition],
                Vec::new(),
            );
            return Ok(());
        }
        let layout = self.function.blocks[source.0].layout;
        let success = self.function.create_empty_block("guard_success", layout);
        self.function
            .set_terminator(source, Terminator::branch_edges(condition, Edge::new(success), failure));
        self.current_block = Some(success);
        Ok(())
    }

    fn lower_while(
        &mut self,
        condition_setup: &[super::typecheck::Statement],
        condition: &Condition,
        captures: &[VariableId],
        body: &[super::typecheck::Statement],
        post_tested: bool,
    ) -> Result<(), String> {
        let source = self.current()?;
        let original_bindings = self.bindings.clone();
        let (capture_types, initial_values) = self.capture_types_and_values(captures)?;
        let loop_body = self
            .function
            .create_named_block("while_body", BlockLayout::Hot, capture_types.clone());
        let exit = self
            .function
            .create_named_block("while_exit", BlockLayout::Hot, capture_types.clone());

        if post_tested {
            self.function
                .set_terminator(source, Terminator::jump_with_arguments(loop_body, initial_values));
            self.current_block = Some(loop_body);
            self.bindings = self.bindings_for_block(&original_bindings, captures, loop_body);
            self.lower_statements(body)?;
            self.lower_statements(condition_setup)?;
            let condition = self.lower_condition(condition)?;
            let loop_end = self.current()?;
            let values = self.capture_values(captures)?;
            self.function.set_terminator(
                loop_end,
                Terminator::branch_edges(
                    condition,
                    Edge::with_arguments(loop_body, values.clone()),
                    Edge::with_arguments(exit, values),
                ),
            );
        } else {
            let header = self
                .function
                .create_named_block("while_header", BlockLayout::Hot, capture_types);
            self.function
                .set_terminator(source, Terminator::jump_with_arguments(header, initial_values));
            self.current_block = Some(header);
            self.bindings = self.bindings_for_block(&original_bindings, captures, header);
            self.lower_statements(condition_setup)?;
            let condition = self.lower_condition(condition)?;
            let header_end = self.current()?;
            let header_values = self.capture_values(captures)?;
            let header_bindings = self.bindings.clone();
            self.function.set_terminator(
                header_end,
                Terminator::branch_edges(
                    condition,
                    Edge::with_arguments(loop_body, header_values.clone()),
                    Edge::with_arguments(exit, header_values),
                ),
            );
            self.current_block = Some(loop_body);
            self.bindings = self.bindings_for_block(&header_bindings, captures, loop_body);
            self.lower_statements(body)?;
            let body_end = self.current()?;
            let backedge_values = self.capture_values(captures)?;
            self.function
                .set_terminator(body_end, Terminator::jump_with_arguments(header, backedge_values));
        }

        self.current_block = Some(exit);
        self.bindings = self.bindings_for_block(&original_bindings, captures, exit);
        Ok(())
    }

    fn lower_value_match(
        &mut self,
        value: &Value,
        tag: VariableId,
        arms: &[ValueMatchArm],
        fallback: &ValueMatchFallbackIr,
    ) -> Result<(), String> {
        let source = self.current()?;
        let value = self.lower_value(value)?;
        let tag_value = self.extract_tag(value, false)?;
        self.bindings.insert(tag, tag_value);
        let original_bindings = self.bindings.clone();
        let arm = |kind| arms.iter().find(|arm| arm.kind == kind);
        let int32_arm = arm(ValueMatchArmKind::Int32).expect("control value match must have an Int32 arm");
        let int32_block = self.function.create_empty_block("value_match_int32", BlockLayout::Hot);
        let double_block = arm(ValueMatchArmKind::Double)
            .map(|_| self.function.create_empty_block("value_match_double", BlockLayout::Hot));
        let boolean_block = arm(ValueMatchArmKind::Boolean).map(|_| {
            self.function
                .create_empty_block("value_match_boolean", BlockLayout::Hot)
        });
        let (fallback_edge, fallback_block) = self.value_match_fallback(fallback, "value_match_fallback", &[], &[])?;

        let arm_blocks = arms
            .iter()
            .filter_map(|arm| {
                Some((
                    arm.kind,
                    match arm.kind {
                        ValueMatchArmKind::Int32 => int32_block,
                        ValueMatchArmKind::Double => double_block?,
                        ValueMatchArmKind::Boolean => boolean_block?,
                    },
                ))
            })
            .collect::<Vec<_>>();
        if let Some((_, block)) = arm_blocks.first() {
            self.function.blocks[block.0].layout = BlockLayout::Preferred;
        }
        self.lower_value_match_tests(
            source,
            tag_value,
            &arm_blocks,
            fallback_edge.clone(),
            &[],
            "value_match_test",
        )?;

        for (arm, block) in [
            (Some(int32_arm), Some(int32_block)),
            (arm(ValueMatchArmKind::Double), double_block),
            (arm(ValueMatchArmKind::Boolean), boolean_block),
        ] {
            let (Some(arm), Some(block)) = (arm, block) else {
                continue;
            };
            self.current_block = Some(block);
            self.bindings = original_bindings.clone();
            if let Some(binding) = arm.binding {
                let payload = match arm.kind {
                    ValueMatchArmKind::Int32 => self.unbox_int32(value, false)?,
                    ValueMatchArmKind::Double => self.unbox_float64(value)?,
                    ValueMatchArmKind::Boolean => self.unbox_boolean(value)?,
                };
                self.bindings.insert(binding, payload);
            }
            self.lower_statements(&arm.body)?;
            if self.current_block.is_some() {
                let name = match arm.kind {
                    ValueMatchArmKind::Int32 => "int32",
                    ValueMatchArmKind::Double => "double",
                    ValueMatchArmKind::Boolean => "boolean",
                };
                return Err(format!("{name} value-match arm does not terminate"));
            }
        }

        if let Some(block) = fallback_block {
            self.current_block = Some(block);
            self.bindings = original_bindings;
            self.lower_statements(&fallback.body)?;
            if self.current_block.is_some() {
                return Err("fallback value-match arm does not terminate".to_string());
            }
        }
        Ok(())
    }

    fn lower_value_value_match(
        &mut self,
        destination: VariableId,
        value: &Value,
        captures: &[VariableId],
        arms: &[ValueMatchArm],
        fallback: &ValueMatchFallbackIr,
    ) -> Result<(), String> {
        let source = self.current()?;
        let value = self.lower_value(value)?;
        let tag = self.extract_tag(value, false)?;
        let original_bindings = self.bindings.clone();
        let arm = |kind| arms.iter().find(|arm| arm.kind == kind);
        let int32_arm = arm(ValueMatchArmKind::Int32).expect("value-producing match must have an Int32 arm");
        let double_arm = arm(ValueMatchArmKind::Double).expect("value-producing match must have a double arm");
        let boolean_arm = arm(ValueMatchArmKind::Boolean);
        let double_first = arms.first().is_some_and(|arm| arm.kind == ValueMatchArmKind::Double);
        let boolean_before_double = arms
            .iter()
            .position(|arm| arm.kind == ValueMatchArmKind::Boolean)
            .zip(arms.iter().position(|arm| arm.kind == ValueMatchArmKind::Double))
            .is_some_and(|(boolean, double)| boolean < double);
        let arm_returns_binding_with = |arm_body: &[super::typecheck::Statement],
                                        binding: Option<VariableId>,
                                        intrinsic: Intrinsic| {
            let Some(binding) = binding else {
                return false;
            };
            let [
                super::typecheck::Statement {
                    kind: StatementKindIr::Call(destinations, call),
                    ..
                },
            ] = arm_body
            else {
                return false;
            };
            destinations.as_slice() == [destination]
                && call.intrinsic() == Some(intrinsic)
                && matches!(call.arguments.as_slice(), [Value { kind: ValueKind::Variable(source), .. }] if *source == binding)
        };
        let shared_integer_boolean_payload = !double_first
            && boolean_before_double
            && arm_returns_binding_with(
                &int32_arm.body,
                int32_arm.binding,
                Intrinsic::LowLevel(LowLevelOperation::Move),
            )
            && boolean_arm.is_some_and(|arm| {
                arm_returns_binding_with(&arm.body, arm.binding, Intrinsic::Value(super::ValueOperation::ToInt32))
            });
        let (capture_types, capture_values) = self.capture_types_and_values(captures)?;
        let int32_block =
            self.function
                .create_named_block("value_result_int32", BlockLayout::Hot, capture_types.clone());
        let double_block =
            self.function
                .create_named_block("value_result_double", double_arm.layout.into(), capture_types.clone());
        let boolean_block = boolean_arm.map(|_| {
            if shared_integer_boolean_payload {
                return int32_block;
            }
            self.function
                .create_named_block("value_result_boolean", BlockLayout::Hot, capture_types.clone())
        });
        let (fallback_edge, fallback_block) =
            self.value_match_fallback(fallback, "value_result_fallback", &capture_types, &capture_values)?;
        let mut continuation_types = vec![self.variable_type(destination)?];
        continuation_types.extend(capture_types);
        let continuation =
            self.function
                .create_named_block("value_result_continuation", BlockLayout::Hot, continuation_types);
        let int32_tag = self
            .function
            .add_constant(Type::ValueTag, Constant::symbol("INT32_TAG"));
        let boolean_tag = self
            .function
            .add_constant(Type::ValueTag, Constant::symbol("BOOLEAN_TAG"));
        let nan_base_tag = self
            .function
            .add_constant(Type::ValueTag, Constant::symbol("NAN_BASE_TAG"));
        let arm_blocks = arms
            .iter()
            .filter_map(|arm| {
                Some((
                    arm.kind,
                    match arm.kind {
                        ValueMatchArmKind::Int32 => int32_block,
                        ValueMatchArmKind::Double => double_block,
                        ValueMatchArmKind::Boolean => boolean_block?,
                    },
                ))
            })
            .collect::<Vec<_>>();
        if let Some((_, block)) = arm_blocks.first()
            && self.function.blocks[block.0].layout != BlockLayout::Cold
        {
            self.function.blocks[block.0].layout = BlockLayout::Preferred;
        }
        if shared_integer_boolean_payload {
            let double_test = self
                .function
                .create_empty_block("value_result_double_test", BlockLayout::Preferred);
            self.function.blocks[int32_block.0].layout = BlockLayout::Hot;
            let shared_edge = Edge::with_arguments(int32_block, capture_values.clone());
            self.function.set_terminator(
                source,
                Terminator::switch(
                    tag,
                    vec![(int32_tag, shared_edge.clone()), (boolean_tag, shared_edge)],
                    Edge::new(double_test),
                ),
            );
            self.current_block = Some(double_test);
            let masked_tag = self.append_pure_operation(
                Operation::Intrinsic(Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(
                    BinaryOperation::And,
                ))),
                vec![tag, nan_base_tag],
                Type::ValueTag,
            )?;
            let is_not_double =
                self.append_integer_comparison(IntegerComparisonOperation::Equal, vec![masked_tag, nan_base_tag])?;
            self.function.set_terminator(
                double_test,
                Terminator::branch_edges(
                    is_not_double,
                    fallback_edge.clone(),
                    Edge::with_arguments(double_block, capture_values.clone()),
                ),
            );
        } else {
            self.lower_value_match_tests(
                source,
                tag,
                &arm_blocks,
                fallback_edge.clone(),
                &capture_values,
                "value_result_test",
            )?;
        }

        self.current_block = Some(int32_block);
        self.bindings = self.bindings_for_block(&original_bindings, captures, int32_block);
        if let Some(binding) = int32_arm.binding {
            let payload = self.unbox_int32(value, false)?;
            self.bindings.insert(binding, payload);
        }
        self.lower_statements(&int32_arm.body)?;
        self.finish_value_result_arm(destination, captures, continuation, "int32")?;

        self.current_block = Some(double_block);
        self.bindings = self.bindings_for_block(&original_bindings, captures, double_block);
        if let Some(binding) = double_arm.binding {
            let payload = self.unbox_float64(value)?;
            self.bindings.insert(binding, payload);
        }
        self.lower_statements(&double_arm.body)?;
        self.finish_value_result_arm(destination, captures, continuation, "double")?;

        if !shared_integer_boolean_payload && let (Some(block), Some(arm)) = (boolean_block, boolean_arm) {
            self.current_block = Some(block);
            self.bindings = self.bindings_for_block(&original_bindings, captures, block);
            if let Some(binding) = arm.binding {
                let payload = self.unbox_boolean(value)?;
                self.bindings.insert(binding, payload);
            }
            self.lower_statements(&arm.body)?;
            self.finish_value_result_arm(destination, captures, continuation, "boolean")?;
        }

        if let Some(block) = fallback_block {
            self.current_block = Some(block);
            self.bindings = self.bindings_for_block(&original_bindings, captures, block);
            self.lower_statements(&fallback.body)?;
            if self.current_block.is_some() {
                return Err("fallback value-match arm does not terminate".to_string());
            }
        }

        self.current_block = Some(continuation);
        self.bindings = original_bindings;
        let parameters = &self.function.blocks[continuation.0].parameters;
        self.bindings.insert(destination, parameters[0]);
        for (capture, parameter) in captures.iter().zip(&parameters[1..]) {
            self.bindings.insert(*capture, *parameter);
        }
        Ok(())
    }

    fn lower_value_match_tests(
        &mut self,
        source: BlockId,
        tag: ValueId,
        arms: &[(ValueMatchArmKind, BlockId)],
        fallback: Edge,
        arguments: &[ValueId],
        block_name: &str,
    ) -> Result<(), String> {
        let int32_tag = self
            .function
            .add_constant(Type::ValueTag, Constant::symbol("INT32_TAG"));
        let boolean_tag = self
            .function
            .add_constant(Type::ValueTag, Constant::symbol("BOOLEAN_TAG"));
        let nan_base_tag = self
            .function
            .add_constant(Type::ValueTag, Constant::symbol("NAN_BASE_TAG"));
        let mut test_block = source;
        for (index, (kind, arm)) in arms.iter().enumerate() {
            self.current_block = Some(test_block);
            let does_not_match = match kind {
                ValueMatchArmKind::Int32 => {
                    self.append_integer_comparison(IntegerComparisonOperation::NotEqual, vec![tag, int32_tag])?
                }
                ValueMatchArmKind::Double => {
                    let masked_tag = self.append_pure_operation(
                        Operation::Intrinsic(Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(
                            BinaryOperation::And,
                        ))),
                        vec![tag, nan_base_tag],
                        Type::ValueTag,
                    )?;
                    self.append_integer_comparison(IntegerComparisonOperation::Equal, vec![masked_tag, nan_base_tag])?
                }
                ValueMatchArmKind::Boolean => {
                    self.append_integer_comparison(IntegerComparisonOperation::NotEqual, vec![tag, boolean_tag])?
                }
            };
            let next = if index + 1 == arms.len() {
                fallback.clone()
            } else {
                Edge::new(
                    self.function
                        .create_empty_block(format!("{block_name}_{}", index + 1), BlockLayout::Hot),
                )
            };
            self.function.set_terminator(
                test_block,
                Terminator::branch_edges(
                    does_not_match,
                    next.clone(),
                    Edge::with_arguments(*arm, arguments.to_vec()),
                ),
            );
            test_block = next.block;
        }
        Ok(())
    }

    fn value_match_fallback(
        &mut self,
        fallback: &ValueMatchFallbackIr,
        name: &str,
        parameter_types: &[Type],
        arguments: &[ValueId],
    ) -> Result<(Edge, Option<BlockId>), String> {
        if let Some(target) = direct_jump_target(&fallback.body) {
            return Ok((self.continuation_edge(target)?, None));
        }
        let layout = if fallback.temperature == BlockTemperature::Cold {
            BlockLayout::Cold
        } else {
            BlockLayout::Hot
        };
        let block = self.function.create_named_block(name, layout, parameter_types.to_vec());
        Ok((Edge::with_arguments(block, arguments.to_vec()), Some(block)))
    }

    fn finish_value_result_arm(
        &mut self,
        destination: VariableId,
        captures: &[VariableId],
        continuation: BlockId,
        name: &str,
    ) -> Result<(), String> {
        let block = self
            .current_block
            .take()
            .ok_or_else(|| format!("value-producing {name} arm terminates"))?;
        let mut arguments = vec![self.binding(destination)?];
        arguments.extend(self.capture_values(captures)?);
        self.function
            .set_terminator(block, Terminator::jump_with_arguments(continuation, arguments));
        Ok(())
    }

    fn lower_refinement(
        &mut self,
        binding: VariableId,
        value: &Value,
        tag: VariableId,
        expected_tag: Option<&str>,
        payload_operation: ValueOperation,
        failure: &str,
    ) -> Result<(), String> {
        let source = self.current()?;
        let value = self.lower_value(value)?;
        let tag_value = self.extract_tag(value, false)?;
        self.bindings.insert(tag, tag_value);
        let condition = if let Some(expected_tag) = expected_tag {
            let expected_tag = self
                .function
                .add_constant(Type::ValueTag, Constant::symbol(expected_tag));
            self.append_integer_comparison(IntegerComparisonOperation::Equal, vec![tag_value, expected_tag])?
        } else {
            self.append_classification(ClassificationOperation::Float64Tag, vec![tag_value])?
        };
        let success = self.function.create_empty_block("refinement_success", BlockLayout::Hot);
        let failure = self.continuation_edge(failure)?;
        self.function
            .set_terminator(source, Terminator::branch_edges(condition, Edge::new(success), failure));
        self.current_block = Some(success);
        let payload_type = self.variable_type(binding)?;
        let payload = self.append_unary_value(payload_operation, value, payload_type)?;
        self.bindings.insert(binding, payload);
        Ok(())
    }

    fn continuation_edge(&mut self, target: &str) -> Result<Edge, String> {
        if let Some(continuation) = self
            .continuation_indices
            .get(target)
            .map(|index| &self.continuations[*index])
        {
            let arguments = continuation
                .parameters
                .iter()
                .chain(&continuation.captures)
                .map(|variable| self.binding(*variable))
                .collect::<Result<Vec<_>, _>>()?;
            return Ok(Edge::with_arguments(continuation.block, arguments));
        }
        if let Some((block, variable)) = self.indirect_targets.get(target) {
            return Ok(Edge::with_arguments(*block, vec![self.binding(*variable)?]));
        }
        let variable = self
            .body
            .variables
            .iter()
            .position(|variable| variable.name == target && variable.ty == Type::label())
            .ok_or_else(|| format!("unknown continuation or label '{target}'"))?;
        let target_value = self.binding(variable)?;
        let block =
            self.function
                .create_named_block(format!("indirect_{target}"), BlockLayout::Cold, vec![Type::label()]);
        self.function.set_terminator(
            block,
            Terminator::IndirectJump {
                target: self.function.blocks[block.0].parameters[0],
            },
        );
        self.indirect_targets.insert(target.to_string(), (block, variable));
        Ok(Edge::with_arguments(block, vec![target_value]))
    }

    fn lower_fallible_call(
        &mut self,
        destinations: &[VariableId],
        call: &super::typecheck::Call,
        captures: &[VariableId],
        failure_body: &[super::typecheck::Statement],
    ) -> Result<(), String> {
        let Some(Intrinsic::CheckedInteger(operation)) = call.intrinsic() else {
            unreachable!("only checked arithmetic uses structured fallible calls")
        };
        let [lhs, rhs, _failure] = call.arguments.as_slice() else {
            unreachable!("checked binary arithmetic has two inputs and a failure")
        };
        let [result] = destinations else {
            unreachable!("checked binary arithmetic has one result")
        };
        let source = self.current()?;
        let source_block = source;
        let original_bindings = self.bindings.clone();
        let inputs = vec![self.lower_value(lhs)?, self.lower_value(rhs)?];
        let rematerializable_inputs = inputs.clone();
        let result_type = self.variable_type(*result)?;
        let success = self.function.create_named_block(
            format!("{}_success", operation.name()),
            BlockLayout::Hot,
            vec![result_type.clone()],
        );
        let (capture_types, capture_values) = self.capture_types_and_values(captures)?;
        let failure = self.function.create_named_block(
            format!("{}_failure", operation.name()),
            BlockLayout::Cold,
            capture_types,
        );
        self.function.set_checked_operation(
            source,
            Intrinsic::CheckedInteger(operation),
            inputs,
            vec![result_type],
            Effects::PURE,
            success,
            Vec::new(),
            Edge::with_arguments(failure, capture_values),
        );

        self.current_block = Some(failure);
        self.bindings = self.bindings_for_block(&original_bindings, captures, failure);
        for original_value in rematerializable_inputs {
            if let Some(source) = self.int32_value_sources.get(&original_value).copied() {
                let source = self.rematerialize_loaded_value(source, source_block).unwrap_or(source);
                let value = self.unbox_int32(source, true)?;
                for variable in original_bindings
                    .iter()
                    .filter_map(|(variable, value)| (*value == original_value).then_some(*variable))
                {
                    self.bindings.insert(variable, value);
                }
            }
        }
        self.lower_statements(failure_body)?;
        if self.current_block.is_some() {
            return Err(format!("{} failure path does not terminate", operation.name()));
        }

        self.current_block = Some(success);
        self.bindings = original_bindings;
        self.bindings
            .insert(*result, self.function.blocks[success.0].parameters[0]);
        Ok(())
    }

    fn rematerialize_loaded_value(&mut self, value: ValueId, target: BlockId) -> Option<ValueId> {
        let ValueDefinition::InstructionResult { instruction, .. } = self.function.values[value.0].definition else {
            return None;
        };
        let cfg = super::analysis::ControlFlowGraph::compute(&self.function);
        let dominators = super::analysis::DominatorTree::compute(&self.function, &cfg);
        let layout = super::analysis::InstructionLayout::compute(&self.function).ok()?;
        let effects = super::effects::EffectDependencies::compute(&self.function, &cfg);
        if !effects.memory_is_stable_after_instruction(&dominators, &layout, instruction, target) {
            return None;
        }
        let instruction = self.function.instructions[instruction.0].clone();
        let (operation, inputs) = match instruction.operation {
            Operation::Intrinsic(Intrinsic::Operand(OperandOperation::Load(OperandLoad::Index))) => (
                OperandOperation::Load(OperandLoad::RematerializedIndex),
                instruction.inputs,
            ),
            // FIXME: Rematerializing stable input-operand loads currently
            //        makes the x86-64 allocator decode output operands again
            //        on cold arithmetic failure paths. Re-enable this for
            //        load and load_operand_pair once allocation weighs the
            //        hot-path lifetime against cold reload cost.
            Operation::Intrinsic(Intrinsic::Operand(
                OperandOperation::Load(OperandLoad::Field) | OperandOperation::LoadPair,
            )) => return None,
            _ => return None,
        };
        let block = self.current().ok()?;
        Some(
            self.function.append_instruction_with_effects(
                block,
                Intrinsic::Operand(operation),
                inputs.to_vec(),
                vec![self.function.values[value.0].ty.clone()],
                instruction.effects,
            )[0],
        )
    }

    fn lower_if(
        &mut self,
        condition: &Condition,
        temperature: BlockTemperature,
        terminal: bool,
        captures: &[VariableId],
        body: &[super::typecheck::Statement],
    ) -> Result<(), String> {
        let condition = self.lower_condition(condition)?;
        let source = self.current()?;
        let original_bindings = self.bindings.clone();
        let (capture_types, capture_values) = self.capture_types_and_values(captures)?;
        let then_block = self.function.create_named_block(
            "if_then",
            if temperature == BlockTemperature::Cold {
                BlockLayout::Cold
            } else {
                BlockLayout::Hot
            },
            capture_types.clone(),
        );

        if terminal {
            let continuation = self.function.create_empty_block("if_continuation", BlockLayout::Hot);
            self.function.set_terminator(
                source,
                Terminator::branch_edges(
                    condition,
                    Edge::with_arguments(then_block, capture_values),
                    Edge::new(continuation),
                ),
            );
            self.current_block = Some(then_block);
            self.bindings = self.bindings_for_block(&original_bindings, captures, then_block);
            self.lower_statements(body)?;
            if self.current_block.is_some() {
                return Err("terminal if body does not terminate".to_string());
            }
            self.current_block = Some(continuation);
            self.bindings = original_bindings;
            return Ok(());
        }

        let continuation = self
            .function
            .create_named_block("if_continuation", BlockLayout::Hot, capture_types);
        self.function.set_terminator(
            source,
            Terminator::branch_edges(
                condition,
                Edge::with_arguments(then_block, capture_values.clone()),
                Edge::with_arguments(continuation, capture_values),
            ),
        );
        self.current_block = Some(then_block);
        self.bindings = self.bindings_for_block(&original_bindings, captures, then_block);
        self.lower_statements(body)?;
        let then_end = self
            .current_block
            .ok_or_else(|| "continuing if body terminates".to_string())?;
        let then_arguments = self.capture_values(captures)?;
        self.function
            .set_terminator(then_end, Terminator::jump_with_arguments(continuation, then_arguments));
        self.current_block = Some(continuation);
        self.bindings = self.bindings_for_block(&original_bindings, captures, continuation);
        Ok(())
    }

    fn lower_if_else(
        &mut self,
        destination: Option<VariableId>,
        condition: &Condition,
        captures: &[VariableId],
        then_body: &[super::typecheck::Statement],
        else_body: &[super::typecheck::Statement],
    ) -> Result<(), String> {
        let condition = self.lower_condition(condition)?;
        let source = self.current()?;
        let original_bindings = self.bindings.clone();
        let (capture_types, capture_values) = self.capture_types_and_values(captures)?;
        let prefix = if destination.is_some() { "value_if" } else { "if" };
        let then_block =
            self.function
                .create_named_block(format!("{prefix}_then"), BlockLayout::Hot, capture_types.clone());
        let else_block =
            self.function
                .create_named_block(format!("{prefix}_else"), BlockLayout::Hot, capture_types.clone());
        let terminals = destination.is_none().then(|| {
            (
                typecheck::block_is_terminal(then_body),
                typecheck::block_is_terminal(else_body),
            )
        });
        let continuation = if terminals.is_some_and(|(then, else_)| then && else_) {
            None
        } else {
            let mut types = destination
                .map(|destination| self.variable_type(destination))
                .transpose()?
                .into_iter()
                .collect::<Vec<_>>();
            types.extend(capture_types);
            Some(
                self.function
                    .create_named_block(format!("{prefix}_continuation"), BlockLayout::Hot, types),
            )
        };
        self.function.set_terminator(
            source,
            Terminator::branch_edges(
                condition,
                Edge::with_arguments(then_block, capture_values.clone()),
                Edge::with_arguments(else_block, capture_values),
            ),
        );

        for (name, block, body, terminal) in [
            ("then", then_block, then_body, terminals.map(|value| value.0)),
            ("else", else_block, else_body, terminals.map(|value| value.1)),
        ] {
            self.current_block = Some(block);
            self.bindings = self.bindings_for_block(&original_bindings, captures, block);
            self.lower_statements(body)?;
            if let Some(destination) = destination {
                let end = self
                    .current_block
                    .ok_or_else(|| format!("value-producing {name} arm terminates"))?;
                let mut arguments = vec![self.binding(destination)?];
                arguments.extend(self.capture_values(captures)?);
                self.function.set_terminator(
                    end,
                    Terminator::jump_with_arguments(
                        continuation.expect("value-producing if must have a continuation"),
                        arguments,
                    ),
                );
            } else {
                self.finish_if_else_arm(
                    terminal.expect("control if must know whether each arm terminates"),
                    continuation,
                    captures,
                    name,
                )?;
            }
        }

        self.current_block = continuation;
        self.bindings = original_bindings;
        if let Some(continuation) = continuation {
            if let Some(destination) = destination {
                let parameters = &self.function.blocks[continuation.0].parameters;
                self.bindings.insert(destination, parameters[0]);
                for (capture, parameter) in captures.iter().zip(&parameters[1..]) {
                    self.bindings.insert(*capture, *parameter);
                }
            } else {
                self.bindings = self.bindings_for_block(&self.bindings, captures, continuation);
            }
        }
        Ok(())
    }

    fn finish_if_else_arm(
        &mut self,
        terminal: bool,
        continuation: Option<BlockId>,
        captures: &[VariableId],
        name: &str,
    ) -> Result<(), String> {
        match (terminal, self.current_block) {
            (true, None) => Ok(()),
            (true, Some(_)) => Err(format!("terminal {name} arm does not terminate")),
            (false, None) => Err(format!("continuing {name} arm terminates")),
            (false, Some(block)) => {
                let arguments = self.capture_values(captures)?;
                self.function.set_terminator(
                    block,
                    Terminator::jump_with_arguments(
                        continuation.expect("a continuing arm requires a continuation"),
                        arguments,
                    ),
                );
                Ok(())
            }
        }
    }

    fn lower_condition(&mut self, condition: &Condition) -> Result<ValueId, String> {
        match condition {
            Condition::LogicalNot(condition) => {
                let condition = self.lower_condition(condition)?;
                self.append_unary_value(ValueOperation::LogicalNot, condition, Type::Bool)
            }
            Condition::ScalarEquality { lhs, rhs, equal } => {
                let lhs = self.lower_value(lhs)?;
                let rhs = self.lower_value(rhs)?;
                self.append_integer_comparison(
                    if *equal {
                        IntegerComparisonOperation::Equal
                    } else {
                        IntegerComparisonOperation::NotEqual
                    },
                    vec![lhs, rhs],
                )
            }
            Condition::ScalarRelational {
                lhs,
                rhs,
                relation,
                signed,
            } => {
                let lhs = self.lower_value(lhs)?;
                let rhs = self.lower_value(rhs)?;
                let domain = if *signed {
                    ComparisonDomain::SignedInteger
                } else {
                    ComparisonDomain::UnsignedInteger
                };
                self.append_integer_comparison(
                    IntegerComparisonOperation::Relational {
                        relation: *relation,
                        domain,
                    },
                    vec![lhs, rhs],
                )
            }
            Condition::BitTest { value, mask, set } => {
                let value = self.lower_value(value)?;
                let mask = self.lower_value(mask)?;
                self.append_integer_comparison(
                    if *set {
                        IntegerComparisonOperation::BitsSet
                    } else {
                        IntegerComparisonOperation::BitsClear
                    },
                    vec![value, mask],
                )
            }
            Condition::ValueTag {
                value,
                tag,
                source_tag,
                mask,
                expected_tag,
                matches_when_equal,
                matches,
            } => {
                let raw_tag = if let Some(source_tag) = source_tag {
                    self.binding(*source_tag)?
                } else {
                    let value = self.lower_value(value)?;
                    self.extract_tag(value, false)?
                };
                let compared_tag = if let Some(mask) = mask {
                    let mask = match mask {
                        TagMask::Immediate(mask) => {
                            self.function.add_constant(Type::ValueTag, Constant::Integer(*mask))
                        }
                        TagMask::Constant(mask) => self
                            .function
                            .add_constant(Type::ValueTag, Constant::symbol(mask.clone())),
                    };
                    self.append_pure_operation(
                        Operation::Intrinsic(Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(
                            BinaryOperation::And,
                        ))),
                        vec![raw_tag, mask],
                        Type::ValueTag,
                    )?
                } else {
                    raw_tag
                };
                self.bindings.insert(*tag, compared_tag);
                let expected = self
                    .function
                    .add_constant(Type::ValueTag, Constant::symbol(expected_tag.clone()));
                let equal = *matches == *matches_when_equal;
                self.append_integer_comparison(
                    if equal {
                        IntegerComparisonOperation::Equal
                    } else {
                        IntegerComparisonOperation::NotEqual
                    },
                    vec![compared_tag, expected],
                )
            }
            Condition::ValueBits {
                value,
                expected,
                expected_bits,
                matches,
            } => {
                let value = self.lower_value(value)?;
                let expected_value = self.function.add_constant(
                    value_type(&self.function, value)?,
                    Constant::symbol(expected_bits.clone()),
                );
                self.bindings.insert(*expected, expected_value);
                self.append_integer_comparison(
                    if *matches {
                        IntegerComparisonOperation::Equal
                    } else {
                        IntegerComparisonOperation::NotEqual
                    },
                    vec![value, expected_value],
                )
            }
        }
    }

    fn append_unary_value(
        &mut self,
        operation: ValueOperation,
        input: ValueId,
        result_type: Type,
    ) -> Result<ValueId, String> {
        self.append_pure_operation(Intrinsic::Value(operation), vec![input], result_type)
    }

    fn extract_tag(&mut self, value: ValueId, rematerialized: bool) -> Result<ValueId, String> {
        self.append_unary_value(ValueOperation::ExtractTag { rematerialized }, value, Type::ValueTag)
    }

    fn unbox_int32(&mut self, value: ValueId, rematerialized: bool) -> Result<ValueId, String> {
        self.append_unary_value(ValueOperation::UnboxInt32 { rematerialized }, value, Type::I32)
    }

    fn unbox_float64(&mut self, value: ValueId) -> Result<ValueId, String> {
        self.append_unary_value(ValueOperation::ValueToFloat64, value, Type::F64)
    }

    fn unbox_boolean(&mut self, value: ValueId) -> Result<ValueId, String> {
        self.append_unary_value(ValueOperation::UnboxBoolean, value, Type::Bool)
    }

    fn append_classification(
        &mut self,
        operation: ClassificationOperation,
        inputs: Vec<ValueId>,
    ) -> Result<ValueId, String> {
        self.append_pure_operation(Intrinsic::Classification(operation), inputs, Type::Bool)
    }

    fn append_integer_comparison(
        &mut self,
        operation: IntegerComparisonOperation,
        inputs: Vec<ValueId>,
    ) -> Result<ValueId, String> {
        self.append_pure_operation(Intrinsic::IntegerComparison(operation), inputs, Type::Bool)
    }

    fn append_pure_operation(
        &mut self,
        operation: impl Into<Operation>,
        inputs: Vec<ValueId>,
        result_type: Type,
    ) -> Result<ValueId, String> {
        let block = self.current()?;
        Ok(self
            .function
            .append_instruction(block, operation, inputs, vec![result_type])[0])
    }

    fn lower_call(&mut self, call: &super::typecheck::Call, result_types: Vec<Type>) -> Result<Vec<ValueId>, String> {
        if call.arguments.len() != call.effects.len() {
            return Err(format!(
                "primitive '{}' has incomplete parameter effects",
                call.display_name()
            ));
        }
        let accesses_inout_operand = call.arguments.first().is_some_and(|argument| {
            matches!(argument.kind, ValueKind::Variable(variable)
                if self.body.variables[variable].ty == Type::Operand
                    && self.body.variables[variable].parameter_mode == Some(ParameterMode::InOut))
        });
        let mut inputs = Vec::new();
        let mut outputs = Vec::new();
        for (argument, mode) in call.arguments.iter().zip(&call.effects) {
            if call.branch_target.as_ref().is_some_and(|target| {
                matches!(&argument.kind, ValueKind::Label(label) if label == target)
                    || matches!(argument.kind, ValueKind::Variable(variable)
                        if self.body.variables[variable].name == *target
                            && self.body.variables[variable].ty == Type::label())
            }) {
                continue;
            }
            if matches!(mode, ParameterMode::In | ParameterMode::InOut)
                || *mode == ParameterMode::Out && argument.ty == Type::Operand
            {
                inputs.push(self.lower_value(argument)?);
            }
            if matches!(mode, ParameterMode::Out | ParameterMode::InOut) && argument.ty != Type::Operand {
                let ValueKind::Variable(variable) = argument.kind else {
                    return Err(format!(
                        "primitive '{}' writes to a non-binding argument",
                        call.display_name()
                    ));
                };
                outputs.push((variable, argument.ty.clone()));
            }
        }
        if call.intrinsic() == Some(Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field)))
            && accesses_inout_operand
        {
            let [operand] = inputs.as_slice() else {
                return Err("'load' requires one operand".to_string());
            };
            let [result_type] = result_types.as_slice() else {
                return Err("'load' requires one result".to_string());
            };
            let block = self.current()?;
            let index = self.function.append_instruction(
                block,
                Intrinsic::Operand(OperandOperation::Decode),
                vec![*operand],
                vec![Type::U32],
            )[0];
            let value = self.function.append_instruction(
                block,
                Intrinsic::Operand(OperandOperation::Load(OperandLoad::Index)),
                vec![index],
                vec![result_type.clone()],
            )[0];
            return Ok(vec![value]);
        }
        if call.intrinsic() == Some(Intrinsic::Operand(OperandOperation::Store(OperandStore::Field)))
            && accesses_inout_operand
        {
            let [operand, value] = inputs.as_slice() else {
                return Err("'store' requires an operand and a value".to_string());
            };
            let block = self.current()?;
            let index = self.function.append_instruction(
                block,
                Intrinsic::Operand(OperandOperation::Decode),
                vec![*operand],
                vec![Type::U32],
            )[0];
            self.function.append_instruction(
                block,
                Intrinsic::Operand(OperandOperation::Store(OperandStore::Index)),
                vec![index, *value],
                Vec::new(),
            );
            return Ok(Vec::new());
        }
        let explicit_result_count = result_types.len();
        let mut all_result_types = result_types;
        all_result_types.extend(outputs.iter().map(|(_, ty)| ty.clone()));
        let block = self.current()?;
        let operation = self.call_operation(&call.target)?;
        let effects = self.function.effects_for_operation(&operation, &inputs);
        let results = if let Some(failure) = &call.branch_target {
            let success_layout = if self.function.blocks[block.0].layout == BlockLayout::Cold {
                BlockLayout::Cold
            } else {
                BlockLayout::Hot
            };
            let success = self.function.create_named_block(
                format!("{}_success", call.display_name()),
                success_layout,
                all_result_types.clone(),
            );
            let failure = self.continuation_edge(failure)?;
            self.function.set_checked_operation(
                block,
                operation,
                inputs,
                all_result_types,
                effects,
                success,
                Vec::new(),
                failure,
            );
            self.current_block = Some(success);
            self.function.blocks[success.0].parameters.clone()
        } else {
            self.function
                .append_instruction(block, operation, inputs, all_result_types)
        };
        for ((variable, _), result) in outputs.iter().zip(&results[explicit_result_count..]) {
            self.bindings.insert(*variable, *result);
        }
        Ok(results[..explicit_result_count].to_vec())
    }

    fn call_operation(&self, target: &CallTarget) -> Result<Operation, String> {
        Ok(match target {
            CallTarget::Intrinsic(intrinsic) => Operation::from(*intrinsic),
            CallTarget::InlineFunction(id) => Operation::InlineCall(*id),
            CallTarget::OperationParameter(variable) => {
                let value = self.binding(*variable)?;
                let ValueDefinition::FunctionParameter(index) = self.function.values[value.0].definition else {
                    return Err(format!("operation variable {variable} is not a function parameter"));
                };
                Operation::Parameter(OperationParameterId(
                    u32::try_from(index).expect("Flap function parameter count fits in u32"),
                ))
            }
            CallTarget::FieldAccess(field_access) => Operation::FieldAccess(*field_access),
        })
    }

    fn lower_machine_assign(
        &mut self,
        destination: crate::types::InterpreterRegister,
        intrinsic: Intrinsic,
        arguments: &[Value],
    ) -> Result<(), String> {
        let inputs = arguments
            .iter()
            .map(|argument| self.lower_value(argument))
            .collect::<Result<Vec<_>, _>>()?;
        let block = self.current()?;
        self.function.append_instruction(
            block,
            Operation::MachineAssign { destination, intrinsic },
            inputs,
            Vec::new(),
        );
        Ok(())
    }

    fn lower_value(&mut self, value: &Value) -> Result<ValueId, String> {
        match &value.kind {
            ValueKind::Variable(variable) => self.binding(*variable),
            ValueKind::Integer(integer) => Ok(self
                .function
                .add_constant(value.ty.clone(), Constant::Integer(*integer))),
            ValueKind::LayoutValue(layout_value) => Ok(self
                .function
                .add_constant(value.ty.clone(), Constant::layout_value(*layout_value))),
            ValueKind::Constant(name) => Ok(self
                .function
                .add_constant(value.ty.clone(), Constant::symbol(name.clone()))),
            ValueKind::Label(name) => {
                let target = self.continuation_edge(name)?;
                let block = self.current()?;
                Ok(self.function.append_instruction(
                    block,
                    Operation::BlockReference(target.block),
                    target.arguments,
                    vec![Type::label()],
                )[0])
            }
            ValueKind::SlowPath(name) => Ok(self
                .function
                .add_constant(value.ty.clone(), Constant::SlowPath(name.clone()))),
            ValueKind::FunctionSymbol(name) => Ok(self
                .function
                .add_constant(value.ty.clone(), Constant::FunctionSymbol(name.clone()))),
            ValueKind::Operation(operation) => Ok(self
                .function
                .add_constant(value.ty.clone(), Constant::Operation(operation.clone()))),
            ValueKind::MachineRegister(name) => Ok(self
                .function
                .add_constant(value.ty.clone(), Constant::MachineRegister(name.clone()))),
            ValueKind::Memory(address) => {
                let block = self.current()?;
                let address = address
                    .iter()
                    .map(|component| self.lower_value(component))
                    .collect::<Result<Vec<_>, _>>()?;
                Ok(self
                    .function
                    .append_instruction(block, Operation::Address, address, vec![value.ty.clone()])[0])
            }
        }
    }

    fn binding(&self, variable: VariableId) -> Result<ValueId, String> {
        self.bindings.get(&variable).copied().ok_or_else(|| {
            format!(
                "handler '{}' uses variable {} before its SSA definition",
                self.function.name, variable
            )
        })
    }

    fn current(&self) -> Result<BlockId, String> {
        self.current_block
            .ok_or_else(|| format!("handler '{}' has already terminated", self.function.name))
    }

    fn variable_type(&self, variable: VariableId) -> Result<Type, String> {
        self.body
            .variables
            .get(variable)
            .map(|value| value.ty.clone())
            .ok_or_else(|| format!("variable {variable} has no type"))
    }

    fn capture_types_and_values(&self, captures: &[VariableId]) -> Result<(Vec<Type>, Vec<ValueId>), String> {
        Ok((
            captures
                .iter()
                .map(|capture| self.variable_type(*capture))
                .collect::<Result<Vec<_>, _>>()?,
            self.capture_values(captures)?,
        ))
    }

    fn capture_values(&self, captures: &[VariableId]) -> Result<Vec<ValueId>, String> {
        captures.iter().map(|capture| self.binding(*capture)).collect()
    }

    fn bindings_for_block(
        &self,
        outer: &HashMap<VariableId, ValueId>,
        captures: &[VariableId],
        block: BlockId,
    ) -> HashMap<VariableId, ValueId> {
        let mut bindings = outer.clone();
        for (capture, parameter) in captures.iter().zip(&self.function.blocks[block.0].parameters) {
            bindings.insert(*capture, *parameter);
        }
        bindings
    }
}

fn value_type(function: &Function, value: ValueId) -> Result<Type, String> {
    function
        .values
        .get(value.0)
        .map(|value| value.ty.clone())
        .ok_or_else(|| format!("value {value:?} has no type"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frontend::parser;
    use crate::hir as typecheck;

    #[test]
    fn attributes_lowering_errors_to_the_ssa_function() {
        let ast = parser::parse(
            "test.flap",
            r#"
handler Invalid() {
    let done = || { dispatch_next; };
    goto done();
}
"#,
        )
        .unwrap();
        let mut typed = typecheck::check("test.flap", &ast).unwrap();
        let duplicate = typed.handlers[0].body.statements[0].clone();
        typed.handlers[0].body.statements.insert(1, duplicate);

        let error = lower_handler(&typed.handlers[0]).unwrap_err();
        assert_eq!(error.stage, CompileStage::Ssa);
        assert_eq!(error.handler.as_deref(), Some("Invalid"));
        assert!(error.message.contains("duplicate continuation"));
    }

    fn lower_source(source: &str) -> Function {
        let ast = parser::parse("test.flap", source).unwrap();
        let typed = typecheck::check("test.flap", &ast).unwrap();
        lower_handler(&typed.handlers[0]).unwrap()
    }

    #[test]
    fn lowers_a_guard_into_the_block_it_continues() {
        let function = lower_source(
            r#"
handler Check(value: i32) {
    guard value != 0 else slow;
    assert_nonzero(value);
    let slow = || { dispatch_next; };
    dispatch_next;
}
"#,
        );

        let entry = &function.blocks[function.entry.0];
        let guards = function.guard_exits(function.entry).collect::<Vec<_>>();
        assert_eq!(guards.len(), 1);
        assert!(guards[0].0 < entry.instructions.len() - 1);
        assert!(
            !function
                .blocks
                .iter()
                .any(|block| block.name.as_deref() == Some("guard_success"))
        );
        function.validate().unwrap();
    }

    #[test]
    fn keeps_a_branch_for_a_guard_that_hands_over_values() {
        let function = lower_source(
            r#"
handler Check(value: i32) {
    let slow = |kept: i32| { assert_nonzero(kept); dispatch_next; };
    guard value != 0 else slow(value);
    dispatch_next;
}
"#,
        );

        assert_eq!(function.guard_exits(function.entry).count(), 0);
        assert!(matches!(
            function.blocks[function.entry.0].terminator,
            Some(Terminator::Branch { .. })
        ));
        function.validate().unwrap();
    }
}
