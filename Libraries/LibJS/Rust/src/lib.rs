/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! # LibJS Parser
//!
//! A JavaScript parser that produces an AST.
//!
//! ## Architecture
//!
//! ```text
//! Source code (UTF-16)
//!     │
//!     ▼
//! ┌─────────────────────────────────────────────────────┐
//! │  Lexer (lexer.rs)                                   │
//! │  Tokenizes UTF-16 source into Token stream          │
//! └──────────────────────┬──────────────────────────────┘
//!                        │ tokens
//!                        ▼
//! ┌─────────────────────────────────────────────────────┐
//! │  Parser (parser.rs + parser/*.rs)                   │
//! │  Recursive descent with precedence climbing         │
//! │  Builds AST (ast.rs)                                │
//! └──────────────────────┬──────────────────────────────┘
//!                        │ AST
//!                        ▼
//! ┌─────────────────────────────────────────────────────┐
//! │  Codegen (bytecode/codegen.rs)                      │
//! │  Walks AST, emits bytecode via Generator            │
//! └──────────────────────┬──────────────────────────────┘
//!                        │ assembled bytecode
//!                        ▼
//! ┌─────────────────────────────────────────────────────┐
//! │  FFI (bytecode/ffi.rs → BytecodeFactory.cpp)        │
//! │  Creates Executable from assembled data             │
//! └─────────────────────────────────────────────────────┘
//! ```
//!
//! ## Module overview
//!
//! - `lib.rs` — Entry point (FFI exports)
//! - `token.rs` — Token types
//! - `lexer.rs` — Tokenizer: UTF-16 input → Token stream
//! - `parser.rs` — Parser state, helpers, token consumption
//! - `parser/expressions.rs` — Expression parsing (precedence climbing)
//! - `parser/statements.rs` — Statement parsing (if, for, while, etc.)
//! - `parser/declarations.rs` — Functions, classes, variables, modules
//! - `ast.rs` — AST type definitions
//! - `bytecode/` — Bytecode generator, instruction types, and FFI
//! - `scope_collector.rs` — Scope analysis

/// Compile-time conversion of an ASCII string literal to `&'static [u16]`.
///
/// Produces a static `[u16; N]` array, so comparisons like
/// `value == utf16!("eval")` involve zero heap allocation.
///
/// # Panics (at compile time)
/// Panics if the string contains non-ASCII characters. All JS keywords
/// and identifiers we compare against are pure ASCII.
macro_rules! utf16 {
    ($s:literal) => {{
        const VALUE: &[u16; $s.len()] = &{
            let bytes = $s.as_bytes();
            let mut arr = [0u16; $s.len()];
            let mut i = 0;
            while i < bytes.len() {
                assert!(bytes[i] < 128, "utf16! only supports ASCII literals");
                arr[i] = bytes[i] as u16;
                i += 1;
            }
            arr
        };
        VALUE.as_slice()
    }};
}

pub mod ast;
pub mod ast_dump;
pub mod bytecode;
pub mod lexer;
pub mod parser;
pub mod scope_collector;
pub mod token;

/// Convert a `usize` to `u32`, panicking if the value exceeds `u32::MAX`.
/// Prefer this over `as u32` which silently truncates on 64-bit platforms.
pub(crate) fn u32_from_usize(value: usize) -> u32 {
    u32::try_from(value).expect("value exceeds u32::MAX")
}

use ast::StatementKind;
use parser::{Parser, ProgramType};
use std::cell::RefCell;
use std::collections::HashSet;
use std::ffi::c_void;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::rc::Rc;

// =============================================================================
// Internal helpers
// =============================================================================

/// Catch any Rust panics to prevent undefined behavior from unwinding across
/// the FFI boundary. Aborts the process on panic.
fn abort_on_panic<F: FnOnce() -> R, R>(f: F) -> R {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(payload) => {
            let msg = if let Some(s) = payload.downcast_ref::<&str>() {
                s.to_string()
            } else if let Some(s) = payload.downcast_ref::<String>() {
                s.clone()
            } else {
                "unknown panic".to_string()
            };
            eprintln!("Rust panic at FFI boundary: {msg}");
            std::process::abort();
        }
    }
}

/// Write an AST dump string to FFI output pointers.
///
/// Produces a string dump of the program, leaks it as a `Box<[u8]>`, and
/// writes the pointer and length to the provided out-parameters. The caller
/// must free via `rust_free_string(ptr, len)`.
///
/// # Safety
/// `output_ptr` and `output_len` must either both be null (no dump requested)
/// or both be valid writable pointers.
unsafe fn write_ast_dump_output(
    program: &ast::Statement,
    function_table: &ast::FunctionTable,
    output_ptr: *mut *mut u8,
    output_len: *mut usize,
) {
    unsafe {
        if output_ptr.is_null() || output_len.is_null() {
            return;
        }
        let dump_string = ast_dump::dump_program_to_string(program, function_table);
        let mut boxed = dump_string.into_bytes().into_boxed_slice();
        *output_ptr = boxed.as_mut_ptr();
        *output_len = boxed.len();
        // NB: Caller must free via rust_free_string(ptr, len).
        std::mem::forget(boxed);
    }
}

/// Create a UTF-16 slice from a raw pointer, returning None if the pointer is null.
///
/// NB: C++ Vector<u16>::data() returns nullptr when the vector is empty (no allocation),
/// so we must handle len == 0 with a null pointer as a valid empty slice.
unsafe fn source_from_raw<'a>(source: *const u16, len: usize) -> Option<&'a [u16]> {
    unsafe {
        if len == 0 {
            return Some(&[]);
        }
        if source.is_null() {
            eprintln!("source_from_raw: null pointer with non-zero length {len}");
            return None;
        }
        Some(std::slice::from_raw_parts(source, len))
    }
}

/// Callback type for reporting parse errors to C++.
type ParseErrorCallback = unsafe extern "C" fn(
    ctx: *mut c_void,
    message: *const u8,
    message_len: usize,
    line: u32,
    column: u32,
);

/// Log parser and scope collector errors, returning true if any were found.
fn check_errors(parser: &mut Parser) -> bool {
    check_errors_with_callback(parser, std::ptr::null_mut(), None)
}

/// Check for errors, optionally reporting them via a C++ callback.
fn check_errors_with_callback(
    parser: &mut Parser,
    error_context: *mut c_void,
    error_callback: Option<ParseErrorCallback>,
) -> bool {
    if parser.has_errors() {
        if let Some(cb) = error_callback {
            for err in parser.errors() {
                let msg = &err.message;
                unsafe {
                    cb(error_context, msg.as_ptr(), msg.len(), err.line, err.column);
                }
            }
        }
        return true;
    }
    if parser.scope_collector.has_errors() {
        if let Some(cb) = error_callback {
            for err in parser.scope_collector.drain_errors() {
                let msg = &err.message;
                unsafe {
                    cb(error_context, msg.as_ptr(), msg.len(), err.line, err.column);
                }
            }
        }
        return true;
    }
    false
}

/// Convert scope local variables to generator LocalVariable format.
fn convert_local_variables(scope: &ast::ScopeData) -> Vec<bytecode::generator::LocalVariable> {
    scope
        .local_variables
        .iter()
        .map(|lv| bytecode::generator::LocalVariable {
            name: lv.name.clone(),
            is_lexically_declared: lv.kind == ast::LocalVarKind::LetOrConst,
            is_initialized_during_declaration_instantiation: false,
        })
        .collect()
}

/// Create a Generator configured for program-level compilation.
fn new_program_generator(
    strict: bool,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    source_len: usize,
) -> bytecode::generator::Generator {
    let mut generator = bytecode::generator::Generator::new();
    generator.strict = strict;
    generator.must_propagate_completion = true;
    generator.vm_ptr = vm_ptr;
    generator.source_code_ptr = source_code_ptr;
    generator.source_len = source_len;
    generator
}

/// Shared compilation pipeline: local variable setup → codegen → assemble → create Executable.
///
/// Called by all three program-level entry points after parsing and scope analysis.
unsafe fn compile_program_body(
    generator: &mut bytecode::generator::Generator,
    program: &ast::Statement,
    scope_ref: &Rc<RefCell<ast::ScopeData>>,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
) -> *mut c_void {
    unsafe {
        generator.local_variables = convert_local_variables(&scope_ref.borrow());

        let entry_block = generator.make_block();
        generator.switch_to_basic_block(entry_block);

        {
            use bytecode::operand::{Operand, Register};
            let env_reg =
                generator.scoped_operand(Operand::register(Register::SAVED_LEXICAL_ENVIRONMENT));
            generator.emit(bytecode::instruction::Instruction::GetLexicalEnvironment {
                dst: env_reg.operand(),
            });
            generator.lexical_environment_register_stack.push(env_reg);
        }

        let result = bytecode::codegen::generate_statement(program, generator, None);

        if !generator.is_current_block_terminated()
            && let Some(value) = result {
                generator.emit(bytecode::instruction::Instruction::End {
                    value: value.operand(),
                });
            }
            // If result is None, the assembler will add End(undefined) as a
            // fallthrough for unterminated blocks, matching C++ compile().

        let assembled = generator.assemble();
        bytecode::ffi::create_executable(generator, &assembled, vm_ptr, source_code_ptr)
    }
}

