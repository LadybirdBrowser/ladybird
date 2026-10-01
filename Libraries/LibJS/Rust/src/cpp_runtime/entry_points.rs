/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The `extern "C"` entry points the C++ runtime calls to parse and compile JavaScript.

use super::bytecode_cache;
use super::ffi;
use super::rust_panic::abort_on_panic;
use crate::ast;
use crate::ast::StatementKind;
use crate::ast_dump;
use crate::bytecode;
use crate::compile::CompiledBytecode;
use crate::compile::CompiledProgram;
use crate::compile::CompiledProgramBytecode;
use crate::compile::FunctionPrecompileMode;
use crate::compile::ParsedProgram;
use crate::compile::collect_module_var_names;
use crate::compile::collect_var_names_recursive;
use crate::compile::compile_function_payload_to_bytecode;
use crate::compile::compile_module_as_async_to_bytecode;
use crate::compile::compile_parsed_program_off_thread_impl;
use crate::compile::compile_program_body_to_bytecode;
use crate::compile::for_each_bound_name;
use crate::compile::module_default_export_binding_name;
use crate::compile::module_environment_scope;
use crate::compile::new_module_async_generator;
use crate::compile::new_program_generator;
use crate::lexer;
use crate::parser::Parser;
use crate::parser::ProgramType;
use crate::token;
use crate::u32_from_usize;
use std::cell::RefCell;
use std::ffi::c_void;
use std::rc::Rc;

pub struct CompiledFunction {
    precompiled: Box<bytecode::generator::PrecompiledFunction>,
}

#[repr(C)]
pub struct BytecodeCacheBlob {
    data: *mut u8,
    length: usize,
}

pub struct DecodedBytecodeCacheBlob {
    _blob: Rc<RefCell<bytecode_cache::DecodedCacheBlob>>,
}

fn validate_decoded_blob(blob: &DecodedBytecodeCacheBlob, source_len: usize) -> bool {
    let mut decoded_blob = blob._blob.borrow_mut();
    decoded_blob.validate_for_materialization(source_len).is_ok()
}

// SAFETY: `CompiledFunction` owns GC-free codegen state and is transferred
// from a compile worker back to the main thread for materialization.
unsafe impl Send for CompiledFunction {}

// =============================================================================
// Internal helpers
// =============================================================================

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
    unsafe extern "C" fn(ctx: *mut c_void, message: *const u16, message_len: usize, line: u32, column: u32) -> (),
>;

unsafe fn report_parse_error(
    callback: unsafe extern "C" fn(
        ctx: *mut c_void,
        message: *const u16,
        message_len: usize,
        line: u32,
        column: u32,
    ) -> (),
    context: *mut c_void,
    message: &str,
    line: u32,
    column: u32,
) {
    let message_utf16: Vec<u16> = message.encode_utf16().collect();
    unsafe {
        callback(context, message_utf16.as_ptr(), message_utf16.len(), line, column);
    }
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
                unsafe {
                    report_parse_error(cb, error_context, &err.message, err.line, err.column);
                }
            }
        }
        return true;
    }
    if parser.scope_collector.has_errors() {
        if let Some(cb) = error_callback {
            for err in parser.scope_collector.drain_errors() {
                unsafe {
                    report_parse_error(cb, error_context, &err.message, err.line, err.column);
                }
            }
        }
        return true;
    }
    false
}

