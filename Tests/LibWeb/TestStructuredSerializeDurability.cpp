/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StructuredSerializeTestHelpers.h"

#include <AK/FlyString.h>
#include <AK/NumericLimits.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/BigIntObject.h>
#include <LibJS/Runtime/BooleanObject.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/NumberObject.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibWeb/Crypto/CryptoKeySerializationTags.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/FileAPI/FileList.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/Geometry/DOMMatrixReadOnly.h>
#include <LibWeb/Geometry/DOMPointReadOnly.h>
#include <LibWeb/Geometry/DOMQuad.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectReadOnly.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

using Web::Crypto::HandleTag;
using Web::Crypto::KeyAlgorithmTag;

TEST_CASE(serializable_objects_round_trip_through_storage)
{
    auto& realm = test_realm();

    {
        auto point = Web::Geometry::DOMPoint::create(realm);
        point->set_x(1.5);
        point->set_y(-2.5);
        point->set_z(3.25);
        point->set_w(4.75);

        auto value = MUST(storage_deserialize(storage_serialize(point)));
        auto& decoded = as<Web::Geometry::DOMPoint>(value.as_object());
        EXPECT_EQ(decoded.x(), 1.5);
        EXPECT_EQ(decoded.y(), -2.5);
        EXPECT_EQ(decoded.z(), 3.25);
        EXPECT_EQ(decoded.w(), 4.75);
    }

    {
        auto blob = Web::FileAPI::Blob::create(realm, MUST(ByteBuffer::copy("hello"sv.bytes())), "text/plain"_utf16);

        auto value = MUST(storage_deserialize(storage_serialize(blob)));
        auto& decoded = as<Web::FileAPI::Blob>(value.as_object());
        EXPECT_EQ(decoded.type(), "text/plain"_utf16);
        EXPECT_EQ(decoded.raw_bytes(), "hello"sv.bytes());
    }
}

TEST_CASE(storage_round_trips_serializable_with_nested_sub_value_before_more_fields)
{
    auto& realm = test_realm();

    // Nested sub-values must not trigger the outer full-consumption check.
    auto point = Web::Geometry::DOMPoint::create(realm);
    point->set_x(7.0);
    auto point_body = serialized_object_body(point);

    Vector<u8> quad_body;
    for (size_t i = 0; i < 4; ++i)
        quad_body.append(point_body.data(), point_body.size());

    auto value = MUST(storage_deserialize(serializable_storage_record("DOMQuad"sv, 1, quad_body)));
    EXPECT(value.is_object());
}

TEST_CASE(crypto_key_storage_record_round_trips)
{
    // Exercise the HKDF raw-bytes CryptoKey shape seen in the wild.
    Array<u8, 32> handle_bytes;
    for (size_t i = 0; i < handle_bytes.size(); ++i)
        handle_bytes[i] = static_cast<u8>(i);
    ReadonlyBytes handle { handle_bytes.data(), handle_bytes.size() };

    auto value = MUST(crypto_storage_deserialize(crypto_key_secret_record(handle, handle.size())));
    auto& key = as<Web::Crypto::CryptoKey>(value.as_object());
    EXPECT_EQ(key.type(), Web::Bindings::KeyType::Secret);
    EXPECT_EQ(key.extractable(), false);
    EXPECT_EQ(key.algorithm_name(), "HKDF"_string);
    EXPECT_EQ(key.internal_usages().size(), 1u);
    EXPECT_EQ(key.internal_usages()[0], Web::Bindings::KeyUsage::Derivekey);
    EXPECT(key.handle().has<ByteBuffer>());
    EXPECT_EQ(key.handle().get<ByteBuffer>().span(), handle);
}

