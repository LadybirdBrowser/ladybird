/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/RustIntegration.h>

#ifdef ENABLE_RUST

#    include <AK/Utf16String.h>
#    include <AK/Utf16View.h>
#    include <LibGC/DeferGC.h>
#    include <LibJS/AST.h>
#    include <LibJS/Bytecode/ClassBlueprint.h>
#    include <LibJS/Bytecode/Executable.h>
#    include <LibJS/Bytecode/Generator.h>
#    include <LibJS/Bytecode/IdentifierTable.h>
#    include <LibJS/Bytecode/PropertyKeyTable.h>
#    include <LibJS/Bytecode/RegexTable.h>
#    include <LibJS/Bytecode/StringTable.h>
#    include <LibJS/BytecodeFactory.h>
#    include <LibJS/Lexer.h>
#    include <LibJS/Parser.h>
#    include <LibJS/PipelineComparison.h>
#    include <LibJS/Runtime/BigInt.h>
#    include <LibJS/Runtime/Intrinsics.h>
#    include <LibJS/Runtime/NativeJavaScriptBackedFunction.h>
#    include <LibJS/Runtime/PrimitiveString.h>
#    include <LibJS/Runtime/RegExpObject.h>
#    include <LibJS/Runtime/SharedFunctionInstanceData.h>
#    include <LibJS/Runtime/VM.h>
#    include <LibJS/Script.h>
#    include <LibJS/SourceCode.h>

extern bool JS::g_dump_ast;
extern bool JS::g_dump_ast_use_color;

namespace JS::RustIntegration {

// --- Shared helpers ---

static bool rust_pipeline_enabled()
{
    static bool const enabled = getenv("LIBJS_CPP") == nullptr;
    return enabled;
}

static Utf16FlyString utf16_fly_from(uint16_t const* data, size_t len)
{
    return Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(data), len });
}

static Utf16FlyString utf16_fly_from_raw(uint16_t const* data, size_t len)
{
    if (len == 0)
        return {};
    return Utf16FlyString::from_utf16(Utf16View(reinterpret_cast<char16_t const*>(data), len));
}

static Utf16String utf16_from_raw(uint16_t const* data, size_t len)
{
    if (len == 0)
        return {};
    return Utf16String::from_utf16(Utf16View(reinterpret_cast<char16_t const*>(data), len));
}

// --- Error collection callbacks ---

// Collects parse errors as a Vector<ParserError> (for Script/Module compilation).
static void collect_parse_errors(void* ctx, char const* message, size_t message_len, uint32_t line, uint32_t column)
{
    auto& errors = *static_cast<Vector<ParserError>*>(ctx);
    errors.append({
        MUST(String::from_utf8({ message, message_len })),
        Position { line, column, 0 },
    });
}

// Collects a single parse error as a formatted String (for eval/dynamic function compilation).
static void collect_single_parse_error(void* ctx, char const* message, size_t message_len, uint32_t line, uint32_t column)
{
    auto& error_message = *static_cast<String*>(ctx);
    if (error_message.is_empty())
        error_message = MUST(String::formatted("{} (line: {}, column: {})", MUST(String::from_utf8({ message, message_len })), line, column));
}

// --- Script GDI builder and callbacks ---

struct ScriptGdiBuilder {
    ScriptResult result;
};

}

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
        return result;
    }
};

}

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

// --- Module builder and callbacks ---

