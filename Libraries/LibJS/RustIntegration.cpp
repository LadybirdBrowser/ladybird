/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/RustIntegration.h>

#include <AK/NumericLimits.h>
#include <AK/TemporaryChange.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/kmalloc.h>
#include <LibCore/EventLoop.h>
#include <LibGC/DeferGC.h>
#include <LibJS/Bytecode/ClassBlueprint.h>
#include <LibJS/Bytecode/Debug.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/IdentifierTable.h>
#include <LibJS/Bytecode/PropertyKeyTable.h>
#include <LibJS/Bytecode/RegexTable.h>
#include <LibJS/Bytecode/StringTable.h>
#include <LibJS/Bytecode/Validator.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/NativeJavaScriptBackedFunction.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/RustFFI.h>
#include <LibJS/Script.h>
#include <LibJS/SourceCode.h>

extern bool JS::g_dump_ast;
extern bool JS::g_dump_ast_use_color;

using namespace JS::FFI;

namespace JS::RustIntegration {

// --- Shared helpers ---

// Bytecode cache blobs are validated before rebuilding Executables. Materialization paths flip this flag for the
// duration of their work so rust_create_executable() does not run the same validator again.
static thread_local bool s_skip_bytecode_validation_for_prevalidated_cache = false;

static Utf16View utf16_view_from_bytes(uint16_t const* data, size_t len)
{
    if (len == 0)
        return {};
    return Utf16View { reinterpret_cast<char16_t const*>(data), len };
}

static Utf16FlyString utf16_fly_from(uint16_t const* data, size_t len)
{
    return Utf16FlyString::from_utf16(utf16_view_from_bytes(data, len));
}

static Utf16FlyString utf16_fly_from_raw(uint16_t const* data, size_t len)
{
    if (len == 0)
        return {};
    return Utf16FlyString::from_utf16(utf16_view_from_bytes(data, len));
}

static Utf16String utf16_from_raw(uint16_t const* data, size_t len)
{
    if (len == 0)
        return {};
    return Utf16String::from_utf16(utf16_view_from_bytes(data, len));
}

// --- Error collection callbacks ---

// Collects parse errors as a Vector<ParserError> (for Script/Module compilation).
static void collect_parse_errors(void* ctx, uint8_t const* message, size_t message_len, uint32_t line, uint32_t column)
{
    auto& errors = *static_cast<Vector<ParserError>*>(ctx);
    errors.append({
        MUST(String::from_utf8({ message, message_len })),
        Position { line, column },
    });
}

// Collects a single parse error as a formatted String (for eval/dynamic function compilation).
static void collect_single_parse_error(void* ctx, uint8_t const* message, size_t message_len, uint32_t line, uint32_t column)
{
    auto& error_message = *static_cast<String*>(ctx);
    if (error_message.is_empty())
        error_message = MUST(String::formatted("{} (line: {}, column: {})", MUST(String::from_utf8({ message, message_len })), line, column));
}

// --- Script GDI builder and callbacks ---

struct ScriptGdiBuilder {
    ScriptResult result;
    SharedFunctionInstanceDataList shared_function_data;

    void collect_shared_function_data()
    {
        result.shared_function_data.clear();
        shared_function_data.for_each([&](auto& shared_data) {
            result.shared_function_data.append(shared_data);
        });
    }
};

}

namespace JS::FFI {

extern "C" void script_gdi_push_lexical_name(void* ctx, uint16_t const* name, size_t len)
{
    static_cast<JS::RustIntegration::ScriptGdiBuilder*>(ctx)->result.lexical_names.append(JS::RustIntegration::utf16_fly_from(name, len));
}

extern "C" void script_gdi_push_var_name(void* ctx, uint16_t const* name, size_t len)
{
    static_cast<JS::RustIntegration::ScriptGdiBuilder*>(ctx)->result.var_names.append(JS::RustIntegration::utf16_fly_from(name, len));
}

extern "C" void script_gdi_push_function(void* ctx, void* sfd_ptr, uint16_t const* name, size_t len)
{
    auto& builder = *static_cast<JS::RustIntegration::ScriptGdiBuilder*>(ctx);
    auto fn_name = JS::RustIntegration::utf16_fly_from(name, len);
    builder.result.declared_function_names.set(fn_name);
    auto& sfd = *static_cast<JS::SharedFunctionInstanceData*>(sfd_ptr);
    builder.result.functions_to_initialize.append({ sfd, move(fn_name) });
}

extern "C" void script_gdi_push_var_scoped_name(void* ctx, uint16_t const* name, size_t len)
{
    static_cast<JS::RustIntegration::ScriptGdiBuilder*>(ctx)->result.var_scoped_names.append(JS::RustIntegration::utf16_fly_from(name, len));
}

extern "C" void script_gdi_push_annex_b_name(void* ctx, uint16_t const* name, size_t len)
{
    static_cast<JS::RustIntegration::ScriptGdiBuilder*>(ctx)->result.annex_b_candidate_names.append(JS::RustIntegration::utf16_fly_from(name, len));
}

extern "C" void script_gdi_push_lexical_binding(void* ctx, uint16_t const* name, size_t len, bool is_constant)
{
    static_cast<JS::RustIntegration::ScriptGdiBuilder*>(ctx)->result.lexical_bindings.append({ JS::RustIntegration::utf16_fly_from(name, len), is_constant });
}

}

// --- Eval GDI builder and callbacks ---

namespace JS::RustIntegration {

struct EvalGdiBuilder {
    GC::Ptr<Bytecode::Executable> executable;
    bool is_strict_mode { false };
    Vector<Utf16FlyString> var_names;
    Vector<EvalDeclarationData::FunctionToInitialize> functions_to_initialize;
    HashTable<Utf16FlyString> declared_function_names;
    Vector<Utf16FlyString> var_scoped_names;
    Vector<Utf16FlyString> annex_b_candidate_names;
    Vector<EvalDeclarationData::LexicalBinding> lexical_bindings;
    Vector<Utf16FlyString> referenced_private_names;

    EvalResult to_result()
    {
        EvalResult result;
        result.executable = executable;
        result.is_strict_mode = is_strict_mode;
        result.declaration_data.var_names = move(var_names);
        result.declaration_data.functions_to_initialize = move(functions_to_initialize);
        result.declaration_data.declared_function_names = move(declared_function_names);
        result.declaration_data.var_scoped_names = move(var_scoped_names);
        result.declaration_data.annex_b_candidate_names = move(annex_b_candidate_names);
        result.declaration_data.lexical_bindings = move(lexical_bindings);
        result.declaration_data.referenced_private_names = move(referenced_private_names);
        return result;
    }
};

}

namespace JS::FFI {

extern "C" void eval_gdi_set_strict(void* ctx, bool is_strict)
{
    static_cast<JS::RustIntegration::EvalGdiBuilder*>(ctx)->is_strict_mode = is_strict;
}

extern "C" void eval_gdi_push_var_name(void* ctx, uint16_t const* name, size_t len)
{
    static_cast<JS::RustIntegration::EvalGdiBuilder*>(ctx)->var_names.append(JS::RustIntegration::utf16_fly_from(name, len));
}

extern "C" void eval_gdi_push_function(void* ctx, void* sfd_ptr, uint16_t const* name, size_t len)
{
    auto& builder = *static_cast<JS::RustIntegration::EvalGdiBuilder*>(ctx);
    auto fn_name = JS::RustIntegration::utf16_fly_from(name, len);
    builder.declared_function_names.set(fn_name);
    auto& sfd = *static_cast<JS::SharedFunctionInstanceData*>(sfd_ptr);
    builder.functions_to_initialize.append({ sfd, move(fn_name) });
}

extern "C" void eval_gdi_push_var_scoped_name(void* ctx, uint16_t const* name, size_t len)
{
    static_cast<JS::RustIntegration::EvalGdiBuilder*>(ctx)->var_scoped_names.append(JS::RustIntegration::utf16_fly_from(name, len));
}

extern "C" void eval_gdi_push_annex_b_name(void* ctx, uint16_t const* name, size_t len)
{
    static_cast<JS::RustIntegration::EvalGdiBuilder*>(ctx)->annex_b_candidate_names.append(JS::RustIntegration::utf16_fly_from(name, len));
}

extern "C" void eval_gdi_push_lexical_binding(void* ctx, uint16_t const* name, size_t len, bool is_constant)
{
    static_cast<JS::RustIntegration::EvalGdiBuilder*>(ctx)->lexical_bindings.append({ JS::RustIntegration::utf16_fly_from(name, len), is_constant });
}

extern "C" void eval_gdi_push_private_name(void* ctx, uint16_t const* name, size_t len)
{
    static_cast<JS::RustIntegration::EvalGdiBuilder*>(ctx)->referenced_private_names.append(JS::RustIntegration::utf16_fly_from(name, len));
}

}

// --- Module builder and callbacks ---

namespace JS::RustIntegration {

struct ModuleBuilder {
    ModuleResult result;
    SharedFunctionInstanceDataList shared_function_data;

