/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Typed high-level IR construction and semantic analysis.

mod capture;
mod definite_initialization;
mod definitions;
mod intrinsic;

pub(crate) use definitions::*;

use crate::frontend::ast;
use crate::frontend::ast::{BinaryOperator, ExpressionKind, ParameterMode, StatementKind, UnaryOperator};
use crate::frontend::diagnostic::{Diagnostic, SourceSpan};
use crate::frontend::layout;
use crate::hash::{HashMap, HashSet};
use crate::identity::{ExternalSymbol, InlineFunctionId};
use crate::intrinsic::{
    AddressOperation, AggregateOperation, BinaryOperation, BytecodeOperation, CallOperation, ComparisonRelation,
    ControlOperation, FieldAccess, FieldAccessKind, FieldPair, FieldWidth, FloatBinaryOperation,
    FloatingPointOperation, IntegerBinaryOperation, IntegerComparisonOperation, Intrinsic, LowLevelOperation,
    MemoryOperation, OperandLoad, OperandOperation, OperationValue, ShiftOperation, ValueOperation,
};
use crate::types::{BlockTemperature, InterpreterRegister, Type};
use capture::{collect_captures, collect_input_captures};
use definite_initialization::check_definite_initialization;
use intrinsic::{
    Signature, intrinsic_is_terminal, operation_signature, resolve_intrinsic, resolve_operation, signature,
    signatures_match,
};

#[derive(Clone)]
struct Symbol {
    id: VariableId,
    ty: Type,
    parameter_mode: Option<ParameterMode>,
}

#[derive(Clone)]
struct ContinuationParameter {
    name: String,
    id: VariableId,
    ty: Type,
}

#[derive(Clone)]
struct ContinuationSymbol {
    lowered_name: String,
    temperature: BlockTemperature,
    parameters: Vec<ContinuationParameter>,
    ty: Type,
}

struct TypedElseContinuation {
    temperature: BlockTemperature,
    captures: Vec<VariableId>,
    body: Vec<Statement>,
}

#[derive(Clone)]
struct TypeFacts {
    known_null_pointers: HashSet<VariableId>,
    known_integer_literals: HashSet<VariableId>,
}

struct Checker<'a> {
    filename: &'a str,
    signatures: &'a HashMap<String, Signature>,
    inline_function_ids: &'a HashMap<String, InlineFunctionId>,
    variables: Vec<Variable>,
    scopes: Vec<HashMap<String, Symbol>>,
    continuation_scopes: Vec<HashMap<String, ContinuationSymbol>>,
    next_continuation_scope: u64,
    statements: Vec<Statement>,
    layouts: &'a HashMap<(Type, String), LayoutField>,
    aggregate_strides: &'a HashMap<Type, layout::LayoutValue>,
    bytecode_fields: HashSet<VariableId>,
    known_null_pointers: HashSet<VariableId>,
    known_integer_literals: HashSet<VariableId>,
    active_value_tags: Vec<(VariableId, VariableId)>,
}

#[derive(Clone)]
struct LayoutField {
    ty: Type,
    offset: layout::LayoutValue,
    nonnull: bool,
    embedded: bool,
    cell_pointer: bool,
    pair: Option<FieldPair>,
    pinned_base: Option<String>,
    pinned_offset: Option<layout::LayoutValue>,
}

impl<'a> Checker<'a> {
    fn new(
        filename: &'a str,
        signatures: &'a HashMap<String, Signature>,
        inline_function_ids: &'a HashMap<String, InlineFunctionId>,
        layouts: &'a HashMap<(Type, String), LayoutField>,
        aggregate_strides: &'a HashMap<Type, layout::LayoutValue>,
    ) -> Self {
        Self {
            filename,
            signatures,
            inline_function_ids,
            variables: Vec::new(),
            scopes: vec![HashMap::default()],
            continuation_scopes: Vec::new(),
            next_continuation_scope: 0,
            statements: Vec::new(),
            layouts,
            aggregate_strides,
            bytecode_fields: HashSet::default(),
            known_null_pointers: HashSet::default(),
            known_integer_literals: HashSet::default(),
            active_value_tags: Vec::new(),
        }
    }

    fn error<T>(&self, span: SourceSpan, message: impl Into<String>) -> Result<T, Diagnostic> {
        Err(Diagnostic::new(self.filename, span, message))
    }

    fn inline_function_id(&self, name: &str, span: SourceSpan) -> Result<InlineFunctionId, Diagnostic> {
        self.inline_function_ids
            .get(name)
            .copied()
            .ok_or_else(|| Diagnostic::new(self.filename, span, format!("missing inline function '{name}'")))
    }

    fn lookup(&self, name: &str) -> Option<&Symbol> {
        self.scopes.iter().rev().find_map(|scope| scope.get(name))
    }

    fn type_facts(&self) -> TypeFacts {
        TypeFacts {
            known_null_pointers: self.known_null_pointers.clone(),
            known_integer_literals: self.known_integer_literals.clone(),
        }
    }

    fn restore_type_facts(&mut self, facts: TypeFacts) {
        self.known_null_pointers = facts.known_null_pointers;
        self.known_integer_literals = facts.known_integer_literals;
    }

    fn declare(
        &mut self,
        name: &str,
        ty: Type,
        parameter_mode: Option<ParameterMode>,
        span: SourceSpan,
    ) -> Result<VariableId, Diagnostic> {
        if self.scopes.last().unwrap().contains_key(name) {
            return self.error(span, format!("duplicate binding '{name}' in the same scope"));
        }
        let id = self.variables.len();
        self.variables.push(Variable {
            name: name.to_string(),
            ty: ty.clone(),
            parameter_mode,
            span,
        });
        self.scopes
            .last_mut()
            .unwrap()
            .insert(name.to_string(), Symbol { id, ty, parameter_mode });
        Ok(id)
    }

    fn collect_continuations(&mut self, block: &ast::Block) -> Result<HashMap<String, ContinuationSymbol>, Diagnostic> {
        let scope_id = self.next_continuation_scope;
        self.next_continuation_scope += 1;
        let mut continuations = HashMap::default();
        for statement in &block.statements {
            if let StatementKind::Continuation {
                name,
                parameters,
                temperature,
                ..
            } = &statement.kind
            {
                let mut names = HashSet::default();
                let mut continuation_parameters = Vec::with_capacity(parameters.len());
                for parameter in parameters {
                    if !names.insert(parameter.name.clone()) {
                        return self.error(
                            parameter.span,
                            format!("duplicate continuation parameter '{}'", parameter.name),
                        );
                    }
                    continuation_parameters.push(ContinuationParameter {
                        name: parameter.name.clone(),
                        id: self.declare_hidden(&parameter.name, parameter.ty.clone(), parameter.span),
                        ty: parameter.ty.clone(),
                    });
                }
                if continuations
                    .insert(
                        name.clone(),
                        ContinuationSymbol {
                            lowered_name: format!(".{}_s{scope_id}", name.trim_start_matches('.')),
                            temperature: *temperature,
                            ty: Type::Continuation(
                                continuation_parameters
                                    .iter()
                                    .map(|parameter| parameter.ty.clone())
                                    .collect(),
                            ),
                            parameters: continuation_parameters,
                        },
                    )
                    .is_some()
                {
                    return self.error(statement.span, format!("duplicate label '{name}'"));
                }
            }
        }
        Ok(continuations)
    }

    fn check_block(&mut self, block: &ast::Block, create_scope: bool) -> Result<(), Diagnostic> {
        if create_scope {
            self.scopes.push(HashMap::default());
        }
        let continuations = self.collect_continuations(block)?;
        self.continuation_scopes.push(continuations);
        for statement in &block.statements {
            self.check_statement(statement)?;
        }
        self.continuation_scopes.pop();
        if create_scope {
            self.scopes.pop();
        }
        Ok(())
    }

    fn check_scoped_block(&mut self, block: &ast::Block) -> Result<Vec<Statement>, Diagnostic> {
        let facts = self.type_facts();
        self.scopes.push(HashMap::default());
        let start = self.statements.len();
        let result = self.check_block(block, false);
        let statements = self.statements.split_off(start);
        self.scopes.pop();
        self.restore_type_facts(facts);
        result?;
        Ok(statements)
    }

    fn check_else_continuation(
        &mut self,
        continuation: &ast::ElseContinuation,
        span: SourceSpan,
    ) -> Result<TypedElseContinuation, Diagnostic> {
        let outer_variable_count = self.variables.len();
        match continuation {
            ast::ElseContinuation::Label(label) => {
                let target = self.check_value(
                    &ast::Expression {
                        kind: ExpressionKind::Name(label.clone()),
                        span,
                    },
                    &Type::label(),
                )?;
                let branch_target = match &target.kind {
                    ValueKind::Label(label) => Some(label.clone()),
                    _ => None,
                };
                Ok(TypedElseContinuation {
                    temperature: BlockTemperature::Default,
                    captures: Vec::new(),
                    body: vec![Statement::new(
                        StatementKindIr::call(
                            [],
                            Call::with_control_flow(
                                CallTarget::Intrinsic(Intrinsic::Control(ControlOperation::JumpLabel)),
                                vec![target],
                                vec![ParameterMode::In],
                                None,
                                true,
                                branch_target,
                            ),
                        ),
                        span,
                    )],
                })
            }
            ast::ElseContinuation::Invocation { target, arguments } => {
                let (_, temperature, body) = self.check_continuation_invocation(target, arguments, span)?;
                let captures = collect_input_captures(&body, outer_variable_count);
                Ok(TypedElseContinuation {
                    temperature,
                    captures,
                    body,
                })
            }
            ast::ElseContinuation::Block { temperature, body } => {
                let body = self.check_scoped_block(body)?;
                if !body.last().is_some_and(statement_is_terminal) {
                    return self.error(span, "an else continuation must terminate control flow");
                }
                let captures = collect_input_captures(&body, outer_variable_count);
                Ok(TypedElseContinuation {
                    temperature: *temperature,
                    captures,
                    body,
                })
            }
        }
    }

    fn materialize_else_continuation(
        &mut self,
        continuation: TypedElseContinuation,
        name: &str,
        span: SourceSpan,
    ) -> Value {
        if let [
            Statement {
                kind: StatementKindIr::Call(destinations, call),
                ..
            },
        ] = continuation.body.as_slice()
            && destinations.is_empty()
            && call.terminal
            && call.intrinsic() == Some(Intrinsic::Control(ControlOperation::JumpLabel))
        {
            return call.arguments[0].clone();
        }
        let label = format!(".else_{name}_{}", self.statements.len());
        self.statements.push(Statement::new(
            StatementKindIr::Continuation {
                name: label.clone(),
                temperature: continuation.temperature,
                parameters: Vec::new(),
                captures: continuation.captures,
                body: continuation.body,
            },
            span,
        ));
        Value::new(ValueKind::Label(label), Type::label(), span)
    }

    fn declare_tuple_destinations(
        &mut self,
        patterns: &[ast::Pattern],
        element_types: Vec<Type>,
        span: SourceSpan,
    ) -> Result<Vec<VariableId>, Diagnostic> {
        patterns
            .iter()
            .zip(element_types)
            .map(|(pattern, ty)| match pattern {
                ast::Pattern::Binding { name, ty: None } => self.declare(name, ty, None, span),
                ast::Pattern::Wildcard => Ok(self.declare_hidden("discard", ty, span)),
                _ => self.error(span, "invalid tuple binding pattern"),
            })
            .collect()
    }

    fn check_value_match_fallback(
        &mut self,
        fallback: &ast::ValueMatchFallback,
    ) -> Result<ValueMatchFallbackIr, Diagnostic> {
        Ok(ValueMatchFallbackIr {
            body: self.check_scoped_block(&fallback.body)?,
            temperature: fallback.temperature,
        })
    }

    fn check_bound_block(
        &mut self,
        binding: Option<&str>,
        ty: Type,
        block: &ast::Block,
        span: SourceSpan,
    ) -> Result<(Option<VariableId>, Vec<Statement>), Diagnostic> {
        let facts = self.type_facts();
        self.scopes.push(HashMap::default());
        let binding = binding
            .map(|binding| self.declare(binding, ty, None, span))
            .transpose()?;
        let start = self.statements.len();
        let result = self.check_block(block, false);
        let statements = self.statements.split_off(start);
        self.scopes.pop();
        self.restore_type_facts(facts);
        result?;
        Ok((binding, statements))
    }

    fn check_bound_value_block(
        &mut self,
        binding: Option<&str>,
        binding_type: Type,
        block: &ast::Block,
        expected: Option<&Type>,
        span: SourceSpan,
    ) -> Result<(Option<VariableId>, Vec<Statement>, Call), Diagnostic> {
        let facts = self.type_facts();
        self.scopes.push(HashMap::default());
        let binding = binding
            .map(|binding| self.declare(binding, binding_type, None, span))
            .transpose()?;
        let result = self.check_value_block(block, expected);
        self.scopes.pop();
        self.restore_type_facts(facts);
        result.map(|(statements, value)| (binding, statements, value))
    }

    fn check_guard_continuation(
        &mut self,
        continuation: &ast::ElseContinuation,
        span: SourceSpan,
    ) -> Result<String, Diagnostic> {
        if let ast::ElseContinuation::Label(label) = continuation {
            return self.check_label(label, span);
        }
        let continuation = self.check_else_continuation(continuation, span)?;
        let ValueKind::Label(label) = self.materialize_else_continuation(continuation, "guard", span).kind else {
            unreachable!()
        };
        Ok(label)
    }

    fn check_value_match_statement(
        &mut self,
        statement: &ast::Statement,
        name: &Option<String>,
        value: &ast::Expression,
        arms: &[ast::ValueMatchArm],
        fallback: &ast::ValueMatchFallback,
    ) -> Result<(), Diagnostic> {
        let value = self.check_materialized_value(value, &Type::Value)?;
        let outer_variable_count = self.variables.len();
        let tag = if name.is_none() {
            Some(self.declare_hidden("match_tag", Type::ValueTag, statement.span))
        } else {
            None
        };
        let active_value_tag = if let (Some(tag), ValueKind::Variable(value)) = (tag, &value.kind) {
            self.active_value_tags.push((*value, tag));
            true
        } else {
            false
        };
        let mut result_type = None;
        let mut checked_arms = Vec::with_capacity(arms.len());
        let mut kinds = Vec::with_capacity(arms.len());
        for arm in arms {
            let (kind, binding_type) = match arm.representation.as_str() {
                "i32" => (ValueMatchArmKind::Int32, Type::I32),
                "f64" => (ValueMatchArmKind::Double, Type::F64),
                "bool" => (ValueMatchArmKind::Boolean, Type::Bool),
                representation => {
                    return self.error(
                        statement.span,
                        format!("unknown Value match representation '{representation}'"),
                    );
                }
            };
            if kinds.contains(&kind) {
                return self.error(
                    statement.span,
                    format!("duplicate Value<{}> match arm", arm.representation),
                );
            }
            kinds.push(kind);
            let (binding, mut body, value) = if name.is_some() {
                let (binding, body, value) = self.check_bound_value_block(
                    arm.binding.as_deref(),
                    binding_type,
                    &arm.body,
                    result_type.as_ref(),
                    statement.span,
                )?;
                let arm_type = value.return_type.clone().ok_or_else(|| {
                    Diagnostic::new(
                        self.filename,
                        statement.span,
                        format!("Value<{}> match arm does not produce a value", arm.representation),
                    )
                })?;
                if let Some(expected) = &result_type
                    && *expected != arm_type
                {
                    return self.error(
                        statement.span,
                        format!("value-producing match arms produce {expected} and {arm_type}"),
                    );
                }
                result_type = Some(arm_type);
                (binding, body, Some(value))
            } else {
                let (binding, body) =
                    self.check_bound_block(arm.binding.as_deref(), binding_type, &arm.body, statement.span)?;
                if !body.last().is_some_and(statement_is_terminal) {
                    return self.error(statement.span, "every match arm must terminate control flow");
                }
                (binding, body, None)
            };
            checked_arms.push((
                ValueMatchArm {
                    kind,
                    binding,
                    layout: arm.temperature,
                    body: std::mem::take(&mut body),
                },
                value,
            ));
        }
        if !kinds.contains(&ValueMatchArmKind::Int32) {
            return self.error(statement.span, "Value match requires an i32 arm");
        }
        if name.is_some() && !kinds.contains(&ValueMatchArmKind::Double) {
            return self.error(statement.span, "value-producing Value match requires an f64 arm");
        }
        let fallback = self.check_value_match_fallback(fallback)?;
        if !fallback.body.last().is_some_and(statement_is_terminal) {
            return self.error(
                statement.span,
                if name.is_some() {
                    "value match fallback must terminate control flow"
                } else {
                    "every match arm must terminate control flow"
                },
            );
        }
        let destination = if let Some(name) = name {
            let result_type = result_type
                .ok_or_else(|| Diagnostic::new(self.filename, statement.span, "value-producing match has no arms"))?;
            let destination = self.declare(name, result_type, None, statement.span)?;
            for (arm, value) in &mut checked_arms {
                arm.body.push(Statement::new(
                    StatementKindIr::call([destination], value.take().expect("value-producing arm has a value")),
                    statement.span,
                ));
            }
            Some(destination)
        } else {
            None
        };
        if active_value_tag {
            self.active_value_tags.pop();
        }
        let arms = checked_arms.into_iter().map(|(arm, _)| arm).collect::<Vec<_>>();
        let mut captures = if destination.is_some() {
            arms.iter()
                .flat_map(|arm| collect_captures(&arm.body, outer_variable_count))
                .collect::<Vec<_>>()
        } else {
            Vec::new()
        };
        if destination.is_some() {
            captures.extend(collect_captures(&fallback.body, outer_variable_count));
        }
        captures.sort_unstable();
        captures.dedup();
        self.statements.push(Statement::new(
            StatementKindIr::ValueMatch {
                destination,
                value,
                tag,
                captures,
                arms,
                fallback,
            },
            statement.span,
        ));
        Ok(())
    }

