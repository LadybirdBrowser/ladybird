/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Definite-initialization analysis for typed HIR.

use super::{
    Body, Statement, StatementKindIr, Variable,
    VariableId,
    condition_uses_and_defs, statement_uses_and_defs,
};
use crate::frontend::ast::ParameterMode;
use crate::frontend::diagnostic::Diagnostic;
use crate::types::Type;
use std::collections::{HashSet, VecDeque};

fn successors(index: usize, statements: &[Statement]) -> Vec<usize> {
    let next = (index + 1 < statements.len()).then_some(index + 1);
    match &statements[index].kind {
        StatementKindIr::Call(_, call) if call.terminal => Vec::new(),
        StatementKindIr::ScalarMatch { .. } => Vec::new(),
        StatementKindIr::ValueMatch {
            destination: None, ..
        } => Vec::new(),
        _ => next.into_iter().collect(),
    }
}

fn definitions_on_edge(
    statement: &Statement,
    successor: usize,
    next: Option<usize>,
) -> Vec<VariableId> {
    match &statement.kind {
        StatementKindIr::ValueRefinement { binding, tag, .. } => {
            let mut definitions = vec![*tag];
            if Some(successor) == next {
                definitions.push(*binding);
            }
            definitions
        }
        _ => statement_uses_and_defs(statement).1,
    }
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
            body.variables[*id].parameter_mode != Some(ParameterMode::Out)
                || body.variables[*id].ty == Type::Operand
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
        if check_outputs
            && let Some(id) = outputs
                .iter()
                .find(|id| variables[**id].parameter_mode == Some(ParameterMode::Out))
            {
                return Err(Diagnostic::new(
                    filename,
                    variables[*id].span,
                    format!(
                        "Out<{}> parameter '{}' is not initialized on this returning path",
                        variables[*id].ty, variables[*id].name
                    ),
                ));
            }
        return Ok(());
    }
    let all: HashSet<VariableId> = (0..variables.len()).collect();
    let mut inputs = vec![all; statements.len()];
    inputs[0] = entry;
    let mut reachable = vec![false; statements.len()];
    reachable[0] = true;
    let mut queue: VecDeque<usize> = (0..statements.len()).collect();
    while let Some(index) = queue.pop_front() {
        if !reachable[index] {
            continue;
        }
        for successor in successors(index, statements) {
            let mut output = inputs[index].clone();
            output.extend(definitions_on_edge(
                &statements[index],
                successor,
                (index + 1 < statements.len()).then_some(index + 1),
            ));
            let new_input = if reachable[successor] {
                inputs[successor].intersection(&output).copied().collect()
            } else {
                output.clone()
            };
            if !reachable[successor] || new_input != inputs[successor] {
                reachable[successor] = true;
                inputs[successor] = new_input;
                queue.push_back(successor);
            }
        }
    }

    for (index, statement) in statements.iter().enumerate() {
        if !reachable[index] {
            continue;
        }
        let check_nested = |statements: &[Statement], entry| {
            check_definite_initialization_with_entry(
                filename,
                variables,
                statements,
                entry,
                &[],
                false,
            )
        };
        let (uses, _) = statement_uses_and_defs(statement);
        if let Some(id) = uses.into_iter().find(|id| !inputs[index].contains(id)) {
            return Err(Diagnostic::new(
                filename,
                statement.span,
                format!("binding '{}' is read before it is initialized", variables[id].name),
            ));
        }
        if let StatementKindIr::ValueMatch {
            tag,
            arms,
            fallback,
            ..
        } = &statement.kind
        {
            let mut nested_arms = arms
                .iter()
                .map(|arm| {
                    let bindings = tag
                        .iter()
                        .chain(arm.binding.iter())
                        .copied()
                        .collect::<Vec<_>>();
                    (&arm.body, bindings)
                })
                .collect::<Vec<_>>();
            nested_arms.push((&fallback.body, tag.iter().copied().collect()));
            for (arm, bindings) in nested_arms {
                let entry = inputs[index].iter().copied().chain(bindings).collect();
                check_nested(arm, entry)?;
            }
        }
        if let StatementKindIr::Call(_, call) = &statement.kind
            && let Some(failure) = &call.failure
        {
            check_nested(&failure.body, inputs[index].clone())?;
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
            let entry = inputs[index]
                .iter()
                .copied()
                .chain(definitions)
                .collect::<HashSet<_>>();
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
            check_nested(condition_setup, inputs[index].clone())?;
            let setup_definitions = condition_setup
                .iter()
                .flat_map(|statement| statement_uses_and_defs(statement).1);
            let (_, condition_definitions) = condition_uses_and_defs(condition);
            let entry = inputs[index]
                .iter()
                .copied()
                .chain(setup_definitions)
                .chain(condition_definitions)
                .collect();
            check_nested(loop_body, entry)?;
        }
    }

    if check_outputs {
        for (index, statement) in statements.iter().enumerate() {
            if !reachable[index] || !successors(index, statements).is_empty() {
                continue;
            }
            let is_terminal = matches!(&statement.kind, StatementKindIr::Call(_, call) if call.terminal);
            if is_terminal {
                continue;
            }
            let (_, defs) = statement_uses_and_defs(statement);
            let initialized: HashSet<_> = inputs[index].iter().copied().chain(defs).collect();
            for id in outputs {
                if variables[*id].parameter_mode == Some(ParameterMode::Out) && !initialized.contains(id) {
                    return Err(Diagnostic::new(
                        filename,
                        statement.span,
                        format!(
                            "Out<{}> parameter '{}' is not initialized on this returning path",
                            variables[*id].ty, variables[*id].name
                        ),
                    ));
                }
            }
        }
    }
    Ok(())
}
