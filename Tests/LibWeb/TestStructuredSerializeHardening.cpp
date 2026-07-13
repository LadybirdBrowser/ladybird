/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StructuredSerializeTestHelpers.h"

TEST_CASE(storage_reader_rejects_unknown_enum_like_identifiers)
{
    EXPECT(decode_unknown_storage_identifier<Web::Bindings::KeyType>("future-key-type"sv).is_error());
    EXPECT(decode_unknown_storage_identifier<Web::Bindings::KeyUsage>("future-key-usage"sv).is_error());
    EXPECT(decode_unknown_storage_identifier<Web::Bindings::PredefinedColorSpace>("future-color-space"sv).is_error());
    EXPECT(decode_unknown_storage_identifier<Gfx::BitmapFormat>("FutureBitmapFormat"sv).is_error());
    EXPECT(decode_unknown_storage_identifier<Gfx::AlphaType>("future-alpha-type"sv).is_error());
    EXPECT(decode_unknown_storage_identifier<Web::Bindings::InterfaceName>("FutureSerializableInterface"sv).is_error());
}

TEST_CASE(storage_reader_rejects_unknown_and_reserved_serializable_versions)
{
    // CryptoKey is covered separately because it is [SecureContext].
    Array identifiers {
        "Blob"sv, "File"sv, "FileList"sv, "DOMException"sv, "QuotaExceededError"sv,
        "DOMMatrix"sv, "DOMMatrixReadOnly"sv, "DOMPoint"sv, "DOMPointReadOnly"sv,
        "DOMRect"sv, "DOMRectReadOnly"sv, "DOMQuad"sv, "ImageData"sv, "ImageBitmap"sv
    };

    for (auto identifier : identifiers) {
        EXPECT(storage_deserialize(serializable_storage_record(identifier, 255)).is_error());
        EXPECT(storage_deserialize(serializable_storage_record(identifier, 0)).is_error());
        EXPECT(storage_deserialize(serializable_storage_record(identifier, 0x10000)).is_error());
    }
}

TEST_CASE(storage_reader_rejects_wrong_nested_serializable_type)
{
    auto& realm = test_realm();

    auto point = Web::Geometry::DOMPoint::create(realm);
    auto point_body = serialized_object_body(point);

    auto blob = Web::FileAPI::Blob::create(realm, MUST(ByteBuffer::copy("x"sv.bytes())), "text/plain"_utf16);
    auto blob_body = serialized_object_body(blob);

    // FileList v1 whose one entry is a DOMPoint, not a File.
    Vector<u8> file_list_body;
    append_storage_leb128(file_list_body, 1); // file count
    file_list_body.append(point_body.data(), point_body.size());
    EXPECT(storage_deserialize(serializable_storage_record("FileList"sv, 1, file_list_body)).is_error());

    // DOMQuad v1 whose first point is a Blob, not a DOMPoint.
    EXPECT(storage_deserialize(serializable_storage_record("DOMQuad"sv, 1, blob_body)).is_error());

    // ImageData v1 whose data sub-value is a DOMPoint, not a Uint8ClampedArray.
    EXPECT(storage_deserialize(serializable_storage_record("ImageData"sv, 1, point_body)).is_error());
}

TEST_CASE(storage_reader_rejects_unknown_version_in_nested_serializable)
{
    // Version rejection must recurse into payload bodies.
    Vector<u8> nested_file;
    nested_file.append(to_underlying(ValueTag::SerializableObject));
    append_storage_ascii_utf16(nested_file, "File"sv);
    append_storage_leb128(nested_file, 255); // unknown File payload version

    Vector<u8> file_list_body;
    append_storage_leb128(file_list_body, 1); // file count
    file_list_body.append(nested_file.data(), nested_file.size());

    EXPECT(storage_deserialize(serializable_storage_record("FileList"sv, 1, file_list_body)).is_error());
}

