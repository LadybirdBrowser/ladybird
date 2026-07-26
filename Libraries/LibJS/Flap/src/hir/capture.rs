/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lexical capture analysis for nested HIR regions.

use super::{Statement, VariableId, statement_uses_and_defs};
use crate::hash::HashSet;

pub(super) fn collect_captures(statements: &[Statement], outer_variable_count: usize) -> Vec<VariableId> {
    let mut captures = HashSet::default();
    for statement in statements {
        let (uses, definitions) = statement_uses_and_defs(statement);
        captures.extend(
            uses.into_iter()
                .chain(definitions)
                .filter(|id| *id < outer_variable_count),
        );
        statement.kind.for_each_nested_body(|body| {
            captures.extend(collect_captures(body, outer_variable_count));
        });
    }
    let mut captures: Vec<_> = captures.into_iter().collect();
    captures.sort_unstable();
    captures
}

pub(super) fn collect_input_captures(statements: &[Statement], outer_variable_count: usize) -> Vec<VariableId> {
    let mut captures = HashSet::default();
    let mut definitions = HashSet::default();
    for statement in statements {
        let (uses, defs) = statement_uses_and_defs(statement);
        captures.extend(
            uses.into_iter()
                .filter(|id| *id < outer_variable_count && !definitions.contains(id)),
        );
        definitions.extend(defs.into_iter().filter(|id| *id < outer_variable_count));
    }
    let mut captures: Vec<_> = captures.into_iter().collect();
    captures.sort_unstable();
    captures
}