// =============================================================================
// FFI entry points: program compilation
// =============================================================================

/// Compile a JavaScript program using the parser and bytecode generator.
///
/// This is the full pipeline: parse → codegen → assemble → create Executable.
/// Called from C++ unless `LIBJS_CPP=1` is set.
///
/// Returns a `GC::Ptr<Bytecode::Executable>` cast to `void*`, or nullptr on failure.
///
/// # Safety
/// - `source` must point to a valid UTF-16 buffer of `source_len` elements.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compile_program(
    source: *const u16,
    source_len: usize,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    program_type: u8,
    starts_in_strict_mode: bool,
    initiated_by_eval: bool,
    in_eval_function_context: bool,
    allow_super_property_lookup: bool,
    allow_super_constructor_call: bool,
    in_class_field_initializer: bool,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            let Some(source_slice) = source_from_raw(source, source_len) else {
                return std::ptr::null_mut();
            };
            let pt = match program_type {
                0 => ProgramType::Script,
                1 => ProgramType::Module,
                _ => {
                    return std::ptr::null_mut();
                }
            };
            let mut parser = Parser::new(source_slice, pt);
            if initiated_by_eval {
                parser.initiated_by_eval = true;
                parser.in_eval_function_context = in_eval_function_context;
                parser.flags.allow_super_property_lookup = allow_super_property_lookup;
                parser.flags.allow_super_constructor_call = allow_super_constructor_call;
                parser.flags.in_class_field_initializer = in_class_field_initializer;
            }

            let program = parser.parse_program(starts_in_strict_mode);

            if check_errors(&mut parser) {
                return std::ptr::null_mut();
            }

            parser.scope_collector.analyze(initiated_by_eval);

            let scope_ref = if let StatementKind::Program(ref data) = program.inner {
                data.scope.clone()
            } else {
                return std::ptr::null_mut();
            };

            let mut generator =
                new_program_generator(starts_in_strict_mode, vm_ptr, source_code_ptr, source_len);
            generator.function_table = std::mem::take(&mut parser.function_table);
            compile_program_body(
                &mut generator,
                &program,
                &scope_ref,
                vm_ptr,
                source_code_ptr,
            )
        })
    }
}

/// Compile a script and extract GDI (GlobalDeclarationInstantiation) metadata.
///
/// This is the path for scripts. It:
/// 1. Parses the program
/// 2. Runs scope analysis
/// 3. Generates bytecode → creates Executable
/// 4. Extracts GDI metadata from the program AST
/// 5. Populates the C++ ScriptGdiBuilder via callbacks
///
/// Returns the `Executable*` as `void*`, or nullptr on failure.
///
/// # Safety
/// - `source` must point to a valid UTF-16 buffer of `source_len` elements.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
/// - `gdi_context` must be a valid pointer to a C++ ScriptGdiBuilder.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compile_script(
    source: *const u16,
    source_len: usize,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    gdi_context: *mut c_void,
    dump_ast: bool,
    use_color: bool,
    error_context: *mut c_void,
    error_callback: Option<ParseErrorCallback>,
    ast_dump_output: *mut *mut u8,
    ast_dump_output_len: *mut usize,
    initial_line_number: usize,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            let Some(source_slice) = source_from_raw(source, source_len) else {
                return std::ptr::null_mut();
            };
            let mut parser = Parser::new_with_line_offset(
                source_slice,
                ProgramType::Script,
                u32_from_usize(initial_line_number),
            );

            let program = parser.parse_program(false);

            if check_errors_with_callback(&mut parser, error_context, error_callback) {
                return std::ptr::null_mut();
            }

            parser.scope_collector.analyze(false);

            // Dump AST if requested (after scope analysis so identifier metadata is populated).
            if dump_ast {
                ast_dump::dump_program(&program, use_color, &parser.function_table);
            }

            write_ast_dump_output(
                &program,
                &parser.function_table,
                ast_dump_output,
                ast_dump_output_len,
            );

            let (scope_ref, is_strict) = if let StatementKind::Program(ref data) = program.inner {
                (data.scope.clone(), data.is_strict_mode)
            } else {
                return std::ptr::null_mut();
            };

            let mut generator =
                new_program_generator(is_strict, vm_ptr, source_code_ptr, source_len);
            generator.function_table = std::mem::take(&mut parser.function_table);
            let exec_ptr = compile_program_body(
                &mut generator,
                &program,
                &scope_ref,
                vm_ptr,
                source_code_ptr,
            );
            if exec_ptr.is_null() {
                return std::ptr::null_mut();
            }

            extract_script_gdi(
                &scope_ref.borrow(),
                is_strict,
                vm_ptr,
                source_code_ptr,
                gdi_context,
                &mut generator.function_table,
            );

            exec_ptr
        })
    }
}

/// Compile an eval script and extract EDI (EvalDeclarationInstantiation) metadata.
///
/// This is the path for eval(). It:
/// 1. Parses the program with eval flags
/// 2. Runs scope analysis with initiated_by_eval=true
/// 3. Generates bytecode → creates Executable
/// 4. Extracts EDI metadata from the program AST
/// 5. Populates the C++ EvalGdiBuilder via callbacks
///
/// Returns the `Executable*` as `void*`, or nullptr on failure.
///
/// # Safety
/// - `source` must point to a valid UTF-16 buffer of `source_len` elements.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
/// - `gdi_context` must be a valid pointer to a C++ EvalGdiBuilder.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compile_eval(
    source: *const u16,
    source_len: usize,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    gdi_context: *mut c_void,
    starts_in_strict_mode: bool,
    in_eval_function_context: bool,
    allow_super_property_lookup: bool,
    allow_super_constructor_call: bool,
    in_class_field_initializer: bool,
    error_context: *mut c_void,
    error_callback: Option<ParseErrorCallback>,
    ast_dump_output: *mut *mut u8,
    ast_dump_output_len: *mut usize,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            let Some(source_slice) = source_from_raw(source, source_len) else {
                return std::ptr::null_mut();
            };
            let mut parser = Parser::new(source_slice, ProgramType::Script);
            parser.initiated_by_eval = true;
            parser.in_eval_function_context = in_eval_function_context;
            parser.flags.allow_super_property_lookup = allow_super_property_lookup;
            parser.flags.allow_super_constructor_call = allow_super_constructor_call;
            parser.flags.in_class_field_initializer = in_class_field_initializer;

            let program = parser.parse_program(starts_in_strict_mode);

            if check_errors_with_callback(&mut parser, error_context, error_callback) {
                return std::ptr::null_mut();
            }

            parser.scope_collector.analyze(true);

            write_ast_dump_output(
                &program,
                &parser.function_table,
                ast_dump_output,
                ast_dump_output_len,
            );

            let (scope_ref, is_strict) = if let StatementKind::Program(ref data) = program.inner {
                (data.scope.clone(), data.is_strict_mode)
            } else {
                return std::ptr::null_mut();
            };

            let mut generator =
                new_program_generator(is_strict, vm_ptr, source_code_ptr, source_len);
            generator.function_table = std::mem::take(&mut parser.function_table);
            let exec_ptr = compile_program_body(
                &mut generator,
                &program,
                &scope_ref,
                vm_ptr,
                source_code_ptr,
            );
            if exec_ptr.is_null() {
                return std::ptr::null_mut();
            }

            extract_eval_gdi(
                &scope_ref.borrow(),
                is_strict,
                vm_ptr,
                source_code_ptr,
                gdi_context,
                &mut generator.function_table,
            );

            exec_ptr
        })
    }
}

// =============================================================================
// FFI entry point: dynamic function (new Function())
// =============================================================================