    fn check_if_statement(&mut self, statement: &ast::Statement) -> Result<(), Diagnostic> {
        let ast::StatementKind::If {
            name,
            ty,
            condition,
            temperature,
            then_body,
            else_body,
        } = &statement.kind
        else {
            unreachable!()
        };
        let mut condition = self.check_condition(condition)?;
        let outer_variable_count = self.variables.len();
        let Some(else_body) = else_body else {
            let then_body = self.check_scoped_block(then_body)?;
            let terminal = then_body.last().is_some_and(statement_is_terminal);
            if *temperature == BlockTemperature::Cold && !terminal {
                return self.error(statement.span, "a continuing if cannot be @cold");
            }
            let captures = collect_captures(&then_body, outer_variable_count);
            self.statements.push(Statement::new(
                StatementKindIr::If {
                    destination: None,
                    condition,
                    temperature: *temperature,
                    terminal,
                    captures,
                    then_body,
                    else_body: None,
                },
                statement.span,
            ));
            return Ok(());
        };
        let (destination, then_body, else_body) =
            if let Some(name) = name {
                let (mut then_body, then_value) = self.check_value_block(then_body, ty.as_ref())?;
                let (mut else_body, else_value) = self.check_value_block(else_body, ty.as_ref())?;
                let then_type = then_value.return_type.clone().ok_or_else(|| {
                    Diagnostic::new(self.filename, statement.span, "then arm does not produce a value")
                })?;
                let else_type = else_value.return_type.clone().ok_or_else(|| {
                    Diagnostic::new(self.filename, statement.span, "else arm does not produce a value")
                })?;
                if then_type != else_type {
                    return self.error(
                        statement.span,
                        format!("value-producing if arms produce {then_type} and {else_type}"),
                    );
                }
                if let Some(expected) = ty
                    && *expected != then_type
                {
                    return self.error(
                        statement.span,
                        format!("value-producing if produces {then_type}, not {expected}"),
                    );
                }
                let destination = self.declare(name, then_type, None, statement.span)?;
                then_body.push(Statement::new(
                    StatementKindIr::call([destination], then_value),
                    statement.span,
                ));
                else_body.push(Statement::new(
                    StatementKindIr::call([destination], else_value),
                    statement.span,
                ));
                (Some(destination), then_body, else_body)
            } else {
                let active_value_tag = match &condition {
                    Condition::ValueTag {
                        value:
                            Value {
                                kind: ValueKind::Variable(value),
                                ..
                            },
                        tag,
                        ..
                    } => {
                        self.active_value_tags.push((*value, *tag));
                        true
                    }
                    _ => false,
                };
                let then_body = self.check_scoped_block(then_body)?;
                let else_body = self.check_scoped_block(else_body)?;
                if active_value_tag {
                    self.active_value_tags.pop();
                }
                (None, then_body, else_body)
            };
        let mut captures = collect_captures(&then_body, outer_variable_count);
        captures.extend(collect_captures(&else_body, outer_variable_count));
        captures.sort_unstable();
        captures.dedup();
        if destination.is_none()
            && let Condition::ValueTag {
                value,
                tag,
                source_tag,
                mask: Some(_),
                ..
            } = &mut condition
            && source_tag.is_none()
            && captures.contains(tag)
        {
            let raw_tag = *tag;
            let compared_tag = self.declare_hidden("if_else_compared_tag", Type::ValueTag, statement.span);
            self.statements.push(Statement::new(
                StatementKindIr::call(
                    [],
                    Call::new(
                        CallTarget::Intrinsic(Intrinsic::Value(ValueOperation::ExtractTag { rematerialized: false })),
                        vec![
                            Value::new(ValueKind::Variable(raw_tag), Type::ValueTag, statement.span),
                            value.clone(),
                        ],
                        vec![ParameterMode::Out, ParameterMode::In],
                        None,
                    ),
                ),
                statement.span,
            ));
            *tag = compared_tag;
            *source_tag = Some(raw_tag);
        }
        self.statements.push(Statement::new(
            StatementKindIr::If {
                destination,
                condition,
                temperature: BlockTemperature::Default,
                terminal: false,
                captures,
                then_body,
                else_body: Some(else_body),
            },
            statement.span,
        ));
        Ok(())
    }

    fn check_value_refinement_statement(
        &mut self,
        statement: &ast::Statement,
        representation: &str,
        binding: &Option<Box<ast::Pattern>>,
        value: &ast::Expression,
        failure: &ast::ElseContinuation,
    ) -> Result<(), Diagnostic> {
        let Some(ast::Pattern::Binding {
            name: binding,
            ty: None,
        }) = binding.as_deref()
        else {
            return self.error(statement.span, "Value refinement requires an untyped binding");
        };
        let (binding_type, expected_tag, payload_operation, hidden_tag_name, reuse_active_tag) = match representation {
            "i32" => (
                Type::I32,
                Some("INT32_TAG"),
                ValueOperation::UnboxInt32 { rematerialized: false },
                "int32_tag",
                false,
            ),
            "f64" => (Type::F64, None, ValueOperation::ValueToFloat64, "double_tag", false),
            "Object" => (
                Type::Object,
                Some("OBJECT_TAG"),
                ValueOperation::UnboxObject,
                "pointer_tag",
                true,
            ),
            "PrimitiveString" => (
                Type::PrimitiveString,
                Some("STRING_TAG"),
                ValueOperation::UnboxObject,
                "pointer_tag",
                true,
            ),
            _ => unreachable!(),
        };
        let binding = if let Some(binding_symbol) = self.lookup(binding).cloned() {
            if binding_symbol.ty != binding_type {
                return self.error(
                    statement.span,
                    format!("Value refinement requires {binding_type}, found {}", binding_symbol.ty),
                );
            }
            self.ensure_writable(&binding_symbol, statement.span)?;
            binding_symbol.id
        } else {
            self.declare(binding, binding_type, None, statement.span)?
        };
        let value = self.check_materialized_value(value, &Type::Value)?;
        let failure = self.check_guard_continuation(failure, statement.span)?;
        let tag = if reuse_active_tag && let Some(tag) = self.lookup("tag").filter(|tag| tag.ty == Type::ValueTag) {
            tag.id
        } else {
            self.declare_hidden(hidden_tag_name, Type::ValueTag, statement.span)
        };
        self.statements.push(Statement::new(
            StatementKindIr::ValueRefinement {
                binding,
                value,
                tag,
                expected_tag: expected_tag.map(str::to_string),
                payload_operation,
                failure,
            },
            statement.span,
        ));
        Ok(())
    }

    fn check_assign_statement(
        &mut self,
        statement: &ast::Statement,
        name: &str,
        initializer: &ast::Expression,
    ) -> Result<(), Diagnostic> {
        if name.contains('.') {
            let call = self.check_field_store(name, initializer, statement.span)?;
            self.statements
                .push(Statement::new(StatementKindIr::call([], call), statement.span));
            return Ok(());
        }
        let Some(symbol) = self.lookup(name).cloned() else {
            let Some(machine_type) = machine_register_type(name) else {
                return self.error(statement.span, format!("unknown binding '{name}'"));
            };
            let call = self.check_initializer_with_expected(initializer, Some(&machine_type))?;
            let Some(return_type) = &call.return_type else {
                return self.error(
                    initializer.span,
                    format!("'{}' does not return a value", call.display_name()),
                );
            };
            let intrinsic = match call.target {
                CallTarget::Intrinsic(intrinsic @ Intrinsic::LowLevel(LowLevelOperation::Move))
                | CallTarget::Intrinsic(intrinsic @ Intrinsic::Address(AddressOperation::EmbeddedField)) => intrinsic,
                CallTarget::FieldAccess(field_access) => Intrinsic::Memory(field_access.kind.memory_operation()),
                _ => {
                    return self.error(
                        initializer.span,
                        "machine register assignment currently requires a binding or field projection",
                    );
                }
            };
            if return_type != &machine_type {
                return self.error(
                    initializer.span,
                    format!("cannot assign {return_type} to machine register '{name}' of type {machine_type}"),
                );
            }
            self.statements.push(Statement::new(
                StatementKindIr::MachineAssign {
                    destination: InterpreterRegister::from_name(name)
                        .expect("machine register type must have a register identity"),
                    intrinsic,
                    arguments: call.arguments,
                },
                statement.span,
            ));
            return Ok(());
        };
        self.ensure_writable(&symbol, statement.span)?;
        let call = self.check_initializer_with_expected(initializer, Some(&symbol.ty))?;
        let Some(return_type) = &call.return_type else {
            return self.error(
                initializer.span,
                format!("'{}' does not return a value", call.display_name()),
            );
        };
        if *return_type != symbol.ty {
            return self.error(
                initializer.span,
                format!("cannot assign {return_type} to '{name}' of type {}", symbol.ty),
            );
        }
        self.update_known_null_pointer(symbol.id, initializer, &symbol.ty);
        self.update_known_integer_literal(symbol.id, initializer, &symbol.ty);
        self.statements
            .push(Statement::new(StatementKindIr::call([symbol.id], call), statement.span));
        Ok(())
    }

    fn check_scalar_match_statement(
        &mut self,
        statement: &ast::Statement,
        value: &ast::Expression,
        arms: &[ast::ScalarMatchArm],
        fallback: &ast::Block,
    ) -> Result<(), Diagnostic> {
        let value = self.check_materialized_gpr_value(value)?;
        if !is_scalar_match_type(&value.ty) {
            return self.error(value.span, format!("cannot scalar-match {}", value.ty));
        }
        let value_type = value.ty.clone();
        let outer_variable_count = self.variables.len();
        let mut checked_arms = Vec::with_capacity(arms.len());
        let mut captures = Vec::new();
        let mut has_hot_arm = false;
        for arm in arms {
            if arm.temperature == BlockTemperature::Hot {
                if has_hot_arm {
                    return self.error(statement.span, "a scalar match can only have one @hot arm");
                }
                has_hot_arm = true;
            }
            let pattern = self.check_scalar_match_pattern(&arm.pattern, &value_type, statement.span)?;
            let body = self.check_scoped_block(&arm.body)?;
            if !body.last().is_some_and(statement_is_terminal) {
                return self.error(statement.span, "every scalar match block must terminate control flow");
            }
            captures.extend(collect_input_captures(&body, outer_variable_count));
            checked_arms.push(ScalarMatchArmIr {
                pattern,
                layout: arm.temperature,
                body,
            });
        }
        let body = self.check_scoped_block(fallback)?;
        if !body.last().is_some_and(statement_is_terminal) {
            return self.error(
                statement.span,
                "a scalar match fallback block must terminate control flow",
            );
        }
        captures.extend(collect_input_captures(&body, outer_variable_count));
        normalize_variables(&mut captures);
        self.statements.push(Statement::new(
            StatementKindIr::ScalarMatch {
                value,
                captures,
                arms: checked_arms,
                fallback: body,
            },
            statement.span,
        ));
        Ok(())
    }

    fn check_continuation_statement(
        &mut self,
        statement: &ast::Statement,
        name: &str,
        temperature: &BlockTemperature,
        body: &ast::Block,
    ) -> Result<(), Diagnostic> {
        let continuation = self.resolve_continuation_symbol(name).unwrap().clone();
        let name = continuation.lowered_name;
        let parameters = continuation.parameters;
        let parameter_ids = parameters.iter().map(|parameter| parameter.id).collect::<Vec<_>>();
        let scope = parameters
            .into_iter()
            .map(|parameter| {
                (
                    parameter.name,
                    Symbol {
                        id: parameter.id,
                        ty: parameter.ty,
                        parameter_mode: None,
                    },
                )
            })
            .collect();
        self.scopes.push(scope);
        let outer_variable_count = self.variables.len();
        let start = self.statements.len();
        self.check_block(body, false)?;
        let body = self.statements.split_off(start);
        self.scopes.pop();
        if !body.last().is_some_and(statement_is_terminal) {
            return self.error(statement.span, "a continuation body must terminate control flow");
        }
        let mut captures = collect_input_captures(&body, outer_variable_count);
        captures.retain(|capture| !parameter_ids.contains(capture));
        self.statements.push(Statement::new(
            StatementKindIr::Continuation {
                name,
                temperature: *temperature,
                parameters: parameter_ids,
                captures,
                body,
            },
            statement.span,
        ));
        Ok(())
    }

    fn check_guard_let_tuple_statement(
        &mut self,
        statement: &ast::Statement,
        patterns: &[ast::Pattern],
        callee: &str,
        arguments: &[ast::Expression],
        failure: &ast::ElseContinuation,
    ) -> Result<(), Diagnostic> {
        let failure = self.check_else_continuation(failure, statement.span)?;
        let failure = self.materialize_else_continuation(failure, callee, statement.span);
        let call = self.check_call(callee, arguments, Some(failure), statement.span)?;
        let Some(Type::Tuple(element_types)) = call.return_type.clone() else {
            return self.error(statement.span, format!("'{callee}' does not return a tuple"));
        };
        if patterns.len() != element_types.len() {
            return self.error(
                statement.span,
                format!(
                    "tuple binding has {} elements, but '{callee}' returns {}",
                    patterns.len(),
                    element_types.len()
                ),
            );
        }
        let destinations = self.declare_tuple_destinations(patterns, element_types, statement.span)?;
        self.statements.push(Statement::new(
            StatementKindIr::call(destinations, call),
            statement.span,
        ));
        Ok(())
    }

    fn check_guard_let_binding_statement(
        &mut self,
        statement: &ast::Statement,
        name: &str,
        ty: &Option<Type>,
        callee: &str,
        arguments: &[ast::Expression],
        failure: &ast::ElseContinuation,
    ) -> Result<(), Diagnostic> {
        let outer_variable_count = self.variables.len();
        let failure = self.check_else_continuation(failure, statement.span)?;
        let structured_failure = resolve_intrinsic(callee, arguments.len()).is_some_and(|resolved| {
            matches!(
                resolved.intrinsic,
                Intrinsic::CheckedInteger(operation)
                    if operation.unchecked().is_some()
            )
        });
        let (mut call, failure) = if structured_failure {
            let target = Value::new(
                ValueKind::Label(format!(".fallible_{callee}_{}", self.statements.len())),
                Type::label(),
                statement.span,
            );
            (
                self.check_call(callee, arguments, Some(target), statement.span)?,
                Some(failure),
            )
        } else {
            let target = self.materialize_else_continuation(failure, callee, statement.span);
            (self.check_call(callee, arguments, Some(target), statement.span)?, None)
        };
        let Some(return_type) = call.return_type.clone() else {
            return self.error(statement.span, format!("'{callee}' does not return a value"));
        };
        let destination_type = ty.clone().unwrap_or(return_type.clone());
        if return_type != destination_type {
            return self.error(
                statement.span,
                format!("cannot initialize '{name}' of type {destination_type} with {return_type}"),
            );
        }
        let destination = self.declare(name, destination_type, None, statement.span)?;
        if let Some(failure) = failure {
            call.failure = Some(CallFailure {
                captures: collect_captures(&failure.body, outer_variable_count),
                body: failure.body,
            });
        }
        self.statements.push(Statement::new(
            StatementKindIr::call([destination], call),
            statement.span,
        ));
        Ok(())
    }

    fn check_let_binding_statement(
        &mut self,
        statement: &ast::Statement,
        name: &str,
        ty: &Option<Type>,
        initializer: &Option<ast::Expression>,
    ) -> Result<(), Diagnostic> {
        if let Some(initializer) = initializer {
            let call = self.check_initializer_with_expected(initializer, ty.as_ref())?;
            let Some(return_type) = call.return_type.clone() else {
                return self.error(
                    initializer.span,
                    format!("'{}' does not return a value", call.display_name()),
                );
            };
            if matches!(return_type, Type::Tuple(_)) {
                return self.error(initializer.span, "a tuple return value must be destructured");
            }
            if ty.as_ref().is_some_and(|ty| &return_type != ty) {
                return self.error(
                    initializer.span,
                    format!(
                        "cannot initialize '{name}' of type {} with {return_type}",
                        ty.as_ref().unwrap()
                    ),
                );
            }
            let declared_type = ty.clone().unwrap_or(return_type);
            let id = self.declare(name, declared_type.clone(), None, statement.span)?;
            self.update_known_null_pointer(id, initializer, &declared_type);
            self.update_known_integer_literal(id, initializer, &declared_type);
            self.statements
                .push(Statement::new(StatementKindIr::call([id], call), statement.span));
        } else {
            self.declare(
                name,
                ty.clone()
                    .expect("parser rejects inferred declarations without initializers"),
                None,
                statement.span,
            )?;
        }
        Ok(())
    }

    fn check_let_tuple_statement(
        &mut self,
        statement: &ast::Statement,
        patterns: &[ast::Pattern],
        initializer: &ast::Expression,
    ) -> Result<(), Diagnostic> {
        let ExpressionKind::Call { callee, arguments } = &initializer.kind else {
            return self.error(initializer.span, "a tuple binding requires a function call");
        };
        let call = self.check_call(callee, arguments, None, initializer.span)?;
        let Some(Type::Tuple(element_types)) = call.return_type.clone() else {
            return self.error(initializer.span, format!("'{callee}' does not return a tuple"));
        };
        if patterns.len() != element_types.len() {
            return self.error(
                statement.span,
                format!(
                    "tuple binding has {} elements, but '{callee}' returns {}",
                    patterns.len(),
                    element_types.len()
                ),
            );
        }
        let destinations = self.declare_tuple_destinations(patterns, element_types, statement.span)?;
        self.statements.push(Statement::new(
            StatementKindIr::call(destinations, call),
            statement.span,
        ));
        Ok(())
    }

