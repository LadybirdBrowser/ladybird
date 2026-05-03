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

#[path = "../../../RustAllocator.rs"]
mod rust_allocator;

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
mod bytecode_cache;
pub mod fast_hash;
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
use bytecode::generator::PendingSharedFunctionData;
use parser::{ParseError, Parser, ProgramType};
use std::collections::HashSet;
use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};

// Compile-time assertion: `ParsedProgram` travels between the parse worker
// thread and the main thread, so it must be `Send`. After the StringId and
// ScopeId arena migrations the AST itself contains no `Rc`/`Cell`/`RefCell`
// values, so this is naturally satisfied without `unsafe impl Send`.
const _: fn() = || {
    fn assert_send<T: Send>() {}
    assert_send::<ParsedProgram>();
};

// =============================================================================
// ParsedProgram: GC-free parse result for off-thread parsing
// =============================================================================

/// A parsed program (script or module) that can be compiled later.
/// Contains no GC references, so it can safely be transferred between threads.
pub struct ParsedProgram {
    program: ast::Statement,
    function_table: ast::FunctionTable,
    arena: std::sync::Arc<ast::AstArena>,
    scope_ref: ast::ScopeId,
    program_type: ast::ProgramType,
    is_strict_mode: bool,
    has_top_level_await: bool,
    errors: Vec<ParseError>,
    ast_dump: Option<Vec<u8>>,
}

pub struct CompiledProgram {
    parsed: ParsedProgram,
    bytecode: CompiledProgramBytecode,
    declaration_functions: Vec<PendingSharedFunctionData>,
}

pub struct CompiledFunction {
    precompiled: Box<bytecode::generator::PrecompiledFunction>,
}

#[repr(C)]
pub struct BytecodeCacheBlob {
    data: *mut u8,
    length: usize,
}

pub struct DecodedBytecodeCacheBlob {
    _blob: bytecode_cache::DecodedCacheBlob,
}

enum CompiledProgramBytecode {
    Program(CompiledBytecode),
    AsyncModule(CompiledBytecode),
}

struct CompiledBytecode {
    generator: bytecode::generator::Generator,
    assembled: bytecode::generator::AssembledBytecode,
}

// SAFETY: `CompiledProgram` owns codegen state that uses `Rc`/`RefCell` and
// raw VM pointers; it is created on the parse-worker thread and consumed (or
// freed) on the main thread, never accessed concurrently.
unsafe impl Send for CompiledProgram {}

// SAFETY: `CompiledFunction` owns GC-free codegen state and is transferred
// from a compile worker back to the main thread for materialization.
unsafe impl Send for CompiledFunction {}

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
    arena: &ast::AstArena,
    output_ptr: *mut *mut u8,
    output_len: *mut usize,
) {
    unsafe {
        if output_ptr.is_null() || output_len.is_null() {
            return;
        }
        let dump_string = ast_dump::dump_program_to_string(program, function_table, arena);
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
pub type ParseErrorCallback = Option<
    unsafe extern "C" fn(ctx: *mut c_void, message: *const u8, message_len: usize, line: u32, column: u32) -> (),
>;

/// Log parser and scope collector errors, returning true if any were found.
fn check_errors(parser: &mut Parser) -> bool {
    check_errors_with_callback(parser, std::ptr::null_mut(), None)
}

/// Check for errors, optionally reporting them via a C++ callback.
fn check_errors_with_callback(
    parser: &mut Parser,
    error_context: *mut c_void,
    error_callback: ParseErrorCallback,
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

/// Shared codegen pipeline: local variable setup → bytecode generation → assembly.
///
/// This deliberately stops before `create_executable()`, because executable materialization creates GC-managed objects
/// and resolves VM-specific constants. Keeping that work separate lets WebContent perform the expensive AST-to-bytecode
/// pass on a worker thread while preserving all main-thread ownership rules for VM and heap data.
fn compile_program_body_to_bytecode(
    generator: &mut bytecode::generator::Generator,
    program: &ast::Statement,
    scope_id: ast::ScopeId,
) -> bytecode::generator::AssembledBytecode {
    let arena_clone = generator.arena.clone();
    generator.local_variables = convert_local_variables(&arena_clone.scopes[scope_id]);

    let entry_block = generator.make_block();
    generator.switch_to_basic_block(entry_block);
    generator.capture_saved_lexical_environment();

    let result = bytecode::codegen::generate_statement(program, generator, None);

    if !generator.is_current_block_terminated()
        && let Some(value) = result
    {
        generator.emit(bytecode::instruction::Instruction::End { value: value.operand() });
    }
    // If result is None, the assembler will add End(undefined) as a fallthrough for unterminated blocks, matching C++.

    generator.assemble()
}

unsafe fn create_executable_from_compiled_bytecode(
    bytecode: &mut CompiledBytecode,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
) -> *mut c_void {
    unsafe {
        bytecode.generator.vm_ptr = vm_ptr;
        bytecode.generator.source_code_ptr = source_code_ptr;
        bytecode::ffi::create_executable(&mut bytecode.generator, &bytecode.assembled, vm_ptr, source_code_ptr)
    }
}

#[derive(Clone, Copy, PartialEq)]
enum FunctionPrecompileMode {
    EagerOnly,
    All,
}

fn precompile_functions(generator: &mut bytecode::generator::Generator, mode: FunctionPrecompileMode) {
    for pending in &mut generator.shared_function_data {
        if pending.precompiled_function.is_some() {
            continue;
        }
        if matches!(mode, FunctionPrecompileMode::EagerOnly) && !pending.should_eager_compile {
            continue;
        }

        let function_data = pending
            .function_data
            .take()
            .expect("pending eager function data was already materialized");
        let subtable = pending
            .subtable
            .take()
            .expect("pending eager function subtable was already materialized");
        let arena = pending.arena.clone().unwrap_or_else(|| generator.arena.clone());
        let payload = ast::FunctionPayload {
            data: *function_data,
            function_table: subtable,
            arena: arena.clone(),
        };
        let (function_data, precompiled) = compile_function_payload_to_bytecode(
            payload,
            generator.source_len,
            generator.builtin_abstract_operations_enabled,
            arena,
            mode,
        );

        pending.function_data = Some(function_data);
        // The precompiled executable owns any nested lazy function payloads. Keep
        // an empty payload here only until materialization creates the SFD; it is
        // immediately cleared after the precompiled executable is attached.
        pending.subtable = Some(ast::FunctionTable::new());
        pending.precompiled_function = Some(precompiled);
    }
}

fn precompile_declaration_functions(
    program_type: ast::ProgramType,
    scope_id: ast::ScopeId,
    generator: &mut bytecode::generator::Generator,
    mode: FunctionPrecompileMode,
) -> Vec<PendingSharedFunctionData> {
    if mode != FunctionPrecompileMode::All {
        return Vec::new();
    }

    match program_type {
        ast::ProgramType::Script => precompile_script_declaration_functions(scope_id, generator, mode),
        ast::ProgramType::Module => precompile_module_declaration_functions(scope_id, generator, mode),
    }
}

fn precompile_script_declaration_functions(
    scope_id: ast::ScopeId,
    generator: &mut bytecode::generator::Generator,
    mode: FunctionPrecompileMode,
) -> Vec<PendingSharedFunctionData> {
    use ast::StatementKind;

    let arena = generator.arena.clone();
    let scope = &arena.scopes[scope_id];
    let mut last_position: std::collections::HashMap<ast::StringId, usize> = std::collections::HashMap::new();
    for (index, child) in scope.children.iter().enumerate() {
        if let StatementKind::FunctionDeclaration(ref function) = child.inner
            && let Some(name) = function.name
        {
            last_position.insert(arena.identifiers[name].name, index);
        }
    }

    let mut declaration_functions = Vec::new();
    for (index, child) in scope.children.iter().enumerate() {
        if let StatementKind::FunctionDeclaration(ref function) = child.inner
            && let Some(name) = function.name
            && last_position.get(&arena.identifiers[name].name).copied() == Some(index)
        {
            declaration_functions.push(precompile_declaration_function(
                function.function_id,
                None,
                generator,
                mode,
            ));
        }
    }

    declaration_functions
}

fn precompile_module_declaration_functions(
    scope_id: ast::ScopeId,
    generator: &mut bytecode::generator::Generator,
    mode: FunctionPrecompileMode,
) -> Vec<PendingSharedFunctionData> {
    use ast::StatementKind;

    let arena = generator.arena.clone();
    let scope = &arena.scopes[scope_id];
    let default_name: ast::Utf16String = utf16!("*default*").into();
    let mut declaration_functions = Vec::new();
    for child in &scope.children {
        let (declaration, is_exported) = match &child.inner {
            StatementKind::Export(export_data) => {
                if let Some(ref statement) = export_data.statement {
                    (&statement.inner, true)
                } else {
                    continue;
                }
            }
            other => (other, false),
        };

        if let StatementKind::FunctionDeclaration(function) = declaration {
            let is_default = is_exported
                && function
                    .name
                    .is_some_and(|name| arena.name_slice(name) == default_name.as_slice());
            let name_override = if is_default {
                Some(utf16!("default").into())
            } else {
                None
            };
            declaration_functions.push(precompile_declaration_function(
                function.function_id,
                name_override,
                generator,
                mode,
            ));
        }
    }

    declaration_functions
}

fn precompile_declaration_function(
    function_id: ast::FunctionId,
    name_override: Option<ast::Utf16String>,
    generator: &mut bytecode::generator::Generator,
    mode: FunctionPrecompileMode,
) -> PendingSharedFunctionData {
    let function_data = generator.function_table.take(function_id);
    let arena = generator.arena.clone();
    let subtable = generator
        .function_table
        .extract_reachable(&function_data, &arena.scopes);
    let payload = ast::FunctionPayload {
        data: *function_data,
        function_table: subtable,
        arena: arena.clone(),
    };
    let (function_data, precompiled_function) = compile_function_payload_to_bytecode(
        payload,
        generator.source_len,
        generator.builtin_abstract_operations_enabled,
        arena.clone(),
        mode,
    );

    PendingSharedFunctionData {
        function_data: Some(function_data),
        subtable: Some(ast::FunctionTable::new()),
        arena: Some(arena),
        name_override,
        class_field_initializer_name: None,
        should_eager_compile: false,
        precompiled_function: Some(precompiled_function),
    }
}

/// Shared compilation pipeline: local variable setup → codegen → assemble → create Executable.
///
/// Called by program-level entry points that compile synchronously on the main thread.
unsafe fn compile_program_body(
    generator: &mut bytecode::generator::Generator,
    program: &ast::Statement,
    scope_id: ast::ScopeId,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
) -> *mut c_void {
    let assembled = compile_program_body_to_bytecode(generator, program, scope_id);
    unsafe { bytecode::ffi::create_executable(generator, &assembled, vm_ptr, source_code_ptr) }
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

            parser.scope_collector.analyze(
                initiated_by_eval,
                &mut parser.arena.identifiers,
                &parser.arena.strings,
                &mut parser.arena.scopes,
            );

            let scope_id = if let StatementKind::Program(ref data) = program.inner {
                data.scope
            } else {
                return std::ptr::null_mut();
            };

            let mut generator = new_program_generator(starts_in_strict_mode, vm_ptr, source_code_ptr, source_len);
            generator.function_table = std::mem::take(&mut parser.function_table);
            generator.arena = std::sync::Arc::new(std::mem::take(&mut parser.arena));
            // Make a clone of the Arc so we can keep using it after generator is consumed.
            compile_program_body(&mut generator, &program, scope_id, vm_ptr, source_code_ptr)
        })
    }
}