/// Compile a dynamically-created function (new Function()).
/// https://tc39.es/ecma262/#sec-createdynamicfunction
///
/// Validates parameters and body separately per spec, then parses
/// the full synthetic source to create a SharedFunctionInstanceData.
///
/// Returns a `SharedFunctionInstanceData*` as `void*`, or nullptr on
/// parse failure.
///
/// # Safety
/// - All source pointers must be valid UTF-16 buffers.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compile_dynamic_function(
    full_source: *const u16,
    full_source_len: usize,
    parameters_source: *const u16,
    parameters_source_len: usize,
    body_source: *const u16,
    body_source_len: usize,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    function_kind: u8,
    error_context: *mut c_void,
    error_callback: Option<ParseErrorCallback>,
    ast_dump_output: *mut *mut u8,
    ast_dump_output_len: *mut usize,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            let kind = match function_kind {
                0 => ast::FunctionKind::Normal,
                1 => ast::FunctionKind::Generator,
                2 => ast::FunctionKind::Async,
                3 => ast::FunctionKind::AsyncGenerator,
                _ => {
                    return std::ptr::null_mut();
                }
            };

            // Validate parameters standalone.
            // First lex independently to catch lexer errors (e.g. unterminated comments)
            // with correct line/column positions relative to the parameter string.
            let Some(parameters_slice) = source_from_raw(parameters_source, parameters_source_len)
            else {
                return std::ptr::null_mut();
            };
            {
                let mut lexer = lexer::Lexer::new(parameters_slice, 1, 0);
                loop {
                    let token = lexer.next();
                    if token.token_type == token::TokenType::Eof {
                        break;
                    }
                    if token.token_type == token::TokenType::Invalid {
                        let msg = token.message.unwrap_or_else(|| {
                            format!("Unexpected token {}", token.token_type.name())
                        });
                        if let Some(cb) = error_callback {
                            cb(
                                error_context,
                                msg.as_ptr(),
                                msg.len(),
                                token.line_number,
                                token.line_column,
                            );
                        }
                        return std::ptr::null_mut();
                    }
                }
            }
            // Then wrap in a function for syntactic validation.
            {
                let mut validate_src: Vec<u16> = Vec::new();
                match kind {
                    ast::FunctionKind::Generator => {
                        validate_src.extend_from_slice(utf16!("function* test("))
                    }
                    ast::FunctionKind::Async => {
                        validate_src.extend_from_slice(utf16!("async function test("))
                    }
                    ast::FunctionKind::AsyncGenerator => {
                        validate_src.extend_from_slice(utf16!("async function* test("))
                    }
                    ast::FunctionKind::Normal => {
                        validate_src.extend_from_slice(utf16!("function test("))
                    }
                }
                validate_src.extend_from_slice(parameters_slice);
                validate_src.extend_from_slice(utf16!("\n) {}"));
                let mut parser = Parser::new(&validate_src, ProgramType::Script);
                parser.parse_program(false);
                if check_errors_with_callback(&mut parser, error_context, error_callback) {
                    return std::ptr::null_mut();
                }
            }

            // Validate body standalone: parse directly with function context flags.
            // NB: The C++ caller already wraps the body as "\nBODY\n" in body_parse_string,
            // so body_source already contains the newline-wrapped body. We parse it
            // directly as a script with function context flags set, matching the C++
            // approach of parse_function_body_from_string.
            {
                let Some(body_slice) = source_from_raw(body_source, body_source_len) else {
                    return std::ptr::null_mut();
                };
                let mut parser = Parser::new(body_slice, ProgramType::Script);
                parser.flags.in_function_context = true;
                match kind {
                    ast::FunctionKind::Async | ast::FunctionKind::AsyncGenerator => {
                        parser.flags.await_expression_is_valid = true;
                    }
                    _ => {}
                }
                match kind {
                    ast::FunctionKind::Generator | ast::FunctionKind::AsyncGenerator => {
                        parser.flags.in_generator_function_context = true;
                    }
                    _ => {}
                }
                parser.parse_program(false);
                if check_errors_with_callback(&mut parser, error_context, error_callback) {
                    return std::ptr::null_mut();
                }
            }

            let Some(full_slice) = source_from_raw(full_source, full_source_len) else {
                return std::ptr::null_mut();
            };
            let mut parser = Parser::new(full_slice, ProgramType::Script);
            let program = parser.parse_program(false);

            if check_errors_with_callback(&mut parser, error_context, error_callback) {
                return std::ptr::null_mut();
            }

            // Run scope analysis. Use analyze_as_dynamic_function() to suppress
            // marking identifiers as global, matching the C++ path which parses
            // as a FunctionExpression (no Program scope for globals to bind to).
            parser.scope_collector.analyze_as_dynamic_function();

            if parser.scope_collector.has_errors() {
                if let Some(cb) = error_callback {
                    for err in parser.scope_collector.drain_errors() {
                        let msg = &err.message;
                        cb(error_context, msg.as_ptr(), msg.len(), err.line, err.column);
                    }
                }
                return std::ptr::null_mut();
            }

            write_ast_dump_output(
                &program,
                &parser.function_table,
                ast_dump_output,
                ast_dump_output_len,
            );

            // Extract the FunctionExpression from the program.
            // The program should contain a single ExpressionStatement wrapping a FunctionExpression.
            let function_id = if let StatementKind::Program(ref data) = program.inner {
                let scope = data.scope.borrow();
                scope.children.iter().find_map(|child| match &child.inner {
                    StatementKind::FunctionDeclaration { function_id, .. } => Some(*function_id),
                    StatementKind::Expression(expression) => {
                        if let ast::ExpressionKind::Function(function_id) = &expression.inner {
                            Some(*function_id)
                        } else {
                            None
                        }
                    }
                    _ => None,
                })
            } else {
                None
            };

            let Some(function_id) = function_id else {
                if let Some(cb) = error_callback {
                    let msg = "Failed to parse dynamic function";
                    cb(error_context, msg.as_ptr(), msg.len(), 0, 0);
                }
                return std::ptr::null_mut();
            };

            let mut function_data = parser.function_table.take(function_id);

            // Dynamic functions always need an arguments object, matching the C++
            // path in FunctionConstructor::create_dynamic_function.
            function_data.parsing_insights.might_need_arguments_object = true;

            let is_strict = function_data.is_strict_mode;
            let subtable = parser.function_table.extract_reachable(&function_data);

            bytecode::ffi::create_sfd_for_gdi(
                function_data,
                subtable,
                vm_ptr,
                source_code_ptr,
                is_strict,
            )
        })
    }
}

// =============================================================================
// FFI entry point: builtin file compilation
// =============================================================================

/// Callback type for reporting builtin file functions to C++.
type BuiltinFunctionCallback =
    unsafe extern "C" fn(ctx: *mut c_void, sfd_ptr: *mut c_void, name: *const u16, name_len: usize);

/// Parse a builtin JS file in strict mode, extract top-level function
/// declarations, and create SharedFunctionInstanceData for each via the
/// the pipeline.
///
/// Calls `push_function` for each top-level FunctionDeclaration found.
///
/// # Safety
/// - `source` must point to a valid UTF-16 buffer of `source_len` elements.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
/// - `ctx` must be a valid pointer passed through to `push_function`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compile_builtin_file(
    source: *const u16,
    source_len: usize,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    ctx: *mut c_void,
    push_function: BuiltinFunctionCallback,
    ast_dump_output: *mut *mut u8,
    ast_dump_output_len: *mut usize,
) {
    unsafe {
        abort_on_panic(|| {
            let Some(source_slice) = source_from_raw(source, source_len) else {
                return;
            };

            let mut parser = Parser::new(source_slice, ProgramType::Script);
            let program = parser.parse_program(true); // strict mode

            if parser.has_errors() {
                let errors: Vec<String> = parser
                    .errors()
                    .iter()
                    .map(|e| format!("{}:{}: {}", e.line, e.column, e.message))
                    .collect();
                panic!("Parse errors in builtin file: {}", errors.join("; "));
            }

            parser.scope_collector.analyze(false);

            write_ast_dump_output(
                &program,
                &parser.function_table,
                ast_dump_output,
                ast_dump_output_len,
            );

            let scope_ref = if let StatementKind::Program(ref data) = program.inner {
                data.scope.clone()
            } else {
                return;
            };

            let scope = scope_ref.borrow();
            for child in &scope.children {
                if let StatementKind::FunctionDeclaration {
                    function_id,
                    ref name,
                    ..
                } = child.inner
                {
                    let function_data = parser.function_table.take(function_id);
                    let subtable = parser.function_table.extract_reachable(&function_data);
                    let sfd_ptr = bytecode::ffi::create_sfd_for_gdi(
                        function_data,
                        subtable,
                        vm_ptr,
                        source_code_ptr,
                        true, // strict
                    );
                    if !sfd_ptr.is_null()
                        && let Some(name_ident) = name {
                            push_function(
                                ctx,
                                sfd_ptr,
                                name_ident.name.as_ptr(),
                                name_ident.name.len(),
                            );
                        }
                }
            }
        });
    }
}

// =============================================================================
// FFI entry point: module compilation
// =============================================================================

/// Callback types for module compilation.
type ModuleBoolCallback = unsafe extern "C" fn(ctx: *mut c_void, value: bool);
type ModuleNameCallback = unsafe extern "C" fn(ctx: *mut c_void, name: *const u16, name_len: usize);
type ModuleImportEntryCallback = unsafe extern "C" fn(
    ctx: *mut c_void,
    import_name: *const u16,
    import_name_len: usize,
    is_namespace: bool,
    local_name: *const u16,
    local_name_len: usize,
    module_specifier: *const u16,
    specifier_len: usize,
    attribute_keys: *const bytecode::ffi::FFIUtf16Slice,
    attribute_values: *const bytecode::ffi::FFIUtf16Slice,
    attribute_count: usize,
);
type ModuleExportEntryCallback = unsafe extern "C" fn(
    ctx: *mut c_void,
    kind: u8,
    export_name: *const u16,
    export_name_len: usize,
    local_or_import_name: *const u16,
    local_or_import_name_len: usize,
    module_specifier: *const u16,
    specifier_len: usize,
    attribute_keys: *const bytecode::ffi::FFIUtf16Slice,
    attribute_values: *const bytecode::ffi::FFIUtf16Slice,
    attribute_count: usize,
);
type ModuleRequestedModuleCallback = unsafe extern "C" fn(
    ctx: *mut c_void,
    specifier: *const u16,
    specifier_len: usize,
    attribute_keys: *const bytecode::ffi::FFIUtf16Slice,
    attribute_values: *const bytecode::ffi::FFIUtf16Slice,
    attribute_count: usize,
);
type ModuleFunctionCallback =
    unsafe extern "C" fn(ctx: *mut c_void, sfd_ptr: *mut c_void, name: *const u16, name_len: usize);
