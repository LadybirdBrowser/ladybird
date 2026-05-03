/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Result.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/ModuleEntry.h>
#include <LibJS/ParserError.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/FunctionKind.h>
#include <LibJS/Script.h>
#include <LibJS/SourceCode.h>
#include <LibJS/SourceTextModule.h>

// Opaque parsed program handle from the Rust pipeline.
namespace JS::FFI {

struct ParsedProgram;
struct CompiledProgram;
struct CompiledFunction;
struct DecodedBytecodeCacheBlob;

}

namespace JS::RustIntegration {

enum class ProgramType : u8 {
    Script = 0,
    Module = 1,
};

// Result type for compile_script().
// NB: Uses GC::Root to prevent collection while the result is in transit
//     between compile_script() and the Script constructor.
struct ScriptResult {
    GC::Root<Bytecode::Executable> executable;
    bool is_strict_mode { false };
    Vector<Utf16FlyString> lexical_names;
    Vector<Utf16FlyString> var_names;
    struct FunctionToInitialize {
        GC::Root<SharedFunctionInstanceData> shared_data;
        Utf16FlyString name;
    };
    Vector<FunctionToInitialize> functions_to_initialize;
    HashTable<Utf16FlyString> declared_function_names;
    Vector<Utf16FlyString> var_scoped_names;
    Vector<Utf16FlyString> annex_b_candidate_names;
    Vector<Script::LexicalBinding> lexical_bindings;
};

// Result type for compile_eval().
// NB: Uses GC::Root to prevent collection while the result is in transit.
struct EvalResult {
    GC::Root<Bytecode::Executable> executable;
    bool is_strict_mode { false };
    EvalDeclarationData declaration_data;
};

// Result type for compile_module().
// NB: Uses GC::Root to prevent collection while the result is in transit.
struct ModuleResult {
    bool has_top_level_await { false };
    Vector<ModuleRequest> requested_modules;
    Vector<ImportEntry> import_entries;
    Vector<ExportEntry> local_export_entries;
    Vector<ExportEntry> indirect_export_entries;
    Vector<ExportEntry> star_export_entries;
    Optional<Utf16FlyString> default_export_binding_name;
    Vector<Utf16FlyString> var_declared_names;
    Vector<SourceTextModule::LexicalBinding> lexical_bindings;
    struct FunctionToInitialize {
        GC::Root<SharedFunctionInstanceData> shared_data;
        Utf16FlyString name;
    };
    Vector<FunctionToInitialize> functions_to_initialize;
    GC::Root<Bytecode::Executable> executable;
    GC::Root<SharedFunctionInstanceData> tla_shared_data;
};

// Check if the Rust pipeline is available for off-thread parsing.
JS_API bool rust_pipeline_available();

// Parse a program (script or module) without GC interaction. Thread-safe.
JS_API FFI::ParsedProgram* parse_program(u16 const* utf16_data, size_t length_in_code_units, ProgramType type, size_t line_number_offset = 0);

// Compile a parsed program to bytecode without touching the VM or GC. Thread-safe.
JS_API FFI::CompiledProgram* compile_parsed_program_off_thread(FFI::ParsedProgram* parsed, size_t length_in_code_units);

// Fully compile a parsed program to bytecode without touching the VM or GC. Thread-safe.
JS_API FFI::CompiledProgram* compile_parsed_program_fully_off_thread(FFI::ParsedProgram* parsed, size_t length_in_code_units);

// Check if a parsed program has errors. Does not consume the program.
JS_API bool parsed_program_has_errors(FFI::ParsedProgram const*);

// Free a parsed program without compiling it.
JS_API void free_parsed_program(FFI::ParsedProgram*);

// Free a compiled program without materializing it.
JS_API void free_compiled_program(FFI::CompiledProgram*);

// Serialize a fully compiled program into a versioned bytecode cache blob.
JS_API ByteBuffer serialize_compiled_program_for_bytecode_cache(FFI::CompiledProgram const&, ProgramType, ReadonlyBytes source_hash);

// Decode a bytecode cache blob into an owned parser-free cache handle.
JS_API FFI::DecodedBytecodeCacheBlob* decode_bytecode_cache_blob(ReadonlyBytes, ProgramType, ReadonlyBytes source_hash);

// Free a decoded bytecode cache blob.
JS_API void free_decoded_bytecode_cache_blob(FFI::DecodedBytecodeCacheBlob*);

// Compile a previously parsed script. Must be called on the main thread.
// Consumes and frees the Rust ParsedProgram.
// Returns nullopt if Rust is not available.
Optional<Result<ScriptResult, Vector<ParserError>>> compile_parsed_script(FFI::ParsedProgram* parsed, NonnullRefPtr<SourceCode const> source_code, Realm& realm);

// Materialize a previously compiled script. Must be called on the main thread.
// Consumes and frees the Rust CompiledProgram.
Optional<Result<ScriptResult, Vector<ParserError>>> materialize_compiled_script(FFI::CompiledProgram* compiled, NonnullRefPtr<SourceCode const> source_code, Realm& realm);

// Compile a script. Returns nullopt if Rust is not available.
Optional<Result<ScriptResult, Vector<ParserError>>> compile_script(StringView source_text, Realm& realm, StringView filename, size_t line_number_offset);

// Compile eval code. Returns nullopt if Rust is not available.
// On success, the executable's name is set to "eval".
Optional<Result<EvalResult, String>> compile_eval(
    PrimitiveString& code_string, VM& vm,
    CallerMode strict_caller, bool in_function, bool in_method,
    bool in_derived_constructor, bool in_class_field_initializer);

// Compile a previously parsed module. Must be called on the main thread.
// Consumes and frees the Rust ParsedProgram.
// Returns nullopt if Rust is not available.
Optional<Result<ModuleResult, Vector<ParserError>>> compile_parsed_module(FFI::ParsedProgram* parsed, NonnullRefPtr<SourceCode const> source_code, Realm& realm);

// Materialize a previously compiled module. Must be called on the main thread.
// Consumes and frees the Rust CompiledProgram.
Optional<Result<ModuleResult, Vector<ParserError>>> materialize_compiled_module(FFI::CompiledProgram* compiled, NonnullRefPtr<SourceCode const> source_code, Realm& realm);

// Compile a module. Returns nullopt if Rust is not available.
Optional<Result<ModuleResult, Vector<ParserError>>> compile_module(StringView source_text, Realm& realm, StringView filename);

// Compile a dynamic function (new Function()).
// On success, returns a SharedFunctionInstanceData with source_text set.
JS_API Optional<Result<GC::Ref<SharedFunctionInstanceData>, String>> compile_dynamic_function(
    VM& vm, StringView source_text, StringView parameters_string, StringView body_parse_string,
    FunctionKind kind);

// Compile a builtin JS file. Returns nullopt if Rust is not available.
Optional<Vector<GC::Root<SharedFunctionInstanceData>>> compile_builtin_file(
    unsigned char const* script_text, VM& vm);

// Compile a function body for lazy compilation.
// Returns nullptr if Rust is not available or the SFD doesn't use Rust compilation.
GC::Ptr<Bytecode::Executable> compile_function(VM& vm, SharedFunctionInstanceData& shared_data, bool builtin_abstract_operations_enabled);

JS_API void* clone_function_ast(void const*);
JS_API FFI::CompiledFunction* compile_function_off_thread(void* function_ast, size_t length_in_code_units, bool builtin_abstract_operations_enabled);
JS_API void materialize_compiled_function(FFI::CompiledFunction*, VM&, SourceCode const&, SharedFunctionInstanceData&);
JS_API void free_compiled_function(FFI::CompiledFunction*);

// Free a Rust function AST pointer. No-op if Rust is not available.
void free_function_ast(void* ast);

}