/// Parse a program (script or module) without any GC interaction.
///
/// Lexes, parses, and runs scope analysis. The result is a `ParsedProgram`
/// that can be compiled later via `rust_compile_parsed_script()` or
/// `rust_compile_parsed_module()`.
///
/// `program_type`: 0 = Script, 1 = Module.
///
/// Returns nullptr if `source` is null. Otherwise returns a non-null
/// pointer. Caller must check for errors via
/// `rust_parsed_program_has_errors()` before compiling.
///
/// # Safety
/// - `source` must point to a valid UTF-16 buffer of `source_len` elements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_program(
    source: *const u16,
    source_len: usize,
    program_type: u8,
    initial_line_number: usize,
    dump_ast: bool,
    use_color: bool,
) -> *mut ParsedProgram {
    unsafe {
        abort_on_panic(|| {
            let pt = match program_type {
                0 => ProgramType::Script,
                1 => ProgramType::Module,
                _ => ProgramType::Script,
            };

            let Some(source_slice) = source_from_raw(source, source_len) else {
                return std::ptr::null_mut();
            };

            let mut parser = if pt == ProgramType::Script {
                Parser::new_with_line_offset(source_slice, pt, u32_from_usize(initial_line_number))
            } else {
                Parser::new(source_slice, pt)
            };

            let program = parser.parse_program(false);

            // Collect errors from both parser and scope collector.
            let mut errors = parser.take_errors();
            if errors.is_empty() {
                errors = parser.scope_collector.drain_errors();
            }

            if errors.is_empty() {
                parser.scope_collector.analyze(
                    false,
                    &mut parser.arena.identifiers,
                    &parser.arena.strings,
                    &mut parser.arena.scopes,
                );
            }

            // Dump AST if requested (after scope analysis).
            if dump_ast && errors.is_empty() {
                ast_dump::dump_program(&program, use_color, &parser.function_table, &parser.arena);
            }

            let (scope_ref, is_strict, has_tla) = if errors.is_empty()
                && let StatementKind::Program(ref data) = program.inner
            {
                (data.scope, data.is_strict_mode, data.has_top_level_await)
            } else {
                (parser.arena.scopes.insert(ast::ScopeData::default()), false, false)
            };

            let parsed = ParsedProgram {
                program,
                function_table: std::mem::take(&mut parser.function_table),
                arena: std::sync::Arc::new(std::mem::take(&mut parser.arena)),
                scope_ref,
                program_type: pt,
                is_strict_mode: is_strict,
                has_top_level_await: has_tla,
                errors,
                ast_dump: None,
            };

            Box::into_raw(Box::new(parsed))
        })
    }
}

/// Check whether a ParsedProgram has parse errors.
///
/// # Safety
/// `parsed` must be a valid pointer from `rust_parse_program()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parsed_program_has_errors(parsed: *const ParsedProgram) -> bool {
    unsafe { !(*parsed).errors.is_empty() }
}

/// Report parse errors from a ParsedProgram via callback, then clear them.
///
/// Calls `error_callback` for each error with the same signature as
/// `ParseErrorCallback`.
///
/// # Safety
/// - `parsed` must be a valid pointer from `rust_parse_program()`.
/// - `error_callback` must be a valid function pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parsed_program_take_errors(
    parsed: *mut ParsedProgram,
    error_context: *mut c_void,
    error_callback: ParseErrorCallback,
) {
    unsafe {
        let parsed = &mut *parsed;
        for err in parsed.errors.drain(..) {
            let msg = err.message.as_bytes();
            error_callback.unwrap()(error_context, msg.as_ptr(), msg.len(), err.line, err.column);
        }
    }
}

/// Free a ParsedProgram without compiling it.
///
/// # Safety
/// `parsed` must be a valid pointer from `rust_parse_program()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_free_parsed_program(parsed: *mut ParsedProgram) {
    unsafe {
        drop(Box::from_raw(parsed));
    }
}

fn compile_parsed_program_off_thread_impl(
    parsed: *mut ParsedProgram,
    source_len: usize,
    function_precompile_mode: FunctionPrecompileMode,
) -> *mut CompiledProgram {
    unsafe {
        abort_on_panic(|| {
            if parsed.is_null() {
                return std::ptr::null_mut();
            }

            let mut parsed = Box::from_raw(parsed);
            let arena_arc = parsed.arena.clone();
            let (bytecode, declaration_functions) = if parsed.has_top_level_await {
                let mut generator = new_module_async_generator(source_len, std::mem::take(&mut parsed.function_table));
                generator.arena = arena_arc;
                generator.eager_compile_direct_iifes = true;
                let assembled = compile_module_as_async_to_bytecode(&parsed.program, parsed.scope_ref, &mut generator);
                let declaration_functions = precompile_declaration_functions(
                    parsed.program_type,
                    parsed.scope_ref,
                    &mut generator,
                    function_precompile_mode,
                );
                precompile_functions(&mut generator, function_precompile_mode);
                (
                    CompiledProgramBytecode::AsyncModule(CompiledBytecode { generator, assembled }),
                    declaration_functions,
                )
            } else {
                let mut generator = new_program_generator(
                    parsed.is_strict_mode,
                    std::ptr::null_mut(),
                    std::ptr::null(),
                    source_len,
                );
                generator.arena = arena_arc;
                generator.eager_compile_direct_iifes = true;
                generator.function_table = std::mem::take(&mut parsed.function_table);
                let assembled = compile_program_body_to_bytecode(&mut generator, &parsed.program, parsed.scope_ref);
                let declaration_functions = precompile_declaration_functions(
                    parsed.program_type,
                    parsed.scope_ref,
                    &mut generator,
                    function_precompile_mode,
                );
                precompile_functions(&mut generator, function_precompile_mode);
                (
                    CompiledProgramBytecode::Program(CompiledBytecode { generator, assembled }),
                    declaration_functions,
                )
            };

            Box::into_raw(Box::new(CompiledProgram {
                parsed: *parsed,
                bytecode,
                declaration_functions,
            }))
        })
    }
}

