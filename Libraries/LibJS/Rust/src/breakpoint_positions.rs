/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The positions in a source that a debugger can stop at: those that the bytecode of the source maps back to.

use crate::ast::ProgramType;
use crate::bytecode::executable::ExecutableData;
use crate::bytecode::generator::PrecompiledFunction;
use crate::compile::CompiledProgram;
use crate::compile::CompiledProgramBytecode;
use crate::compile::FunctionPrecompileMode;
use crate::compile::compile_parsed_program_off_thread;
use crate::compile::parse;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct BreakpointPosition {
    pub line: u32,
    pub column: u32,
}

/// The breakpoint positions of `source`, sorted and without repeats, with lines counted as [`parse()`] counts them
/// from `initial_line_number`. A source with syntax errors has none.
pub fn breakpoint_positions_for_source(
    source: &[u16],
    program_type: ProgramType,
    initial_line_number: usize,
) -> Vec<BreakpointPosition> {
    let parsed = parse(source, program_type, initial_line_number);
    if parsed.has_errors() {
        return Vec::new();
    }
    let compiled = compile_parsed_program_off_thread(parsed, source.len(), FunctionPrecompileMode::All);
    let mut positions = Vec::new();
    compiled.for_each_breakpoint_position(|position| positions.push(position));
    compiled.discard();
    positions.sort_unstable();
    positions.dedup();
    positions
}

impl CompiledProgram {
    /// Calls `callback` with the position of every source map entry in the bytecode of the program and of the
    /// functions that were compiled with it, in no particular order and with repeats.
    pub fn for_each_breakpoint_position(&self, mut callback: impl FnMut(BreakpointPosition)) {
        fn for_each_in_precompiled_function(
            precompiled: &PrecompiledFunction,
            callback: &mut dyn FnMut(BreakpointPosition),
        ) {
            for_each_in_executable(&precompiled.executable, callback);
        }

        fn for_each_in_executable(executable: &ExecutableData, callback: &mut dyn FnMut(BreakpointPosition)) {
            for entry in &executable.source_map {
                if entry.line != 0 {
                    callback(BreakpointPosition {
                        line: entry.line,
                        column: entry.column,
                    });
                }
            }
            for shared_data in &executable.shared_function_data {
                if let Some(precompiled) = &shared_data.precompiled_function {
                    for_each_in_precompiled_function(precompiled, callback);
                }
            }
        }

        match &self.bytecode {
            CompiledProgramBytecode::Program(executable) | CompiledProgramBytecode::AsyncModule(executable) => {
                for_each_in_executable(executable, &mut callback);
            }
        }
        for declaration in &self.declaration_functions {
            if let Some(precompiled) = &declaration.precompiled_function {
                for_each_in_precompiled_function(precompiled, &mut callback);
            }
        }
    }
}
