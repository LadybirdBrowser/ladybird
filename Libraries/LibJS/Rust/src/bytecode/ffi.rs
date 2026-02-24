/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! FFI bridge between the codegen and C++ runtime.
//!
//! This module handles the boundary between the bytecode generator
//! and the C++ `Bytecode::Executable` / `SharedFunctionInstanceData` types.
//!
//! ## Key operations
//!
//! - `create_executable()` -- packages assembled bytecode, tables, and
//!   metadata into a C++ `Bytecode::Executable` via `rust_create_executable()`
//! - `create_shared_function_data()` -- creates a C++ `SharedFunctionInstanceData`
//!   for a parsed function, transferring ownership of the AST
//! - `compile_regex()` -- delegates regex compilation to the C++ regex engine
//!
//! ## FFI types
//!
//! All `FFI*` structs are `#[repr(C)]` and must match their counterparts
//! in `BytecodeFactory.h`. Changes to field order or types here require
//! corresponding changes on the C++ side.

use std::ffi::c_void;

use super::generator::{AssembledBytecode, ConstantValue, Generator};
use crate::ast::Utf16String;
use crate::u32_from_usize;

/// Opaque pointer returned from rust_create_executable.
pub type ExecutableHandle = *mut c_void;

// FFI types matching BytecodeFactory.h.

/// Exception handler range (C++ `BytecodeFactory::ExceptionHandlerData`).
#[repr(C)]
pub struct FFIExceptionHandler {
    pub start_offset: u32,
    pub end_offset: u32,
    pub handler_offset: u32,
}

/// Source map entry mapping bytecode offset to source range.
#[repr(C)]
pub struct FFISourceMapEntry {
    pub bytecode_offset: u32,
    pub source_start: u32,
    pub source_end: u32,
}

/// A borrowed UTF-16 string slice for passing across FFI.
/// Points into Rust-owned memory; valid only for the duration of the FFI call.
#[repr(C)]
pub struct FFIUtf16Slice {
    pub data: *const u16,
    pub length: usize,
}

impl From<&[u16]> for FFIUtf16Slice {
    fn from(slice: &[u16]) -> Self {
        Self {
            data: slice.as_ptr(),
            length: slice.len(),
        }
    }
}

impl From<&Utf16String> for FFIUtf16Slice {
    fn from(s: &Utf16String) -> Self {
        Self {
            data: s.as_ptr(),
            length: s.len(),
        }
    }
}

/// C-compatible `Optional<u32>` (C++ doesn't have a standard Optional ABI).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FFIOptionalU32 {
    pub value: u32,
    pub has_value: bool,
}

impl FFIOptionalU32 {
    pub fn none() -> Self {
        Self {
            value: 0,
            has_value: false,
        }
    }

    pub fn some(value: u32) -> Self {
        Self {
            value,
            has_value: true,
        }
    }
}

impl From<Option<u32>> for FFIOptionalU32 {
    fn from(opt: Option<u32>) -> Self {
        match opt {
            Some(value) => Self::some(value),
            None => Self::none(),
        }
    }
}

/// Class element descriptor for ClassBlueprint creation
/// (C++ `BytecodeFactory::ClassElementData`).
#[repr(C)]
pub struct FFIClassElement {
    pub kind: u8, // ClassElementKind
    pub is_static: bool,
    pub is_private: bool,
    pub private_identifier: *const u16,
    pub private_identifier_len: usize,
    pub shared_function_data_index: FFIOptionalU32,
    pub has_initializer: bool,
    pub literal_value_kind: u8, // LiteralValueKind
    pub literal_value_number: f64,
    pub literal_value_string: *const u16,
    pub literal_value_string_len: usize,
}

/// Data for creating a C++ `SharedFunctionInstanceData`.
/// Passed to `rust_create_sfd()`.
#[repr(C)]
pub struct FFISharedFunctionData {
    pub name: *const u16,
    pub name_len: usize,
    pub function_kind: u8,
    pub function_length: i32,
    pub formal_parameter_count: u32,
    pub strict: bool,
    pub is_arrow: bool,
    pub has_simple_parameter_list: bool,
    pub parameter_names: *const FFIUtf16Slice,
    pub parameter_name_count: usize,
    pub source_text_offset: usize,
    pub source_text_length: usize,
    pub rust_function_ast: *mut c_void,
    pub uses_this: bool,
    pub uses_this_from_environment: bool,
}