/// Compile a parsed program to an off-thread bytecode artifact.
///
/// Consumes and frees the ParsedProgram. The returned CompiledProgram still needs to be materialized on the main thread
/// before it becomes a GC-backed Executable.
///
/// # Safety
/// - `parsed` must be a valid pointer from `rust_parse_program()` with no errors.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compile_parsed_program_off_thread(
    parsed: *mut ParsedProgram,
    source_len: usize,
) -> *mut CompiledProgram {
    compile_parsed_program_off_thread_impl(parsed, source_len, FunctionPrecompileMode::EagerOnly)
}

/// Fully compile a parsed program to an off-thread bytecode artifact for persistence.
///
/// This is intended for post-handoff cache generation, not the latency-sensitive
/// path that produces bytecode for immediate execution.
///
/// # Safety
/// - `parsed` must be a valid pointer from `rust_parse_program()` with no errors.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compile_parsed_program_fully_off_thread(
    parsed: *mut ParsedProgram,
    source_len: usize,
) -> *mut CompiledProgram {
    compile_parsed_program_off_thread_impl(parsed, source_len, FunctionPrecompileMode::All)
}

/// Free a CompiledProgram without materializing it.
///
/// # Safety
/// `compiled` must be a valid pointer from `rust_compile_parsed_program_off_thread()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_free_compiled_program(compiled: *mut CompiledProgram) {
    unsafe {
        drop(Box::from_raw(compiled));
    }
}

/// Serialize a fully compiled program into a versioned bytecode cache blob.
///
/// The caller owns the returned bytes and must release them with
/// `rust_free_bytecode_cache_blob()`.
///
/// # Safety
/// `compiled` must be a valid pointer from `rust_compile_parsed_program_fully_off_thread()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_serialize_compiled_program_for_bytecode_cache(
    compiled: *const CompiledProgram,
    program_type: u8,
    source_hash: *const u8,
    source_hash_len: usize,
) -> BytecodeCacheBlob {
    unsafe {
        abort_on_panic(|| {
            if compiled.is_null() || source_hash.is_null() || source_hash_len != 32 {
                return BytecodeCacheBlob {
                    data: std::ptr::null_mut(),
                    length: 0,
                };
            }

            let program_type = match program_type {
                0 => ast::ProgramType::Script,
                1 => ast::ProgramType::Module,
                _ => {
                    return BytecodeCacheBlob {
                        data: std::ptr::null_mut(),
                        length: 0,
                    };
                }
            };

            let source_hash = std::slice::from_raw_parts(source_hash, source_hash_len)
                .try_into()
                .expect("source hash length was checked");
            let bytes = bytecode_cache::serialize_compiled_program(&*compiled, program_type, source_hash);
            let length = bytes.len();
            let mut bytes = bytes.into_boxed_slice();
            let data = bytes.as_mut_ptr();
            std::mem::forget(bytes);
            BytecodeCacheBlob { data, length }
        })
    }
}

/// Free a bytecode cache blob returned by `rust_serialize_compiled_program_for_bytecode_cache()`.
///
/// # Safety
/// `data` and `length` must match a blob returned by Rust.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_free_bytecode_cache_blob(data: *mut u8, length: usize) {
    unsafe {
        abort_on_panic(|| {
            if !data.is_null() {
                drop(Vec::from_raw_parts(data, length, length));
            }
        });
    }
}

/// Decode a bytecode cache blob into an owned parser-free cache handle.
///
/// # Safety
/// `data` must point to `length` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_decode_bytecode_cache_blob(
    data: *const u8,
    length: usize,
    expected_program_type: u8,
    expected_source_hash: *const u8,
    expected_source_hash_len: usize,
) -> *mut DecodedBytecodeCacheBlob {
    unsafe {
        abort_on_panic(|| {
            if data.is_null() || expected_source_hash.is_null() || expected_source_hash_len != 32 {
                return std::ptr::null_mut();
            }
            let expected_program_type = match expected_program_type {
                0 => ast::ProgramType::Script,
                1 => ast::ProgramType::Module,
                _ => return std::ptr::null_mut(),
            };
            let expected_source_hash = std::slice::from_raw_parts(expected_source_hash, expected_source_hash_len)
                .try_into()
                .expect("source hash length was checked");
            let Some(blob) = bytecode_cache::decode_blob(
                std::slice::from_raw_parts(data, length),
                expected_program_type,
                expected_source_hash,
            ) else {
                return std::ptr::null_mut();
            };
            Box::into_raw(Box::new(DecodedBytecodeCacheBlob { _blob: blob }))
        })
    }
}

/// Free a decoded bytecode cache blob.
///
/// # Safety
/// `blob` must be a valid pointer from `rust_decode_bytecode_cache_blob()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_free_decoded_bytecode_cache_blob(blob: *mut DecodedBytecodeCacheBlob) {
    unsafe {
        drop(Box::from_raw(blob));
    }
}

/// Get the AST dump string from a ParsedProgram.
///
/// Generates the dump on first call and caches it. Writes the pointer
/// and length to the provided out-parameters. The string is owned by
/// the ParsedProgram and freed when it is freed or compiled.
///
/// # Safety
/// - `parsed` must be a valid pointer from `rust_parse_program()` with no errors.
/// - `output_ptr` and `output_len` must be valid writable pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parsed_program_ast_dump(
    parsed: *mut ParsedProgram,
    output_ptr: *mut *const u8,
    output_len: *mut usize,
) {
    unsafe {
        let parsed = &mut *parsed;
        let dump = parsed.ast_dump.get_or_insert_with(|| {
            ast_dump::dump_program_to_string(&parsed.program, &parsed.function_table, &parsed.arena).into_bytes()
        });
        *output_ptr = dump.as_ptr();
        *output_len = dump.len();
    }
}

/// Compile a previously parsed script. Consumes and frees the ParsedProgram.
///
/// Performs codegen and GDI extraction. Requires VM and GC access.
///
/// Returns the `Executable*` as `void*`, or nullptr on failure.
///
/// # Safety
/// - `parsed` must be a valid pointer from `rust_parse_program()` with no errors.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
/// - `gdi_context` must be a valid pointer to a C++ ScriptGdiBuilder.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compile_parsed_script(
    parsed: *mut ParsedProgram,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    gdi_context: *mut c_void,
    source_len: usize,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            let mut parsed = Box::from_raw(parsed);

            let mut generator = new_program_generator(parsed.is_strict_mode, vm_ptr, source_code_ptr, source_len);
            generator.function_table = std::mem::take(&mut parsed.function_table);
            generator.arena = parsed.arena.clone();
            let exec_ptr = compile_program_body(
                &mut generator,
                &parsed.program,
                parsed.scope_ref,
                vm_ptr,
                source_code_ptr,
            );
            if exec_ptr.is_null() {
                return std::ptr::null_mut();
            }

            extract_script_gdi(
                &parsed.arena.scopes[parsed.scope_ref],
                parsed.is_strict_mode,
                vm_ptr,
                source_code_ptr,
                gdi_context,
                &mut generator.function_table,
                &parsed.arena,
            );

            exec_ptr
        })
    }
}

