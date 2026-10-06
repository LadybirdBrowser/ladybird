/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/ScriptAndModuleABIConversions.h>
#include <LibJS/ScriptCompilation.h>
#include <LibJS/SourceTextModule.h>

namespace JS {

using namespace EmbeddingABI;

static_assert(to_underlying(FunctionKind::Normal) == JS_FUNCTION_KIND_NORMAL);
static_assert(to_underlying(FunctionKind::Generator) == JS_FUNCTION_KIND_GENERATOR);
static_assert(to_underlying(FunctionKind::Async) == JS_FUNCTION_KIND_ASYNC);
static_assert(to_underlying(FunctionKind::AsyncGenerator) == JS_FUNCTION_KIND_ASYNC_GENERATOR);
static_assert(sizeof(ScriptOrModule) == sizeof(JSScriptOrModule));
static_assert(sizeof(Position) == sizeof(JSPosition));
static_assert(offsetof(Position, line) == offsetof(JSPosition, line));
static_assert(offsetof(Position, column) == offsetof(JSPosition, column));

static JSParsedProgram* parsed_program_to_abi(FFI::ParsedProgram* program)
{
    return reinterpret_cast<JSParsedProgram*>(program);
}

static FFI::ParsedProgram* parsed_program_from_abi(JSParsedProgram* program)
{
    return reinterpret_cast<FFI::ParsedProgram*>(program);
}

static JSCompiledProgram* compiled_program_to_abi(FFI::CompiledProgram* program)
{
    return reinterpret_cast<JSCompiledProgram*>(program);
}

static FFI::CompiledProgram* compiled_program_from_abi(JSCompiledProgram* program)
{
    return reinterpret_cast<FFI::CompiledProgram*>(program);
}

CompiledDynamicFunction::CompiledDynamicFunction(GC::Ref<FunctionData> function_data)
    : m_function_data(function_data)
{
}

Result<CompiledDynamicFunction, Utf16String> CompiledDynamicFunction::compile(VM& vm, Utf16View source_text, Utf16View parameters_string, Utf16View body_parse_string, FunctionKind kind)
{
    JSOwnedUtf16String error_message {};
    auto* function_data = js_function_compile_dynamic(vm_to_abi(vm), utf16_view_to_abi(source_text), utf16_view_to_abi(parameters_string), utf16_view_to_abi(body_parse_string), to_underlying(kind), &error_message);
    if (!function_data)
        return owned_utf16_string_from_abi(error_message);
    return CompiledDynamicFunction { *reinterpret_cast<FunctionData*>(function_data) };
}

GC::Ref<FunctionObject> CompiledDynamicFunction::instantiate(Realm& realm, Environment& scope, GC::Ptr<PrivateEnvironment> private_environment, ScriptOrModule script_or_module) const
{
    auto* function = js_function_instantiate_dynamic(
        vm_to_abi(realm.vm()),
        realm_to_abi(realm),
        reinterpret_cast<JSSharedFunctionData*>(m_function_data.ptr()),
        reinterpret_cast<JSEnvironment*>(&scope),
        reinterpret_cast<JSPrivateEnvironment*>(private_environment.ptr()),
        bit_cast<JSScriptOrModule>(script_or_module));
    return static_cast<FunctionObject&>(object_from_abi(function));
}

Vector<Position> breakpoint_positions_for_source(SourceCode const& source_code, ProgramType program_type, size_t line_number_offset)
{
    Vector<Position> positions;
    JSPositionSink sink {
        .context = &positions,
        .append = [](void* context, JSPosition const* abi_positions, size_t count) {
            static_cast<Vector<Position>*>(context)->append(reinterpret_cast<Position const*>(abi_positions), count);
        },
    };
    js_compile_breakpoint_positions_for_source(utf16_view_to_abi(source_code.code().utf16_view()), program_type_to_abi(program_type), line_number_offset, &sink);
    return positions;
}

ParsedProgram::ParsedProgram(FFI::ParsedProgram* program)
    : m_program(program)
{
}

ParsedProgram::ParsedProgram(ParsedProgram&& other)
    : m_program(exchange(other.m_program, nullptr))
{
}

ParsedProgram& ParsedProgram::operator=(ParsedProgram&& other)
{
    if (this != &other) {
        js_compile_parsed_program_destroy(parsed_program_to_abi(m_program));
        m_program = exchange(other.m_program, nullptr);
    }
    return *this;
}

ParsedProgram::~ParsedProgram()
{
    js_compile_parsed_program_destroy(parsed_program_to_abi(m_program));
}

ParsedProgram ParsedProgram::parse(ReadonlySpan<u16> source_text, ProgramType program_type, size_t line_number_offset)
{
    JSUtf16View source { source_text.data(), source_text.size(), false };
    return ParsedProgram { parsed_program_from_abi(js_compile_parse(source, program_type_to_abi(program_type), line_number_offset)) };
}

bool ParsedProgram::has_errors() const
{
    return js_compile_parsed_program_has_errors(parsed_program_to_abi(m_program));
}

ParsedProgram ParsedProgram::clone() const
{
    return ParsedProgram { parsed_program_from_abi(js_compile_parsed_program_clone(parsed_program_to_abi(m_program))) };
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
        js_compile_compiled_program_destroy(compiled_program_to_abi(m_program));
        m_program = exchange(other.m_program, nullptr);
    }
    return *this;
}