type ModuleLexicalBindingCallback = unsafe extern "C" fn(
    ctx: *mut c_void,
    name: *const u16,
    name_len: usize,
    is_constant: bool,
    function_index: i32,
);

/// Module callback table passed from C++ to avoid many function pointer parameters.
#[repr(C)]
pub struct ModuleCallbacks {
    pub set_has_top_level_await: ModuleBoolCallback,
    pub push_import_entry: ModuleImportEntryCallback,
    pub push_local_export: ModuleExportEntryCallback,
    pub push_indirect_export: ModuleExportEntryCallback,
    pub push_star_export: ModuleExportEntryCallback,
    pub push_requested_module: ModuleRequestedModuleCallback,
    pub set_default_export_binding: ModuleNameCallback,
    pub push_var_name: ModuleNameCallback,
    pub push_function: ModuleFunctionCallback,
    pub push_lexical_binding: ModuleLexicalBindingCallback,
}

/// Helper to build FFI attribute arrays from a ModuleRequest.
fn build_attribute_slices(
    attributes: &[ast::ImportAttribute],
) -> (
    Vec<bytecode::ffi::FFIUtf16Slice>,
    Vec<bytecode::ffi::FFIUtf16Slice>,
) {
    attributes
        .iter()
        .map(|a| {
            (
                bytecode::ffi::FFIUtf16Slice::from(a.key.as_ref()),
                bytecode::ffi::FFIUtf16Slice::from(a.value.as_ref()),
            )
        })
        .unzip()
}

/// Helper to call an export entry callback with optional module request.
unsafe fn call_export_callback(
    callback: ModuleExportEntryCallback,
    ctx: *mut c_void,
    kind: u8,
    export_name: &Option<ast::Utf16String>,
    local_or_import_name: &Option<ast::Utf16String>,
    module_request: Option<&ast::ModuleRequest>,
) {
    unsafe {
        let (en_ptr, en_len) = export_name
            .as_ref()
            .map_or((std::ptr::null(), 0), |n| (n.as_ptr(), n.len()));
        let (lin_ptr, lin_len) = local_or_import_name
            .as_ref()
            .map_or((std::ptr::null(), 0), |n| (n.as_ptr(), n.len()));

        if let Some(mr) = module_request {
            let (keys, values) = build_attribute_slices(&mr.attributes);
            callback(
                ctx,
                kind,
                en_ptr,
                en_len,
                lin_ptr,
                lin_len,
                mr.module_specifier.as_ptr(),
                mr.module_specifier.len(),
                keys.as_ptr(),
                values.as_ptr(),
                keys.len(),
            );
        } else {
            callback(
                ctx,
                kind,
                en_ptr,
                en_len,
                lin_ptr,
                lin_len,
                std::ptr::null(),
                0,
                std::ptr::null(),
                std::ptr::null(),
                0,
            );
        }
    }
}

/// Compile an ES module using the parser and bytecode generator.
///
/// Parses the source as a module, extracts import/export metadata,
/// compiles the module body to bytecode, and extracts declaration data
/// needed for initialize_environment().
///
/// Returns `Executable*` for non-TLA modules (tla_executable_out is null),
/// or nullptr for TLA modules (tla_executable_out is set to the async wrapper executable).
///
/// # Safety
/// - `source` must point to a valid UTF-16 buffer of `source_len` elements.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
/// - `module_context` must be a valid `ModuleBuilder*`.
/// - `callbacks` must point to a valid `ModuleCallbacks`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compile_module(
    source: *const u16,
    source_len: usize,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    module_context: *mut c_void,
    callbacks: *const ModuleCallbacks,
    dump_ast: bool,
    use_color: bool,
    error_context: *mut c_void,
    error_callback: Option<ParseErrorCallback>,
    tla_executable_out: *mut *mut c_void,
    ast_dump_output: *mut *mut u8,
    ast_dump_output_len: *mut usize,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            let Some(source_slice) = source_from_raw(source, source_len) else {
                return std::ptr::null_mut();
            };

            let cb = &*callbacks;

            // 1. Parse as module.
            let mut parser = Parser::new(source_slice, ProgramType::Module);
            let program = parser.parse_program(false);

            if check_errors_with_callback(&mut parser, error_context, error_callback) {
                return std::ptr::null_mut();
            }

            parser.scope_collector.analyze(false);

            // Dump AST if requested (after scope analysis so identifier metadata is populated).
            if dump_ast {
                ast_dump::dump_program(&program, use_color, &parser.function_table);
            }

            write_ast_dump_output(
                &program,
                &parser.function_table,
                ast_dump_output,
                ast_dump_output_len,
            );

            let program_data = if let StatementKind::Program(ref data) = program.inner {
                data
            } else {
                return std::ptr::null_mut();
            };

            let scope_ref = program_data.scope.clone();
            let has_top_level_await = program_data.has_top_level_await;

            let mut function_table = std::mem::take(&mut parser.function_table);

            // 2. Report has_top_level_await.
            (cb.set_has_top_level_await)(module_context, has_top_level_await);

            // 3. Process imports and exports.
            extract_module_metadata(&scope_ref.borrow(), module_context, cb);

            // 4. Extract var declared names and lexical bindings.
            extract_module_declarations(
                &scope_ref.borrow(),
                vm_ptr,
                source_code_ptr,
                module_context,
                cb,
                &mut function_table,
            );

            // 5. Compute requested modules (sorted by source offset).
            extract_requested_modules(&scope_ref.borrow(), module_context, cb);

            // 6. Compile module body.
            if has_top_level_await {
                // Compile as an async wrapper function.
                let exec_ptr = compile_module_as_async(
                    &program,
                    &scope_ref,
                    vm_ptr,
                    source_code_ptr,
                    source,
                    source_len,
                    function_table,
                );
                if !tla_executable_out.is_null() {
                    *tla_executable_out = exec_ptr;
                }
                std::ptr::null_mut()
            } else {
                // Compile as a regular program.
                if !tla_executable_out.is_null() {
                    *tla_executable_out = std::ptr::null_mut();
                }
                let mut generator =
                    new_program_generator(true, vm_ptr, source_code_ptr, source_len);
                generator.function_table = function_table;
                compile_program_body(
                    &mut generator,
                    &program,
                    &scope_ref,
                    vm_ptr,
                    source_code_ptr,
                )
            }
        })
    }
}

/// Extract import/export metadata from a module's scope and call C++ callbacks.
unsafe fn extract_module_metadata(scope: &ast::ScopeData, ctx: *mut c_void, cb: &ModuleCallbacks) {
    unsafe {
        use ast::{ExportEntryKind, StatementKind};

        // Collect all import entries with their module requests.
        struct ImportEntryWithRequest {
            import_name: Option<ast::Utf16String>,
            local_name: ast::Utf16String,
            module_request: ast::ModuleRequest,
        }
        let mut all_import_entries: Vec<ImportEntryWithRequest> = Vec::new();

        for child in &scope.children {
            if let StatementKind::Import(ref import_data) = child.inner {
                for entry in &import_data.entries {
                    // Report each import entry.
                    let (in_ptr, in_len, is_ns) = entry
                        .import_name
                        .as_ref()
                        .map_or((std::ptr::null(), 0, true), |n| {
                            (n.as_ptr(), n.len(), false)
                        });
                    let (keys, values) =
                        build_attribute_slices(&import_data.module_request.attributes);
                    (cb.push_import_entry)(
                        ctx,
                        in_ptr,
                        in_len,
                        is_ns,
                        entry.local_name.as_ptr(),
                        entry.local_name.len(),
                        import_data.module_request.module_specifier.as_ptr(),
                        import_data.module_request.module_specifier.len(),
                        keys.as_ptr(),
                        values.as_ptr(),
                        keys.len(),
                    );

                    all_import_entries.push(ImportEntryWithRequest {
                        import_name: entry.import_name.clone(),
                        local_name: entry.local_name.clone(),
                        module_request: import_data.module_request.clone(),
                    });
                }
            }
        }

        // Process export entries (matching SourceTextModule::parse steps 9-10).
        for child in &scope.children {
            let export_data = if let StatementKind::Export(ref data) = child.inner {
                data
            } else {
                continue;
            };

            // Handle default export binding name.
            if export_data.is_default_export && export_data.entries.len() == 1 {
                let entry = &export_data.entries[0];
                // If the default export is not a declaration (function/class/etc.),
                // its binding name is the local_or_import_name.
                let is_declaration = export_data.statement.as_ref().is_some_and(|s| {
                    matches!(
                        s.inner,
                        StatementKind::FunctionDeclaration { .. }
                            | StatementKind::ClassDeclaration(_)
                    )
                });
                if !is_declaration
                    && let Some(ref name) = entry.local_or_import_name {
                        (cb.set_default_export_binding)(ctx, name.as_ptr(), name.len());
                    }
            }

            for entry in &export_data.entries {
                if entry.kind == ExportEntryKind::EmptyNamedExport {
                    break;
                }

                let has_module_request = export_data.module_request.is_some();

                if !has_module_request {
                    // No module request: check against import entries.
                    let matching_import = all_import_entries
                        .iter()
                        .find(|ie| entry.local_or_import_name.as_ref() == Some(&ie.local_name));

                    if let Some(import_entry) = matching_import {
                        if import_entry.import_name.is_none() {
                            // Namespace re-export → local export.
                            call_export_callback(
                                cb.push_local_export,
                                ctx,
                                entry.kind as u8,
                                &entry.export_name,
                                &entry.local_or_import_name,
                                None,
                            );
                        } else {
                            // Re-export of a specific binding → indirect export.
                            call_export_callback(
                                cb.push_indirect_export,
                                ctx,
                                ExportEntryKind::NamedExport as u8,
                                &entry.export_name,
                                &import_entry.import_name,
                                Some(&import_entry.module_request),
                            );
                        }
                    } else {
                        // Direct local export.
                        call_export_callback(
                            cb.push_local_export,
                            ctx,
                            entry.kind as u8,
                            &entry.export_name,
                            &entry.local_or_import_name,
                            None,
                        );
                    }
                } else if entry.kind == ExportEntryKind::ModuleRequestAllButDefault {
                    // export * from "module"
                    call_export_callback(
                        cb.push_star_export,
                        ctx,
                        entry.kind as u8,
                        &entry.export_name,
                        &entry.local_or_import_name,
                        export_data.module_request.as_ref(),
                    );
                } else {
                    // export { x } from "module" or export { x as y } from "module"
                    call_export_callback(
                        cb.push_indirect_export,
                        ctx,
                        entry.kind as u8,
                        &entry.export_name,
                        &entry.local_or_import_name,
                        export_data.module_request.as_ref(),
                    );
                }
            }
        }
    }
}