    void collect_shared_function_data()
    {
        result.shared_function_data.clear();
        shared_function_data.for_each([&](auto& shared_data) {
            result.shared_function_data.append(shared_data);
        });
    }
};

static Vector<ImportAttribute> attributes_from_ffi(FFIUtf16Slice const* keys, FFIUtf16Slice const* values, size_t count)
{
    Vector<ImportAttribute> attributes;
    for (size_t i = 0; i < count; ++i)
        attributes.empend(utf16_from_raw(keys[i].data, keys[i].length), utf16_from_raw(values[i].data, values[i].length));
    return attributes;
}

static Optional<ModuleRequest> module_request_from_ffi(uint16_t const* specifier, size_t specifier_len,
    FFIUtf16Slice const* attribute_keys, FFIUtf16Slice const* attribute_values, size_t attribute_count)
{
    if (specifier == nullptr || specifier_len == 0)
        return {};
    auto attributes = attributes_from_ffi(attribute_keys, attribute_values, attribute_count);
    if (attributes.is_empty())
        return ModuleRequest { utf16_fly_from_raw(specifier, specifier_len) };
    return ModuleRequest { utf16_fly_from_raw(specifier, specifier_len), move(attributes) };
}

}

extern "C" {

static void module_set_has_top_level_await(void* ctx, bool value)
{
    static_cast<JS::RustIntegration::ModuleBuilder*>(ctx)->result.has_top_level_await = value;
}

static void module_push_import_entry(void* ctx,
    uint16_t const* import_name, size_t import_name_len, bool is_namespace,
    uint16_t const* local_name, size_t local_name_len,
    uint16_t const* specifier, size_t specifier_len,
    FFIUtf16Slice const* attribute_keys, FFIUtf16Slice const* attribute_values, size_t attribute_count)
{
    auto* builder = static_cast<JS::RustIntegration::ModuleBuilder*>(ctx);
    Optional<Utf16FlyString> import_name_opt;
    if (!is_namespace)
        import_name_opt = JS::RustIntegration::utf16_fly_from_raw(import_name, import_name_len);
    JS::ImportEntry entry { move(import_name_opt), JS::RustIntegration::utf16_fly_from_raw(local_name, local_name_len) };
    entry.m_module_request = JS::RustIntegration::module_request_from_ffi(specifier, specifier_len, attribute_keys, attribute_values, attribute_count);
    builder->result.import_entries.append(move(entry));
}

static void module_push_export_entry(Vector<JS::ExportEntry>& list, uint8_t kind,
    uint16_t const* export_name, size_t export_name_len,
    uint16_t const* local_or_import_name, size_t local_or_import_name_len,
    uint16_t const* specifier, size_t specifier_len,
    FFIUtf16Slice const* attribute_keys, FFIUtf16Slice const* attribute_values, size_t attribute_count)
{
    Optional<Utf16FlyString> en;
    if (export_name)
        en = JS::RustIntegration::utf16_fly_from_raw(export_name, export_name_len);
    Optional<Utf16FlyString> lin;
    if (local_or_import_name)
        lin = JS::RustIntegration::utf16_fly_from_raw(local_or_import_name, local_or_import_name_len);
    JS::ExportEntry entry { static_cast<JS::ExportEntry::Kind>(kind), move(en), move(lin) };
    entry.m_module_request = JS::RustIntegration::module_request_from_ffi(specifier, specifier_len, attribute_keys, attribute_values, attribute_count);
    list.append(move(entry));
}

static void module_push_local_export(void* ctx, uint8_t kind,
    uint16_t const* export_name, size_t export_name_len,
    uint16_t const* local_or_import_name, size_t local_or_import_name_len,
    uint16_t const* specifier, size_t specifier_len,
    FFIUtf16Slice const* attribute_keys, FFIUtf16Slice const* attribute_values, size_t attribute_count)
{
    module_push_export_entry(static_cast<JS::RustIntegration::ModuleBuilder*>(ctx)->result.local_export_entries, kind,
        export_name, export_name_len, local_or_import_name, local_or_import_name_len,
        specifier, specifier_len, attribute_keys, attribute_values, attribute_count);
}

static void module_push_indirect_export(void* ctx, uint8_t kind,
    uint16_t const* export_name, size_t export_name_len,
    uint16_t const* local_or_import_name, size_t local_or_import_name_len,
    uint16_t const* specifier, size_t specifier_len,
    FFIUtf16Slice const* attribute_keys, FFIUtf16Slice const* attribute_values, size_t attribute_count)
{
    module_push_export_entry(static_cast<JS::RustIntegration::ModuleBuilder*>(ctx)->result.indirect_export_entries, kind,
        export_name, export_name_len, local_or_import_name, local_or_import_name_len,
        specifier, specifier_len, attribute_keys, attribute_values, attribute_count);
}

static void module_push_star_export(void* ctx, uint8_t kind,
    uint16_t const* export_name, size_t export_name_len,
    uint16_t const* local_or_import_name, size_t local_or_import_name_len,
    uint16_t const* specifier, size_t specifier_len,
    FFIUtf16Slice const* attribute_keys, FFIUtf16Slice const* attribute_values, size_t attribute_count)
{
    module_push_export_entry(static_cast<JS::RustIntegration::ModuleBuilder*>(ctx)->result.star_export_entries, kind,
        export_name, export_name_len, local_or_import_name, local_or_import_name_len,
        specifier, specifier_len, attribute_keys, attribute_values, attribute_count);
}

static void module_push_requested_module(void* ctx,
    uint16_t const* specifier, size_t specifier_len,
    FFIUtf16Slice const* attribute_keys, FFIUtf16Slice const* attribute_values, size_t attribute_count)
{
    auto* builder = static_cast<JS::RustIntegration::ModuleBuilder*>(ctx);
    auto attributes = JS::RustIntegration::attributes_from_ffi(attribute_keys, attribute_values, attribute_count);
    if (attributes.is_empty())
        builder->result.requested_modules.empend(JS::RustIntegration::utf16_fly_from_raw(specifier, specifier_len));
    else
        builder->result.requested_modules.empend(JS::RustIntegration::utf16_fly_from_raw(specifier, specifier_len), move(attributes));
}

static void module_set_default_export_binding(void* ctx, uint16_t const* name, size_t name_len)
{
    static_cast<JS::RustIntegration::ModuleBuilder*>(ctx)->result.default_export_binding_name = JS::RustIntegration::utf16_fly_from_raw(name, name_len);
}

static void module_push_var_name(void* ctx, uint16_t const* name, size_t name_len)
{
    static_cast<JS::RustIntegration::ModuleBuilder*>(ctx)->result.var_declared_names.append(JS::RustIntegration::utf16_fly_from_raw(name, name_len));
}

static void module_push_function(void* ctx, void* sfd_ptr, uint16_t const* name, size_t name_len)
{
    auto& shared = *static_cast<JS::SharedFunctionInstanceData*>(sfd_ptr);
    static_cast<JS::RustIntegration::ModuleBuilder*>(ctx)->result.functions_to_initialize.append({ shared, JS::RustIntegration::utf16_fly_from_raw(name, name_len) });
}

static void module_push_lexical_binding(void* ctx, uint16_t const* name, size_t name_len, bool is_constant, int32_t function_index)
{
    static_cast<JS::RustIntegration::ModuleBuilder*>(ctx)->result.lexical_bindings.append({
        .name = JS::RustIntegration::utf16_fly_from_raw(name, name_len),
        .is_constant = is_constant,
        .function_index = function_index,
    });
}

} // extern "C"

// --- Builtin file callback ---