/// All data needed to create a C++ `Bytecode::Executable`.
/// Passed to `rust_create_executable()`.
#[repr(C)]
pub struct FFIExecutableData {
    pub bytecode: *const u8,
    pub bytecode_length: usize,
    pub identifier_table: *const FFIUtf16Slice,
    pub identifier_count: usize,
    pub property_key_table: *const FFIUtf16Slice,
    pub property_key_count: usize,
    pub string_table: *const FFIUtf16Slice,
    pub string_count: usize,
    pub constants_data: *const u8,
    pub constants_data_length: usize,
    pub constants_count: usize,
    pub exception_handlers: *const FFIExceptionHandler,
    pub exception_handler_count: usize,
    pub source_map: *const FFISourceMapEntry,
    pub source_map_count: usize,
    pub basic_block_offsets: *const usize,
    pub basic_block_count: usize,
    pub local_variable_names: *const FFIUtf16Slice,
    pub local_variable_count: usize,
    pub property_lookup_cache_count: u32,
    pub global_variable_cache_count: u32,
    pub template_object_cache_count: u32,
    pub object_shape_cache_count: u32,
    pub number_of_registers: u32,
    pub is_strict: bool,
    pub length_identifier: FFIOptionalU32,
    pub shared_function_data: *const *const c_void,
    pub shared_function_data_count: usize,
    pub class_blueprints: *const *mut c_void,
    pub class_blueprint_count: usize,
    pub compiled_regexes: *const *mut c_void,
    pub regex_count: usize,
}

extern "C" {
    fn rust_create_executable(
        vm_ptr: *mut c_void,
        source_code_ptr: *const c_void,
        data: *const FFIExecutableData,
    ) -> *mut c_void;

    pub fn rust_create_sfd(
        vm_ptr: *mut c_void,
        source_code_ptr: *const c_void,
        data: *const FFISharedFunctionData,
    ) -> *mut c_void;

    pub fn rust_sfd_set_class_field_initializer_name(
        sfd_ptr: *mut c_void,
        name: *const u16,
        name_len: usize,
        is_private: bool,
    );

    pub fn rust_create_class_blueprint(
        vm_ptr: *mut c_void,
        source_code_ptr: *const c_void,
        name: *const u16,
        name_len: usize,
        source_text_offset: usize,
        source_text_len: usize,
        constructor_sfd_index: u32,
        has_super_class: bool,
        has_name: bool,
        elements: *const FFIClassElement,
        element_count: usize,
    ) -> *mut c_void;

    // Callbacks for populating Script GDI data from Rust.
    pub fn script_gdi_push_lexical_name(ctx: *mut c_void, name: *const u16, len: usize);
    pub fn script_gdi_push_var_name(ctx: *mut c_void, name: *const u16, len: usize);
    pub fn script_gdi_push_function(
        ctx: *mut c_void,
        sfd: *mut c_void,
        name: *const u16,
        len: usize,
    );
    pub fn script_gdi_push_var_scoped_name(ctx: *mut c_void, name: *const u16, len: usize);
    pub fn script_gdi_push_annex_b_name(ctx: *mut c_void, name: *const u16, len: usize);
    pub fn script_gdi_push_lexical_binding(
        ctx: *mut c_void,
        name: *const u16,
        len: usize,
        is_constant: bool,
    );

    // Callbacks for populating eval EDI data from Rust.
    pub fn eval_gdi_set_strict(ctx: *mut c_void, is_strict: bool);
    pub fn eval_gdi_push_var_name(ctx: *mut c_void, name: *const u16, len: usize);
    pub fn eval_gdi_push_function(ctx: *mut c_void, sfd: *mut c_void, name: *const u16, len: usize);
    pub fn eval_gdi_push_var_scoped_name(ctx: *mut c_void, name: *const u16, len: usize);
    pub fn eval_gdi_push_annex_b_name(ctx: *mut c_void, name: *const u16, len: usize);
    pub fn eval_gdi_push_lexical_binding(
        ctx: *mut c_void,
        name: *const u16,
        len: usize,
        is_constant: bool,
    );

    pub fn rust_compile_regex(
        pattern_data: *const u16,
        pattern_len: usize,
        flags_data: *const u16,
        flags_len: usize,
        error_out: *mut *const std::os::raw::c_char,
    ) -> *mut c_void;

    pub fn rust_free_error_string(str: *const std::os::raw::c_char);

    pub fn rust_number_to_utf16(value: f64, buffer: *mut u16, buffer_len: usize) -> usize;

    // Get a well-known symbol as an opaque Value.
    // symbol_id: 0 = Symbol.iterator, 1 = Symbol.asyncIterator
    pub fn get_well_known_symbol(vm_ptr: *mut c_void, symbol_id: u32) -> u64;

    // Get an intrinsic abstract operation function as an opaque Value.
    // name/name_len is the function name (e.g. "GetMethod").
    pub fn get_abstract_operation_function(
        vm_ptr: *mut c_void,
        name: *const u16,
        name_len: usize,
    ) -> u64;
}