unsafe fn create_executable_from_compiled_bytecode(
    bytecode: &mut CompiledBytecode,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    shared_function_data_owner: ffi::SharedFunctionDataOwner,
) -> *mut c_void {
    unsafe {
        bytecode.generator.vm_ptr = vm_ptr;
        bytecode.generator.source_code_ptr = source_code_ptr;
        ffi::create_executable(
            &mut bytecode.generator,
            &bytecode.assembled,
            vm_ptr,
            source_code_ptr,
            shared_function_data_owner,
        )
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
    shared_function_data_owner: ffi::SharedFunctionDataOwner,
) -> *mut c_void {
    let assembled = compile_program_body_to_bytecode(generator, program, scope_id);
    unsafe {
        ffi::create_executable(
            generator,
            &assembled,
            vm_ptr,
            source_code_ptr,
            shared_function_data_owner,
        )
    }
}

// =============================================================================
// FFI entry points: program compilation
// =============================================================================

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

            let initial_line_number = if pt == ProgramType::Module && initial_line_number == 0 {
                1
            } else {
                initial_line_number
            };
            let mut parser = Parser::new_with_line_offset(source_slice, pt, u32_from_usize(initial_line_number));

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
            report_parse_error(
                error_callback.unwrap(),
                error_context,
                &err.message,
                err.line,
                err.column,
            );
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

fn compile_parsed_program_off_thread_impl_into_raw(
    parsed: *mut ParsedProgram,
    source_len: usize,
    function_precompile_mode: FunctionPrecompileMode,
) -> *mut CompiledProgram {
    unsafe {
        abort_on_panic(|| {
            if parsed.is_null() {
                return std::ptr::null_mut();
            }

            let parsed = Box::from_raw(parsed);
            Box::into_raw(Box::new(compile_parsed_program_off_thread_impl(
                *parsed,
                source_len,
                function_precompile_mode,
            )))
        })
    }
}

/// Retain an independent parse snapshot for background cache generation.
/// The immutable arena and compiled regex handles are shared; function-table
/// ownership is independent so either compilation can consume its functions.
///
/// # Safety
/// `parsed` must point to a valid parsed program with no errors.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_clone_parsed_program(parsed: *const ParsedProgram) -> *mut ParsedProgram {
    unsafe {
        abort_on_panic(|| {
            let parsed = &*parsed;
            assert!(parsed.errors.is_empty());
            Box::into_raw(Box::new(ParsedProgram {
                program: parsed.program.clone(),
                function_table: parsed.function_table.clone(),
                arena: parsed.arena.clone(),
                scope_ref: parsed.scope_ref,
                program_type: parsed.program_type,
                is_strict_mode: parsed.is_strict_mode,
                has_top_level_await: parsed.has_top_level_await,
                errors: Vec::new(),
                ast_dump: None,
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
    compile_parsed_program_off_thread_impl_into_raw(parsed, source_len, FunctionPrecompileMode::EagerOnly)
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
    compile_parsed_program_off_thread_impl_into_raw(parsed, source_len, FunctionPrecompileMode::All)
}

/// Free a CompiledProgram without materializing it.
///
/// # Safety
/// `compiled` must be a valid pointer from `rust_compile_parsed_program_off_thread()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_free_compiled_program(compiled: *mut CompiledProgram) {
    unsafe {
        fn free_generator_regexes(generator: &mut bytecode::generator::Generator) {
            for regex in generator.compiled_regexes.drain(..) {
                unsafe { crate::host::free_compiled_regex(regex) };
            }
            for shared_data in &mut generator.shared_function_data {
                if let Some(precompiled) = &mut shared_data.precompiled_function {
                    free_generator_regexes(&mut precompiled.generator);
                }
            }
        }

        let mut compiled = Box::from_raw(compiled);
        match &mut compiled.bytecode {
            CompiledProgramBytecode::Program(bytecode) | CompiledProgramBytecode::AsyncModule(bytecode) => {
                free_generator_regexes(&mut bytecode.generator);
            }
        }
        for declaration in &mut compiled.declaration_functions {
            if let Some(precompiled) = &mut declaration.precompiled_function {
                free_generator_regexes(&mut precompiled.generator);
            }
        }
    }
}

/// Collect all source-map positions from a fully compiled program.
///
/// # Safety
/// `compiled` must be a valid pointer returned by
/// `rust_compile_parsed_program_fully_off_thread`, and `callback` must be valid
/// for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_collect_compiled_program_breakpoint_positions(
    compiled: *const CompiledProgram,
    context: *mut c_void,
    callback: unsafe extern "C" fn(context: *mut c_void, line: u32, column: u32),
) {
    fn collect_precompiled_function(
        precompiled: &bytecode::generator::PrecompiledFunction,
        context: *mut c_void,
        callback: unsafe extern "C" fn(context: *mut c_void, line: u32, column: u32),
    ) {
        collect_bytecode(&precompiled.generator, &precompiled.assembled, context, callback);
    }

    fn collect_bytecode(
        generator: &bytecode::generator::Generator,
        assembled: &bytecode::generator::AssembledBytecode,
        context: *mut c_void,
        callback: unsafe extern "C" fn(context: *mut c_void, line: u32, column: u32),
    ) {
        for entry in &assembled.source_map {
            if entry.line != 0 {
                unsafe { callback(context, entry.line, entry.column) };
            }
        }
        for shared_data in &generator.shared_function_data {
            if let Some(precompiled) = &shared_data.precompiled_function {
                collect_precompiled_function(precompiled, context, callback);
            }
        }
    }

    unsafe {
        abort_on_panic(|| {
            if compiled.is_null() {
                return;
            }
            let compiled = &*compiled;
            match &compiled.bytecode {
                CompiledProgramBytecode::Program(bytecode) | CompiledProgramBytecode::AsyncModule(bytecode) => {
                    collect_bytecode(&bytecode.generator, &bytecode.assembled, context, callback);
                }
            }
            for declaration in &compiled.declaration_functions {
                if let Some(precompiled) = &declaration.precompiled_function {
                    collect_precompiled_function(precompiled, context, callback);
                }
            }
        });
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

/// Decode an ImmutableBytes-backed bytecode cache blob into a parser-free cache handle.
///
/// # Safety
/// - `data` must point to `length` readable bytes.
/// - `owner` must keep `data` alive until `free_owner` is called.
/// - `clone_owner` must return a new `bytecode_owner` for `rust_create_executable()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_decode_bytecode_cache_blob_with_owner(
    data: *const u8,
    length: usize,
    expected_program_type: u8,
    expected_source_hash: *const u8,
    expected_source_hash_len: usize,
    owner: *mut c_void,
    clone_owner: bytecode_cache::CloneBytecodeCacheBlobOwner,
    free_owner: bytecode_cache::FreeBytecodeCacheBlobOwner,
) -> *mut DecodedBytecodeCacheBlob {
    unsafe {
        abort_on_panic(|| {
            if owner.is_null() {
                return std::ptr::null_mut();
            }
            let reject = || {
                free_owner(owner);
                std::ptr::null_mut()
            };
            if data.is_null() || expected_source_hash.is_null() || expected_source_hash_len != 32 {
                return reject();
            }
            let expected_program_type = match expected_program_type {
                0 => ast::ProgramType::Script,
                1 => ast::ProgramType::Module,
                _ => return reject(),
            };
            let expected_source_hash = std::slice::from_raw_parts(expected_source_hash, expected_source_hash_len)
                .try_into()
                .expect("source hash length was checked");
            let Some(blob) = bytecode_cache::decode_blob_with_foreign_owner(
                std::slice::from_raw_parts(data, length),
                expected_program_type,
                expected_source_hash,
                bytecode_cache::ForeignBytecodeCacheBlobOwner {
                    owner,
                    clone_owner,
                    free_owner,
                },
            ) else {
                return std::ptr::null_mut();
            };
            Box::into_raw(Box::new(DecodedBytecodeCacheBlob {
                _blob: Rc::new(RefCell::new(blob)),
            }))
        })
    }
}

/// Free a decoded bytecode cache blob.
///
/// # Safety
/// `blob` must be a valid pointer from `rust_decode_bytecode_cache_blob_with_owner()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_free_decoded_bytecode_cache_blob(blob: *mut DecodedBytecodeCacheBlob) {
    unsafe {
        drop(Box::from_raw(blob));
    }
}

/// Validate a decoded bytecode cache blob before materializing it.
///
/// # Safety
/// `blob` must be a valid pointer from `rust_decode_bytecode_cache_blob_with_owner()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_validate_decoded_bytecode_cache_blob(
    blob: *mut DecodedBytecodeCacheBlob,
    source_len: usize,
) -> bool {
    unsafe {
        abort_on_panic(|| {
            if blob.is_null() {
                return false;
            }
            (*blob)
                ._blob
                .borrow_mut()
                .validate_for_materialization(source_len)
                .is_ok()
        })
    }
}

/// Add a reference to a decoded bytecode cache blob.
///
/// # Safety
/// `blob` must be a valid pointer from `rust_decode_bytecode_cache_blob_with_owner()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_ref_decoded_bytecode_cache_blob(
    blob: *const DecodedBytecodeCacheBlob,
) -> *mut DecodedBytecodeCacheBlob {
    unsafe {
        abort_on_panic(|| {
            if blob.is_null() {
                return std::ptr::null_mut();
            }

            Box::into_raw(Box::new(DecodedBytecodeCacheBlob {
                _blob: Rc::clone(&(*blob)._blob),
            }))
        })
    }
}

/// Materialize a decoded script bytecode cache blob. Consumes and frees the blob.
///
/// # Safety
/// - `blob` must be a valid pointer from `rust_decode_bytecode_cache_blob_with_owner()`.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
/// - `gdi_context` must be a valid pointer to a C++ ScriptGdiBuilder.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_materialize_bytecode_cache_script(
    blob: *mut DecodedBytecodeCacheBlob,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    source_len: usize,
    shared_function_data_list_ptr: *mut c_void,
    gdi_context: *mut c_void,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            if blob.is_null() {
                return std::ptr::null_mut();
            }
            let blob = Box::from_raw(blob);
            if !validate_decoded_blob(&blob, source_len) {
                return std::ptr::null_mut();
            }
            blob._blob
                .borrow()
                .materialize_script(vm_ptr, source_code_ptr, shared_function_data_list_ptr, gdi_context)
        })
    }
}