namespace JS::RustIntegration {

static void collect_builtin_function(void* ctx, void* sfd_ptr, uint16_t const*, size_t)
{
    auto& list = *static_cast<Vector<GC::Root<SharedFunctionInstanceData>>*>(ctx);
    list.append(*static_cast<SharedFunctionInstanceData*>(sfd_ptr));
}

// --- Compile functions ---

ParsedProgram* parse_program(u16 const* utf16_data, size_t length_in_code_units, ProgramType type, size_t line_number_offset)
{
    return rust_parse_program(utf16_data, length_in_code_units, static_cast<u8>(type), line_number_offset, g_dump_ast, g_dump_ast_use_color);
}

CompiledProgram* compile_parsed_program_off_thread(ParsedProgram* parsed, size_t length_in_code_units)
{
    return rust_compile_parsed_program_off_thread(parsed, length_in_code_units);
}

CompiledProgram* compile_parsed_program_fully_off_thread(ParsedProgram* parsed, size_t length_in_code_units)
{
    return rust_compile_parsed_program_fully_off_thread(parsed, length_in_code_units);
}

bool parsed_program_has_errors(ParsedProgram const* parsed)
{
    return rust_parsed_program_has_errors(const_cast<ParsedProgram*>(parsed));
}

void free_parsed_program(ParsedProgram* parsed)
{
    rust_free_parsed_program(parsed);
}

void free_compiled_program(CompiledProgram* compiled)
{
    rust_free_compiled_program(compiled);
}

ByteBuffer serialize_compiled_program_for_bytecode_cache(CompiledProgram const& compiled, ProgramType type, ReadonlyBytes source_hash)
{
    auto blob = rust_serialize_compiled_program_for_bytecode_cache(&compiled, static_cast<u8>(type), source_hash.data(), source_hash.size());
    if (!blob.data || blob.length == 0)
        return {};

    auto bytes = ByteBuffer::copy({ blob.data, blob.length }).release_value_but_fixme_should_propagate_errors();
    rust_free_bytecode_cache_blob(blob.data, blob.length);
    return bytes;
}

struct BytecodeCacheBlobOwner {
    Core::ImmutableBytes bytes;
    Core::EventLoop* event_loop { nullptr };
};

static void free_bytecode_cache_blob_owner(void* owner)
{
    auto owner_ptr = adopt_own_if_nonnull(static_cast<BytecodeCacheBlobOwner*>(owner));
    if (!owner_ptr)
        return;

    if (owner_ptr->event_loop) {
        owner_ptr->event_loop->deferred_invoke([owner = move(owner_ptr)] { (void)owner; });
        return;
    }
}

static void* clone_bytecode_cache_blob_owner(void const* owner)
{
    auto const& existing_owner = *static_cast<BytecodeCacheBlobOwner const*>(owner);
    return new Core::ImmutableBytes(existing_owner.bytes);
}

DecodedBytecodeCacheBlob* decode_bytecode_cache_blob(Core::ImmutableBytes bytes, ProgramType expected_type, ReadonlyBytes source_hash)
{
    auto* owner = new BytecodeCacheBlobOwner { move(bytes) };
    return rust_decode_bytecode_cache_blob_with_owner(owner->bytes.bytes().data(), owner->bytes.bytes().size(), static_cast<u8>(expected_type), source_hash.data(), source_hash.size(), owner, clone_bytecode_cache_blob_owner, free_bytecode_cache_blob_owner);
}

DecodedBytecodeCacheBlob* decode_bytecode_cache_blob(Core::ImmutableBytes bytes, ProgramType expected_type, ReadonlyBytes source_hash, Core::EventLoop& event_loop)
{
    auto* owner = new BytecodeCacheBlobOwner { move(bytes), &event_loop };
    return rust_decode_bytecode_cache_blob_with_owner(owner->bytes.bytes().data(), owner->bytes.bytes().size(), static_cast<u8>(expected_type), source_hash.data(), source_hash.size(), owner, clone_bytecode_cache_blob_owner, free_bytecode_cache_blob_owner);
}

bool validate_decoded_bytecode_cache_blob(DecodedBytecodeCacheBlob* blob, size_t source_length)
{
    return rust_validate_decoded_bytecode_cache_blob(blob, source_length);
}

void free_decoded_bytecode_cache_blob(DecodedBytecodeCacheBlob* blob)
{
    rust_free_decoded_bytecode_cache_blob(blob);
}

Optional<Result<ScriptResult, Vector<ParserError>>> compile_parsed_script(ParsedProgram* parsed, NonnullRefPtr<SourceCode const> source_code, Realm& realm)
{
    if (!parsed)
        return {};

    if (rust_parsed_program_has_errors(parsed)) {
        Vector<ParserError> parse_errors;
        rust_parsed_program_take_errors(parsed, &parse_errors, collect_parse_errors);
        rust_free_parsed_program(parsed);
        return parse_errors;
    }

    auto length = source_code->length_in_code_units();

    GC::DeferGC defer_gc(realm.vm().heap());
    ScriptGdiBuilder builder;

    void* exec_ptr = rust_compile_parsed_script(parsed, &realm.vm(), source_code.ptr(), &builder.shared_function_data, &builder, length);

    if (!exec_ptr)
        return Vector<ParserError> {};

    builder.collect_shared_function_data();
    builder.result.executable = static_cast<Bytecode::Executable*>(exec_ptr);
    return builder.result;
}

Optional<Result<ScriptResult, Vector<ParserError>>> materialize_compiled_script(CompiledProgram* compiled, NonnullRefPtr<SourceCode const> source_code, Realm& realm)
{
    if (!compiled)
        return {};

    GC::DeferGC defer_gc(realm.vm().heap());
    ScriptGdiBuilder builder;

    void* exec_ptr = rust_materialize_compiled_script(compiled, &realm.vm(), source_code.ptr(), &builder.shared_function_data, &builder);

    if (!exec_ptr)
        return Vector<ParserError> {};

    builder.collect_shared_function_data();
    builder.result.executable = static_cast<Bytecode::Executable*>(exec_ptr);
    return builder.result;
}

Optional<Result<ScriptResult, Vector<ParserError>>> materialize_bytecode_cache_script(DecodedBytecodeCacheBlob* blob, NonnullRefPtr<SourceCode const> source_code, Realm& realm)
{
    if (!blob)
        return {};

    GC::DeferGC defer_gc(realm.vm().heap());
    TemporaryChange skip_cache_executable_validation { s_skip_bytecode_validation_for_prevalidated_cache, true };
    ScriptGdiBuilder builder;

    void* exec_ptr = rust_materialize_bytecode_cache_script(blob, &realm.vm(), source_code.ptr(), source_code->length_in_code_units(), &builder.shared_function_data, &builder);

    if (!exec_ptr)
        return Vector<ParserError> { ParserError { "Failed to materialize bytecode cache"_string, {} } };

    builder.collect_shared_function_data();
    builder.result.executable = static_cast<Bytecode::Executable*>(exec_ptr);
    return builder.result;
}

Optional<Result<ScriptResult, Vector<ParserError>>> compile_script(StringView source_text, Realm& realm, StringView filename, size_t line_number_offset)
{
    auto source_code = SourceCode::create(
        String::from_utf8(filename).release_value_but_fixme_should_propagate_errors(),
        Utf16String::from_utf8(source_text));

    auto const* source_ptr = source_code->utf16_data();
    auto length = source_code->length_in_code_units();

    auto* parsed = rust_parse_program(source_ptr, length, static_cast<u8>(ProgramType::Script), line_number_offset, g_dump_ast, g_dump_ast_use_color);

    return compile_parsed_script(parsed, source_code, realm);
}

Optional<Result<EvalResult, String>> compile_eval(
    PrimitiveString& code_string, VM& vm,
    CallerMode strict_caller, bool in_function, bool in_method,
    bool in_derived_constructor, bool in_class_field_initializer)
{
    auto source_code = SourceCode::create({}, code_string.utf16_string());
    auto const& code_view = source_code->code_view();
    auto length = code_view.length_in_code_units();

    GC::DeferGC defer_gc(vm.heap());
    EvalGdiBuilder builder;
    String parse_error;

    auto const* source_ptr = source_code->utf16_data();

    void* exec_ptr = rust_compile_eval(source_ptr, length, &vm, source_code.ptr(), &builder,
        strict_caller == CallerMode::Strict,
        in_function, in_method, in_derived_constructor, in_class_field_initializer,
        &parse_error, collect_single_parse_error, nullptr, nullptr);

    if (!exec_ptr)
        return parse_error;

    builder.executable = static_cast<Bytecode::Executable*>(exec_ptr);
    builder.executable->name = "eval"_utf16_fly_string;

    auto result = builder.to_result();

    // If the caller is strict, the eval is always strict regardless of what Rust reported.
    if (strict_caller == CallerMode::Strict)
        result.is_strict_mode = true;

    return result;
}

Optional<Result<ModuleResult, Vector<ParserError>>> compile_parsed_module(ParsedProgram* parsed, NonnullRefPtr<SourceCode const> source_code, Realm& realm)
{
    if (!parsed)
        return {};

    if (rust_parsed_program_has_errors(parsed)) {
        Vector<ParserError> parse_errors;
        rust_parsed_program_take_errors(parsed, &parse_errors, collect_parse_errors);
        rust_free_parsed_program(parsed);
        return parse_errors;
    }

    auto length = source_code->length_in_code_units();

    GC::DeferGC defer_gc(realm.vm().heap());
    ModuleBuilder builder;
    ModuleCallbacks callbacks {
        .set_has_top_level_await = module_set_has_top_level_await,
        .push_import_entry = module_push_import_entry,
        .push_local_export = module_push_local_export,
        .push_indirect_export = module_push_indirect_export,
        .push_star_export = module_push_star_export,
        .push_requested_module = module_push_requested_module,
        .set_default_export_binding = module_set_default_export_binding,
        .push_var_name = module_push_var_name,
        .push_function = module_push_function,
        .push_lexical_binding = module_push_lexical_binding,
    };

    void* tla_executable = nullptr;

    void* exec_ptr = rust_compile_parsed_module(parsed, &realm.vm(), source_code.ptr(), &builder.shared_function_data,
        &builder, &callbacks, &tla_executable, length);

    if (!exec_ptr && !tla_executable)
        return Vector<ParserError> {};

    builder.collect_shared_function_data();
    if (tla_executable) {
        auto& vm = realm.vm();
        auto* tla_exec = static_cast<Bytecode::Executable*>(tla_executable);

        builder.result.tla_shared_data = vm.heap().allocate<SharedFunctionInstanceData>(
            vm, FunctionKind::Async,
            "module code with top-level await"_utf16_fly_string,
            0, 0, true, false, true,
            Vector<Utf16FlyString> {}, NoSharedFunctionDataList {}, nullptr);
        builder.result.tla_shared_data->m_is_module_wrapper = true;
        builder.result.tla_shared_data->m_uses_this = true;
        builder.result.tla_shared_data->m_function_environment_needed = true;
        builder.result.tla_shared_data->update_asm_call_metadata();
        builder.result.tla_shared_data->set_executable(tla_exec);
    } else {
        builder.result.executable = static_cast<Bytecode::Executable*>(exec_ptr);
    }

    return builder.result;
}

Optional<Result<ModuleResult, Vector<ParserError>>> materialize_compiled_module(CompiledProgram* compiled, NonnullRefPtr<SourceCode const> source_code, Realm& realm)
{
    if (!compiled)
        return {};

    GC::DeferGC defer_gc(realm.vm().heap());
    ModuleBuilder builder;
    ModuleCallbacks callbacks {
        .set_has_top_level_await = module_set_has_top_level_await,
        .push_import_entry = module_push_import_entry,
        .push_local_export = module_push_local_export,
        .push_indirect_export = module_push_indirect_export,
        .push_star_export = module_push_star_export,
        .push_requested_module = module_push_requested_module,
        .set_default_export_binding = module_set_default_export_binding,
        .push_var_name = module_push_var_name,
        .push_function = module_push_function,
        .push_lexical_binding = module_push_lexical_binding,
    };

    void* tla_executable = nullptr;

    void* exec_ptr = rust_materialize_compiled_module(compiled, &realm.vm(), source_code.ptr(), &builder.shared_function_data,
        &builder, &callbacks, &tla_executable);

    if (!exec_ptr && !tla_executable)
        return Vector<ParserError> {};

    builder.collect_shared_function_data();
    if (tla_executable) {
        auto& vm = realm.vm();
        auto* tla_exec = static_cast<Bytecode::Executable*>(tla_executable);

        builder.result.tla_shared_data = vm.heap().allocate<SharedFunctionInstanceData>(
            vm, FunctionKind::Async,
            "module code with top-level await"_utf16_fly_string,
            0, 0, true, false, true,
            Vector<Utf16FlyString> {}, NoSharedFunctionDataList {}, nullptr);
        builder.result.tla_shared_data->m_is_module_wrapper = true;
        builder.result.tla_shared_data->m_uses_this = true;
        builder.result.tla_shared_data->m_function_environment_needed = true;
        builder.result.tla_shared_data->update_asm_call_metadata();
        builder.result.tla_shared_data->set_executable(tla_exec);
    } else {
        builder.result.executable = static_cast<Bytecode::Executable*>(exec_ptr);
    }

    return builder.result;
}

Optional<Result<ModuleResult, Vector<ParserError>>> materialize_bytecode_cache_module(DecodedBytecodeCacheBlob* blob, NonnullRefPtr<SourceCode const> source_code, Realm& realm)
{
    if (!blob)
        return {};

    GC::DeferGC defer_gc(realm.vm().heap());
    TemporaryChange skip_cache_executable_validation { s_skip_bytecode_validation_for_prevalidated_cache, true };
    ModuleBuilder builder;
    ModuleCallbacks callbacks {
        .set_has_top_level_await = module_set_has_top_level_await,
        .push_import_entry = module_push_import_entry,
        .push_local_export = module_push_local_export,
        .push_indirect_export = module_push_indirect_export,
        .push_star_export = module_push_star_export,
        .push_requested_module = module_push_requested_module,
        .set_default_export_binding = module_set_default_export_binding,
        .push_var_name = module_push_var_name,
        .push_function = module_push_function,
        .push_lexical_binding = module_push_lexical_binding,
    };

    void* tla_executable = nullptr;

    void* exec_ptr = rust_materialize_bytecode_cache_module(blob, &realm.vm(), source_code.ptr(), source_code->length_in_code_units(), &builder.shared_function_data,
        &builder, &callbacks, &tla_executable);

    if (!exec_ptr && !tla_executable)
        return Vector<ParserError> { ParserError { "Failed to materialize bytecode cache"_string, {} } };

    builder.collect_shared_function_data();
    if (tla_executable) {
        auto& vm = realm.vm();
        auto* tla_exec = static_cast<Bytecode::Executable*>(tla_executable);

        builder.result.tla_shared_data = vm.heap().allocate<SharedFunctionInstanceData>(
            vm, FunctionKind::Async,
            "module code with top-level await"_utf16_fly_string,
            0, 0, true, false, true,
            Vector<Utf16FlyString> {}, NoSharedFunctionDataList {}, nullptr);
        builder.result.tla_shared_data->m_is_module_wrapper = true;
        builder.result.tla_shared_data->m_uses_this = true;
        builder.result.tla_shared_data->m_function_environment_needed = true;
        builder.result.tla_shared_data->update_asm_call_metadata();
        builder.result.tla_shared_data->set_executable(tla_exec);
    } else {
        builder.result.executable = static_cast<Bytecode::Executable*>(exec_ptr);
    }

    return builder.result;
}

GC::Ptr<Bytecode::Executable> try_install_bytecode_cache_script(DecodedBytecodeCacheBlob* blob, NonnullRefPtr<SourceCode const> source_code, Realm& realm, Bytecode::Executable& existing_executable, ReadonlySpan<SharedFunctionInstanceData*> existing_shared_function_data)
{
    if (!blob)
        return {};

    Vector<void*> existing_shared_function_data_ptrs;
    existing_shared_function_data_ptrs.ensure_capacity(existing_shared_function_data.size());
    for (auto* function : existing_shared_function_data)
        existing_shared_function_data_ptrs.unchecked_append(function);

    GC::Root<Bytecode::Executable> executable;
    {
        GC::DeferGC defer_gc(realm.vm().heap());
        TemporaryChange skip_cache_executable_validation { s_skip_bytecode_validation_for_prevalidated_cache, true };

        executable = static_cast<Bytecode::Executable*>(rust_install_bytecode_cache_script(
            blob, &realm.vm(), source_code.ptr(), source_code->length_in_code_units(), &existing_executable,
            existing_shared_function_data_ptrs.data(), existing_shared_function_data_ptrs.size()));
    }

    if (executable)
        executable->copy_runtime_caches_from(existing_executable);
    return executable.ptr();
}

GC::Ref<Bytecode::Executable> install_generated_bytecode_cache_script(DecodedBytecodeCacheBlob* blob, NonnullRefPtr<SourceCode const> source_code, Realm& realm, Bytecode::Executable& existing_executable, ReadonlySpan<SharedFunctionInstanceData*> existing_shared_function_data)
{
    auto executable = try_install_bytecode_cache_script(blob, move(source_code), realm, existing_executable, existing_shared_function_data);
    VERIFY(executable);
    return *executable;
}

Optional<ModuleBytecodeCacheInstallResult> try_install_bytecode_cache_module(DecodedBytecodeCacheBlob* blob, NonnullRefPtr<SourceCode const> source_code, Realm& realm, Bytecode::Executable* existing_executable, ReadonlySpan<SharedFunctionInstanceData*> existing_shared_function_data, SharedFunctionInstanceData* existing_top_level_await_shared_data)
{
    if (!blob)
        return {};

    Vector<void*> existing_shared_function_data_ptrs;
    existing_shared_function_data_ptrs.ensure_capacity(existing_shared_function_data.size());
    for (auto* function : existing_shared_function_data)
        existing_shared_function_data_ptrs.unchecked_append(function);

    GC::DeferGC defer_gc(realm.vm().heap());
    TemporaryChange skip_cache_executable_validation { s_skip_bytecode_validation_for_prevalidated_cache, true };

    void* top_level_await_executable = nullptr;
    auto* exec = static_cast<Bytecode::Executable*>(rust_install_bytecode_cache_module(
        blob, &realm.vm(), source_code.ptr(), source_code->length_in_code_units(),
        existing_executable, existing_shared_function_data_ptrs.data(), existing_shared_function_data_ptrs.size(),
        existing_top_level_await_shared_data, &top_level_await_executable));

    if (!exec && !top_level_await_executable)
        return {};

    ModuleBytecodeCacheInstallResult result;
    if (exec) {
        if (existing_executable)
            exec->copy_runtime_caches_from(*existing_executable);
        result.executable = exec;
    }
    if (top_level_await_executable) {
        auto* executable = static_cast<Bytecode::Executable*>(top_level_await_executable);
        if (existing_top_level_await_shared_data && existing_top_level_await_shared_data->m_executable)
            executable->copy_runtime_caches_from(*existing_top_level_await_shared_data->m_executable);
        result.top_level_await_executable = executable;
    }
    return result;
}

ModuleBytecodeCacheInstallResult install_generated_bytecode_cache_module(DecodedBytecodeCacheBlob* blob, NonnullRefPtr<SourceCode const> source_code, Realm& realm, Bytecode::Executable* existing_executable, ReadonlySpan<SharedFunctionInstanceData*> existing_shared_function_data, SharedFunctionInstanceData* existing_top_level_await_shared_data)
{
    auto result = try_install_bytecode_cache_module(blob, move(source_code), realm, existing_executable, existing_shared_function_data, existing_top_level_await_shared_data);
    VERIFY(result.has_value());
    return result.release_value();
}

Optional<Result<ModuleResult, Vector<ParserError>>> compile_module(StringView source_text, Realm& realm, StringView filename)
{
    auto source_code = SourceCode::create(String::from_utf8(filename).release_value_but_fixme_should_propagate_errors(), Utf16String::from_utf8(source_text));

    auto const* source_ptr = source_code->utf16_data();
    auto length = source_code->length_in_code_units();
    auto* parsed = rust_parse_program(source_ptr, length, static_cast<u8>(ProgramType::Module), 0, g_dump_ast, g_dump_ast_use_color);

    return compile_parsed_module(parsed, source_code, realm);
}

Optional<Result<GC::Ref<SharedFunctionInstanceData>, String>> compile_dynamic_function(
    VM& vm, StringView source_text, StringView parameters_string, StringView body_parse_string,
    FunctionKind kind)
{
    auto source_code = SourceCode::create({}, Utf16String::from_utf8(source_text));
    auto const& code_view = source_code->code_view();
    auto full_length = code_view.length_in_code_units();

    auto params_utf16 = Utf16String::from_utf8(parameters_string);
    auto body_utf16 = Utf16String::from_utf8(body_parse_string);

    auto prepare_utf16 = [](Utf16View const& view, Vector<u16>& buf) -> u16 const* {
        if (view.has_ascii_storage()) {
            auto ascii = view.ascii_span();
            buf.ensure_capacity(view.length_in_code_units());
            for (size_t i = 0; i < view.length_in_code_units(); ++i)
                buf.unchecked_append(static_cast<u16>(ascii[i]));
            return buf.data();
        }
        return reinterpret_cast<u16 const*>(view.utf16_span().data());
    };

    Vector<u16> full_buf, params_buf, body_buf;
    auto const* full_data = prepare_utf16(code_view, full_buf);
    auto const* params_data = prepare_utf16(params_utf16.utf16_view(), params_buf);
    auto const* body_data = prepare_utf16(body_utf16.utf16_view(), body_buf);

    GC::DeferGC defer_gc(vm.heap());
    String parse_error;

    void* sfd_ptr = rust_compile_dynamic_function(
        full_data, full_length,
        params_data, params_utf16.utf16_view().length_in_code_units(),
        body_data, body_utf16.utf16_view().length_in_code_units(),
        &vm, source_code.ptr(),
        static_cast<u8>(kind),
        &parse_error, collect_single_parse_error,
        nullptr, nullptr);

    if (!sfd_ptr)
        return parse_error;

    auto& function_data = *static_cast<SharedFunctionInstanceData*>(sfd_ptr);
    function_data.m_source_text_owner = Utf16String::from_utf8(source_text);

    return GC::Ref<SharedFunctionInstanceData> { function_data };
}

Optional<Vector<GC::Root<SharedFunctionInstanceData>>> compile_builtin_file(
    unsigned char const* script_text, VM& vm)
{
    auto script_text_as_utf16 = Utf16String::from_utf8_without_validation({ script_text, strlen(reinterpret_cast<char const*>(script_text)) });
    auto code = SourceCode::create("BuiltinFile"_string, move(script_text_as_utf16));

    auto const& code_view = code->code_view();
    auto length = code_view.length_in_code_units();

    GC::DeferGC defer_gc(vm.heap());

    Vector<GC::Root<SharedFunctionInstanceData>> shared_data_list;

    auto const* source_ptr = code->utf16_data();

    rust_compile_builtin_file(source_ptr, length, &vm, code.ptr(), &shared_data_list, collect_builtin_function,
        nullptr, nullptr);

    return shared_data_list;
}

GC::Ptr<Bytecode::Executable> compile_function(VM& vm, SharedFunctionInstanceData& shared_data, bool builtin_abstract_operations_enabled)
{
    if (shared_data.m_precompiled_bytecode_executable) {
        GC::DeferGC defer_gc(vm.heap());
        auto* exec = static_cast<Bytecode::Executable*>(rust_materialize_precompiled_bytecode_function(
            shared_data.m_precompiled_bytecode_executable,
            &vm,
            shared_data.m_source_code.ptr(),
            shared_data.m_owner_shared_function_data_list));
        shared_data.m_precompiled_bytecode_executable = nullptr;
        return exec;
    }

    if (shared_data.m_cached_bytecode_executable) {
        GC::DeferGC defer_gc(vm.heap());
        TemporaryChange skip_cache_executable_validation { s_skip_bytecode_validation_for_prevalidated_cache, true };
        auto* exec = static_cast<Bytecode::Executable*>(rust_materialize_bytecode_cache_function(
            shared_data.m_cached_bytecode_executable,
            &vm,
            shared_data.m_source_code.ptr(),
            shared_data.m_owner_shared_function_data_list));
        shared_data.m_cached_bytecode_executable = nullptr;
        return exec;
    }

    if (!shared_data.m_use_rust_compilation)
        return nullptr;

    VERIFY(shared_data.m_rust_function_ast);
    GC::DeferGC defer_gc(vm.heap());
    auto const* source_ptr = shared_data.m_source_code->utf16_data();
    auto* exec = static_cast<Bytecode::Executable*>(rust_compile_function(
        &vm,
        shared_data.m_source_code.ptr(),
        source_ptr,
        shared_data.m_source_code->length_in_code_units(),
        &shared_data,
        shared_data.m_rust_function_ast,
        builtin_abstract_operations_enabled,
        shared_data.m_owner_shared_function_data_list));
    shared_data.m_rust_function_ast = nullptr;

    return exec;
}

void* clone_function_ast(void const* ast)
{
    return rust_clone_function_ast(ast);
}

CompiledFunction* compile_function_off_thread(void* function_ast, size_t length_in_code_units, bool builtin_abstract_operations_enabled)
{
    return rust_compile_function_off_thread(function_ast, length_in_code_units, builtin_abstract_operations_enabled);
}

void materialize_compiled_function(CompiledFunction* compiled, VM& vm, SourceCode const& source_code, SharedFunctionInstanceData& shared_data)
{
    GC::DeferGC defer_gc(vm.heap());
    rust_materialize_compiled_function(compiled, &vm, &source_code, &shared_data);
}

void free_compiled_function(CompiledFunction* compiled)
{
    rust_free_compiled_function(compiled);
}

void free_cached_bytecode_executable(void* executable)
{
    if (executable)
        rust_free_cached_bytecode_executable(executable);
}

void free_precompiled_bytecode_executable(void* executable)
{
    if (executable)
        rust_free_precompiled_bytecode_executable(executable);
}

void free_function_ast(void* ast)
{
    if (ast)
        rust_free_function_ast(ast);
}

}

