/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibCore/ImmutableBytes.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/RustIntegration.h>
#include <LibJS/Script.h>
#include <LibJS/SourceCode.h>
#include <LibJS/SourceTextModule.h>
#include <LibTest/TestCase.h>

struct BytecodeCacheTestData {
    NonnullRefPtr<JS::SourceCode const> source_code;
    ByteBuffer blob;
    Crypto::Hash::SHA256::DigestType source_hash;
};

class BytecodeCacheBlobReader {
public:
    explicit BytecodeCacheBlobReader(ReadonlyBytes bytes)
        : m_bytes(bytes)
    {
    }

    void skip(size_t length)
    {
        VERIFY(m_offset + length <= m_bytes.size());
        m_offset += length;
    }

    void align_to(size_t alignment)
    {
        auto padding = (alignment - (m_offset % alignment)) % alignment;
        skip(padding);
    }

    bool read_bool()
    {
        return read_u8() != 0;
    }

    u8 read_u8()
    {
        VERIFY(m_offset < m_bytes.size());
        return m_bytes[m_offset++];
    }

    u32 read_u32()
    {
        VERIFY(m_offset + sizeof(u32) <= m_bytes.size());
        auto value = static_cast<u32>(m_bytes[m_offset])
            | (static_cast<u32>(m_bytes[m_offset + 1]) << 8)
            | (static_cast<u32>(m_bytes[m_offset + 2]) << 16)
            | (static_cast<u32>(m_bytes[m_offset + 3]) << 24);
        m_offset += sizeof(u32);
        return value;
    }

    void skip_utf16()
    {
        align_to(alignof(u16));
        auto length = read_u32();
        skip(length * sizeof(u16));
    }

    void skip_utf16_vector()
    {
        auto length = read_u32();
        for (u32 i = 0; i < length; ++i)
            skip_utf16();
    }

    void skip_optional_utf16()
    {
        if (read_bool())
            skip_utf16();
    }

    void skip_optional_u32()
    {
        if (read_bool())
            skip(sizeof(u32));
    }

    size_t offset() const { return m_offset; }

private:
    ReadonlyBytes m_bytes;
    size_t m_offset { 0 };
};

static void write_u32(ByteBuffer& bytes, size_t offset, u32 value)
{
    VERIFY(offset + sizeof(u32) <= bytes.size());
    bytes[offset] = value & 0xff;
    bytes[offset + 1] = (value >> 8) & 0xff;
    bytes[offset + 2] = (value >> 16) & 0xff;
    bytes[offset + 3] = (value >> 24) & 0xff;
}

static BytecodeCacheTestData create_bytecode_cache_blob(StringView source)
{
    auto source_code = JS::SourceCode::create("test.js"_string, Utf16String::from_utf8(source));
    auto source_hash = Crypto::Hash::SHA256::hash(reinterpret_cast<u8 const*>(source_code->utf16_data()), source_code->length_in_code_units() * sizeof(u16));

    auto* parsed = JS::RustIntegration::parse_program(source_code->utf16_data(), source_code->length_in_code_units(), JS::RustIntegration::ProgramType::Script);
    VERIFY(parsed);
    ArmedScopeGuard free_parsed = [&] {
        JS::RustIntegration::free_parsed_program(parsed);
    };
    EXPECT(!JS::RustIntegration::parsed_program_has_errors(parsed));

    auto* compiled = JS::RustIntegration::compile_parsed_program_fully_off_thread(parsed, source_code->length_in_code_units());
    VERIFY(compiled);
    free_parsed.disarm();
    ScopeGuard free_compiled = [&] {
        JS::RustIntegration::free_compiled_program(compiled);
    };

    auto blob = JS::RustIntegration::serialize_compiled_program_for_bytecode_cache(*compiled, JS::RustIntegration::ProgramType::Script, source_hash.bytes());
    VERIFY(!blob.is_empty());

    return {
        .source_code = source_code,
        .blob = move(blob),
        .source_hash = source_hash,
    };
}

