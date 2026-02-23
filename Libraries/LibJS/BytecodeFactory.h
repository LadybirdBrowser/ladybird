/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef ENABLE_RUST

#    include <stddef.h>
#    include <stdint.h>

// FFI types for creating a Bytecode::Executable from Rust.
//
// The Rust bytecode generator assembles instructions into a byte buffer
// matching C++ layout. This FFI layer creates the C++ Executable from
// that data.

// Constant value tags (matches Rust ConstantValue enum discriminants)
#    define CONSTANT_TAG_NUMBER 0
#    define CONSTANT_TAG_BOOLEAN_TRUE 1
#    define CONSTANT_TAG_BOOLEAN_FALSE 2
#    define CONSTANT_TAG_NULL 3
#    define CONSTANT_TAG_UNDEFINED 4
#    define CONSTANT_TAG_EMPTY 5
#    define CONSTANT_TAG_STRING 6
#    define CONSTANT_TAG_BIGINT 7
#    define CONSTANT_TAG_RAW_VALUE 8

struct FFIExceptionHandler {
    uint32_t start_offset;
    uint32_t end_offset;
    uint32_t handler_offset;
};

struct FFISourceMapEntry {
    uint32_t bytecode_offset;
    uint32_t source_start;
    uint32_t source_end;
};

// A UTF-16 string slice (pointer + length).
struct FFIUtf16Slice {
    uint16_t const* data;
    size_t length;
};

// An optional uint32_t for FFI (replaces -1 sentinel values).
struct FFIOptionalU32 {
    uint32_t value;
    bool has_value;
};

// Class element descriptor for FFI (matches ClassElementDescriptor::Kind).
// Kind values: 0=Method, 1=Getter, 2=Setter, 3=Field, 4=StaticInitializer
struct FFIClassElement {
    uint8_t kind;
    bool is_static;
    bool is_private;
    uint16_t const* private_identifier;
    size_t private_identifier_len;
    FFIOptionalU32 shared_function_data_index;
    bool has_initializer;
    uint8_t literal_value_kind; // 0=none, 1=number, 2=boolean_true, 3=boolean_false, 4=null, 5=string
    double literal_value_number;
    uint16_t const* literal_value_string;
    size_t literal_value_string_len;
};

