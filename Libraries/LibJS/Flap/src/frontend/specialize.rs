/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Expansion of declarative bytecode specializations.

use super::ast::{
    Block, Declaration, ElseContinuation, Expression, ExpressionKind, HandlerDeclaration, Parameter, Pattern, Program,
    ScalarMatchArm, Statement, StatementKind, ValueMatchArm,
};
use super::diagnostic::{Diagnostic, SourceLocation, SourceSpan};
use crate::hash::HashMap;
use crate::metadata::{SpecializedInstruction, SpecializedParameterBinding};
use crate::types::{BlockTemperature, ParameterMode, Type};

pub(crate) fn expand_declarative_specializations(
    filename: &str,
    program: &mut Program,
    specializations: &[SpecializedInstruction],
) -> Result<(), Diagnostic> {
    if specializations.is_empty() {
        return Ok(());
    }

    let fallback_span = program
        .declarations
        .iter()
        .find_map(|declaration| match declaration {
            Declaration::Handler(handler) => Some(handler.span),
            Declaration::InlineFunction(_) => None,
        })
        .ok_or_else(|| {
            let start = SourceLocation {
                offset: 0,
                line: 1,
                column: 1,
            };
            Diagnostic::new(
                filename,
                SourceSpan { start, end: start },
                "specializations require at least one handler",
            )
        })?;
    let handlers = program
        .declarations
        .iter()
        .filter_map(|declaration| {
            let Declaration::Handler(handler) = declaration else {
                return None;
            };
            Some((handler.name.clone(), handler.clone()))
        })
        .collect::<HashMap<_, _>>();

    for specialization in specializations {
        let mut parameters = Vec::<Parameter>::new();
        for field in &specialization.definition.fields {
            let field_name = field.name.strip_prefix("m_").unwrap_or(&field.name);
            let source_parameter = specialization.components.iter().find_map(|component| {
                let handler = handlers.get(&component.bytecode)?;
                component.parameters.iter().find_map(|(source_name, binding)| {
                    if binding != &SpecializedParameterBinding::Field(field_name.to_string()) {
                        return None;
                    }
                    handler
                        .parameters
                        .iter()
                        .find(|parameter| parameter.name == *source_name)
                })
            });
            let Some(source_parameter) = source_parameter else {
                return Err(Diagnostic::new(
                    filename,
                    fallback_span,
                    format!(
                        "specialized field '{}.{}' has no source parameter",
                        specialization.definition.name, field_name
                    ),
                ));
            };
            parameters.push(Parameter {
                name: field_name.to_string(),
                mode: source_parameter.mode,
                ty: if field.ty == "i32" {
                    Type::InlineInt32
                } else {
                    source_parameter.ty.clone()
                },
                is_else: false,
                span: source_parameter.span,
            });
        }

        let mut component_bodies = Vec::with_capacity(specialization.components.len());
        for component in &specialization.components {
            let handler = handlers.get(&component.bytecode).ok_or_else(|| {
                Diagnostic::new(
                    filename,
                    fallback_span,
                    format!("specialization references unknown handler '{}'", component.bytecode),
                )
            })?;
            for (source_name, binding) in &component.parameters {
                if binding != &SpecializedParameterBinding::Undefined {
                    continue;
                }
                let source_parameter = handler
                    .parameters
                    .iter()
                    .find(|parameter| parameter.name == *source_name)
                    .ok_or_else(|| {
                        Diagnostic::new(
                            filename,
                            handler.span,
                            format!(
                                "specialization parameter '{}.{source_name}' has no source parameter",
                                component.bytecode
                            ),
                        )
                    })?;
                if source_parameter.mode != ParameterMode::In {
                    return Err(Diagnostic::new(
                        filename,
                        source_parameter.span,
                        format!(
                            "cannot constrain output parameter '{}.{source_name}' to Undefined",
                            component.bytecode
                        ),
                    ));
                }
            }
            let substitutions = component
                .parameters
                .iter()
                .map(|(source_name, binding)| {
                    let expression = match binding {
                        SpecializedParameterBinding::Field(name) => ExpressionKind::Name(name.clone()),
                        SpecializedParameterBinding::Undefined => ExpressionKind::Name("Value<Undefined>".to_string()),
                    };
                    (source_name.clone(), expression)
                })
                .collect::<HashMap<_, _>>();
            let mut body = handler.body.clone();
            substitute_block_parameters(&mut body, &substitutions);
            component_bodies.push(body);
        }

        let span = component_bodies.first().map(|body| body.span).unwrap_or(fallback_span);
        let body = if component_bodies.len() == 1 {
            component_bodies.pop().unwrap()
        } else {
            let continuation_names = (0..component_bodies.len())
                .map(|index| format!("__specialization_component_{index}"))
                .collect::<Vec<_>>();
            let mut statements = Vec::with_capacity(component_bodies.len() + 1);
            for (index, mut body) in component_bodies.into_iter().enumerate() {
                if let Some(next) = continuation_names.get(index + 1) {
                    replace_dispatch_next(&mut body, next);
                }
                statements.push(Statement::new(
                    StatementKind::Continuation {
                        name: continuation_names[index].clone(),
                        temperature: BlockTemperature::Default,
                        parameters: Vec::new(),
                        body,
                    },
                    span,
                ));
            }
            statements.push(Statement::new(
                StatementKind::ContinuationJump {
                    target: continuation_names[0].clone(),
                    arguments: Vec::new(),
                },
                span,
            ));
            Block {
                statements,
                value: None,
                span,
            }
        };
        program.declarations.push(Declaration::Handler(HandlerDeclaration {
            name: specialization.definition.name.clone(),
            parameters,
            temperature: BlockTemperature::Default,
            body,
            span,
        }));
    }
    Ok(())
}