TEST_CASE(value_tag_primitives_round_trip)
{
    auto decode = [](Vector<u8> const& value) { return MUST(storage_deserialize(storage_record_with_value(value))); };

    EXPECT(decode({ to_underlying(ValueTag::UndefinedPrimitive) }).is_undefined());
    EXPECT(decode({ to_underlying(ValueTag::NullPrimitive) }).is_null());

    {
        auto value = decode({ to_underlying(ValueTag::BooleanPrimitive), 1 });
        EXPECT(value.is_boolean());
        EXPECT_EQ(value.as_bool(), true);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::NumberPrimitive) };
        append_little_endian_double(value, 13.5);
        auto decoded = decode(value);
        EXPECT(decoded.is_number());
        EXPECT_EQ(decoded.as_double(), 13.5);
    }
    {
        for (i32 number : { 0, 1, -1, NumericLimits<i32>::max(), NumericLimits<i32>::min() }) {
            Vector<u8> value { to_underlying(ValueTag::Int32Primitive) };
            append_storage_signed_leb128(value, number);
            auto decoded = decode(value);
            EXPECT(decoded.is_number());
            EXPECT(decoded.is_int32());
            EXPECT_EQ(decoded.as_i32(), number);
        }
    }
    {
        Vector<u8> value { to_underlying(ValueTag::StringPrimitive) };
        append_storage_ascii_utf16(value, "hi"sv);
        auto decoded = decode(value);
        EXPECT(decoded.is_string());
        auto const& string = decoded.as_string().utf16_string();
        expect_utf16_equals(string, "hi"_utf16);
        EXPECT(string.utf16_view().has_ascii_storage());
    }
    {
        Vector<u8> value { to_underlying(ValueTag::StringPrimitive) };
        Array<u16, 3> code_units { 0x00E9, 0xD83E, 0xDDAC };
        append_storage_utf16(value, code_units);
        auto decoded = decode(value);
        EXPECT(decoded.is_string());
        Array<char16_t, 3> expected_units { 0x00E9, 0xD83E, 0xDDAC };
        auto const& string = decoded.as_string().utf16_string();
        expect_utf16_equals(string, Utf16String::from_utf16({ expected_units.data(), expected_units.size() }));
        EXPECT(!string.utf16_view().has_ascii_storage());
    }
    {
        Vector<u8> value { to_underlying(ValueTag::StringPrimitive) };
        Array<u16, 1> code_units { 0xD800 };
        append_storage_utf16(value, code_units);
        auto decoded = decode(value);
        EXPECT(decoded.is_string());
        EXPECT_EQ(decoded.as_string().utf16_string().length_in_code_units(), 1u);
        EXPECT_EQ(decoded.as_string().utf16_string().code_unit_at(0), 0xD800u);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::BigIntPrimitive) };
        Array<u8, 14> big_integer { 0x00, 0x01, 0x8E, 0xE9, 0x0F, 0xF6, 0xC3, 0x73, 0xE0, 0xEE, 0x4E, 0x3F, 0x0A, 0xD2 };
        append_storage_leb128(value, big_integer.size());
        value.append(big_integer.data(), big_integer.size());
        auto decoded = decode(value);
        EXPECT(decoded.is_bigint());
        EXPECT_EQ(MUST(decoded.as_bigint().big_integer().to_base(10)), "123456789012345678901234567890"_string);
    }
}

TEST_CASE(value_tag_wrapper_objects_round_trip)
{
    auto decode = [](Vector<u8> const& value) { return MUST(storage_deserialize(storage_record_with_value(value))); };

    {
        auto decoded = decode({ to_underlying(ValueTag::BooleanObject), 1 });
        EXPECT_EQ(as<JS::BooleanObject>(decoded.as_object()).boolean(), true);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::NumberObject) };
        append_little_endian_double(value, 2.5);
        EXPECT_EQ(as<JS::NumberObject>(decode(value).as_object()).number(), 2.5);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::StringObject) };
        append_storage_ascii_utf16(value, "wrap"sv);
        expect_utf16_equals(as<JS::StringObject>(decode(value).as_object()).primitive_string().utf16_string(), "wrap"_utf16);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::BigIntObject) };
        Array<u8, 3> big_integer { 0x00, 0x03, 0xE7 }; // +999
        append_storage_leb128(value, big_integer.size());
        value.append(big_integer.data(), big_integer.size());
        EXPECT_EQ(MUST(as<JS::BigIntObject>(decode(value).as_object()).bigint().big_integer().to_base(10)), "999"_string);
    }
}

TEST_CASE(value_tag_date_and_regexp_round_trip)
{
    auto decode = [](Vector<u8> const& value) { return MUST(storage_deserialize(storage_record_with_value(value))); };

    {
        Vector<u8> value { to_underlying(ValueTag::DateObject) };
        append_little_endian_double(value, 1704067200000.0);
        EXPECT_EQ(as<JS::Date>(decode(value).as_object()).date_value(), 1704067200000.0);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::RegExpObject) };
        append_storage_ascii_utf16(value, "ab"sv);
        append_storage_ascii_utf16(value, "gi"sv);
        auto& regexp = as<JS::RegExpObject>(decode(value).as_object());
        expect_utf16_equals(regexp.pattern(), "ab"_utf16);
        expect_utf16_equals(regexp.flags(), "gi"_utf16);
    }
}