TEST_CASE(storage_reader_rejects_malformed_container_and_truncation)
{
    auto deserialize_bytes = [](Vector<u8> const& bytes) {
        Web::HTML::StorageSerializationRecord record { MUST(ByteBuffer::copy(bytes.span())) };
        return storage_deserialize(record).is_error();
    };

    // Invalid magic.
    {
        Vector<u8> bytes { 'N', 'O', 'P', 'E' };
        append_storage_leb128(bytes, 1);
        append_storage_leb128(bytes, 0);
        bytes.append(to_underlying(ValueTag::SerializableObject));
        EXPECT(deserialize_bytes(bytes));
    }

    // Unsupported storage format version.
    {
        Vector<u8> bytes { 'L', 'B', 'S', 'C' };
        append_storage_leb128(bytes, 2);
        append_storage_leb128(bytes, 0);
        EXPECT(deserialize_bytes(bytes));
    }

    // Unsupported storage format version in a multi-byte varint.
    {
        Vector<u8> bytes { 'L', 'B', 'S', 'C' };
        append_storage_leb128(bytes, 0x10000);
        append_storage_leb128(bytes, 0);
        EXPECT(deserialize_bytes(bytes));
    }

    // Unknown header flags.
    {
        Vector<u8> bytes { 'L', 'B', 'S', 'C' };
        append_storage_leb128(bytes, 1);
        append_storage_leb128(bytes, 1);
        bytes.append(to_underlying(ValueTag::NullPrimitive));
        EXPECT(deserialize_bytes(bytes));
    }

    // Unknown header flags in a multi-byte varint.
    {
        Vector<u8> bytes { 'L', 'B', 'S', 'C' };
        append_storage_leb128(bytes, 1);
        append_storage_leb128(bytes, 0x10000);
        bytes.append(to_underlying(ValueTag::NullPrimitive));
        EXPECT(deserialize_bytes(bytes));
    }

    // Truncated right after the interface identifier — the payload-version varint is missing.
    {
        Vector<u8> bytes { 'L', 'B', 'S', 'C' };
        append_storage_leb128(bytes, 1);
        append_storage_leb128(bytes, 0);
        bytes.append(to_underlying(ValueTag::SerializableObject));
        append_storage_ascii_utf16(bytes, "DOMPoint"sv);
        EXPECT(deserialize_bytes(bytes));
    }
}

TEST_CASE(storage_reader_rejects_invalid_value_tags)
{
    auto with_tag = [](u8 tag) {
        Vector<u8> value;
        value.append(tag);
        return storage_record_with_value(value);
    };

    EXPECT(storage_deserialize(with_tag(to_underlying(ValueTag::Empty))).is_error()); // Reserved value.
    EXPECT(storage_deserialize(with_tag(99)).is_error());                             // Beyond the last defined tag.
}

TEST_CASE(storage_reader_rejects_a_truncated_or_overlong_int32)
{
    auto int32_record = [](Vector<u8> const& body) {
        Vector<u8> value { to_underlying(ValueTag::Int32Primitive) };
        value.append(body.data(), body.size());
        return storage_record_with_value(value);
    };

    // No varint at all after the tag.
    EXPECT(storage_deserialize(int32_record({})).is_error());

    // A varint whose continuation bit is set but whose remaining bytes never arrive.
    EXPECT(storage_deserialize(int32_record({ 0x80 })).is_error());

    // More continuation bytes than an i32 can hold.
    EXPECT(storage_deserialize(int32_record({ 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 })).is_error());

    // A well-formed signed LEB128 still decodes.
    Vector<u8> body;
    append_storage_signed_leb128(body, -2);
    auto decoded = MUST(storage_deserialize(int32_record(body)));
    EXPECT(decoded.is_int32());
    EXPECT_EQ(decoded.as_i32(), -2);
}

TEST_CASE(storage_reader_rejects_trailing_bytes)
{
    Vector<u8> with_trailing;
    with_trailing.append(to_underlying(ValueTag::NullPrimitive));
    with_trailing.append(0xff); // junk after the value
    EXPECT(storage_deserialize(storage_record_with_value(with_trailing)).is_error());

    Vector<u8> clean;
    clean.append(to_underlying(ValueTag::NullPrimitive));
    EXPECT(!storage_deserialize(storage_record_with_value(clean)).is_error());
}

TEST_CASE(storage_reader_rejects_ascii_flagged_string_with_non_ascii_bytes)
{
    Vector<u8> value { to_underlying(ValueTag::StringPrimitive) };
    append_storage_string_header(value, /*is_ascii=*/true, 1);
    value.append(0x80); // not ASCII
    EXPECT(storage_deserialize(storage_record_with_value(value)).is_error());
}

TEST_CASE(storage_reader_decodes_ascii_content_flagged_as_utf16)
{
    // Compact string storage is not enforced on read.
    Vector<u8> value { to_underlying(ValueTag::StringPrimitive) };
    Array<u16, 2> code_units { 'h', 'i' };
    append_storage_utf16(value, code_units);
    auto decoded = MUST(storage_deserialize(storage_record_with_value(value)));
    EXPECT(decoded.is_string());
    expect_utf16_equals(decoded.as_string().utf16_string(), Utf16String::from_utf8("hi"sv));
}

TEST_CASE(storage_reader_decodes_a_mixed_ascii_and_non_ascii_string)
{
    Vector<u8> value { to_underlying(ValueTag::StringPrimitive) };
    Array<u16, 4> code_units { 'c', 'a', 'f', 0x00E9 }; // "café"
    append_storage_utf16(value, code_units);
    auto decoded = MUST(storage_deserialize(storage_record_with_value(value)));
    EXPECT(decoded.is_string());
    expect_utf16_equals(decoded.as_string().utf16_string(), Utf16String::from_utf8("café"sv));
}