/// Create a SharedFunctionInstanceData from a FunctionData.
///
/// Computes has_simple_parameter_list, builds parameter name slices,
/// transfers ownership of the Box to C++ via `Box::into_raw`, and
/// calls `rust_create_sfd`.
///
/// Used by both `emit_new_function` in codegen.rs (for function
/// expressions/declarations) and `create_sfd_for_gdi` below (for
/// top-level GDI function initialization).
///
/// # Safety
/// `vm_ptr` and `source_code_ptr` must be valid pointers.
#[allow(clippy::boxed_local)] // Callers produce Box<FunctionData>; unboxing would copy a large struct.
pub unsafe fn create_shared_function_data(
    function_data: Box<crate::ast::FunctionData>,
    subtable: crate::ast::FunctionTable,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    is_strict: bool,
    name_override: Option<&[u16]>,
) -> *mut c_void {
    use crate::ast::FunctionParameterBinding;

    let source_start = function_data.source_text_start as usize;
    let source_end = function_data.source_text_end as usize;
    let source_text_len = source_end - source_start;

    let (name_ptr, name_len) = if let Some(name) = name_override {
        (name.as_ptr(), name.len())
    } else if let Some(ref name_ident) = function_data.name {
        (name_ident.name.as_ptr(), name_ident.name.len())
    } else {
        (std::ptr::null(), 0)
    };

    let has_simple_parameter_list = function_data.parameters.iter().all(|p| {
        !p.is_rest
            && p.default_value.is_none()
            && matches!(p.binding, FunctionParameterBinding::Identifier(_))
    });

    let parameter_name_slices: Vec<FFIUtf16Slice> = if has_simple_parameter_list {
        function_data
            .parameters
            .iter()
            .map(|p| {
                if let FunctionParameterBinding::Identifier(ref id) = p.binding {
                    FFIUtf16Slice::from(id.name.as_ref())
                } else {
                    unreachable!(
                        "has_simple_parameter_list guarantees all bindings are identifiers"
                    )
                }
            })
            .collect()
    } else {
        Vec::new()
    };

    let function_kind = function_data.kind as u8;
    let strict = function_data.is_strict_mode || is_strict;
    let function_length = function_data.function_length;
    let formal_parameter_count = u32_from_usize(function_data.parameters.len());
    let is_arrow = function_data.is_arrow_function;
    let uses_this = function_data.parsing_insights.uses_this;
    let uses_this_from_environment = function_data.parsing_insights.uses_this_from_environment;

    let payload = Box::new(crate::ast::FunctionPayload {
        data: *function_data,
        function_table: subtable,
    });
    let rust_ast_ptr = Box::into_raw(payload) as *mut c_void;

    let ffi_data = FFISharedFunctionData {
        name: name_ptr,
        name_len,
        function_kind,
        function_length,
        formal_parameter_count,
        strict,
        is_arrow,
        has_simple_parameter_list,
        parameter_names: parameter_name_slices.as_ptr(),
        parameter_name_count: parameter_name_slices.len(),
        source_text_offset: source_start,
        source_text_length: source_text_len,
        rust_function_ast: rust_ast_ptr,
        uses_this,
        uses_this_from_environment,
    };

    let sfd_ptr = rust_create_sfd(vm_ptr, source_code_ptr, &ffi_data);

    assert!(
        !sfd_ptr.is_null(),
        "create_shared_function_data: rust_create_sfd returned null"
    );
    sfd_ptr
}