namespace JS::RustIntegration {

struct ModuleBuilder {
    ModuleResult result;
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

Optional<Result<ScriptResult, Vector<ParserError>>> compile_script(
    StringView source_text, Realm& realm, StringView filename, size_t line_number_offset)
{
    bool const compare_pipelines = compare_pipelines_enabled();
    if (!rust_pipeline_enabled() && !compare_pipelines)
        return {};

    auto source_code = SourceCode::create(
        String::from_utf8(filename).release_value_but_fixme_should_propagate_errors(),
        Utf16String::from_utf8(source_text));

    auto const& code_view = source_code->code_view();
    auto length = code_view.length_in_code_units();

    GC::DeferGC defer_gc(realm.vm().heap());
    ScriptGdiBuilder builder;
    Vector<ParserError> parse_errors;

    u8* rust_ast_data = nullptr;
    size_t rust_ast_len = 0;
    u8** rust_ast_data_ptr = compare_pipelines ? &rust_ast_data : nullptr;
    size_t* rust_ast_len_ptr = compare_pipelines ? &rust_ast_len : nullptr;

    auto const* source_ptr = source_code->utf16_data();

    void* exec_ptr = rust_compile_script(source_ptr, length, &realm.vm(), source_code.ptr(), &builder,
        g_dump_ast, g_dump_ast_use_color,
        &parse_errors, collect_parse_errors, rust_ast_data_ptr, rust_ast_len_ptr, line_number_offset);

    if (!exec_ptr) {
        if (rust_ast_data)
            rust_free_string(rust_ast_data, rust_ast_len);
        return parse_errors;
    }

    if (compare_pipelines) {
        auto rust_ast_dump = StringView { rust_ast_data, rust_ast_len };

        auto parser = Parser(Lexer(source_code, line_number_offset));
        auto cpp_program = parser.parse_program();

        if (!parser.has_errors()) {
            auto cpp_ast_dump = cpp_program->dump_to_string();
            compare_pipeline_asts(rust_ast_dump, cpp_ast_dump, filename);

            if (!cpp_program->is_strict_mode()) {
                HashTable<Utf16FlyString> lexical_names;
                MUST(cpp_program->for_each_lexically_declared_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
                    lexical_names.set(identifier.string());
                    return {};
                }));
                MUST(cpp_program->for_each_function_hoistable_with_annexB_extension([&](FunctionDeclaration& function_declaration) -> ThrowCompletionOr<void> {
                    if (!lexical_names.contains(function_declaration.name()))
                        function_declaration.set_should_do_additional_annexB_steps();
                    return {};
                }));
            }

            auto& rust_executable = *static_cast<Bytecode::Executable*>(exec_ptr);
            auto cpp_executable = Bytecode::Generator::generate_from_ast_node(realm.vm(), *cpp_program, {});
            auto rust_bytecode_dump = rust_executable.dump_to_string();
            auto cpp_bytecode_dump = cpp_executable->dump_to_string();
            compare_pipeline_bytecode(rust_bytecode_dump, cpp_bytecode_dump, filename, cpp_ast_dump);
        }

        rust_free_string(rust_ast_data, rust_ast_len);
    }

    builder.result.executable = static_cast<Bytecode::Executable*>(exec_ptr);
    return builder.result;
}

Optional<Result<EvalResult, String>> compile_eval(
    PrimitiveString& code_string, VM& vm,
    CallerMode strict_caller, bool in_function, bool in_method,
    bool in_derived_constructor, bool in_class_field_initializer)
{
    bool const compare_pipelines = compare_pipelines_enabled();
    if (!rust_pipeline_enabled() && !compare_pipelines)
        return {};

    auto source_code = SourceCode::create({}, code_string.utf16_string());
    auto const& code_view = source_code->code_view();
    auto length = code_view.length_in_code_units();

    GC::DeferGC defer_gc(vm.heap());
    EvalGdiBuilder builder;
    String parse_error;

    u8* rust_ast_data = nullptr;
    size_t rust_ast_len = 0;
    u8** rust_ast_data_ptr = compare_pipelines ? &rust_ast_data : nullptr;
    size_t* rust_ast_len_ptr = compare_pipelines ? &rust_ast_len : nullptr;

    auto const* source_ptr = source_code->utf16_data();

    void* exec_ptr = rust_compile_eval(source_ptr, length, &vm, source_code.ptr(), &builder,
        strict_caller == CallerMode::Strict,
        in_function, in_method, in_derived_constructor, in_class_field_initializer,
        &parse_error, collect_single_parse_error, rust_ast_data_ptr, rust_ast_len_ptr);

    if (!exec_ptr) {
        if (rust_ast_data)
            rust_free_string(rust_ast_data, rust_ast_len);
        return parse_error;
    }

    if (compare_pipelines) {
        auto rust_ast_dump = StringView { rust_ast_data, rust_ast_len };

        Parser::EvalInitialState initial_state {
            .in_eval_function_context = in_function,
            .allow_super_property_lookup = in_method,
            .allow_super_constructor_call = in_derived_constructor,
            .in_class_field_initializer = in_class_field_initializer,
        };
        Parser parser(Lexer(source_code), Program::Type::Script, move(initial_state));
        auto cpp_program = parser.parse_program(strict_caller == CallerMode::Strict);

        if (!parser.has_errors()) {
            auto cpp_ast_dump = cpp_program->dump_to_string();
            compare_pipeline_asts(rust_ast_dump, cpp_ast_dump, "eval"sv);

            if (!cpp_program->is_strict_mode()) {
                HashTable<Utf16FlyString> lexical_names;
                MUST(cpp_program->for_each_lexically_declared_identifier([&](Identifier const& identifier) -> ThrowCompletionOr<void> {
                    lexical_names.set(identifier.string());
                    return {};
                }));
                MUST(cpp_program->for_each_function_hoistable_with_annexB_extension([&](FunctionDeclaration& function_declaration) -> ThrowCompletionOr<void> {
                    if (!lexical_names.contains(function_declaration.name()))
                        function_declaration.set_should_do_additional_annexB_steps();
                    return {};
                }));
            }

            auto& rust_executable = *static_cast<Bytecode::Executable*>(exec_ptr);
            auto cpp_executable = Bytecode::Generator::generate_from_ast_node(vm, *cpp_program, {});
            auto rust_bytecode_dump = rust_executable.dump_to_string();
            auto cpp_bytecode_dump = cpp_executable->dump_to_string();
            compare_pipeline_bytecode(rust_bytecode_dump, cpp_bytecode_dump, "eval"sv, cpp_ast_dump);
        }

        rust_free_string(rust_ast_data, rust_ast_len);
    }

    builder.executable = static_cast<Bytecode::Executable*>(exec_ptr);
    builder.executable->name = "eval"_utf16_fly_string;

    auto result = builder.to_result();

    // If the caller is strict, the eval is always strict regardless of what Rust reported.
    if (strict_caller == CallerMode::Strict)
        result.is_strict_mode = true;

    return result;
}