TEST_CASE(storage_reader_rejects_a_string_with_fewer_bytes_than_its_length_claim)
{
    auto string_record = [](Vector<u8> const& body) {
        Vector<u8> value { to_underlying(ValueTag::StringPrimitive) };
        value.append(body.data(), body.size());
        return storage_record_with_value(value);
    };

    // ASCII: one byte per code unit, so two code units need two bytes — only one is present.
    {
        Vector<u8> body;
        append_storage_string_header(body, /*is_ascii=*/true, 2);
        body.append('h'); // one byte, not two
        EXPECT(storage_deserialize(string_record(body)).is_error());
    }

    // Non-ASCII: two bytes per code unit, so two code units need four bytes — only two are present.
    {
        Vector<u8> body;
        append_storage_string_header(body, /*is_ascii=*/false, 2);
        append_little_endian_u16(body, u'h'); // one code unit (two bytes), not two
        EXPECT(storage_deserialize(string_record(body)).is_error());
    }
}

TEST_CASE(storage_reader_rejects_a_string_with_a_missing_or_truncated_length)
{
    auto string_record = [](Vector<u8> const& body) {
        Vector<u8> value { to_underlying(ValueTag::StringPrimitive) };
        value.append(body.data(), body.size());
        return storage_record_with_value(value);
    };

    // No length varint at all.
    EXPECT(storage_deserialize(string_record({})).is_error());

    // A length varint whose continuation bit is set but whose remaining bytes never arrive.
    {
        Vector<u8> body;
        body.append(0x80); // continues, then EOF
        EXPECT(storage_deserialize(string_record(body)).is_error());
    }
}

TEST_CASE(storage_reader_decodes_a_string_with_a_non_canonical_length)
{
    // LEB128 canonical form is not enforced on read.
    Vector<u8> value { to_underlying(ValueTag::StringPrimitive) };
    value.append(0x85); // (2 << 1) | ascii == 5, written overlong as two bytes
    value.append(0x00);
    value.append('h');
    value.append('i');
    auto decoded = MUST(storage_deserialize(storage_record_with_value(value)));
    EXPECT(decoded.is_string());
    expect_utf16_equals(decoded.as_string().utf16_string(), Utf16String::from_utf8("hi"sv));
}

TEST_CASE(storage_reader_reads_a_string_to_its_length_not_the_end_of_the_record)
{
    // A surplus byte after the declared string length is trailing data.
    Vector<u8> value { to_underlying(ValueTag::StringPrimitive) };
    append_storage_ascii_utf16(value, "hi"sv);
    value.append(0xFF); // one byte past the string
    EXPECT(storage_deserialize(storage_record_with_value(value)).is_error());
}

TEST_CASE(storage_reader_rejects_invalid_bigint)
{
    auto with_bytes = [](ReadonlyBytes signed_binary) {
        Vector<u8> value;
        value.append(to_underlying(ValueTag::BigIntPrimitive));
        append_storage_leb128(value, signed_binary.size());
        value.append(signed_binary.data(), signed_binary.size());
        return storage_record_with_value(value);
    };

    // A signed-binary BigInt needs at least the sign byte.
    EXPECT(storage_deserialize(with_bytes({})).is_error());

    Array<u8, 3> valid { 0x00, 0x03, 0xE7 }; // +999
    EXPECT(!storage_deserialize(with_bytes({ valid.data(), valid.size() })).is_error());
}

TEST_CASE(storage_reader_rejects_non_array_buffer_backed_view)
{
    Vector<u8> value;
    value.append(to_underlying(ValueTag::ArrayBufferView));
    value.append(to_underlying(ValueTag::NullPrimitive)); // the backing sub-value
    EXPECT(storage_deserialize(storage_record_with_value(value)).is_error());
}
TEST_CASE(storage_reader_rejects_out_of_bounds_array_buffer_view)
{
    EXPECT(storage_deserialize(array_buffer_view_record("Float64Array"sv, BackingBuffer::Fixed, 8, 16, 0, 2)).is_error());

    // Caught in 64 bits before the engine's u32 element-access check can wrap.
    EXPECT(storage_deserialize(array_buffer_view_record("Float64Array"sv, BackingBuffer::Fixed, 8, 0, 0, 0x20000000)).is_error());

    EXPECT(storage_deserialize(array_buffer_view_record("Float64Array"sv, BackingBuffer::Fixed, 16, 1000, 0, 2)).is_error());

    EXPECT(storage_deserialize(array_buffer_view_record("Float64Array"sv, BackingBuffer::Fixed, 16, 8, 1, 1)).is_error());

    EXPECT(!storage_deserialize(array_buffer_view_record("Float64Array"sv, BackingBuffer::Fixed, 16, 16, 0, 2)).is_error());
}

