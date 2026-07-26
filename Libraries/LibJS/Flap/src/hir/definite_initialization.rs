/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Definite-initialization analysis for typed HIR.

use super::{Body, Statement, StatementKindIr, Variable, VariableId, condition_uses_and_defs, statement_uses_and_defs};
use crate::frontend::ast::ParameterMode;
use crate::frontend::diagnostic::Diagnostic;
use crate::hash::HashSet;
use crate::types::Type;

fn statement_continues(statement: &Statement) -> bool {
    !matches!(
        &statement.kind,
        StatementKindIr::Call(_, call) if call.terminal
    ) && !matches!(
        &statement.kind,
        StatementKindIr::ScalarMatch { .. } | StatementKindIr::ValueMatch { destination: None, .. }
    )
}

pub(super) fn check_definite_initialization(
    filename: &str,
    body: &Body,
    parameters: &[VariableId],
    check_outputs: bool,
) -> Result<(), Diagnostic> {
    let entry: HashSet<VariableId> = parameters
        .iter()
        .copied()
        .filter(|id| {
            body.variables[*id].parameter_mode != Some(ParameterMode::Out) || body.variables[*id].ty == Type::Operand
        })
        .collect();
    check_definite_initialization_with_entry(
        filename,
        &body.variables,
        &body.statements,
        entry,
        parameters,
        check_outputs,
    )
}

fn check_definite_initialization_with_entry(
    filename: &str,
    variables: &[Variable],
    statements: &[Statement],
    entry: HashSet<VariableId>,
    outputs: &[VariableId],
    check_outputs: bool,
) -> Result<(), Diagnostic> {
    if statements.is_empty() {
        check_initialized_outputs(filename, variables, outputs, &entry, check_outputs, None)?;
        return Ok(());
    }

    let mut initialized = entry;
    let check_nested = |statements: &[Statement], entry| {
        check_definite_initialization_with_entry(filename, variables, statements, entry, &[], false)
    };
    for statement in statements {
        let (uses, definitions) = statement_uses_and_defs(statement);
        if let Some(id) = uses.into_iter().find(|id| !initialized.contains(id)) {
            return Err(Diagnostic::new(
                filename,
                statement.span,
                format!("binding '{}' is read before it is initialized", variables[id].name),
            ));
        }
        if let StatementKindIr::ValueMatch {
            tag, arms, fallback, ..
        } = &statement.kind
        {
            let mut nested_arms = arms
                .iter()
                .map(|arm| {
                    let bindings = tag.iter().chain(arm.binding.iter()).copied().collect::<Vec<_>>();
                    (&arm.body, bindings)
                })
                .collect::<Vec<_>>();
            nested_arms.push((&fallback.body, tag.iter().copied().collect()));
            for (arm, bindings) in nested_arms {
                let entry = initialized.iter().copied().chain(bindings).collect();
                check_nested(arm, entry)?;
            }
        }
        if let StatementKindIr::Call(_, call) = &statement.kind
            && let Some(failure) = &call.failure
        {
            check_nested(&failure.body, initialized.clone())?;
        }
        if let StatementKindIr::If {
            destination,
            condition,
            then_body,
            else_body,
            ..
        } = &statement.kind
        {
            let definitions = if destination.is_some() {
                Vec::new()
            } else {
                condition_uses_and_defs(condition).1
            };
            let entry = initialized.iter().copied().chain(definitions).collect::<HashSet<_>>();
            check_nested(then_body, entry.clone())?;
            if let Some(else_body) = else_body {
                check_nested(else_body, entry)?;
            }
        }
        if let StatementKindIr::While {
            condition_setup,
            condition,
            body: loop_body,
            ..
        } = &statement.kind
        {
            check_nested(condition_setup, initialized.clone())?;
            let setup_definitions = condition_setup
                .iter()
                .flat_map(|statement| statement_uses_and_defs(statement).1);
            let (_, condition_definitions) = condition_uses_and_defs(condition);
            let entry = initialized
                .iter()
                .copied()
                .chain(setup_definitions)
                .chain(condition_definitions)
                .collect();
            check_nested(loop_body, entry)?;
        }
        initialized.extend(definitions);
        if !statement_continues(statement) {
            check_initialized_outputs(filename, variables, outputs, &initialized, false, Some(statement))?;
            return Ok(());
        }
    }

    check_initialized_outputs(
        filename,
        variables,
        outputs,
        &initialized,
        check_outputs,
        statements.last(),
    )?;
    Ok(())
}

fn check_initialized_outputs(
    filename: &str,
    variables: &[Variable],
    outputs: &[VariableId],
    initialized: &HashSet<VariableId>,
    check_outputs: bool,
    exit: Option<&Statement>,
) -> Result<(), Diagnostic> {
    if !check_outputs {
        return Ok(());
    }
    let Some(id) = outputs
        .iter()
        .find(|id| variables[**id].parameter_mode == Some(ParameterMode::Out) && !initialized.contains(id))
    else {
        return Ok(());
    };
    Err(Diagnostic::new(
        filename,
        exit.map_or(variables[*id].span, |statement| statement.span),
        format!(
            "Out<{}> parameter '{}' is not initialized on this returning path",
            variables[*id].ty, variables[*id].name
        ),
    ))
}