TEST_CASE(value_tag_map_and_set_round_trip)
{
    {
        Vector<u8> value { to_underlying(ValueTag::MapObject) };
        append_storage_leb128(value, 1); // entry count
        value.append(to_underlying(ValueTag::NumberPrimitive));
        append_little_endian_double(value, 1.0);
        value.append(to_underlying(ValueTag::NumberPrimitive));
        append_little_endian_double(value, 2.0);
        auto& map = as<JS::Map>(MUST(storage_deserialize(storage_record_with_value(value))).as_object());
        EXPECT_EQ(map.map_size(), 1u);
        auto entry = map.map_get(JS::Value(1.0));
        EXPECT(entry.has_value());
        EXPECT_EQ(entry.value().as_double(), 2.0);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::SetObject) };
        append_storage_leb128(value, 2); // entry count
        value.append(to_underlying(ValueTag::NumberPrimitive));
        append_little_endian_double(value, 7.0);
        value.append(to_underlying(ValueTag::NumberPrimitive));
        append_little_endian_double(value, 8.0);
        auto& set = as<JS::Set>(MUST(storage_deserialize(storage_record_with_value(value))).as_object());
        EXPECT_EQ(set.set_size(), 2u);
        EXPECT(set.set_has(JS::Value(7.0)));
        EXPECT(set.set_has(JS::Value(8.0)));
        EXPECT(!set.set_has(JS::Value(9.0)));
    }
}

TEST_CASE(value_tag_array_and_object_round_trip)
{
    {
        Vector<u8> value { to_underlying(ValueTag::ArrayObject) };
        append_storage_leb128(value, 2); // array length
        // Property values precede their keys; EndObject terminates the list.
        value.append(to_underlying(ValueTag::NumberPrimitive));
        append_little_endian_double(value, 10.0);
        append_storage_ascii_utf16(value, "0"sv);
        value.append(to_underlying(ValueTag::StringPrimitive));
        append_storage_ascii_utf16(value, "x"sv);
        append_storage_ascii_utf16(value, "1"sv);
        value.append(to_underlying(ValueTag::EndObject));
        auto& object = MUST(storage_deserialize(storage_record_with_value(value))).as_object();
        EXPECT(is<JS::Array>(object));
        EXPECT_EQ(MUST(object.get(0u)).as_double(), 10.0);
        expect_utf16_equals(MUST(object.get(1u)).as_string().utf16_string(), "x"_utf16);
    }
    {
        Vector<u8> value { to_underlying(ValueTag::Object) };
        value.append(to_underlying(ValueTag::NumberPrimitive));
        append_little_endian_double(value, 42.0);
        append_storage_ascii_utf16(value, "answer"sv);
        value.append(to_underlying(ValueTag::EndObject));
        auto& object = MUST(storage_deserialize(storage_record_with_value(value))).as_object();
        EXPECT_EQ(MUST(object.get(JS::PropertyKey { "answer"_utf16 })).as_double(), 42.0);
    }
}

TEST_CASE(value_tag_object_reference_aliases_prior_object)
{
    // The second slot must alias the object created for the first slot.
    Vector<u8> value { to_underlying(ValueTag::ArrayObject) };
    append_storage_leb128(value, 2); // array length
    value.append(to_underlying(ValueTag::Object));
    value.append(to_underlying(ValueTag::EndObject));
    append_storage_ascii_utf16(value, "0"sv);
    value.append(to_underlying(ValueTag::ObjectReference));
    append_storage_leb128(value, 1);
    append_storage_ascii_utf16(value, "1"sv);
    value.append(to_underlying(ValueTag::EndObject));

    auto& object = MUST(storage_deserialize(storage_record_with_value(value))).as_object();
    EXPECT(is<JS::Array>(object));
    auto first = MUST(object.get(0u));
    auto second = MUST(object.get(1u));
    EXPECT(first.is_object());
    EXPECT(second.is_object());
    EXPECT(&first.as_object() == &second.as_object());
}

