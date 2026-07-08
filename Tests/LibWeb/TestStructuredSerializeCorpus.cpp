/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StructuredSerializeTestHelpers.h"

#include <LibCore/DirIterator.h>
#include <LibCore/File.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Crypto/CryptoKeySerializationTags.h>

static constexpr auto corpus_directory = "test-inputs/structured-serialize-corpus"sv;

struct CorpusEntry {
    String name;
    Web::HTML::StorageSerializationRecord record;
};

// One record per stable-format surface: value tags, typed-array constructors, native errors,
// serializable interfaces, and CryptoKey algorithm/handle variants.
static Vector<CorpusEntry> build_corpus_entries()
{
    Vector<CorpusEntry> entries;
    auto add = [&](String name, Web::HTML::StorageSerializationRecord record) {
        entries.append({ move(name), move(record) });
    };
    auto add_value = [&](StringView name, Vector<u8> const& value) {
        add(MUST(String::from_utf8(name)), storage_record_with_value(value));
    };

    add_value("value-undefined"sv, { to_underlying(ValueTag::UndefinedPrimitive) });
    add_value("value-null"sv, { to_underlying(ValueTag::NullPrimitive) });
    add_value("value-boolean"sv, { to_underlying(ValueTag::BooleanPrimitive), 1 });

    {
        Vector<u8> value { to_underlying(ValueTag::NumberPrimitive) };
        append_little_endian_double(value, 13.5);
        add_value("value-number"sv, value);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::Int32Primitive) };
        append_storage_signed_leb128(value, -2);
        add_value("value-int32"sv, value);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::StringPrimitive) };
        append_storage_ascii_utf16(value, "corpus"sv);
        add_value("value-string"sv, value);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::StringPrimitive) };
        Array<u16, 4> code_units { 'c', 'a', 'f', 0x00E9 };
        append_storage_utf16(value, code_units);
        add_value("value-string-utf16"sv, value);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::StringPrimitive) };
        Array<u16, 1> code_units { 0xD800 };
        append_storage_utf16(value, code_units);
        add_value("value-string-lone-surrogate"sv, value);
    }

    Array<u8, 3> big_integer { 0x00, 0x03, 0xE7 }; // +999
    {
        Vector<u8> value { to_underlying(ValueTag::BigIntPrimitive) };
        append_storage_leb128(value, big_integer.size());
        value.append(big_integer.data(), big_integer.size());
        add_value("value-bigint"sv, value);
    }

    add_value("value-boolean-object"sv, { to_underlying(ValueTag::BooleanObject), 1 });
    {
        Vector<u8> value { to_underlying(ValueTag::NumberObject) };
        append_little_endian_double(value, 2.5);
        add_value("value-number-object"sv, value);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::StringObject) };
        append_storage_ascii_utf16(value, "wrap"sv);
        add_value("value-string-object"sv, value);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::BigIntObject) };
        append_storage_leb128(value, big_integer.size());
        value.append(big_integer.data(), big_integer.size());
        add_value("value-bigint-object"sv, value);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::DateObject) };
        append_little_endian_double(value, 1704067200000.0);
        add_value("value-date"sv, value);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::RegExpObject) };
        append_storage_ascii_utf16(value, "ab"sv);
        append_storage_ascii_utf16(value, "gi"sv);
        add_value("value-regexp"sv, value);
    }

    {
        Vector<u8> value { to_underlying(ValueTag::MapObject) };
        append_storage_leb128(value, 1); // entry count
        value.append(to_underlying(ValueTag::NumberPrimitive));
        append_little_endian_double(value, 1.0);
        value.append(to_underlying(ValueTag::NumberPrimitive));
        append_little_endian_double(value, 2.0);
        add_value("value-map"sv, value);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::SetObject) };
        append_storage_leb128(value, 2); // entry count
        value.append(to_underlying(ValueTag::NumberPrimitive));
        append_little_endian_double(value, 7.0);
        value.append(to_underlying(ValueTag::NumberPrimitive));
        append_little_endian_double(value, 8.0);
        add_value("value-set"sv, value);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::ArrayObject) };
        append_storage_leb128(value, 1); // array length
        value.append(to_underlying(ValueTag::NumberPrimitive));
        append_little_endian_double(value, 10.0);
        append_storage_ascii_utf16(value, "0"sv);
        value.append(to_underlying(ValueTag::EndObject));
        add_value("value-array"sv, value);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::Object) };
        value.append(to_underlying(ValueTag::NumberPrimitive));
        append_little_endian_double(value, 42.0);
        append_storage_ascii_utf16(value, "answer"sv);
        value.append(to_underlying(ValueTag::EndObject));
        add_value("value-object"sv, value);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::ArrayObject) };
        append_storage_leb128(value, 2); // array length
        value.append(to_underlying(ValueTag::Object));
        value.append(to_underlying(ValueTag::EndObject));
        append_storage_ascii_utf16(value, "0"sv);
        value.append(to_underlying(ValueTag::ObjectReference));
        append_storage_leb128(value, 1);
        append_storage_ascii_utf16(value, "1"sv);
        value.append(to_underlying(ValueTag::EndObject));
        add_value("value-object-reference"sv, value);
    }

    auto error_value = [](StringView type_name) {
        Vector<u8> value { to_underlying(ValueTag::ErrorObject) };
        append_storage_string(value, type_name);
        value.append(0); // Optional message: absent
        value.append(0); // Optional cause: absent
        return value;
    };
    add_value("value-error"sv, error_value("Error"sv));
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    add(MUST(String::formatted("error-{}", #ClassName##sv)), storage_record_with_value(error_value(#ClassName##sv)));
    JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE

    Array<u8, 4> contents { 0xde, 0xad, 0xbe, 0xef };
    {
        Vector<u8> value { to_underlying(ValueTag::ArrayBuffer) };
        append_storage_leb128(value, contents.size());
        value.append(contents.data(), contents.size());
        add_value("value-array-buffer"sv, value);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::ResizeableArrayBuffer) };
        append_storage_leb128(value, contents.size());
        value.append(contents.data(), contents.size());
        append_storage_leb128(value, 8); // max byte length
        add_value("value-resizable-array-buffer"sv, value);
    }
    add("value-array-buffer-view"_string, array_buffer_view_record("Float64Array"sv, BackingBuffer::Fixed, 16, 16, 0, 2));
    add("view-DataView"_string, array_buffer_view_record("DataView"sv, BackingBuffer::Fixed, 8, 8, 0));

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type)                                    \
    {                                                                                                                  \
        auto element_size = static_cast<u32>(MUST(JS::ClassName::create(test_realm(), 0))->element_size());            \
        add(MUST(String::formatted("view-{}", #ClassName##sv)),                                                        \
            array_buffer_view_record(#ClassName##sv, BackingBuffer::Fixed, element_size * 2, element_size * 2, 0, 2)); \
    }
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

    {
        Vector<u8> point_body;
        for (double element : { 1.5, -2.5, 3.5, -4.5 })
            append_little_endian_double(point_body, element);
        add("serializable-DOMPoint"_string, serializable_storage_record("DOMPoint"sv, 1, point_body));
        add("serializable-DOMPointReadOnly"_string, serializable_storage_record("DOMPointReadOnly"sv, 1, point_body));

        Vector<u8> quad_body;
        for (size_t i = 0; i < 4; ++i)
            frozen_serializable_value(quad_body, "DOMPoint"sv, 1, point_body);
        add("serializable-DOMQuad"_string, serializable_storage_record("DOMQuad"sv, 1, quad_body));
    }
    {
        Vector<u8> body;
        for (double element : { 10.0, 20.0, 30.0, 40.0 })
            append_little_endian_double(body, element);
        add("serializable-DOMRect"_string, serializable_storage_record("DOMRect"sv, 1, body));
        add("serializable-DOMRectReadOnly"_string, serializable_storage_record("DOMRectReadOnly"sv, 1, body));
    }
    {
        Vector<u8> body;
        body.append(1); // is 2D
        for (double element : { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 })
            append_little_endian_double(body, element);
        add("serializable-DOMMatrix"_string, serializable_storage_record("DOMMatrix"sv, 1, body));
        add("serializable-DOMMatrixReadOnly"_string, serializable_storage_record("DOMMatrixReadOnly"sv, 1, body));
    }
    {
        Vector<u8> body;
        append_storage_string(body, "text/plain"sv);
        append_storage_bytes(body, "hello"sv.bytes());
        add("serializable-Blob"_string, serializable_storage_record("Blob"sv, 1, body));
    }
    {
        Vector<u8> file_body;
        append_storage_string(file_body, "text/plain"sv);
        append_storage_bytes(file_body, "data"sv.bytes());
        append_storage_string(file_body, "f.txt"sv);
        append_storage_signed_leb128(file_body, 1234); // last modified
        add("serializable-File"_string, serializable_storage_record("File"sv, 1, file_body));

        Vector<u8> list_body;
        append_storage_leb128(list_body, 1); // file count
        frozen_serializable_value(list_body, "File"sv, 1, file_body);
        add("serializable-FileList"_string, serializable_storage_record("FileList"sv, 1, list_body));
    }
    {
        Vector<u8> body;
        append_storage_string(body, "AbortError"sv);
        append_storage_ascii_utf16(body, "stop"sv);
        add("serializable-DOMException"_string, serializable_storage_record("DOMException"sv, 1, body));
    }
    {
        Vector<u8> body;
        append_storage_string(body, "QuotaExceededError"sv);
        append_storage_ascii_utf16(body, "over"sv);
        body.append(0); // quota absent
        body.append(0); // requested absent
        add("serializable-QuotaExceededError"_string, serializable_storage_record("QuotaExceededError"sv, 1, body));
    }
    {
        Array<u8, 4> pixels { 1, 2, 3, 4 };
        add("serializable-ImageData"_string, frozen_image_data_record({ pixels.data(), pixels.size() }, 1, 1, "srgb"sv));

        Array<u8, 16> bitmap_pixels {};
        add("serializable-ImageBitmap"_string, image_bitmap_record(2, 2, 8, "BGRA8888"sv, "premultiplied"sv, { bitmap_pixels.data(), bitmap_pixels.size() }));
    }

    add("serializable-CryptoKey"_string, crypto_key_full_record("secret"sv, "deriveKey"sv, alg_key_algorithm("HKDF"sv), handle_byte_buffer("0123456789abcdef"sv.bytes())));
    for (auto const& golden : crypto_algorithm_goldens())
        add(MUST(String::formatted("cryptokey-algorithm-{}", to_underlying(golden.tag))), crypto_key_full_record(golden.type, golden.usage, golden.algorithm, golden.handle));
    for (auto const& golden : crypto_handle_goldens())
        add(MUST(String::formatted("cryptokey-handle-{}", to_underlying(golden.tag))), crypto_key_full_record(golden.type, golden.usage, golden.algorithm, golden.handle));

    return entries;
}

// SerializableObject is covered per registry identifier instead of by a single entry.
static Optional<StringView> corpus_entry_name_for(ValueTag tag)
{
    switch (tag) {
    case ValueTag::Empty:
        return {};
    case ValueTag::UndefinedPrimitive:
        return "value-undefined"sv;
    case ValueTag::NullPrimitive:
        return "value-null"sv;
    case ValueTag::BooleanPrimitive:
        return "value-boolean"sv;
    case ValueTag::NumberPrimitive:
        return "value-number"sv;
    case ValueTag::StringPrimitive:
        return "value-string"sv;
    case ValueTag::BigIntPrimitive:
        return "value-bigint"sv;
    case ValueTag::BooleanObject:
        return "value-boolean-object"sv;
    case ValueTag::NumberObject:
        return "value-number-object"sv;
    case ValueTag::StringObject:
        return "value-string-object"sv;
    case ValueTag::BigIntObject:
        return "value-bigint-object"sv;
    case ValueTag::DateObject:
        return "value-date"sv;
    case ValueTag::RegExpObject:
        return "value-regexp"sv;
    case ValueTag::MapObject:
        return "value-map"sv;
    case ValueTag::SetObject:
        return "value-set"sv;
    case ValueTag::ArrayObject:
        return "value-array"sv;
    case ValueTag::ErrorObject:
        return "value-error"sv;
    case ValueTag::Object:
        return "value-object"sv;
    case ValueTag::ObjectReference:
        return "value-object-reference"sv;
    case ValueTag::GrowableSharedArrayBuffer:
    case ValueTag::SharedArrayBuffer:
        return {};
    case ValueTag::ResizeableArrayBuffer:
        return "value-resizable-array-buffer"sv;
    case ValueTag::ArrayBuffer:
        return "value-array-buffer"sv;
    case ValueTag::ArrayBufferView:
        return "value-array-buffer-view"sv;
    case ValueTag::SerializableObject:
        return {};
    case ValueTag::Int32Primitive:
        return "value-int32"sv;
    case ValueTag::EndObject:
        return {};
    }
    return {};
}

TEST_CASE(corpus_covers_the_stable_format_surface)
{
    auto entries = build_corpus_entries();
    auto has_entry = [&](StringView name) {
        for (auto const& entry : entries) {
            if (entry.name == name)
                return true;
        }
        return false;
    };

    for (auto const& expectation : Web::HTML::s_value_tag_expectations) {
        if (expectation.expectation != Web::HTML::Expectation::PositiveGolden)
            continue;
        auto name = corpus_entry_name_for(expectation.tag);
        if (!name.has_value())
            continue;
        EXPECT(has_entry(*name));
    }

    for (auto const& entry : Web::HTML::serializable_storage_registry())
        EXPECT(has_entry(MUST(String::formatted("serializable-{}", entry.identifier))));

    for (auto const& expectation : Web::Crypto::s_key_algorithm_tag_expectations) {
        if (expectation.expectation == Web::HTML::Expectation::PositiveGolden)
            EXPECT(has_entry(MUST(String::formatted("cryptokey-algorithm-{}", to_underlying(expectation.tag)))));
    }
    for (auto const& expectation : Web::Crypto::s_handle_tag_expectations) {
        if (expectation.expectation == Web::HTML::Expectation::PositiveGolden)
            EXPECT(has_entry(MUST(String::formatted("cryptokey-handle-{}", to_underlying(expectation.tag)))));
    }
}

// The checked-in files are frozen: validation decodes the file bytes, never the freshly built ones,
// so a record written by any released version must keep decoding even as the builders evolve. A new
// entry's record is checked in once by writing its bytes to <name>.bin.
TEST_CASE(corpus_files_are_present_and_decode)
{
    auto entries = build_corpus_entries();

    for (auto const& entry : entries) {
        auto path = MUST(String::formatted("{}/{}.bin", corpus_directory, entry.name));
        auto file = Core::File::open(path, Core::File::OpenMode::Read);
        if (file.is_error()) {
            FAIL(MUST(String::formatted("Missing corpus file {}", path)));
            continue;
        }
    }

    size_t decoded_count = 0;
    Core::DirIterator iterator(ByteString(corpus_directory), Core::DirIterator::Flags::SkipDots);
    while (iterator.has_next()) {
        auto name = iterator.next_path();
        if (!name.ends_with(".bin"sv))
            continue;

        auto path = MUST(String::formatted("{}/{}", corpus_directory, name));
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
        auto bytes = MUST(file->read_until_eof());

        Web::HTML::StorageSerializationRecord record { move(bytes) };
        auto result = crypto_storage_deserialize(record);
        if (result.is_exception())
            FAIL(MUST(String::formatted("Corpus file {} no longer deserializes", name)));
        ++decoded_count;
    }

    // An empty or unreachable corpus directory must fail, not pass vacuously.
    EXPECT(decoded_count >= entries.size());
}