static BytecodeCacheTestData create_module_bytecode_cache_blob(StringView source)
{
    auto source_code = JS::SourceCode::create("test.mjs"_string, Utf16String::from_utf8(source));
    auto source_hash = Crypto::Hash::SHA256::hash(reinterpret_cast<u8 const*>(source_code->utf16_data()), source_code->length_in_code_units() * sizeof(u16));

    auto* parsed = JS::RustIntegration::parse_program(source_code->utf16_data(), source_code->length_in_code_units(), JS::RustIntegration::ProgramType::Module);
    VERIFY(parsed);
    ArmedScopeGuard free_parsed = [&] {
        JS::RustIntegration::free_parsed_program(parsed);
    };
    EXPECT(!JS::RustIntegration::parsed_program_has_errors(parsed));

    auto* compiled = JS::RustIntegration::compile_parsed_program_fully_off_thread(parsed, source_code->length_in_code_units());
    VERIFY(compiled);
    free_parsed.disarm();
    ScopeGuard free_compiled = [&] {
        JS::RustIntegration::free_compiled_program(compiled);
    };

    auto blob = JS::RustIntegration::serialize_compiled_program_for_bytecode_cache(*compiled, JS::RustIntegration::ProgramType::Module, source_hash.bytes());
    VERIFY(!blob.is_empty());

    return {
        .source_code = source_code,
        .blob = move(blob),
        .source_hash = source_hash,
    };
}

static size_t first_declaration_function_bytecode_payload_offset(ReadonlyBytes blob)
{
    BytecodeCacheBlobReader reader { blob };

    reader.skip(8);           // Magic.
    reader.skip(sizeof(u32)); // Format version.
    reader.skip(1);           // Program type.
    reader.skip(32);          // Source hash.
    reader.skip(1);           // Has top-level await.
    reader.skip(1);           // Is strict mode.

    EXPECT_EQ(reader.read_u8(), 0); // Script declaration metadata.
    reader.skip_utf16_vector();     // Lexical names.
    reader.skip_utf16_vector();     // Var names.
    reader.skip_utf16_vector();     // Function names.
    reader.skip_utf16_vector();     // Var-scoped names.
    reader.skip_utf16_vector();     // Annex B candidate names.

    auto lexical_binding_count = reader.read_u32();
    for (u32 i = 0; i < lexical_binding_count; ++i) {
        reader.skip_utf16();
        reader.skip(1);
    }

    auto declaration_function_count = reader.read_u32();
    VERIFY(declaration_function_count > 0);

    reader.skip_optional_utf16(); // Function name.
    reader.skip(sizeof(u32));     // Source text start.
    reader.skip(sizeof(u32));     // Source text end.
    reader.skip(sizeof(i32));     // Function length.
    reader.skip(sizeof(u32));     // Formal parameter count.
    reader.skip(1);               // Function kind.
    reader.skip(1);               // Strict mode.
    reader.skip(1);               // Arrow function.

    if (reader.read_bool())
        reader.skip_utf16_vector();

    reader.skip(1); // Uses this.
    reader.skip(1); // Uses this from environment.
    if (reader.read_bool()) {
        reader.skip_utf16();
        reader.skip(1);
    }

    reader.skip(3);           // Function metadata bools.
    reader.skip(sizeof(u64)); // Function environment bindings count.
    reader.skip(sizeof(u64)); // Var environment bindings count.
    reader.skip(2);           // Function metadata bools.

    reader.align_to(alignof(u16));
    auto executable_length = reader.read_u32();
    VERIFY(executable_length > 0);

    reader.skip(1);               // Executable strict mode.
    reader.skip(sizeof(u32));     // Number of registers.
    reader.skip(sizeof(u32));     // Number of arguments.
    reader.skip(5 * sizeof(u32)); // Cache counters.
    reader.skip(1);               // This value needs environment resolution.
    reader.skip_optional_u32();   // Length identifier.

    auto bytecode_length = reader.read_u32();
    VERIFY(bytecode_length > 0);
    return reader.offset();
}

static size_t first_declaration_function_source_text_start_offset(ReadonlyBytes blob)
{
    BytecodeCacheBlobReader reader { blob };

    reader.skip(8);           // Magic.
    reader.skip(sizeof(u32)); // Format version.
    reader.skip(1);           // Program type.
    reader.skip(32);          // Source hash.
    reader.skip(1);           // Has top-level await.
    reader.skip(1);           // Is strict mode.

    EXPECT_EQ(reader.read_u8(), 0); // Script declaration metadata.
    reader.skip_utf16_vector();     // Lexical names.
    reader.skip_utf16_vector();     // Var names.
    reader.skip_utf16_vector();     // Function names.
    reader.skip_utf16_vector();     // Var-scoped names.
    reader.skip_utf16_vector();     // Annex B candidate names.

    auto lexical_binding_count = reader.read_u32();
    for (u32 i = 0; i < lexical_binding_count; ++i) {
        reader.skip_utf16();
        reader.skip(1);
    }

    auto declaration_function_count = reader.read_u32();
    VERIFY(declaration_function_count > 0);

    reader.skip_optional_utf16(); // Function name.
    return reader.offset();
}

