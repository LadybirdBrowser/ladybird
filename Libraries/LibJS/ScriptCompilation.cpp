/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/RustIntegration.h>
#include <LibJS/ScriptCompilation.h>
#include <LibJS/SourceTextModule.h>

namespace JS {

CompiledDynamicFunction::CompiledDynamicFunction(GC::Ref<SharedFunctionInstanceData> function_data)
    : m_function_data(function_data)
{
}

Result<CompiledDynamicFunction, Utf16String> CompiledDynamicFunction::compile(VM& vm, Utf16View source_text, Utf16View parameters_string, Utf16View body_parse_string, FunctionKind kind)
{
    auto compilation = RustIntegration::compile_dynamic_function(vm, source_text, parameters_string, body_parse_string, kind);
    if (!compilation.has_value())
        return "Failed to compile dynamic function"_utf16;
    if (compilation->is_error())
        return compilation->release_error();
    return CompiledDynamicFunction { compilation->value() };
}

GC::Ref<FunctionObject> CompiledDynamicFunction::instantiate(Realm& realm, Environment& scope, GC::Ptr<PrivateEnvironment> private_environment, ScriptOrModule script_or_module) const
{
    auto function = ECMAScriptFunctionObject::create_from_function_data(realm, *m_function_data, scope, private_environment);
    function->set_script_or_module(move(script_or_module));
    return function;
}

Vector<Position> breakpoint_positions_for_source(SourceCode const& source_code, ProgramType type, size_t line_number_offset)
{
    return RustIntegration::breakpoint_positions_for_source(source_code, type, line_number_offset);
}

ParsedProgram::ParsedProgram(FFI::ParsedProgram* program, size_t source_length_in_code_units)
    : m_program(program)
    , m_source_length_in_code_units(source_length_in_code_units)
{
}

ParsedProgram::ParsedProgram(ParsedProgram&& other)
    : m_program(exchange(other.m_program, nullptr))
    , m_source_length_in_code_units(other.m_source_length_in_code_units)
{
}

ParsedProgram& ParsedProgram::operator=(ParsedProgram&& other)
{
    if (this != &other) {
        if (m_program)
            RustIntegration::free_parsed_program(m_program);
        m_program = exchange(other.m_program, nullptr);
        m_source_length_in_code_units = other.m_source_length_in_code_units;
    }
    return *this;
}

ParsedProgram::~ParsedProgram()
{
    if (m_program)
        RustIntegration::free_parsed_program(m_program);
}

ParsedProgram ParsedProgram::parse(ReadonlySpan<u16> source_text, ProgramType type, size_t line_number_offset)
{
    return { RustIntegration::parse_program(source_text.data(), source_text.size(), type, line_number_offset), source_text.size() };
}

bool ParsedProgram::has_errors() const
{
    return RustIntegration::parsed_program_has_errors(m_program);
}

ParsedProgram ParsedProgram::clone() const
{
    return { RustIntegration::clone_parsed_program(m_program), m_source_length_in_code_units };
}

CompiledProgram::CompiledProgram(FFI::CompiledProgram* program)
    : m_program(program)
{
}

CompiledProgram::CompiledProgram(CompiledProgram&& other)
    : m_program(exchange(other.m_program, nullptr))
{
}

CompiledProgram& CompiledProgram::operator=(CompiledProgram&& other)
{
    if (this != &other) {
        if (m_program)
            RustIntegration::free_compiled_program(m_program);
        m_program = exchange(other.m_program, nullptr);
    }
    return *this;
}

CompiledProgram::~CompiledProgram()
{
    if (m_program)
        RustIntegration::free_compiled_program(m_program);
}

CompiledProgram CompiledProgram::compile(ParsedProgram parsed)
{
    return CompiledProgram { RustIntegration::compile_parsed_program_off_thread(exchange(parsed.m_program, nullptr), parsed.m_source_length_in_code_units) };
}

CompiledProgram CompiledProgram::compile_all_functions(ParsedProgram parsed)
{
    return CompiledProgram { RustIntegration::compile_parsed_program_fully_off_thread(exchange(parsed.m_program, nullptr), parsed.m_source_length_in_code_units) };
}

ByteBuffer CompiledProgram::serialize_for_bytecode_cache(ProgramType type, ReadonlyBytes source_hash) const
{
    return RustIntegration::serialize_compiled_program_for_bytecode_cache(*m_program, type, source_hash);
}

Result<GC::Ref<Script>, Vector<ParserError>> create_script(ParsedProgram parsed, NonnullRefPtr<SourceCode const> source_code, Realm& realm, StringView filename, GC::Ptr<GC::Cell> host_defined)
{
    return Script::create_from_parsed(exchange(parsed.m_program, nullptr), move(source_code), realm, filename, host_defined);
}

Result<GC::Ref<Script>, Vector<ParserError>> create_script(CompiledProgram compiled, NonnullRefPtr<SourceCode const> source_code, Realm& realm, StringView filename, GC::Ptr<GC::Cell> host_defined)
{
    return Script::create_from_compiled(exchange(compiled.m_program, nullptr), move(source_code), realm, filename, host_defined);
}

Result<GC::Ref<SourceTextModule>, Vector<ParserError>> create_module(ParsedProgram parsed, NonnullRefPtr<SourceCode const> source_code, Realm& realm, StringView filename, GC::Ptr<GC::Cell> host_defined)
{
    return SourceTextModule::parse_from_pre_parsed(exchange(parsed.m_program, nullptr), move(source_code), realm, filename, host_defined);
}

Result<GC::Ref<SourceTextModule>, Vector<ParserError>> create_module(CompiledProgram compiled, NonnullRefPtr<SourceCode const> source_code, Realm& realm, StringView filename, GC::Ptr<GC::Cell> host_defined)
{
    return SourceTextModule::parse_from_pre_compiled(exchange(compiled.m_program, nullptr), move(source_code), realm, filename, host_defined);
}

RefPtr<DecodedBytecodeCache> decode_and_validate_bytecode_cache(Core::ImmutableBytes bytes, ProgramType type, ReadonlyBytes source_hash, size_t source_length_in_code_units, Core::EventLoop& main_thread_event_loop)
{
    auto* blob = RustIntegration::decode_bytecode_cache_blob(move(bytes), type, source_hash, main_thread_event_loop);
    if (!blob)
        return {};
    if (!RustIntegration::validate_decoded_bytecode_cache_blob(blob, source_length_in_code_units)) {
        RustIntegration::free_decoded_bytecode_cache_blob(blob);
        return {};
    }
    return DecodedBytecodeCache::create(blob);
}

// The worker can still be inside post_to_main_thread() when the main thread runs the posted task and moves on, so each
// side holds its own reference to the callbacks.
class SharedOffThreadCompilationCallbacks final : public AtomicRefCounted<SharedOffThreadCompilationCallbacks> {
public:
    explicit SharedOffThreadCompilationCallbacks(OffThreadCompilationCallbacks callbacks)
        : callbacks(move(callbacks))
    {
    }