/// Materialize a decoded module bytecode cache blob. Consumes and frees the blob.
///
/// # Safety
/// - `blob` must be a valid pointer from `rust_decode_bytecode_cache_blob_with_owner()`.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
/// - `module_context` must be a valid `ModuleBuilder*`.
/// - `callbacks` must point to a valid `ModuleCallbacks`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_materialize_bytecode_cache_module(
    blob: *mut DecodedBytecodeCacheBlob,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    source_len: usize,
    shared_function_data_list_ptr: *mut c_void,
    module_context: *mut c_void,
    callbacks: *const ModuleCallbacks,
    tla_executable_out: *mut *mut c_void,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            if blob.is_null() {
                return std::ptr::null_mut();
            }
            let blob = Box::from_raw(blob);
            if !validate_decoded_blob(&blob, source_len) {
                return std::ptr::null_mut();
            }
            blob._blob.borrow().materialize_module(
                vm_ptr,
                source_code_ptr,
                shared_function_data_list_ptr,
                module_context,
                callbacks,
                tla_executable_out,
            )
        })
    }
}

/// Install a decoded script bytecode cache blob into an existing program.
/// Consumes and frees the blob.
///
/// # Safety
/// - `blob` must be a valid pointer from `rust_decode_bytecode_cache_blob_with_owner()`.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
/// - `existing_executable_ptr` must be a valid `Bytecode::Executable*`.
/// - `existing_declaration_function_ptrs` must be null when
///   `existing_declaration_function_count` is zero, otherwise it must point to
///   `existing_declaration_function_count` valid `SharedFunctionInstanceData*`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_install_bytecode_cache_script(
    blob: *mut DecodedBytecodeCacheBlob,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    source_len: usize,
    existing_executable_ptr: *const c_void,
    existing_declaration_function_ptrs: *const *mut c_void,
    existing_declaration_function_count: usize,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            if blob.is_null() {
                return std::ptr::null_mut();
            }
            let blob = Box::from_raw(blob);
            let existing_declaration_functions = if existing_declaration_function_count == 0 {
                &[]
            } else {
                if existing_declaration_function_ptrs.is_null() {
                    return std::ptr::null_mut();
                }
                std::slice::from_raw_parts(existing_declaration_function_ptrs, existing_declaration_function_count)
            };
            if !validate_decoded_blob(&blob, source_len) {
                return std::ptr::null_mut();
            }
            blob._blob.borrow().install_script(
                vm_ptr,
                source_code_ptr,
                existing_executable_ptr,
                existing_declaration_functions,
            )
        })
    }
}