/// Extract var declared names and lexical bindings from a module scope.
unsafe fn extract_module_declarations(
    scope: &ast::ScopeData,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    ctx: *mut c_void,
    cb: &ModuleCallbacks,
    function_table: &mut ast::FunctionTable,
) {
    unsafe {
        use ast::StatementKind;

        let default_name: ast::Utf16String = utf16!("*default*").into();

        // Var declared names (walk all nesting levels).
        for child in &scope.children {
            collect_module_var_names(&child.inner, ctx, cb.push_var_name);
        }

        // Lexical bindings and functions to initialize.
        let mut function_count: i32 = 0;
        for child in &scope.children {
            let (declaration, is_exported) = match &child.inner {
                StatementKind::Export(export_data) => {
                    if let Some(ref stmt) = export_data.statement {
                        (&stmt.inner, true)
                    } else {
                        continue;
                    }
                }
                other => (other, false),
            };

            match declaration {
                StatementKind::FunctionDeclaration {
                    function_id, name, ..
                } => {
                    let is_default =
                        is_exported && name.as_ref().is_some_and(|n| n.name == default_name);

                    let function_data = function_table.take(*function_id);
                    let subtable = function_table.extract_reachable(&function_data);
                    let sfd_ptr = bytecode::ffi::create_sfd_for_gdi(
                        function_data,
                        subtable,
                        vm_ptr,
                        source_code_ptr,
                        true,
                    );
                    if sfd_ptr.is_null() {
                        continue;
                    }

                    // Get the binding name from the AST (e.g., "*default*" for anonymous defaults).
                    let binding_name = if let Some(name_ident) = name {
                        name_ident.name.clone()
                    } else {
                        continue;
                    };

                    // If default export with *default* name, set the SFD display name to "default".
                    let sfd_name = if is_default {
                        let sfd_display_name = utf16!("default");
                        module_sfd_set_name(
                            sfd_ptr,
                            sfd_display_name.as_ptr(),
                            sfd_display_name.len(),
                        );
                        let display_name: ast::Utf16String = sfd_display_name.into();
                        display_name
                    } else {
                        binding_name.clone()
                    };

                    let function_index = function_count;
                    (cb.push_function)(ctx, sfd_ptr, sfd_name.as_ptr(), sfd_name.len());
                    function_count += 1;

                    // Lexical binding uses the AST name (e.g., "*default*").
                    (cb.push_lexical_binding)(
                        ctx,
                        binding_name.as_ptr(),
                        binding_name.len(),
                        false,
                        function_index,
                    );
                }
                StatementKind::ClassDeclaration(class_data) => {
                    if let Some(ref name_ident) = class_data.name {
                        (cb.push_lexical_binding)(
                            ctx,
                            name_ident.name.as_ptr(),
                            name_ident.name.len(),
                            false,
                            -1,
                        );
                    }
                }
                StatementKind::VariableDeclaration { kind, declarations }
                    if *kind != ast::DeclarationKind::Var =>
                {
                    let is_constant = *kind == ast::DeclarationKind::Const;
                    for declaration in declarations {
                        for_each_bound_name(&declaration.target, &mut |name| {
                            (cb.push_lexical_binding)(
                                ctx,
                                name.as_ptr(),
                                name.len(),
                                is_constant,
                                -1,
                            );
                        });
                    }
                }
                StatementKind::UsingDeclaration { declarations } => {
                    for declaration in declarations {
                        for_each_bound_name(&declaration.target, &mut |name| {
                            (cb.push_lexical_binding)(ctx, name.as_ptr(), name.len(), false, -1);
                        });
                    }
                }
                _ => {}
            }
        }
    }
}

/// Recursively collect var declared names for module scope.
unsafe fn collect_module_var_names(
    statement: &ast::StatementKind,
    ctx: *mut c_void,
    push_var_name: ModuleNameCallback,
) {
    unsafe {
        match statement {
            ast::StatementKind::VariableDeclaration {
                kind: ast::DeclarationKind::Var,
                declarations,
            } => {
                for declaration in declarations {
                    for_each_bound_name(&declaration.target, &mut |name| {
                        push_var_name(ctx, name.as_ptr(), name.len());
                    });
                }
            }
            ast::StatementKind::Export(export_data) => {
                if let Some(ref stmt) = export_data.statement {
                    collect_module_var_names(&stmt.inner, ctx, push_var_name);
                }
            }
            _ => {
                for_each_child_statement(statement, &mut |child| {
                    collect_module_var_names(child, ctx, push_var_name);
                });
            }
        }
    }
}

/// Extract requested modules sorted by source offset.
unsafe fn extract_requested_modules(
    scope: &ast::ScopeData,
    ctx: *mut c_void,
    cb: &ModuleCallbacks,
) {
    unsafe {
        use ast::StatementKind;

        struct RequestedModule {
            source_offset: u32,
            specifier: ast::Utf16String,
            attributes: Vec<ast::ImportAttribute>,
        }

        let mut modules: Vec<RequestedModule> = Vec::new();

        for child in &scope.children {
            match &child.inner {
                StatementKind::Import(import_data) => {
                    modules.push(RequestedModule {
                        source_offset: child.range.start.offset,
                        specifier: import_data.module_request.module_specifier.clone(),
                        attributes: import_data.module_request.attributes.clone(),
                    });
                }
                StatementKind::Export(export_data) => {
                    if let Some(ref mr) = export_data.module_request {
                        modules.push(RequestedModule {
                            source_offset: child.range.start.offset,
                            specifier: mr.module_specifier.clone(),
                            attributes: mr.attributes.clone(),
                        });
                    }
                }
                _ => {}
            }
        }

        // Sort by source offset (spec requirement).
        modules.sort_by_key(|m| m.source_offset);

        for module in &modules {
            let (keys, values) = build_attribute_slices(&module.attributes);
            (cb.push_requested_module)(
                ctx,
                module.specifier.as_ptr(),
                module.specifier.len(),
                keys.as_ptr(),
                values.as_ptr(),
                keys.len(),
            );
        }
    }
}

/// Compile a module body as an async function (for TLA modules).
///
/// Emits async-function wrapping (initial Yield, final Yield) around the
/// module body statements.
unsafe fn compile_module_as_async(
    program: &ast::Statement,
    scope_ref: &Rc<RefCell<ast::ScopeData>>,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    _source: *const u16,
    source_len: usize,
    function_table: ast::FunctionTable,
) -> *mut c_void {
    unsafe {
        use bytecode::generator::Generator;
        use bytecode::instruction::Instruction;
        use bytecode::operand::{Operand, Register};

        let scope = scope_ref.borrow();
        let mut generator = Generator::new();
        generator.strict = true;
        generator.function_table = function_table;
        generator.vm_ptr = vm_ptr;
        generator.source_code_ptr = source_code_ptr;
        generator.source_len = source_len;
        generator.enclosing_function_kind = ast::FunctionKind::Async;

        // Extract local variables from the program scope so the executable has the
        // correct registers_and_locals_count. Without this, locals are not saved
        // across await suspension points, causing them to become undefined.
        generator.local_variables = convert_local_variables(&scope);

        let entry_block = generator.make_block();
        generator.switch_to_basic_block(entry_block);

        // Async function start: emit initial Yield before GetLexicalEnvironment.
        let start_block = generator.make_block();
        let undef = generator.add_constant_undefined();
        generator.emit(Instruction::Yield {
            continuation_label: Some(start_block),
            value: undef.operand(),
        });
        generator.switch_to_basic_block(start_block);

        // Get lexical environment.
        let env_reg =
            generator.scoped_operand(Operand::register(Register::SAVED_LEXICAL_ENVIRONMENT));
        generator.emit(Instruction::GetLexicalEnvironment {
            dst: env_reg.operand(),
        });
        generator.lexical_environment_register_stack.push(env_reg);

        // Generate module body statements.
        let _result = bytecode::codegen::generate_statement(program, &mut generator, None);

        // Async function end: emit final Yield (no continuation = done).
        if !generator.is_current_block_terminated() {
            let undef = generator.add_constant_undefined();
            generator.emit(Instruction::Yield {
                continuation_label: None,
                value: undef.operand(),
            });
        }

        // Terminate all unterminated blocks with Yield.
        generator.terminate_unterminated_blocks_with_yield();

        let assembled = generator.assemble();
        bytecode::ffi::create_executable(&generator, &assembled, vm_ptr, source_code_ptr)
    }
}