/// Create a SharedFunctionInstanceData for GDI use (no name override).
///
/// # Safety
/// `vm_ptr` and `source_code_ptr` must be valid pointers.
pub unsafe fn create_sfd_for_gdi(
    function_data: Box<crate::ast::FunctionData>,
    subtable: crate::ast::FunctionTable,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    is_strict: bool,
) -> *mut c_void {
    create_shared_function_data(
        function_data,
        subtable,
        vm_ptr,
        source_code_ptr,
        is_strict,
        None,
    )
}

/// Constant tags for the FFI constant buffer (ABI-compatible with BytecodeFactory).
#[repr(u8)]
enum ConstantTag {
    Number = 0,
    BooleanTrue = 1,
    BooleanFalse = 2,
    Null = 3,
    Undefined = 4,
    Empty = 5,
    String = 6,
    BigInt = 7,
    RawValue = 8,
}

/// Encode constants into a tagged byte buffer for FFI.
fn encode_constants(constants: &[ConstantValue]) -> Vec<u8> {
    let mut buffer = Vec::new();
    for c in constants {
        match c {
            ConstantValue::Number(v) => {
                buffer.push(ConstantTag::Number as u8);
                buffer.extend_from_slice(&v.to_le_bytes());
            }
            ConstantValue::Boolean(true) => buffer.push(ConstantTag::BooleanTrue as u8),
            ConstantValue::Boolean(false) => buffer.push(ConstantTag::BooleanFalse as u8),
            ConstantValue::Null => buffer.push(ConstantTag::Null as u8),
            ConstantValue::Undefined => buffer.push(ConstantTag::Undefined as u8),
            ConstantValue::Empty => buffer.push(ConstantTag::Empty as u8),
            ConstantValue::String(s) => {
                buffer.push(ConstantTag::String as u8);
                let len = u32_from_usize(s.len());
                buffer.extend_from_slice(&len.to_le_bytes());
                for &code_unit in s {
                    buffer.extend_from_slice(&code_unit.to_le_bytes());
                }
            }
            ConstantValue::BigInt(s) => {
                buffer.push(ConstantTag::BigInt as u8);
                let len = u32_from_usize(s.len());
                buffer.extend_from_slice(&len.to_le_bytes());
                buffer.extend_from_slice(s.as_bytes());
            }
            ConstantValue::RawValue(encoded) => {
                buffer.push(ConstantTag::RawValue as u8);
                buffer.extend_from_slice(&encoded.to_le_bytes());
            }
        }
    }
    buffer
}