TEST_CASE(bytecode_cache_materialization_failure_has_parser_error)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto test_data = create_bytecode_cache_blob("1;"sv);

    auto corrupted_blob = MUST(ByteBuffer::copy(test_data.blob.bytes()));

    // For this minimal script, the encoded top-level bytecode payload begins
    // after the cache header, empty declaration metadata, and executable
    // metadata. Corrupting the payload keeps the blob structurally decodable
    // while causing executable validation to reject it during materialization.
    constexpr size_t bytecode_payload_offset_for_empty_script = 112;
    VERIFY(corrupted_blob.size() > bytecode_payload_offset_for_empty_script);
    corrupted_blob[bytecode_payload_offset_for_empty_script] ^= 0xff;

    // Structural decode still succeeds because the blob is internally well-formed; the corruption is in the bytecode
    // payload, which is only checked by the validator that runs during materialization.
    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(corrupted_blob.bytes(), JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    auto materialized = JS::RustIntegration::materialize_bytecode_cache_script(decoded_blob, test_data.source_code, realm);
    EXPECT(materialized.has_value());
    EXPECT(materialized->is_error());
    EXPECT(!materialized->error().is_empty());
    EXPECT_EQ(materialized->error().first().message, "Failed to materialize bytecode cache"_string);
}

TEST_CASE(bytecode_cache_rejects_corrupt_declaration_function)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto test_data = create_bytecode_cache_blob("function f() { return 1; } f();"_string);
    auto corrupted_blob = MUST(ByteBuffer::copy(test_data.blob.bytes()));

    auto declaration_function_bytecode_offset = first_declaration_function_bytecode_payload_offset(corrupted_blob.bytes());
    VERIFY(declaration_function_bytecode_offset < corrupted_blob.size());
    corrupted_blob[declaration_function_bytecode_offset] ^= 0xff;

    // Structural decode still succeeds (the blob layout is intact); the corruption is in a function's bytecode payload
    // and is only caught when the materializer asks the validator to check it.
    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(corrupted_blob.bytes(), JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    auto materialized = JS::RustIntegration::materialize_bytecode_cache_script(decoded_blob, test_data.source_code, realm);
    EXPECT(materialized.has_value());
    EXPECT(materialized->is_error());
    EXPECT(!materialized->error().is_empty());
    EXPECT_EQ(materialized->error().first().message, "Failed to materialize bytecode cache"_string);
}

TEST_CASE(bytecode_cache_rejects_out_of_range_declaration_function_source_span)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto test_data = create_bytecode_cache_blob("function f() { return 1; } f();"_string);
    auto corrupted_blob = MUST(ByteBuffer::copy(test_data.blob.bytes()));

    auto source_text_start_offset = first_declaration_function_source_text_start_offset(corrupted_blob.bytes());
    write_u32(corrupted_blob, source_text_start_offset, test_data.source_code->length_in_code_units() + 1);

    // The source hash still matches and the blob layout is intact, but the cached function source span points outside
    // the current SourceCode. Materialization should reject it as a cache miss instead of handing the range to C++.
    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(corrupted_blob.bytes(), JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    auto materialized = JS::RustIntegration::materialize_bytecode_cache_script(decoded_blob, test_data.source_code, realm);
    EXPECT(materialized.has_value());
    EXPECT(materialized->is_error());
    EXPECT(!materialized->error().is_empty());
    EXPECT_EQ(materialized->error().first().message, "Failed to materialize bytecode cache"_string);
}

