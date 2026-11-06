/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Bytecode generation from the columns of the syntax tree, straight into bytes.
//!
//! A top level has marks in its rows from its first parse, and a function gets parsed again with them. `sweep` goes
//! through the rows in one loop, leaving what does not come in the order of its rows to `constructs`. Both call the emission core in `stream`, which
//! knows nothing of rows, and writes each instruction into bytes. What names mean comes from `scope`, which goes by a
//! summary of the first parse. Everything with memory of its own is in `context`, and comes back for the next compile
//! on the thread.

mod bindings;
mod builtins;
mod constants;
mod constructs;
mod context;
pub mod emitter;
mod environment;
mod generators;
mod members;
mod names;
mod nodes;
mod operators;
mod registers;
pub mod scope;
pub mod stream;
mod sweep;

use std::sync::Arc;

use crate::ast::FunctionKind;
use crate::ast::SyntaxTree;
use crate::bytecode::executable_data::ExecutableData;
use crate::bytecode::executable_data::PrecompiledFunction;
use crate::bytecode::instruction::typed as op;
use crate::parser::EnclosingCode;
use crate::parser::OutOfStack;
use context::Generator;
pub use environment::EnclosingEnvironmentScope;
use nodes::NodeId;
pub use stream::DisposableResourceKind;

/// Compiles the function at `row`, which gets parsed again for that.
pub fn compile_function(
    script: &Arc<crate::parser::ScriptData>,
    row: crate::parser::FunctionRow,
    enclosing_environment_scope: Option<Arc<EnclosingEnvironmentScope>>,
    enclosing_code: &Arc<EnclosingCode>,
    builtin_abstract_operations_enabled: bool,
    stack_limit: usize,
) -> Result<PrecompiledFunction, OutOfStack> {
    let (tree, node) = script.reparse_for_sweep(row, stack_limit)?;
    let result = {
        let mut generator = Generator::new(&tree, enclosing_code.clone(), stack_limit);
        generator.enclosing_environment_scope = enclosing_environment_scope;
        generator.builtin_abstract_operations_enabled = builtin_abstract_operations_enabled;
        generator.begin_function_scope(row);
        let function = NodeId(node);
        let flags = script.index.flags[row.0 as usize];
        use crate::parser::columns::function_flag::*;
        let swept = if flags & FIELD_INITIALIZER != 0 {
            generator.sweep_field_initializer(function)
        } else if flags & STATIC_BLOCK != 0 {
            generator.sweep_static_block(function)
        } else if flags & DEFAULT_CONSTRUCTOR != 0 {
            generator.current_source_offset =
                crate::bytecode::source_map::SourceOffset(script.index.start[row.0 as usize]);
            generator.generate_default_constructor(flags & DERIVED_CONSTRUCTOR != 0);
            Ok(())
        } else {
            generator.sweep_parameter_names(function);
            generator
                .sweep_parameters(function)
                .and_then(|()| sweep::sweep_function_body(&mut generator, function))
        };
        swept.map(|()| generator.finish_function())
    };
    tree.give_back_memory();
    result
}

/// What a compile needs to know of a program, which `compile.rs` works out.
pub struct ProgramInput<'a> {
    pub tree: &'a Arc<SyntaxTree>,
    pub strict: bool,
    /// Whether the program is a module with top-level await, which runs like an async function.
    pub is_async_module: bool,
    pub eager_compile_direct_iifes: bool,
    pub enclosing_environment_scope: Option<Arc<EnclosingEnvironmentScope>>,
    pub stack_limit: usize,
}

/// Compiles the script, module or eval that is the program of the tree in `input`, from the rows that its parse left.
pub fn compile_program(input: &ProgramInput<'_>) -> Result<ExecutableData, OutOfStack> {
    let parsed = input.tree;
    let script = &parsed.script;
    let (tree, node) = script.take_top_level_rows()?;
    let kind = if script.program_type == crate::ast::ProgramType::Module {
        scope::ProgramKind::Module
    } else if script.program_context.has_no_globals {
        scope::ProgramKind::WithoutGlobals
    } else {
        scope::ProgramKind::Script
    };
    let compiled = {
        let mut generator = Generator::new(&tree, EnclosingCode::of_program(parsed), input.stack_limit);
        generator.strict = input.strict;
        generator.eager_compile_direct_iifes = input.eager_compile_direct_iifes;
        generator.enclosing_environment_scope = input.enclosing_environment_scope.clone();
        if input.is_async_module {
            generator.enclosing_function_kind = FunctionKind::Async;
        }
        generator.context.annexb_function_names.extend_from_slice(
            parsed
                .program_scope()
                .annexb_function_names
                .of(&parsed.scopes.annexb_function_names),
        );
        generator.begin_program_scope(kind, input.is_async_module);
        sweep::sweep_program(&mut generator, NodeId(node)).map(|()| generator.finish_function())
    };
    tree.give_back_memory();
    let mut executable = Arc::into_inner(compiled?.executable).expect("nothing else holds on to new code");
    for pending in executable.shared_function_data.iter_mut() {
        if pending.should_eager_compile {
            pending.precompiled_function =
                crate::compile::compile_function(&pending.description.payload, false, input.stack_limit)
                    .ok()
                    .map(Arc::new);
        }
    }
    Ok(executable)
}

/// The code of a program that nests too deeply to generate code for it with the stack there is. Running it throws, like
/// a function does that is too deep to compile.
pub fn compile_program_too_deep_to_generate(tree: &Arc<SyntaxTree>) -> ExecutableData {
    let tree_ref: &SyntaxTree = tree;
    let mut generator = Generator::new(tree_ref, EnclosingCode::of_program(tree_ref), 0);
    generator.begin_program_scope(scope::ProgramKind::Script, true);
    let exception = generator.temporary();
    let error_string = generator.intern_string_text(crate::utf16::Utf16View::Utf16(utf16!(
        "Maximum call stack size exceeded"
    )));
    generator.put(op::NewTypeError {
        dst: exception,
        error_string,
    });
    generator.put(op::Throw { src: exception });
    let compiled = generator.finish_function();
    Arc::into_inner(compiled.executable).expect("nothing else holds on to new code")
}