TEST_CASE(storage_reader_validates_length_tracking_array_buffer_views)
{
    // Length-tracking views are only valid over resizable buffers.
    EXPECT(storage_deserialize(array_buffer_view_record("Float64Array"sv, BackingBuffer::Fixed, 16, {}, 0, {})).is_error());
    EXPECT(storage_deserialize(array_buffer_view_record("DataView"sv, BackingBuffer::Fixed, 16, {}, 0)).is_error());

    EXPECT(storage_deserialize(array_buffer_view_record("Float64Array"sv, BackingBuffer::Resizable, 16, {}, 0, 2)).is_error());

    EXPECT(!storage_deserialize(array_buffer_view_record("Float64Array"sv, BackingBuffer::Resizable, 16, {}, 0, {})).is_error());
    EXPECT(!storage_deserialize(array_buffer_view_record("DataView"sv, BackingBuffer::Resizable, 16, {}, 0)).is_error());
}

TEST_CASE(storage_reader_rejects_array_with_invalid_length_property)
{
    // A corrupt record can try to redefine Array's non-configurable length property.
    auto array_with_length_value = [](Vector<u8> const& length_value) {
        Vector<u8> value;
        value.append(to_underlying(ValueTag::ArrayObject));
        append_storage_leb128(value, 0);                        // ArrayCreate length
        value.append(length_value.data(), length_value.size()); // property value, then its key
        append_storage_ascii_utf16(value, "length"sv);
        value.append(to_underlying(ValueTag::EndObject));
        return storage_record_with_value(value);
    };

    Vector<u8> non_integer_length;
    non_integer_length.append(to_underlying(ValueTag::NumberPrimitive));
    append_little_endian_double(non_integer_length, 3.5);
    EXPECT(storage_deserialize(array_with_length_value(non_integer_length)).is_error());

    Vector<u8> integer_length;
    integer_length.append(to_underlying(ValueTag::NumberPrimitive));
    append_little_endian_double(integer_length, 5.0);
    EXPECT(storage_deserialize(array_with_length_value(integer_length)).is_error());
}

TEST_CASE(storage_reader_rejects_an_object_without_a_terminator)
{
    Vector<u8> value;
    value.append(to_underlying(ValueTag::Object));
    value.append(to_underlying(ValueTag::NumberPrimitive)); // one complete property...
    append_little_endian_double(value, 1.0);
    append_storage_ascii_utf16(value, "a"sv);
    // ...then EOF instead of EndObject.
    EXPECT(storage_deserialize(storage_record_with_value(value)).is_error());
}

TEST_CASE(storage_reader_rejects_end_object_tag_where_a_value_is_expected)
{
    auto rejects = [](Vector<u8> const& value) {
        EXPECT(storage_deserialize(storage_record_with_value(value)).is_error());
    };

    // At the top level.
    rejects({ to_underlying(ValueTag::EndObject) });

    // As a Map key.
    {
        Vector<u8> value { to_underlying(ValueTag::MapObject) };
        append_storage_leb128(value, 1);                  // entry count
        value.append(to_underlying(ValueTag::EndObject)); // key
        rejects(value);
    }

    // As a Map value (after a valid key).
    {
        Vector<u8> value { to_underlying(ValueTag::MapObject) };
        append_storage_leb128(value, 1);                        // entry count
        value.append(to_underlying(ValueTag::NumberPrimitive)); // key
        append_little_endian_double(value, 1.0);
        value.append(to_underlying(ValueTag::EndObject)); // value
        rejects(value);
    }

    // As a Set entry.
    {
        Vector<u8> value { to_underlying(ValueTag::SetObject) };
        append_storage_leb128(value, 1);                  // entry count
        value.append(to_underlying(ValueTag::EndObject)); // entry
        rejects(value);
    }
}

TEST_CASE(storage_reader_does_not_overallocate_on_huge_vector_count)
{
    Vector<u8> value;
    append_storage_leb128(value, NumericLimits<u64>::max());
    auto record = storage_record_with_value(value);
    Web::HTML::StructuredSerializeReader reader { record };
    EXPECT(reader.decode<Vector<Web::Bindings::KeyUsage>>().is_error());
}

TEST_CASE(crypto_key_with_truncated_handle_is_rejected)
{
    Array<u8, 8> handle_bytes {};
    ReadonlyBytes handle { handle_bytes.data(), handle_bytes.size() };

    EXPECT(crypto_storage_deserialize(crypto_key_secret_record(handle, 128)).is_error());

    EXPECT(!crypto_storage_deserialize(crypto_key_secret_record(handle, handle.size())).is_error());
}
