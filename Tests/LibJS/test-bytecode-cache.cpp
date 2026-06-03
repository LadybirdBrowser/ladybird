/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibCore/ImmutableBytes.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Runtime/Array.h>
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
    Core::ImmutableBytes blob;
    Crypto::Hash::SHA256::DigestType source_hash;
};

static Crypto::Hash::SHA256::DigestType bytecode_cache_source_hash(StringView source, StringView source_encoding)
{
    auto hasher = Crypto::Hash::SHA256::create();
    hasher->update(source.bytes());

    auto encoding_length = static_cast<u32>(source_encoding.length());
    Array<u8, sizeof(u32)> encoded_length {
        static_cast<u8>(encoding_length),
        static_cast<u8>(encoding_length >> 8),
        static_cast<u8>(encoding_length >> 16),
        static_cast<u8>(encoding_length >> 24),
    };
    hasher->update(encoded_length.span());
    hasher->update(source_encoding);
    return hasher->digest();
}

TEST_CASE(lazy_source_code_decoding_replaces_utf8_surrogates)
{
    auto source_data = Vector<u8> { 0xed, 0xa0, 0x80 };
    auto source_bytes = TRY_OR_FAIL(Core::ImmutableBytes::copy(source_data.span()));
    auto source_code = JS::SourceCode::create("test.js"_string, 3, "UTF-8"_string, move(source_bytes));

    EXPECT_EQ(source_code->source_text_from_offsets(0, 3).to_utf8(), "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"sv);
    EXPECT_EQ(source_code->code().to_utf8(), "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"sv);
}

TEST_CASE(lazy_source_code_decoding_replaces_odd_trailing_utf16_byte)
{
    auto source_data = Vector<u8> { 'A', 0x00, 0xff };
    auto source_bytes = TRY_OR_FAIL(Core::ImmutableBytes::copy(source_data.span()));
    auto source_code = JS::SourceCode::create("test.js"_string, 2, "UTF-16LE"_string, move(source_bytes));

    EXPECT_EQ(source_code->code().to_utf8(), "A\xef\xbf\xbd"sv);
    EXPECT_EQ(source_code->source_text_from_offsets(0, 2).to_utf8(), "A\xef\xbf\xbd"sv);
}

TEST_CASE(lazy_source_code_decoding_replaces_single_trailing_utf16_byte)
{
    auto source_data = Vector<u8> { 'A' };
    auto source_bytes = TRY_OR_FAIL(Core::ImmutableBytes::copy(source_data.span()));
    auto source_code = JS::SourceCode::create("test.js"_string, 1, "UTF-16LE"_string, move(source_bytes));

    EXPECT_EQ(source_code->source_text_from_offsets(0, 1).to_utf8(), "\xef\xbf\xbd"sv);
}

TEST_CASE(lazy_source_code_decoding_uses_pdfdocencoding_mapping)
{
    auto source_data = Vector<u8> { 0x18, 'A' };
    auto source_bytes = TRY_OR_FAIL(Core::ImmutableBytes::copy(source_data.span()));
    auto source_code = JS::SourceCode::create("test.js"_string, 2, "PDFDocEncoding"_string, move(source_bytes));

    EXPECT_EQ(source_code->source_text_from_offsets(0, 1).to_utf8(), "\xcb\x98"sv);
    EXPECT_EQ(source_code->code().to_utf8(), "\xcb\x98"
                                             "A"sv);
}

TEST_CASE(lazy_source_code_decoding_replaces_overlong_utf8_sequences)
{
    auto source_data = Vector<u8> { 0xc0, 0x80 };
    auto source_bytes = TRY_OR_FAIL(Core::ImmutableBytes::copy(source_data.span()));
    auto source_code = JS::SourceCode::create("test.js"_string, 2, "UTF-8"_string, move(source_bytes));

    EXPECT_EQ(source_code->source_text_from_offsets(0, 2).to_utf8(), "\xef\xbf\xbd\xef\xbf\xbd"sv);
    EXPECT_EQ(source_code->code().to_utf8(), "\xef\xbf\xbd\xef\xbf\xbd"sv);
}

