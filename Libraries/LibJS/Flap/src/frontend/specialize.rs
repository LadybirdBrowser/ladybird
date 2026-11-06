/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Expansion of declarative bytecode specializations.

use super::ast::{
    Block, Declaration, ElseContinuation, Expression, ExpressionKind, HandlerDeclaration, Parameter, Pattern, Program,
    ScalarMatchArm, StatementKind, ValueMatchArm,
};
use super::diagnostic::{Diagnostic, SourceLocation, SourceSpan};
use crate::hash::HashMap;
use crate::metadata::{SpecializedInstruction, SpecializedParameterBinding};
use crate::types::{ParameterMode, Type};

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
            let source_parameter = handlers.get(&specialization.bytecode).and_then(|handler| {
                specialization.parameters.iter().find_map(|(source_name, binding)| {
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

        let handler = handlers.get(&specialization.bytecode).ok_or_else(|| {
            Diagnostic::new(
                filename,
                fallback_span,
                format!(
                    "specialization references unknown handler '{}'",
                    specialization.bytecode
                ),
            )
        })?;
        for (source_name, binding) in &specialization.parameters {
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
                            specialization.bytecode
                        ),
                    )
                })?;
            if source_parameter.mode != ParameterMode::In {
                return Err(Diagnostic::new(
                    filename,
                    source_parameter.span,
                    format!(
                        "cannot constrain output parameter '{}.{source_name}' to Undefined",
                        specialization.bytecode
                    ),
                ));
            }
        }
        let substitutions = specialization
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
        program.declarations.push(Declaration::Handler(HandlerDeclaration {
            name: specialization.definition.name.clone(),
            parameters,
            temperature: handler.temperature,
            span: body.span,
            body,
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::BlockTemperature;

    #[test]
    fn preserves_handler_temperature() {
        let source = "handler Source(src: Operand) @cold { dispatch_next; }";
        let mut program = super::super::parser::parse("test.flap", source).unwrap();
        let operations = crate::metadata::parse_flap_metadata("test.flap", source).unwrap();
        let declarations =
            crate::metadata::parse_specializations("test.flap", "specialize Source(src: Int32);").unwrap();
        let specializations = crate::metadata::derive_specialized_instructions(&operations, &declarations).unwrap();

        expand_declarative_specializations("test.flap", &mut program, &specializations).unwrap();

        let Some(Declaration::Handler(specialized)) = program.declarations.last() else {
            panic!("expected specialized handler");
        };
        assert_eq!(specialized.name, "SourceSrcInt32");
        assert_eq!(specialized.temperature, BlockTemperature::Cold);
    }

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
                falls_through: false,
                layout: Default::default(),
                array: None,
                opcode: None,
                is_fallback: false,
            },
            bytecode: String::new(),
            parameters: std::collections::HashMap::new(),
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
                    mode: crate::metadata::ParameterMode::In,
                }],
                is_terminator: false,
                falls_through: false,
                layout: Default::default(),
                array: None,
                opcode: None,
                is_fallback: false,
            },
            bytecode: "Source".to_string(),
            parameters: std::collections::HashMap::from([(
                "missing".to_string(),
                SpecializedParameterBinding::Field("field".to_string()),
            )]),
        }];

        let error = expand_declarative_specializations("test.flap", &mut program, &specializations).unwrap_err();

        assert_eq!(
            error.message,
            "specialized field 'Specialized.field' has no source parameter"
        );
    }
}