    fn check_statement(&mut self, statement: &ast::Statement) -> Result<(), Diagnostic> {
        match &statement.kind {
            StatementKind::Let {
                pattern: ast::Pattern::Binding { name, ty },
                initializer,
            } => self.check_let_binding_statement(statement, name, ty, initializer)?,
            StatementKind::Let {
                pattern: ast::Pattern::Tuple(patterns),
                initializer: Some(initializer),
            } => self.check_let_tuple_statement(statement, patterns, initializer)?,
            StatementKind::Let { .. } => return self.error(statement.span, "invalid binding pattern"),
            StatementKind::GuardLet {
                pattern: ast::Pattern::Tuple(patterns),
                initializer:
                    ast::Expression {
                        kind: ExpressionKind::Call { callee, arguments },
                        ..
                    },
                failure,
            } => self.check_guard_let_tuple_statement(statement, patterns, callee, arguments, failure)?,
            StatementKind::GuardLet {
                pattern: ast::Pattern::Binding { name, ty },
                initializer:
                    ast::Expression {
                        kind: ExpressionKind::Call { callee, arguments },
                        ..
                    },
                failure,
            } => self.check_guard_let_binding_statement(statement, name, ty, callee, arguments, failure)?,
            StatementKind::Assign { name, initializer } => self.check_assign_statement(statement, name, initializer)?,
            StatementKind::IndexAssign { base, index, value } => {
                let (base, element_type) = self.check_index_base(base)?;
                let index = self.check_sequence_index(index)?;
                let value = self.check_materialized_value(value, &element_type)?;
                let (_, store_operation, scale) = sequence_access(&element_type).ok_or_else(|| {
                    Diagnostic::new(
                        self.filename,
                        value.span,
                        format!("cannot store sequence element of type {element_type}"),
                    )
                })?;
                self.statements.push(Statement::new(
                    StatementKindIr::call(
                        [],
                        Call::new(
                            CallTarget::Intrinsic(Intrinsic::Memory(store_operation)),
                            vec![
                                Value::new(
                                    ValueKind::Memory(vec![
                                        base,
                                        index,
                                        Value::new(ValueKind::Integer(scale), Type::U8, statement.span),
                                    ]),
                                    Type::Memory,
                                    statement.span,
                                ),
                                value,
                            ],
                            vec![ParameterMode::In, ParameterMode::In],
                            None,
                        ),
                    ),
                    statement.span,
                ));
            }
            StatementKind::Expression(expression) => {
                let (callee, arguments) = match &expression.kind {
                    ExpressionKind::Call { callee, arguments } => (callee.as_str(), arguments.as_slice()),
                    ExpressionKind::Name(callee) => (callee.as_str(), &[][..]),
                    _ => return self.error(expression.span, "expected a call statement"),
                };
                let call = self.check_call(callee, arguments, None, expression.span)?;
                if call.return_type.is_some() {
                    return self.error(expression.span, format!("return value of '{callee}' is ignored"));
                }
                self.statements
                    .push(Statement::new(StatementKindIr::call([], call), statement.span));
            }
            StatementKind::FallibleCall {
                callee,
                arguments,
                failure,
            } => {
                let failure = self.check_else_continuation(failure, statement.span)?;
                let failure = self.materialize_else_continuation(failure, callee, statement.span);
                let call = self.check_call(callee, arguments, Some(failure), statement.span)?;
                if call.return_type.is_some() {
                    return self.error(statement.span, format!("return value of '{callee}' is ignored"));
                }
                self.statements
                    .push(Statement::new(StatementKindIr::call([], call), statement.span));
            }
            StatementKind::GuardLet {
                pattern:
                    ast::Pattern::ValueRepresentation {
                        representation,
                        binding,
                    },
                initializer: value,
                failure,
            } if matches!(representation.as_str(), "i32" | "f64" | "Object" | "PrimitiveString") => {
                self.check_value_refinement_statement(statement, representation, binding, value, failure)?
            }
            StatementKind::GuardLet { .. } => return self.error(statement.span, "invalid fallible binding pattern"),
            StatementKind::ValueMatch {
                name,
                value,
                arms,
                fallback,
            } => self.check_value_match_statement(statement, name, value, arms, fallback)?,
            StatementKind::ScalarMatch { value, arms, fallback } => {
                self.check_scalar_match_statement(statement, value, arms, fallback)?
            }
            StatementKind::Guard { condition, failure } => {
                let condition = self.check_condition(condition)?;
                let failure = self.check_guard_continuation(failure, statement.span)?;
                self.statements.push(Statement::new(
                    StatementKindIr::Guard { condition, failure },
                    statement.span,
                ));
            }
            StatementKind::Continuation {
                name,
                temperature,
                body,
                ..
            } => self.check_continuation_statement(statement, name, temperature, body)?,
            StatementKind::ContinuationJump { target, arguments } => {
                if self.resolve_continuation_symbol(target).is_none() {
                    if !arguments.is_empty() {
                        return self.error(statement.span, format!("unknown continuation '{target}'"));
                    }
                    let target = self.check_label(target, statement.span)?;
                    self.push_jump(&target, statement.span);
                    return Ok(());
                }
                let (_, _, body) = self.check_continuation_invocation(target, arguments, statement.span)?;
                self.statements.extend(body);
            }
            StatementKind::If { .. } => self.check_if_statement(statement)?,
            StatementKind::While {
                condition,
                body,
                post_tested,
            } => {
                let outer_variable_count = self.variables.len();
                let condition_start = self.statements.len();
                let condition = self.check_condition(condition)?;
                let condition_setup = self.statements.split_off(condition_start);
                let body = self.check_scoped_block(body)?;
                let mut captures = collect_captures(&condition_setup, outer_variable_count);
                captures.extend(collect_captures(&body, outer_variable_count));
                captures.sort_unstable();
                captures.dedup();
                self.statements.push(Statement::new(
                    StatementKindIr::While {
                        condition_setup,
                        condition,
                        captures,
                        body,
                        post_tested: *post_tested,
                    },
                    statement.span,
                ));
            }
        }
        Ok(())
    }

    fn declare_hidden(&mut self, base: &str, ty: Type, span: SourceSpan) -> VariableId {
        let id = self.variables.len();
        self.variables.push(Variable {
            name: format!("{base}_{id}"),
            ty,
            parameter_mode: None,
            span,
        });
        id
    }