#    ifdef __cplusplus
extern "C" {
#    endif

// Callback for reporting parse errors from Rust.
// message is UTF-8, line and column are 1-based.
typedef void (*RustParseErrorCallback)(void* ctx, char const* message, size_t message_len, uint32_t line, uint32_t column);

// Parse, compile, and extract GDI metadata for a script using the Rust
// parser. Populates gdi_context (a ScriptGdiBuilder*) via callbacks.
// On parse failure, calls error_callback for each error, then returns nullptr.
// Returns a Bytecode::Executable* cast to void*, or nullptr on failure.
void* rust_compile_script(
    uint16_t const* source,
    size_t source_len,
    void* vm_ptr,
    void const* source_code_ptr,
    void* gdi_context,
    bool dump_ast,
    bool use_color,
    void* error_context,
    RustParseErrorCallback error_callback,
    uint8_t** ast_dump_output,
    size_t* ast_dump_output_len,
    size_t initial_line_number);

// Parse and compile a JavaScript program using the Rust parser and
// bytecode generator. Returns a Bytecode::Executable* cast to void*,
// or nullptr on failure.
void* rust_compile_program(
    uint16_t const* source,
    size_t source_len,
    void* vm_ptr,
    void const* source_code_ptr,
    uint8_t program_type,
    bool starts_in_strict_mode,
    bool initiated_by_eval,
    bool in_eval_function_context,
    bool allow_super_property_lookup,
    bool allow_super_constructor_call,
    bool in_class_field_initializer);

// All the data needed to create a Bytecode::Executable from Rust.
struct FFIExecutableData {
    // Bytecode
    uint8_t const* bytecode;
    size_t bytecode_length;
    // Tables: arrays of UTF-16 string slices
    FFIUtf16Slice const* identifier_table;
    size_t identifier_count;
    FFIUtf16Slice const* property_key_table;
    size_t property_key_count;
    FFIUtf16Slice const* string_table;
    size_t string_count;
    // Constants: tagged byte array
    uint8_t const* constants_data;
    size_t constants_data_length;
    size_t constants_count;
    // Exception handlers
    FFIExceptionHandler const* exception_handlers;
    size_t exception_handler_count;
    // Source map
    FFISourceMapEntry const* source_map;
    size_t source_map_count;
    // Basic block start offsets
    size_t const* basic_block_offsets;
    size_t basic_block_count;
    // Local variable names
    FFIUtf16Slice const* local_variable_names;
    size_t local_variable_count;
    // Cache counts
    uint32_t property_lookup_cache_count;
    uint32_t global_variable_cache_count;
    uint32_t template_object_cache_count;
    uint32_t object_shape_cache_count;
    // Register and mode
    uint32_t number_of_registers;
    bool is_strict;
    // Length identifier: PropertyKeyTableIndex for "length"
    FFIOptionalU32 length_identifier;
    // Shared function data (inner functions)
    void const* const* shared_function_data;
    size_t shared_function_data_count;
    // Class blueprints (heap-allocated, ownership transfers)
    void* const* class_blueprints;
    size_t class_blueprint_count;
    // Regex table (pre-compiled, ownership transfers)
    void* const* compiled_regexes;
    size_t regex_count;
};

// Create a C++ Bytecode::Executable from assembled Rust bytecode data.
//
// The source_code parameter is a SourceCode const* cast to void*.
// Returns a GC::Ptr<Executable> cast to void*, or nullptr on failure.
void* rust_create_executable(
    void* vm_ptr,
    void* source_code_ptr,
    FFIExecutableData const* data);

// All the data needed to create a SharedFunctionInstanceData from Rust.
struct FFISharedFunctionData {
    // Function name (UTF-16)
    uint16_t const* name;
    size_t name_len;
    // Metadata
    uint8_t function_kind;
    int32_t function_length;
    uint32_t formal_parameter_count;
    bool strict;
    bool is_arrow;
    bool has_simple_parameter_list;
    // Parameter names for mapped arguments (only for simple parameter lists)
    FFIUtf16Slice const* parameter_names;
    size_t parameter_name_count;
    // Source text range (for Function.prototype.toString)
    size_t source_text_offset;
    size_t source_text_length;
    // Opaque Rust AST pointer (Box<FunctionData>)
    void* rust_function_ast;
    // Parsing insights needed before lazy compilation
    bool uses_this;
    bool uses_this_from_environment;
};

// Create a SharedFunctionInstanceData from pre-computed metadata (Rust pipeline).
// Stores an opaque Rust AST pointer for lazy compilation.
//
// Returns a SharedFunctionInstanceData* cast to void*.
void* rust_create_sfd(
    void* vm_ptr,
    void const* source_code_ptr,
    FFISharedFunctionData const* data);

// Set class_field_initializer_name on a SharedFunctionInstanceData.
// Called after rust_create_sfd for class field initializer functions.
void rust_sfd_set_class_field_initializer_name(
    void* sfd_ptr,
    uint16_t const* name,
    size_t name_len,
    bool is_private);

// Compile a function body using the Rust pipeline.
// Takes ownership of the Rust AST (frees it after compilation).
//
// Writes FDI runtime metadata to the SFD via the sfd_ptr parameter.
// Returns a Bytecode::Executable* cast to void*, or nullptr on failure.
void* rust_compile_function(
    void* vm_ptr,
    void const* source_code_ptr,
    uint16_t const* source,
    size_t source_len,
    void* sfd_ptr,
    void* rust_function_ast,
    bool builtin_abstract_operations_enabled);

// Free a Rust Box<FunctionData> (called from SFD destructor).
void rust_free_function_ast(void* ast);

// Set FDI runtime metadata on a SharedFunctionInstanceData.
// Called from Rust after compiling a function body.
void rust_sfd_set_metadata(
    void* sfd_ptr,
    bool uses_this,
    bool function_environment_needed,
    size_t function_environment_bindings_count,
    bool might_need_arguments_object,
    bool contains_direct_call_to_eval);

// Create a ClassBlueprint on the heap. Ownership transfers to the
// caller; pass the pointer to rust_create_executable which will move
// the blueprint into the Executable.
//
// Returns a heap-allocated ClassBlueprint* cast to void*.
void* rust_create_class_blueprint(
    // VM pointer for creating GC objects (e.g. PrimitiveString)
    void* vm_ptr,
    // Source code object for substring_view
    void const* source_code_ptr,
    // Class name (empty for anonymous)
    uint16_t const* name,
    size_t name_len,
    // Source text of the entire class (for Function.prototype.toString)
    size_t source_text_offset,
    size_t source_text_len,
    // Index into shared_function_data for the constructor
    uint32_t constructor_sfd_index,
    bool has_super_class,
    bool has_name,
    // Array of class element descriptors
    FFIClassElement const* elements,
    size_t element_count);

// Callbacks used by rust_compile_script to populate GDI metadata.
void script_gdi_push_lexical_name(void* ctx, uint16_t const* name, size_t len);
void script_gdi_push_var_name(void* ctx, uint16_t const* name, size_t len);
void script_gdi_push_function(void* ctx, void* sfd_ptr, uint16_t const* name, size_t len);
void script_gdi_push_var_scoped_name(void* ctx, uint16_t const* name, size_t len);
void script_gdi_push_annex_b_name(void* ctx, uint16_t const* name, size_t len);
void script_gdi_push_lexical_binding(void* ctx, uint16_t const* name, size_t len, bool is_constant);

// Parse, compile, and extract EDI metadata for eval using the Rust
// parser. Populates gdi_context (an EvalGdiBuilder*) via callbacks.
// Returns a Bytecode::Executable* cast to void*, or nullptr on failure.
void* rust_compile_eval(
    uint16_t const* source,
    size_t source_len,
    void* vm_ptr,
    void const* source_code_ptr,
    void* gdi_context,
    bool starts_in_strict_mode,
    bool in_eval_function_context,
    bool allow_super_property_lookup,
    bool allow_super_constructor_call,
    bool in_class_field_initializer,
    void* error_context,
    RustParseErrorCallback error_callback,
    uint8_t** ast_dump_output,
    size_t* ast_dump_output_len);

// Parse and compile a dynamically-created function (new Function()).
// Validates parameters and body separately per spec, then parses the
// full synthetic source to create a SharedFunctionInstanceData.
//
// Returns a SharedFunctionInstanceData* cast to void*, or nullptr on
// parse failure (caller should throw SyntaxError).
//
// function_kind: 0=Normal, 1=Generator, 2=Async, 3=AsyncGenerator
void* rust_compile_dynamic_function(
    uint16_t const* full_source,
    size_t full_source_len,
    uint16_t const* params_source,
    size_t params_source_len,
    uint16_t const* body_source,
    size_t body_source_len,
    void* vm_ptr,
    void const* source_code_ptr,
    uint8_t function_kind,
    void* error_context,
    RustParseErrorCallback error_callback,
    uint8_t** ast_dump_output,
    size_t* ast_dump_output_len);

// Callbacks used by rust_compile_eval to populate EDI metadata.
void eval_gdi_set_strict(void* ctx, bool is_strict);
void eval_gdi_push_var_name(void* ctx, uint16_t const* name, size_t len);
void eval_gdi_push_function(void* ctx, void* sfd, uint16_t const* name, size_t len);
void eval_gdi_push_var_scoped_name(void* ctx, uint16_t const* name, size_t len);
void eval_gdi_push_annex_b_name(void* ctx, uint16_t const* name, size_t len);
void eval_gdi_push_lexical_binding(void* ctx, uint16_t const* name, size_t len, bool is_constant);

// Parse a builtin JS file in strict mode, extract top-level function
// declarations, and create SharedFunctionInstanceData for each via the
// Rust pipeline. Calls push_function for each function found.
typedef void (*RustBuiltinFunctionCallback)(void* ctx, void* sfd_ptr, uint16_t const* name, size_t name_len);
void rust_compile_builtin_file(
    uint16_t const* source,
    size_t source_len,
    void* vm_ptr,
    void const* source_code_ptr,
    void* ctx,
    RustBuiltinFunctionCallback push_function,
    uint8_t** ast_dump_output,
    size_t* ast_dump_output_len);

// Module compilation callback table (matches Rust ModuleCallbacks struct).
struct ModuleCallbacks {
    void (*set_has_top_level_await)(void* ctx, bool value);
    void (*push_import_entry)(
        void* ctx,
        uint16_t const* import_name,
        size_t import_name_len,
        bool is_namespace,
        uint16_t const* local_name,
        size_t local_name_len,
        uint16_t const* module_specifier,
        size_t specifier_len,
        FFIUtf16Slice const* attribute_keys,
        FFIUtf16Slice const* attribute_values,
        size_t attribute_count);
    void (*push_local_export)(
        void* ctx,
        uint8_t kind,
        uint16_t const* export_name,
        size_t export_name_len,
        uint16_t const* local_or_import_name,
        size_t local_or_import_name_len,
        uint16_t const* module_specifier,
        size_t specifier_len,
        FFIUtf16Slice const* attribute_keys,
        FFIUtf16Slice const* attribute_values,
        size_t attribute_count);
    void (*push_indirect_export)(
        void* ctx,
        uint8_t kind,
        uint16_t const* export_name,
        size_t export_name_len,
        uint16_t const* local_or_import_name,
        size_t local_or_import_name_len,
        uint16_t const* module_specifier,
        size_t specifier_len,
        FFIUtf16Slice const* attribute_keys,
        FFIUtf16Slice const* attribute_values,
        size_t attribute_count);
    void (*push_star_export)(
        void* ctx,
        uint8_t kind,
        uint16_t const* export_name,
        size_t export_name_len,
        uint16_t const* local_or_import_name,
        size_t local_or_import_name_len,
        uint16_t const* module_specifier,
        size_t specifier_len,
        FFIUtf16Slice const* attribute_keys,
        FFIUtf16Slice const* attribute_values,
        size_t attribute_count);
    void (*push_requested_module)(
        void* ctx,
        uint16_t const* specifier,
        size_t specifier_len,
        FFIUtf16Slice const* attribute_keys,
        FFIUtf16Slice const* attribute_values,
        size_t attribute_count);
    void (*set_default_export_binding)(void* ctx, uint16_t const* name, size_t name_len);
    void (*push_var_name)(void* ctx, uint16_t const* name, size_t name_len);
    void (*push_function)(void* ctx, void* sfd_ptr, uint16_t const* name, size_t name_len);
    void (*push_lexical_binding)(void* ctx, uint16_t const* name, size_t name_len, bool is_constant, int32_t function_index);
};

// Parse, compile, and extract module metadata using the Rust parser.
// Populates module_context (a ModuleBuilder*) via callbacks.
// On parse failure, calls error_callback for each error, then returns nullptr.
//
// Returns Executable* for non-TLA modules (tla_executable_out is null).
// For TLA modules, returns nullptr and sets tla_executable_out to the
// async wrapper Executable*.
void* rust_compile_module(
    uint16_t const* source,
    size_t source_len,
    void* vm_ptr,
    void const* source_code_ptr,
    void* module_context,
    ModuleCallbacks const* callbacks,
    bool dump_ast,
    bool use_color,
    void* error_context,
    RustParseErrorCallback error_callback,
    void** tla_executable_out,
    uint8_t** ast_dump_output,
    size_t* ast_dump_output_len);

// Set the name on a SharedFunctionInstanceData (used for module default
// export renaming from "*default*" to "default").
void module_sfd_set_name(void* sfd_ptr, uint16_t const* name, size_t name_len);

// Compile a regex pattern+flags. On success, returns a heap-allocated
// opaque object (RustCompiledRegex*) and sets *error_out to nullptr.
// On failure, returns nullptr and sets *error_out to a heap-allocated
// error string (caller must free with rust_free_error_string).
// Successful results must be freed with rust_free_compiled_regex or
// passed to rust_create_executable (which takes ownership).
void* rust_compile_regex(
    uint16_t const* pattern_data, size_t pattern_len, uint16_t const* flags_data, size_t flags_len, char const** error_out);
void rust_free_compiled_regex(void* ptr);
void rust_free_error_string(char const* str);

// Convert a JS number to its UTF-16 string representation using the
// ECMA-262 Number::toString algorithm. Writes up to buffer_len code
// units into buffer and returns the actual length.
size_t rust_number_to_utf16(double value, uint16_t* buffer, size_t buffer_len);

// FIXME: This FFI workaround exists only to match C++ float-to-string
//        formatting in the Rust AST dump. Once the C++ pipeline is
//        removed, this can be deleted and the Rust side can use its own
//        formatting without needing to match C++.
// Format a double using AK's shortest-representation algorithm.
// Writes up to buffer_len bytes into buffer and returns the actual length.
size_t rust_format_double(double value, uint8_t* buffer, size_t buffer_len);

// Get a well-known symbol as an encoded JS::Value.
// symbol_id: 0 = Symbol.iterator, 1 = Symbol.asyncIterator
uint64_t get_well_known_symbol(void* vm_ptr, uint32_t symbol_id);

// Get an intrinsic abstract operation function as an encoded JS::Value.
uint64_t get_abstract_operation_function(void* vm_ptr, uint16_t const* name, size_t name_len);

// Free a string allocated by Rust (e.g. AST dump output).
void rust_free_string(uint8_t* ptr, size_t len);

#    ifdef __cplusplus
}
#    endif

#endif // ENABLE_RUST
