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
use crate::ast_dump;
use crate::bytecode;
use crate::bytecode::executable::ExecutableData;
use crate::bytecode::generator::PendingSharedFunctionData;
use crate::bytecode::generator::PrecompiledFunction;
use crate::compile::CompiledProgram;
use crate::compile::CompiledProgramBytecode;
use crate::compile::CompiledScript;
use crate::compile::EvalDeclarations;
use crate::compile::FunctionPrecompileMode;
use crate::compile::ParsedProgram;
use crate::compile::ScriptDeclarations;
use crate::compile::collect_eval_declarations;
use crate::compile::collect_script_declarations;
use crate::compile::compile_function_payload_to_bytecode;
use crate::compile::compile_module_as_async_to_bytecode;
use crate::compile::compile_parsed_program_off_thread_impl;
use crate::compile::compile_program_body_to_bytecode;
use crate::compile::compile_script;
use crate::compile::new_module_async_generator;
use crate::compile::new_program_generator;
use crate::compile::parse;
use crate::lexer;
use crate::parser::ProgramType;
use crate::token;
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

/// Shared compilation pipeline: local variable setup → codegen → assemble → create Executable.
///
/// Called by program-level entry points that compile synchronously on the main thread. Also returns the functions
/// that codegen left in the function table for declaration instantiation.
unsafe fn compile_program_body(
    mut generator: bytecode::generator::Generator,
    program: &ast::Statement,
    scope_id: ast::ScopeId,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    shared_function_data_owner: ffi::SharedFunctionDataOwner,
) -> (*mut c_void, ast::FunctionTable) {
    let assembled = compile_program_body_to_bytecode(&mut generator, program, scope_id);
    let function_table = std::mem::take(&mut generator.function_table);
    let executable = ExecutableData::new(generator, assembled);
    let executable_ptr =
        unsafe { ffi::create_executable(executable, vm_ptr, source_code_ptr, shared_function_data_owner) };
    (executable_ptr, function_table)
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

            let parsed = parse(source_slice, pt, initial_line_number);

            // Dump AST if requested (after scope analysis).
            if dump_ast && !parsed.has_errors() {
                ast_dump::dump_program(&parsed.program, use_color, &parsed.function_table, &parsed.arena);
            }

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
        fn free_executable_regexes(executable: &mut ExecutableData) {
            for regex in executable.compiled_regexes.drain(..) {
                unsafe { crate::host::free_compiled_regex(regex) };
            }
            for shared_data in &mut executable.shared_function_data {
                if let Some(precompiled) = &mut shared_data.precompiled_function {
                    free_executable_regexes(&mut precompiled.executable);
                }
            }
        }

        let mut compiled = Box::from_raw(compiled);
        match &mut compiled.bytecode {
            CompiledProgramBytecode::Program(executable) | CompiledProgramBytecode::AsyncModule(executable) => {
                free_executable_regexes(executable);
            }
        }
        for declaration in &mut compiled.declaration_functions {
            if let Some(precompiled) = &mut declaration.precompiled_function {
                free_executable_regexes(&mut precompiled.executable);
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
        precompiled: &PrecompiledFunction,
        context: *mut c_void,
        callback: unsafe extern "C" fn(context: *mut c_void, line: u32, column: u32),
    ) {
        collect_bytecode(&precompiled.executable, context, callback);
    }

    fn collect_bytecode(
        executable: &ExecutableData,
        context: *mut c_void,
        callback: unsafe extern "C" fn(context: *mut c_void, line: u32, column: u32),
    ) {
        for entry in &executable.source_map {
            if entry.line != 0 {
                unsafe { callback(context, entry.line, entry.column) };
            }
        }
        for shared_data in &executable.shared_function_data {
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
                CompiledProgramBytecode::Program(executable) | CompiledProgramBytecode::AsyncModule(executable) => {
                    collect_bytecode(executable, context, callback);
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
            let precompiled = Box::from_raw(precompiled_executable as *mut PrecompiledFunction);
            ffi::create_executable(
                precompiled.executable,
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
                drop(Box::from_raw(precompiled_executable as *mut PrecompiledFunction));
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
        let dump = (*parsed).ast_dump();
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
            let parsed = Box::from_raw(parsed);
            let is_strict = parsed.is_strict_mode;
            let CompiledScript {
                executable,
                declarations,
            } = compile_script(*parsed, source_len);

            let shared_function_data_context = ffi::SharedFunctionDataCreationContext {
                vm_ptr,
                source_code_ptr,
                owner: ffi::SharedFunctionDataOwner::List(shared_function_data_list_ptr),
            };
            let exec_ptr =
                ffi::create_executable(executable, vm_ptr, source_code_ptr, shared_function_data_context.owner);
            if exec_ptr.is_null() {
                return std::ptr::null_mut();
            }

            push_script_declarations(declarations, is_strict, shared_function_data_context, gdi_context);

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

            let CompiledProgram {
                mut parsed, bytecode, ..
            } = *Box::from_raw(compiled);
            let CompiledProgramBytecode::Program(executable) = bytecode else {
                return std::ptr::null_mut();
            };

            let shared_function_data_context = ffi::SharedFunctionDataCreationContext {
                vm_ptr,
                source_code_ptr,
                owner: ffi::SharedFunctionDataOwner::List(shared_function_data_list_ptr),
            };
            let exec_ptr =
                ffi::create_executable(executable, vm_ptr, source_code_ptr, shared_function_data_context.owner);
            if exec_ptr.is_null() {
                return std::ptr::null_mut();
            }

            let declarations = collect_script_declarations(
                &parsed.arena.scopes[parsed.scope_ref],
                &mut parsed.function_table,
                &parsed.arena,
            );
            push_script_declarations(
                declarations,
                parsed.is_strict_mode,
                shared_function_data_context,
                gdi_context,
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
            let context = crate::compile::EvalContext {
                starts_in_strict_mode,
                in_eval_function_context,
                allow_super_property_lookup,
                allow_super_constructor_call,
                in_class_field_initializer,
            };
            let parsed = match crate::compile::parse_eval(source_slice, context) {
                Ok(parsed) => parsed,
                Err(errors) => {
                    if let Some(callback) = error_callback {
                        for error in &errors {
                            report_parse_error(callback, error_context, &error.message, error.line, error.column);
                        }
                    }
                    return std::ptr::null_mut();
                }
            };

            write_ast_dump_output(
                &parsed.program,
                &parsed.function_table,
                &parsed.arena,
                ast_dump_output,
                ast_dump_output_len,
            );

            let crate::compile::ParsedEval {
                program,
                function_table,
                arena: arena_arc,
                scope_id,
                is_strict,
                eval_referenced_private_names,
            } = parsed;
            let mut generator = new_program_generator(is_strict, source_len);
            generator.function_table = function_table;
            generator.arena = arena_arc.clone();
            let (exec_ptr, mut function_table) = compile_program_body(
                generator,
                &program,
                scope_id,
                vm_ptr,
                source_code_ptr,
                ffi::SharedFunctionDataOwner::None,
            );
            if exec_ptr.is_null() {
                return std::ptr::null_mut();
            }

            let declarations = collect_eval_declarations(
                &arena_arc.scopes[scope_id],
                is_strict,
                &mut function_table,
                &arena_arc,
                eval_referenced_private_names,
            );
            push_eval_declarations(declarations, vm_ptr, source_code_ptr, gdi_context);

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

            let report_errors = |errors: &[crate::parser::ParseError]| {
                if let Some(callback) = error_callback {
                    for error in errors {
                        report_parse_error(callback, error_context, &error.message, error.line, error.column);
                    }
                }
            };
            let (Some(parameters_slice), Some(body_slice), Some(full_slice)) = (
                source_from_raw(parameters_source, parameters_source_len),
                source_from_raw(body_source, body_source_len),
                source_from_raw(full_source, full_source_len),
            ) else {
                return std::ptr::null_mut();
            };
            let parsed = match crate::compile::parse_dynamic_function(full_slice, parameters_slice, body_slice, kind) {
                Ok(parsed) => parsed,
                Err(errors) => {
                    report_errors(&errors);
                    return std::ptr::null_mut();
                }
            };

            write_ast_dump_output(
                &parsed.program,
                &parsed.function_table,
                &parsed.arena,
                ast_dump_output,
                ast_dump_output_len,
            );

            let description = match parsed.into_description() {
                Ok(description) => description,
                Err(errors) => {
                    report_errors(&errors);
                    return std::ptr::null_mut();
                }
            };
            ffi::create_shared_function_data_from_description(
                description,
                ffi::SharedFunctionDataCreationContext {
                    vm_ptr,
                    source_code_ptr,
                    owner: ffi::SharedFunctionDataOwner::None,
                },
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

            let mut parsed = crate::compile::parse_builtin_file(source_slice);

            write_ast_dump_output(
                &parsed.program,
                &parsed.function_table,
                &parsed.arena,
                ast_dump_output,
                ast_dump_output_len,
            );

            let context = ffi::SharedFunctionDataCreationContext {
                vm_ptr,
                source_code_ptr,
                owner: ffi::SharedFunctionDataOwner::None,
            };
            for description in crate::compile::describe_builtin_file_functions(&mut parsed) {
                let name = description.name.clone();
                let sfd_ptr = ffi::create_shared_function_data_from_description(description, context);
                if !sfd_ptr.is_null() {
                    push_function(ctx, sfd_ptr, name.as_ptr(), name.len());
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

            // 1. Hand C++ the module's records.
            let declarations = crate::compile::collect_module_declarations(
                &parsed.arena.scopes[parsed.scope_ref],
                parsed.has_top_level_await,
                &mut parsed.function_table,
                &parsed.arena,
            );
            push_module_declarations(declarations, shared_function_data_context, module_context, cb);

            // 2. Compile module body.
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
                let mut generator = new_program_generator(true, source_len);
                generator.function_table = std::mem::take(&mut parsed.function_table);
                generator.arena = parsed.arena.clone();
                let (exec_ptr, _) = compile_program_body(
                    generator,
                    &parsed.program,
                    parsed.scope_ref,
                    vm_ptr,
                    source_code_ptr,
                    shared_function_data_context.owner,
                );
                exec_ptr
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

            let CompiledProgram {
                mut parsed, bytecode, ..
            } = *Box::from_raw(compiled);
            let cb = &*callbacks;
            let shared_function_data_context = ffi::SharedFunctionDataCreationContext {
                vm_ptr,
                source_code_ptr,
                owner: ffi::SharedFunctionDataOwner::List(shared_function_data_list_ptr),
            };

            let declarations = crate::compile::collect_module_declarations(
                &parsed.arena.scopes[parsed.scope_ref],
                parsed.has_top_level_await,
                &mut parsed.function_table,
                &parsed.arena,
            );
            push_module_declarations(declarations, shared_function_data_context, module_context, cb);

            match bytecode {
                CompiledProgramBytecode::AsyncModule(executable) => {
                    let exec_ptr =
                        ffi::create_executable(executable, vm_ptr, source_code_ptr, shared_function_data_context.owner);
                    if !tla_executable_out.is_null() {
                        *tla_executable_out = exec_ptr;
                    }
                    std::ptr::null_mut()
                }
                CompiledProgramBytecode::Program(executable) => {
                    if !tla_executable_out.is_null() {
                        *tla_executable_out = std::ptr::null_mut();
                    }
                    ffi::create_executable(executable, vm_ptr, source_code_ptr, shared_function_data_context.owner)
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

/// Hands C++ what ParseModule records of a module, through the callbacks of its ModuleBuilder, creating the
/// SharedFunctionInstanceData of each function to initialize as it goes.
unsafe fn push_module_declarations(
    declarations: crate::compile::ModuleDeclarations,
    shared_function_data_context: ffi::SharedFunctionDataCreationContext,
    ctx: *mut c_void,
    cb: &ModuleCallbacks,
) {
    unsafe {
        (cb.set_has_top_level_await)(ctx, declarations.has_top_level_await);

        for entry in &declarations.import_entries {
            let (import_name, import_name_len, is_namespace) = entry
                .import_name
                .as_ref()
                .map_or((std::ptr::null(), 0, true), |name| (name.as_ptr(), name.len(), false));
            let (keys, values) = build_attribute_slices(&entry.module_request.attributes);
            (cb.push_import_entry)(
                ctx,
                import_name,
                import_name_len,
                is_namespace,
                entry.local_name.as_ptr(),
                entry.local_name.len(),
                entry.module_request.module_specifier.as_ptr(),
                entry.module_request.module_specifier.len(),
                keys.as_ptr(),
                values.as_ptr(),
                keys.len(),
            );
        }

        if let Some(name) = &declarations.default_export_binding_name {
            (cb.set_default_export_binding)(ctx, name.as_ptr(), name.len());
        }

        for (callback, entries) in [
            (cb.push_local_export, &declarations.local_export_entries),
            (cb.push_indirect_export, &declarations.indirect_export_entries),
            (cb.push_star_export, &declarations.star_export_entries),
        ] {
            for entry in entries {
                call_export_callback(
                    callback,
                    ctx,
                    entry.kind as u8,
                    entry.export_name.as_ref(),
                    entry.local_or_import_name.as_ref(),
                    entry.module_request.as_ref(),
                );
            }
        }

        for name in &declarations.var_names {
            (cb.push_var_name)(ctx, name.as_ptr(), name.len());
        }

        for function in declarations.functions_to_initialize {
            let sfd_ptr =
                ffi::create_shared_function_data_from_description(function.description, shared_function_data_context);
            if function.is_anonymous_default_export {
                module_sfd_set_name(sfd_ptr, function.name.as_ptr(), function.name.len());
            }
            (cb.push_function)(ctx, sfd_ptr, function.name.as_ptr(), function.name.len());
        }

        for binding in &declarations.lexical_bindings {
            let function_index = binding.function_index.map_or(-1, |index| {
                i32::try_from(index).expect("the function index fits in i32")
            });
            (cb.push_lexical_binding)(
                ctx,
                binding.name.as_ptr(),
                binding.name.len(),
                binding.is_constant,
                function_index,
            );
        }

        for module_request in &declarations.requested_modules {
            let (keys, values) = build_attribute_slices(&module_request.attributes);
            (cb.push_requested_module)(
                ctx,
                module_request.module_specifier.as_ptr(),
                module_request.module_specifier.len(),
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

        let assembled = compile_module_as_async_to_bytecode(program, scope_id, &mut generator);
        ffi::create_executable(
            ExecutableData::new(generator, assembled),
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
// GDI/EDI metadata
// =============================================================================

/// Create the SharedFunctionInstanceData of a function that declaration
/// instantiation initializes.
unsafe fn create_sfd_for_function_to_initialize(
    shared_function_data: PendingSharedFunctionData,
    shared_function_data_context: ffi::SharedFunctionDataCreationContext,
    is_strict: bool,
) -> *mut c_void {
    let sfd_ptr = unsafe {
        ffi::create_sfd_for_gdi(
            shared_function_data
                .function_data
                .expect("function to initialize is missing its function data"),
            shared_function_data
                .subtable
                .expect("function to initialize is missing its function table"),
            shared_function_data_context,
            is_strict,
            shared_function_data
                .arena
                .expect("function to initialize is missing its AST arena"),
        )
    };
    assert!(!sfd_ptr.is_null(), "create_sfd_for_gdi returned null");
    sfd_ptr
}

/// Populate the C++ EvalGdiBuilder from collected EDI metadata, creating each
/// function's SharedFunctionInstanceData just before pushing it.
unsafe fn push_eval_declarations(
    declarations: EvalDeclarations,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    ctx: *mut c_void,
) {
    unsafe {
        use ffi::eval_gdi_push_annex_b_name;
        use ffi::eval_gdi_push_function;
        use ffi::eval_gdi_push_lexical_binding;
        use ffi::eval_gdi_push_private_name;
        use ffi::eval_gdi_push_var_name;
        use ffi::eval_gdi_push_var_scoped_name;
        use ffi::eval_gdi_set_strict;

        eval_gdi_set_strict(ctx, declarations.is_strict);

        for name in &declarations.var_names {
            eval_gdi_push_var_name(ctx, name.as_ptr(), name.len());
        }
        for function in declarations.functions_to_initialize {
            let sfd_ptr = create_sfd_for_function_to_initialize(
                function.shared_function_data,
                ffi::SharedFunctionDataCreationContext {
                    vm_ptr,
                    source_code_ptr,
                    owner: ffi::SharedFunctionDataOwner::None,
                },
                declarations.is_strict,
            );
            eval_gdi_push_function(ctx, sfd_ptr, function.name.as_ptr(), function.name.len());
        }
        for name in &declarations.var_scoped_names {
            eval_gdi_push_var_scoped_name(ctx, name.as_ptr(), name.len());
        }
        for name in &declarations.annex_b_candidate_names {
            eval_gdi_push_annex_b_name(ctx, name.as_ptr(), name.len());
        }
        for binding in &declarations.lexical_bindings {
            eval_gdi_push_lexical_binding(ctx, binding.name.as_ptr(), binding.name.len(), binding.is_constant);
        }

        for name in &declarations.private_names {
            eval_gdi_push_private_name(ctx, name.as_ptr(), name.len());
        }
    }
}

/// Populate the C++ ScriptGdiBuilder from collected GDI metadata, creating
/// each function's SharedFunctionInstanceData just before pushing it.
unsafe fn push_script_declarations(
    declarations: ScriptDeclarations,
    is_strict: bool,
    shared_function_data_context: ffi::SharedFunctionDataCreationContext,
    ctx: *mut c_void,
) {
    unsafe {
        use ffi::script_gdi_push_annex_b_name;
        use ffi::script_gdi_push_function;
        use ffi::script_gdi_push_lexical_binding;
        use ffi::script_gdi_push_lexical_name;
        use ffi::script_gdi_push_var_name;
        use ffi::script_gdi_push_var_scoped_name;

        for name in &declarations.lexical_names {
            script_gdi_push_lexical_name(ctx, name.as_ptr(), name.len());
        }
        for name in &declarations.var_names {
            script_gdi_push_var_name(ctx, name.as_ptr(), name.len());
        }
        for function in declarations.functions_to_initialize {
            let sfd_ptr = create_sfd_for_function_to_initialize(
                function.shared_function_data,
                shared_function_data_context,
                is_strict,
            );
            script_gdi_push_function(ctx, sfd_ptr, function.name.as_ptr(), function.name.len());
        }
        for name in &declarations.var_scoped_names {
            script_gdi_push_var_scoped_name(ctx, name.as_ptr(), name.len());
        }
        for name in &declarations.annex_b_candidate_names {
            script_gdi_push_annex_b_name(ctx, name.as_ptr(), name.len());
        }
        for binding in &declarations.lexical_bindings {
            script_gdi_push_lexical_binding(ctx, binding.name.as_ptr(), binding.name.len(), binding.is_constant);
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
    shared_function_data_list_ptr: *mut c_void,
) -> *mut c_void {
    unsafe {
        abort_on_panic(|| {
            if rust_function_ast.is_null() {
                return std::ptr::null_mut();
            }
            let payload = Box::from_raw(rust_function_ast as *mut ast::FunctionPayload);
            let precompiled =
                crate::compile::compile_function(payload, source_len, builtin_abstract_operations_enabled);

            write_sfd_metadata(sfd_ptr, &precompiled.metadata);

            ffi::create_executable(
                precompiled.executable,
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