CompiledProgram::~CompiledProgram()
{
    js_compile_compiled_program_destroy(compiled_program_to_abi(m_program));
}

CompiledProgram CompiledProgram::compile(ParsedProgram parsed)
{
    auto* program = parsed_program_to_abi(exchange(parsed.m_program, nullptr));
    return CompiledProgram { compiled_program_from_abi(js_compile_parsed_program(program)) };
}

CompiledProgram CompiledProgram::compile_all_functions(ParsedProgram parsed)
{
    auto* program = parsed_program_to_abi(exchange(parsed.m_program, nullptr));
    return CompiledProgram { compiled_program_from_abi(js_compile_parsed_program_with_all_functions(program)) };
}

ByteBuffer CompiledProgram::serialize_for_bytecode_cache(ProgramType program_type, ReadonlyBytes source_hash) const
{
    ByteBuffer blob;
    JSByteSink sink {
        .context = &blob,
        .append = [](void* context, u8 const* bytes, size_t length) {
            return !static_cast<ByteBuffer*>(context)->try_append(bytes, length).is_error();
        },
    };
    if (!js_bytecode_cache_serialize(compiled_program_to_abi(m_program), program_type_to_abi(program_type), source_hash.data(), source_hash.size(), &sink))
        return {};
    return blob;
}

Result<GC::Ref<Script>, Vector<ParserError>> create_script(ParsedProgram parsed, NonnullRefPtr<SourceCode const> source_code, Realm& realm, StringView filename, GC::Ptr<GC::Cell> host_defined)
{
    ParserErrorCollector errors;
    auto utf16_filename = filename_to_utf16(filename);
    auto* script = js_compile_create_script_from_parsed_program(
        vm_to_abi(realm.vm()),
        parsed_program_to_abi(exchange(parsed.m_program, nullptr)),
        source_code_to_abi(*source_code),
        realm_to_abi(realm),
        utf16_view_to_abi(utf16_filename.utf16_view()),
        host_defined.ptr(),
        errors.sink());
    return record_or_parser_errors_from_abi<Script>(script, errors);
}

Result<GC::Ref<Script>, Vector<ParserError>> create_script(CompiledProgram compiled, NonnullRefPtr<SourceCode const> source_code, Realm& realm, StringView filename, GC::Ptr<GC::Cell> host_defined)
{
    auto utf16_filename = filename_to_utf16(filename);
    auto* script = js_compile_create_script_from_compiled_program(
        vm_to_abi(realm.vm()),
        compiled_program_to_abi(exchange(compiled.m_program, nullptr)),
        source_code_to_abi(*source_code),
        realm_to_abi(realm),
        utf16_view_to_abi(utf16_filename.utf16_view()),
        host_defined.ptr());
    return script_from_abi(script);
}

Result<GC::Ref<SourceTextModule>, Vector<ParserError>> create_module(ParsedProgram parsed, NonnullRefPtr<SourceCode const> source_code, Realm& realm, StringView filename, GC::Ptr<GC::Cell> host_defined)
{
    ParserErrorCollector errors;
    auto utf16_filename = filename_to_utf16(filename);
    auto* module = js_compile_create_module_from_parsed_program(
        vm_to_abi(realm.vm()),
        parsed_program_to_abi(exchange(parsed.m_program, nullptr)),
        source_code_to_abi(*source_code),
        realm_to_abi(realm),
        utf16_view_to_abi(utf16_filename.utf16_view()),
        host_defined.ptr(),
        errors.sink());
    return record_or_parser_errors_from_abi<SourceTextModule>(module, errors);
}