TEST_CASE(lazy_source_code_decoding_extracts_utf8_range_after_non_ascii)
{
    auto prefix = "// Known Trick\xe2\x84\xa2\n"sv;
    auto function_source = "function f() { return 1; }"sv;
    auto source = ByteString::formatted("{}{}", prefix, function_source);
    auto source_utf16 = Utf16String::from_utf8(source.view());
    auto source_bytes = TRY_OR_FAIL(Core::ImmutableBytes::copy(source.bytes()));
    auto source_code = JS::SourceCode::create("test.js"_string, source_utf16.length_in_code_units(), "UTF-8"_string, move(source_bytes));

    auto start_offset = Utf16String::from_utf8(prefix).length_in_code_units();
    EXPECT_EQ(source_code->source_text_from_offsets(start_offset, function_source.length()).to_utf8(), function_source);
}

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

    void align_bytes_payload_to(size_t alignment)
    {
        auto payload_offset = m_offset + sizeof(u32);
        auto padding = (alignment - (payload_offset % alignment)) % alignment;
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

static void skip_script_declaration_metadata(BytecodeCacheBlobReader& reader)
{
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
}

static size_t executable_bytecode_payload_offset(BytecodeCacheBlobReader& reader)
{
    reader.skip(1);               // Executable strict mode.
    reader.skip(sizeof(u32));     // Number of registers.
    reader.skip(sizeof(u32));     // Number of arguments.
    reader.skip(5 * sizeof(u32)); // Cache counters.
    reader.skip(1);               // This value needs environment resolution.
    reader.skip_optional_u32();   // Length identifier.

    reader.align_bytes_payload_to(alignof(JS::Bytecode::Instruction));
    auto bytecode_length = reader.read_u32();
    VERIFY(bytecode_length > 0);
    return reader.offset();
}

static void enter_declaration_function_table(BytecodeCacheBlobReader&, bool require_nonempty);

static size_t top_level_bytecode_payload_offset(ReadonlyBytes blob)
{
    BytecodeCacheBlobReader reader { blob };

    reader.skip(8);           // Magic.
    reader.skip(sizeof(u32)); // Format version.
    reader.skip(1);           // Program type.
    reader.skip(32);          // Source hash.
    reader.skip(sizeof(u32)); // Source length in code units.
    reader.skip(1);           // Has top-level await.
    reader.skip(1);           // Is strict mode.

    skip_script_declaration_metadata(reader);

    enter_declaration_function_table(reader, false);

    reader.skip(1); // Program kind.
    return executable_bytecode_payload_offset(reader);
}

static void enter_declaration_function_table(BytecodeCacheBlobReader& reader, bool require_nonempty)
{
    auto declaration_function_count = reader.read_u32();
    if (require_nonempty)
        VERIFY(declaration_function_count > 0);
    else
        VERIFY(declaration_function_count == 0);
    reader.align_bytes_payload_to(alignof(JS::Bytecode::Instruction));
    auto declaration_function_payload_length = reader.read_u32();
    if (require_nonempty)
        VERIFY(declaration_function_payload_length > 0);
    else
        VERIFY(declaration_function_payload_length == 0);
}

static BytecodeCacheTestData create_bytecode_cache_blob(StringView source)
{
    auto source_code = JS::SourceCode::create("test.js"_string, Utf16String::from_utf8(source));
    auto source_hash = bytecode_cache_source_hash(source, "UTF-8"sv);

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
        .blob = Core::ImmutableBytes::adopt(move(blob)),
        .source_hash = source_hash,
    };
}

static BytecodeCacheTestData create_module_bytecode_cache_blob(StringView source)
{
    auto source_code = JS::SourceCode::create("test.mjs"_string, Utf16String::from_utf8(source));
    auto source_hash = bytecode_cache_source_hash(source, "UTF-8"sv);

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
        .blob = Core::ImmutableBytes::adopt(move(blob)),
        .source_hash = source_hash,
    };
}