TEST_CASE(bytecode_cache_materializes_function_executables_lazily)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto test_data = create_bytecode_cache_blob("let f = function lazy() { return 1; }; f();"_string);

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(test_data.blob.bytes(), JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    auto script_or_error = JS::Script::create_from_bytecode_cache(decoded_blob, test_data.source_code, realm);
    VERIFY(!script_or_error.is_error());
    auto script = script_or_error.release_value();

    auto* executable = script->cached_executable();
    VERIFY(executable);
    VERIFY(!executable->shared_function_data.is_empty());
    auto& shared_data = *executable->shared_function_data[0];
    EXPECT(!shared_data.m_executable);
    EXPECT(shared_data.m_cached_bytecode_executable);

    auto result = vm->run(script);
    VERIFY(!result.is_throw_completion());
    EXPECT(shared_data.m_executable);
    EXPECT(!shared_data.m_cached_bytecode_executable);
}

TEST_CASE(bytecode_cache_materializes_from_mapped_blob)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto test_data = create_bytecode_cache_blob("let f = function mapped() { return 1; }; f();"_string);
    auto path = ByteString::formatted("{}/bytecode-cache-test-{}.blob", Core::StandardPaths::tempfile_directory(), Core::System::getpid());
    ScopeGuard remove_file = [&] {
        (void)Core::System::unlink(path);
    };

    {
        auto file = TRY_OR_FAIL(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        TRY_OR_FAIL(file->write_until_depleted(test_data.blob.bytes()));
    }

    auto file = TRY_OR_FAIL(Core::File::open(path, Core::File::OpenMode::Read));
    auto mapped_blob = TRY_OR_FAIL(Core::ImmutableBytes::map_from_fd_range_and_close(file->leak_fd(), path, 0, test_data.blob.size()));
    EXPECT(mapped_blob.is_file_backed());

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(mapped_blob, JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    auto script_or_error = JS::Script::create_from_bytecode_cache(decoded_blob, test_data.source_code, realm);
    VERIFY(!script_or_error.is_error());

    auto result = vm->run(script_or_error.release_value());
    VERIFY(!result.is_throw_completion());
}

TEST_CASE(fresh_precompiled_function_executables_materialize_lazily)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto source_code = JS::SourceCode::create("test.js"_string, Utf16String::from_utf8("let f = function lazy() { return 1; }; f();"_string));
    auto* parsed = JS::RustIntegration::parse_program(source_code->utf16_data(), source_code->length_in_code_units(), JS::RustIntegration::ProgramType::Script);
    VERIFY(parsed);
    ArmedScopeGuard free_parsed = [&] {
        JS::RustIntegration::free_parsed_program(parsed);
    };
    EXPECT(!JS::RustIntegration::parsed_program_has_errors(parsed));

    auto* compiled = JS::RustIntegration::compile_parsed_program_fully_off_thread(parsed, source_code->length_in_code_units());
    VERIFY(compiled);
    free_parsed.disarm();

    auto script_or_error = JS::Script::create_from_compiled(compiled, source_code, realm);
    VERIFY(!script_or_error.is_error());
    auto script = script_or_error.release_value();

    auto* executable = script->cached_executable();
    VERIFY(executable);
    VERIFY(!executable->shared_function_data.is_empty());
    auto& shared_data = *executable->shared_function_data[0];
    EXPECT(!shared_data.m_executable);
    EXPECT(shared_data.m_precompiled_bytecode_executable);

    auto result = vm->run(script);
    VERIFY(!result.is_throw_completion());
    EXPECT(shared_data.m_executable);
    EXPECT(!shared_data.m_precompiled_bytecode_executable);
}

TEST_CASE(bytecode_cache_preserves_re_exported_import_names)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto test_data = create_module_bytecode_cache_blob("import { pass as renamed } from './source.mjs'; export { renamed as default };"sv);

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(test_data.blob.bytes(), JS::RustIntegration::ProgramType::Module, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    auto source_module = JS::SourceTextModule::parse("export function pass() {}"sv, realm, "./source.mjs"sv).release_value();
    source_module->set_status(JS::ModuleStatus::Unlinked);

    auto cached_module = JS::SourceTextModule::parse_from_bytecode_cache(decoded_blob, test_data.source_code, realm).release_value();
    cached_module->set_status(JS::ModuleStatus::Unlinked);
    cached_module->loaded_modules().append(JS::LoadedModuleRequest {
        .specifier = Utf16String::from_utf8("./source.mjs"sv),
        .attributes = {},
        .module = source_module,
    });

    auto resolution = cached_module->resolve_export(*vm, "default"_utf16_fly_string);
    EXPECT(resolution.is_valid());
    EXPECT_EQ(resolution.module.ptr(), source_module.ptr());
    EXPECT_EQ(resolution.export_name, "pass"sv);
}