Result<GC::Ref<SourceTextModule>, Vector<ParserError>> create_module(CompiledProgram compiled, NonnullRefPtr<SourceCode const> source_code, Realm& realm, StringView filename, GC::Ptr<GC::Cell> host_defined)
{
    auto utf16_filename = filename_to_utf16(filename);
    auto* module = js_compile_create_module_from_compiled_program(
        vm_to_abi(realm.vm()),
        compiled_program_to_abi(exchange(compiled.m_program, nullptr)),
        source_code_to_abi(*source_code),
        realm_to_abi(realm),
        utf16_view_to_abi(utf16_filename.utf16_view()),
        host_defined.ptr());
    return module_from_abi<SourceTextModule>(module);
}

RefPtr<DecodedBytecodeCache> decode_and_validate_bytecode_cache(Core::ImmutableBytes bytes, ProgramType program_type, ReadonlyBytes source_hash, size_t source_length_in_code_units, Core::EventLoop& main_thread_event_loop)
{
    // The runtime may release the bytes on this thread, so their last reference goes on the main thread, as the C++
    // runtime releases them.
    struct BlobBytesOwner {
        AK_ALLOC_WITH_KMALLOC;

        Core::ImmutableBytes bytes;
        Core::EventLoop& main_thread_event_loop;
    };
    auto* bytes_owner = new BlobBytesOwner { move(bytes), main_thread_event_loop };
    JSBytecodeCacheBlobOwner owner {
        .owner = bytes_owner,
        .release = [](void* owner) {
            auto* bytes_owner = static_cast<BlobBytesOwner*>(owner);
            bytes_owner->main_thread_event_loop.deferred_invoke([bytes_owner] { delete bytes_owner; });
        },
    };
    auto blob = bytes_owner->bytes.bytes();
    return adopt_decoded_bytecode_cache_from_abi(js_bytecode_cache_decode_and_validate(blob.data(), blob.size(), program_type_to_abi(program_type), source_hash.data(), source_hash.size(), source_length_in_code_units, owner));
}

// The host's callbacks, which the runtime shares between the main thread and the workers until it releases them.
static JSOffThreadCompilationCallbacks off_thread_compilation_callbacks_to_abi(OffThreadCompilationCallbacks callbacks)
{
    return JSOffThreadCompilationCallbacks {
        .context = new OffThreadCompilationCallbacks(move(callbacks)),
        .submit_work = [](void* context, JSOffThreadTask task) { static_cast<OffThreadCompilationCallbacks*>(context)->submit_work([task] { task.run(task.data); }); },
        .post_to_main_thread = [](void* context, JSOffThreadTask task) { static_cast<OffThreadCompilationCallbacks*>(context)->post_to_main_thread([task] { task.run(task.data); }); },
        .release = [](void* context) { delete static_cast<OffThreadCompilationCallbacks*>(context); },
    };
}

void compile_remaining_functions_off_thread(Script& script, NonnullRefPtr<SourceCode const>, OffThreadCompilationCallbacks callbacks)
{
    auto abi_callbacks = off_thread_compilation_callbacks_to_abi(move(callbacks));
    js_compile_remaining_functions_of_script_off_thread(vm_to_abi(script.vm()), script_to_abi(script), &abi_callbacks);
}

void compile_remaining_functions_off_thread(SourceTextModule& module, NonnullRefPtr<SourceCode const>, OffThreadCompilationCallbacks callbacks)
{
    auto abi_callbacks = off_thread_compilation_callbacks_to_abi(move(callbacks));
    js_compile_remaining_functions_of_module_off_thread(vm_to_abi(module.vm()), module_to_abi(module), &abi_callbacks);
}

RefPtr<SourceCode const> top_level_source_code(Script const& script)
{
    return source_code_from_abi(js_compile_top_level_source_code_of_script(script_to_abi(script)));
}

RefPtr<SourceCode const> top_level_source_code(SourceTextModule const& module)
{
    return source_code_from_abi(js_compile_top_level_source_code_of_module(module_to_abi(module)));
}

}