static JS::SharedFunctionInstanceData& first_shared_function_with_template_object_cache(JS::Bytecode::Executable& executable)
{
    for (auto& shared_data : executable.shared_function_data) {
        if (!shared_data || !shared_data->m_executable)
            continue;
        if (!shared_data->m_executable->template_object_caches.is_empty())
            return *shared_data;
    }
    VERIFY_NOT_REACHED();
}

static size_t count_shared_functions_with_rust_ast(JS::Bytecode::Executable& executable)
{
    size_t count = 0;
    for (auto& shared_data : executable.shared_function_data) {
        if (!shared_data)
            continue;
        if (shared_data->m_rust_function_ast)
            ++count;
        if (shared_data->m_executable)
            count += count_shared_functions_with_rust_ast(*shared_data->m_executable);
    }
    return count;
}

static size_t count_shared_functions_with_cached_bytecode(JS::Bytecode::Executable& executable)
{
    size_t count = 0;
    for (auto& shared_data : executable.shared_function_data) {
        if (!shared_data)
            continue;
        if (shared_data->m_cached_bytecode_executable)
            ++count;
        if (shared_data->m_executable)
            count += count_shared_functions_with_cached_bytecode(*shared_data->m_executable);
    }
    return count;
}

static size_t first_declaration_function_bytecode_payload_offset(ReadonlyBytes blob)
{
    BytecodeCacheBlobReader reader { blob };

    reader.skip(8);           // Magic.
    reader.skip(sizeof(u32)); // Format version.
    reader.skip(1);           // Program type.
    reader.skip(32);          // Source hash.
    reader.skip(sizeof(u32)); // Source length in code units.
    reader.skip(1);           // Has top-level await.
    reader.skip(1);           // Is strict mode.

    skip_script_declaration_metadata(reader);
    enter_declaration_function_table(reader, true);

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

    reader.align_bytes_payload_to(alignof(JS::Bytecode::Instruction));
    auto executable_length = reader.read_u32();
    VERIFY(executable_length > 0);

    return executable_bytecode_payload_offset(reader);
}

