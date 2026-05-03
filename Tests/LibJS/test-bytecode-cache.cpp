/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/RustIntegration.h>
#include <LibJS/SourceCode.h>
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

    reader.skip(3);               // Function metadata bools.
    reader.skip(sizeof(u64));     // Function environment bindings count.
    reader.skip(sizeof(u64));     // Var environment bindings count.
    reader.skip(2);               // Function metadata bools.
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