    fn check_call(
        &mut self,
        callee: &str,
        arguments: &[ast::Expression],
        failure: Option<Value>,
        span: SourceSpan,
    ) -> Result<Call, Diagnostic> {
        let mut resolved_intrinsic = resolve_intrinsic(callee, arguments.len());
        let returning_builtin = resolved_intrinsic.as_ref().is_some_and(|resolved| {
            matches!(
                resolved.intrinsic,
                Intrinsic::Call(CallOperation::Interpreter | CallOperation::RawNative)
            ) && resolved.signature.return_type.is_some()
        });
        let indirect_condition = self
            .lookup(callee)
            .is_some_and(|symbol| matches!(symbol.ty, Type::IntegerCondition | Type::FloatCondition));
        let bytecode_load = (|| {
            if arguments.len() != 1
                || !resolved_intrinsic.as_ref().is_some_and(|resolved| {
                    resolved.intrinsic == Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field))
                })
            {
                return None;
            }
            let ExpressionKind::Name(name) = &arguments[0].kind else {
                return None;
            };
            let symbol = self.lookup(name)?;
            if matches!(
                symbol.ty,
                Type::EnvironmentCoordinateCacheIndex | Type::GlobalVariableCacheIndex | Type::PropertyLookupCacheIndex
            ) {
                return Some((symbol.ty.clone(), Type::U32, BytecodeOperation::Load(FieldWidth::U32)));
            }
            if !self.bytecode_fields.contains(&symbol.id) {
                return None;
            }
            if symbol.ty == Type::InlineInt32 {
                return Some((Type::InlineInt32, Type::Value, BytecodeOperation::LoadInlineInt32));
            }
            let width = match symbol.ty {
                Type::U8 | Type::I8 | Type::Bool => FieldWidth::U8,
                Type::U16 | Type::I16 => FieldWidth::U16,
                Type::U32 | Type::I32 | Type::ValueTag => FieldWidth::U32,
                Type::U64 | Type::I64 => FieldWidth::U64,
                _ => return None,
            };
            Some((symbol.ty.clone(), symbol.ty.clone(), BytecodeOperation::Load(width)))
        })();
        let (target, signature) = if returning_builtin {
            let resolved = resolved_intrinsic
                .take()
                .expect("returning builtins are resolved intrinsics");
            (CallTarget::Intrinsic(resolved.intrinsic), resolved.signature)
        } else if let Some((parameter_type, return_type, operation)) = &bytecode_load {
            (
                CallTarget::Intrinsic(Intrinsic::Bytecode(*operation)),
                signature(
                    &[(ParameterMode::In, parameter_type.clone())],
                    Some(return_type.clone()),
                ),
            )
        } else if let Some(signature) = self.signatures.get(callee).cloned() {
            (
                CallTarget::InlineFunction(
                    *self
                        .inline_function_ids
                        .get(callee)
                        .expect("every inline function signature has an identity"),
                ),
                signature,
            )
        } else if let Some(resolved) = resolved_intrinsic {
            (CallTarget::Intrinsic(resolved.intrinsic), resolved.signature)
        } else if let Some(symbol) = self.lookup(callee)
            && let Some(signature) = operation_signature(&symbol.ty)
        {
            (CallTarget::OperationParameter(symbol.id), signature)
        } else {
            return Err(Diagnostic::new(
                self.filename,
                span,
                format!("unknown operation '{callee}'"),
            ));
        };
        let explicit_parameter_count = signature.parameters.len() - usize::from(signature.has_else_parameter);
        if arguments.len() != explicit_parameter_count {
            return self.error(
                span,
                format!(
                    "'{callee}' expects {} arguments, found {}",
                    explicit_parameter_count,
                    arguments.len()
                ),
            );
        }
        if signature.has_else_parameter != failure.is_some() {
            return self.error(
                span,
                if signature.has_else_parameter {
                    format!("'{callee}' requires an else continuation")
                } else {
                    format!("'{callee}' does not accept an else continuation")
                },
            );
        }

        let mut values = Vec::new();
        for (argument, (mode, expected_type)) in arguments.iter().zip(&signature.parameters[..explicit_parameter_count])
        {
            if *expected_type == Type::Operand
                && let ExpressionKind::Name(name) = &argument.kind
                && let Some(symbol) = self.lookup(name)
            {
                match (mode, symbol.parameter_mode) {
                    (ParameterMode::In, Some(ParameterMode::Out)) => {
                        return self.error(argument.span, "cannot read through an out Operand parameter");
                    }
                    (ParameterMode::InOut, Some(ParameterMode::Out)) => {
                        return self.error(argument.span, "cannot read through an out Operand parameter");
                    }
                    _ => {}
                }
            }
            let value = if *expected_type == Type::AnyGpr {
                self.check_materialized_gpr_value(argument)?
            } else if *mode == ParameterMode::In
                && (matches!(
                    argument.kind,
                    ExpressionKind::Call { .. }
                        | ExpressionKind::Binary {
                            operator: BinaryOperator::Add | BinaryOperator::Subtract | BinaryOperator::Multiply,
                            ..
                        }
                        | ExpressionKind::Index { .. }
                ) || matches!(&argument.kind, ExpressionKind::Name(name) if name.contains('.') && !name.starts_with('.')))
            {
                let call = self.check_initializer(argument)?;
                if call.return_type.as_ref() != Some(expected_type) {
                    let found = call
                        .return_type
                        .as_ref()
                        .map(ToString::to_string)
                        .unwrap_or_else(|| "no value".to_string());
                    return self.error(argument.span, format!("expected {expected_type}, found {found}"));
                }
                let destination = self.declare_hidden("argument", expected_type.clone(), argument.span);
                self.statements.push(Statement::new(
                    StatementKindIr::call([destination], call),
                    argument.span,
                ));
                Value::new(ValueKind::Variable(destination), expected_type.clone(), argument.span)
            } else {
                self.check_value(argument, expected_type)?
            };
            if matches!(mode, ParameterMode::Out | ParameterMode::InOut) {
                if let ValueKind::Variable(id) = &value.kind {
                    let symbol = Symbol {
                        id: *id,
                        ty: self.variables[*id].ty.clone(),
                        parameter_mode: self.variables[*id].parameter_mode,
                    };
                    self.ensure_writable(&symbol, argument.span)?;
                } else if !matches!(value.kind, ValueKind::MachineRegister(_)) {
                    return self.error(argument.span, "an output argument must be a writable binding");
                }
            }
            values.push(value);
        }
        let failure_target = failure.as_ref().and_then(|failure| match &failure.kind {
            ValueKind::Label(target) => Some(target.clone()),
            _ => None,
        });
        if let Some(failure) = failure {
            values.push(failure);
        }

        let terminal =
            signature.terminal || matches!(target, CallTarget::Intrinsic(intrinsic) if intrinsic.is_terminal());
        let branch_target =
            failure_target.or_else(|| branch_target(&target, &values, &self.variables, indirect_condition));
        Ok(Call::with_control_flow(
            target,
            values,
            signature.parameters.iter().map(|(mode, _)| *mode).collect(),
            signature.return_type,
            terminal,
            branch_target,
        ))
    }

    fn check_field_load(&self, base: Value, field_name: &str, span: SourceSpan) -> Result<Call, Diagnostic> {
        let field = self
            .layouts
            .get(&(base.ty.clone(), field_name.to_string()))
            .cloned()
            .ok_or_else(|| {
                Diagnostic::new(
                    self.filename,
                    span,
                    format!("type {} has no field '{field_name}'", base.ty),
                )
            })?;
        let memory = self.field_memory(base, &field, span);
        if field.embedded {
            return Ok(Call::new(
                CallTarget::Intrinsic(Intrinsic::Address(AddressOperation::EmbeddedField)),
                vec![memory],
                vec![ParameterMode::In],
                Some(field.ty),
            ));
        }
        let width = field_width(&field.ty, false)
            .ok_or_else(|| Diagnostic::new(self.filename, span, format!("cannot load field of type {}", field.ty)))?;
        let kind = if field.cell_pointer {
            debug_assert_eq!(width, FieldWidth::U64);
            FieldAccessKind::LoadCellPointer { nonzero: field.nonnull }
        } else {
            FieldAccessKind::Load {
                width,
                nonzero: field.nonnull && width == FieldWidth::U64,
            }
        };
        Ok(Call::new(
            CallTarget::FieldAccess(FieldAccess { kind, pair: field.pair }),
            vec![memory],
            vec![ParameterMode::In],
            Some(field.ty),
        ))
    }

    fn field_memory(&self, base: Value, field: &LayoutField, span: SourceSpan) -> Value {
        let (base, offset) = match (&base.kind, &field.pinned_base, &field.pinned_offset) {
            (ValueKind::MachineRegister(_), Some(pinned_base), Some(pinned_offset)) => (
                Value::new(
                    ValueKind::MachineRegister(pinned_base.clone().into()),
                    machine_register_type(pinned_base)
                        .expect("generated pinned field base must name a machine register"),
                    span,
                ),
                *pinned_offset,
            ),
            _ => (base, field.offset),
        };
        Value::new(
            ValueKind::Memory(vec![base, Value::new(ValueKind::LayoutValue(offset), Type::U64, span)]),
            Type::Memory,
            span,
        )
    }

    fn check_field_projection_base(&mut self, name: &str, span: SourceSpan) -> Result<(Value, String), Diagnostic> {
        let mut components = name.split('.');
        let base_name = components.next().unwrap();
        let base = if let Some(base) = self.lookup(base_name).cloned() {
            Value::new(ValueKind::Variable(base.id), base.ty, span)
        } else if let Some(ty) = machine_register_type(base_name) {
            Value::new(ValueKind::MachineRegister(base_name.into()), ty, span)
        } else {
            return self.error(span, format!("unknown name '{base_name}'"));
        };
        let fields = components.collect::<Vec<_>>();
        let Some((field_name, intermediate_fields)) = fields.split_last() else {
            return self.error(span, "field projection requires a field name");
        };
        let mut base = base;
        for intermediate_field in intermediate_fields {
            let call = self.check_field_load(base, intermediate_field, span)?;
            let ty = call.return_type.clone().unwrap();
            let destination = self.declare_hidden("field_base", ty.clone(), span);
            self.statements
                .push(Statement::new(StatementKindIr::call([destination], call), span));
            base = Value::new(ValueKind::Variable(destination), ty, span);
        }
        Ok((base, (*field_name).to_string()))
    }

    fn check_field_store(
        &mut self,
        name: &str,
        initializer: &ast::Expression,
        span: SourceSpan,
    ) -> Result<Call, Diagnostic> {
        let root_name = name.split('.').next().unwrap();
        if let Some(root) = self.lookup(root_name).cloned() {
            self.ensure_writable(&root, span)?;
        }
        let (base, field_name) = self.check_field_projection_base(name, span)?;
        let field = self
            .layouts
            .get(&(base.ty.clone(), field_name.clone()))
            .cloned()
            .ok_or_else(|| {
                Diagnostic::new(
                    self.filename,
                    span,
                    format!("type {} has no field '{field_name}'", base.ty),
                )
            })?;
        if field.embedded {
            return self.error(span, format!("cannot assign embedded field '{}.{field_name}'", base.ty));
        }
        if field.nonnull && self.initializer_is_known_null_pointer(initializer, &field.ty) {
            return self.error(
                initializer.span,
                format!("cannot assign null to non-null field '{}.{field_name}'", base.ty),
            );
        }
        if matches!(
            &initializer.kind,
            ExpressionKind::Call { callee, arguments }
                if callee == "zeroed" && arguments.is_empty()
        ) {
            if field.ty != Type::ScriptOrModule {
                return self.error(
                    initializer.span,
                    format!(
                        "cannot zero-initialize field '{}.{field_name}' of type {}",
                        base.ty, field.ty
                    ),
                );
            }
            return Ok(Call::new(
                CallTarget::Intrinsic(Intrinsic::Aggregate(AggregateOperation::Zero16)),
                vec![Value::new(
                    ValueKind::Memory(vec![
                        base,
                        Value::new(ValueKind::LayoutValue(field.offset), Type::U64, span),
                    ]),
                    Type::Memory,
                    span,
                )],
                vec![ParameterMode::In],
                None,
            ));
        }
        if field.ty == Type::ScriptOrModule {
            let ExpressionKind::Name(source_name) = &initializer.kind else {
                return self.error(
                    initializer.span,
                    "a ScriptOrModule field can only be copied from another ScriptOrModule field",
                );
            };
            let (source_base, source_field_name) = self.check_field_projection_base(source_name, initializer.span)?;
            let source_field = self
                .layouts
                .get(&(source_base.ty.clone(), source_field_name.clone()))
                .cloned()
                .ok_or_else(|| {
                    Diagnostic::new(
                        self.filename,
                        initializer.span,
                        format!("type {} has no field '{source_field_name}'", source_base.ty),
                    )
                })?;
            if source_field.ty != Type::ScriptOrModule {
                return self.error(
                    initializer.span,
                    format!("expected ScriptOrModule, got {}", source_field.ty),
                );
            }
            let memory = |base, offset: layout::LayoutValue, value_span| {
                Value::new(
                    ValueKind::Memory(vec![
                        base,
                        Value::new(ValueKind::LayoutValue(offset), Type::U64, value_span),
                    ]),
                    Type::Memory,
                    value_span,
                )
            };
            return Ok(Call::new(
                CallTarget::Intrinsic(Intrinsic::Aggregate(AggregateOperation::Copy16)),
                vec![
                    memory(base, field.offset, span),
                    memory(source_base, source_field.offset, initializer.span),
                ],
                vec![ParameterMode::In, ParameterMode::In],
                None,
            ));
        }
        let memory = self.field_memory(base, &field, span);
        let value = self.check_materialized_value(initializer, &field.ty)?;
        let width = field_width(&field.ty, true)
            .ok_or_else(|| Diagnostic::new(self.filename, span, format!("cannot store field of type {}", field.ty)))?;
        Ok(Call::new(
            CallTarget::FieldAccess(FieldAccess {
                kind: FieldAccessKind::Store(width),
                pair: field.pair,
            }),
            vec![memory, value],
            vec![ParameterMode::In, ParameterMode::In],
            None,
        ))
    }

    fn update_known_null_pointer(&mut self, variable: VariableId, initializer: &ast::Expression, ty: &Type) {
        if self.initializer_is_known_null_pointer(initializer, ty) {
            self.known_null_pointers.insert(variable);
        } else {
            self.known_null_pointers.remove(&variable);
        }
    }

    fn initializer_is_known_null_pointer(&self, initializer: &ast::Expression, ty: &Type) -> bool {
        let ExpressionKind::Name(name) = &initializer.kind else {
            return false;
        };
        if name == "null" {
            return is_pointer_like(ty);
        }
        self.lookup(name)
            .is_some_and(|symbol| symbol.ty == *ty && self.known_null_pointers.contains(&symbol.id))
    }

    fn update_known_integer_literal(&mut self, variable: VariableId, initializer: &ast::Expression, ty: &Type) {
        if self.initializer_is_known_integer_literal(initializer, ty) {
            self.known_integer_literals.insert(variable);
        } else {
            self.known_integer_literals.remove(&variable);
        }
    }

    fn initializer_is_known_integer_literal(&self, initializer: &ast::Expression, ty: &Type) -> bool {
        if !is_integer_arithmetic_type(ty) {
            return false;
        }
        match &initializer.kind {
            ExpressionKind::Integer(_) => true,
            ExpressionKind::Name(name) => self
                .lookup(name)
                .is_some_and(|symbol| symbol.ty == *ty && self.known_integer_literals.contains(&symbol.id)),
            _ => false,
        }
    }

    fn value_is_known_integer_literal(&self, value: &Value) -> bool {
        match value.kind {
            ValueKind::Integer(_) | ValueKind::Constant(_) => true,
            ValueKind::Variable(variable) => self.known_integer_literals.contains(&variable),
            _ => false,
        }
    }

    fn check_initializer(&mut self, expression: &ast::Expression) -> Result<Call, Diagnostic> {
        self.check_initializer_with_expected(expression, None)
    }

    fn check_initializer_with_expected(
        &mut self,
        expression: &ast::Expression,
        expected: Option<&Type>,
    ) -> Result<Call, Diagnostic> {
        match &expression.kind {
            ExpressionKind::Call { callee, arguments } if callee == "alias" => {
                let Some(expected) = expected else {
                    return self.error(expression.span, "alias requires an explicit result type");
                };
                let [source] = arguments.as_slice() else {
                    return self.error(expression.span, "alias expects one value");
                };
                let source = self.check_alias_value(source, expected, expression.span)?;
                Ok(Call::new(
                    CallTarget::Intrinsic(Intrinsic::Value(ValueOperation::Reuse)),
                    vec![source],
                    vec![ParameterMode::In],
                    Some(expected.clone()),
                ))
            }
            ExpressionKind::Name(name) if name.contains('.') => {
                let (base, field_name) = self.check_field_projection_base(name, expression.span)?;
                self.check_field_load(base, &field_name, expression.span)
            }
            ExpressionKind::Name(name) => {
                let Some(symbol) = self.lookup(name).cloned() else {
                    if expected == Some(&Type::Value)
                        && let Some(constant) = value_literal_constant(name)
                    {
                        return Ok(Call::new(
                            CallTarget::Intrinsic(Intrinsic::LowLevel(LowLevelOperation::Move)),
                            vec![Value::new(constant_value_kind(constant), Type::Value, expression.span)],
                            vec![ParameterMode::In],
                            Some(Type::Value),
                        ));
                    }
                    if name == "null"
                        && let Some(expected) = expected
                        && is_pointer_like(expected)
                    {
                        return Ok(Call::new(
                            CallTarget::Intrinsic(Intrinsic::LowLevel(LowLevelOperation::Move)),
                            vec![Value::new(ValueKind::Integer(0), expected.clone(), expression.span)],
                            vec![ParameterMode::In],
                            Some(expected.clone()),
                        ));
                    }
                    if let Some(expected) = expected
                        && is_constant(name, expected)
                    {
                        return Ok(Call::new(
                            CallTarget::Intrinsic(Intrinsic::LowLevel(LowLevelOperation::Move)),
                            vec![Value::new(constant_value_kind(name), expected.clone(), expression.span)],
                            vec![ParameterMode::In],
                            Some(expected.clone()),
                        ));
                    }
                    let Some(ty) = machine_register_type(name) else {
                        return self.error(expression.span, format!("unknown binding '{name}'"));
                    };
                    return Ok(Call::new(
                        CallTarget::Intrinsic(Intrinsic::LowLevel(LowLevelOperation::Move)),
                        vec![Value::new(
                            ValueKind::MachineRegister(name.clone().into()),
                            ty.clone(),
                            expression.span,
                        )],
                        vec![ParameterMode::In],
                        Some(ty),
                    ));
                };
                Ok(Call::new(
                    CallTarget::Intrinsic(Intrinsic::LowLevel(LowLevelOperation::Move)),
                    vec![Value::new(
                        ValueKind::Variable(symbol.id),
                        symbol.ty.clone(),
                        expression.span,
                    )],
                    vec![ParameterMode::In],
                    Some(symbol.ty),
                ))
            }
            ExpressionKind::Integer(_) => {
                let Some(expected) = expected else {
                    return self.error(
                        expression.span,
                        "an integer literal initializer requires an explicit type",
                    );
                };
                let value = self.check_value(expression, expected)?;
                Ok(Call::new(
                    CallTarget::Intrinsic(Intrinsic::LowLevel(LowLevelOperation::Move)),
                    vec![value],
                    vec![ParameterMode::In],
                    Some(expected.clone()),
                ))
            }
            ExpressionKind::Index { base, index } => {
                let (base, element_type) = self.check_index_base(base)?;
                let index = self.check_sequence_index(index)?;
                let (load_operation, _, scale) = sequence_access(&element_type).ok_or_else(|| {
                    Diagnostic::new(
                        self.filename,
                        expression.span,
                        format!("cannot load sequence element of type {element_type}"),
                    )
                })?;
                Ok(Call::new(
                    CallTarget::Intrinsic(Intrinsic::Memory(load_operation)),
                    vec![Value::new(
                        ValueKind::Memory(vec![
                            base,
                            index,
                            Value::new(ValueKind::Integer(scale), Type::U8, expression.span),
                        ]),
                        Type::Memory,
                        expression.span,
                    )],
                    vec![ParameterMode::In],
                    Some(element_type),
                ))
            }
            ExpressionKind::Call { callee, arguments }
                if callee == "call_helper" && arguments.len() == 2 && expected.is_some() =>
            {
                let return_type = expected.expect("guarded by expected.is_some()");
                if !is_gpr_type(return_type) {
                    return self.error(expression.span, format!("call_helper cannot return {return_type}"));
                }
                Ok(Call::new(
                    CallTarget::Intrinsic(Intrinsic::Call(CallOperation::Helper)),
                    vec![
                        self.check_value(&arguments[0], &Type::FunctionSymbol)?,
                        self.check_materialized_gpr_value(&arguments[1])?,
                    ],
                    vec![ParameterMode::In, ParameterMode::In],
                    Some(return_type.clone()),
                ))
            }
            ExpressionKind::Call { callee, arguments }
                if callee == "unbox_object" && arguments.len() == 1 && expected.is_some() =>
            {
                let return_type = expected.expect("guarded by expected.is_some()");
                if !is_pointer_like(return_type) {
                    return self.error(expression.span, format!("unbox_object cannot return {return_type}"));
                }
                Ok(Call::new(
                    CallTarget::Intrinsic(Intrinsic::Value(ValueOperation::UnboxObject)),
                    vec![self.check_materialized_value(&arguments[0], &Type::Value)?],
                    vec![ParameterMode::In],
                    Some(return_type.clone()),
                ))
            }
            ExpressionKind::Call { callee, arguments } => self.check_call(callee, arguments, None, expression.span),
            ExpressionKind::Unary { operator, operand } => {
                self.check_unary_initializer(operator, operand, expression.span)
            }
            ExpressionKind::Binary { operator, lhs, rhs } => {
                self.check_binary_initializer(*operator, lhs, rhs, expected, expression.span)
            }
            _ => self.error(
                expression.span,
                "a binding initializer must be a call or arithmetic expression",
            ),
        }
    }

    fn check_unary_initializer(
        &mut self,
        operator: &UnaryOperator,
        operand: &ast::Expression,
        span: SourceSpan,
    ) -> Result<Call, Diagnostic> {
        if *operator == UnaryOperator::Reference {
            if let ExpressionKind::Name(name) = &operand.kind {
                let Some(symbol) = self.lookup(name).cloned() else {
                    return self.error(operand.span, format!("unknown binding '{name}'"));
                };
                let return_type = if symbol.ty == Type::EnvironmentCoordinate {
                    Type::EnvironmentCoordinateEntry
                } else if self.bytecode_fields.contains(&symbol.id) && matches!(symbol.ty, Type::Sequence(_)) {
                    symbol.ty.clone()
                } else {
                    return self.error(operand.span, format!("cannot take the address of {}", symbol.ty));
                };
                return Ok(Call::new(
                    CallTarget::Intrinsic(Intrinsic::Address(AddressOperation::BytecodeField)),
                    vec![Value::new(ValueKind::Variable(symbol.id), symbol.ty, operand.span)],
                    vec![ParameterMode::In],
                    Some(return_type),
                ));
            }
            let ExpressionKind::Index { base, index } = &operand.kind else {
                return self.error(
                    operand.span,
                    "can only take the address of an embedded bytecode field or sequence element",
                );
            };
            let (base, element_type) = self.check_index_base(base)?;
            let index = self.check_materialized_value(index, &Type::U32)?;
            let scale = self.aggregate_strides.get(&element_type).ok_or_else(|| {
                Diagnostic::new(
                    self.filename,
                    operand.span,
                    format!("generated layout metadata has no stride for {element_type}"),
                )
            })?;
            return Ok(Call::new(
                CallTarget::Intrinsic(Intrinsic::Address(AddressOperation::SequenceElement)),
                vec![base, index, Value::new(ValueKind::LayoutValue(*scale), Type::U64, span)],
                vec![ParameterMode::In, ParameterMode::In, ParameterMode::In],
                Some(element_type),
            ));
        }
        let (target, expected, return_type) = match operator {
            UnaryOperator::Negate => (
                CallTarget::InlineFunction(self.inline_function_id("negate", span)?),
                Type::Value,
                Type::Value,
            ),
            UnaryOperator::LogicalNot => (
                CallTarget::InlineFunction(self.inline_function_id("logical_not", span)?),
                Type::Value,
                Type::Value,
            ),
            UnaryOperator::BitwiseNot => (
                CallTarget::Intrinsic(Intrinsic::LowLevel(LowLevelOperation::BitwiseNotInt32)),
                Type::I32,
                Type::I32,
            ),
            UnaryOperator::Reference | UnaryOperator::ValueTypeTest { .. } => {
                return self.error(span, "this unary expression cannot initialize a binding");
            }
        };
        Ok(Call::new(
            target,
            vec![self.check_value(operand, &expected)?],
            vec![ParameterMode::In],
            Some(return_type),
        ))
    }

    fn check_binary_initializer(
        &mut self,
        operator: BinaryOperator,
        lhs: &ast::Expression,
        rhs: &ast::Expression,
        expected: Option<&Type>,
        span: SourceSpan,
    ) -> Result<Call, Diagnostic> {
        if let Some(operation) = integer_binary_operation(operator) {
            let integer_operation = IntegerBinaryOperation::Binary(operation);
            let integer_type = self.infer_integer_arithmetic_type(expected, lhs).or_else(|| {
                matches!(
                    operation,
                    BinaryOperation::And | BinaryOperation::Or | BinaryOperation::Xor
                )
                .then(|| self.infer_value_tag_bitwise_type(expected, lhs))
                .flatten()
            });
            if let Some(integer_type) = integer_type {
                return self.check_integer_arithmetic(lhs, rhs, &integer_type, integer_operation);
            }
            if matches!(
                operation,
                BinaryOperation::And | BinaryOperation::Or | BinaryOperation::Xor
            ) {
                return Ok(Call::new(
                    CallTarget::Intrinsic(Intrinsic::IntegerBinary(integer_operation)),
                    vec![
                        self.check_materialized_value(lhs, &Type::I32)?,
                        self.check_materialized_value(rhs, &Type::I32)?,
                    ],
                    vec![ParameterMode::In, ParameterMode::In],
                    Some(Type::I32),
                ));
            }
        }
        if let Some(operation) = float_binary_operation(operator) {
            let lhs = self.check_f64_operand(lhs)?;
            let rhs = self.check_f64_operand(rhs)?;
            if !matches!(lhs.kind, ValueKind::Variable(_)) {
                let operation = if operator == BinaryOperator::Divide {
                    "division"
                } else {
                    "arithmetic"
                };
                return self.error(
                    lhs.span,
                    format!("f64 {operation} requires a binding as its left operand"),
                );
            }
            return Ok(Call::new(
                CallTarget::Intrinsic(Intrinsic::FloatingPoint(FloatingPointOperation::Binary(operation))),
                vec![lhs, rhs],
                vec![ParameterMode::In, ParameterMode::In],
                Some(Type::F64),
            ));
        }
        if operator == BinaryOperator::Modulo {
            return Ok(Call::new(
                CallTarget::InlineFunction(self.inline_function_id("modulo", span)?),
                vec![
                    self.check_value(lhs, &Type::Value)?,
                    self.check_value(rhs, &Type::Value)?,
                ],
                vec![ParameterMode::In, ParameterMode::In],
                Some(Type::Value),
            ));
        }
        if matches!(operator, BinaryOperator::LeftShift | BinaryOperator::RightShift) {
            if let Some(integer_type) = self.infer_integer_arithmetic_type(expected, lhs) {
                let operation = if operator == BinaryOperator::LeftShift {
                    IntegerBinaryOperation::Shift(ShiftOperation::Left)
                } else if matches!(integer_type, Type::U32 | Type::U64) {
                    IntegerBinaryOperation::Shift(ShiftOperation::RightLogical)
                } else {
                    IntegerBinaryOperation::Shift(ShiftOperation::RightArithmetic)
                };
                return self.check_integer_arithmetic(lhs, rhs, &integer_type, operation);
            }
            let operation = if operator == BinaryOperator::LeftShift {
                IntegerBinaryOperation::Shift(ShiftOperation::Left)
            } else {
                IntegerBinaryOperation::Shift(ShiftOperation::RightArithmetic)
            };
            return Ok(Call::new(
                CallTarget::Intrinsic(Intrinsic::IntegerBinary(operation)),
                vec![self.check_value(lhs, &Type::I32)?, self.check_value(rhs, &Type::I32)?],
                vec![ParameterMode::In, ParameterMode::In],
                Some(Type::I32),
            ));
        }
        if operator == BinaryOperator::UnsignedRightShift {
            return Ok(Call::new(
                CallTarget::InlineFunction(self.inline_function_id("unsigned_right_shift_u32", span)?),
                vec![self.check_value(lhs, &Type::I32)?, self.check_value(rhs, &Type::I32)?],
                vec![ParameterMode::In, ParameterMode::In],
                Some(Type::U32),
            ));
        }
        if matches!(
            operator,
            BinaryOperator::LessThan
                | BinaryOperator::GreaterThan
                | BinaryOperator::LessThanOrEqual
                | BinaryOperator::GreaterThanOrEqual
        ) {
            let relation = match operator {
                BinaryOperator::LessThan => crate::intrinsic::ComparisonRelation::Less,
                BinaryOperator::GreaterThan => crate::intrinsic::ComparisonRelation::Greater,
                BinaryOperator::LessThanOrEqual => crate::intrinsic::ComparisonRelation::LessOrEqual,
                BinaryOperator::GreaterThanOrEqual => crate::intrinsic::ComparisonRelation::GreaterOrEqual,
                _ => unreachable!(),
            };
            return Ok(Call::new(
                CallTarget::Intrinsic(Intrinsic::IntegerComparison(IntegerComparisonOperation::Relational {
                    relation,
                    domain: crate::intrinsic::ComparisonDomain::Value,
                })),
                vec![
                    self.check_value(lhs, &Type::Value)?,
                    self.check_value(rhs, &Type::Value)?,
                ],
                vec![ParameterMode::In, ParameterMode::In],
                Some(Type::Value),
            ));
        }
        self.error(span, "this binary expression cannot initialize a binding")
    }

    fn check_integer_arithmetic(
        &mut self,
        lhs: &ast::Expression,
        rhs: &ast::Expression,
        ty: &Type,
        operation: IntegerBinaryOperation,
    ) -> Result<Call, Diagnostic> {
        let lhs = self.check_materialized_value(lhs, ty)?;
        if !matches!(lhs.kind, ValueKind::Variable(_)) {
            return self.error(lhs.span, "integer arithmetic requires a binding as its left operand");
        }
        let rhs = self.check_materialized_value(rhs, ty)?;
        Ok(Call::new(
            CallTarget::Intrinsic(Intrinsic::IntegerBinary(operation)),
            vec![lhs, rhs],
            vec![ParameterMode::In, ParameterMode::In],
            Some(ty.clone()),
        ))
    }

    fn infer_integer_arithmetic_type(&self, expected: Option<&Type>, lhs: &ast::Expression) -> Option<Type> {
        if let Some(expected) = expected.filter(|ty| is_integer_arithmetic_type(ty)) {
            return Some(expected.clone());
        }
        match &lhs.kind {
            ExpressionKind::Name(name) => self
                .lookup(name)
                .map(|symbol| symbol.ty.clone())
                .filter(is_integer_arithmetic_type),
            // NB: An `if let` guard would read better here, but the Flatpak SDK's Rust
            //     predates that feature.
            _ => integer_arithmetic_lhs(lhs).and_then(|lhs| self.infer_integer_arithmetic_type(None, lhs)),
        }
    }

    fn infer_value_tag_bitwise_type(&self, expected: Option<&Type>, lhs: &ast::Expression) -> Option<Type> {
        if expected == Some(&Type::ValueTag) {
            return Some(Type::ValueTag);
        }
        match &lhs.kind {
            ExpressionKind::Name(name) => self
                .lookup(name)
                .filter(|symbol| symbol.ty == Type::ValueTag)
                .map(|_| Type::ValueTag),
            ExpressionKind::Binary {
                operator: BinaryOperator::BitwiseAnd | BinaryOperator::BitwiseXor | BinaryOperator::BitwiseOr,
                lhs,
                ..
            } => self.infer_value_tag_bitwise_type(None, lhs),
            _ => None,
        }
    }

    fn check_index_base(&mut self, expression: &ast::Expression) -> Result<(Value, Type), Diagnostic> {
        let value = match &expression.kind {
            ExpressionKind::Name(name) if name.contains('.') => {
                let call = self.check_initializer(expression)?;
                let ty = call.return_type.clone().ok_or_else(|| {
                    Diagnostic::new(self.filename, expression.span, "indexed base does not produce a value")
                })?;
                let destination = self.declare_hidden("indexed_base", ty.clone(), expression.span);
                self.statements.push(Statement::new(
                    StatementKindIr::call([destination], call),
                    expression.span,
                ));
                Value::new(ValueKind::Variable(destination), ty, expression.span)
            }
            ExpressionKind::Name(name) => {
                let ty = self
                    .lookup(name)
                    .map(|symbol| symbol.ty.clone())
                    .or_else(|| machine_register_type(name))
                    .ok_or_else(|| {
                        Diagnostic::new(self.filename, expression.span, format!("unknown binding '{name}'"))
                    })?;
                self.check_value(expression, &ty)?
            }
            _ => return self.error(expression.span, "indexed base must be a binding or field projection"),
        };
        let element_type = match &value.ty {
            Type::PropertyStorage => Type::Value,
            Type::Sequence(element_type) => (**element_type).clone(),
            ty => return self.error(expression.span, format!("type {ty} is not a sequence")),
        };
        Ok((value, element_type))
    }

    fn check_sequence_index(&mut self, expression: &ast::Expression) -> Result<Value, Diagnostic> {
        let expected = match &expression.kind {
            ExpressionKind::Name(name) => self
                .lookup(name)
                .map(|symbol| symbol.ty.clone())
                .or_else(|| machine_register_type(name))
                .unwrap_or(Type::U32),
            _ => Type::U32,
        };
        if !matches!(expected, Type::U32 | Type::U64) {
            return self.error(
                expression.span,
                format!("sequence index must be u32 or u64, found {expected}"),
            );
        }
        self.check_materialized_value(expression, &expected)
    }

    fn check_value_block(
        &mut self,
        block: &ast::Block,
        expected: Option<&Type>,
    ) -> Result<(Vec<Statement>, Call), Diagnostic> {
        let value = block
            .value
            .as_ref()
            .expect("the parser requires a tail expression for value-producing blocks");
        let facts = self.type_facts();
        self.scopes.push(HashMap::default());
        let start = self.statements.len();
        let result = self.check_block(block, false).and_then(|()| {
            let mut value = self.check_initializer_with_expected(value, expected)?;
            if value.intrinsic() == Some(Intrinsic::LowLevel(LowLevelOperation::Move))
                && value.return_type == Some(Type::BytecodeOffset)
                && matches!(value.arguments[0].kind, ValueKind::Variable(source) if self.bytecode_fields.contains(&source))
            {
                value.target = CallTarget::Intrinsic(Intrinsic::LowLevel(LowLevelOperation::LoadLabel));
            }
            Ok(value)
        });
        let statements = self.statements.split_off(start);
        self.scopes.pop();
        self.restore_type_facts(facts);
        Ok((statements, result?))
    }

    fn check_condition(&mut self, expression: &ast::Expression) -> Result<Condition, Diagnostic> {
        if let ExpressionKind::Binary {
            operator: BinaryOperator::Identity { negated },
            lhs,
            rhs,
        } = &expression.kind
        {
            return Ok(Condition::ScalarEquality {
                lhs: self.check_materialized_value(lhs, &Type::Value)?,
                rhs: self.check_materialized_value(rhs, &Type::Value)?,
                equal: !negated,
            });
        }
        if let ExpressionKind::Binary {
            operator: BinaryOperator::BitTest { set },
            lhs,
            rhs,
        } = &expression.kind
        {
            let Some((value, ty)) = self.check_scalar_condition_lhs(lhs)? else {
                return self.error(lhs.span, "bit test requires a sized integer value");
            };
            if !is_integer_representation(&ty) {
                return self.error(lhs.span, format!("cannot test bits in {ty}"));
            }
            return Ok(Condition::BitTest {
                value,
                mask: self.check_materialized_value(rhs, &ty)?,
                set: *set,
            });
        }
        if let ExpressionKind::Unary {
            operator: UnaryOperator::ValueTypeTest { type_name, negated },
            operand: value,
        } = &expression.kind
        {
            if matches!(type_name.as_str(), "Empty" | "NegativeZero" | "Undefined") {
                let expected = self.declare_hidden("expected_value", Type::Value, expression.span);
                return Ok(Condition::ValueBits {
                    value: self.check_materialized_value(value, &Type::Value)?,
                    expected,
                    expected_bits: match type_name.as_str() {
                        "Empty" => "EMPTY_VALUE",
                        "NegativeZero" => "NEGATIVE_ZERO",
                        "Undefined" => "UNDEFINED_SHIFTED",
                        _ => unreachable!(),
                    }
                    .to_string(),
                    matches: !negated,
                });
            }
            let (mask, expected_tag, matches_when_equal) = match type_name.as_str() {
                "Accessor" => (None, "ACCESSOR_TAG", true),
                "Nullish" => (Some(TagMask::Immediate(0xfffe)), "UNDEFINED_TAG", true),
                "Object" => (None, "OBJECT_TAG", true),
                "f64" => (
                    Some(TagMask::Constant("NAN_BASE_TAG".to_string())),
                    "NAN_BASE_TAG",
                    false,
                ),
                _ => return self.error(expression.span, format!("unknown Value representation '{type_name}'")),
            };
            let value = self.check_materialized_value(value, &Type::Value)?;
            let source_tag = match &value.kind {
                ValueKind::Variable(value) => self
                    .active_value_tags
                    .iter()
                    .rev()
                    .find_map(|(source, tag)| (*source == *value).then_some(*tag)),
                _ => None,
            };
            let tag = self.declare_hidden("value_tag", Type::ValueTag, expression.span);
            return Ok(Condition::ValueTag {
                value,
                tag,
                source_tag,
                mask,
                expected_tag: expected_tag.to_string(),
                matches_when_equal,
                matches: !negated,
            });
        }
        if let ExpressionKind::Unary {
            operator: UnaryOperator::LogicalNot,
            operand,
        } = &expression.kind
        {
            return Ok(Condition::LogicalNot(Box::new(self.check_condition(operand)?)));
        }
        let scalar_equality = match &expression.kind {
            ExpressionKind::Binary {
                operator: BinaryOperator::LooseEqual,
                lhs,
                rhs,
            } => Some((true, lhs, rhs)),
            ExpressionKind::Binary {
                operator: BinaryOperator::LooseNotEqual,
                lhs,
                rhs,
            } => Some((false, lhs, rhs)),
            _ => None,
        };
        if let Some((equal, lhs, rhs)) = scalar_equality {
            if let Some(condition) = self.check_scalar_equality(lhs, rhs, equal)? {
                return Ok(condition);
            }
            return self.error(
                expression.span,
                "Value equality requires an explicit equality operation",
            );
        }
        let scalar_relational = match &expression.kind {
            ExpressionKind::Binary {
                operator: BinaryOperator::LessThan,
                lhs,
                rhs,
            } => Some((ComparisonRelation::Less, lhs, rhs)),
            ExpressionKind::Binary {
                operator: BinaryOperator::GreaterThan,
                lhs,
                rhs,
            } => Some((ComparisonRelation::Greater, lhs, rhs)),
            ExpressionKind::Binary {
                operator: BinaryOperator::LessThanOrEqual,
                lhs,
                rhs,
            } => Some((ComparisonRelation::LessOrEqual, lhs, rhs)),
            ExpressionKind::Binary {
                operator: BinaryOperator::GreaterThanOrEqual,
                lhs,
                rhs,
            } => Some((ComparisonRelation::GreaterOrEqual, lhs, rhs)),
            _ => None,
        };
        if let Some((relation, lhs, rhs)) = scalar_relational {
            if let Some(condition) = self.check_scalar_relational(lhs, rhs, relation)? {
                return Ok(condition);
            }
            return self.error(expression.span, "scalar comparison requires sized integer operands");
        }
        Ok(Condition::ScalarEquality {
            lhs: self.check_materialized_value(expression, &Type::Bool)?,
            rhs: Value::new(ValueKind::Integer(0), Type::Bool, expression.span),
            equal: false,
        })
    }

    fn check_scalar_match_pattern(
        &mut self,
        pattern: &ast::Pattern,
        value_type: &Type,
        span: SourceSpan,
    ) -> Result<Vec<Value>, Diagnostic> {
        match pattern {
            ast::Pattern::Expression(expression) => Ok(vec![self.check_value(expression, value_type)?]),
            ast::Pattern::Alternatives(patterns) => {
                let mut alternatives = Vec::new();
                for pattern in patterns {
                    alternatives.extend(self.check_scalar_match_pattern(pattern, value_type, span)?);
                }
                Ok(alternatives)
            }
            ast::Pattern::Binding { .. }
            | ast::Pattern::Tuple(_)
            | ast::Pattern::Wildcard
            | ast::Pattern::ValueRepresentation { .. } => self.error(span, "pattern is not valid in a scalar match"),
        }
    }

    fn check_scalar_relational(
        &mut self,
        lhs: &ast::Expression,
        rhs: &ast::Expression,
        relation: ComparisonRelation,
    ) -> Result<Option<Condition>, Diagnostic> {
        let Some((lhs, lhs_type)) = self.check_scalar_condition_lhs(lhs)? else {
            return Ok(None);
        };
        let signed = match lhs_type {
            Type::I8 | Type::I16 | Type::I32 | Type::I64 => true,
            Type::U8 | Type::U16 | Type::U32 | Type::U64 => false,
            _ => return Ok(None),
        };
        let rhs = self.check_materialized_value(rhs, &lhs_type)?;
        Ok(Some(Condition::ScalarRelational {
            lhs,
            rhs,
            relation,
            signed,
        }))
    }

    fn check_scalar_equality(
        &mut self,
        lhs: &ast::Expression,
        rhs: &ast::Expression,
        equal: bool,
    ) -> Result<Option<Condition>, Diagnostic> {
        let Some((lhs, lhs_type)) = self.check_scalar_condition_lhs(lhs)? else {
            return Ok(None);
        };
        if lhs_type == Type::Value {
            return Ok(None);
        }
        let rhs = match &rhs.kind {
            ExpressionKind::Integer(0) => Value::new(ValueKind::Integer(0), lhs_type.clone(), rhs.span),
            _ => self.check_materialized_value(rhs, &lhs_type)?,
        };
        Ok(Some(Condition::ScalarEquality { lhs, rhs, equal }))
    }

    fn check_scalar_condition_lhs(
        &mut self,
        expression: &ast::Expression,
    ) -> Result<Option<(Value, Type)>, Diagnostic> {
        if let ExpressionKind::Name(name) = &expression.kind
            && !name.contains('.')
        {
            let Some(symbol) = self.lookup(name).cloned() else {
                let Some(ty) = machine_register_type(name) else {
                    return Ok(None);
                };
                return Ok(Some((
                    Value::new(
                        ValueKind::MachineRegister(name.clone().into()),
                        ty.clone(),
                        expression.span,
                    ),
                    ty,
                )));
            };
            let value = self.check_materialized_value(expression, &symbol.ty)?;
            return Ok(Some((value, symbol.ty)));
        }
        if !matches!(
            expression.kind,
            ExpressionKind::Name(_) | ExpressionKind::Call { .. } | ExpressionKind::Index { .. }
        ) && integer_arithmetic_lhs(expression).is_none()
        {
            return Ok(None);
        }
        let call = self.check_initializer(expression)?;
        let Some(ty) = call.return_type.clone() else {
            return self.error(expression.span, "left operand does not produce a value");
        };
        let destination = self.declare_hidden("condition", ty.clone(), expression.span);
        self.statements.push(Statement::new(
            StatementKindIr::call([destination], call),
            expression.span,
        ));
        Ok(Some((
            Value::new(ValueKind::Variable(destination), ty.clone(), expression.span),
            ty,
        )))
    }

    fn check_materialized_value(&mut self, expression: &ast::Expression, expected: &Type) -> Result<Value, Diagnostic> {
        if let ExpressionKind::Call { callee, arguments } = &expression.kind
            && callee == "alias"
        {
            let [source] = arguments.as_slice() else {
                return self.error(expression.span, "alias expects one value");
            };
            return self.check_alias_value(source, expected, expression.span);
        }
        if matches!(&expression.kind, ExpressionKind::Name(name) if name.contains('.'))
            || matches!(
                expression.kind,
                ExpressionKind::Call { .. } | ExpressionKind::Index { .. }
            )
            || integer_arithmetic_lhs(expression).is_some()
        {
            let call = self.check_initializer_with_expected(expression, Some(expected))?;
            if call.return_type.as_ref() != Some(expected) {
                let found = call
                    .return_type
                    .as_ref()
                    .map(ToString::to_string)
                    .unwrap_or_else(|| "no value".to_string());
                return self.error(expression.span, format!("expected {expected}, found {found}"));
            }
            let destination = self.declare_hidden("expression", expected.clone(), expression.span);
            self.statements.push(Statement::new(
                StatementKindIr::call([destination], call),
                expression.span,
            ));
            return Ok(Value::new(
                ValueKind::Variable(destination),
                expected.clone(),
                expression.span,
            ));
        }
        self.check_value(expression, expected)
    }

    fn check_alias_value(
        &mut self,
        source: &ast::Expression,
        expected: &Type,
        span: SourceSpan,
    ) -> Result<Value, Diagnostic> {
        let source = if matches!(expected, Type::Sequence(_)) {
            let pointer_call = match &source.kind {
                ExpressionKind::Name(name) if !name.contains('.') => self
                    .lookup(name)
                    .is_some_and(|symbol| is_pointer_like(&symbol.ty))
                    .then(|| self.check_initializer(source))
                    .transpose()?,
                ExpressionKind::Name(_) => {
                    let call = self.check_initializer(source)?;
                    call.return_type.as_ref().is_some_and(is_pointer_like).then_some(call)
                }
                _ => None,
            };
            if let Some(call) = pointer_call {
                let source_type = call.return_type.clone().unwrap();
                let destination = self.declare_hidden("alias_source", source_type.clone(), source.span);
                self.statements
                    .push(Statement::new(StatementKindIr::call([destination], call), source.span));
                Value::new(ValueKind::Variable(destination), source_type, source.span)
            } else {
                self.check_materialized_gpr_value(source)?
            }
        } else {
            self.check_materialized_gpr_value(source)?
        };
        let explicit_type_change = matches!(
            (&source.ty, expected),
            (Type::U32, Type::I32)
                | (Type::I32, Type::U32)
                | (Type::U64, Type::I64)
                | (Type::I64, Type::U64)
                | (Type::Boxed(_), Type::Value)
                | (Type::Bool | Type::U8 | Type::U16 | Type::U32, Type::U64)
                | (Type::U8, Type::U16)
                | (Type::U8 | Type::U16, Type::U32)
                | (Type::I8 | Type::I16 | Type::U8 | Type::U16, Type::I32)
        );
        let raw_alias = (source.ty == Type::U64 || is_pointer_like(&source.ty))
            && (*expected == Type::U64 || is_pointer_like(expected));
        if raw_alias && is_pointer_like(expected) && self.value_is_known_integer_literal(&source) {
            return self.error(span, format!("cannot alias integer literal as {expected}"));
        }
        if !explicit_type_change && !raw_alias {
            return self.error(span, format!("cannot alias {} as {expected}", source.ty));
        }
        Ok(Value::new(source.kind, expected.clone(), span))
    }

    fn check_f64_operand(&mut self, expression: &ast::Expression) -> Result<Value, Diagnostic> {
        let ExpressionKind::Call { callee, arguments } = &expression.kind else {
            return self.check_value(expression, &Type::F64);
        };
        let call = self.check_call(callee, arguments, None, expression.span)?;
        if call.return_type != Some(Type::F64) {
            return self.error(expression.span, format!("'{callee}' does not return f64"));
        }
        let destination = self.declare_hidden("expression", Type::F64, expression.span);
        self.statements.push(Statement::new(
            StatementKindIr::call([destination], call),
            expression.span,
        ));
        Ok(Value::new(ValueKind::Variable(destination), Type::F64, expression.span))
    }

    fn check_value(&self, expression: &ast::Expression, expected: &Type) -> Result<Value, Diagnostic> {
        match &expression.kind {
            ExpressionKind::Memory(components) if *expected == Type::Memory => Ok(Value::new(
                ValueKind::Memory(
                    components
                        .iter()
                        .map(|component| self.check_memory_component(component))
                        .collect::<Result<_, _>>()?,
                ),
                Type::Memory,
                expression.span,
            )),
            ExpressionKind::Name(name) => {
                if *expected == Type::Value
                    && let Some(constant) = value_literal_constant(name)
                {
                    return Ok(Value::new(constant_value_kind(constant), Type::Value, expression.span));
                }
                if name == "null" && is_pointer_like(expected) {
                    return Ok(Value::new(ValueKind::Integer(0), expected.clone(), expression.span));
                }
                if *expected == Type::Bool
                    && let Some(value) = match name.as_str() {
                        "false" => Some(0),
                        "true" => Some(1),
                        _ => None,
                    }
                {
                    return Ok(Value::new(ValueKind::Integer(value), Type::Bool, expression.span));
                }
                if let Some(symbol) = self.lookup(name) {
                    if &symbol.ty != expected {
                        return self.error(
                            expression.span,
                            format!("expected {expected}, found {} for '{name}'", symbol.ty),
                        );
                    }
                    return Ok(Value::new(
                        ValueKind::Variable(symbol.id),
                        symbol.ty.clone(),
                        expression.span,
                    ));
                }
                if *expected == Type::SlowPath
                    && (name.starts_with("asm_slow_path_")
                        || name.chars().all(|character| {
                            character.is_ascii_lowercase() || character.is_ascii_digit() || character == '_'
                        }))
                {
                    return Ok(Value::new(
                        ValueKind::SlowPath(ExternalSymbol::new(if name.starts_with("asm_slow_path_") {
                            name.clone()
                        } else {
                            format!("asm_slow_path_{name}")
                        })),
                        Type::SlowPath,
                        expression.span,
                    ));
                }
                if *expected == Type::FunctionSymbol {
                    return Ok(Value::new(
                        ValueKind::FunctionSymbol(ExternalSymbol::new(name)),
                        Type::FunctionSymbol,
                        expression.span,
                    ));
                }
                if let Some(ty) = machine_register_type(name) {
                    if &ty != expected {
                        return self.error(expression.span, format!("expected {expected}, found {ty} for '{name}'"));
                    }
                    return Ok(Value::new(
                        ValueKind::MachineRegister(name.clone().into()),
                        ty,
                        expression.span,
                    ));
                }
                if matches!(expected, Type::Continuation(_)) {
                    if let Some(continuation) = self.resolve_continuation_symbol(name)
                        && &continuation.ty != expected
                    {
                        return self.error(
                            expression.span,
                            format!("expected {expected}, found {} for '{name}'", continuation.ty),
                        );
                    }
                    let name = self.check_label(name, expression.span)?;
                    return Ok(Value::new(ValueKind::Label(name), expected.clone(), expression.span));
                }
                let matches_inline_operation = operation_signature(expected).is_some_and(|signature| {
                    self.signatures
                        .get(name)
                        .is_some_and(|candidate| signatures_match(candidate, &signature))
                });
                if let Some(intrinsic) = resolve_operation(name, expected) {
                    return Ok(Value::new(
                        ValueKind::Operation(OperationValue::Intrinsic(intrinsic)),
                        expected.clone(),
                        expression.span,
                    ));
                }
                if matches_inline_operation {
                    return Ok(Value::new(
                        ValueKind::Operation(OperationValue::InlineFunction(
                            *self
                                .inline_function_ids
                                .get(name)
                                .expect("every inline function signature has an identity"),
                        )),
                        expected.clone(),
                        expression.span,
                    ));
                }
                if is_constant(name, expected) {
                    return Ok(Value::new(constant_value_kind(name), expected.clone(), expression.span));
                }
                self.error(expression.span, format!("unknown name '{name}'"))
            }
            ExpressionKind::Integer(value) if is_integer_representation(expected) => Ok(Value::new(
                ValueKind::Integer(*value),
                expected.clone(),
                expression.span,
            )),
            ExpressionKind::Integer(_) => {
                self.error(expression.span, format!("expected {expected}, found integer literal"))
            }
            ExpressionKind::Call { .. }
            | ExpressionKind::Unary { .. }
            | ExpressionKind::Binary { .. }
            | ExpressionKind::Index { .. }
            | ExpressionKind::Tuple(_)
            | ExpressionKind::Memory(_) => self.error(expression.span, "nested expressions are not supported here"),
        }
    }

    fn check_gpr_value(&self, expression: &ast::Expression) -> Result<Value, Diagnostic> {
        if let ExpressionKind::Name(name) = &expression.kind {
            if let Some(symbol) = self.lookup(name) {
                if matches!(symbol.ty, Type::Field(_)) {
                    return Ok(Value::new(
                        ValueKind::Variable(symbol.id),
                        symbol.ty.clone(),
                        expression.span,
                    ));
                }
                if !is_gpr_type(&symbol.ty) {
                    return self.error(
                        expression.span,
                        format!("expected a general-purpose value, found {}", symbol.ty),
                    );
                }
                return self.check_value(expression, &symbol.ty);
            }
            if let Some(ty) = machine_register_type(name) {
                return Ok(Value::new(
                    ValueKind::MachineRegister(name.clone().into()),
                    ty,
                    expression.span,
                ));
            }
            return Ok(Value::new(
                if name
                    .chars()
                    .all(|character| character.is_ascii_uppercase() || character == '_' || character.is_ascii_digit())
                {
                    constant_value_kind(name)
                } else {
                    ValueKind::MachineRegister(name.clone().into())
                },
                Type::AnyGpr,
                expression.span,
            ));
        }
        if let ExpressionKind::Integer(value) = expression.kind {
            return Ok(Value::new(ValueKind::Integer(value), Type::AnyGpr, expression.span));
        }
        self.error(expression.span, "expected a general-purpose value")
    }

    fn check_materialized_gpr_value(&mut self, expression: &ast::Expression) -> Result<Value, Diagnostic> {
        if matches!(&expression.kind, ExpressionKind::Name(name) if !name.contains('.'))
            || matches!(expression.kind, ExpressionKind::Integer(_))
        {
            return self.check_gpr_value(expression);
        }
        let call = self.check_initializer(expression)?;
        let Some(return_type) = call.return_type.clone() else {
            return self.error(expression.span, "expected a general-purpose value");
        };
        if !is_gpr_type(&return_type) {
            return self.error(
                expression.span,
                format!("expected a general-purpose value, found {return_type}"),
            );
        }
        let destination = self.declare_hidden("expression", return_type.clone(), expression.span);
        self.statements.push(Statement::new(
            StatementKindIr::call([destination], call),
            expression.span,
        ));
        Ok(Value::new(
            ValueKind::Variable(destination),
            return_type,
            expression.span,
        ))
    }

    fn check_memory_component(&self, expression: &ast::Expression) -> Result<Value, Diagnostic> {
        if let ExpressionKind::Name(name) = &expression.kind
            && let Some(symbol) = self.lookup(name)
            && matches!(symbol.ty, Type::Field(_) | Type::Operand)
        {
            return Ok(Value::new(
                ValueKind::Variable(symbol.id),
                symbol.ty.clone(),
                expression.span,
            ));
        }
        self.check_gpr_value(expression)
    }

    fn resolve_continuation_symbol(&self, name: &str) -> Option<&ContinuationSymbol> {
        self.continuation_scopes.iter().rev().find_map(|scope| scope.get(name))
    }

    fn push_jump(&mut self, target: &str, span: SourceSpan) {
        self.statements.push(Statement::new(
            StatementKindIr::call(
                [],
                Call::with_control_flow(
                    CallTarget::Intrinsic(Intrinsic::Control(ControlOperation::JumpLabel)),
                    vec![Value::new(ValueKind::Label(target.to_string()), Type::label(), span)],
                    vec![ParameterMode::In],
                    None,
                    true,
                    Some(target.to_string()),
                ),
            ),
            span,
        ));
    }

    fn check_continuation_invocation(
        &mut self,
        target: &str,
        arguments: &[ast::Expression],
        span: SourceSpan,
    ) -> Result<(String, BlockTemperature, Vec<Statement>), Diagnostic> {
        let Some(continuation) = self.resolve_continuation_symbol(target).cloned() else {
            return self.error(span, format!("unknown continuation '{target}'"));
        };
        let parameters = continuation.parameters;
        if arguments.len() != parameters.len() {
            let expected = parameters.len();
            return self.error(
                span,
                format!(
                    "continuation '{target}' expects {expected} argument{}, found {}",
                    if expected == 1 { "" } else { "s" },
                    arguments.len()
                ),
            );
        }

        let start = self.statements.len();
        for (argument, parameter) in arguments.iter().zip(parameters) {
            let call = self.check_initializer_with_expected(argument, Some(&parameter.ty))?;
            let Some(return_type) = &call.return_type else {
                return self.error(
                    argument.span,
                    format!("'{}' does not return a value", call.display_name()),
                );
            };
            if *return_type != parameter.ty {
                return self.error(argument.span, format!("expected {}, found {return_type}", parameter.ty));
            }
            self.statements.push(Statement::new(
                StatementKindIr::call([parameter.id], call),
                argument.span,
            ));
        }
        self.push_jump(&continuation.lowered_name, span);
        let body = self.statements.split_off(start);
        Ok((continuation.lowered_name, continuation.temperature, body))
    }

    fn check_label(&self, name: &str, span: SourceSpan) -> Result<String, Diagnostic> {
        if let Some(continuation) = self.resolve_continuation_symbol(name) {
            if !continuation.parameters.is_empty() {
                return self.error(span, format!("continuation '{name}' requires arguments"));
            }
            return Ok(continuation.lowered_name.clone());
        }
        if !name.starts_with('.') {
            return Ok(name.to_string());
        }
        Err(Diagnostic::new(self.filename, span, format!("unknown label '{name}'")))
    }

    fn ensure_writable(&self, symbol: &Symbol, span: SourceSpan) -> Result<(), Diagnostic> {
        if symbol.parameter_mode == Some(ParameterMode::In) {
            return self.error(span, format!("cannot write through an in {} parameter", symbol.ty));
        }
        Ok(())
    }
}

