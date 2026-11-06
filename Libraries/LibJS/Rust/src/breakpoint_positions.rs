/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The positions in a source that a debugger can stop at: those that the bytecode of the source maps back to.

use crate::ast::ProgramType;
use crate::bytecode::executable_data::ExecutableData;
use crate::bytecode::executable_data::PendingSharedFunctionData;
use crate::compile::FunctionPayload;
use crate::compile::compile_function;
use crate::compile::compile_parsed_program_off_thread;
use crate::compile::parse;
use crate::parser::base::DEFAULT_STACK_BUDGET;
use crate::parser::stack_limit_for_budget;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct BreakpointPosition {
    pub line: u32,
    pub column: u32,
}

/// The breakpoint positions of `source`, sorted and without repeats, with lines counted as [`parse()`] counts them
/// from `initial_line_number`. A source with syntax errors has none. Every function gets compiled to find them, on its
/// own, the way that it would on its first call. The positions of what does not fit the stack left out.
pub fn breakpoint_positions_for_source(
    source: &ak::Utf16String,
    program_type: ProgramType,
    initial_line_number: usize,
) -> Vec<BreakpointPosition> {
    let parsed = parse(source, program_type, initial_line_number);
    if parsed.has_errors() {
        return Vec::new();
    }
    let compiled = compile_parsed_program_off_thread(parsed);
    let mut positions = Vec::new();
    match program_type {
        ProgramType::Script => {
            let script = compiled.into_script();
            add_positions_of_executable(&script.executable, &mut positions);
            for function in &script.declarations.functions_to_initialize {
                add_positions_of_function(&function.shared_function_data, &mut positions);
            }
        }
        ProgramType::Module => {
            let module = compiled.into_module();
            add_positions_of_executable(&module.executable, &mut positions);
            for function in &module.declarations.functions_to_initialize {
                add_positions_of_payload(&function.description.payload, &mut positions);
            }
        }
    }
    positions.sort_unstable();
    positions.dedup();
    positions
}

fn add_positions_of_executable(executable: &ExecutableData, positions: &mut Vec<BreakpointPosition>) {
    for entry in executable.source_positions() {
        if entry.line != 0 {
            positions.push(BreakpointPosition {
                line: entry.line,
                column: entry.column,
            });
        }
    }
    for function in &executable.shared_function_data {
        add_positions_of_function(function, positions);
    }
}

fn add_positions_of_function(function: &PendingSharedFunctionData, positions: &mut Vec<BreakpointPosition>) {
    if let Some(precompiled) = &function.precompiled_function {
        add_positions_of_executable(&precompiled.executable, positions);
    } else {
        add_positions_of_payload(&function.description.payload, positions);
    }
}

fn add_positions_of_payload(payload: &FunctionPayload, positions: &mut Vec<BreakpointPosition>) {
    if let Ok(compiled) = compile_function(payload, false, stack_limit_for_budget(DEFAULT_STACK_BUDGET)) {
        add_positions_of_executable(&compiled.executable, positions);
    }
}