/// Install a decoded module bytecode cache blob into an existing program.
/// Consumes and frees the blob.
///
/// # Safety
/// - `blob` must be a valid pointer from `rust_decode_bytecode_cache_blob_with_owner()`.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
/// - `existing_executable_ptr` must be null or a valid `Bytecode::Executable*`.
/// - `existing_declaration_function_ptrs` must be null when
///   `existing_declaration_function_count` is zero, otherwise it must point to
///   `existing_declaration_function_count` valid `SharedFunctionInstanceData*`.
/// - `existing_tla_sfd_ptr` must be null or a valid `SharedFunctionInstanceData*`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_install_bytecode_cache_module(
    blob: *mut DecodedBytecodeCacheBlob,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    source_len: usize,
    existing_executable_ptr: *const c_void,
    existing_declaration_function_ptrs: *const *mut c_void,
    existing_declaration_function_count: usize,
    existing_tla_sfd_ptr: *mut c_void,
    tla_executable_out: *mut *mut c_void,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            if blob.is_null() {
                return std::ptr::null_mut();
            }
            let blob = Box::from_raw(blob);
            let existing_declaration_functions = if existing_declaration_function_count == 0 {
                &[]
            } else {
                if existing_declaration_function_ptrs.is_null() {
                    return std::ptr::null_mut();
                }
                std::slice::from_raw_parts(existing_declaration_function_ptrs, existing_declaration_function_count)
            };
            if !validate_decoded_blob(&blob, source_len) {
                return std::ptr::null_mut();
            }
            blob._blob.borrow().install_module(
                vm_ptr,
                source_code_ptr,
                existing_executable_ptr,
                existing_declaration_functions,
                existing_tla_sfd_ptr,
                tla_executable_out,
            )
        })
    }
}

/// Materialize a decoded function executable from a bytecode cache blob.
/// Consumes and frees the cached executable.
///
/// # Safety
/// - `cached_executable` must be a valid pointer attached to a
///   `SharedFunctionInstanceData` by bytecode cache materialization.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_materialize_bytecode_cache_function(
    cached_executable: *mut c_void,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    shared_function_data_list_ptr: *mut c_void,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            bytecode_cache::materialize_cached_function(
                cached_executable,
                vm_ptr,
                source_code_ptr,
                shared_function_data_list_ptr,
            )
        })
    }
}

/// Free a cached decoded function executable without materializing it.
///
/// # Safety
/// `cached_executable` must be either null or a valid pointer attached to a
/// `SharedFunctionInstanceData` by bytecode cache materialization.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_free_cached_bytecode_executable(cached_executable: *mut c_void) {
    unsafe {
        abort_on_panic(|| {
            bytecode_cache::free_cached_function(cached_executable);
        });
    }
}