Optional<Result<EvalResult, String>> compile_shadow_realm_eval(
    PrimitiveString& source_text, VM& vm)
{
    bool const compare_pipelines = compare_pipelines_enabled();
    if (!rust_pipeline_enabled() && !compare_pipelines)
        return {};

    auto source_code = SourceCode::create({}, source_text.utf16_string());
    auto const& code_view = source_code->code_view();
    auto length = code_view.length_in_code_units();

    GC::DeferGC defer_gc(vm.heap());
    EvalGdiBuilder builder;
    String parse_error;

    u8* rust_ast_data = nullptr;
    size_t rust_ast_len = 0;
    u8** rust_ast_data_ptr = compare_pipelines ? &rust_ast_data : nullptr;
    size_t* rust_ast_len_ptr = compare_pipelines ? &rust_ast_len : nullptr;

    auto const* source_ptr = source_code->utf16_data();

    void* exec_ptr = rust_compile_eval(source_ptr, length, &vm, source_code.ptr(), &builder,
        false, false, false, false, false,
        &parse_error, collect_single_parse_error, rust_ast_data_ptr, rust_ast_len_ptr);

    if (!exec_ptr) {
        if (rust_ast_data)
            rust_free_string(rust_ast_data, rust_ast_len);
        return parse_error;
    }

    if (compare_pipelines) {
        auto rust_ast_dump = StringView { rust_ast_data, rust_ast_len };

        auto parser = Parser(Lexer(source_code), Program::Type::Script, Parser::EvalInitialState {});
        auto cpp_program = parser.parse_program();

        if (!parser.has_errors()) {
            auto cpp_ast_dump = cpp_program->dump_to_string();
            compare_pipeline_asts(rust_ast_dump, cpp_ast_dump, "ShadowRealmEval"sv);

            auto& rust_executable = *static_cast<Bytecode::Executable*>(exec_ptr);
            auto cpp_executable = Bytecode::Generator::generate_from_ast_node(vm, *cpp_program, {});
            auto rust_bytecode_dump = rust_executable.dump_to_string();
            auto cpp_bytecode_dump = cpp_executable->dump_to_string();
            compare_pipeline_bytecode(rust_bytecode_dump, cpp_bytecode_dump, "ShadowRealmEval"sv, cpp_ast_dump);
        }

        rust_free_string(rust_ast_data, rust_ast_len);
    }

    builder.executable = static_cast<Bytecode::Executable*>(exec_ptr);
    builder.executable->name = "ShadowRealmEval"_utf16_fly_string;

    return builder.to_result();
}