fn is_integer_representation(ty: &Type) -> bool {
    matches!(
        ty,
        Type::I8 | Type::I16 | Type::I32 | Type::U8 | Type::U16 | Type::U32 | Type::I64 | Type::U64 | Type::ValueTag
    )
}

fn is_gpr_type(ty: &Type) -> bool {
    !matches!(
        ty,
        Type::F32
            | Type::F64
            | Type::Memory
            | Type::AnyGpr
            | Type::Field(_)
            | Type::Operand
            | Type::Continuation(_)
            | Type::BinaryOperation(_)
            | Type::CheckedBinaryOperation(_)
            | Type::EqualityOperation
            | Type::FunctionSymbol
            | Type::SlowPath
    )
}

fn field_width(ty: &Type, allow_pointer: bool) -> Option<FieldWidth> {
    Some(match ty {
        Type::U8 | Type::I8 | Type::Bool => FieldWidth::U8,
        Type::U16 | Type::I16 => FieldWidth::U16,
        Type::U32 | Type::I32 | Type::ValueTag => FieldWidth::U32,
        Type::U64
        | Type::I64
        | Type::Value
        | Type::Boxed(_)
        | Type::Object
        | Type::PrimitiveString
        | Type::Utf16StringData
        | Type::Shape
        | Type::ExecutionContext
        | Type::VM
        | Type::Executable
        | Type::Realm
        | Type::GlobalEnvironment
        | Type::Environment
        | Type::PrivateEnvironment
        | Type::DeclarativeEnvironment
        | Type::EnvironmentShape
        | Type::DeclarativeEnvironmentRareData
        | Type::GlobalVariableCache
        | Type::PropertyLookupCache
        | Type::PropertyNameIterator
        | Type::ObjectPropertyIteratorCacheData
        | Type::ObjectPropertyIteratorCache
        | Type::PrototypeChainValidity
        | Type::FunctionObject
        | Type::ECMAScriptFunctionObject
        | Type::RawNativeFunction
        | Type::NativeFunctionTableEntry
        | Type::SharedFunctionInstanceData
        | Type::PropertyStorage
        | Type::Sequence(_) => FieldWidth::U64,
        Type::Pointer(_) if allow_pointer => FieldWidth::U64,
        _ => return None,
    })
}