unsafe extern "C" {
    fn module_sfd_set_name(sfd_ptr: *mut c_void, name: *const u16, name_len: usize);
}

// =============================================================================
// GDI/EDI metadata extraction
// =============================================================================

/// Recursively collect var-declared names from a statement and all nested
/// statements, excluding function/class bodies (which create new var scopes).
fn collect_var_names_recursive(statement: &ast::StatementKind, push_name: &mut dyn FnMut(&[u16])) {
    match statement {
        ast::StatementKind::VariableDeclaration {
            kind: ast::DeclarationKind::Var,
            declarations,
        } => {
            for declaration in declarations {
                for_each_bound_name(&declaration.target, push_name);
            }
        }
        _ => {
            for_each_child_statement(statement, &mut |child| {
                collect_var_names_recursive(child, push_name);
            });
        }
    }
}

/// Collect var names + function declaration names, deduplicated function
/// initializations, var-scoped names, annex B names, and lexical bindings.
///
/// Shared by both script and eval GDI extraction. All unsafe FFI calls are
/// confined to the closures passed in by the caller.
#[allow(clippy::too_many_arguments)]
fn extract_gdi_common(
    scope: &ast::ScopeData,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    is_strict: bool,
    push_var_name: &mut dyn FnMut(&[u16]),
    push_function: &mut dyn FnMut(*mut c_void, &[u16]),
    push_var_scoped_name: &mut dyn FnMut(&[u16]),
    push_annex_b_name: &mut dyn FnMut(&[u16]),
    push_lexical_binding: &mut dyn FnMut(&[u16], bool),
    function_table: &mut ast::FunctionTable,
) {
    use ast::{DeclarationKind, StatementKind};

    // Var names (var declarations at any nesting level + top-level function declarations)
    for child in &scope.children {
        collect_var_names_recursive(&child.inner, push_var_name);
        if let StatementKind::FunctionDeclaration {
            name: Some(ref name_ident),
            ..
        } = child.inner
        {
            push_var_name(&name_ident.name);
        }
    }

    // Functions to initialize (reverse order, deduplicated by name).
    let mut seen_names: HashSet<ast::Utf16String> = HashSet::new();
    let mut functions_to_init: Vec<(ast::FunctionId, ast::Utf16String)> = Vec::new();
    for child in scope.children.iter().rev() {
        if let StatementKind::FunctionDeclaration {
            function_id,
            name: Some(ref name_ident),
            ..
        } = child.inner
            && seen_names.insert(name_ident.name.clone()) {
                functions_to_init.push((function_id, name_ident.name.clone()));
            }
    }
    for (function_id, name) in &functions_to_init {
        let function_data = function_table.take(*function_id);
        let subtable = function_table.extract_reachable(&function_data);
        let sfd_ptr = unsafe {
            bytecode::ffi::create_sfd_for_gdi(
                function_data,
                subtable,
                vm_ptr,
                source_code_ptr,
                is_strict,
            )
        };
        assert!(!sfd_ptr.is_null(), "create_sfd_for_gdi returned null");
        push_function(sfd_ptr, name);
    }

    // Var-scoped names (var VariableDeclaration names, excluding function declarations)
    for child in &scope.children {
        collect_var_names_recursive(&child.inner, push_var_scoped_name);
    }

    for name in &scope.annexb_function_names {
        push_annex_b_name(name);
    }

    for child in &scope.children {
        match &child.inner {
            StatementKind::VariableDeclaration { kind, declarations }
                if *kind != DeclarationKind::Var =>
            {
                let is_constant = *kind == DeclarationKind::Const;
                for declaration in declarations {
                    for_each_bound_name(&declaration.target, &mut |name| {
                        push_lexical_binding(name, is_constant);
                    });
                }
            }
            StatementKind::UsingDeclaration { declarations } => {
                for declaration in declarations {
                    for_each_bound_name(&declaration.target, &mut |name| {
                        push_lexical_binding(name, false);
                    });
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                if let Some(ref name) = class_data.name {
                    push_lexical_binding(&name.name, false);
                }
            }
            _ => {}
        }
    }
}

/// Extract EDI metadata from a program-level ScopeData and populate
/// the C++ EvalGdiBuilder via callbacks.
unsafe fn extract_eval_gdi(
    scope: &ast::ScopeData,
    is_strict: bool,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    ctx: *mut c_void,
    function_table: &mut ast::FunctionTable,
) {
    unsafe {
        use bytecode::ffi::{
            eval_gdi_push_annex_b_name, eval_gdi_push_function, eval_gdi_push_lexical_binding,
            eval_gdi_push_var_name, eval_gdi_push_var_scoped_name, eval_gdi_set_strict,
        };

        eval_gdi_set_strict(ctx, is_strict);

        extract_gdi_common(
            scope,
            vm_ptr,
            source_code_ptr,
            is_strict,
            &mut |name| eval_gdi_push_var_name(ctx, name.as_ptr(), name.len()),
            &mut |sfd_ptr, name| eval_gdi_push_function(ctx, sfd_ptr, name.as_ptr(), name.len()),
            &mut |name| eval_gdi_push_var_scoped_name(ctx, name.as_ptr(), name.len()),
            &mut |name| eval_gdi_push_annex_b_name(ctx, name.as_ptr(), name.len()),
            &mut |name, is_const| {
                eval_gdi_push_lexical_binding(ctx, name.as_ptr(), name.len(), is_const)
            },
            function_table,
        );
    }
}

/// Extract GDI metadata from a program-level ScopeData and populate
/// the C++ ScriptGdiBuilder via callbacks.
unsafe fn extract_script_gdi(
    scope: &ast::ScopeData,
    is_strict: bool,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    ctx: *mut c_void,
    function_table: &mut ast::FunctionTable,
) {
    unsafe {
        use ast::{DeclarationKind, StatementKind};
        use bytecode::ffi::{
            script_gdi_push_annex_b_name, script_gdi_push_function,
            script_gdi_push_lexical_binding, script_gdi_push_lexical_name,
            script_gdi_push_var_name, script_gdi_push_var_scoped_name,
        };

        // Lexical names (let/const/using/class at top level) — script-only step.
        for child in &scope.children {
            match &child.inner {
                StatementKind::VariableDeclaration { kind, declarations }
                    if *kind != DeclarationKind::Var =>
                {
                    for declaration in declarations {
                        for_each_bound_name(&declaration.target, &mut |name| {
                            script_gdi_push_lexical_name(ctx, name.as_ptr(), name.len());
                        });
                    }
                }
                StatementKind::UsingDeclaration { declarations } => {
                    for declaration in declarations {
                        for_each_bound_name(&declaration.target, &mut |name| {
                            script_gdi_push_lexical_name(ctx, name.as_ptr(), name.len());
                        });
                    }
                }
                StatementKind::ClassDeclaration(class_data) => {
                    if let Some(ref name) = class_data.name {
                        script_gdi_push_lexical_name(ctx, name.name.as_ptr(), name.name.len());
                    }
                }
                _ => {}
            }
        }

        extract_gdi_common(
            scope,
            vm_ptr,
            source_code_ptr,
            is_strict,
            &mut |name| script_gdi_push_var_name(ctx, name.as_ptr(), name.len()),
            &mut |sfd_ptr, name| script_gdi_push_function(ctx, sfd_ptr, name.as_ptr(), name.len()),
            &mut |name| script_gdi_push_var_scoped_name(ctx, name.as_ptr(), name.len()),
            &mut |name| script_gdi_push_annex_b_name(ctx, name.as_ptr(), name.len()),
            &mut |name, is_const| {
                script_gdi_push_lexical_binding(ctx, name.as_ptr(), name.len(), is_const)
            },
            function_table,
        );
    }
}

/// Visit each child statement of a statement, excluding function/class bodies
/// (which create new var scopes). This enables recursive var-declaration walking.
fn for_each_child_statement(
    statement: &ast::StatementKind,
    f: &mut dyn FnMut(&ast::StatementKind),
) {
    use ast::StatementKind;

    match statement {
        StatementKind::Block(scope) => {
            for child in &scope.borrow().children {
                f(&child.inner);
            }
        }
        StatementKind::If {
            consequent,
            alternate,
            ..
        } => {
            f(&consequent.inner);
            if let Some(alt) = alternate {
                f(&alt.inner);
            }
        }
        StatementKind::While { body, .. }
        | StatementKind::DoWhile { body, .. }
        | StatementKind::With { body, .. } => {
            f(&body.inner);
        }
        StatementKind::For { init, body, .. } => {
            if let Some(ast::ForInit::Declaration(decl)) = init {
                f(&decl.inner);
            }
            f(&body.inner);
        }
        StatementKind::ForInOf { lhs, body, .. } => {
            if let ast::ForInOfLhs::Declaration(declaration) = lhs {
                f(&declaration.inner);
            }
            f(&body.inner);
        }
        StatementKind::Switch(data) => {
            for case in &data.cases {
                for child in &case.scope.borrow().children {
                    f(&child.inner);
                }
            }
        }
        StatementKind::Try(data) => {
            f(&data.block.inner);
            if let Some(ref handler) = data.handler {
                f(&handler.body.inner);
            }
            if let Some(ref finalizer) = data.finalizer {
                f(&finalizer.inner);
            }
        }
        StatementKind::Labelled { item, .. } => {
            f(&item.inner);
        }
        // Don't recurse into function/class bodies (new var scopes)
        _ => {}
    }
}

fn for_each_bound_name(target: &ast::VariableDeclaratorTarget, f: &mut dyn FnMut(&[u16])) {
    match target {
        ast::VariableDeclaratorTarget::Identifier(id) => f(&id.name),
        ast::VariableDeclaratorTarget::BindingPattern(pattern) => {
            for_each_bound_name_in_pattern(pattern, f);
        }
    }
}

fn for_each_bound_name_in_pattern(pattern: &ast::BindingPattern, f: &mut dyn FnMut(&[u16])) {
    for entry in &pattern.entries {
        match &entry.alias {
            None => {
                if let Some(ast::BindingEntryName::Identifier(id)) = &entry.name {
                    f(&id.name);
                }
            }
            Some(ast::BindingEntryAlias::Identifier(id)) => f(&id.name),
            Some(ast::BindingEntryAlias::BindingPattern(inner)) => {
                for_each_bound_name_in_pattern(inner, f);
            }
            Some(ast::BindingEntryAlias::MemberExpression(_)) => {}
        }
    }
}

// =============================================================================
// FFI entry points: memory management and function compilation
// =============================================================================

/// Free a `Box<FunctionData>` stored in a C++ SharedFunctionInstanceData.
///
/// Called from the SFD's `finalize()` or `clear_compile_inputs()` when the
/// AST is no longer needed.
///
/// # Safety
/// `ast` must be a valid pointer returned by `Box::into_raw(Box<FunctionData>)`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_free_function_ast(ast: *mut c_void) {
    unsafe {
        abort_on_panic(|| {
            if !ast.is_null() {
                drop(Box::from_raw(ast as *mut ast::FunctionPayload));
            }
        });
    }
}