Optional<Result<ModuleResult, Vector<ParserError>>> compile_module(
    StringView source_text, Realm& realm, StringView filename)
{
    bool const compare_pipelines = compare_pipelines_enabled();
    if (!rust_pipeline_enabled() && !compare_pipelines)
        return {};

    auto source_code = SourceCode::create(String::from_utf8(filename).release_value_but_fixme_should_propagate_errors(), Utf16String::from_utf8(source_text));
    auto const& code_view = source_code->code_view();
    auto length = code_view.length_in_code_units();

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

    Vector<ParserError> errors;
    auto error_callback = [](void* ctx, char const* message, size_t message_len, u32 line, u32 column) {
        auto* error_list = static_cast<Vector<ParserError>*>(ctx);
        auto msg = String::from_utf8({ message, message_len }).release_value_but_fixme_should_propagate_errors();
        error_list->empend(move(msg), Position { .line = line, .column = column, .offset = 0 });
    };

    u8* rust_ast_data = nullptr;
    size_t rust_ast_len = 0;
    u8** rust_ast_data_ptr = compare_pipelines ? &rust_ast_data : nullptr;
    size_t* rust_ast_len_ptr = compare_pipelines ? &rust_ast_len : nullptr;

    void* tla_executable = nullptr;

    auto const* source_ptr = source_code->utf16_data();

    void* exec_ptr = rust_compile_module(source_ptr, length,
        &realm.vm(), source_code.ptr(),
        &builder, &callbacks,
        g_dump_ast, g_dump_ast_use_color,
        &errors, error_callback,
        &tla_executable,
        rust_ast_data_ptr, rust_ast_len_ptr);

    if (!exec_ptr && !tla_executable) {
        if (rust_ast_data)
            rust_free_string(rust_ast_data, rust_ast_len);
        return errors;
    }

    if (compare_pipelines) {
        auto rust_ast_dump = StringView { rust_ast_data, rust_ast_len };

        auto parser = Parser(Lexer(source_code), Program::Type::Module);
        auto cpp_program = parser.parse_program();

        if (!parser.has_errors()) {
            auto cpp_ast_dump = cpp_program->dump_to_string();
            compare_pipeline_asts(rust_ast_dump, cpp_ast_dump, filename);

            if (!tla_executable) {
                auto& rust_executable = *static_cast<Bytecode::Executable*>(exec_ptr);
                auto cpp_executable = Bytecode::Generator::generate_from_ast_node(realm.vm(), *cpp_program, {});
                auto rust_bytecode_dump = rust_executable.dump_to_string();
                auto cpp_bytecode_dump = cpp_executable->dump_to_string();
                compare_pipeline_bytecode(rust_bytecode_dump, cpp_bytecode_dump, filename, cpp_ast_dump);
            }
        }

        rust_free_string(rust_ast_data, rust_ast_len);
    }

    if (tla_executable) {
        auto& vm = realm.vm();
        auto* tla_exec = static_cast<Bytecode::Executable*>(tla_executable);

        builder.result.tla_shared_data = vm.heap().allocate<SharedFunctionInstanceData>(
            vm, FunctionKind::Async,
            "module code with top-level await"_utf16_fly_string,
            0, 0, true, false, true,
            Vector<Utf16FlyString> {}, nullptr);
        builder.result.tla_shared_data->m_is_module_wrapper = true;
        builder.result.tla_shared_data->m_uses_this = true;
        builder.result.tla_shared_data->m_function_environment_needed = true;
        builder.result.tla_shared_data->m_executable = tla_exec;
    } else {
        builder.result.executable = static_cast<Bytecode::Executable*>(exec_ptr);
    }

    return builder.result;
}

Optional<Result<GC::Ref<SharedFunctionInstanceData>, String>> compile_dynamic_function(
    VM& vm, StringView source_text, StringView parameters_string, StringView body_parse_string,
    FunctionKind kind)
{
    bool const compare_pipelines = compare_pipelines_enabled();
    if (!rust_pipeline_enabled() && !compare_pipelines)
        return {};

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

    u8* rust_ast_data = nullptr;
    size_t rust_ast_len = 0;
    u8** rust_ast_data_ptr = compare_pipelines ? &rust_ast_data : nullptr;
    size_t* rust_ast_len_ptr = compare_pipelines ? &rust_ast_len : nullptr;

    void* sfd_ptr = rust_compile_dynamic_function(
        full_data, full_length,
        params_data, params_utf16.utf16_view().length_in_code_units(),
        body_data, body_utf16.utf16_view().length_in_code_units(),
        &vm, source_code.ptr(),
        static_cast<u8>(kind),
        &parse_error, collect_single_parse_error,
        rust_ast_data_ptr, rust_ast_len_ptr);

    if (!sfd_ptr) {
        if (rust_ast_data)
            rust_free_string(rust_ast_data, rust_ast_len);
        return parse_error;
    }

    if (compare_pipelines) {
        auto rust_ast_dump = StringView { rust_ast_data, rust_ast_len };

        auto source_parser = Parser(Lexer(source_code));
        source_parser.set_is_dynamic_function();
        auto cpp_program = source_parser.parse_program();

        if (!source_parser.has_errors()) {
            auto cpp_ast_dump = cpp_program->dump_to_string();
            compare_pipeline_asts(rust_ast_dump, cpp_ast_dump, "new Function"sv);
        }

        rust_free_string(rust_ast_data, rust_ast_len);
    }

    auto& function_data = *static_cast<SharedFunctionInstanceData*>(sfd_ptr);
    function_data.m_source_text_owner = Utf16String::from_utf8(source_text);
    function_data.m_source_text = function_data.m_source_text_owner.utf16_view();

    return GC::Ref<SharedFunctionInstanceData> { function_data };
}