fn is_constant(name: &str, expected: &Type) -> bool {
    match expected {
        Type::ValueTag => name.ends_with("_TAG"),
        Type::Value | Type::I8 | Type::I16 | Type::I32 | Type::U8 | Type::U16 | Type::U32 | Type::I64 | Type::U64 => {
            name.chars()
                .all(|character| character.is_ascii_uppercase() || character == '_' || character.is_ascii_digit())
        }
        _ => false,
    }
}

fn constant_value_kind(name: &str) -> ValueKind {
    match name {
        "INT32_MAX" => ValueKind::Integer(i32::MAX.into()),
        _ => ValueKind::Constant(name.to_string()),
    }
}

fn value_literal_constant(name: &str) -> Option<&'static str> {
    match name {
        "Value<Empty>" => Some("EMPTY_VALUE"),
        "Value<False>" => Some("BOOLEAN_FALSE"),
        "Value<NegativeZero>" => Some("NEGATIVE_ZERO"),
        "Value<True>" => Some("BOOLEAN_TRUE"),
        "Value<Undefined>" => Some("UNDEFINED_SHIFTED"),
        _ => None,
    }
}

fn integer_arithmetic_lhs(expression: &ast::Expression) -> Option<&ast::Expression> {
    let ExpressionKind::Binary { operator, lhs, .. } = &expression.kind else {
        return None;
    };
    matches!(
        operator,
        BinaryOperator::Add
            | BinaryOperator::Subtract
            | BinaryOperator::Multiply
            | BinaryOperator::BitwiseAnd
            | BinaryOperator::BitwiseXor
            | BinaryOperator::BitwiseOr
            | BinaryOperator::LeftShift
            | BinaryOperator::RightShift
    )
    .then_some(lhs)
}

fn integer_binary_operation(operator: BinaryOperator) -> Option<BinaryOperation> {
    Some(match operator {
        BinaryOperator::Add => BinaryOperation::Add,
        BinaryOperator::Subtract => BinaryOperation::Subtract,
        BinaryOperator::Multiply => BinaryOperation::Multiply,
        BinaryOperator::BitwiseAnd => BinaryOperation::And,
        BinaryOperator::BitwiseOr => BinaryOperation::Or,
        BinaryOperator::BitwiseXor => BinaryOperation::Xor,
        _ => return None,
    })
}

fn float_binary_operation(operator: BinaryOperator) -> Option<FloatBinaryOperation> {
    Some(match operator {
        BinaryOperator::Add => FloatBinaryOperation::Add,
        BinaryOperator::Subtract => FloatBinaryOperation::Subtract,
        BinaryOperator::Multiply => FloatBinaryOperation::Multiply,
        BinaryOperator::Divide => FloatBinaryOperation::Divide,
        _ => return None,
    })
}

fn is_integer_arithmetic_type(ty: &Type) -> bool {
    matches!(ty, Type::I32 | Type::U32 | Type::I64 | Type::U64)
}

fn sequence_access(element_type: &Type) -> Option<(MemoryOperation, MemoryOperation, i64)> {
    let load = element_type.memory_load()?;
    Some((
        load,
        MemoryOperation::Store(load.width()),
        i64::from(load.access_width()),
    ))
}

fn is_pointer_like(ty: &Type) -> bool {
    ty.is_pointer()
}

fn machine_register_type(name: &str) -> Option<Type> {
    InterpreterRegister::from_name(name).and_then(InterpreterRegister::source_value_type)
}

fn branch_target(
    target: &CallTarget,
    values: &[Value],
    variables: &[Variable],
    indirect_condition: bool,
) -> Option<String> {
    if (indirect_condition
        || matches!(
            target,
            CallTarget::Intrinsic(
                Intrinsic::Branch(_) | Intrinsic::CheckedInteger(_) | Intrinsic::Control(ControlOperation::JumpLabel)
            )
        ))
        && let Some(value) = values.last()
    {
        return match &value.kind {
            ValueKind::Label(label) => Some(label.clone()),
            ValueKind::Variable(variable) if value.ty == Type::label() => Some(variables[*variable].name.clone()),
            _ => None,
        };
    }
    None
}

fn ast_block_is_terminal(block: &ast::Block, signatures: &HashMap<String, Signature>) -> bool {
    block
        .statements
        .last()
        .is_some_and(|statement| ast_statement_is_terminal(statement, signatures))
}

fn ast_statement_is_terminal(statement: &ast::Statement, signatures: &HashMap<String, Signature>) -> bool {
    let call_is_terminal = |callee: &str| {
        intrinsic_is_terminal(callee) || signatures.get(callee).is_some_and(|signature| signature.terminal)
    };
    match &statement.kind {
        ast::StatementKind::Expression(ast::Expression {
            kind: ExpressionKind::Call { callee, .. },
            ..
        }) => call_is_terminal(callee),
        ast::StatementKind::FallibleCall { callee, .. } => call_is_terminal(callee),
        ast::StatementKind::ContinuationJump { .. } => true,
        ast::StatementKind::If {
            then_body,
            else_body: Some(else_body),
            ..
        } => ast_block_is_terminal(then_body, signatures) && ast_block_is_terminal(else_body, signatures),
        _ => false,
    }
}

fn collect_signatures(filename: &str, program: &ast::Program) -> Result<HashMap<String, Signature>, Diagnostic> {
    let mut signatures = HashMap::default();
    for declaration in &program.declarations {
        let ast::Declaration::InlineFunction(declaration) = declaration else {
            continue;
        };
        let signature = Signature {
            parameters: declaration
                .parameters
                .iter()
                .map(|parameter| (parameter.mode, parameter.ty.clone()))
                .collect(),
            has_else_parameter: declaration.parameters.last().is_some_and(|parameter| parameter.is_else),
            return_type: declaration.return_type.clone(),
            terminal: false,
        };
        if signatures.insert(declaration.name.clone(), signature).is_some() {
            return Err(Diagnostic::new(
                filename,
                declaration.span,
                format!("duplicate inline function '{}'", declaration.name),
            ));
        }
    }

    loop {
        let mut newly_terminal = Vec::new();
        for declaration in &program.declarations {
            let ast::Declaration::InlineFunction(function) = declaration else {
                continue;
            };
            if !signatures[&function.name].terminal && ast_block_is_terminal(&function.body, &signatures) {
                newly_terminal.push(function.name.clone());
            }
        }
        if newly_terminal.is_empty() {
            break;
        }
        for name in newly_terminal {
            signatures.get_mut(&name).unwrap().terminal = true;
        }
    }
    Ok(signatures)
}

#[derive(Default)]
struct InlineCallSummary {
    callees: HashSet<usize>,
    invoked_parameters: HashSet<usize>,
}

#[derive(Clone, Copy)]
enum InlineOperationArgument {
    Function(InlineFunctionId),
    Parameter(VariableId),
}

struct InlineCallSite {
    callee: usize,
    operation_arguments: Vec<Option<InlineOperationArgument>>,
}

fn collect_inline_call_facts(
    statements: &[Statement],
    parameter_indices: &HashMap<VariableId, usize>,
    summary: &mut InlineCallSummary,
    call_sites: &mut Vec<InlineCallSite>,
) {
    for statement in statements {
        if let StatementKindIr::Call(_, call) = &statement.kind {
            match call.target {
                CallTarget::InlineFunction(callee) => {
                    summary.callees.insert(callee.index());
                    call_sites.push(InlineCallSite {
                        callee: callee.index(),
                        operation_arguments: call
                            .arguments
                            .iter()
                            .map(|argument| match argument.kind {
                                ValueKind::Operation(OperationValue::InlineFunction(function)) => {
                                    Some(InlineOperationArgument::Function(function))
                                }
                                ValueKind::Variable(variable) if parameter_indices.contains_key(&variable) => {
                                    Some(InlineOperationArgument::Parameter(variable))
                                }
                                _ => None,
                            })
                            .collect(),
                    });
                }
                CallTarget::OperationParameter(parameter) => {
                    if let Some(index) = parameter_indices.get(&parameter) {
                        summary.invoked_parameters.insert(*index);
                    }
                }
                _ => {}
            }
        }
        statement
            .kind
            .for_each_nested_body(|body| collect_inline_call_facts(body, parameter_indices, summary, call_sites));
    }
}

fn find_inline_cycle(
    function: usize,
    graph: &[Vec<usize>],
    states: &mut [u8],
    stack: &mut Vec<usize>,
) -> Option<Vec<usize>> {
    states[function] = 1;
    stack.push(function);
    for &callee in &graph[function] {
        if states[callee] == 0 {
            if let Some(cycle) = find_inline_cycle(callee, graph, states, stack) {
                return Some(cycle);
            }
        } else if states[callee] == 1 {
            let start = stack.iter().position(|candidate| *candidate == callee).unwrap();
            let mut cycle = stack[start..].to_vec();
            cycle.push(callee);
            return Some(cycle);
        }
    }
    stack.pop();
    states[function] = 2;
    None
}