/// Materialize an off-thread-compiled script. Consumes and frees the CompiledProgram.
///
/// # Safety
/// - `compiled` must be a valid pointer from `rust_compile_parsed_program_off_thread()`.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
/// - `gdi_context` must be a valid pointer to a C++ ScriptGdiBuilder.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_materialize_compiled_script(
    compiled: *mut CompiledProgram,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    gdi_context: *mut c_void,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            if compiled.is_null() {
                return std::ptr::null_mut();
            }

            let mut compiled = Box::from_raw(compiled);
            let CompiledProgramBytecode::Program(ref mut bytecode) = compiled.bytecode else {
                return std::ptr::null_mut();
            };

            let exec_ptr = create_executable_from_compiled_bytecode(bytecode, vm_ptr, source_code_ptr);
            if exec_ptr.is_null() {
                return std::ptr::null_mut();
            }

            extract_script_gdi(
                &compiled.parsed.arena.scopes[compiled.parsed.scope_ref],
                compiled.parsed.is_strict_mode,
                vm_ptr,
                source_code_ptr,
                gdi_context,
                &mut bytecode.generator.function_table,
                &compiled.parsed.arena,
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
    error_callback: ParseErrorCallback,
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

            parser.scope_collector.analyze(
                true,
                &mut parser.arena.identifiers,
                &parser.arena.strings,
                &mut parser.arena.scopes,
            );

            write_ast_dump_output(
                &program,
                &parser.function_table,
                &parser.arena,
                ast_dump_output,
                ast_dump_output_len,
            );

            let (scope_id, is_strict) = if let StatementKind::Program(ref data) = program.inner {
                (data.scope, data.is_strict_mode)
            } else {
                return std::ptr::null_mut();
            };

            let arena_arc = std::sync::Arc::new(std::mem::take(&mut parser.arena));
            let mut generator = new_program_generator(is_strict, vm_ptr, source_code_ptr, source_len);
            generator.function_table = std::mem::take(&mut parser.function_table);
            generator.arena = arena_arc.clone();
            let exec_ptr = compile_program_body(&mut generator, &program, scope_id, vm_ptr, source_code_ptr);
            if exec_ptr.is_null() {
                return std::ptr::null_mut();
            }

            extract_eval_gdi(
                &arena_arc.scopes[scope_id],
                is_strict,
                vm_ptr,
                source_code_ptr,
                gdi_context,
                &mut generator.function_table,
                &arena_arc,
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
    error_callback: ParseErrorCallback,
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
            let Some(parameters_slice) = source_from_raw(parameters_source, parameters_source_len) else {
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
                        let msg = token
                            .message
                            .unwrap_or_else(|| format!("Unexpected token {}", token.token_type.name()));
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
                        validate_src.extend_from_slice(utf16!("function* test("));
                    }
                    ast::FunctionKind::Async => {
                        validate_src.extend_from_slice(utf16!("async function test("));
                    }
                    ast::FunctionKind::AsyncGenerator => {
                        validate_src.extend_from_slice(utf16!("async function* test("));
                    }
                    ast::FunctionKind::Normal => {
                        validate_src.extend_from_slice(utf16!("function test("));
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
                parser.flags.new_target_is_valid = true;
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
            parser.scope_collector.analyze_as_dynamic_function(
                &mut parser.arena.identifiers,
                &parser.arena.strings,
                &mut parser.arena.scopes,
            );

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
                &parser.arena,
                ast_dump_output,
                ast_dump_output_len,
            );

            // Extract the FunctionExpression from the program.
            // The program should contain a single ExpressionStatement wrapping a FunctionExpression.
            let function_id = if let StatementKind::Program(ref data) = program.inner {
                let scope = &parser.arena.scopes[data.scope];
                scope.children.iter().find_map(|child| match &child.inner {
                    StatementKind::FunctionDeclaration(fd) => Some(fd.function_id),
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
            let subtable = parser
                .function_table
                .extract_reachable(&function_data, &parser.arena.scopes);
            let arena = std::sync::Arc::new(std::mem::take(&mut parser.arena));

            bytecode::ffi::create_sfd_for_gdi(function_data, subtable, vm_ptr, source_code_ptr, is_strict, arena)
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

            parser.scope_collector.analyze(
                false,
                &mut parser.arena.identifiers,
                &parser.arena.strings,
                &mut parser.arena.scopes,
            );

            write_ast_dump_output(
                &program,
                &parser.function_table,
                &parser.arena,
                ast_dump_output,
                ast_dump_output_len,
            );

            let scope_id = if let StatementKind::Program(ref data) = program.inner {
                data.scope
            } else {
                return;
            };

            let arena = std::sync::Arc::new(std::mem::take(&mut parser.arena));
            let scope = &arena.scopes[scope_id];
            for child in &scope.children {
                if let StatementKind::FunctionDeclaration(ref fd) = child.inner {
                    let function_data = parser.function_table.take(fd.function_id);
                    let subtable = parser.function_table.extract_reachable(&function_data, &arena.scopes);
                    let sfd_ptr = bytecode::ffi::create_sfd_for_gdi(
                        function_data,
                        subtable,
                        vm_ptr,
                        source_code_ptr,
                        true, // strict
                        arena.clone(),
                    );
                    if !sfd_ptr.is_null()
                        && let Some(name_ident) = fd.name
                    {
                        let name = arena.name_of(name_ident);
                        push_function(ctx, sfd_ptr, name.as_ptr(), name.len());
                    }
                }
            }
        });
    }
}

// =============================================================================
// Module compilation
// =============================================================================

/// Compile a previously parsed module. Consumes and frees the ParsedProgram.
///
/// Extracts import/export metadata, compiles the module body to bytecode,
/// and extracts declaration data needed for initialize_environment().
///
/// Returns `Executable*` for non-TLA modules (tla_executable_out is null),
/// or nullptr for TLA modules (tla_executable_out is set to the async wrapper).
///
/// # Safety
/// - `parsed` must be a valid pointer from `rust_parse_program()` with no errors.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
/// - `module_context` must be a valid `ModuleBuilder*`.
/// - `callbacks` must point to a valid `ModuleCallbacks`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compile_parsed_module(
    parsed: *mut ParsedProgram,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    module_context: *mut c_void,
    callbacks: *const ModuleCallbacks,
    tla_executable_out: *mut *mut c_void,
    source_len: usize,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            let mut parsed = Box::from_raw(parsed);
            let cb = &*callbacks;

            // 1. Report has_top_level_await.
            (cb.set_has_top_level_await)(module_context, parsed.has_top_level_await);

            // 2. Process imports and exports.
            extract_module_metadata(&parsed.arena.scopes[parsed.scope_ref], module_context, cb);

            // 3. Extract var declared names and lexical bindings.
            extract_module_declarations(
                &parsed.arena.scopes[parsed.scope_ref],
                vm_ptr,
                source_code_ptr,
                module_context,
                cb,
                &mut parsed.function_table,
                &parsed.arena,
            );

            // 4. Compute requested modules (sorted by source offset).
            extract_requested_modules(&parsed.arena.scopes[parsed.scope_ref], module_context, cb);

            // 5. Compile module body.
            if parsed.has_top_level_await {
                let exec_ptr = compile_module_as_async(
                    &parsed.program,
                    parsed.scope_ref,
                    parsed.arena.clone(),
                    vm_ptr,
                    source_code_ptr,
                    source_len,
                    std::mem::take(&mut parsed.function_table),
                );
                if !tla_executable_out.is_null() {
                    *tla_executable_out = exec_ptr;
                }
                std::ptr::null_mut()
            } else {
                if !tla_executable_out.is_null() {
                    *tla_executable_out = std::ptr::null_mut();
                }
                let mut generator = new_program_generator(true, vm_ptr, source_code_ptr, source_len);
                generator.function_table = std::mem::take(&mut parsed.function_table);
                generator.arena = parsed.arena.clone();
                compile_program_body(
                    &mut generator,
                    &parsed.program,
                    parsed.scope_ref,
                    vm_ptr,
                    source_code_ptr,
                )
            }
        })
    }
}