    OffThreadCompilationCallbacks callbacks;
};

struct LazyFunctionsToCompile {
    Vector<GC::Root<SharedFunctionInstanceData>> shared_function_data;
    Vector<void*> function_asts;
};

static LazyFunctionsToCompile lazy_functions_to_compile(Bytecode::Executable const& executable)
{
    LazyFunctionsToCompile functions;
    for (auto& shared_data : executable.shared_function_data) {
        if (!shared_data || shared_data->m_executable || !shared_data->m_rust_function_ast)
            continue;

        auto* cloned_ast = RustIntegration::clone_function_ast(shared_data->m_rust_function_ast);
        if (!cloned_ast)
            continue;

        functions.shared_function_data.append(GC::make_root(*shared_data));
        functions.function_asts.append(cloned_ast);
    }
    return functions;
}

static void compile_lazy_functions_off_thread(VM& vm, LazyFunctionsToCompile functions, NonnullRefPtr<SourceCode const> source_code, NonnullRefPtr<SharedOffThreadCompilationCallbacks> shared_callbacks)
{
    auto length = source_code->length_in_code_units();

    // NB: The GC roots must only be touched on the main thread, so the worker only carries a pointer to them.
    auto* on_compiled = new Function<void(Vector<FFI::CompiledFunction*>)>(
        [&vm, shared_function_data = move(functions.shared_function_data), source_code = move(source_code), shared_callbacks](Vector<FFI::CompiledFunction*> compiled_functions) mutable {
            VERIFY(compiled_functions.size() == shared_function_data.size());
            for (size_t i = 0; i < compiled_functions.size(); ++i) {
                auto* compiled_function = compiled_functions[i];
                if (!compiled_function)
                    continue;

                auto& shared_data = *shared_function_data[i];
                if (shared_data.m_executable) {
                    auto nested_functions = lazy_functions_to_compile(*shared_data.m_executable);
                    if (!nested_functions.function_asts.is_empty())
                        compile_lazy_functions_off_thread(vm, move(nested_functions), source_code, shared_callbacks);
                    RustIntegration::free_compiled_function(compiled_function);
                    continue;
                }

                // Bytecode-cache installation may have cleared this while the worker compiled an AST clone.
                if (!shared_data.m_rust_function_ast) {
                    RustIntegration::free_compiled_function(compiled_function);
                    continue;
                }

                RustIntegration::materialize_compiled_function(compiled_function, vm, *source_code, shared_data);
            }
        });

    shared_callbacks->callbacks.submit_work([function_asts = move(functions.function_asts), length, on_compiled, shared_callbacks]() mutable {
        Vector<FFI::CompiledFunction*> compiled_functions;
        compiled_functions.ensure_capacity(function_asts.size());
        for (auto* function_ast : function_asts)
            compiled_functions.append(RustIntegration::compile_function_off_thread(function_ast, length, false));

        shared_callbacks->callbacks.post_to_main_thread([compiled_functions = move(compiled_functions), on_compiled]() mutable {
            (*on_compiled)(move(compiled_functions));
            delete on_compiled;
        });
    });
}

static void compile_remaining_functions_off_thread(VM& vm, Bytecode::Executable const& executable, NonnullRefPtr<SourceCode const> source_code, OffThreadCompilationCallbacks callbacks)
{
    auto functions = lazy_functions_to_compile(executable);
    if (functions.function_asts.is_empty())
        return;
    compile_lazy_functions_off_thread(vm, move(functions), move(source_code), adopt_ref(*new SharedOffThreadCompilationCallbacks(move(callbacks))));
}

void compile_remaining_functions_off_thread(Script& script, NonnullRefPtr<SourceCode const> source_code, OffThreadCompilationCallbacks callbacks)
{
    if (auto* executable = script.cached_executable())
        compile_remaining_functions_off_thread(script.realm().vm(), *executable, move(source_code), move(callbacks));
}

void compile_remaining_functions_off_thread(SourceTextModule& module, NonnullRefPtr<SourceCode const> source_code, OffThreadCompilationCallbacks callbacks)
{
    if (auto* executable = module.cached_executable()) {
        compile_remaining_functions_off_thread(module.realm().vm(), *executable, move(source_code), move(callbacks));
        return;
    }

    auto* top_level_await_shared_data = module.top_level_await_shared_data();
    if (!top_level_await_shared_data || !top_level_await_shared_data->m_executable)
        return;
    compile_remaining_functions_off_thread(module.realm().vm(), *top_level_await_shared_data->m_executable, move(source_code), move(callbacks));
}

RefPtr<SourceCode const> top_level_source_code(Script const& script)
{
    if (auto* executable = script.cached_executable())
        return executable->source_code;
    return {};
}

RefPtr<SourceCode const> top_level_source_code(SourceTextModule const& module)
{
    if (auto* executable = module.cached_executable())
        return executable->source_code;
    return {};
}

}