fn validate_inline_call_graph(filename: &str, inline_functions: &[InlineFunction]) -> Result<(), Diagnostic> {
    let parameter_indices = inline_functions
        .iter()
        .map(|function| {
            function
                .parameters
                .iter()
                .enumerate()
                .map(|(index, parameter)| (*parameter, index))
                .collect::<HashMap<_, _>>()
        })
        .collect::<Vec<_>>();
    let mut summaries = (0..inline_functions.len())
        .map(|_| InlineCallSummary::default())
        .collect::<Vec<_>>();
    let mut call_sites = (0..inline_functions.len()).map(|_| Vec::new()).collect::<Vec<_>>();
    for (function, inline_function) in inline_functions.iter().enumerate() {
        collect_inline_call_facts(
            &inline_function.body.statements,
            &parameter_indices[function],
            &mut summaries[function],
            &mut call_sites[function],
        );
    }

    let mut invoked_parameters = Vec::new();
    loop {
        let mut changed = false;
        for (function, function_calls) in call_sites.iter().enumerate() {
            for call in function_calls {
                invoked_parameters.clear();
                invoked_parameters.extend(summaries[call.callee].invoked_parameters.iter().copied());
                for &parameter in &invoked_parameters {
                    let Some(Some(argument)) = call.operation_arguments.get(parameter) else {
                        continue;
                    };
                    match argument {
                        InlineOperationArgument::Function(callee) => {
                            changed |= summaries[function].callees.insert(callee.index());
                        }
                        InlineOperationArgument::Parameter(variable) => {
                            if let Some(parameter) = parameter_indices[function].get(variable) {
                                changed |= summaries[function].invoked_parameters.insert(*parameter);
                            }
                        }
                    }
                }
            }
        }
        if !changed {
            break;
        }
    }

    let graph = summaries
        .into_iter()
        .map(|summary| summary.callees.into_iter().collect::<Vec<_>>())
        .collect::<Vec<_>>();
    let mut states = vec![0; inline_functions.len()];
    for function in 0..inline_functions.len() {
        if states[function] != 0 {
            continue;
        }
        let Some(cycle) = find_inline_cycle(function, &graph, &mut states, &mut Vec::new()) else {
            continue;
        };
        let names = cycle
            .iter()
            .map(|function| inline_functions[*function].name.as_str())
            .collect::<Vec<_>>()
            .join(" -> ");
        let member = cycle[0];
        return Err(Diagnostic::new(
            filename,
            inline_functions[member].span,
            format!(
                "recursive inline function cycle involving '{}': {names}",
                inline_functions[member].name
            ),
        ));
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn check_body(
    filename: &str,
    signatures: &HashMap<String, Signature>,
    inline_function_ids: &HashMap<String, InlineFunctionId>,
    parameters: &[ast::Parameter],
    body: &ast::Block,
    check_outputs: bool,
    parameters_are_bytecode_fields: bool,
    layouts: &HashMap<(Type, String), LayoutField>,
    aggregate_strides: &HashMap<Type, layout::LayoutValue>,
) -> Result<(Vec<VariableId>, Body), Diagnostic> {
    let mut checker = Checker::new(filename, signatures, inline_function_ids, layouts, aggregate_strides);
    let mut parameter_ids = Vec::new();
    for parameter in parameters {
        parameter_ids.push(checker.declare(
            &parameter.name,
            parameter.ty.clone(),
            Some(parameter.mode),
            parameter.span,
        )?);
    }
    if parameters_are_bytecode_fields {
        checker.bytecode_fields.extend(parameter_ids.iter().copied());
    }
    checker.check_block(body, true)?;
    let body = Body {
        variables: checker.variables,
        statements: checker.statements,
        bytecode_fields: if parameters_are_bytecode_fields {
            parameter_ids.iter().copied().collect()
        } else {
            HashSet::default()
        },
    };
    check_definite_initialization(filename, &body, &parameter_ids, check_outputs)?;
    Ok((parameter_ids, body))
}

pub(crate) fn check(filename: &str, program: &ast::Program) -> Result<Program, Diagnostic> {
    check_with_layouts(filename, program, &[])
}

pub(crate) fn check_with_layouts(
    filename: &str,
    program: &ast::Program,
    layout_fields: &[layout::Field],
) -> Result<Program, Diagnostic> {
    let mut layouts = HashMap::default();
    let mut aggregate_strides = HashMap::default();
    let mut pairs = HashMap::default();
    let mut next_pair_id = 0u32;
    for field in layout_fields {
        let owner = field.owner.clone();
        let ty = field.ty.clone();
        let pair = field.pair.as_ref().map(|pair| {
            let (id, count) = pairs.entry((owner.clone(), pair.clone())).or_insert_with(|| {
                let id = next_pair_id;
                next_pair_id += 1;
                (id, 0u8)
            });
            let pair = FieldPair { id: *id, order: *count };
            *count += 1;
            pair
        });
        if let Some(stride) = field.stride
            && let Some(previous) = aggregate_strides.insert(owner.clone(), stride)
        {
            assert_eq!(previous, stride, "conflicting generated strides for {}", field.owner);
        }
        layouts.insert(
            (owner, field.name.clone()),
            LayoutField {
                ty,
                offset: field.offset,
                nonnull: field.nonnull,
                embedded: field.embedded,
                cell_pointer: field.cell_pointer,
                pair,
                pinned_base: field.pinned_base.clone(),
                pinned_offset: field.pinned_offset,
            },
        );
    }
    let signatures = collect_signatures(filename, program)?;
    let inline_function_ids = program
        .declarations
        .iter()
        .filter_map(|declaration| {
            let ast::Declaration::InlineFunction(declaration) = declaration else {
                return None;
            };
            Some(declaration.name.clone())
        })
        .enumerate()
        .map(|(index, name)| (name, InlineFunctionId::new(index)))
        .collect::<HashMap<_, _>>();
    let mut inline_functions = Vec::new();
    let mut handlers = Vec::new();
    let mut declaration_names = HashSet::default();
    for declaration in &program.declarations {
        match declaration {
            ast::Declaration::InlineFunction(declaration) => {
                if !declaration_names.insert(declaration.name.clone()) {
                    return Err(Diagnostic::new(
                        filename,
                        declaration.span,
                        format!("duplicate declaration '{}'", declaration.name),
                    ));
                }
                let mut ast_parameters = declaration.parameters.clone();
                let mut ast_body = declaration.body.clone();
                if let Some(return_type) = &declaration.return_type {
                    let return_value = ast_body
                        .value
                        .take()
                        .expect("the parser requires a tail expression for returning functions");
                    match (return_type, &return_value.kind) {
                        (Type::Tuple(return_types), ExpressionKind::Tuple(return_values)) => {
                            if return_types.len() != return_values.len() {
                                return Err(Diagnostic::new(
                                    filename,
                                    return_value.span,
                                    format!(
                                        "function returns {} tuple elements, but its tail expression has {}",
                                        return_types.len(),
                                        return_values.len()
                                    ),
                                ));
                            }
                            for (index, ty) in return_types.iter().enumerate().rev() {
                                let name = format!("__return_value_{index}");
                                ast_parameters.insert(
                                    0,
                                    ast::Parameter {
                                        name: name.clone(),
                                        mode: ParameterMode::Out,
                                        ty: ty.clone(),
                                        is_else: false,
                                        span: declaration.span,
                                    },
                                );
                            }
                            for (index, value) in return_values.iter().enumerate() {
                                let name = format!("__return_value_{index}");
                                ast_body.statements.push(ast::Statement {
                                    kind: ast::StatementKind::Assign {
                                        name,
                                        initializer: value.clone(),
                                    },
                                    span: declaration.span,
                                });
                            }
                        }
                        (Type::Tuple(_), _) => {
                            return Err(Diagnostic::new(
                                filename,
                                return_value.span,
                                "a tuple-returning function requires a tuple tail expression",
                            ));
                        }
                        (_, ExpressionKind::Tuple(_)) => {
                            return Err(Diagnostic::new(
                                filename,
                                return_value.span,
                                "a scalar-returning function cannot return a tuple",
                            ));
                        }
                        (_, _) => {
                            ast_parameters.insert(
                                0,
                                ast::Parameter {
                                    name: "__return_value".to_string(),
                                    mode: ParameterMode::Out,
                                    ty: return_type.clone(),
                                    is_else: false,
                                    span: declaration.span,
                                },
                            );
                            ast_body.statements.push(ast::Statement {
                                kind: ast::StatementKind::Assign {
                                    name: "__return_value".to_string(),
                                    initializer: return_value,
                                },
                                span: declaration.span,
                            });
                        }
                    }
                }
                let (parameters, body) = check_body(
                    filename,
                    &signatures,
                    &inline_function_ids,
                    &ast_parameters,
                    &ast_body,
                    true,
                    false,
                    &layouts,
                    &aggregate_strides,
                )?;
                inline_functions.push(InlineFunction {
                    id: inline_function_ids[&declaration.name],
                    name: declaration.name.clone(),
                    span: declaration.span,
                    parameters,
                    return_type: declaration.return_type.clone(),
                    body,
                });
            }
            ast::Declaration::Handler(declaration) => {
                if !declaration_names.insert(declaration.name.clone()) {
                    return Err(Diagnostic::new(
                        filename,
                        declaration.span,
                        format!("duplicate declaration '{}'", declaration.name),
                    ));
                }
                let (parameters, body) = check_body(
                    filename,
                    &signatures,
                    &inline_function_ids,
                    &declaration.parameters,
                    &declaration.body,
                    false,
                    true,
                    &layouts,
                    &aggregate_strides,
                )?;
                handlers.push(Handler {
                    name: declaration.name.clone(),
                    parameters,
                    temperature: declaration.temperature,
                    body,
                });
            }
        }
    }
    validate_inline_call_graph(filename, &inline_functions)?;
    Ok(Program {
        inline_functions,
        handlers,
    })
}

fn statement_uses_and_defs(statement: &Statement) -> (Vec<VariableId>, Vec<VariableId>) {
    let call_uses_and_defs = |call: &Call| {
        if call.intrinsic()
            == Some(Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(
                BinaryOperation::Xor,
            )))
            && call.arguments.len() == 2
            && call.arguments[0].kind == call.arguments[1].kind
            && let ValueKind::Variable(id) = call.arguments[0].kind
        {
            return (Vec::new(), vec![id]);
        }
        let mut uses = Vec::new();
        let mut defs = Vec::new();
        for (value, mode) in call.arguments.iter().zip(&call.effects) {
            if matches!(mode, ParameterMode::In | ParameterMode::InOut) {
                uses.extend(value_variable_ids(value));
            }
            if matches!(mode, ParameterMode::Out | ParameterMode::InOut)
                && let ValueKind::Variable(id) = value.kind
            {
                defs.push(id);
            }
        }
        (uses, defs)
    };
    match &statement.kind {
        StatementKindIr::Continuation { .. } => (Vec::new(), Vec::new()),
        StatementKindIr::Call(destinations, call) => {
            let (uses, mut defs) = call_uses_and_defs(call);
            defs.extend(destinations.iter().copied());
            (uses, defs)
        }
        StatementKindIr::MachineAssign { arguments, .. } => (direct_value_variables(arguments), Vec::new()),
        StatementKindIr::ValueRefinement {
            binding, value, tag, ..
        } => (direct_value_variables([value]), vec![*tag, *binding]),
        StatementKindIr::ValueMatch {
            destination,
            value,
            captures,
            ..
        } => {
            let mut uses = captures.clone();
            uses.extend(direct_value_variables([value]));
            normalize_variables(&mut uses);
            (uses, destination.iter().copied().collect())
        }
        StatementKindIr::ScalarMatch {
            value, captures, arms, ..
        } => {
            let mut uses = captures.clone();
            uses.extend(direct_value_variables([value]));
            for arm in arms {
                add_scalar_pattern_variables(&mut uses, &arm.pattern);
            }
            normalize_variables(&mut uses);
            (uses, Vec::new())
        }
        StatementKindIr::If {
            destination,
            condition,
            terminal,
            captures,
            else_body,
            ..
        } => {
            let (mut uses, mut definitions) = condition_uses_and_defs(condition);
            if else_body.is_some() {
                uses.extend(
                    captures
                        .iter()
                        .copied()
                        .filter(|capture| destination.is_some() || !definitions.contains(capture)),
                );
                definitions.extend(destination);
            } else if !terminal {
                // Conditional definitions are also uses at the join because the
                // false path must preserve values that entered the if.
                uses.extend(captures.iter().copied());
            }
            normalize_variables(&mut uses);
            (uses, definitions)
        }
        StatementKindIr::While {
            condition_setup,
            condition,
            captures,
            ..
        } => {
            let (mut uses, _) = condition_uses_and_defs(condition);
            let setup_definitions = condition_setup
                .iter()
                .flat_map(|statement| statement_uses_and_defs(statement).1)
                .collect::<HashSet<_>>();
            uses.retain(|id| !setup_definitions.contains(id));
            uses.extend(captures.iter().copied());
            normalize_variables(&mut uses);
            (uses, Vec::new())
        }
        StatementKindIr::Guard { condition, .. } => condition_uses_and_defs(condition),
    }
}

fn direct_value_variables<'a>(values: impl IntoIterator<Item = &'a Value>) -> Vec<VariableId> {
    values
        .into_iter()
        .filter_map(|value| match value.kind {
            ValueKind::Variable(id) => Some(id),
            _ => None,
        })
        .collect()
}

fn normalize_variables(variables: &mut Vec<VariableId>) {
    variables.sort_unstable();
    variables.dedup();
}

fn is_scalar_match_type(ty: &Type) -> bool {
    matches!(
        ty,
        Type::I8
            | Type::I16
            | Type::I32
            | Type::U8
            | Type::U16
            | Type::U32
            | Type::I64
            | Type::U64
            | Type::Bool
            | Type::ValueTag
    )
}

fn add_scalar_pattern_variables(variables: &mut Vec<VariableId>, pattern: &[Value]) {
    variables.extend(pattern.iter().flat_map(value_variable_ids));
}

fn value_variable_ids(value: &Value) -> Vec<VariableId> {
    match &value.kind {
        ValueKind::Variable(id) => vec![*id],
        ValueKind::Memory(components) => components.iter().flat_map(value_variable_ids).collect(),
        _ => Vec::new(),
    }
}

pub(crate) fn condition_uses_and_defs(condition: &Condition) -> (Vec<VariableId>, Vec<VariableId>) {
    match condition {
        Condition::LogicalNot(condition) => condition_uses_and_defs(condition),
        Condition::ScalarEquality { lhs, rhs, .. } => (direct_value_variables([lhs, rhs]), Vec::new()),
        Condition::ScalarRelational { lhs, rhs, .. } => (direct_value_variables([lhs, rhs]), Vec::new()),
        Condition::BitTest { value, mask, .. } => (direct_value_variables([value, mask]), Vec::new()),
        Condition::ValueTag {
            value, tag, source_tag, ..
        } => {
            let mut uses = direct_value_variables([value]);
            if let Some(source_tag) = source_tag {
                uses.push(*source_tag);
            }
            (uses, vec![*tag])
        }
        Condition::ValueBits { value, expected, .. } => (direct_value_variables([value]), vec![*expected]),
    }
}

pub(super) fn statement_is_terminal(statement: &Statement) -> bool {
    match &statement.kind {
        StatementKindIr::Call(_, call) => call.terminal,
        StatementKindIr::ValueMatch { .. } | StatementKindIr::ScalarMatch { .. } => true,
        StatementKindIr::If {
            destination: None,
            then_body,
            else_body: Some(else_body),
            ..
        } => block_is_terminal(then_body) && block_is_terminal(else_body),
        _ => false,
    }
}