TEST_CASE(value_tag_error_objects_round_trip)
{
    auto& vm = test_realm().vm();
    auto decode_error = [](StringView type_name, StringView message) {
        Vector<u8> value { to_underlying(ValueTag::ErrorObject) };
        append_storage_string(value, type_name);
        value.append(1); // Optional message: present
        append_storage_ascii_utf16(value, message);
        value.append(0); // Optional cause: absent
        return MUST(storage_deserialize(storage_record_with_value(value)));
    };

    EXPECT(is<JS::EvalError>(decode_error("EvalError"sv, "m"sv).as_object()));
    EXPECT(is<JS::InternalError>(decode_error("InternalError"sv, "m"sv).as_object()));
    EXPECT(is<JS::RangeError>(decode_error("RangeError"sv, "m"sv).as_object()));
    EXPECT(is<JS::ReferenceError>(decode_error("ReferenceError"sv, "m"sv).as_object()));
    EXPECT(is<JS::SyntaxError>(decode_error("SyntaxError"sv, "m"sv).as_object()));
    EXPECT(is<JS::TypeError>(decode_error("TypeError"sv, "m"sv).as_object()));
    EXPECT(is<JS::URIError>(decode_error("URIError"sv, "m"sv).as_object()));

    auto base = decode_error("Error"sv, "boom"sv);
    EXPECT(is<JS::Error>(base.as_object()));
    expect_utf16_equals(MUST(base.as_object().get(vm.names.name)).as_string().utf16_string(), "Error"_utf16);
    expect_utf16_equals(MUST(base.as_object().get(vm.names.message)).as_string().utf16_string(), "boom"_utf16);
}

TEST_CASE(value_tag_array_buffers_round_trip)
{
    Array<u8, 4> contents { 0xde, 0xad, 0xbe, 0xef };
    {
        Vector<u8> value { to_underlying(ValueTag::ArrayBuffer) };
        append_storage_leb128(value, contents.size());
        value.append(contents.data(), contents.size());
        auto& buffer = as<JS::ArrayBuffer>(MUST(storage_deserialize(storage_record_with_value(value))).as_object());
        EXPECT(buffer.is_fixed_length());
        EXPECT_EQ(buffer.byte_length(), 4u);
        EXPECT_EQ(MUST(buffer.copy_to_byte_buffer()).span(), contents.span());
    }
    {
        Vector<u8> value { to_underlying(ValueTag::ResizeableArrayBuffer) };
        append_storage_leb128(value, contents.size());
        value.append(contents.data(), contents.size());
        append_storage_leb128(value, 8); // max byte length
        auto& buffer = as<JS::ArrayBuffer>(MUST(storage_deserialize(storage_record_with_value(value))).as_object());
        EXPECT(!buffer.is_fixed_length());
        EXPECT_EQ(buffer.byte_length(), 4u);
        EXPECT_EQ(buffer.max_byte_length(), 8u);
        EXPECT_EQ(MUST(buffer.copy_to_byte_buffer()).span(), contents.span());
    }
}

TEST_CASE(value_tag_array_buffer_views_round_trip)
{
    {
        Vector<u8> backing;
        append_little_endian_double(backing, 1.5);
        append_little_endian_double(backing, 2.5);
        Vector<u8> value;
        append_array_buffer_view_value(value, "Float64Array"sv, BackingBuffer::Fixed, backing, 16u, 0u, 2u);
        auto decoded = MUST(storage_deserialize(storage_record_with_value(value)));
        auto& view = as<JS::TypedArrayBase>(decoded.as_object());
        EXPECT_EQ(view.byte_offset(), 0u);
        EXPECT(!view.array_length().is_auto());
        EXPECT_EQ(view.array_length().length(), 2u);
        EXPECT_EQ(view.byte_length().length(), 16u);
        EXPECT_EQ(MUST(decoded.as_object().get(0u)).as_double(), 1.5);
        EXPECT_EQ(MUST(decoded.as_object().get(1u)).as_double(), 2.5);
    }
    {
        Vector<u8> backing;
        backing.resize(8);
        Vector<u8> value;
        append_array_buffer_view_value(value, "DataView"sv, BackingBuffer::Fixed, backing, 8u, 0u);
        auto& view = as<JS::DataView>(MUST(storage_deserialize(storage_record_with_value(value))).as_object());
        EXPECT_EQ(view.byte_offset(), 0u);
        EXPECT_EQ(view.byte_length().length(), 8u);
    }
}