/// Create a C++ Executable from the generator's assembled output.
///
/// # Safety
/// `vm_ptr` must be a valid `JS::VM*` and `source_code_ptr` a valid
/// `JS::SourceCode const*`.
pub unsafe fn create_executable(
    gen: &Generator,
    assembled: &AssembledBytecode,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
) -> ExecutableHandle {
    // Build FFI slices for tables
    let ident_slices: Vec<FFIUtf16Slice> = gen
        .identifier_table
        .iter()
        .map(|s| FFIUtf16Slice::from(s.as_ref()))
        .collect();

    let property_key_slices: Vec<FFIUtf16Slice> = gen
        .property_key_table
        .iter()
        .map(|s| FFIUtf16Slice::from(s.as_ref()))
        .collect();

    let string_slices: Vec<FFIUtf16Slice> = gen
        .string_table
        .iter()
        .map(|s| FFIUtf16Slice::from(s.as_ref()))
        .collect();

    // Encode constants
    let constants_buffer = encode_constants(&gen.constants);

    // Build FFI exception handlers
    let ffi_handlers: Vec<FFIExceptionHandler> = assembled
        .exception_handlers
        .iter()
        .map(|h| FFIExceptionHandler {
            start_offset: h.start_offset,
            end_offset: h.end_offset,
            handler_offset: h.handler_offset,
        })
        .collect();

    // Build FFI source map
    let ffi_source_map: Vec<FFISourceMapEntry> = assembled
        .source_map
        .iter()
        .map(|e| FFISourceMapEntry {
            bytecode_offset: e.bytecode_offset,
            source_start: e.source_start,
            source_end: e.source_end,
        })
        .collect();

    // Build local variable name slices
    let local_var_slices: Vec<FFIUtf16Slice> = gen
        .local_variables
        .iter()
        .map(|v| FFIUtf16Slice::from(v.name.as_ref()))
        .collect();

    // Collect shared function data pointers
    let sfd_ptrs: Vec<*const c_void> = gen
        .shared_function_data
        .iter()
        .map(|ptr| *ptr as *const c_void)
        .collect();

    // Collect class blueprint pointers
    let bp_ptrs = &gen.class_blueprints;

    let ffi_data = FFIExecutableData {
        bytecode: assembled.bytecode.as_ptr(),
        bytecode_length: assembled.bytecode.len(),
        identifier_table: ident_slices.as_ptr(),
        identifier_count: ident_slices.len(),
        property_key_table: property_key_slices.as_ptr(),
        property_key_count: property_key_slices.len(),
        string_table: string_slices.as_ptr(),
        string_count: string_slices.len(),
        constants_data: constants_buffer.as_ptr(),
        constants_data_length: constants_buffer.len(),
        constants_count: gen.constants.len(),
        exception_handlers: ffi_handlers.as_ptr(),
        exception_handler_count: ffi_handlers.len(),
        source_map: ffi_source_map.as_ptr(),
        source_map_count: ffi_source_map.len(),
        basic_block_offsets: assembled.basic_block_start_offsets.as_ptr(),
        basic_block_count: assembled.basic_block_start_offsets.len(),
        local_variable_names: local_var_slices.as_ptr(),
        local_variable_count: local_var_slices.len(),
        property_lookup_cache_count: gen.next_property_lookup_cache,
        global_variable_cache_count: gen.next_global_variable_cache,
        template_object_cache_count: gen.next_template_object_cache,
        object_shape_cache_count: gen.next_object_shape_cache,
        number_of_registers: assembled.number_of_registers,
        is_strict: gen.strict,
        length_identifier: FFIOptionalU32::from(gen.length_identifier.map(|index| index.0)),
        shared_function_data: sfd_ptrs.as_ptr(),
        shared_function_data_count: sfd_ptrs.len(),
        class_blueprints: bp_ptrs.as_ptr(),
        class_blueprint_count: bp_ptrs.len(),
        compiled_regexes: gen.compiled_regexes.as_ptr(),
        regex_count: gen.compiled_regexes.len(),
    };

    rust_create_executable(vm_ptr, source_code_ptr, &ffi_data)
}

/// Convert a JS number to its UTF-16 string representation using the
/// ECMA-262 Number::toString algorithm (via C++ runtime).
pub fn js_number_to_utf16(value: f64) -> Utf16String {
    let mut buffer = [0u16; 64];
    let len = unsafe { rust_number_to_utf16(value, buffer.as_mut_ptr(), buffer.len()) };
    Utf16String(buffer[..len].to_vec())
}

/// Compile a regex pattern+flags using the C++ regex engine.
///
/// On success, returns an opaque handle to the compiled regex (a C++
/// RustCompiledRegex*). On failure, returns the error message.
pub fn compile_regex(pattern: &[u16], flags: &[u16]) -> Result<*mut c_void, String> {
    unsafe {
        let mut error: *const std::os::raw::c_char = std::ptr::null();
        let handle = rust_compile_regex(
            pattern.as_ptr(),
            pattern.len(),
            flags.as_ptr(),
            flags.len(),
            &mut error,
        );
        if error.is_null() {
            Ok(handle)
        } else {
            let msg = std::ffi::CStr::from_ptr(error)
                .to_string_lossy()
                .into_owned();
            rust_free_error_string(error);
            Err(msg)
        }
    }
}