pub(super) fn block_is_terminal(body: &[Statement]) -> bool {
    body.iter()
        .rev()
        .find(|statement| !matches!(statement.kind, StatementKindIr::Continuation { .. }))
        .is_some_and(statement_is_terminal)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frontend::parser;

    fn check_source(source: &str) -> Result<Program, Diagnostic> {
        let ast = parser::parse("test.flap", source)?;
        check("test.flap", &ast)
    }

    macro_rules! accepts {
        ($name:ident, $source:expr) => {
            #[test]
            fn $name() {
                check_source($source).unwrap();
            }
        };
    }

    macro_rules! rejects {
        ($name:ident, $source:expr, $message:literal) => {
            #[test]
            fn $name() {
                let error = check_source($source).unwrap_err();
                assert!(error.message.contains($message), "{}", error.message);
            }
        };
    }

    #[test]
    fn keeps_int32_refinement_in_typed_ir() {
        let program = check_source(
            r#"
inline fn unbox(value: in Value, result: out i32, fail: Label) {
    guard let Value<i32>(result) = value else fail;
}
"#,
        )
        .unwrap();

        assert!(matches!(
            program.inline_functions[0].body.statements[0].kind,
            StatementKindIr::ValueRefinement { .. }
        ));
    }

    #[test]
    fn infers_local_types_and_accepts_inline_functions() {
        let program = check_source(
            r#"
inline fn consume(value: in Value) {
    dispatch_next;
}
handler Read(src: in Operand) {
    let value = load(src);
    consume(value);
}
"#,
        )
        .unwrap();

        let initialized = &program.handlers[0].body.statements[0];
        let StatementKindIr::Call(ref destinations, ref call) = initialized.kind else {
            panic!("expected inferred initialization")
        };
        let [destination] = destinations.as_slice() else {
            panic!("expected one destination")
        };
        assert_eq!(program.handlers[0].body.variables[*destination].ty, Type::Value);
        assert_eq!(
            call.target,
            CallTarget::Intrinsic(Intrinsic::Operand(OperandOperation::Load(
                crate::intrinsic::OperandLoad::Field,
            )))
        );
        let StatementKindIr::Call(_, call) = &program.handlers[0].body.statements[1].kind else {
            panic!("expected inline function call")
        };
        assert_eq!(call.target, CallTarget::InlineFunction(program.inline_functions[0].id));
    }

    #[test]
    fn resolves_operation_parameters_by_variable_id() {
        let program = check_source(
            r#"
inline fn apply(value: i32, operation: BinaryOperation<i32>) {
    let result = value;
    operation(result, 1);
    dispatch_next;
}
"#,
        )
        .unwrap();

        let StatementKindIr::Call(_, call) = &program.inline_functions[0].body.statements[1].kind else {
            panic!("expected operation call")
        };
        assert_eq!(
            call.target,
            CallTarget::OperationParameter(program.inline_functions[0].parameters[1])
        );
    }

    #[test]
    fn distinguishes_inline_operation_values_from_function_symbols() {
        let program = check_source(
            r#"
inline fn add(lhs: inout i32, rhs: i32) {
    lhs = lhs + rhs;
}

inline fn apply(
    lhs: inout i32,
    rhs: i32,
    operation: BinaryOperation<i32>
) {
    operation(lhs, rhs);
}

inline fn use_add(lhs: inout i32, rhs: i32) {
    apply(lhs, rhs, add);
}

handler Invoke() {
    let result = call_interp(interpreter_entry);
    dispatch_next;
}
"#,
        )
        .unwrap();

        let use_add = program
            .inline_functions
            .iter()
            .find(|function| function.name == "use_add")
            .unwrap();
        let add = program
            .inline_functions
            .iter()
            .find(|function| function.name == "add")
            .unwrap();
        let StatementKindIr::Call(_, call) = &use_add.body.statements[0].kind else {
            panic!("expected inline function call")
        };
        assert!(matches!(
            call.arguments[2].kind,
            ValueKind::Operation(OperationValue::InlineFunction(id))
                if id == add.id
        ));

        let StatementKindIr::Call(_, call) = &program.handlers[0].body.statements[0].kind else {
            panic!("expected interpreter call")
        };
        assert!(matches!(
            call.arguments[0].kind,
            ValueKind::FunctionSymbol(ref name)
                if name.as_str() == "interpreter_entry"
        ));
    }

    rejects!(
        requires_tuple_returns_to_be_destructured,
        "inline fn pair(value: u32) -> (u32, u32) { (value, value) }\n\
         handler Use(value: u32) { let pair = pair(value); dispatch_next; }",
        "a tuple return value must be destructured"
    );

    rejects!(
        rejects_nonterminating_scalar_match_fallback_blocks,
        r#"
handler Dispatch(kind: u8) {
    match kind {
        FIRST_KIND => done,
        _ => {
            let value = kind;
        },
    }
    let done = || { dispatch_next; };
}
"#,
        "fallback block must terminate"
    );

    rejects!(
        rejects_multiple_hot_scalar_match_arms,
        r#"
handler Dispatch(kind: u8) {
    match kind {
        FIRST_KIND => @hot { dispatch_next; },
        SECOND_KIND => @hot { dispatch_next; },
        _ => fallback,
    }
    let fallback = || { dispatch_next; };
}
"#,
        "a scalar match can only have one @hot arm"
    );

    rejects!(
        rejects_scalar_match_on_value,
        r#"
handler Dispatch(value: Value) {
    match value {
        SOME_VALUE => matched,
        _ => fallback,
    }
    let matched = || { dispatch_next; };
    let fallback = || { dispatch_next; };
}
"#,
        "cannot scalar-match Value"
    );

    #[test]
    fn records_checked_add_continuation_captures() {
        let program = check_source(
            r#"
handler Add(lhs: i32, rhs: i32) {
    let dst = box_i32(lhs);
    guard let sum = checked_add(lhs, rhs) else @cold {
        dst = box_i32(lhs);
        dispatch_next;
    };
    dst = box_i32(sum);
    dispatch_next;
}
"#,
        )
        .unwrap();

        let StatementKindIr::Call(destinations, call) = &program.handlers[0].body.statements[1].kind else {
            panic!("expected checked add")
        };
        let failure = call.failure.as_ref().expect("expected structured failure");
        assert_eq!(program.handlers[0].body.variables[destinations[0]].ty, Type::I32);
        assert_eq!(
            call.intrinsic(),
            Some(Intrinsic::CheckedInteger(
                crate::intrinsic::CheckedIntegerOperation::Add,
            ))
        );
        assert_eq!(failure.captures, &[0, 2]);
    }

    rejects!(
        rejects_nonterminating_checked_add_continuations,
        r#"
handler Add(lhs: i32, rhs: i32) {
    guard let sum = checked_add(lhs, rhs) else @cold {
        let temporary: i32;
    };
    dispatch_next;
}
"#,
        "else continuation must terminate control flow"
    );

    #[test]
    fn requires_else_syntax_only_for_declared_else_parameters() {
        let missing_else = check_source(
            r#"
inline fn checked_identity(value: i32, else failure: Label) -> i32 {
    guard value != 0 else failure;
    value
}
handler Check(value: i32) {
    let result = checked_identity(value);
    dispatch_next;
}
"#,
        )
        .unwrap_err();
        assert_eq!(missing_else.message, "'checked_identity' requires an else continuation");

        let unexpected_else = check_source(
            r#"
inline fn identity(value: i32) -> i32 { value }
handler Check(value: i32) {
    guard let result = identity(value) else @cold { call_slow_path(check); };
    dispatch_next;
}
"#,
        )
        .unwrap_err();
        assert_eq!(
            unexpected_else.message,
            "'identity' does not accept an else continuation"
        );
    }

    rejects!(
        gives_blocks_lexical_scope,
        r#"
handler Add() {
    let number: f64;
    if true {
        let integer: i32;
        mov(integer, 0);
        let inner_number = to_f64(integer);
    }
    number = to_f64(integer);
    dispatch_next;
}
"#,
        "unknown name 'integer'"
    );

    #[test]
    fn gives_continuations_lexical_scope() {
        let program = check_source(
            r#"
handler Add() {
    if true {
        goto done;
        let done = || { dispatch_next; };
    }
    if true {
        goto done;
        let done = || { dispatch_next; };
    }
}
"#,
        )
        .unwrap();
        fn collect_labels(statements: &[Statement], labels: &mut Vec<String>) {
            for statement in statements {
                if let StatementKindIr::Continuation { name, .. } = &statement.kind {
                    labels.push(name.clone());
                }
                statement.kind.for_each_nested_body(|body| collect_labels(body, labels));
            }
        }
        let mut labels = Vec::new();
        collect_labels(&program.handlers[0].body.statements, &mut labels);

        assert_eq!(labels.len(), 2);
        assert_ne!(labels[0], labels[1]);
    }

    rejects!(
        rejects_writing_through_in_parameter,
        r#"
inline fn bad(value: in Value) {
    canonicalize_nan(value, 0);
}
"#,
        "cannot write through an in Value parameter"
    );

    #[test]
    fn rejects_field_assignment_through_in_parameter() {
        let layouts = crate::frontend::layout::LayoutDatabase::parse(
            "const PROGRAM_COUNTER = 0\nfield ExecutionContext.program_counter u32 PROGRAM_COUNTER nullable scalar\n",
        )
        .unwrap();
        let ast = parser::parse(
            "test.flap",
            r#"
inline fn bad(frame: in ExecutionContext) {
    frame.program_counter = 0;
}
"#,
        )
        .unwrap();
        let error = check_with_layouts("test.flap", &ast, layouts.fields()).unwrap_err();

        assert!(
            error
                .message
                .contains("cannot write through an in ExecutionContext parameter")
        );
    }

    #[test]
    fn accepts_boolean_literals() {
        let layouts = crate::frontend::layout::LayoutDatabase::parse(
            "const YIELD_IS_AWAIT = 0\nfield ExecutionContext.yield_is_await bool YIELD_IS_AWAIT nullable scalar\n",
        )
        .unwrap();
        let ast = parser::parse(
            "test.flap",
            r#"
handler Initialize(address: u64) {
    let frame: ExecutionContext = alias(address);
    frame.yield_is_await = false;
    frame.yield_is_await = true;
    dispatch_next;
}
"#,
        )
        .unwrap();

        check_with_layouts("test.flap", &ast, layouts.fields()).unwrap();
    }

    #[test]
    fn rejects_definitely_null_binding_store_to_nonnull_field() {
        let layouts = crate::frontend::layout::LayoutDatabase::parse(
            "const SHAPE = 0\nfield Object.shape Shape SHAPE nonnull cell\n",
        )
        .unwrap();
        let ast = parser::parse(
            "test.flap",
            r#"
handler StoreNull(address: u64) {
    let object: Object = alias(address);
    let shape: Shape = null;
    object.shape = shape;
    dispatch_next;
}
"#,
        )
        .unwrap();
        let error = check_with_layouts("test.flap", &ast, layouts.fields()).unwrap_err();

        assert!(
            error
                .message
                .contains("cannot assign null to non-null field 'Object.shape'"),
            "{}",
            error.message
        );
    }

    #[test]
    fn rejects_null_binding_store_after_conditional_assignment() {
        let layouts = crate::frontend::layout::LayoutDatabase::parse(
            "const SHAPE = 0\nfield Object.shape Shape SHAPE nonnull cell\n",
        )
        .unwrap();
        let ast = parser::parse(
            "test.flap",
            r#"
handler StoreNull(address: u64, flag: u32) {
    let object: Object = alias(address);
    let shape: Shape = null;
    if load(flag) == 0 {
        shape = object.shape;
    } else {
    }
    object.shape = shape;
    dispatch_next;
}
"#,
        )
        .unwrap();
        let error = check_with_layouts("test.flap", &ast, layouts.fields()).unwrap_err();

        assert!(
            error
                .message
                .contains("cannot assign null to non-null field 'Object.shape'"),
            "{}",
            error.message
        );
    }

    #[test]
    fn accepts_reassigned_null_binding_store_to_nonnull_field() {
        let layouts = crate::frontend::layout::LayoutDatabase::parse(
            "const SHAPE = 0\nfield Object.shape Shape SHAPE nonnull cell\n",
        )
        .unwrap();
        let ast = parser::parse(
            "test.flap",
            r#"
handler StoreShape(address: u64) {
    let object: Object = alias(address);
    let shape: Shape = null;
    shape = object.shape;
    object.shape = shape;
    dispatch_next;
}
"#,
        )
        .unwrap();

        check_with_layouts("test.flap", &ast, layouts.fields()).unwrap();
    }

    #[test]
    fn does_not_apply_pointer_nonzero_checks_to_byte_fields() {
        let layouts = crate::frontend::layout::LayoutDatabase::parse(
            "const FLAG = 0\nfield ExecutionContext.flag bool FLAG nonnull scalar\n",
        )
        .unwrap();
        let ast = parser::parse(
            "test.flap",
            r#"
handler Read(address: u64) {
    let frame: ExecutionContext = alias(address);
    let flag = frame.flag;
    assert_nonzero(flag);
    dispatch_next;
}
"#,
        )
        .unwrap();
        let program = check_with_layouts("test.flap", &ast, layouts.fields()).unwrap();
        let field_access = program.handlers[0]
            .body
            .statements
            .iter()
            .find_map(|statement| match &statement.kind {
                StatementKindIr::Call(_, call) => call.field_access(),
                _ => None,
            })
            .unwrap();

        assert_eq!(
            field_access.kind,
            FieldAccessKind::Load {
                width: FieldWidth::U8,
                nonzero: false,
            }
        );
    }

    #[test]
    fn routes_cell_pointer_fields_through_cell_pointer_loads() {
        let layouts = crate::frontend::layout::LayoutDatabase::parse(
            "const SHAPE = 0\nfield Object.shape Shape SHAPE nonnull cell\n",
        )
        .unwrap();
        let ast = parser::parse(
            "test.flap",
            r#"
handler Read(address: u64) {
    let object: Object = alias(address);
    let shape = object.shape;
    assert_nonzero(shape);
    dispatch_next;
}
"#,
        )
        .unwrap();
        let program = check_with_layouts("test.flap", &ast, layouts.fields()).unwrap();
        let field_access = program.handlers[0]
            .body
            .statements
            .iter()
            .find_map(|statement| match &statement.kind {
                StatementKindIr::Call(_, call) => call.field_access(),
                _ => None,
            })
            .unwrap();

        assert_eq!(field_access.kind, FieldAccessKind::LoadCellPointer { nonzero: true });
    }

    accepts!(
        explicitly_widens_refined_values,
        r#"
inline fn box(object: Object) -> Value<Object> {
    let value: Value<Object>;
    mov(value, object);
    value
}

handler Store(dst: out Operand, object: Object) {
    let value = box(object);
    let widened: Value = alias(value);
    store(dst, widened);
    dispatch_next;
}
"#
    );

    accepts!(
        explicitly_widens_boolean_values_to_u64,
        r#"
handler Box(dst: out Operand, value: bool) {
    let payload: u64 = alias(value);
    let bits = payload | BOOLEAN_FALSE;
    store(dst, reinterpret_u64_as_value(bits));
    dispatch_next;
}
"#
    );

    rejects!(
        rejects_implicit_numeric_promotion,
        "handler Bad(value: u8) { let widened: u64 = value; dispatch_next; }",
        "cannot initialize 'widened' of type u64 with u8"
    );

    rejects!(
        rejects_aliasing_integer_literals_to_pointer_types,
        "handler Bad() { let raw: u64 = 1; let object: Object = alias(raw); dispatch_next; }",
        "cannot alias integer literal as Object"
    );

    rejects!(
        rejects_aliasing_conditionally_assigned_integer_literals_to_pointer_types,
        r#"
handler Bad(address: u64, flag: u32) {
    let raw: u64 = 1;
    if load(flag) == 0 {
        raw = address;
    } else {
    }
    let object: Object = alias(raw);
    dispatch_next;
}
"#,
        "cannot alias integer literal as Object"
    );

    accepts!(
        accepts_aliasing_reassigned_integer_variables_to_pointer_types,
        r#"
handler Good(address: u64) {
    let raw: u64 = 1;
    raw = address;
    let object: Object = alias(raw);
    assert_nonzero(object);
    dispatch_next;
}
"#
    );

    accepts!(
        checks_expected_typed_integer_arithmetic,
        r#"
handler FrameSize(slot_count: u32) {
    let wide_slot_count: u64 = alias(slot_count);
    let bytes = wide_slot_count * 8 + 120;
    assert_nonzero(bytes);
    dispatch_next;
}
"#
    );

    accepts!(
        checks_value_tag_bitwise_arithmetic,
        r#"
handler Mask(value: Value) {
    let tag = extract_tag(value);
    tag = tag & NAN_BASE_TAG;
    dispatch_next;
}
"#
    );

    rejects!(
        rejects_reading_through_out_operand,
        r#"
handler Bad(dst: out Operand) {
    let value = load(dst);
    dispatch_next;
}
"#,
        "cannot read through an out Operand parameter"
    );

    rejects!(
        rejects_writing_through_in_operand,
        r#"
handler Bad(src: in Operand) {
    let value = load(src);
    store(src, value);
    dispatch_next;
}
"#,
        "cannot write through an in Operand parameter"
    );

    rejects!(
        rejects_reading_out_parameter_before_assignment,
        r#"
inline fn bad(value: out Value, dst: out Operand) {
    store(dst, value);
}
"#,
        "read before it is initialized"
    );

    rejects!(
        rejects_uninitialized_output_on_returning_path,
        r#"
inline fn bad(value: out Value) {
    let temporary: i32;
}
"#,
        "is not initialized on this returning path"
    );

    rejects!(
        rejects_partially_initialized_output_at_if_join,
        r#"
inline fn bad(value: out Value, tag: in ValueTag, number: in f64) {
    if tag != INT32_TAG {
        canonicalize_nan(value, number);
    }
}
"#,
        "read before it is initialized"
    );

    accepts!(
        permits_uninitialized_output_on_terminal_path,
        r#"
inline fn terminal(value: out Value, slow: SlowPath) {
    call_slow_path(slow);
}
"#
    );

    accepts!(
        permits_uninitialized_output_on_terminal_match_paths,
        r#"
inline fn terminal(value: out Value, input: Value, slow: SlowPath) {
    match input {
        Value<i32>(integer) => {
            call_slow_path(slow);
        },
        _ => {
            call_slow_path(slow);
        },
    }
}
"#
    );

    rejects!(
        rejects_local_values_used_as_external_slow_paths,
        "handler Bad(slow: Value) { call_slow_path(slow); }",
        "expected SlowPath, found Value for 'slow'"
    );

    accepts!(
        treats_goto_handler_as_terminal_in_continuations,
        r#"
handler Jump(target: BytecodeOffset) {
    let take = || {
        let target_offset: BytecodeOffset;
        load_label(target_offset, target);
        goto_handler(target_offset);
    };
    goto take;
}
"#
    );

    accepts!(
        treats_exit_as_terminal_in_continuations,
        r#"
handler Exit() {
    let leave = || @cold {
        exit;
    };
    goto leave;
}
"#
    );

    accepts!(
        treats_variable_dispatch_as_terminal_in_continuations,
        r#"
handler Dispatch(length: u32) {
    let finish = || {
        dispatch_variable(length);
    };
    goto finish;
}
"#
    );

    accepts!(
        propagates_terminal_control_flow_through_inline_functions,
        r#"
inline fn forward(target: Label) {
    finish_at(target);
}

inline fn finish_at(target: Label) {
    goto target;
}

handler Jump(target: Label) {
    let finish = || {
        forward(target);
    };
    goto finish;
}
"#
    );

    rejects!(
        rejects_directly_recursive_inline_functions,
        r#"
inline fn recurse(value: i32) -> i32 {
    recurse(value)
}
"#,
        "recursive inline function cycle involving 'recurse'"
    );

    rejects!(
        rejects_mutually_recursive_inline_functions,
        r#"
inline fn first(value: i32) -> i32 {
    second(value)
}

inline fn second(value: i32) -> i32 {
    first(value)
}
"#,
        "recursive inline function cycle involving"
    );

    rejects!(
        rejects_recursion_through_an_operation_parameter,
        r#"
inline fn apply(lhs: inout i32, rhs: i32, operation: BinaryOperation<i32>) {
    operation(lhs, rhs);
}

inline fn recurse(lhs: inout i32, rhs: i32) {
    apply(lhs, rhs, recurse);
}

handler Loop(lhs: i32, rhs: i32) {
    let value = lhs;
    recurse(value, rhs);
    assert_nonzero(value);
    dispatch_next;
}
"#,
        "recursive inline function cycle involving 'recurse': recurse -> recurse"
    );

    #[test]
    fn propagates_terminal_control_flow_through_fallible_calls() {
        let program = check_source(
            r#"
inline fn transfer(else failure: Label) {
    goto failure;
}

inline fn finish() {
    transfer() else {
        exit;
    };
}

handler Finish() {
    finish();
}
"#,
        )
        .unwrap();

        let StatementKindIr::Call(_, call) = &program.handlers[0].body.statements[0].kind else {
            panic!("expected terminal inline call")
        };
        assert!(call.terminal);
    }

    #[test]
    fn checks_parameterized_continuation_invocations() {
        check_source(
            r#"
handler Good(value: i32) {
    let finish = |raw: i32| {
        dispatch_next;
    };
    guard value == 0 else finish(value);
    goto finish(value);
}
"#,
        )
        .unwrap();

        let missing_argument = check_source(
            r#"
handler Bad() {
    let finish = |raw: i32| {
        dispatch_next;
    };
    goto finish;
}
"#,
        )
        .unwrap_err();
        assert!(missing_argument.message.contains("expects 1 argument, found 0"));

        let bare_target = check_source(
            r#"
handler Bad(value: i32) {
    let finish = |raw: i32| {
        dispatch_next;
    };
    branch_zero(value, finish);
    dispatch_next;
}
"#,
        )
        .unwrap_err();
        assert_eq!(
            bare_target.message,
            "expected Label, found Continuation<(i32)> for 'finish'"
        );
    }

    #[test]
    fn records_continuation_parameters_and_lexical_captures() {
        let program = check_source(
            r#"
handler Finish(value: i32, other: i32) {
    let finish = |raw: i32| @cold {
        assert_nonzero(raw);
        assert_nonzero(other);
        dispatch_next;
    };
    goto finish(value);
}
"#,
        )
        .unwrap();

        let StatementKindIr::Continuation {
            temperature,
            parameters,
            captures,
            ..
        } = &program.handlers[0]
            .body
            .statements
            .iter()
            .find(|statement| matches!(statement.kind, StatementKindIr::Continuation { .. }))
            .unwrap()
            .kind
        else {
            panic!("expected a continuation")
        };
        assert_eq!(*temperature, BlockTemperature::Cold);
        assert_eq!(parameters, &[2]);
        assert_eq!(captures, &[1]);
    }

    rejects!(
        rejects_incompatible_inline_function_argument_type,
        r#"
inline fn consume(value: in Value) {
    dispatch_next;
}
handler Bad(value: i32) {
    consume(value);
}
"#,
        "expected Value, found i32"
    );

    #[test]
    fn rejects_use_before_definition_and_duplicate_bindings() {
        let use_error = check_source(
            r#"
handler Bad(dst: out Operand) {
    let value: Value;
    store(dst, value);
}
"#,
        )
        .unwrap_err();
        assert!(use_error.message.contains("read before it is initialized"));

        check_source(
            r#"
handler Bad(value: Value) {
    if true {
        let value: Value;
    }
    dispatch_next;
}
"#,
        )
        .unwrap();

        let duplicate_error = check_source(
            r#"
handler Bad() {
    let value: Value;
    let value: Value;
    dispatch_next;
}
"#,
        )
        .unwrap_err();
        assert!(duplicate_error.message.contains("duplicate binding 'value'"));
    }

    accepts!(
        treats_self_xor_as_initialization,
        r#"
handler Zero() {
    let value: u64;
    xor(value, value);
    store64([values, 0], value);
    dispatch_next;
}
"#
    );
}