TEST_CASE(geometry_serializables_round_trip)
{
    Vector<u8> point_body;
    append_little_endian_double(point_body, 1.5);
    append_little_endian_double(point_body, -2.5);
    append_little_endian_double(point_body, 3.5);
    append_little_endian_double(point_body, -4.5);

    {
        auto& point = as<Web::Geometry::DOMPointReadOnly>(MUST(storage_deserialize(serializable_storage_record("DOMPointReadOnly"sv, 1, point_body))).as_object());
        EXPECT_EQ(point.x(), 1.5);
        EXPECT_EQ(point.y(), -2.5);
        EXPECT_EQ(point.z(), 3.5);
        EXPECT_EQ(point.w(), -4.5);
    }
    {
        auto& point = as<Web::Geometry::DOMPoint>(MUST(storage_deserialize(serializable_storage_record("DOMPoint"sv, 1, point_body))).as_object());
        EXPECT_EQ(point.x(), 1.5);
        EXPECT_EQ(point.w(), -4.5);
    }

    Vector<u8> rect_body;
    append_little_endian_double(rect_body, 10.0);
    append_little_endian_double(rect_body, 20.0);
    append_little_endian_double(rect_body, 30.0);
    append_little_endian_double(rect_body, 40.0);

    {
        auto& rect = as<Web::Geometry::DOMRectReadOnly>(MUST(storage_deserialize(serializable_storage_record("DOMRectReadOnly"sv, 1, rect_body))).as_object());
        EXPECT_EQ(rect.x(), 10.0);
        EXPECT_EQ(rect.y(), 20.0);
        EXPECT_EQ(rect.width(), 30.0);
        EXPECT_EQ(rect.height(), 40.0);
    }
    {
        auto& rect = as<Web::Geometry::DOMRect>(MUST(storage_deserialize(serializable_storage_record("DOMRect"sv, 1, rect_body))).as_object());
        EXPECT_EQ(rect.width(), 30.0);
        EXPECT_EQ(rect.height(), 40.0);
    }

    {
        Vector<u8> body;
        body.append(1);
        for (double element : { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0 })
            append_little_endian_double(body, element);
        auto& matrix = as<Web::Geometry::DOMMatrixReadOnly>(MUST(storage_deserialize(serializable_storage_record("DOMMatrixReadOnly"sv, 1, body))).as_object());
        EXPECT(matrix.is2d());
        EXPECT_EQ(matrix.m11(), 1.0);
        EXPECT_EQ(matrix.m22(), 4.0);
        EXPECT_EQ(matrix.m42(), 6.0);
    }
    {
        Vector<u8> body;
        body.append(0);
        for (int i = 1; i <= 16; ++i)
            append_little_endian_double(body, static_cast<double>(i));
        auto& matrix = as<Web::Geometry::DOMMatrix>(MUST(storage_deserialize(serializable_storage_record("DOMMatrix"sv, 1, body))).as_object());
        EXPECT(!matrix.is2d());
        EXPECT_EQ(matrix.m11(), 1.0);
        EXPECT_EQ(matrix.m22(), 6.0);
        EXPECT_EQ(matrix.m44(), 16.0);
    }
    {
        Vector<u8> body;
        for (int k = 0; k < 4; ++k) {
            Vector<u8> nested_point;
            append_little_endian_double(nested_point, (k * 4) + 1.0);
            append_little_endian_double(nested_point, (k * 4) + 2.0);
            append_little_endian_double(nested_point, (k * 4) + 3.0);
            append_little_endian_double(nested_point, (k * 4) + 4.0);
            frozen_serializable_value(body, "DOMPoint"sv, 1, nested_point);
        }
        auto& quad = as<Web::Geometry::DOMQuad>(MUST(storage_deserialize(serializable_storage_record("DOMQuad"sv, 1, body))).as_object());
        EXPECT_EQ(quad.p1()->x(), 1.0);
        EXPECT_EQ(quad.p2()->x(), 5.0);
        EXPECT_EQ(quad.p4()->w(), 16.0);
    }
}