/// Free a string allocated by Rust (e.g. AST dump output).
///
/// # Safety
/// `ptr` and `len` must correspond to a `Box<[u8]>` previously leaked via `std::mem::forget`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_free_string(ptr: *mut u8, len: usize) {
    unsafe {
        abort_on_panic(|| {
            if !ptr.is_null() {
                drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(ptr, len)));
            }
        });
    }
}

/// Compile a function body.
///
/// Takes ownership of the `Box<FunctionData>` and compiles it into a
/// C++ `Bytecode::Executable`. Also populates FDI runtime metadata on the
/// `SharedFunctionInstanceData`.
///
/// # Safety
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
/// - `sfd_ptr` must be a valid `JS::SharedFunctionInstanceData*`.
/// - `rust_function_ast` must be a valid `Box<FunctionData>` pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compile_function(
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    _source: *const u16,
    source_len: usize,
    sfd_ptr: *mut c_void,
    rust_function_ast: *mut c_void,
    builtin_abstract_operations_enabled: bool,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            if rust_function_ast.is_null() {
                return std::ptr::null_mut();
            }
            let payload = Box::from_raw(rust_function_ast as *mut ast::FunctionPayload);
            let function_data = Box::new(payload.data);

            let body_scope = match &function_data.body.inner {
                StatementKind::FunctionBody { scope, .. } => Some(scope),
                StatementKind::Block(scope) => Some(scope),
                _ => None,
            };

            // Compute SFD metadata before codegen so the generator can use
            // function_environment_needed to optimize `this` access.
            let sfd_metadata = compute_sfd_metadata(&function_data);

            let mut generator = bytecode::generator::Generator::new();
            generator.strict = function_data.is_strict_mode;
            generator.function_environment_needed = sfd_metadata.function_environment_needed;
            generator.builtin_abstract_operations_enabled = builtin_abstract_operations_enabled;
            generator.function_table = payload.function_table;
            generator.vm_ptr = vm_ptr;
            generator.source_code_ptr = source_code_ptr;
            generator.source_len = source_len;
            generator.enclosing_function_kind = function_data.kind;

            if let Some(scope) = body_scope {
                generator.local_variables = convert_local_variables(&scope.borrow());
            }

            let entry_block = generator.make_block();
            generator.switch_to_basic_block(entry_block);

            // https://tc39.es/ecma262/#sec-async-functions-abstract-operations-async-function-start
            // For async (non-generator) functions, emit the initial Yield BEFORE
            // GetLexicalEnvironment so that parameter evaluation errors are caught
            // by the async promise wrapper. This matches C++ ordering.
            if generator.is_in_async_function() && !generator.is_in_generator_function() {
                let start_block = generator.make_block();
                let undef = generator.add_constant_undefined();
                generator.emit(bytecode::instruction::Instruction::Yield {
                    continuation_label: Some(start_block),
                    value: undef.operand(),
                });
                generator.switch_to_basic_block(start_block);
            }

            {
                use bytecode::operand::{Operand, Register};
                let env_reg = generator
                    .scoped_operand(Operand::register(Register::SAVED_LEXICAL_ENVIRONMENT));
                generator.emit(bytecode::instruction::Instruction::GetLexicalEnvironment {
                    dst: env_reg.operand(),
                });
                generator.lexical_environment_register_stack.push(env_reg);
            }

            if let Some(scope) = body_scope {
                bytecode::codegen::emit_function_declaration_instantiation(
                    &mut generator,
                    &function_data,
                    &scope.borrow(),
                );
            }

            // https://tc39.es/ecma262/#sec-generatorstart
            // For generator functions (including async generators), emit the initial Yield
            // AFTER FDI. Parameter evaluation happens synchronously before the generator starts.
            if generator.is_in_generator_function() {
                let start_block = generator.make_block();
                let undef = generator.add_constant_undefined();
                generator.emit(bytecode::instruction::Instruction::Yield {
                    continuation_label: Some(start_block),
                    value: undef.operand(),
                });
                generator.switch_to_basic_block(start_block);
            }

            let result =
                bytecode::codegen::generate_statement(&function_data.body, &mut generator, None);

            if !generator.is_current_block_terminated() {
                if generator.is_in_generator_or_async_function() {
                    // Generator/async functions end with Yield (no continuation = done).
                    let undef = generator.add_constant_undefined();
                    generator.emit(bytecode::instruction::Instruction::Yield {
                        continuation_label: None,
                        value: undef.operand(),
                    });
                } else if let Some(value) = result {
                    generator.emit(bytecode::instruction::Instruction::End {
                        value: value.operand(),
                    });
                }
                // If result is None, the assembler will add End(undefined) as a
                // fallthrough for unterminated blocks, matching C++ compile().
            }

            // For generator/async functions, terminate all unterminated blocks with Yield.
            if generator.is_in_generator_or_async_function() {
                generator.terminate_unterminated_blocks_with_yield();
            }

            let assembled = generator.assemble();

            write_sfd_metadata(sfd_ptr, &sfd_metadata);

            bytecode::ffi::create_executable(&generator, &assembled, vm_ptr, source_code_ptr)
        })
    }
}

// =============================================================================
// SFD metadata computation (ECMA-262 section 10.2.11)
// =============================================================================

/// Metadata computed from scope analysis for a SharedFunctionInstanceData.
struct SfdMetadata {
    uses_this: bool,
    function_environment_needed: bool,
    function_environment_bindings_count: usize,
    might_need_arguments: bool,
    contains_eval: bool,
}

/// Intermediate scope analysis data extracted from the function body scope.
struct BodyScopeInfo {
    uses_this: bool,
    contains_eval: bool,
    uses_this_from_env: bool,
    might_need_arguments: bool,
    has_function_named_arguments: bool,
    has_lexically_declared_arguments: bool,
    non_local_var_count: usize,
    non_local_var_count_for_parameter_expressions: usize,
    var_names: Vec<ast::Utf16String>,
    annexb_function_names: Vec<ast::Utf16String>,
    has_arguments_object_local: bool,
}