fn substitute_expression(expression: &mut Expression, substitutions: &HashMap<String, ExpressionKind>) {
    match &mut expression.kind {
        ExpressionKind::Name(name) => {
            if let Some(replacement) = substitutions.get(name) {
                expression.kind = replacement.clone();
            }
        }
        ExpressionKind::Call { callee, arguments } => {
            if callee == "load"
                && let [
                    Expression {
                        kind: ExpressionKind::Name(name),
                        ..
                    },
                ] = arguments.as_slice()
                && let Some(ExpressionKind::Name(replacement)) = substitutions.get(name)
                && replacement.starts_with("Value<")
            {
                expression.kind = ExpressionKind::Name(replacement.clone());
                return;
            }
            for argument in arguments {
                substitute_expression(argument, substitutions);
            }
        }
        ExpressionKind::Memory(arguments) | ExpressionKind::Tuple(arguments) => {
            for argument in arguments {
                substitute_expression(argument, substitutions);
            }
        }
        ExpressionKind::Unary { operand, .. } => substitute_expression(operand, substitutions),
        ExpressionKind::Binary { lhs, rhs, .. } => {
            substitute_expression(lhs, substitutions);
            substitute_expression(rhs, substitutions);
        }
        ExpressionKind::Index { base, index } => {
            substitute_expression(base, substitutions);
            substitute_expression(index, substitutions);
        }
        ExpressionKind::Integer(_) => {}
    }
}

fn substitute_pattern(pattern: &mut Pattern, substitutions: &HashMap<String, ExpressionKind>) {
    match pattern {
        Pattern::Tuple(patterns) | Pattern::Alternatives(patterns) => {
            for pattern in patterns {
                substitute_pattern(pattern, substitutions);
            }
        }
        Pattern::Expression(expression) => substitute_expression(expression, substitutions),
        Pattern::ValueRepresentation { binding, .. } => {
            if let Some(binding) = binding {
                substitute_pattern(binding, substitutions);
            }
        }
        Pattern::Binding { .. } | Pattern::Wildcard => {}
    }
}

fn substitute_else_continuation(continuation: &mut ElseContinuation, substitutions: &HashMap<String, ExpressionKind>) {
    match continuation {
        ElseContinuation::Label(_) => {}
        ElseContinuation::Invocation { arguments, .. } => {
            for argument in arguments {
                substitute_expression(argument, substitutions);
            }
        }
        ElseContinuation::Block { body, .. } => substitute_block_parameters(body, substitutions),
    }
}

fn substitute_block_parameters(block: &mut Block, substitutions: &HashMap<String, ExpressionKind>) {
    for statement in &mut block.statements {
        match &mut statement.kind {
            StatementKind::Let { pattern, initializer } => {
                substitute_pattern(pattern, substitutions);
                if let Some(initializer) = initializer {
                    substitute_expression(initializer, substitutions);
                }
            }
            StatementKind::GuardLet {
                pattern,
                initializer,
                failure,
            } => {
                substitute_pattern(pattern, substitutions);
                substitute_expression(initializer, substitutions);
                substitute_else_continuation(failure, substitutions);
            }
            StatementKind::FallibleCall { arguments, failure, .. } => {
                for argument in arguments {
                    substitute_expression(argument, substitutions);
                }
                substitute_else_continuation(failure, substitutions);
            }
            StatementKind::Assign { name, initializer } => {
                if let Some(ExpressionKind::Name(replacement)) = substitutions.get(name) {
                    *name = replacement.clone();
                }
                substitute_expression(initializer, substitutions);
            }
            StatementKind::IndexAssign { base, index, value } => {
                substitute_expression(base, substitutions);
                substitute_expression(index, substitutions);
                substitute_expression(value, substitutions);
            }
            StatementKind::Expression(expression) => substitute_expression(expression, substitutions),
            StatementKind::ValueMatch {
                value, arms, fallback, ..
            } => {
                substitute_expression(value, substitutions);
                for ValueMatchArm { body, .. } in arms {
                    substitute_block_parameters(body, substitutions);
                }
                substitute_block_parameters(&mut fallback.body, substitutions);
            }
            StatementKind::ScalarMatch { value, arms, fallback } => {
                substitute_expression(value, substitutions);
                for ScalarMatchArm { pattern, body, .. } in arms {
                    substitute_pattern(pattern, substitutions);
                    substitute_block_parameters(body, substitutions);
                }
                substitute_block_parameters(fallback, substitutions);
            }
            StatementKind::Guard { condition, failure } => {
                substitute_expression(condition, substitutions);
                substitute_else_continuation(failure, substitutions);
            }
            StatementKind::Continuation { body, .. } | StatementKind::While { body, .. } => {
                substitute_block_parameters(body, substitutions);
                if let StatementKind::While { condition, .. } = &mut statement.kind {
                    substitute_expression(condition, substitutions);
                }
            }
            StatementKind::ContinuationJump { arguments, .. } => {
                for argument in arguments {
                    substitute_expression(argument, substitutions);
                }
            }
            StatementKind::If {
                condition,
                then_body,
                else_body,
                ..
            } => {
                substitute_expression(condition, substitutions);
                substitute_block_parameters(then_body, substitutions);
                if let Some(else_body) = else_body {
                    substitute_block_parameters(else_body, substitutions);
                }
            }
        }
    }
    if let Some(value) = &mut block.value {
        substitute_expression(value, substitutions);
    }
}