TEST_CASE(file_api_serializables_round_trip)
{
    {
        Vector<u8> body;
        append_storage_string(body, "text/plain"sv);
        append_storage_bytes(body, "hello"sv.bytes()); // ByteBuffer body
        auto& blob = as<Web::FileAPI::Blob>(MUST(storage_deserialize(serializable_storage_record("Blob"sv, 1, body))).as_object());
        EXPECT_EQ(blob.type(), "text/plain"_string);
        EXPECT_EQ(blob.raw_bytes(), "hello"sv.bytes());
    }

    Vector<u8> file_body;
    append_storage_string(file_body, "text/plain"sv);
    append_storage_bytes(file_body, "data"sv.bytes()); // ByteBuffer body
    append_storage_string(file_body, "f.txt"sv);
    append_storage_signed_leb128(file_body, 1234); // i64 last modified

    {
        auto& file = as<Web::FileAPI::File>(MUST(storage_deserialize(serializable_storage_record("File"sv, 1, file_body))).as_object());
        EXPECT_EQ(file.name(), "f.txt"_string);
        EXPECT_EQ(file.last_modified(), 1234);
        EXPECT_EQ(file.type(), "text/plain"_string);
        EXPECT_EQ(file.raw_bytes(), "data"sv.bytes());
    }

    {
        Vector<u8> body;
        append_storage_leb128(body, 1);
        frozen_serializable_value(body, "File"sv, 1, file_body);
        auto& list = as<Web::FileAPI::FileList>(MUST(storage_deserialize(serializable_storage_record("FileList"sv, 1, body))).as_object());
        EXPECT_EQ(list.length(), 1u);
        EXPECT_EQ(list.item(0)->name(), "f.txt"_string);
    }
}

TEST_CASE(webidl_serializables_round_trip)
{
    {
        Vector<u8> body;
        append_storage_string(body, "AbortError"sv);
        append_storage_ascii_utf16(body, "stop"sv);
        auto& exception = as<Web::WebIDL::DOMException>(MUST(storage_deserialize(serializable_storage_record("DOMException"sv, 1, body))).as_object());
        EXPECT(exception.name() == "AbortError"sv);
        EXPECT(exception.message() == "stop"sv);
    }

    {
        Vector<u8> body;
        append_storage_string(body, "QuotaExceededError"sv);
        append_storage_ascii_utf16(body, "over"sv);
        body.append(1); // quota present
        append_little_endian_double(body, 100.0);
        body.append(1); // requested present
        append_little_endian_double(body, 200.0);
        auto& error = as<Web::WebIDL::QuotaExceededError>(MUST(storage_deserialize(serializable_storage_record("QuotaExceededError"sv, 1, body))).as_object());
        EXPECT(error.name() == "QuotaExceededError"sv);
        EXPECT(error.message() == "over"sv);
        EXPECT(error.quota().has_value());
        EXPECT_EQ(error.quota().value(), 100.0);
        EXPECT(error.requested().has_value());
        EXPECT_EQ(error.requested().value(), 200.0);
    }
}

TEST_CASE(image_serializables_round_trip)
{
    {
        Array<u8, 4> pixels { 1, 2, 3, 4 };
        auto& image = as<Web::HTML::ImageData>(MUST(storage_deserialize(frozen_image_data_record({ pixels.data(), pixels.size() }, 1, 1, "srgb"sv))).as_object());
        EXPECT_EQ(image.width(), 1u);
        EXPECT_EQ(image.height(), 1u);
        EXPECT(image.data() != nullptr);
    }

    {
        Array<u8, 16> pixels {};
        auto& bitmap = as<Web::HTML::ImageBitmap>(MUST(storage_deserialize(image_bitmap_record(2, 2, 8, "BGRA8888"sv, "premultiplied"sv, { pixels.data(), pixels.size() }))).as_object());
        EXPECT_EQ(bitmap.width(), 2u);
        EXPECT_EQ(bitmap.height(), 2u);
    }
}

TEST_CASE(crypto_key_algorithm_variants_decode_from_frozen_bytes)
{
    for (auto const& golden : crypto_algorithm_goldens()) {
        auto record = crypto_key_full_record(golden.type, golden.usage, golden.algorithm, golden.handle);
        auto value = MUST(crypto_storage_deserialize(record));
        auto& key = as<Web::Crypto::CryptoKey>(value.as_object());

        if (golden.tag == KeyAlgorithmTag::EcKeyAlgorithm) {
            auto& algorithm = as<Web::Crypto::EcKeyAlgorithm>(*key.algorithm());
            expect_utf16_equals(algorithm.named_curve(), "P-256"_utf16);
        }
    }
}