/// Materialize a precompiled function executable.
/// Consumes and frees the precompiled executable.
///
/// # Safety
/// - `precompiled_executable` must be a valid `Box<PrecompiledFunction>`
///   pointer attached to a `SharedFunctionInstanceData`.
/// - `vm_ptr` must be a valid `JS::VM*`.
/// - `source_code_ptr` must be a valid `JS::SourceCode const*`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_materialize_precompiled_bytecode_function(
    precompiled_executable: *mut c_void,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    shared_function_data_list_ptr: *mut c_void,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            if precompiled_executable.is_null() {
                return std::ptr::null_mut();
            }
            let mut precompiled =
                Box::from_raw(precompiled_executable as *mut bytecode::generator::PrecompiledFunction);
            precompiled.generator.vm_ptr = vm_ptr;
            precompiled.generator.source_code_ptr = source_code_ptr;
            ffi::create_executable(
                &mut precompiled.generator,
                &precompiled.assembled,
                vm_ptr,
                source_code_ptr,
                if shared_function_data_list_ptr.is_null() {
                    ffi::SharedFunctionDataOwner::None
                } else {
                    ffi::SharedFunctionDataOwner::List(shared_function_data_list_ptr)
                },
            )
        })
    }
}

/// Free a precompiled function executable without materializing it.
///
/// # Safety
/// `precompiled_executable` must be either null or a valid
/// `Box<PrecompiledFunction>` pointer attached to a `SharedFunctionInstanceData`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_free_precompiled_bytecode_executable(precompiled_executable: *mut c_void) {
    unsafe {
        abort_on_panic(|| {
            if !precompiled_executable.is_null() {
                drop(Box::from_raw(
                    precompiled_executable as *mut bytecode::generator::PrecompiledFunction,
                ));
            }
        });
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
    shared_function_data_list_ptr: *mut c_void,
    gdi_context: *mut c_void,
    source_len: usize,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            let mut parsed = Box::from_raw(parsed);

            let mut generator = new_program_generator(parsed.is_strict_mode, vm_ptr, source_code_ptr, source_len);
            generator.function_table = std::mem::take(&mut parsed.function_table);
            generator.arena = parsed.arena.clone();
            let shared_function_data_context = ffi::SharedFunctionDataCreationContext {
                vm_ptr,
                source_code_ptr,
                owner: ffi::SharedFunctionDataOwner::List(shared_function_data_list_ptr),
            };
            let exec_ptr = compile_program_body(
                &mut generator,
                &parsed.program,
                parsed.scope_ref,
                vm_ptr,
                source_code_ptr,
                shared_function_data_context.owner,
            );
            if exec_ptr.is_null() {
                return std::ptr::null_mut();
            }

            extract_script_gdi(
                &parsed.arena.scopes[parsed.scope_ref],
                parsed.is_strict_mode,
                shared_function_data_context,
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
    shared_function_data_list_ptr: *mut c_void,
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

            let shared_function_data_context = ffi::SharedFunctionDataCreationContext {
                vm_ptr,
                source_code_ptr,
                owner: ffi::SharedFunctionDataOwner::List(shared_function_data_list_ptr),
            };
            let exec_ptr = create_executable_from_compiled_bytecode(
                bytecode,
                vm_ptr,
                source_code_ptr,
                shared_function_data_context.owner,
            );
            if exec_ptr.is_null() {
                return std::ptr::null_mut();
            }

            extract_script_gdi(
                &compiled.parsed.arena.scopes[compiled.parsed.scope_ref],
                compiled.parsed.is_strict_mode,
                shared_function_data_context,
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

            let eval_referenced_private_names = parser.eval_referenced_private_names().to_vec();

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
            let exec_ptr = compile_program_body(
                &mut generator,
                &program,
                scope_id,
                vm_ptr,
                source_code_ptr,
                ffi::SharedFunctionDataOwner::None,
            );
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
                &eval_referenced_private_names,
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
                            report_parse_error(cb, error_context, &msg, token.line_number, token.line_column);
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
                        report_parse_error(cb, error_context, &err.message, err.line, err.column);
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
                    report_parse_error(cb, error_context, "Failed to parse dynamic function", 0, 0);
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

            ffi::create_sfd_for_gdi(
                function_data,
                subtable,
                ffi::SharedFunctionDataCreationContext {
                    vm_ptr,
                    source_code_ptr,
                    owner: ffi::SharedFunctionDataOwner::None,
                },
                is_strict,
                arena,
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
                    let sfd_ptr = ffi::create_sfd_for_gdi(
                        function_data,
                        subtable,
                        ffi::SharedFunctionDataCreationContext {
                            vm_ptr,
                            source_code_ptr,
                            owner: ffi::SharedFunctionDataOwner::None,
                        },
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
    shared_function_data_list_ptr: *mut c_void,
    module_context: *mut c_void,
    callbacks: *const ModuleCallbacks,
    tla_executable_out: *mut *mut c_void,
    source_len: usize,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            let mut parsed = Box::from_raw(parsed);
            let cb = &*callbacks;
            let shared_function_data_context = ffi::SharedFunctionDataCreationContext {
                vm_ptr,
                source_code_ptr,
                owner: ffi::SharedFunctionDataOwner::List(shared_function_data_list_ptr),
            };

            // 1. Report has_top_level_await.
            (cb.set_has_top_level_await)(module_context, parsed.has_top_level_await);

            // 2. Process imports and exports.
            extract_module_metadata(&parsed.arena.scopes[parsed.scope_ref], module_context, cb);

            // 3. Extract var declared names and lexical bindings.
            extract_module_declarations(
                &parsed.arena.scopes[parsed.scope_ref],
                shared_function_data_context,
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
                    shared_function_data_context,
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
                    shared_function_data_context.owner,
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
    shared_function_data_list_ptr: *mut c_void,
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
            let shared_function_data_context = ffi::SharedFunctionDataCreationContext {
                vm_ptr,
                source_code_ptr,
                owner: ffi::SharedFunctionDataOwner::List(shared_function_data_list_ptr),
            };

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
                shared_function_data_context,
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
                    let exec_ptr = create_executable_from_compiled_bytecode(
                        bytecode,
                        vm_ptr,
                        source_code_ptr,
                        shared_function_data_context.owner,
                    );
                    if !tla_executable_out.is_null() {
                        *tla_executable_out = exec_ptr;
                    }
                    std::ptr::null_mut()
                }
                CompiledProgramBytecode::Program(bytecode) => {
                    if !tla_executable_out.is_null() {
                        *tla_executable_out = std::ptr::null_mut();
                    }
                    create_executable_from_compiled_bytecode(
                        bytecode,
                        vm_ptr,
                        source_code_ptr,
                        shared_function_data_context.owner,
                    )
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
    attribute_keys: *const ffi::FFIUtf16Slice,
    attribute_values: *const ffi::FFIUtf16Slice,
    attribute_count: usize,
);
pub(super) type ModuleExportEntryCallback = unsafe extern "C" fn(
    ctx: *mut c_void,
    kind: u8,
    export_name: *const u16,
    export_name_len: usize,
    local_or_import_name: *const u16,
    local_or_import_name_len: usize,
    module_specifier: *const u16,
    specifier_len: usize,
    attribute_keys: *const ffi::FFIUtf16Slice,
    attribute_values: *const ffi::FFIUtf16Slice,
    attribute_count: usize,
);
type ModuleRequestedModuleCallback = unsafe extern "C" fn(
    ctx: *mut c_void,
    specifier: *const u16,
    specifier_len: usize,
    attribute_keys: *const ffi::FFIUtf16Slice,
    attribute_values: *const ffi::FFIUtf16Slice,
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
fn build_attribute_slices(attributes: &[ast::ImportAttribute]) -> (Vec<ffi::FFIUtf16Slice>, Vec<ffi::FFIUtf16Slice>) {
    attributes
        .iter()
        .map(|a| {
            (
                ffi::FFIUtf16Slice::from(a.key.as_ref()),
                ffi::FFIUtf16Slice::from(a.value.as_ref()),
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
    shared_function_data_list_ptr: *mut c_void,
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
                shared_function_data_list_ptr,
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
        use ast::ExportEntryKind;
        use ast::StatementKind;

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

        if let Some(name) = module_default_export_binding_name(scope) {
            (cb.set_default_export_binding)(ctx, name.as_ptr(), name.len());
        }

        // Process export entries (matching SourceTextModule::parse steps 9-10).
        for child in &scope.children {
            let StatementKind::Export(ref export_data) = child.inner else {
                continue;
            };

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
                            // Re-export of an imported module namespace object becomes an indirect namespace export.
                            call_export_callback(
                                cb.push_indirect_export,
                                ctx,
                                ExportEntryKind::ModuleRequestAll as u8,
                                entry.export_name.as_ref(),
                                None,
                                Some(&import_entry.module_request),
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
    shared_function_data_context: ffi::SharedFunctionDataCreationContext,
    ctx: *mut c_void,
    cb: &ModuleCallbacks,
    function_table: &mut ast::FunctionTable,
    arena: &std::sync::Arc<ast::AstArena>,
) {
    unsafe {
        use ast::StatementKind;

        let default_name: ast::Utf16String = utf16!("*default*").into();
        let module_environment_scope = module_environment_scope(scope, arena);

        // Var declared names (walk all nesting levels).
        for child in &scope.children {
            collect_module_var_names(&child.inner, arena, &mut |name| {
                (cb.push_var_name)(ctx, name.as_ptr(), name.len());
            });
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
                    let sfd_ptr = ffi::create_shared_function_data(
                        function_data,
                        subtable,
                        shared_function_data_context,
                        true,
                        None,
                        arena.clone(),
                        Some(module_environment_scope.clone()),
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

unsafe fn compile_module_as_async(
    program: &ast::Statement,
    scope_id: ast::ScopeId,
    arena: std::sync::Arc<ast::AstArena>,
    shared_function_data_context: ffi::SharedFunctionDataCreationContext,
    source_len: usize,
    function_table: ast::FunctionTable,
) -> *mut c_void {
    unsafe {
        let mut generator = new_module_async_generator(source_len, function_table);
        generator.arena = arena;
        generator.vm_ptr = shared_function_data_context.vm_ptr;
        generator.source_code_ptr = shared_function_data_context.source_code_ptr;

        let assembled = compile_module_as_async_to_bytecode(program, scope_id, &mut generator);
        ffi::create_executable(
            &mut generator,
            &assembled,
            shared_function_data_context.vm_ptr,
            shared_function_data_context.source_code_ptr,
            shared_function_data_context.owner,
        )
    }
}

unsafe extern "C" {
    fn module_sfd_set_name(sfd_ptr: *mut c_void, name: *const u16, name_len: usize);
}

// =============================================================================
// GDI/EDI metadata extraction
// =============================================================================

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
    shared_function_data_owner: ffi::SharedFunctionDataOwner,
    is_strict: bool,
    push_var_name: &mut dyn FnMut(&[u16]),
    push_function: &mut dyn FnMut(*mut c_void, &[u16]),
    push_var_scoped_name: &mut dyn FnMut(&[u16]),
    push_annex_b_name: &mut dyn FnMut(&[u16]),
    push_lexical_binding: &mut dyn FnMut(&[u16], bool),
    function_table: &mut ast::FunctionTable,
    arena: &std::sync::Arc<ast::AstArena>,
) {
    use ast::DeclarationKind;
    use ast::StatementKind;

    // Var names (var declarations at any nesting level + top-level function declarations)
    for child in &scope.children {
        collect_var_names_recursive(&child.inner, arena, push_var_name);
        if let Some(fd) = child.inner.function_declaration_for_labelled_item()
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
        if let Some(fd) = child.inner.function_declaration_for_labelled_item()
            && let Some(name_ident) = fd.name
        {
            last_position.insert(arena.identifiers[name_ident].name, i);
        }
    }
    for (i, child) in scope.children.iter().enumerate() {
        if let Some(fd) = child.inner.function_declaration_for_labelled_item()
            && let Some(name_ident) = fd.name
            && last_position.get(&arena.identifiers[name_ident].name).copied() == Some(i)
        {
            let function_data = function_table.take(fd.function_id);
            let subtable = function_table.extract_reachable(&function_data, &arena.scopes);
            let sfd_ptr = unsafe {
                ffi::create_sfd_for_gdi(
                    function_data,
                    subtable,
                    ffi::SharedFunctionDataCreationContext {
                        vm_ptr,
                        source_code_ptr,
                        owner: shared_function_data_owner,
                    },
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
#[allow(clippy::too_many_arguments)]
unsafe fn extract_eval_gdi(
    scope: &ast::ScopeData,
    is_strict: bool,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    ctx: *mut c_void,
    function_table: &mut ast::FunctionTable,
    arena: &std::sync::Arc<ast::AstArena>,
    referenced_private_names: &[ast::Utf16String],
) {
    unsafe {
        use ffi::eval_gdi_push_annex_b_name;
        use ffi::eval_gdi_push_function;
        use ffi::eval_gdi_push_lexical_binding;
        use ffi::eval_gdi_push_private_name;
        use ffi::eval_gdi_push_var_name;
        use ffi::eval_gdi_push_var_scoped_name;
        use ffi::eval_gdi_set_strict;

        eval_gdi_set_strict(ctx, is_strict);

        extract_gdi_common(
            scope,
            vm_ptr,
            source_code_ptr,
            ffi::SharedFunctionDataOwner::None,
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

        for name in referenced_private_names {
            eval_gdi_push_private_name(ctx, name.as_ptr(), name.len());
        }
    }
}

/// Extract GDI metadata from a program-level ScopeData and populate
/// the C++ ScriptGdiBuilder via callbacks.
unsafe fn extract_script_gdi(
    scope: &ast::ScopeData,
    is_strict: bool,
    shared_function_data_context: ffi::SharedFunctionDataCreationContext,
    ctx: *mut c_void,
    function_table: &mut ast::FunctionTable,
    arena: &std::sync::Arc<ast::AstArena>,
) {
    unsafe {
        use ast::DeclarationKind;
        use ast::StatementKind;
        use ffi::script_gdi_push_annex_b_name;
        use ffi::script_gdi_push_function;
        use ffi::script_gdi_push_lexical_binding;
        use ffi::script_gdi_push_lexical_name;
        use ffi::script_gdi_push_var_name;
        use ffi::script_gdi_push_var_scoped_name;

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
            shared_function_data_context.vm_ptr,
            shared_function_data_context.source_code_ptr,
            shared_function_data_context.owner,
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
    shared_function_data_list_ptr: *mut c_void,
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

            ffi::create_executable(
                &mut precompiled.generator,
                &precompiled.assembled,
                vm_ptr,
                source_code_ptr,
                if shared_function_data_list_ptr.is_null() {
                    ffi::SharedFunctionDataOwner::None
                } else {
                    ffi::SharedFunctionDataOwner::List(shared_function_data_list_ptr)
                },
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

/// Attach a GC-free compiled function to an existing SFD.
///
/// Consumes the compiled function. Its precompiled bytecode is materialized
/// lazily the first time the function is called.
///
/// # Safety
/// - `compiled` must be a valid pointer returned by `rust_compile_function_off_thread`.
/// - `vm_ptr`, `source_code_ptr`, and `sfd_ptr` must be valid main-thread pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_materialize_compiled_function(
    compiled: *mut CompiledFunction,
    _vm_ptr: *mut c_void,
    _source_code_ptr: *const c_void,
    sfd_ptr: *mut c_void,
) {
    unsafe {
        abort_on_panic(|| {
            if compiled.is_null() {
                return;
            }
            let CompiledFunction { precompiled } = *Box::from_raw(compiled);
            let uses_this = precompiled.metadata.uses_this;
            let this_value_needs_environment_resolution = precompiled.metadata.this_value_needs_environment_resolution;
            let function_environment_needed = precompiled.metadata.function_environment_needed;
            let function_environment_bindings_count = precompiled.metadata.function_environment_bindings_count;
            let var_environment_bindings_count = precompiled.metadata.var_environment_bindings_count;
            let might_need_arguments = precompiled.metadata.might_need_arguments;
            let contains_eval = precompiled.metadata.contains_eval;
            let precompiled_ptr = Box::into_raw(precompiled) as *mut c_void;
            ffi::rust_sfd_set_precompiled_bytecode_executable(
                sfd_ptr,
                precompiled_ptr,
                uses_this,
                this_value_needs_environment_resolution,
                function_environment_needed,
                function_environment_bindings_count,
                var_environment_bindings_count,
                might_need_arguments,
                contains_eval,
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
            metadata.var_environment_bindings_count,
            metadata.might_need_arguments,
            metadata.contains_eval,
        );
    }
}

unsafe extern "C" {
    fn rust_sfd_set_metadata(
        sfd_ptr: *mut c_void,
        uses_this: bool,
        this_value_needs_environment_resolution: bool,
        function_environment_needed: bool,
        function_environment_bindings_count: usize,
        var_environment_bindings_count: usize,
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
            use bytecode::validator::FFIValidationError;
            use bytecode::validator::ValidationErrorKind;
            use bytecode::validator::validate_bytecode;

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

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;
    use std::sync::atomic::Ordering;

    static FREED_FOREIGN_OWNERS: AtomicUsize = AtomicUsize::new(0);

    unsafe extern "C" fn count_freed_foreign_owner(owner: *mut c_void) {
        FREED_FOREIGN_OWNERS.fetch_add(1, Ordering::Relaxed);
        unsafe {
            drop(Box::from_raw(owner.cast::<u8>()));
        }
    }

    unsafe extern "C" fn clone_foreign_owner(owner: *const c_void) -> *mut c_void {
        let value = unsafe { *owner.cast::<u8>() };
        Box::into_raw(Box::new(value)).cast()
    }

    fn new_foreign_owner() -> *mut c_void {
        Box::into_raw(Box::new(0u8)).cast()
    }

    #[test]
    fn decode_blob_with_owner_frees_owner_on_early_rejection() {
        FREED_FOREIGN_OWNERS.store(0, Ordering::Relaxed);
        let source_hash = [0u8; 32];

        let blob = unsafe {
            rust_decode_bytecode_cache_blob_with_owner(
                std::ptr::null(),
                0,
                0,
                source_hash.as_ptr(),
                source_hash.len(),
                new_foreign_owner(),
                clone_foreign_owner,
                count_freed_foreign_owner,
            )
        };
        assert!(blob.is_null());
        assert_eq!(FREED_FOREIGN_OWNERS.load(Ordering::Relaxed), 1);

        let bytes = [0u8; 1];
        let blob = unsafe {
            rust_decode_bytecode_cache_blob_with_owner(
                bytes.as_ptr(),
                bytes.len(),
                2,
                source_hash.as_ptr(),
                source_hash.len(),
                new_foreign_owner(),
                clone_foreign_owner,
                count_freed_foreign_owner,
            )
        };
        assert!(blob.is_null());
        assert_eq!(FREED_FOREIGN_OWNERS.load(Ordering::Relaxed), 2);
    }
}