/// Materialize an off-thread-compiled module. Consumes and frees the CompiledProgram.
///
/// # Safety
/// - `compiled` must be a valid pointer from `rust_compile_parsed_program_off_thread()`.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
/// - `module_context` must be a valid `ModuleBuilder*`.
/// - `callbacks` must point to a valid `ModuleCallbacks`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_materialize_compiled_module(
    compiled: *mut CompiledProgram,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    module_context: *mut c_void,
    callbacks: *const ModuleCallbacks,
    tla_executable_out: *mut *mut c_void,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            if compiled.is_null() {
                return std::ptr::null_mut();
            }

            let mut compiled = Box::from_raw(compiled);
            let cb = &*callbacks;

            (cb.set_has_top_level_await)(module_context, compiled.parsed.has_top_level_await);
            extract_module_metadata(
                &compiled.parsed.arena.scopes[compiled.parsed.scope_ref],
                module_context,
                cb,
            );

            let bytecode = match &mut compiled.bytecode {
                CompiledProgramBytecode::Program(bytecode) | CompiledProgramBytecode::AsyncModule(bytecode) => bytecode,
            };
            extract_module_declarations(
                &compiled.parsed.arena.scopes[compiled.parsed.scope_ref],
                vm_ptr,
                source_code_ptr,
                module_context,
                cb,
                &mut bytecode.generator.function_table,
                &compiled.parsed.arena,
            );
            extract_requested_modules(
                &compiled.parsed.arena.scopes[compiled.parsed.scope_ref],
                module_context,
                cb,
            );

            match &mut compiled.bytecode {
                CompiledProgramBytecode::AsyncModule(bytecode) => {
                    let exec_ptr = create_executable_from_compiled_bytecode(bytecode, vm_ptr, source_code_ptr);
                    if !tla_executable_out.is_null() {
                        *tla_executable_out = exec_ptr;
                    }
                    std::ptr::null_mut()
                }
                CompiledProgramBytecode::Program(bytecode) => {
                    if !tla_executable_out.is_null() {
                        *tla_executable_out = std::ptr::null_mut();
                    }
                    create_executable_from_compiled_bytecode(bytecode, vm_ptr, source_code_ptr)
                }
            }
        })
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
type ModuleLexicalBindingCallback =
    unsafe extern "C" fn(ctx: *mut c_void, name: *const u16, name_len: usize, is_constant: bool, function_index: i32);

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
) -> (Vec<bytecode::ffi::FFIUtf16Slice>, Vec<bytecode::ffi::FFIUtf16Slice>) {
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
    export_name: Option<&ast::Utf16String>,
    local_or_import_name: Option<&ast::Utf16String>,
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
/// This is the combined parse+compile path for modules. Internally calls
/// `rust_parse_program()` + `rust_compile_parsed_module()`.
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
    error_callback: ParseErrorCallback,
    tla_executable_out: *mut *mut c_void,
    ast_dump_output: *mut *mut u8,
    ast_dump_output_len: *mut usize,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            let parsed = rust_parse_program(source, source_len, 1, 0, dump_ast, use_color);

            if parsed.is_null() {
                return std::ptr::null_mut();
            }

            if rust_parsed_program_has_errors(parsed) {
                if let Some(cb) = error_callback {
                    rust_parsed_program_take_errors(parsed, error_context, Some(cb));
                }
                rust_free_parsed_program(parsed);
                return std::ptr::null_mut();
            }

            let parsed_ref = &*parsed;
            write_ast_dump_output(
                &parsed_ref.program,
                &parsed_ref.function_table,
                &parsed_ref.arena,
                ast_dump_output,
                ast_dump_output_len,
            );

            rust_compile_parsed_module(
                parsed,
                vm_ptr,
                source_code_ptr,
                module_context,
                callbacks,
                tla_executable_out,
                source_len,
            )
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
                        .map_or((std::ptr::null(), 0, true), |n| (n.as_ptr(), n.len(), false));
                    let (keys, values) = build_attribute_slices(&import_data.module_request.attributes);
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
            let StatementKind::Export(ref export_data) = child.inner else {
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
                        StatementKind::FunctionDeclaration(_) | StatementKind::ClassDeclaration(_)
                    )
                });
                if !is_declaration && let Some(ref name) = entry.local_or_import_name {
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
                                entry.export_name.as_ref(),
                                entry.local_or_import_name.as_ref(),
                                None,
                            );
                        } else {
                            // Re-export of a specific binding → indirect export.
                            call_export_callback(
                                cb.push_indirect_export,
                                ctx,
                                ExportEntryKind::NamedExport as u8,
                                entry.export_name.as_ref(),
                                import_entry.import_name.as_ref(),
                                Some(&import_entry.module_request),
                            );
                        }
                    } else {
                        // Direct local export.
                        call_export_callback(
                            cb.push_local_export,
                            ctx,
                            entry.kind as u8,
                            entry.export_name.as_ref(),
                            entry.local_or_import_name.as_ref(),
                            None,
                        );
                    }
                } else if entry.kind == ExportEntryKind::ModuleRequestAllButDefault {
                    // export * from "module"
                    call_export_callback(
                        cb.push_star_export,
                        ctx,
                        entry.kind as u8,
                        entry.export_name.as_ref(),
                        entry.local_or_import_name.as_ref(),
                        export_data.module_request.as_ref(),
                    );
                } else {
                    // export { x } from "module" or export { x as y } from "module"
                    call_export_callback(
                        cb.push_indirect_export,
                        ctx,
                        entry.kind as u8,
                        entry.export_name.as_ref(),
                        entry.local_or_import_name.as_ref(),
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
    arena: &std::sync::Arc<ast::AstArena>,
) {
    unsafe {
        use ast::StatementKind;

        let default_name: ast::Utf16String = utf16!("*default*").into();

        // Var declared names (walk all nesting levels).
        for child in &scope.children {
            collect_module_var_names(&child.inner, ctx, cb.push_var_name, arena);
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
                StatementKind::FunctionDeclaration(fd) => {
                    let is_default = is_exported
                        && fd
                            .name
                            .is_some_and(|n| arena.name_of(n).as_slice() == default_name.as_slice());

                    let function_data = function_table.take(fd.function_id);
                    let subtable = function_table.extract_reachable(&function_data, &arena.scopes);
                    let sfd_ptr = bytecode::ffi::create_sfd_for_gdi(
                        function_data,
                        subtable,
                        vm_ptr,
                        source_code_ptr,
                        true,
                        arena.clone(),
                    );
                    if sfd_ptr.is_null() {
                        continue;
                    }

                    // Get the binding name from the AST (e.g., "*default*" for anonymous defaults).
                    let binding_name = if let Some(name_ident) = fd.name {
                        arena.name_of(name_ident).clone()
                    } else {
                        continue;
                    };

                    // If default export with *default* name, set the SFD display name to "default".
                    let sfd_name = if is_default {
                        let sfd_display_name = utf16!("default");
                        module_sfd_set_name(sfd_ptr, sfd_display_name.as_ptr(), sfd_display_name.len());
                        let display_name: ast::Utf16String = sfd_display_name.into();
                        display_name
                    } else {
                        binding_name.clone()
                    };

                    let function_index = function_count;
                    (cb.push_function)(ctx, sfd_ptr, sfd_name.as_ptr(), sfd_name.len());
                    function_count += 1;

                    // Lexical binding uses the AST name (e.g., "*default*").
                    (cb.push_lexical_binding)(ctx, binding_name.as_ptr(), binding_name.len(), false, function_index);
                }
                StatementKind::ClassDeclaration(class_data) => {
                    if let Some(name_ident) = class_data.name {
                        let name = arena.name_of(name_ident);
                        (cb.push_lexical_binding)(ctx, name.as_ptr(), name.len(), false, -1);
                    }
                }
                StatementKind::VariableDeclaration(vd) if vd.kind != ast::DeclarationKind::Var => {
                    let is_constant = vd.kind == ast::DeclarationKind::Const;
                    for declaration in &vd.declarations {
                        for_each_bound_name(&declaration.target, arena, &mut |name| {
                            (cb.push_lexical_binding)(ctx, name.as_ptr(), name.len(), is_constant, -1);
                        });
                    }
                }
                StatementKind::UsingDeclaration(declarations) => {
                    for declaration in declarations.iter() {
                        for_each_bound_name(&declaration.target, arena, &mut |name| {
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
    arena: &ast::AstArena,
) {
    unsafe {
        match statement {
            ast::StatementKind::VariableDeclaration(vd) if vd.kind == ast::DeclarationKind::Var => {
                for declaration in &vd.declarations {
                    for_each_bound_name(&declaration.target, arena, &mut |name| {
                        push_var_name(ctx, name.as_ptr(), name.len());
                    });
                }
            }
            ast::StatementKind::Export(export_data) => {
                if let Some(ref stmt) = export_data.statement {
                    collect_module_var_names(&stmt.inner, ctx, push_var_name, arena);
                }
            }
            _ => {
                for_each_child_statement(statement, arena, &mut |child| {
                    collect_module_var_names(child, ctx, push_var_name, arena);
                });
            }
        }
    }
}

/// Extract requested modules sorted by source offset.
unsafe fn extract_requested_modules(scope: &ast::ScopeData, ctx: *mut c_void, cb: &ModuleCallbacks) {
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
fn new_module_async_generator(source_len: usize, function_table: ast::FunctionTable) -> bytecode::generator::Generator {
    let mut generator = bytecode::generator::Generator::new();
    generator.strict = true;
    generator.function_table = function_table;
    generator.source_len = source_len;
    generator.enclosing_function_kind = ast::FunctionKind::Async;
    generator
}

fn compile_module_as_async_to_bytecode(
    program: &ast::Statement,
    scope_id: ast::ScopeId,
    generator: &mut bytecode::generator::Generator,
) -> bytecode::generator::AssembledBytecode {
    use bytecode::instruction::Instruction;

    let arena_clone = generator.arena.clone();
    let scope = &arena_clone.scopes[scope_id];

    // Extract local variables from the program scope so the executable has the correct registers_and_locals_count.
    // Without this, locals are not saved across await suspension points, causing them to become undefined.
    generator.local_variables = convert_local_variables(scope);

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
    generator.capture_saved_lexical_environment();

    // Generate module body statements.
    let _result = bytecode::codegen::generate_statement(program, generator, None);

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

    generator.assemble()
}

unsafe fn compile_module_as_async(
    program: &ast::Statement,
    scope_id: ast::ScopeId,
    arena: std::sync::Arc<ast::AstArena>,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    source_len: usize,
    function_table: ast::FunctionTable,
) -> *mut c_void {
    unsafe {
        let mut generator = new_module_async_generator(source_len, function_table);
        generator.arena = arena;
        generator.vm_ptr = vm_ptr;
        generator.source_code_ptr = source_code_ptr;

        let assembled = compile_module_as_async_to_bytecode(program, scope_id, &mut generator);
        bytecode::ffi::create_executable(&mut generator, &assembled, vm_ptr, source_code_ptr)
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
fn collect_var_names_recursive(
    statement: &ast::StatementKind,
    arena: &ast::AstArena,
    push_name: &mut dyn FnMut(&[u16]),
) {
    match statement {
        ast::StatementKind::VariableDeclaration(vd) if vd.kind == ast::DeclarationKind::Var => {
            for declaration in &vd.declarations {
                for_each_bound_name(&declaration.target, arena, push_name);
            }
        }
        _ => {
            for_each_child_statement(statement, arena, &mut |child| {
                collect_var_names_recursive(child, arena, push_name);
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
    arena: &std::sync::Arc<ast::AstArena>,
) {
    use ast::{DeclarationKind, StatementKind};

    // Var names (var declarations at any nesting level + top-level function declarations)
    for child in &scope.children {
        collect_var_names_recursive(&child.inner, arena, push_var_name);
        if let StatementKind::FunctionDeclaration(ref fd) = child.inner
            && let Some(name_ident) = fd.name
        {
            push_var_name(arena.name_slice(name_ident));
        }
    }

    // Functions to initialize: keep the last declaration with each name
    // (ECMAScript hoisting semantics), but emit them in source order. Two
    // forward passes; StringId keys keep the inserts to a u32 compare.
    let mut last_position: std::collections::HashMap<ast::StringId, usize> = std::collections::HashMap::new();
    for (i, child) in scope.children.iter().enumerate() {
        if let StatementKind::FunctionDeclaration(ref fd) = child.inner
            && let Some(name_ident) = fd.name
        {
            last_position.insert(arena.identifiers[name_ident].name, i);
        }
    }
    for (i, child) in scope.children.iter().enumerate() {
        if let StatementKind::FunctionDeclaration(ref fd) = child.inner
            && let Some(name_ident) = fd.name
            && last_position.get(&arena.identifiers[name_ident].name).copied() == Some(i)
        {
            let function_data = function_table.take(fd.function_id);
            let subtable = function_table.extract_reachable(&function_data, &arena.scopes);
            let sfd_ptr = unsafe {
                bytecode::ffi::create_sfd_for_gdi(
                    function_data,
                    subtable,
                    vm_ptr,
                    source_code_ptr,
                    is_strict,
                    arena.clone(),
                )
            };
            assert!(!sfd_ptr.is_null(), "create_sfd_for_gdi returned null");
            push_function(sfd_ptr, arena.name_slice(name_ident));
        }
    }

    // Var-scoped names (var VariableDeclaration names, excluding function declarations)
    for child in &scope.children {
        collect_var_names_recursive(&child.inner, arena, push_var_scoped_name);
    }

    for name in &scope.annexb_function_names {
        push_annex_b_name(name);
    }

    for child in &scope.children {
        match &child.inner {
            StatementKind::VariableDeclaration(vd) if vd.kind != DeclarationKind::Var => {
                let is_constant = vd.kind == DeclarationKind::Const;
                for declaration in &vd.declarations {
                    for_each_bound_name(&declaration.target, arena, &mut |name| {
                        push_lexical_binding(name, is_constant);
                    });
                }
            }
            StatementKind::UsingDeclaration(declarations) => {
                for declaration in declarations.iter() {
                    for_each_bound_name(&declaration.target, arena, &mut |name| {
                        push_lexical_binding(name, false);
                    });
                }
            }
            StatementKind::ClassDeclaration(class_data) => {
                if let Some(name) = class_data.name {
                    push_lexical_binding(arena.name_slice(name), false);
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
    arena: &std::sync::Arc<ast::AstArena>,
) {
    unsafe {
        use bytecode::ffi::{
            eval_gdi_push_annex_b_name, eval_gdi_push_function, eval_gdi_push_lexical_binding, eval_gdi_push_var_name,
            eval_gdi_push_var_scoped_name, eval_gdi_set_strict,
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
                eval_gdi_push_lexical_binding(ctx, name.as_ptr(), name.len(), is_const);
            },
            function_table,
            arena,
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
    arena: &std::sync::Arc<ast::AstArena>,
) {
    unsafe {
        use ast::{DeclarationKind, StatementKind};
        use bytecode::ffi::{
            script_gdi_push_annex_b_name, script_gdi_push_function, script_gdi_push_lexical_binding,
            script_gdi_push_lexical_name, script_gdi_push_var_name, script_gdi_push_var_scoped_name,
        };

        // Lexical names (let/const/using/class at top level) — script-only step.
        for child in &scope.children {
            match &child.inner {
                StatementKind::VariableDeclaration(vd) if vd.kind != DeclarationKind::Var => {
                    for declaration in &vd.declarations {
                        for_each_bound_name(&declaration.target, arena, &mut |name| {
                            script_gdi_push_lexical_name(ctx, name.as_ptr(), name.len());
                        });
                    }
                }
                StatementKind::UsingDeclaration(declarations) => {
                    for declaration in declarations.iter() {
                        for_each_bound_name(&declaration.target, arena, &mut |name| {
                            script_gdi_push_lexical_name(ctx, name.as_ptr(), name.len());
                        });
                    }
                }
                StatementKind::ClassDeclaration(class_data) => {
                    if let Some(name) = class_data.name {
                        let n = arena.name_of(name);
                        script_gdi_push_lexical_name(ctx, n.as_ptr(), n.len());
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
                script_gdi_push_lexical_binding(ctx, name.as_ptr(), name.len(), is_const);
            },
            function_table,
            arena,
        );
    }
}

/// Visit each child statement of a statement, excluding function/class bodies
/// (which create new var scopes). This enables recursive var-declaration walking.
fn for_each_child_statement(
    statement: &ast::StatementKind,
    arena: &ast::AstArena,
    f: &mut dyn FnMut(&ast::StatementKind),
) {
    use ast::StatementKind;

    match statement {
        StatementKind::Block(scope) => {
            for child in &arena.scopes[*scope].children {
                f(&child.inner);
            }
        }
        StatementKind::If(data) => {
            f(&data.consequent.inner);
            if let Some(alt) = &data.alternate {
                f(&alt.inner);
            }
        }
        StatementKind::While(data) => {
            f(&data.body.inner);
        }
        StatementKind::DoWhile(data) => {
            f(&data.body.inner);
        }
        StatementKind::With(data) => {
            f(&data.body.inner);
        }
        StatementKind::For(data) => {
            if let Some(ast::ForInit::Declaration(decl)) = &data.init {
                f(&decl.inner);
            }
            f(&data.body.inner);
        }
        StatementKind::ForInOf(data) => {
            if let ast::ForInOfLhs::Declaration(declaration) = &data.lhs {
                f(&declaration.inner);
            }
            f(&data.body.inner);
        }
        StatementKind::Switch(data) => {
            for case in &data.cases {
                for child in &arena.scopes[case.scope].children {
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
        StatementKind::Labelled(data) => {
            f(&data.item.inner);
        }
        // Don't recurse into function/class bodies (new var scopes)
        _ => {}
    }
}

fn for_each_bound_name(target: &ast::VariableDeclaratorTarget, arena: &ast::AstArena, f: &mut dyn FnMut(&[u16])) {
    match target {
        ast::VariableDeclaratorTarget::Identifier(id) => f(arena.name_slice(*id)),
        ast::VariableDeclaratorTarget::BindingPattern(pattern) => {
            for_each_bound_name_in_pattern(pattern, arena, f);
        }
    }
}

fn for_each_bound_name_in_pattern(pattern: &ast::BindingPattern, arena: &ast::AstArena, f: &mut dyn FnMut(&[u16])) {
    for entry in &pattern.entries {
        match &entry.alias {
            None => {
                if let Some(ast::BindingEntryName::Identifier(id)) = &entry.name {
                    f(arena.name_slice(*id));
                }
            }
            Some(ast::BindingEntryAlias::Identifier(id)) => f(arena.name_slice(*id)),
            Some(ast::BindingEntryAlias::BindingPattern(inner)) => {
                for_each_bound_name_in_pattern(inner, arena, f);
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

/// Clone a lazy function compilation payload.
///
/// The clone lets background compilation race with synchronous lazy
/// compilation. Each path owns and eventually frees its own AST payload.
///
/// # Safety
/// `ast` must be null or a valid pointer returned by `Box::into_raw(Box<FunctionPayload>)`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_clone_function_ast(ast: *const c_void) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            if ast.is_null() {
                return std::ptr::null_mut();
            }
            let payload = &*(ast as *const ast::FunctionPayload);
            Box::into_raw(Box::new(payload.clone())) as *mut c_void
        })
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
            let arena = payload.arena.clone();
            let (_function_data, mut precompiled) = compile_function_payload_to_bytecode(
                *payload,
                source_len,
                builtin_abstract_operations_enabled,
                arena,
                FunctionPrecompileMode::EagerOnly,
            );

            precompiled.generator.vm_ptr = vm_ptr;
            precompiled.generator.source_code_ptr = source_code_ptr;

            write_sfd_metadata(sfd_ptr, &precompiled.metadata);

            bytecode::ffi::create_executable(
                &mut precompiled.generator,
                &precompiled.assembled,
                vm_ptr,
                source_code_ptr,
            )
        })
    }
}

/// Compile a function payload to a GC-free bytecode artifact.
///
/// Takes ownership of the cloned `Box<FunctionPayload>`. The result must be
/// materialized on the main thread or freed with `rust_free_compiled_function`.
///
/// # Safety
/// `rust_function_ast` must be a valid `Box<FunctionPayload>` pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compile_function_off_thread(
    rust_function_ast: *mut c_void,
    source_len: usize,
    builtin_abstract_operations_enabled: bool,
) -> *mut CompiledFunction {
    unsafe {
        abort_on_panic(|| {
            if rust_function_ast.is_null() {
                return std::ptr::null_mut();
            }
            let payload = Box::from_raw(rust_function_ast as *mut ast::FunctionPayload);
            let arena = payload.arena.clone();
            let (_function_data, precompiled) = compile_function_payload_to_bytecode(
                *payload,
                source_len,
                builtin_abstract_operations_enabled,
                arena,
                FunctionPrecompileMode::All,
            );
            Box::into_raw(Box::new(CompiledFunction { precompiled }))
        })
    }
}

/// Materialize a GC-free compiled function onto an existing SFD.
///
/// Consumes and frees the compiled function.
///
/// # Safety
/// - `compiled` must be a valid pointer returned by `rust_compile_function_off_thread`.
/// - `vm_ptr`, `source_code_ptr`, and `sfd_ptr` must be valid main-thread pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_materialize_compiled_function(
    compiled: *mut CompiledFunction,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    sfd_ptr: *mut c_void,
) {
    unsafe {
        abort_on_panic(|| {
            if compiled.is_null() {
                return;
            }
            let mut compiled = Box::from_raw(compiled);
            compiled.precompiled.generator.vm_ptr = vm_ptr;
            compiled.precompiled.generator.source_code_ptr = source_code_ptr;
            let executable_ptr = bytecode::ffi::create_executable(
                &mut compiled.precompiled.generator,
                &compiled.precompiled.assembled,
                vm_ptr,
                source_code_ptr,
            );
            assert!(
                !executable_ptr.is_null(),
                "rust_materialize_compiled_function: executable materialization failed"
            );
            bytecode::ffi::rust_sfd_set_precompiled_executable(
                sfd_ptr,
                executable_ptr,
                compiled.precompiled.metadata.uses_this,
                compiled.precompiled.metadata.this_value_needs_environment_resolution,
                compiled.precompiled.metadata.function_environment_needed,
                compiled.precompiled.metadata.function_environment_bindings_count,
                compiled.precompiled.metadata.might_need_arguments,
                compiled.precompiled.metadata.contains_eval,
            );
        });
    }
}

/// Free a GC-free compiled function without materializing it.
///
/// # Safety
/// `compiled` must be null or a valid pointer returned by `rust_compile_function_off_thread`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_free_compiled_function(compiled: *mut CompiledFunction) {
    unsafe {
        abort_on_panic(|| {
            if !compiled.is_null() {
                drop(Box::from_raw(compiled));
            }
        });
    }
}

fn compile_function_payload_to_bytecode(
    payload: ast::FunctionPayload,
    source_len: usize,
    builtin_abstract_operations_enabled: bool,
    arena: std::sync::Arc<ast::AstArena>,
    precompile_mode: FunctionPrecompileMode,
) -> (Box<ast::FunctionData>, Box<bytecode::generator::PrecompiledFunction>) {
    let function_data = Box::new(payload.data);

    let body_scope: Option<ast::ScopeId> = match &function_data.body.inner {
        StatementKind::FunctionBody { scope, .. } => Some(*scope),
        StatementKind::Block(scope) => Some(*scope),
        _ => None,
    };

    // Compute SFD metadata before codegen so the generator can optimize
    // direct `this` access when it does not need environment resolution.
    let sfd_metadata = compute_sfd_metadata(&function_data, &arena);

    let mut generator = bytecode::generator::Generator::new();
    generator.arena = arena;
    generator.strict = function_data.is_strict_mode;
    generator.this_value_needs_environment_resolution = sfd_metadata.this_value_needs_environment_resolution;
    generator.builtin_abstract_operations_enabled = builtin_abstract_operations_enabled;
    generator.function_table = payload.function_table;
    generator.source_len = source_len;
    generator.enclosing_function_kind = function_data.kind;

    if let Some(scope_id) = body_scope {
        let arena_clone = generator.arena.clone();
        generator.local_variables = convert_local_variables(&arena_clone.scopes[scope_id]);
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

    generator.capture_saved_lexical_environment();

    if let Some(scope_id) = body_scope {
        let arena_clone = generator.arena.clone();
        bytecode::codegen::emit_function_declaration_instantiation(
            &mut generator,
            &function_data,
            &arena_clone.scopes[scope_id],
            sfd_metadata.var_environment_bindings_count,
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

    let result = bytecode::codegen::generate_statement(&function_data.body, &mut generator, None);

    if !generator.is_current_block_terminated() {
        if generator.is_in_generator_or_async_function() {
            // Generator/async functions end with Yield (no continuation = done).
            let undef = generator.add_constant_undefined();
            generator.emit(bytecode::instruction::Instruction::Yield {
                continuation_label: None,
                value: undef.operand(),
            });
        } else if let Some(value) = result {
            generator.emit(bytecode::instruction::Instruction::End { value: value.operand() });
        }
        // If result is None, the assembler will add End(undefined) as a
        // fallthrough for unterminated blocks, matching C++ compile().
    }

    // For generator/async functions, terminate all unterminated blocks with Yield.
    if generator.is_in_generator_or_async_function() {
        generator.terminate_unterminated_blocks_with_yield();
    }

    precompile_functions(&mut generator, precompile_mode);

    let assembled = generator.assemble();

    (
        function_data,
        Box::new(bytecode::generator::PrecompiledFunction {
            generator: Box::new(generator),
            assembled,
            metadata: sfd_metadata,
        }),
    )
}

// =============================================================================
// SFD metadata computation (ECMA-262 section 10.2.11)
// =============================================================================

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
fn compute_sfd_metadata(
    function_data: &ast::FunctionData,
    arena: &ast::AstArena,
) -> bytecode::generator::FunctionSfdMetadata {
    let body_scope: Option<ast::ScopeId> = match &function_data.body.inner {
        ast::StatementKind::FunctionBody { scope, .. } => Some(*scope),
        _ => None,
    };

    let strict = function_data.is_strict_mode;
    let is_arrow = function_data.is_arrow_function;

    // Extract all scope analysis data in one borrow.
    let bsi = if let Some(scope_id) = body_scope {
        let sd = &arena.scopes[scope_id];
        let fsd = sd.function_scope_data.as_ref();
        BodyScopeInfo {
            uses_this: sd.uses_this || function_data.parsing_insights.uses_this,
            contains_eval: sd.contains_direct_call_to_eval,
            uses_this_from_env: sd.uses_this_from_environment
                || function_data.parsing_insights.uses_this_from_environment,
            might_need_arguments: function_data.parsing_insights.might_need_arguments_object,
            has_function_named_arguments: fsd.is_some_and(|f| f.has_function_named_arguments),
            has_lexically_declared_arguments: fsd.is_some_and(|f| f.has_lexically_declared_arguments),
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
            || matches!(
                p.binding,
                ast::FunctionParameterBinding::BindingPattern(ref pat) if pat.contains_expression()
            )
    });

    // §10.2.11 steps 5-8: count non-local unique parameter names.
    let mut parameter_names: HashSet<ast::Utf16String> = HashSet::new();
    let mut parameters_in_environment: usize = 0;
    for parameter in &function_data.parameters {
        match &parameter.binding {
            ast::FunctionParameterBinding::Identifier(id) => {
                let ident = &arena.identifiers[*id];
                if parameter_names.insert(arena.strings[ident.name].clone()) && !ident.is_local() {
                    parameters_in_environment += 1;
                }
            }
            ast::FunctionParameterBinding::BindingPattern(pattern) => {
                for_each_binding_pattern_identifier(pattern, arena, &mut |ident| {
                    if parameter_names.insert(arena.strings[ident.name].clone()) && !ident.is_local() {
                        parameters_in_environment += 1;
                    }
                });
            }
        }
    }

    // §10.2.11 steps 15-18: determine if arguments object is needed.
    // We trust either the parser's conservative "might need arguments" flag
    // (set when we consume an `arguments` or `eval` Identifier as a free
    // reference) OR scope analysis having allocated an ArgumentsObject local
    // for `arguments`. The latter catches references created without going
    // through consume(), e.g. shorthand `{ arguments }` in an object literal.
    // Skip the local-driven path when a function named `arguments` shadows
    // it; in that case the local belongs to the function declaration, not
    // a real arguments-object reference.
    let arguments_object_referenced =
        bsi.might_need_arguments || (bsi.has_arguments_object_local && !bsi.has_function_named_arguments);
    let arguments_object_needed = arguments_object_referenced
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
            let non_local_lex_count = count_non_local_lex_declarations(body_scope, arena);
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

            let non_local_lex_count = count_non_local_lex_declarations(body_scope, arena);
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

    let this_value_needs_environment_resolution = bsi.uses_this_from_env;
    let function_environment_needed = arguments_object_needs_binding
        || function_environment_bindings_count > 0
        || var_environment_bindings_count > 0
        || lex_environment_bindings_count > 0
        || (!is_arrow && bsi.uses_this_from_env)
        || bsi.contains_eval;

    bytecode::generator::FunctionSfdMetadata {
        uses_this: bsi.uses_this,
        this_value_needs_environment_resolution,
        function_environment_needed,
        function_environment_bindings_count,
        var_environment_bindings_count,
        might_need_arguments: bsi.might_need_arguments,
        contains_eval: bsi.contains_eval,
    }
}

/// Write precomputed SFD metadata to a C++ SharedFunctionInstanceData via FFI.
///
/// # Safety
/// `sfd_ptr` must be a valid `JS::SharedFunctionInstanceData*`.
unsafe fn write_sfd_metadata(sfd_ptr: *mut c_void, metadata: &bytecode::generator::FunctionSfdMetadata) {
    unsafe {
        rust_sfd_set_metadata(
            sfd_ptr,
            metadata.uses_this,
            metadata.this_value_needs_environment_resolution,
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
fn count_non_local_lex_declarations(scope_id: ast::ScopeId, arena: &ast::AstArena) -> usize {
    let sd = &arena.scopes[scope_id];
    let mut count = 0;
    for child in &sd.children {
        match &child.inner {
            ast::StatementKind::VariableDeclaration(vd) => {
                use parser::DeclarationKind;
                if vd.kind == DeclarationKind::Let || vd.kind == DeclarationKind::Const {
                    for declaration in &vd.declarations {
                        count_non_local_names_in_target(&declaration.target, &mut count, arena);
                    }
                }
            }
            ast::StatementKind::UsingDeclaration(declarations) => {
                for declaration in declarations.iter() {
                    count_non_local_names_in_target(&declaration.target, &mut count, arena);
                }
            }
            ast::StatementKind::ClassDeclaration(class_data) => {
                if let Some(name_ident) = class_data.name
                    && !arena.identifiers[name_ident].is_local()
                {
                    count += 1;
                }
            }
            _ => {}
        }
    }
    count
}

fn count_non_local_names_in_target(target: &ast::VariableDeclaratorTarget, count: &mut usize, arena: &ast::AstArena) {
    match target {
        ast::VariableDeclaratorTarget::Identifier(id) => {
            if !arena.identifiers[*id].is_local() {
                *count += 1;
            }
        }
        ast::VariableDeclaratorTarget::BindingPattern(pattern) => {
            count_non_local_names_in_binding_pattern(pattern, count, arena);
        }
    }
}

fn count_non_local_names_in_binding_pattern(pattern: &ast::BindingPattern, count: &mut usize, arena: &ast::AstArena) {
    for entry in &pattern.entries {
        match &entry.alias {
            Some(ast::BindingEntryAlias::Identifier(id)) => {
                if !arena.identifiers[*id].is_local() {
                    *count += 1;
                }
            }
            Some(ast::BindingEntryAlias::BindingPattern(sub)) => {
                count_non_local_names_in_binding_pattern(sub, count, arena);
            }
            None => {
                if let Some(ast::BindingEntryName::Identifier(id)) = &entry.name
                    && !arena.identifiers[*id].is_local()
                {
                    *count += 1;
                }
            }
            Some(ast::BindingEntryAlias::MemberExpression(_)) => {}
        }
    }
}

fn for_each_binding_pattern_identifier(
    pattern: &ast::BindingPattern,
    arena: &ast::AstArena,
    callback: &mut dyn FnMut(&ast::Identifier),
) {
    for entry in &pattern.entries {
        match &entry.alias {
            Some(ast::BindingEntryAlias::Identifier(id)) => callback(&arena.identifiers[*id]),
            Some(ast::BindingEntryAlias::BindingPattern(sub)) => {
                for_each_binding_pattern_identifier(sub, arena, callback);
            }
            None => {
                if let Some(ast::BindingEntryName::Identifier(id)) = &entry.name {
                    callback(&arena.identifiers[*id]);
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
        this_value_needs_environment_resolution: bool,
        function_environment_needed: bool,
        function_environment_bindings_count: usize,
        might_need_arguments_object: bool,
        contains_direct_call_to_eval: bool,
    );
}

/// C-compatible token info for the tokenize callback.
#[repr(C)]
pub struct FFIToken {
    pub token_type: u8,
    pub category: u8,
    pub offset: u32,
    pub length: u32,
    pub trivia_offset: u32,
    pub trivia_length: u32,
}

/// Tokenize a UTF-16 source string, calling `callback` for each token.
///
/// # Safety
/// - `source` must point to a valid UTF-16 buffer of `source_len` elements.
/// - `callback` must be a valid function pointer.
/// - `ctx` is passed through to the callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_tokenize(
    source: *const u16,
    source_len: usize,
    ctx: *mut c_void,
    callback: unsafe extern "C" fn(ctx: *mut c_void, token: *const FFIToken),
) {
    unsafe {
        abort_on_panic(|| {
            let Some(source_slice) = source_from_raw(source, source_len) else {
                return;
            };
            let mut lex = lexer::Lexer::new(source_slice, 1, 0);
            loop {
                let tok = lex.next();
                let is_eof = tok.token_type == token::TokenType::Eof;
                let ffi_tok = FFIToken {
                    token_type: tok.token_type as u8,
                    category: tok.token_type.category() as u8,
                    offset: tok.value_start,
                    length: tok.value_len,
                    trivia_offset: tok.trivia_start,
                    trivia_length: tok.trivia_len,
                };
                callback(ctx, &raw const ffi_tok);
                if is_eof {
                    break;
                }
            }
        });
    }
}

/// Validate the structural integrity of a packed bytecode buffer along with
/// the structural metadata that travels with it (basic block offsets,
/// exception handler ranges, source map entries).
///
/// Returns `true` if every instruction is well-formed against the supplied
/// bounds. On failure, writes the error category, byte offset, and opcode
/// into `*error_out` (when non-null) and returns `false`.
///
/// # Safety
/// - `bytecode_ptr` must point to a buffer of `bytecode_len` bytes, aligned
///   to 8 bytes (matching `alignof(Instruction)`). May be null only when
///   `bytecode_len` is zero.
/// - `bounds` must point to a valid `FFIValidatorBounds`.
/// - `extras` must point to a valid `FFIValidatorExtras`. Each
///   inner pointer may be null only when its corresponding count is zero.
/// - `error_out` must be either null or a writable `FFIValidationError`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_validate_bytecode(
    bytecode_ptr: *const u8,
    bytecode_len: usize,
    bounds: *const bytecode::validator::FFIValidatorBounds,
    extras: *const bytecode::validator::FFIValidatorExtras,
    error_out: *mut bytecode::validator::FFIValidationError,
) -> bool {
    unsafe {
        abort_on_panic(|| {
            use bytecode::validator::{FFIValidationError, ValidationErrorKind, validate_bytecode};

            let write_error = |err: FFIValidationError| {
                if !error_out.is_null() {
                    *error_out = err;
                }
            };

            if bytecode_len > 0 && bytecode_ptr.is_null() {
                write_error(FFIValidationError::new(ValidationErrorKind::TruncatedInstruction, 0, 0));
                return false;
            }
            if !bytecode_ptr.is_null() && !(bytecode_ptr as usize).is_multiple_of(8) {
                write_error(FFIValidationError::new(ValidationErrorKind::BufferNotAligned, 0, 0));
                return false;
            }
            if bounds.is_null() || extras.is_null() {
                write_error(FFIValidationError::new(ValidationErrorKind::TruncatedInstruction, 0, 0));
                return false;
            }

            let bytes = if bytecode_ptr.is_null() {
                &[][..]
            } else {
                std::slice::from_raw_parts(bytecode_ptr, bytecode_len)
            };

            let extras_ref = &*extras;
            let basic_block_offsets = if extras_ref.basic_block_count == 0 {
                &[][..]
            } else {
                std::slice::from_raw_parts(extras_ref.basic_block_offsets, extras_ref.basic_block_count)
            };
            let exception_handlers = if extras_ref.exception_handler_count == 0 {
                &[][..]
            } else {
                std::slice::from_raw_parts(extras_ref.exception_handlers, extras_ref.exception_handler_count)
            };
            let source_map_offsets = if extras_ref.source_map_count == 0 {
                &[][..]
            } else {
                std::slice::from_raw_parts(extras_ref.source_map_offsets, extras_ref.source_map_count)
            };

            match validate_bytecode(
                bytes,
                &*bounds,
                basic_block_offsets,
                exception_handlers,
                source_map_offsets,
            ) {
                Ok(()) => true,
                Err(err) => {
                    write_error(err);
                    false
                }
            }
        })
    }
}