/// Compute FDI runtime metadata matching the C++ SharedFunctionInstanceData
/// constructor (ECMA-262 §10.2.11).
fn compute_sfd_metadata(function_data: &ast::FunctionData) -> SfdMetadata {
    let body_scope = match &function_data.body.inner {
        ast::StatementKind::FunctionBody { scope, .. } => Some(scope),
        _ => None,
    };

    let strict = function_data.is_strict_mode;
    let is_arrow = function_data.is_arrow_function;

    // Extract all scope analysis data in one borrow.
    let bsi = if let Some(scope) = &body_scope {
        let sd = scope.borrow();
        let fsd = sd.function_scope_data.as_ref();
        BodyScopeInfo {
            uses_this: sd.uses_this || function_data.parsing_insights.uses_this,
            contains_eval: sd.contains_direct_call_to_eval,
            uses_this_from_env: sd.uses_this_from_environment
                || function_data.parsing_insights.uses_this_from_environment,
            might_need_arguments: function_data.parsing_insights.might_need_arguments_object,
            has_function_named_arguments: fsd.is_some_and(|f| f.has_function_named_arguments),
            has_lexically_declared_arguments: fsd
                .is_some_and(|f| f.has_lexically_declared_arguments),
            non_local_var_count: fsd.map_or(0, |f| f.non_local_var_count),
            non_local_var_count_for_parameter_expressions: fsd
                .map_or(0, |f| f.non_local_var_count_for_parameter_expressions),
            var_names: fsd.map(|f| &f.var_names).cloned().unwrap_or_default(),
            annexb_function_names: sd.annexb_function_names.clone(),
            has_arguments_object_local: sd
                .local_variables
                .iter()
                .any(|lv| lv.kind == ast::LocalVarKind::ArgumentsObject),
        }
    } else {
        BodyScopeInfo {
            uses_this: function_data.parsing_insights.uses_this,
            contains_eval: function_data.parsing_insights.contains_direct_call_to_eval,
            uses_this_from_env: function_data.parsing_insights.uses_this_from_environment,
            might_need_arguments: function_data.parsing_insights.might_need_arguments_object,
            has_function_named_arguments: false,
            has_lexically_declared_arguments: false,
            non_local_var_count: 0,
            non_local_var_count_for_parameter_expressions: 0,
            var_names: Vec::new(),
            annexb_function_names: Vec::new(),
            has_arguments_object_local: false,
        }
    };

    // §10.2.11 step 4: check for parameter expressions.
    let has_parameter_expressions = function_data.parameters.iter().any(|p| {
        p.default_value.is_some()
            || matches!(p.binding, ast::FunctionParameterBinding::BindingPattern(_))
    });

    // §10.2.11 steps 5-8: count non-local unique parameter names.
    let mut parameter_names: HashSet<ast::Utf16String> = HashSet::new();
    let mut parameters_in_environment: usize = 0;
    for parameter in &function_data.parameters {
        match &parameter.binding {
            ast::FunctionParameterBinding::Identifier(ident) => {
                if parameter_names.insert(ident.name.clone()) && !ident.is_local() {
                    parameters_in_environment += 1;
                }
            }
            ast::FunctionParameterBinding::BindingPattern(pattern) => {
                for_each_binding_pattern_identifier(pattern, &mut |ident| {
                    if parameter_names.insert(ident.name.clone()) && !ident.is_local() {
                        parameters_in_environment += 1;
                    }
                });
            }
        }
    }

    // §10.2.11 steps 15-18: determine if arguments object is needed.
    let arguments_object_needed = bsi.might_need_arguments
        && !is_arrow
        && !parameter_names.contains(utf16!("arguments"))
        && body_scope.is_some()
        && (has_parameter_expressions || !bsi.has_function_named_arguments)
        && (has_parameter_expressions || !bsi.has_lexically_declared_arguments);

    // Arguments object needs an environment binding if it's not a local variable.
    let arguments_object_needs_binding = arguments_object_needed && !bsi.has_arguments_object_local;

    let mut function_environment_bindings_count: usize = 0;
    let mut var_environment_bindings_count: usize = 0;
    let mut lex_environment_bindings_count: usize = 0;

    // §10.2.11 step 19: route parameter bindings.
    let env_is_function_env = strict || !has_parameter_expressions;
    if env_is_function_env {
        function_environment_bindings_count += parameters_in_environment;
    }

    // §10.2.11 step 22: arguments binding.
    if arguments_object_needs_binding && env_is_function_env {
        function_environment_bindings_count += 1;
    }

    if let Some(body_scope) = body_scope {
        if !has_parameter_expressions {
            // §10.2.11 step 27: var env shares function env.
            function_environment_bindings_count += bsi.non_local_var_count;

            // Annex B: function names hoisted from blocks that aren't already vars.
            if !strict {
                for name in &bsi.annexb_function_names {
                    if !bsi.var_names.contains(name) {
                        function_environment_bindings_count += 1;
                    }
                }
            }

            // §10.2.11 step 30: lexical environment.
            let non_local_lex_count = count_non_local_lex_declarations(body_scope);
            if strict {
                // Lex env == var env == function env.
                function_environment_bindings_count += non_local_lex_count;
            } else {
                let can_elide = !bsi.contains_eval && non_local_lex_count == 0;
                if !can_elide {
                    lex_environment_bindings_count += non_local_lex_count;
                }
            }
        } else {
            // §10.2.11 step 28: separate var environment.
            var_environment_bindings_count += bsi.non_local_var_count_for_parameter_expressions;

            if !strict {
                for name in &bsi.annexb_function_names {
                    if !bsi.var_names.contains(name) {
                        var_environment_bindings_count += 1;
                    }
                }
            }

            let non_local_lex_count = count_non_local_lex_declarations(body_scope);
            if strict {
                // Lex env == var env.
                var_environment_bindings_count += non_local_lex_count;
            } else {
                let can_elide = !bsi.contains_eval && non_local_lex_count == 0;
                if !can_elide {
                    lex_environment_bindings_count += non_local_lex_count;
                }
            }
        }
    }

    let function_environment_needed = arguments_object_needs_binding
        || function_environment_bindings_count > 0
        || var_environment_bindings_count > 0
        || lex_environment_bindings_count > 0
        || bsi.uses_this_from_env
        || bsi.contains_eval;

    SfdMetadata {
        uses_this: bsi.uses_this,
        function_environment_needed,
        function_environment_bindings_count,
        might_need_arguments: bsi.might_need_arguments,
        contains_eval: bsi.contains_eval,
    }
}

/// Write precomputed SFD metadata to a C++ SharedFunctionInstanceData via FFI.
///
/// # Safety
/// `sfd_ptr` must be a valid `JS::SharedFunctionInstanceData*`.
unsafe fn write_sfd_metadata(sfd_ptr: *mut c_void, metadata: &SfdMetadata) {
    unsafe {
        rust_sfd_set_metadata(
            sfd_ptr,
            metadata.uses_this,
            metadata.function_environment_needed,
            metadata.function_environment_bindings_count,
            metadata.might_need_arguments,
            metadata.contains_eval,
        );
    }
}

/// Count non-local lexically-declared identifiers in a function body scope.
/// Returns the count (used for environment sizing in the function_environment_needed
/// computation).
fn count_non_local_lex_declarations(scope: &Rc<RefCell<ast::ScopeData>>) -> usize {
    let sd = scope.borrow();
    let mut count = 0;
    for child in &sd.children {
        match &child.inner {
            ast::StatementKind::VariableDeclaration { kind, declarations } => {
                use parser::DeclarationKind;
                if *kind == DeclarationKind::Let || *kind == DeclarationKind::Const {
                    for declaration in declarations {
                        count_non_local_names_in_target(&declaration.target, &mut count);
                    }
                }
            }
            ast::StatementKind::UsingDeclaration { declarations } => {
                for declaration in declarations {
                    count_non_local_names_in_target(&declaration.target, &mut count);
                }
            }
            ast::StatementKind::ClassDeclaration(class_data) => {
                if let Some(ref name_ident) = class_data.name
                    && !name_ident.is_local() {
                        count += 1;
                    }
            }
            _ => {}
        }
    }
    count
}

fn count_non_local_names_in_target(target: &ast::VariableDeclaratorTarget, count: &mut usize) {
    match target {
        ast::VariableDeclaratorTarget::Identifier(ident) => {
            if !ident.is_local() {
                *count += 1;
            }
        }
        ast::VariableDeclaratorTarget::BindingPattern(pattern) => {
            count_non_local_names_in_binding_pattern(pattern, count);
        }
    }
}

fn count_non_local_names_in_binding_pattern(pattern: &ast::BindingPattern, count: &mut usize) {
    for entry in &pattern.entries {
        match &entry.alias {
            Some(ast::BindingEntryAlias::Identifier(ident)) => {
                if !ident.is_local() {
                    *count += 1;
                }
            }
            Some(ast::BindingEntryAlias::BindingPattern(sub)) => {
                count_non_local_names_in_binding_pattern(sub, count);
            }
            None => {
                if let Some(ast::BindingEntryName::Identifier(ident)) = &entry.name
                    && !ident.is_local() {
                        *count += 1;
                    }
            }
            Some(ast::BindingEntryAlias::MemberExpression(_)) => {}
        }
    }
}

fn for_each_binding_pattern_identifier(
    pattern: &ast::BindingPattern,
    callback: &mut dyn FnMut(&Rc<ast::Identifier>),
) {
    for entry in &pattern.entries {
        match &entry.alias {
            Some(ast::BindingEntryAlias::Identifier(ident)) => callback(ident),
            Some(ast::BindingEntryAlias::BindingPattern(sub)) => {
                for_each_binding_pattern_identifier(sub, callback);
            }
            None => {
                if let Some(ast::BindingEntryName::Identifier(ident)) = &entry.name {
                    callback(ident);
                }
            }
            Some(ast::BindingEntryAlias::MemberExpression(_)) => {}
        }
    }
}

unsafe extern "C" {
    fn rust_sfd_set_metadata(
        sfd_ptr: *mut c_void,
        uses_this: bool,
        function_environment_needed: bool,
        function_environment_bindings_count: usize,
        might_need_arguments_object: bool,
        contains_direct_call_to_eval: bool,
    );
}