fn replace_dispatch_next(block: &mut Block, next: &str) {
    for statement in &mut block.statements {
        if matches!(
            &statement.kind,
            StatementKind::Expression(Expression {
                kind: ExpressionKind::Name(name),
                ..
            }) if name == "dispatch_next"
        ) || matches!(
            &statement.kind,
            StatementKind::Expression(Expression {
                kind: ExpressionKind::Call { callee, arguments },
                ..
            }) if callee == "dispatch_next" && arguments.is_empty()
        ) {
            statement.kind = StatementKind::ContinuationJump {
                target: next.to_string(),
                arguments: Vec::new(),
            };
            continue;
        }
        match &mut statement.kind {
            StatementKind::GuardLet { failure, .. }
            | StatementKind::FallibleCall { failure, .. }
            | StatementKind::Guard { failure, .. } => replace_dispatch_in_else(failure, next),
            StatementKind::ValueMatch { arms, fallback, .. } => {
                for arm in arms {
                    replace_dispatch_next(&mut arm.body, next);
                }
                replace_dispatch_next(&mut fallback.body, next);
            }
            StatementKind::ScalarMatch { arms, fallback, .. } => {
                for arm in arms {
                    replace_dispatch_next(&mut arm.body, next);
                }
                replace_dispatch_next(fallback, next);
            }
            StatementKind::Continuation { body, .. } | StatementKind::While { body, .. } => {
                replace_dispatch_next(body, next);
            }
            StatementKind::If {
                then_body, else_body, ..
            } => {
                replace_dispatch_next(then_body, next);
                if let Some(else_body) = else_body {
                    replace_dispatch_next(else_body, next);
                }
            }
            _ => {}
        }
    }
}

fn replace_dispatch_in_else(continuation: &mut ElseContinuation, next: &str) {
    if let ElseContinuation::Block { body, .. } = continuation {
        replace_dispatch_next(body, next);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_handlerless_programs_without_specializations() {
        let mut program = Program {
            declarations: Vec::new(),
        };

        expand_declarative_specializations("test.flap", &mut program, &[]).unwrap();
    }

    #[test]
    fn rejects_specializations_without_handlers() {
        let mut program = Program {
            declarations: Vec::new(),
        };
        let specializations = [SpecializedInstruction {
            definition: crate::metadata::InstructionDefinition {
                name: "Specialized".to_string(),
                parent: "Instruction".to_string(),
                fields: Vec::new(),
                is_terminator: false,
                layout: Default::default(),
                array: None,
            },
            components: Vec::new(),
        }];

        let error = expand_declarative_specializations("test.flap", &mut program, &specializations).unwrap_err();

        assert_eq!(error.message, "specializations require at least one handler");
        assert_eq!(error.span.start.line, 1);
        assert_eq!(error.span.start.column, 1);
    }

    #[test]
    fn diagnoses_specialization_fields_without_source_parameters() {
        let mut program =
            super::super::parser::parse("test.flap", "handler Source(actual: Operand) { dispatch_next; }").unwrap();
        let specializations = [SpecializedInstruction {
            definition: crate::metadata::InstructionDefinition {
                name: "Specialized".to_string(),
                parent: "Instruction".to_string(),
                fields: vec![crate::metadata::Field {
                    name: "m_field".to_string(),
                    ty: "Operand".to_string(),
                    is_array: false,
                }],
                is_terminator: false,
                layout: Default::default(),
                array: None,
            },
            components: vec![crate::metadata::SpecializedComponentLayout {
                bytecode: "Source".to_string(),
                parameters: std::collections::HashMap::from([(
                    "missing".to_string(),
                    SpecializedParameterBinding::Field("field".to_string()),
                )]),
            }],
        }];

        let error = expand_declarative_specializations("test.flap", &mut program, &specializations).unwrap_err();

        assert_eq!(
            error.message,
            "specialized field 'Specialized.field' has no source parameter"
        );
    }
}