Optional<Vector<GC::Root<SharedFunctionInstanceData>>> compile_builtin_file(
    unsigned char const* script_text, VM& vm)
{
    bool const compare_pipelines = compare_pipelines_enabled();
    if (!rust_pipeline_enabled() && !compare_pipelines)
        return {};

    auto script_text_as_utf16 = Utf16String::from_utf8_without_validation({ script_text, strlen(reinterpret_cast<char const*>(script_text)) });
    auto code = SourceCode::create("BuiltinFile"_string, move(script_text_as_utf16));

    auto const& code_view = code->code_view();
    auto length = code_view.length_in_code_units();

    GC::DeferGC defer_gc(vm.heap());

    u8* rust_ast_data = nullptr;
    size_t rust_ast_len = 0;
    u8** rust_ast_data_ptr = compare_pipelines ? &rust_ast_data : nullptr;
    size_t* rust_ast_len_ptr = compare_pipelines ? &rust_ast_len : nullptr;

    Vector<GC::Root<SharedFunctionInstanceData>> shared_data_list;

    auto const* source_ptr = code->utf16_data();

    rust_compile_builtin_file(source_ptr, length, &vm, code.ptr(), &shared_data_list, collect_builtin_function,
        rust_ast_data_ptr, rust_ast_len_ptr);

    if (compare_pipelines) {
        auto rust_ast_dump = StringView { rust_ast_data, rust_ast_len };

        auto lexer = Lexer { code };
        auto parser = Parser { move(lexer) };
        auto cpp_program = parser.parse_program(true);

        if (!parser.has_errors()) {
            auto cpp_ast_dump = cpp_program->dump_to_string();
            compare_pipeline_asts(rust_ast_dump, cpp_ast_dump, "BuiltinFile"sv);
        }

        rust_free_string(rust_ast_data, rust_ast_len);
    }

    return shared_data_list;
}

GC::Ptr<Bytecode::Executable> compile_function(VM& vm, SharedFunctionInstanceData& shared_data, bool builtin_abstract_operations_enabled)
{
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
        builtin_abstract_operations_enabled));
    shared_data.m_rust_function_ast = nullptr;
    return exec;
}

void free_function_ast(void* ast)
{
    if (ast)
        rust_free_function_ast(ast);
}

}

// --- FFI factory functions (called by Rust to create C++ objects) ---

struct RustCompiledRegex {
    regex::Parser::Result parsed_regex;
    String parsed_pattern;
    regex::RegexOptions<ECMAScriptFlags> flags;
};

static Utf16View view_from_ffi(FFIUtf16Slice slice)
{
    return Utf16View { reinterpret_cast<char16_t const*>(slice.data), slice.length };
}

static Utf16String utf16_from_ffi(FFIUtf16Slice slice)
{
    return Utf16String::from_utf16(view_from_ffi(slice));
}

static Utf16FlyString utf16_fly_from_ffi(FFIUtf16Slice slice)
{
    return Utf16FlyString::from_utf16(view_from_ffi(slice));
}