// --- FFI factory functions (called by Rust to create C++ objects) ---

namespace JS::FFI {

struct RustCompiledRegex {
    String parsed_pattern;
};

static Utf16View view_from_ffi(FFIUtf16Slice slice)
{
    return JS::RustIntegration::utf16_view_from_bytes(slice.data, slice.length);
}

static Utf16String utf16_from_ffi(FFIUtf16Slice slice)
{
    return Utf16String::from_utf16(view_from_ffi(slice));
}

static Utf16FlyString utf16_fly_from_ffi(FFIUtf16Slice slice)
{
    return Utf16FlyString::from_utf16(view_from_ffi(slice));
}

static void align_constant_cursor(uint8_t const* begin, uint8_t const*& cursor, uint8_t const* end, size_t alignment)
{
    auto offset = static_cast<size_t>(cursor - begin);
    auto aligned_offset = (offset + alignment - 1) & ~(alignment - 1);
    VERIFY(aligned_offset <= static_cast<size_t>(end - begin));
    cursor = begin + aligned_offset;
}

static bool constant_cursor_is_aligned(uint8_t const* cursor, size_t alignment)
{
    return reinterpret_cast<FlatPtr>(cursor) % alignment == 0;
}

static JS::Value decode_constant(JS::VM& vm, uint8_t const* begin, uint8_t const*& cursor, uint8_t const* end)
{
    VERIFY(cursor < end);
    auto const tag = *cursor++;

    switch (static_cast<ConstantTag>(tag)) {
    case ConstantTag::Number: {
        VERIFY(cursor + 8 <= end);
        double value;
        memcpy(&value, cursor, 8);
        cursor += 8;
        return JS::Value(value);
    }
    case ConstantTag::BooleanTrue:
        return JS::Value(true);
    case ConstantTag::BooleanFalse:
        return JS::Value(false);
    case ConstantTag::Null:
        return JS::js_null();
    case ConstantTag::Undefined:
        return JS::js_undefined();
    case ConstantTag::Empty:
        return JS::js_special_empty_value();
    case ConstantTag::String: {
        align_constant_cursor(begin, cursor, end, alignof(char16_t));
        VERIFY(cursor + 4 <= end);
        uint32_t len;
        memcpy(&len, cursor, 4);
        cursor += 4;
        VERIFY(len <= static_cast<size_t>(end - cursor) / sizeof(char16_t));
        if (len == 0)
            return JS::PrimitiveString::create(vm, Utf16String {});
        auto string_byte_length = static_cast<size_t>(len) * sizeof(char16_t);
        auto str = [&] {
            if (constant_cursor_is_aligned(cursor, alignof(char16_t)))
                return Utf16String::from_utf16(Utf16View(reinterpret_cast<char16_t const*>(cursor), len));

            Vector<char16_t> code_units;
            code_units.resize(len);
            memcpy(code_units.data(), cursor, string_byte_length);
            return Utf16String::from_utf16(Utf16View(code_units.data(), len));
        }();
        cursor += string_byte_length;
        return JS::PrimitiveString::create(vm, move(str));
    }
    case ConstantTag::BigInt: {
        VERIFY(cursor + 4 <= end);
        uint32_t len;
        memcpy(&len, cursor, 4);
        cursor += 4;
        VERIFY(cursor + len <= end);
        auto ascii = StringView(reinterpret_cast<char const*>(cursor), len);
        cursor += len;
        auto integer = [&] {
            if (len >= 3 && ascii[0] == '0') {
                if (ascii[1] == 'x' || ascii[1] == 'X')
                    return MUST(Crypto::SignedBigInteger::from_base(16, ascii.substring_view(2)));
                if (ascii[1] == 'o' || ascii[1] == 'O')
                    return MUST(Crypto::SignedBigInteger::from_base(8, ascii.substring_view(2)));
                if (ascii[1] == 'b' || ascii[1] == 'B')
                    return MUST(Crypto::SignedBigInteger::from_base(2, ascii.substring_view(2)));
            }
            return MUST(Crypto::SignedBigInteger::from_base(10, ascii));
        }();
        return JS::BigInt::create(vm, move(integer));
    }
    case ConstantTag::WellKnownSymbol: {
        VERIFY(cursor + 1 <= end);
        auto symbol_id = static_cast<WellKnownSymbolKind>(*cursor++);
        switch (symbol_id) {
        case WellKnownSymbolKind::SymbolIterator:
            return vm.well_known_symbol_iterator();
        case WellKnownSymbolKind::SymbolAsyncIterator:
            return vm.well_known_symbol_async_iterator();
        default:
            VERIFY_NOT_REACHED();
        }
    }
    case ConstantTag::AbstractOperation: {
        VERIFY(cursor + 1 <= end);
        auto operation = static_cast<AbstractOperationKind>(*cursor++);
        auto& intrinsics = vm.current_realm()->intrinsics();
        switch (operation) {
        case AbstractOperationKind::AsyncIteratorClose:
            return JS::Value(intrinsics.async_iterator_close_abstract_operation_function().ptr());
        case AbstractOperationKind::GetMethod:
            return JS::Value(intrinsics.get_method_abstract_operation_function().ptr());
        case AbstractOperationKind::GetIteratorDirect:
            return JS::Value(intrinsics.get_iterator_direct_abstract_operation_function().ptr());
        case AbstractOperationKind::GetIteratorFromMethod:
            return JS::Value(intrinsics.get_iterator_from_method_abstract_operation_function().ptr());
        case AbstractOperationKind::IteratorComplete:
            return JS::Value(intrinsics.iterator_complete_abstract_operation_function().ptr());
        }
        VERIFY_NOT_REACHED();
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

extern "C" void* rust_create_executable(
    void* vm_ptr,
    void const* source_code_ptr,
    FFIExecutableData const* data)
{
    auto& vm = *static_cast<JS::VM*>(vm_ptr);
    auto& source_code = *static_cast<JS::SourceCode const*>(source_code_ptr);

    auto bytecode = [&] {
        if (data->bytecode_owner) {
            auto bytecode_owner = adopt_own_if_nonnull(static_cast<Core::ImmutableBytes*>(data->bytecode_owner));
            VERIFY(bytecode_owner);
            auto bytes = bytecode_owner->bytes();
            size_t offset = 0;
            if (!bytes.is_empty()) {
                VERIFY(data->bytecode >= bytes.data());
                offset = static_cast<size_t>(data->bytecode - bytes.data());
            }
            VERIFY(data->bytecode_length <= bytes.size());
            VERIFY(offset <= bytes.size() - data->bytecode_length);
            return JS::Bytecode::InstructionStream { move(*bytecode_owner), offset, data->bytecode_length };
        }

        Vector<u8> bytecode_vec;
        bytecode_vec.append(data->bytecode, data->bytecode_length);
        return JS::Bytecode::InstructionStream { move(bytecode_vec) };
    }();

    // Build identifier table
    auto ident_table = make<JS::Bytecode::IdentifierTable>();
    ident_table->ensure_capacity(data->identifier_count);
    for (size_t i = 0; i < data->identifier_count; ++i) {
        ident_table->insert(utf16_fly_from_ffi(data->identifier_table[i]));
    }

    // Build property key table
    auto prop_key_table = make<JS::Bytecode::PropertyKeyTable>();
    prop_key_table->ensure_capacity(data->property_key_count);
    for (size_t i = 0; i < data->property_key_count; ++i) {
        prop_key_table->insert(utf16_fly_from_ffi(data->property_key_table[i]));
    }

    // Build string table
    auto str_table = make<JS::Bytecode::StringTable>();
    str_table->ensure_capacity(data->string_count);
    for (size_t i = 0; i < data->string_count; ++i) {
        str_table->insert(utf16_from_ffi(data->string_table[i]));
    }

    // Build regex table from pre-compiled regex objects.
    // NB: The regex table is no longer read at runtime (new_regexp uses pattern+flags directly),
    // but we still need to iterate and free the RustCompiledRegex objects.
    auto regex_tbl = make<JS::Bytecode::RegexTable>();
    for (size_t i = 0; i < data->regex_count; ++i) {
        auto* cr = static_cast<RustCompiledRegex*>(data->compiled_regexes[i]);
        delete cr;
    }

    // Decode constants
    Vector<JS::Value> constants_vec;
    constants_vec.ensure_capacity(data->constants_count);
    auto const* cursor = data->constants_data;
    auto const* end = data->constants_data + data->constants_data_length;
    for (size_t i = 0; i < data->constants_count; ++i) {
        constants_vec.append(decode_constant(vm, data->constants_data, cursor, end));
    }
    VERIFY(cursor == end);

    // Create executable
    auto executable = vm.heap().allocate<JS::Bytecode::Executable>(
        move(bytecode),
        move(ident_table),
        move(prop_key_table),
        move(str_table),
        move(regex_tbl),
        move(constants_vec),
        source_code,
        data->property_lookup_cache_count,
        data->global_variable_cache_count,
        data->environment_coordinate_cache_count,
        data->template_object_cache_count,
        data->object_shape_cache_count,
        data->object_property_iterator_cache_count,
        data->number_of_registers,
        data->is_strict ? JS::Strict::Yes : JS::Strict::No);

    // Set exception handlers
    executable->exception_handlers.ensure_capacity(data->exception_handler_count);
    for (size_t i = 0; i < data->exception_handler_count; ++i) {
        executable->exception_handlers.append({
            data->exception_handlers[i].start_offset,
            data->exception_handlers[i].end_offset,
            data->exception_handlers[i].handler_offset,
        });
    }

    // Set source map
    executable->source_map.ensure_capacity(data->source_map_count);
    for (size_t i = 0; i < data->source_map_count; ++i) {
        executable->source_map.append({
            data->source_map[i].bytecode_offset,
            data->source_map[i].source_start_line,
            data->source_map[i].source_start_column,
        });
    }

    // Keep basic block offsets transient. They are only needed by the
    // validator while this Executable is being constructed.
    Vector<u32> basic_block_offsets;
    basic_block_offsets.ensure_capacity(data->basic_block_count);
    for (size_t i = 0; i < data->basic_block_count; ++i) {
        VERIFY(data->basic_block_offsets[i] <= NumericLimits<u32>::max());
        basic_block_offsets.append(static_cast<u32>(data->basic_block_offsets[i]));
    }

    // Set local variable names
    executable->local_variable_names.ensure_capacity(data->local_variable_count);
    for (size_t i = 0; i < data->local_variable_count; ++i) {
        executable->local_variable_names.append({
            .name = utf16_fly_from_ffi(data->local_variable_names[i]),
            .declaration_kind = JS::LocalVariable::DeclarationKind::Var,
        });
    }

    // Set layout indices
    executable->local_index_base = data->number_of_registers;
    executable->argument_index_base = data->number_of_registers + data->local_variable_count + data->constants_count;
    executable->registers_and_locals_count = data->number_of_registers + data->local_variable_count;
    executable->registers_and_locals_and_constants_count = data->number_of_registers + data->local_variable_count + data->constants_count;
    executable->number_of_arguments = data->number_of_arguments;

    // Set length identifier (for GetLength optimization)
    if (data->length_identifier.has_value)
        executable->length_identifier = JS::Bytecode::PropertyKeyTableIndex(data->length_identifier.value);

    // Set shared function data (inner function definitions)
    executable->shared_function_data.ensure_capacity(data->shared_function_data_count);
    for (size_t i = 0; i < data->shared_function_data_count; ++i) {
        auto* sfd = const_cast<JS::SharedFunctionInstanceData*>(
            static_cast<JS::SharedFunctionInstanceData const*>(data->shared_function_data[i]));
        executable->shared_function_data.append(sfd);
    }

    // Set class blueprints (move from heap-allocated objects)
    executable->class_blueprints.ensure_capacity(data->class_blueprint_count);
    for (size_t i = 0; i < data->class_blueprint_count; ++i) {
        auto* bp = static_cast<JS::Bytecode::ClassBlueprint*>(data->class_blueprints[i]);
        executable->class_blueprints.append(move(*bp));
        delete bp;
    }

#if !defined(NDEBUG) || defined(HAS_ADDRESS_SANITIZER)
    auto const should_validate_bytecode = !JS::RustIntegration::s_skip_bytecode_validation_for_prevalidated_cache;
#else
    auto const should_validate_bytecode = false;
#endif
    if (should_validate_bytecode) {
        if (auto validation = JS::Bytecode::validate_bytecode(*executable, basic_block_offsets.span()); validation.is_error()) {
#if !defined(NDEBUG) || defined(HAS_ADDRESS_SANITIZER)
            VERIFY_NOT_REACHED();
#else
            return nullptr;
#endif
        }
    }

    return executable.ptr();
}

template<typename SharedFunctionDataList>
static GC::Ref<JS::SharedFunctionInstanceData> create_shared_function_instance_data(
    void* vm_ptr,
    void const* source_code_ptr,
    FFISharedFunctionData const* data,
    SharedFunctionDataList&& shared_function_data_list)
{
    auto& vm = *static_cast<JS::VM*>(vm_ptr);
    auto& source_code = *static_cast<JS::SourceCode const*>(source_code_ptr);

    auto fn_name = data->name_len > 0
        ? Utf16FlyString::from_utf16(Utf16View(reinterpret_cast<char16_t const*>(data->name), data->name_len))
        : Utf16FlyString {};

    Vector<Utf16FlyString> mapped_param_names;
    if (data->has_simple_parameter_list) {
        mapped_param_names.ensure_capacity(data->parameter_name_count);
        for (size_t i = 0; i < data->parameter_name_count; ++i)
            mapped_param_names.append(utf16_fly_from_ffi(data->parameter_names[i]));
    }

    auto shared = vm.heap().allocate<JS::SharedFunctionInstanceData>(
        vm,
        static_cast<JS::FunctionKind>(data->function_kind),
        move(fn_name),
        data->function_length,
        data->formal_parameter_count,
        data->strict,
        data->is_arrow,
        data->has_simple_parameter_list,
        move(mapped_param_names),
        forward<SharedFunctionDataList>(shared_function_data_list),
        data->rust_function_ast);

    // Set parsing insights that must be available before lazy compilation.
    shared->m_uses_this = data->uses_this;
    shared->m_this_value_needs_environment_resolution = data->uses_this_from_environment;
    if (data->uses_this_from_environment && !data->is_arrow)
        shared->m_function_environment_needed = true;
    shared->update_asm_call_metadata();

    shared->set_source_text_range(source_code, data->source_text_offset, data->source_text_length);
    shared->m_bytecode_cache_source_text_offset = data->source_text_offset;
    shared->m_bytecode_cache_source_text_length = data->source_text_length;
    shared->m_has_bytecode_cache_source_text_range = true;
    return shared;
}

extern "C" void* rust_create_sfd(
    void* vm_ptr,
    void const* source_code_ptr,
    FFISharedFunctionData const* data)
{
    return create_shared_function_instance_data(vm_ptr, source_code_ptr, data, JS::NoSharedFunctionDataList {}).ptr();
}

extern "C" void* rust_create_sfd_in_list(
    void* vm_ptr,
    void const* source_code_ptr,
    void* shared_function_data_list_ptr,
    FFISharedFunctionData const* data)
{
    auto& shared_function_data_list = *static_cast<JS::SharedFunctionInstanceDataList*>(shared_function_data_list_ptr);
    return create_shared_function_instance_data(vm_ptr, source_code_ptr, data, shared_function_data_list).ptr();
}

extern "C" void rust_sfd_set_metadata(
    void* sfd_ptr,
    bool uses_this,
    bool this_value_needs_environment_resolution,
    bool function_environment_needed,
    size_t function_environment_bindings_count,
    size_t var_environment_bindings_count,
    bool might_need_arguments_object,
    bool contains_direct_call_to_eval)
{
    auto& shared = *static_cast<JS::SharedFunctionInstanceData*>(sfd_ptr);
    shared.m_uses_this = uses_this;
    shared.m_this_value_needs_environment_resolution = this_value_needs_environment_resolution;
    shared.m_function_environment_needed = function_environment_needed;
    shared.update_asm_call_metadata();
    shared.m_function_environment_bindings_count = function_environment_bindings_count;
    shared.m_var_environment_bindings_count = var_environment_bindings_count;
    shared.m_might_need_arguments_object = might_need_arguments_object;
    shared.m_contains_direct_call_to_eval = contains_direct_call_to_eval;
}

extern "C" void rust_sfd_set_class_field_initializer_name(
    void* sfd_ptr,
    uint16_t const* name,
    size_t name_len,
    bool is_private)
{
    auto& shared = *static_cast<JS::SharedFunctionInstanceData*>(sfd_ptr);
    auto utf16_name = Utf16FlyString::from_utf16(JS::RustIntegration::utf16_view_from_bytes(name, name_len));
    if (is_private) {
        shared.m_class_field_initializer_name = JS::PrivateName(0, utf16_name);
    } else {
        shared.m_class_field_initializer_name = JS::PropertyKey(utf16_name.to_utf16_string());
    }
}

extern "C" void rust_sfd_set_precompiled_executable(
    void* sfd_ptr,
    void* executable_ptr,
    bool uses_this,
    bool this_value_needs_environment_resolution,
    bool function_environment_needed,
    size_t function_environment_bindings_count,
    size_t var_environment_bindings_count,
    bool might_need_arguments_object,
    bool contains_direct_call_to_eval)
{
    auto& shared = *static_cast<JS::SharedFunctionInstanceData*>(sfd_ptr);
    auto& executable = *static_cast<JS::Bytecode::Executable*>(executable_ptr);
    auto previous_executable = shared.m_executable;
    if (previous_executable)
        executable.copy_runtime_caches_from(*previous_executable);

    shared.m_uses_this = uses_this;
    shared.m_this_value_needs_environment_resolution = this_value_needs_environment_resolution;
    shared.m_function_environment_needed = function_environment_needed;
    shared.m_function_environment_bindings_count = function_environment_bindings_count;
    shared.m_var_environment_bindings_count = var_environment_bindings_count;
    shared.m_might_need_arguments_object = might_need_arguments_object;
    shared.m_contains_direct_call_to_eval = contains_direct_call_to_eval;
    shared.set_executable(executable);
    executable.name = shared.m_name;
    if (Bytecode::g_dump_bytecode)
        executable.dump();
    shared.clear_compile_inputs();
}

extern "C" void rust_sfd_set_cached_bytecode_executable(
    void* sfd_ptr,
    void* cached_executable_ptr,
    bool uses_this,
    bool this_value_needs_environment_resolution,
    bool function_environment_needed,
    size_t function_environment_bindings_count,
    size_t var_environment_bindings_count,
    bool might_need_arguments_object,
    bool contains_direct_call_to_eval)
{
    auto& shared = *static_cast<JS::SharedFunctionInstanceData*>(sfd_ptr);

    shared.m_uses_this = uses_this;
    shared.m_this_value_needs_environment_resolution = this_value_needs_environment_resolution;
    shared.m_function_environment_needed = function_environment_needed;
    shared.m_function_environment_bindings_count = function_environment_bindings_count;
    shared.m_var_environment_bindings_count = var_environment_bindings_count;
    shared.m_might_need_arguments_object = might_need_arguments_object;
    shared.m_contains_direct_call_to_eval = contains_direct_call_to_eval;
    shared.m_cached_bytecode_executable = cached_executable_ptr;
    shared.update_asm_call_metadata();
}

extern "C" void rust_sfd_set_precompiled_bytecode_executable(
    void* sfd_ptr,
    void* precompiled_executable_ptr,
    bool uses_this,
    bool this_value_needs_environment_resolution,
    bool function_environment_needed,
    size_t function_environment_bindings_count,
    size_t var_environment_bindings_count,
    bool might_need_arguments_object,
    bool contains_direct_call_to_eval)
{
    auto& shared = *static_cast<JS::SharedFunctionInstanceData*>(sfd_ptr);

    shared.clear_compile_inputs();
    shared.m_uses_this = uses_this;
    shared.m_this_value_needs_environment_resolution = this_value_needs_environment_resolution;
    shared.m_function_environment_needed = function_environment_needed;
    shared.m_function_environment_bindings_count = function_environment_bindings_count;
    shared.m_var_environment_bindings_count = var_environment_bindings_count;
    shared.m_might_need_arguments_object = might_need_arguments_object;
    shared.m_contains_direct_call_to_eval = contains_direct_call_to_eval;
    shared.m_precompiled_bytecode_executable = precompiled_executable_ptr;
    shared.update_asm_call_metadata();
}

extern "C" size_t rust_executable_shared_function_data_count(void const* executable_ptr)
{
    if (!executable_ptr)
        return 0;
    auto& executable = *static_cast<JS::Bytecode::Executable const*>(executable_ptr);
    return executable.shared_function_data.size();
}

extern "C" void* rust_executable_shared_function_data_at(void const* executable_ptr, size_t index)
{
    if (!executable_ptr)
        return nullptr;
    auto& executable = *static_cast<JS::Bytecode::Executable const*>(executable_ptr);
    if (index >= executable.shared_function_data.size())
        return nullptr;
    return executable.shared_function_data[index].ptr();
}

extern "C" void* rust_sfd_executable(void const* sfd_ptr)
{
    if (!sfd_ptr)
        return nullptr;
    auto& shared = *static_cast<JS::SharedFunctionInstanceData const*>(sfd_ptr);
    return shared.m_executable.ptr();
}

static size_t bytecode_cache_source_text_offset(JS::SharedFunctionInstanceData const& shared)
{
    if (shared.m_has_bytecode_cache_source_text_range)
        return shared.m_bytecode_cache_source_text_offset;
    return shared.m_source_text_offset;
}

static size_t bytecode_cache_source_text_length(JS::SharedFunctionInstanceData const& shared)
{
    if (shared.m_has_bytecode_cache_source_text_range)
        return shared.m_bytecode_cache_source_text_length;
    return shared.m_source_text_length;
}

extern "C" bool rust_sfd_matches_bytecode_cache_function(void const* sfd_ptr, FFISharedFunctionData const* data)
{
    if (!sfd_ptr || !data)
        return false;
    auto& shared = *static_cast<JS::SharedFunctionInstanceData const*>(sfd_ptr);
    return bytecode_cache_source_text_offset(shared) == data->source_text_offset
        && bytecode_cache_source_text_length(shared) == data->source_text_length
        && shared.m_function_length == data->function_length
        && shared.m_formal_parameter_count == data->formal_parameter_count
        && shared.m_kind == static_cast<JS::FunctionKind>(data->function_kind)
        && shared.m_strict == data->strict
        && shared.m_is_arrow_function == data->is_arrow
        && shared.m_has_simple_parameter_list == data->has_simple_parameter_list;
}

extern "C" void rust_sfd_install_bytecode_cache_executable(
    void* sfd_ptr,
    void* executable_ptr,
    bool uses_this,
    bool this_value_needs_environment_resolution,
    bool function_environment_needed,
    size_t function_environment_bindings_count,
    size_t var_environment_bindings_count,
    bool might_need_arguments_object,
    bool contains_direct_call_to_eval)
{
    auto& shared = *static_cast<JS::SharedFunctionInstanceData*>(sfd_ptr);
    auto& executable = *static_cast<JS::Bytecode::Executable*>(executable_ptr);
    auto previous_executable = shared.m_executable;
    if (previous_executable)
        executable.copy_runtime_caches_from(*previous_executable);

    shared.m_uses_this = uses_this;
    shared.m_this_value_needs_environment_resolution = this_value_needs_environment_resolution;
    shared.m_function_environment_needed = function_environment_needed;
    shared.m_function_environment_bindings_count = function_environment_bindings_count;
    shared.m_var_environment_bindings_count = var_environment_bindings_count;
    shared.m_might_need_arguments_object = might_need_arguments_object;
    shared.m_contains_direct_call_to_eval = contains_direct_call_to_eval;
    shared.set_executable(executable);
    executable.name = shared.m_name;
    if (Bytecode::g_dump_bytecode)
        executable.dump();
    shared.clear_compile_inputs();
}

extern "C" void rust_sfd_install_cached_bytecode_executable(
    void* sfd_ptr,
    void* cached_executable_ptr,
    bool uses_this,
    bool this_value_needs_environment_resolution,
    bool function_environment_needed,
    size_t function_environment_bindings_count,
    size_t var_environment_bindings_count,
    bool might_need_arguments_object,
    bool contains_direct_call_to_eval)
{
    auto& shared = *static_cast<JS::SharedFunctionInstanceData*>(sfd_ptr);

    shared.clear_compile_inputs();
    shared.m_uses_this = uses_this;
    shared.m_this_value_needs_environment_resolution = this_value_needs_environment_resolution;
    shared.m_function_environment_needed = function_environment_needed;
    shared.m_function_environment_bindings_count = function_environment_bindings_count;
    shared.m_var_environment_bindings_count = var_environment_bindings_count;
    shared.m_might_need_arguments_object = might_need_arguments_object;
    shared.m_contains_direct_call_to_eval = contains_direct_call_to_eval;
    shared.m_cached_bytecode_executable = cached_executable_ptr;
    shared.update_asm_call_metadata();
}

extern "C" void* rust_create_class_blueprint(
    void* vm_ptr,
    void const* source_code_ptr,
    uint16_t const* name,
    size_t name_len,
    size_t source_text_offset,
    size_t source_text_len,
    uint32_t constructor_sfd_index,
    bool has_super_class,
    bool has_name,
    FFIClassElement const* elements,
    size_t element_count)
{
    auto* blueprint = new JS::Bytecode::ClassBlueprint();
    blueprint->constructor_shared_function_data_index = constructor_sfd_index;
    blueprint->has_super_class = has_super_class;
    blueprint->has_name = has_name;

    if (name_len > 0)
        blueprint->name = Utf16FlyString::from_utf16(JS::RustIntegration::utf16_view_from_bytes(name, name_len));

    blueprint->source_code = static_cast<JS::SourceCode const*>(source_code_ptr);
    blueprint->source_text_offset = source_text_offset;
    blueprint->source_text_length = source_text_len;

    for (size_t i = 0; i < element_count; ++i) {
        auto const& elem = elements[i];
        JS::Bytecode::ClassElementDescriptor desc;
        desc.kind = static_cast<JS::Bytecode::ClassElementDescriptor::Kind>(elem.kind);
        desc.is_static = elem.is_static;
        desc.is_private = elem.is_private;
        if (elem.private_identifier_len > 0)
            desc.private_identifier = Utf16FlyString::from_utf16(JS::RustIntegration::utf16_view_from_bytes(elem.private_identifier, elem.private_identifier_len));
        if (elem.shared_function_data_index.has_value)
            desc.shared_function_data_index = elem.shared_function_data_index.value;
        desc.has_initializer = elem.has_initializer;
        switch (elem.literal_value_kind) {
        case LiteralValueKind::None:
            break;
        case LiteralValueKind::Number:
            desc.literal_value = JS::Value(elem.literal_value_number);
            break;
        case LiteralValueKind::BooleanTrue:
            desc.literal_value = JS::Value(true);
            break;
        case LiteralValueKind::BooleanFalse:
            desc.literal_value = JS::Value(false);
            break;
        case LiteralValueKind::Null:
            desc.literal_value = JS::js_null();
            break;
        case LiteralValueKind::String: {
            auto& vm = *static_cast<JS::VM*>(vm_ptr);
            auto str_view = JS::RustIntegration::utf16_view_from_bytes(elem.literal_value_string, elem.literal_value_string_len);
            desc.literal_value = JS::Value(JS::PrimitiveString::create(vm, str_view));
            break;
        }
        }
        blueprint->elements.append(desc);
    }

    return blueprint;
}

extern "C" void module_sfd_set_name(
    void* sfd_ptr,
    uint16_t const* name,
    size_t name_len)
{
    auto& shared = *static_cast<JS::SharedFunctionInstanceData*>(sfd_ptr);
    shared.m_name = Utf16FlyString::from_utf16(JS::RustIntegration::utf16_view_from_bytes(name, name_len));
}

extern "C" void* rust_compile_regex(
    uint16_t const* pattern_data, size_t pattern_len,
    uint16_t const* flags_data, size_t flags_len,
    char const** error_out)
{
    *error_out = nullptr;
    auto pattern = JS::RustIntegration::utf16_view_from_bytes(pattern_data, pattern_len);
    auto flags_view = JS::RustIntegration::utf16_view_from_bytes(flags_data, flags_len);

    // Extract unicode/unicode_sets from flags for parse_regex_pattern.
    bool is_unicode = false;
    bool is_unicode_sets = false;
    for (size_t i = 0; i < flags_view.length_in_code_units(); ++i) {
        auto ch = flags_view.code_unit_at(i);
        if (ch == 'u')
            is_unicode = true;
        else if (ch == 'v')
            is_unicode_sets = true;
    }

    auto parsed_pattern = JS::parse_regex_pattern(pattern, is_unicode, is_unicode_sets);
    if (parsed_pattern.is_error()) {
        auto msg = MUST(String::formatted("RegExp compile error: {}", parsed_pattern.release_error().error));
        auto* buf = static_cast<char*>(kmalloc(msg.byte_count() + 1));
        memcpy(buf, msg.bytes().data(), msg.byte_count());
        buf[msg.byte_count()] = '\0';
        *error_out = buf;
        return nullptr;
    }
    auto pattern_str = parsed_pattern.release_value();

    // Build compile flags from the flag characters.
    regex::ECMAScriptCompileFlags compile_flags {};
    for (size_t i = 0; i < flags_view.length_in_code_units(); ++i) {
        auto ch = flags_view.code_unit_at(i);
        switch (ch) {
        case 'g':
            compile_flags.global = true;
            break;
        case 'i':
            compile_flags.ignore_case = true;
            break;
        case 'm':
            compile_flags.multiline = true;
            break;
        case 's':
            compile_flags.dot_all = true;
            break;
        case 'u':
            compile_flags.unicode = true;
            break;
        case 'v':
            compile_flags.unicode_sets = true;
            break;
        case 'y':
            compile_flags.sticky = true;
            break;
        case 'd':
            compile_flags.has_indices = true;
            break;
        default:
            break;
        }
    }

    auto compiled = regex::ECMAScriptRegex::compile(pattern_str.bytes_as_string_view(), compile_flags);
    if (compiled.is_error()) {
        auto msg = MUST(String::formatted("RegExp compile error: {}", compiled.release_error()));
        auto* buf = static_cast<char*>(kmalloc(msg.byte_count() + 1));
        memcpy(buf, msg.bytes().data(), msg.byte_count());
        buf[msg.byte_count()] = '\0';
        *error_out = buf;
        return nullptr;
    }

    return new RustCompiledRegex { move(pattern_str) };
}

extern "C" void rust_free_compiled_regex(void* ptr)
{
    delete static_cast<RustCompiledRegex*>(ptr);
}

extern "C" void rust_free_error_string(char const* str)
{
    kfree(const_cast<char*>(str));
}

extern "C" size_t rust_number_to_utf16(double value, uint16_t* buffer, size_t buffer_len)
{
    auto str = JS::number_to_utf16_string(value);
    auto view = str.utf16_view();
    auto len = min(view.length_in_code_units(), buffer_len);
    for (size_t i = 0; i < len; ++i)
        buffer[i] = view.code_unit_at(i);
    return len;
}

// FIXME: This FFI workaround exists only to match C++ float-to-string
//        formatting in the Rust AST dump. Once the C++ pipeline is
//        removed, this can be deleted and the Rust side can use its own
//        formatting without needing to match C++.
extern "C" size_t rust_format_double(double value, uint8_t* buffer, size_t buffer_len)
{
    auto str = MUST(String::formatted("{}", value));
    auto bytes = str.bytes();
    auto len = min(bytes.size(), buffer_len);
    memcpy(buffer, bytes.data(), len);
    return len;
}

}