static size_t first_declaration_function_source_text_start_offset(ReadonlyBytes blob)
{
    BytecodeCacheBlobReader reader { blob };

    reader.skip(8);           // Magic.
    reader.skip(sizeof(u32)); // Format version.
    reader.skip(1);           // Program type.
    reader.skip(32);          // Source hash.
    reader.skip(sizeof(u32)); // Source length in code units.
    reader.skip(1);           // Has top-level await.
    reader.skip(1);           // Is strict mode.

    skip_script_declaration_metadata(reader);
    enter_declaration_function_table(reader, true);

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

    auto bytecode_payload_offset = top_level_bytecode_payload_offset(corrupted_blob.bytes());
    VERIFY(corrupted_blob.size() > bytecode_payload_offset);
    corrupted_blob[bytecode_payload_offset] ^= 0xff;

    // Structural decode still succeeds because the blob is internally well-formed; the corruption is in the bytecode
    // payload, which is only checked by the validator that runs during materialization.
    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(Core::ImmutableBytes::adopt(move(corrupted_blob)), JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
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
    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(Core::ImmutableBytes::adopt(move(corrupted_blob)), JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
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
    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(Core::ImmutableBytes::adopt(move(corrupted_blob)), JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
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

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(test_data.blob, JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    auto script_or_error = JS::Script::create_from_bytecode_cache(decoded_blob, test_data.source_code, realm);
    VERIFY(!script_or_error.is_error());
    auto script = script_or_error.release_value();
    EXPECT(script->executable_backing().is_mapped_bytecode_cache());

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

TEST_CASE(bytecode_cache_install_shares_template_object_cache_slots)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto source = "var tag = function(strings) { return strings; };\n"
                  "var f = function(useTemplate) { if (useTemplate) return tag`hello`; return null; };\n"
                  "f(false);"_string;
    auto test_data = create_bytecode_cache_blob(source);

    auto script_or_error = JS::Script::parse(source, realm, "test.js"sv);
    VERIFY(!script_or_error.is_error());
    auto script = script_or_error.release_value();
    EXPECT(script->executable_backing().is_source());
    EXPECT(script->can_generate_bytecode_cache());

    auto result = vm->run(script);
    VERIFY(!result.is_throw_completion());
    EXPECT(result.value().is_null());

    auto* old_executable = script->cached_executable();
    VERIFY(old_executable);
    auto& shared_data = first_shared_function_with_template_object_cache(*old_executable);
    auto old_function_executable = shared_data.m_executable;
    VERIFY(old_function_executable);
    VERIFY(old_function_executable->template_object_caches.size() == 1);

    auto old_template_cache = old_function_executable->template_object_caches[0];
    EXPECT(!old_template_cache->cached_template_object);

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(test_data.blob, JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    EXPECT(script->try_install_bytecode_cache(decoded_blob, test_data.source_code));
    auto new_function_executable = shared_data.m_executable;
    VERIFY(new_function_executable);
    EXPECT_NE(new_function_executable.ptr(), old_function_executable.ptr());
    VERIFY(new_function_executable->template_object_caches.size() == 1);
    EXPECT_EQ(new_function_executable->template_object_caches[0].ptr(), old_template_cache.ptr());

    auto late_template_object = MUST(JS::Array::create(realm, 0));
    old_template_cache->cached_template_object = late_template_object;
    EXPECT_EQ(new_function_executable->template_object_caches[0]->cached_template_object.ptr(), late_template_object.ptr());
}

TEST_CASE(bytecode_cache_install_rejects_corrupt_declaration_function)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto source = "function f() { return 1; } f();"_string;
    auto test_data = create_bytecode_cache_blob(source);
    auto corrupted_blob = MUST(ByteBuffer::copy(test_data.blob.bytes()));

    auto declaration_function_bytecode_offset = first_declaration_function_bytecode_payload_offset(corrupted_blob.bytes());
    VERIFY(declaration_function_bytecode_offset < corrupted_blob.size());
    corrupted_blob[declaration_function_bytecode_offset] ^= 0xff;

    auto script_or_error = JS::Script::parse(source, realm, "test.js"sv);
    VERIFY(!script_or_error.is_error());
    auto script = script_or_error.release_value();

    auto* old_executable = script->cached_executable();
    VERIFY(old_executable);

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(Core::ImmutableBytes::adopt(move(corrupted_blob)), JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    EXPECT(!script->try_install_bytecode_cache(decoded_blob, test_data.source_code));
    EXPECT_EQ(script->cached_executable(), old_executable);
}

TEST_CASE(bytecode_cache_install_failure_preserves_existing_shared_functions)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto source = "var f = function() { return 1; }; f();"_string;
    auto test_data = create_bytecode_cache_blob(source);

    auto script_or_error = JS::Script::parse(source, realm, "test.js"sv);
    VERIFY(!script_or_error.is_error());
    auto script = script_or_error.release_value();

    auto result = vm->run(script);
    VERIFY(!result.is_throw_completion());
    EXPECT_EQ(result.value().as_i32(), 1);

    auto* old_executable = script->cached_executable();
    VERIFY(old_executable);
    VERIFY(old_executable->shared_function_data.size() == 1);
    auto& shared_data = *old_executable->shared_function_data[0];
    auto old_function_executable = shared_data.m_executable;
    VERIFY(old_function_executable);

    auto corrupted_blob = MUST(ByteBuffer::copy(test_data.blob.bytes()));
    auto bytecode_payload_offset = top_level_bytecode_payload_offset(corrupted_blob.bytes());
    VERIFY(bytecode_payload_offset < corrupted_blob.size());
    corrupted_blob[bytecode_payload_offset] ^= 0xff;

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(Core::ImmutableBytes::adopt(move(corrupted_blob)), JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    EXPECT(!script->try_install_bytecode_cache(decoded_blob, test_data.source_code));
    EXPECT_EQ(script->cached_executable(), old_executable);
    EXPECT_EQ(shared_data.m_executable.ptr(), old_function_executable.ptr());
}

TEST_CASE(bytecode_cache_install_clears_lazy_nested_function_inputs)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto source = "let f = function outer() { function inner() { return 1; } return inner; }; let g = f();"_string;
    auto test_data = create_bytecode_cache_blob(source);

    auto script_or_error = JS::Script::parse(source, realm, "test.js"sv);
    VERIFY(!script_or_error.is_error());
    auto script = script_or_error.release_value();

    auto* executable = script->cached_executable();
    VERIFY(executable);
    VERIFY(executable->shared_function_data.size() == 1);
    auto& outer_shared_data = *executable->shared_function_data[0];
    EXPECT(outer_shared_data.m_owner_shared_function_data_list);

    auto result = vm->run(script);
    VERIFY(!result.is_throw_completion());

    VERIFY(outer_shared_data.m_executable);
    VERIFY(outer_shared_data.m_executable->shared_function_data.size() == 1);
    auto& inner_shared_data = *outer_shared_data.m_executable->shared_function_data[0];
    EXPECT_EQ(inner_shared_data.m_owner_shared_function_data_list, outer_shared_data.m_owner_shared_function_data_list);
    EXPECT(inner_shared_data.m_rust_function_ast);

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(test_data.blob, JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    EXPECT(script->try_install_bytecode_cache(decoded_blob, test_data.source_code));
    EXPECT(!inner_shared_data.m_rust_function_ast);
    EXPECT(inner_shared_data.m_cached_bytecode_executable);
}

TEST_CASE(bytecode_cache_install_matches_class_constructor_after_source_text_expands)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto source = "let C = class { constructor() { this.value = 1; } method() { return this.value; } }; C;"_string;
    auto test_data = create_bytecode_cache_blob(source);

    auto script_or_error = JS::Script::parse(source, realm, "test.js"sv);
    VERIFY(!script_or_error.is_error());
    auto script = script_or_error.release_value();

    auto result = vm->run(script);
    VERIFY(!result.is_throw_completion());

    auto* old_executable = script->cached_executable();
    VERIFY(old_executable);
    EXPECT(count_shared_functions_with_rust_ast(*old_executable) > 0);

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(test_data.blob, JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    script->begin_bytecode_cache_generation();
    EXPECT(script->executable_backing().is_source());
    EXPECT(!script->can_generate_bytecode_cache());
    EXPECT(script->can_install_generated_bytecode_cache());
    script->finish_bytecode_cache_generation_without_install();
    EXPECT(script->executable_backing().is_source());
    EXPECT(script->can_generate_bytecode_cache());
    EXPECT(!script->can_install_generated_bytecode_cache());

    script->begin_bytecode_cache_generation();
    EXPECT(script->can_install_generated_bytecode_cache());
    script->install_generated_bytecode_cache(decoded_blob, test_data.source_code);
    EXPECT(script->executable_backing().is_mapped_bytecode_cache());
    EXPECT(!script->can_generate_bytecode_cache());
    EXPECT(!script->can_install_generated_bytecode_cache());
    auto* new_executable = script->cached_executable();
    VERIFY(new_executable);
    EXPECT_EQ(count_shared_functions_with_rust_ast(*new_executable), static_cast<size_t>(0));
    EXPECT(count_shared_functions_with_cached_bytecode(*new_executable) > 0);
}

TEST_CASE(bytecode_cache_install_clears_precompiled_lazy_function_inputs)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto source = "let f = function lazy() { return 1; }; f;"_string;
    auto test_data = create_bytecode_cache_blob(source);

    auto* parsed = JS::RustIntegration::parse_program(test_data.source_code->utf16_data(), test_data.source_code->length_in_code_units(), JS::RustIntegration::ProgramType::Script);
    VERIFY(parsed);
    ArmedScopeGuard free_parsed = [&] {
        JS::RustIntegration::free_parsed_program(parsed);
    };
    EXPECT(!JS::RustIntegration::parsed_program_has_errors(parsed));

    auto* compiled = JS::RustIntegration::compile_parsed_program_fully_off_thread(parsed, test_data.source_code->length_in_code_units());
    VERIFY(compiled);
    free_parsed.disarm();

    auto script_or_error = JS::Script::create_from_compiled(compiled, test_data.source_code, realm);
    VERIFY(!script_or_error.is_error());
    auto script = script_or_error.release_value();
    EXPECT(script->executable_backing().is_heap_bytecode());

    auto* executable = script->cached_executable();
    VERIFY(executable);
    VERIFY(executable->shared_function_data.size() == 1);
    auto& shared_data = *executable->shared_function_data[0];
    EXPECT(!shared_data.m_executable);
    EXPECT(shared_data.m_precompiled_bytecode_executable);
    EXPECT(!shared_data.m_cached_bytecode_executable);

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(test_data.blob, JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    script->begin_bytecode_cache_generation();
    EXPECT(script->executable_backing().is_heap_bytecode());
    EXPECT(!script->can_generate_bytecode_cache());
    EXPECT(script->can_install_generated_bytecode_cache());
    script->finish_bytecode_cache_generation_without_install();
    EXPECT(script->executable_backing().is_heap_bytecode());
    EXPECT(script->can_generate_bytecode_cache());
    EXPECT(!script->can_install_generated_bytecode_cache());

    script->begin_bytecode_cache_generation();
    script->install_generated_bytecode_cache(decoded_blob, test_data.source_code);
    EXPECT(script->executable_backing().is_mapped_bytecode_cache());
    EXPECT(!script->can_generate_bytecode_cache());
    EXPECT(!script->can_install_generated_bytecode_cache());
    EXPECT(!shared_data.m_rust_function_ast);
    EXPECT(!shared_data.m_precompiled_bytecode_executable);
    EXPECT(shared_data.m_cached_bytecode_executable);
}

TEST_CASE(bytecode_cache_install_updates_top_level_await_module_executable)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto source = "await 1;"_string;
    auto test_data = create_module_bytecode_cache_blob(source);

    auto module_or_error = JS::SourceTextModule::parse(source, realm, "test.mjs"sv);
    VERIFY(!module_or_error.is_error());
    auto module = module_or_error.release_value();
    EXPECT(module->executable_backing().is_source());

    auto* top_level_await_shared_data = module->top_level_await_shared_data();
    VERIFY(top_level_await_shared_data);
    auto old_executable = top_level_await_shared_data->m_executable;
    VERIFY(old_executable);

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(test_data.blob, JS::RustIntegration::ProgramType::Module, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    module->begin_bytecode_cache_generation();
    EXPECT(module->can_install_generated_bytecode_cache());
    module->install_generated_bytecode_cache(decoded_blob, test_data.source_code);

    EXPECT(module->executable_backing().is_mapped_bytecode_cache());
    EXPECT(!module->can_generate_bytecode_cache());
    EXPECT(!module->can_install_generated_bytecode_cache());
    EXPECT_EQ(module->top_level_await_shared_data(), top_level_await_shared_data);
    VERIFY(top_level_await_shared_data->m_executable);
    EXPECT_NE(top_level_await_shared_data->m_executable.ptr(), old_executable.ptr());
}

TEST_CASE(bytecode_cache_to_string_caches_lazy_ascii_source_text)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto source = "let f = function mapped() { return 'hello'; }; f.toString() + '|' + f.toString();"sv;
    auto test_data = create_bytecode_cache_blob(source);

    auto source_bytes = TRY_OR_FAIL(Core::ImmutableBytes::copy(source.bytes()));
    auto source_code = JS::SourceCode::create("test.js"_string, test_data.source_code->length_in_code_units(), "UTF-8"_string, move(source_bytes));

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(test_data.blob, JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    auto script_or_error = JS::Script::create_from_bytecode_cache(decoded_blob, source_code, realm);
    VERIFY(!script_or_error.is_error());
    auto script = script_or_error.release_value();

    auto* executable = script->cached_executable();
    VERIFY(executable);
    VERIFY(!executable->shared_function_data.is_empty());
    auto& shared_data = *executable->shared_function_data[0];
    EXPECT(shared_data.m_source_text_owner.is_empty());

    auto result = vm->run(script);
    VERIFY(!result.is_throw_completion());
    VERIFY(result.value().is_string());
    EXPECT_EQ(result.value().as_string().utf8_string(), "function mapped() { return 'hello'; }|function mapped() { return 'hello'; }"_string);
    EXPECT_EQ(shared_data.m_source_text_owner.to_utf8(), "function mapped() { return 'hello'; }"sv);
}

TEST_CASE(bytecode_cache_to_string_uses_lazy_utf8_offset_map)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto prefix = "// Known Trick\xe2\x84\xa2\n"sv;
    auto body = "let f = function mapped() { return 'Known Trick\xe2\x84\xa2'; }; f.toString();"sv;
    auto source = ByteString::formatted("{}{}", prefix, body);
    auto test_data = create_bytecode_cache_blob(source.view());

    auto source_bytes = TRY_OR_FAIL(Core::ImmutableBytes::copy(source.bytes()));
    auto source_code = JS::SourceCode::create("test.js"_string, test_data.source_code->length_in_code_units(), "UTF-8"_string, move(source_bytes));

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(test_data.blob, JS::RustIntegration::ProgramType::Script, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    auto script_or_error = JS::Script::create_from_bytecode_cache(decoded_blob, source_code, realm);
    VERIFY(!script_or_error.is_error());
    auto script = script_or_error.release_value();

    auto* executable = script->cached_executable();
    VERIFY(executable);
    VERIFY(!executable->shared_function_data.is_empty());
    auto& shared_data = *executable->shared_function_data[0];
    EXPECT(shared_data.m_source_text_owner.is_empty());

    auto result = vm->run(script);
    VERIFY(!result.is_throw_completion());
    VERIFY(result.value().is_string());
    EXPECT_EQ(result.value().as_string().utf8_string(), "function mapped() { return 'Known Trick\xe2\x84\xa2'; }"_string);
    EXPECT_EQ(shared_data.m_source_text_owner.to_utf8(), "function mapped() { return 'Known Trick\xe2\x84\xa2'; }"sv);
}

TEST_CASE(bytecode_cache_materializes_from_mapped_blob)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto test_data = create_bytecode_cache_blob("let f = function mapped() { return 'hello'; }; f();"_string);
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
    VERIFY(result.value().is_string());
    EXPECT_EQ(result.value().as_string().utf8_string(), "hello"_string);
}

TEST_CASE(fresh_precompiled_function_executables_materialize_lazily)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto source_code = JS::SourceCode::create("test.js"_string, Utf16String::from_utf8("let f = function lazy() { function inner() { return 1; } return inner(); }; f();"_string));
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
    EXPECT(script->executable_backing().is_heap_bytecode());

    auto* executable = script->cached_executable();
    VERIFY(executable);
    VERIFY(!executable->shared_function_data.is_empty());
    auto& shared_data = *executable->shared_function_data[0];
    EXPECT(!shared_data.m_executable);
    EXPECT(shared_data.m_precompiled_bytecode_executable);
    EXPECT(!shared_data.m_rust_function_ast);
    EXPECT(shared_data.m_functions_to_initialize.is_empty());

    auto result = vm->run(script);
    VERIFY(!result.is_throw_completion());
    EXPECT_EQ(result.value().as_i32(), 1);
    EXPECT(shared_data.m_executable);
    EXPECT(!shared_data.m_precompiled_bytecode_executable);
}

TEST_CASE(bytecode_cache_preserves_re_exported_import_names)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto test_data = create_module_bytecode_cache_blob("import { pass as renamed } from './source.mjs'; export { renamed as default };"sv);

    auto* decoded_blob = JS::RustIntegration::decode_bytecode_cache_blob(test_data.blob, JS::RustIntegration::ProgramType::Module, test_data.source_hash.bytes());
    VERIFY(decoded_blob);

    auto source_module = JS::SourceTextModule::parse("export function pass() {}"sv, realm, "./source.mjs"sv).release_value();
    source_module->set_status(JS::ModuleStatus::Unlinked);

    auto cached_module = JS::SourceTextModule::parse_from_bytecode_cache(decoded_blob, test_data.source_code, realm).release_value();
    EXPECT(cached_module->executable_backing().is_mapped_bytecode_cache());
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