static JS::Value decode_constant(JS::VM& vm, uint8_t const*& cursor, uint8_t const* end)
{
    VERIFY(cursor < end);
    auto tag = *cursor++;

    switch (tag) {
    case CONSTANT_TAG_NUMBER: {
        VERIFY(cursor + 8 <= end);
        double value;
        memcpy(&value, cursor, 8);
        cursor += 8;
        return JS::Value(value);
    }
    case CONSTANT_TAG_BOOLEAN_TRUE:
        return JS::Value(true);
    case CONSTANT_TAG_BOOLEAN_FALSE:
        return JS::Value(false);
    case CONSTANT_TAG_NULL:
        return JS::js_null();
    case CONSTANT_TAG_UNDEFINED:
        return JS::js_undefined();
    case CONSTANT_TAG_EMPTY:
        return JS::js_special_empty_value();
    case CONSTANT_TAG_STRING: {
        VERIFY(cursor + 4 <= end);
        uint32_t len;
        memcpy(&len, cursor, 4);
        cursor += 4;
        VERIFY(cursor + len * 2 <= end);
        // NB: cursor may not be 2-byte aligned, so copy to an aligned buffer.
        Vector<char16_t> aligned_buf;
        aligned_buf.resize(len);
        if (len > 0)
            memcpy(aligned_buf.data(), cursor, len * 2);
        auto str = Utf16String::from_utf16(Utf16View(aligned_buf.data(), len));
        cursor += len * 2;
        return JS::PrimitiveString::create(vm, move(str));
    }
    case CONSTANT_TAG_BIGINT: {
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
    case CONSTANT_TAG_RAW_VALUE: {
        VERIFY(cursor + 8 <= end);
        JS::Value value;
        memcpy(&value, cursor, 8);
        cursor += 8;
        return value;
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

extern "C" void* rust_create_executable(
    void* vm_ptr,
    void* source_code_ptr,
    FFIExecutableData const* data)
{
    auto& vm = *static_cast<JS::VM*>(vm_ptr);
    auto& source_code = *static_cast<JS::SourceCode const*>(source_code_ptr);

    // Build bytecode vector
    Vector<u8> bytecode_vec;
    bytecode_vec.append(data->bytecode, data->bytecode_length);

    // Build identifier table
    auto ident_table = make<JS::Bytecode::IdentifierTable>();
    for (size_t i = 0; i < data->identifier_count; ++i) {
        ident_table->insert(utf16_fly_from_ffi(data->identifier_table[i]));
    }

    // Build property key table
    auto prop_key_table = make<JS::Bytecode::PropertyKeyTable>();
    for (size_t i = 0; i < data->property_key_count; ++i) {
        prop_key_table->insert(utf16_fly_from_ffi(data->property_key_table[i]));
    }

    // Build string table
    auto str_table = make<JS::Bytecode::StringTable>();
    for (size_t i = 0; i < data->string_count; ++i) {
        str_table->insert(utf16_from_ffi(data->string_table[i]));
    }

    // Build regex table from pre-compiled regex objects
    auto regex_tbl = make<JS::Bytecode::RegexTable>();
    for (size_t i = 0; i < data->regex_count; ++i) {
        auto* cr = static_cast<RustCompiledRegex*>(data->compiled_regexes[i]);
        regex_tbl->insert(JS::Bytecode::ParsedRegex { move(cr->parsed_regex), move(cr->parsed_pattern), cr->flags });
        delete cr;
    }

    // Decode constants
    Vector<JS::Value> constants_vec;
    constants_vec.ensure_capacity(data->constants_count);
    auto const* cursor = data->constants_data;
    auto const* end = data->constants_data + data->constants_data_length;
    for (size_t i = 0; i < data->constants_count; ++i) {
        constants_vec.append(decode_constant(vm, cursor, end));
    }
    VERIFY(cursor == end);

    // Create executable
    auto executable = vm.heap().allocate<JS::Bytecode::Executable>(
        move(bytecode_vec),
        move(ident_table),
        move(prop_key_table),
        move(str_table),
        move(regex_tbl),
        move(constants_vec),
        source_code,
        data->property_lookup_cache_count,
        data->global_variable_cache_count,
        data->template_object_cache_count,
        data->object_shape_cache_count,
        data->number_of_registers,
        data->is_strict ? JS::Strict::Yes : JS::Strict::No);

    // Set exception handlers
    for (size_t i = 0; i < data->exception_handler_count; ++i) {
        executable->exception_handlers.append({
            data->exception_handlers[i].start_offset,
            data->exception_handlers[i].end_offset,
            data->exception_handlers[i].handler_offset,
        });
    }

    // Set source map
    for (size_t i = 0; i < data->source_map_count; ++i) {
        executable->source_map.append({
            data->source_map[i].bytecode_offset,
            { data->source_map[i].source_start, data->source_map[i].source_end },
        });
    }

    // Set basic block offsets
    for (size_t i = 0; i < data->basic_block_count; ++i) {
        executable->basic_block_start_offsets.append(data->basic_block_offsets[i]);
    }

    // Set local variable names
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

    // Set length identifier (for GetLength optimization)
    if (data->length_identifier.has_value)
        executable->length_identifier = JS::Bytecode::PropertyKeyTableIndex(data->length_identifier.value);

    // Set shared function data (inner function definitions)
    for (size_t i = 0; i < data->shared_function_data_count; ++i) {
        auto* sfd = const_cast<JS::SharedFunctionInstanceData*>(
            static_cast<JS::SharedFunctionInstanceData const*>(data->shared_function_data[i]));
        executable->shared_function_data.append(sfd);
    }

    // Set class blueprints (move from heap-allocated objects)
    for (size_t i = 0; i < data->class_blueprint_count; ++i) {
        auto* bp = static_cast<JS::Bytecode::ClassBlueprint*>(data->class_blueprints[i]);
        executable->class_blueprints.append(move(*bp));
        delete bp;
    }

    return executable.ptr();
}

extern "C" void* rust_create_sfd(
    void* vm_ptr,
    void const* source_code_ptr,
    FFISharedFunctionData const* data)
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
        data->rust_function_ast);

    // Set parsing insights that must be available before lazy compilation.
    shared->m_uses_this = data->uses_this;
    if (data->uses_this_from_environment)
        shared->m_function_environment_needed = true;

    // Set source text as a view into the original source code.
    shared->m_source_code = &source_code;
    if (data->source_text_length > 0) {
        auto const& code_view = source_code.code_view();
        shared->m_source_text = code_view.substring_view(data->source_text_offset, data->source_text_length);
    }

    return shared.ptr();
}

extern "C" void rust_sfd_set_metadata(
    void* sfd_ptr,
    bool uses_this,
    bool function_environment_needed,
    size_t function_environment_bindings_count,
    bool might_need_arguments_object,
    bool contains_direct_call_to_eval)
{
    auto& shared = *static_cast<JS::SharedFunctionInstanceData*>(sfd_ptr);
    shared.m_uses_this = uses_this;
    shared.m_function_environment_needed = function_environment_needed;
    shared.m_function_environment_bindings_count = function_environment_bindings_count;
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
    auto utf16_name = Utf16FlyString::from_utf16(Utf16View(reinterpret_cast<char16_t const*>(name), name_len));
    if (is_private) {
        shared.m_class_field_initializer_name = JS::PrivateName(0, utf16_name);
    } else {
        shared.m_class_field_initializer_name = JS::PropertyKey(utf16_name.to_utf16_string());
    }
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
        blueprint->name = Utf16FlyString::from_utf16(Utf16View(reinterpret_cast<char16_t const*>(name), name_len));

    // Store source text as a view into the SourceCode buffer.
    if (source_text_len > 0) {
        auto& source_code = *static_cast<JS::SourceCode const*>(source_code_ptr);
        auto const& code_view = source_code.code_view();
        blueprint->source_text = code_view.substring_view(source_text_offset, source_text_len);
    }

    for (size_t i = 0; i < element_count; ++i) {
        auto const& elem = elements[i];
        JS::Bytecode::ClassElementDescriptor desc;
        desc.kind = static_cast<JS::Bytecode::ClassElementDescriptor::Kind>(elem.kind);
        desc.is_static = elem.is_static;
        desc.is_private = elem.is_private;
        if (elem.private_identifier_len > 0)
            desc.private_identifier = Utf16FlyString::from_utf16(Utf16View(reinterpret_cast<char16_t const*>(elem.private_identifier), elem.private_identifier_len));
        if (elem.shared_function_data_index.has_value)
            desc.shared_function_data_index = elem.shared_function_data_index.value;
        desc.has_initializer = elem.has_initializer;
        switch (elem.literal_value_kind) {
        case 0: // none
            break;
        case 1: // number
            desc.literal_value = JS::Value(elem.literal_value_number);
            break;
        case 2: // boolean true
            desc.literal_value = JS::Value(true);
            break;
        case 3: // boolean false
            desc.literal_value = JS::Value(false);
            break;
        case 4: // null
            desc.literal_value = JS::js_null();
            break;
        case 5: { // string
            auto& vm = *static_cast<JS::VM*>(vm_ptr);
            auto str_view = Utf16View(reinterpret_cast<char16_t const*>(elem.literal_value_string), elem.literal_value_string_len);
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
    shared.m_name = Utf16FlyString::from_utf16(Utf16View(reinterpret_cast<char16_t const*>(name), name_len));
}

extern "C" void* rust_compile_regex(
    uint16_t const* pattern_data, size_t pattern_len,
    uint16_t const* flags_data, size_t flags_len,
    char const** error_out)
{
    *error_out = nullptr;
    auto pattern = Utf16View { reinterpret_cast<char16_t const*>(pattern_data), pattern_len };
    auto flags_view = Utf16View { reinterpret_cast<char16_t const*>(flags_data), flags_len };
    auto parsed_flags = JS::regex_flags_from_string(flags_view);
    auto ecma_flags = parsed_flags.is_error() ? regex::RegexOptions<ECMAScriptFlags> {} : parsed_flags.release_value();
    auto parsed_pattern = JS::parse_regex_pattern(pattern, ecma_flags.has_flag_set(ECMAScriptFlags::Unicode), ecma_flags.has_flag_set(ECMAScriptFlags::UnicodeSets));
    if (parsed_pattern.is_error()) {
        auto msg = MUST(String::formatted("RegExp compile error: {}", parsed_pattern.release_error().error));
        auto* buf = static_cast<char*>(malloc(msg.byte_count() + 1));
        memcpy(buf, msg.bytes().data(), msg.byte_count());
        buf[msg.byte_count()] = '\0';
        *error_out = buf;
        return nullptr;
    }
    auto pattern_str = parsed_pattern.release_value();
    auto parsed_regex = Regex<ECMA262>::parse_pattern(pattern_str, ecma_flags);
    if (parsed_regex.error != regex::Error::NoError) {
        auto error_string = Regex<ECMA262>(parsed_regex, ""sv, ecma_flags).error_string();
        auto msg = MUST(String::formatted("RegExp compile error: {}", error_string));
        auto* buf = static_cast<char*>(malloc(msg.byte_count() + 1));
        memcpy(buf, msg.bytes().data(), msg.byte_count());
        buf[msg.byte_count()] = '\0';
        *error_out = buf;
        return nullptr;
    }
    return new RustCompiledRegex { move(parsed_regex), move(pattern_str), ecma_flags };
}

extern "C" void rust_free_compiled_regex(void* ptr)
{
    delete static_cast<RustCompiledRegex*>(ptr);
}

extern "C" void rust_free_error_string(char const* str)
{
    free(const_cast<char*>(str));
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

extern "C" uint64_t get_well_known_symbol(void* vm_ptr, uint32_t symbol_id)
{
    auto& vm = *static_cast<JS::VM*>(vm_ptr);
    JS::Value value;
    switch (symbol_id) {
    case 0:
        value = vm.well_known_symbol_iterator();
        break;
    case 1:
        value = vm.well_known_symbol_async_iterator();
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    return value.encoded();
}

extern "C" uint64_t get_abstract_operation_function(void* vm_ptr, uint16_t const* name, size_t name_len)
{
    auto& vm = *static_cast<JS::VM*>(vm_ptr);
    auto& intrinsics = vm.current_realm()->intrinsics();
    auto name_view = Utf16View(reinterpret_cast<char16_t const*>(name), name_len);
    auto name_str = MUST(name_view.to_utf8());

#    define __JS_ENUMERATE(snake_name, functionName, length) \
        if (name_str == #functionName##sv)                   \
            return JS::Value(intrinsics.snake_name##_abstract_operation_function().ptr()).encoded();
    JS_ENUMERATE_NATIVE_JAVASCRIPT_BACKED_ABSTRACT_OPERATIONS
#    undef __JS_ENUMERATE

    VERIFY_NOT_REACHED();
}

#else // !ENABLE_RUST

namespace JS::RustIntegration {

Optional<Result<ScriptResult, Vector<ParserError>>> compile_script(StringView, Realm&, StringView, size_t)
{
    return {};
}

Optional<Result<EvalResult, String>> compile_eval(PrimitiveString&, VM&, CallerMode, bool, bool, bool, bool)
{
    return {};
}

Optional<Result<EvalResult, String>> compile_shadow_realm_eval(PrimitiveString&, VM&)
{
    return {};
}

Optional<Result<ModuleResult, Vector<ParserError>>> compile_module(StringView, Realm&, StringView)
{
    return {};
}

Optional<Result<GC::Ref<SharedFunctionInstanceData>, String>> compile_dynamic_function(VM&, StringView, StringView, StringView, FunctionKind)
{
    return {};
}

Optional<Vector<GC::Root<SharedFunctionInstanceData>>> compile_builtin_file(unsigned char const*, VM&)
{
    return {};
}

GC::Ptr<Bytecode::Executable> compile_function(VM&, SharedFunctionInstanceData&, bool)
{
    return nullptr;
}

void free_function_ast(void*) { }

}

#endif // ENABLE_RUST