TEST_CASE(crypto_key_handle_variants_decode_from_frozen_bytes)
{
    for (auto const& golden : crypto_handle_goldens()) {
        auto record = crypto_key_full_record(golden.type, golden.usage, golden.algorithm, golden.handle);
        auto value = MUST(crypto_storage_deserialize(record));
        auto& key = as<Web::Crypto::CryptoKey>(value.as_object());

        switch (golden.tag) {
        case HandleTag::ByteBuffer:
            EXPECT(key.handle().has<ByteBuffer>());
            break;
        case HandleTag::RSAPublicKey:
            EXPECT(key.handle().has<::Crypto::PK::RSAPublicKey>());
            break;
        case HandleTag::RSAPrivateKey:
            EXPECT(key.handle().has<::Crypto::PK::RSAPrivateKey>());
            break;
        case HandleTag::ECPublicKey:
            EXPECT(key.handle().has<::Crypto::PK::ECPublicKey>());
            break;
        case HandleTag::ECPrivateKey:
            EXPECT(key.handle().has<::Crypto::PK::ECPrivateKey>());
            break;
        case HandleTag::MLDSAPublicKey:
            EXPECT(key.handle().has<::Crypto::PK::MLDSAPublicKey>());
            break;
        case HandleTag::MLDSAPrivateKey:
            EXPECT(key.handle().has<::Crypto::PK::MLDSAPrivateKey>());
            break;
        case HandleTag::MLKEMPublicKey:
            EXPECT(key.handle().has<::Crypto::PK::MLKEMPublicKey>());
            break;
        case HandleTag::MLKEMPrivateKey:
            EXPECT(key.handle().has<::Crypto::PK::MLKEMPrivateKey>());
            break;
        case HandleTag::OKPPublicKey:
            EXPECT(key.handle().has<Web::Crypto::OKPPublicKey>());
            break;
        case HandleTag::OKPPrivateKey:
            EXPECT(key.handle().has<Web::Crypto::OKPPrivateKey>());
            break;
        }
    }
}

TEST_CASE(crypto_key_handle_completeness)
{
    // Every HandleTag needs a decode golden.
    auto goldens = crypto_handle_goldens();
    auto has_golden = [&](HandleTag tag) {
        for (auto const& golden : goldens) {
            if (golden.tag == tag)
                return true;
        }
        return false;
    };

    for (auto const& entry : Web::Crypto::s_handle_tag_expectations) {
        EXPECT_EQ(entry.expectation, Web::HTML::Expectation::PositiveGolden);
        EXPECT(has_golden(entry.tag));
    }
}

TEST_CASE(crypto_key_payload_version_rejects)
{
    Array<u8, 8> handle_bytes {};
    ReadonlyBytes handle { handle_bytes.data(), handle_bytes.size() };
    EXPECT(crypto_storage_deserialize(crypto_key_secret_record(handle, handle.size())).is_error() == false);

    EXPECT(crypto_storage_deserialize(serializable_storage_record("CryptoKey"sv, 0)).is_error());
    EXPECT(crypto_storage_deserialize(serializable_storage_record("CryptoKey"sv, 255)).is_error());
    EXPECT(crypto_storage_deserialize(serializable_storage_record("CryptoKey"sv, 0x10000)).is_error());
}

TEST_CASE(every_typed_array_constructor_decodes_from_frozen_bytes)
{
    auto decode_view = [](StringView constructor, u32 element_size, u32 array_length) {
        auto buffer_size = element_size * array_length;
        Vector<u8> backing;
        backing.resize(buffer_size);
        Vector<u8> value;
        append_array_buffer_view_value(value, constructor, BackingBuffer::Fixed, backing, buffer_size, 0, array_length);
        return MUST(storage_deserialize(storage_record_with_value(value)));
    };

    // Enumerate the same typed-array list deserialization accepts.
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type)      \
    {                                                                                    \
        auto view = MUST(JS::ClassName::create(test_realm(), 0));                        \
        auto element_size = static_cast<u32>(view->element_size());                      \
        auto decoded = decode_view(#ClassName##sv, element_size, 2);                     \
        auto& base = as<JS::TypedArrayBase>(decoded.as_object());                        \
        EXPECT(is<JS::ClassName>(decoded.as_object()));                                  \
        EXPECT_EQ(base.byte_offset(), 0u);                                               \
        EXPECT_EQ(base.byte_length().length(), element_size * 2u);                       \
        EXPECT(!base.array_length().is_auto());                                          \
        EXPECT_EQ(base.array_length().length(), 2u);                                     \
        auto sample = MUST(decoded.as_object().get(0u));                                 \
        if (#ClassName##sv == "BigInt64Array"sv || #ClassName##sv == "BigUint64Array"sv) \
            EXPECT(sample.is_bigint());                                                  \
        else                                                                             \
            EXPECT(sample.is_number());                                                  \
    }
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

    {
        Vector<u8> backing;
        backing.resize(8);
        Vector<u8> value;
        append_array_buffer_view_value(value, "DataView"sv, BackingBuffer::Fixed, backing, 8u, 0u);
        auto& view = as<JS::DataView>(MUST(storage_deserialize(storage_record_with_value(value))).as_object());
        EXPECT_EQ(view.byte_offset(), 0u);
        EXPECT_EQ(view.byte_length().length(), 8u);
    }
}

TEST_CASE(every_native_error_name_decodes_from_frozen_bytes)
{
    auto decode_error = [](StringView type_name) {
        Vector<u8> value { to_underlying(ValueTag::ErrorObject) };
        append_storage_string(value, type_name);
        value.append(0); // Optional message: absent
        value.append(0); // Optional cause: absent
        return MUST(storage_deserialize(storage_record_with_value(value)));
    };

    EXPECT(is<JS::Error>(decode_error("Error"sv).as_object()));
    // Enumerate the same native-error list error_type_to_name() accepts.
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    EXPECT(is<JS::ClassName>(decode_error(#ClassName##sv).as_object()));
    JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE
}

TEST_CASE(file_name_non_ascii_text_field_round_trips)
{
    Vector<u8> body;
    append_storage_string(body, "text/plain"sv);  // type
    append_storage_bytes(body, "data"sv.bytes()); // ByteBuffer body
    append_storage_string(body, "café.txt"sv);    // non-ASCII name via the folded little-endian-UTF-16 path
    append_storage_signed_leb128(body, 0);        // last modified

    auto& file = as<Web::FileAPI::File>(MUST(storage_deserialize(serializable_storage_record("File"sv, 1, body))).as_object());
    EXPECT_EQ(file.name(), "café.txt"_utf16);
}

TEST_CASE(file_name_with_lone_surrogate_round_trips)
{
    Vector<u8> body;
    append_storage_string(body, "text/plain"sv);
    append_storage_bytes(body, "data"sv.bytes());
    // A lone high surrogate written as a raw little-endian code unit.
    Array<u16, 4> name_units { 'f', 'i', 'l', 0xD800 };
    append_storage_utf16(body, name_units);
    append_storage_signed_leb128(body, 0);

    auto& file = as<Web::FileAPI::File>(MUST(storage_deserialize(serializable_storage_record("File"sv, 1, body))).as_object());
    Array<char16_t, 4> expected_name_units { 'f', 'i', 'l', 0xD800 };
    EXPECT_EQ(file.name(), Utf16String::from_utf16({ expected_name_units.data(), expected_name_units.size() }));
}

TEST_CASE(serializable_registry_completeness)
{
    // Every registry entry must have a decode golden.
    Array covered {
        "Blob"sv, "File"sv, "FileList"sv, "DOMException"sv, "QuotaExceededError"sv,
        "DOMMatrix"sv, "DOMMatrixReadOnly"sv, "DOMPoint"sv, "DOMPointReadOnly"sv,
        "DOMRect"sv, "DOMRectReadOnly"sv, "DOMQuad"sv, "ImageData"sv, "ImageBitmap"sv, "CryptoKey"sv
    };
    auto is_covered = [&](StringView identifier) {
        for (auto candidate : covered) {
            if (candidate == identifier)
                return true;
        }
        return false;
    };

    EXPECT_EQ(Web::HTML::serializable_storage_registry().size(), covered.size());
    for (auto const& entry : Web::HTML::serializable_storage_registry())
        EXPECT(is_covered(entry.identifier));
}
