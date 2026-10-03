/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StructuredSerializeTestHelpers.h"
#include <AK/NumericLimits.h>
#include <AK/ScopeGuard.h>
#include <LibCore/System.h>
#include <LibIPC/Limits.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Serializable.h>
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
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/WebAssembly/Module.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

#if !defined(AK_OS_WINDOWS)
#    include <fcntl.h>
#    include <sys/resource.h>
#    include <unistd.h>
#endif

static_assert(!IsSame<Web::HTML::IPCSerializationRecord, Web::HTML::StorageSerializationRecord>);
static_assert(IsSame<decltype(Web::HTML::IPCSerializationRecord {}.data), IPC::MessageDataType>);
static_assert(IsSame<decltype(Web::HTML::StorageSerializationRecord {}.data), ByteBuffer>);

// ValueTag values are the on-disk wire format; pin them so a renumber is caught as a deliberate format break.
static_assert(to_underlying(ValueTag::Empty) == 0);
static_assert(to_underlying(ValueTag::UndefinedPrimitive) == 1);
static_assert(to_underlying(ValueTag::NullPrimitive) == 2);
static_assert(to_underlying(ValueTag::BooleanPrimitive) == 3);
static_assert(to_underlying(ValueTag::NumberPrimitive) == 4);
static_assert(to_underlying(ValueTag::StringPrimitive) == 5);
static_assert(to_underlying(ValueTag::BigIntPrimitive) == 6);
static_assert(to_underlying(ValueTag::BooleanObject) == 7);
static_assert(to_underlying(ValueTag::NumberObject) == 8);
static_assert(to_underlying(ValueTag::StringObject) == 9);
static_assert(to_underlying(ValueTag::BigIntObject) == 10);
static_assert(to_underlying(ValueTag::DateObject) == 11);
static_assert(to_underlying(ValueTag::RegExpObject) == 12);
static_assert(to_underlying(ValueTag::MapObject) == 13);
static_assert(to_underlying(ValueTag::SetObject) == 14);
static_assert(to_underlying(ValueTag::ArrayObject) == 15);
static_assert(to_underlying(ValueTag::ErrorObject) == 16);
static_assert(to_underlying(ValueTag::Object) == 17);
static_assert(to_underlying(ValueTag::ObjectReference) == 18);
static_assert(to_underlying(ValueTag::GrowableSharedArrayBuffer) == 19);
static_assert(to_underlying(ValueTag::SharedArrayBuffer) == 20);
static_assert(to_underlying(ValueTag::ResizeableArrayBuffer) == 21);
static_assert(to_underlying(ValueTag::ArrayBuffer) == 22);
static_assert(to_underlying(ValueTag::ArrayBufferView) == 23);
static_assert(to_underlying(ValueTag::SerializableObject) == 24);
static_assert(to_underlying(ValueTag::Int32Primitive) == 25);
static_assert(to_underlying(ValueTag::EndObject) == 0xFF);

template<typename Record>
concept CanAppendToTransferDataEncoder = requires(Web::HTML::TransferDataEncoder encoder, Record record) {
    encoder.append(move(record));
};

static_assert(CanAppendToTransferDataEncoder<Web::HTML::IPCSerializationRecord>);
static_assert(!CanAppendToTransferDataEncoder<Web::HTML::StorageSerializationRecord>);

TEST_CASE(storage_writer_uses_stable_header_and_leb128_encoding)
{
    auto writer = Web::HTML::StructuredSerializeWriter::create_storage();
    EXPECT_EQ(writer.type(), Web::HTML::SerializationType::Storage);

    writer.encode(true);
    writer.encode(static_cast<u32>(0x01020304));
    writer.encode(static_cast<i64>(-2));
    writer.encode(1.0);

    auto record = writer.take_storage_record();

    Array<u8, 20> expected {
        'L',
        'B',
        'S',
        'C',
        0x01, // version (varint)
        0x00, // flags (varint)
        0x01, // true
        0x84, // u32 0x01020304 as LEB128
        0x86,
        0x88,
        0x08,
        0x7e, // i64 -2 as signed LEB128
        0x00, // double 1.0, fixed little-endian
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0xf0,
        0x3f,
    };
    EXPECT_EQ(record.data.span(), expected);

    Web::HTML::StructuredSerializeReader reader { record };
    EXPECT_EQ(reader.type(), Web::HTML::SerializationType::Storage);
    EXPECT_EQ(MUST(reader.decode<bool>()), true);
    EXPECT_EQ(MUST(reader.decode<u32>()), 0x01020304u);
    EXPECT_EQ(MUST(reader.decode<i64>()), -2);
    EXPECT_EQ(MUST(reader.decode<double>()), 1.0);
}

TEST_CASE(storage_writer_stores_int32_range_numbers_as_a_signed_varint_tag)
{
    auto& vm = test_realm().vm();

    // Strip the 6-byte "LBSC" + varint version + varint flags header, leaving [ValueTag][body].
    auto value_bytes = [&](JS::Value value) {
        auto record = MUST(Web::HTML::structured_serialize_for_storage(vm, value));
        return MUST(ByteBuffer::copy(record.data.bytes().slice(6)));
    };

    {
        Array<u8, 2> expected { to_underlying(ValueTag::Int32Primitive), 0x01 };
        EXPECT_EQ(value_bytes(JS::Value(1.0)).span(), expected);
    }
    {
        Array<u8, 2> expected { to_underlying(ValueTag::Int32Primitive), 0x7e };
        EXPECT_EQ(value_bytes(JS::Value(-2.0)).span(), expected);
    }

    // The int32 extremes keep the compact tag (the varint widens; the tag does not).
    EXPECT_EQ(value_bytes(JS::Value(static_cast<double>(NumericLimits<i32>::max())))[0], to_underlying(ValueTag::Int32Primitive));
    EXPECT_EQ(value_bytes(JS::Value(static_cast<double>(NumericLimits<i32>::min())))[0], to_underlying(ValueTag::Int32Primitive));

    EXPECT_EQ(value_bytes(JS::Value(NumericLimits<i32>::max() + 1.0))[0], to_underlying(ValueTag::NumberPrimitive));
    EXPECT_EQ(value_bytes(JS::Value(NumericLimits<i32>::min() - 1.0))[0], to_underlying(ValueTag::NumberPrimitive));
    EXPECT_EQ(value_bytes(JS::Value(13.5))[0], to_underlying(ValueTag::NumberPrimitive));

    // Negative zero stays a double: an int32 cannot carry its sign.
    EXPECT_EQ(value_bytes(JS::Value(-0.0))[0], to_underlying(ValueTag::NumberPrimitive));
}

TEST_CASE(storage_writer_encodes_enum_like_values_as_stable_identifiers)
{
    expect_storage_identifier_encoding(Web::Bindings::KeyType::Public, "public"sv);
    expect_storage_identifier_encoding(Web::Bindings::KeyType::Private, "private"sv);
    expect_storage_identifier_encoding(Web::Bindings::KeyType::Secret, "secret"sv);

    expect_storage_identifier_encoding(Web::Bindings::KeyUsage::Encrypt, "encrypt"sv);
    expect_storage_identifier_encoding(Web::Bindings::KeyUsage::Decrypt, "decrypt"sv);
    expect_storage_identifier_encoding(Web::Bindings::KeyUsage::Sign, "sign"sv);
    expect_storage_identifier_encoding(Web::Bindings::KeyUsage::Verify, "verify"sv);
    expect_storage_identifier_encoding(Web::Bindings::KeyUsage::DeriveKey, "deriveKey"sv);
    expect_storage_identifier_encoding(Web::Bindings::KeyUsage::DeriveBits, "deriveBits"sv);
    expect_storage_identifier_encoding(Web::Bindings::KeyUsage::WrapKey, "wrapKey"sv);
    expect_storage_identifier_encoding(Web::Bindings::KeyUsage::UnwrapKey, "unwrapKey"sv);
    expect_storage_identifier_encoding(Web::Bindings::KeyUsage::EncapsulateKey, "encapsulateKey"sv);
    expect_storage_identifier_encoding(Web::Bindings::KeyUsage::EncapsulateBits, "encapsulateBits"sv);
    expect_storage_identifier_encoding(Web::Bindings::KeyUsage::DecapsulateKey, "decapsulateKey"sv);
    expect_storage_identifier_encoding(Web::Bindings::KeyUsage::DecapsulateBits, "decapsulateBits"sv);

    expect_storage_identifier_encoding(Web::Bindings::PredefinedColorSpace::Srgb, "srgb"sv);
    expect_storage_identifier_encoding(Web::Bindings::PredefinedColorSpace::SrgbLinear, "srgb-linear"sv);
    expect_storage_identifier_encoding(Web::Bindings::PredefinedColorSpace::DisplayP3, "display-p3"sv);
    expect_storage_identifier_encoding(Web::Bindings::PredefinedColorSpace::DisplayP3Linear, "display-p3-linear"sv);

    expect_storage_identifier_encoding(Gfx::BitmapFormat::BGRx8888, "BGRx8888"sv);
    expect_storage_identifier_encoding(Gfx::BitmapFormat::BGRA8888, "BGRA8888"sv);
    expect_storage_identifier_encoding(Gfx::BitmapFormat::RGBx8888, "RGBx8888"sv);
    expect_storage_identifier_encoding(Gfx::BitmapFormat::RGBA8888, "RGBA8888"sv);

    expect_storage_identifier_encoding(Gfx::AlphaType::Premultiplied, "premultiplied"sv);
    expect_storage_identifier_encoding(Gfx::AlphaType::Unpremultiplied, "unpremultiplied"sv);

    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::Blob, "Blob"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::File, "File"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::FileList, "FileList"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::DOMException, "DOMException"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::DOMMatrixReadOnly, "DOMMatrixReadOnly"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::DOMMatrix, "DOMMatrix"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::DOMPointReadOnly, "DOMPointReadOnly"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::DOMPoint, "DOMPoint"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::DOMRectReadOnly, "DOMRectReadOnly"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::DOMRect, "DOMRect"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::CryptoKey, "CryptoKey"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::DOMQuad, "DOMQuad"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::ImageData, "ImageData"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::ImageBitmap, "ImageBitmap"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::Module, "Module"sv);
    expect_storage_identifier_encoding(Web::Bindings::InterfaceName::QuotaExceededError, "QuotaExceededError"sv);
}

TEST_CASE(all_public_writer_reader_types_round_trip_through_ipc_and_storage)
{
    expect_structured_round_trip(false);
    expect_structured_round_trip(true);
    expect_structured_round_trip(-12345);
    expect_structured_round_trip(static_cast<u32>(0xfedcba98));
    expect_structured_round_trip(static_cast<u64>(0x0102030405060708));
    expect_structured_round_trip(static_cast<i64>(-0x0102030405060708));
    expect_structured_round_trip(13.5);

    auto utf16 = Utf16String::from_utf8("stored UTF-16 string"sv);
    expect_utf16_equals(structured_ipc_round_trip(utf16), utf16);
    expect_utf16_equals(structured_storage_round_trip(utf16), utf16);

    // Non-ASCII forces 16-bit string storage rather than the ASCII fast path.
    auto non_ascii = Utf16String::from_utf8("héllo 🦬"sv);
    expect_utf16_equals(structured_ipc_round_trip(non_ascii), non_ascii);
    expect_utf16_equals(structured_storage_round_trip(non_ascii), non_ascii);

    expect_structured_round_trip(Web::Bindings::KeyType::Public);
    expect_structured_round_trip(Web::Bindings::KeyType::Private);
    expect_structured_round_trip(Web::Bindings::KeyType::Secret);

    expect_structured_round_trip(Web::Bindings::KeyUsage::Encrypt);
    expect_structured_round_trip(Web::Bindings::KeyUsage::Decrypt);
    expect_structured_round_trip(Web::Bindings::KeyUsage::Sign);
    expect_structured_round_trip(Web::Bindings::KeyUsage::Verify);
    expect_structured_round_trip(Web::Bindings::KeyUsage::DeriveKey);
    expect_structured_round_trip(Web::Bindings::KeyUsage::DeriveBits);
    expect_structured_round_trip(Web::Bindings::KeyUsage::WrapKey);
    expect_structured_round_trip(Web::Bindings::KeyUsage::UnwrapKey);
    expect_structured_round_trip(Web::Bindings::KeyUsage::EncapsulateKey);
    expect_structured_round_trip(Web::Bindings::KeyUsage::EncapsulateBits);
    expect_structured_round_trip(Web::Bindings::KeyUsage::DecapsulateKey);
    expect_structured_round_trip(Web::Bindings::KeyUsage::DecapsulateBits);

    expect_structured_round_trip(Web::Bindings::PredefinedColorSpace::Srgb);
    expect_structured_round_trip(Web::Bindings::PredefinedColorSpace::SrgbLinear);
    expect_structured_round_trip(Web::Bindings::PredefinedColorSpace::DisplayP3);
    expect_structured_round_trip(Web::Bindings::PredefinedColorSpace::DisplayP3Linear);

    expect_structured_round_trip(Gfx::BitmapFormat::BGRx8888);
    expect_structured_round_trip(Gfx::BitmapFormat::BGRA8888);
    expect_structured_round_trip(Gfx::BitmapFormat::RGBx8888);
    expect_structured_round_trip(Gfx::BitmapFormat::RGBA8888);

    expect_structured_round_trip(Gfx::AlphaType::Premultiplied);
    expect_structured_round_trip(Gfx::AlphaType::Unpremultiplied);

    Optional<double> no_double;
    expect_structured_round_trip(no_double);
    expect_structured_round_trip(Optional<double> { -0.25 });

    Optional<Utf16String> no_utf16;
    expect_optional_utf16_equals(structured_ipc_round_trip(no_utf16), no_utf16);
    expect_optional_utf16_equals(structured_storage_round_trip(no_utf16), no_utf16);

    auto optional_utf16 = Optional<Utf16String> { utf16 };
    expect_optional_utf16_equals(structured_ipc_round_trip(optional_utf16), optional_utf16);
    expect_optional_utf16_equals(structured_storage_round_trip(optional_utf16), optional_utf16);

    expect_structured_round_trip(Web::Bindings::InterfaceName::Blob);
    expect_structured_round_trip(Web::Bindings::InterfaceName::File);
    expect_structured_round_trip(Web::Bindings::InterfaceName::FileList);
    expect_structured_round_trip(Web::Bindings::InterfaceName::DOMException);
    expect_structured_round_trip(Web::Bindings::InterfaceName::DOMMatrixReadOnly);
    expect_structured_round_trip(Web::Bindings::InterfaceName::DOMMatrix);
    expect_structured_round_trip(Web::Bindings::InterfaceName::DOMPointReadOnly);
    expect_structured_round_trip(Web::Bindings::InterfaceName::DOMPoint);
    expect_structured_round_trip(Web::Bindings::InterfaceName::DOMRectReadOnly);
    expect_structured_round_trip(Web::Bindings::InterfaceName::DOMRect);
    expect_structured_round_trip(Web::Bindings::InterfaceName::CryptoKey);
    expect_structured_round_trip(Web::Bindings::InterfaceName::DOMQuad);
    expect_structured_round_trip(Web::Bindings::InterfaceName::ImageData);
    expect_structured_round_trip(Web::Bindings::InterfaceName::ImageBitmap);
    expect_structured_round_trip(Web::Bindings::InterfaceName::Module);
    expect_structured_round_trip(Web::Bindings::InterfaceName::QuotaExceededError);

    Array<u8, 4> raw_bytes { 0xde, 0xad, 0xbe, 0xef };
    ReadonlyBytes readonly_bytes { raw_bytes.data(), raw_bytes.size() };
    EXPECT_EQ(structured_ipc_buffer_round_trip(readonly_bytes).span(), raw_bytes);
    EXPECT_EQ(structured_storage_buffer_round_trip(readonly_bytes).span(), raw_bytes);

    auto byte_buffer = MUST(ByteBuffer::copy(raw_bytes.span()));
    EXPECT_EQ(structured_ipc_buffer_round_trip(byte_buffer).span(), raw_bytes);
    EXPECT_EQ(structured_storage_buffer_round_trip(byte_buffer).span(), raw_bytes);
}

TEST_CASE(storage_writer_encodes_js_strings_as_utf16_code_units)
{
    Array<char16_t, 3> code_units {
        u'A',
        static_cast<char16_t>(0xd800),
        u'B',
    };
    auto value = Utf16String::from_utf16({ code_units.data(), code_units.size() });

    auto writer = Web::HTML::StructuredSerializeWriter::create_storage();
    writer.encode(value);

    auto record = writer.take_storage_record();

    Array<u8, 13> expected {
        'L',
        'B',
        'S',
        'C',
        0x01, // version
        0x00, // flags
        0x06, // string header: (3 << 1) | utf16
        0x41,
        0x00, // 'A'
        0x00,
        0xd8, // U+D800
        0x42,
        0x00, // 'B'
    };
    EXPECT_EQ(record.data.span(), expected);

    Web::HTML::StructuredSerializeReader reader { record };
    auto decoded = MUST(reader.decode<Utf16String>());
    auto view = decoded.utf16_view();
    EXPECT_EQ(view.length_in_code_units(), 3u);
    EXPECT_EQ(view.code_unit_at(0), u'A');
    EXPECT_EQ(view.code_unit_at(1), static_cast<char16_t>(0xd800));
    EXPECT_EQ(view.code_unit_at(2), u'B');
}

TEST_CASE(storage_writer_encodes_ascii_strings_as_one_byte_per_code_unit)
{
    auto value = Utf16String::from_utf8("AB"sv);
    EXPECT(value.utf16_view().has_ascii_storage());

    auto writer = Web::HTML::StructuredSerializeWriter::create_storage();
    writer.encode(value);

    auto record = writer.take_storage_record();

    Array<u8, 9> expected {
        'L',
        'B',
        'S',
        'C',
        0x01, // version
        0x00, // flags
        0x05, // string header: (2 << 1) | ascii
        'A',
        'B',
    };
    EXPECT_EQ(record.data.span(), expected);

    Web::HTML::StructuredSerializeReader reader { record };
    auto decoded = MUST(reader.decode<Utf16String>());
    EXPECT_EQ(decoded, value);
    EXPECT(decoded.utf16_view().has_ascii_storage());
}

TEST_CASE(storage_writer_encodes_the_string_length_as_a_leb128_varint)
{
    // Pin both sides of the folded-header LEB128 width boundary.
    {
        auto value = Utf16String::repeated('a', 63);
        auto writer = Web::HTML::StructuredSerializeWriter::create_storage();
        writer.encode(value);
        auto record = writer.take_storage_record();

        Array<u8, 1> expected_framing { 0x7F };
        EXPECT_EQ(record.data.span().slice(6, expected_framing.size()), expected_framing);

        Web::HTML::StructuredSerializeReader reader { record };
        EXPECT_EQ(MUST(reader.decode<Utf16String>()), value);
    }

    {
        auto value = Utf16String::repeated('a', 64);
        auto writer = Web::HTML::StructuredSerializeWriter::create_storage();
        writer.encode(value);
        auto record = writer.take_storage_record();

        Array<u8, 2> expected_framing { 0x81, 0x01 };
        EXPECT_EQ(record.data.span().slice(6, expected_framing.size()), expected_framing);

        Web::HTML::StructuredSerializeReader reader { record };
        EXPECT_EQ(MUST(reader.decode<Utf16String>()), value);
    }
}

TEST_CASE(empty_strings_round_trip)
{
    auto empty_utf16 = Utf16String {};
    expect_utf16_equals(structured_ipc_round_trip(empty_utf16), empty_utf16);
    expect_utf16_equals(structured_storage_round_trip(empty_utf16), empty_utf16);
}

TEST_CASE(record_wrappers_round_trip_through_ipc)
{
    Array<u8, 12> payload {
        'L',
        'B',
        'S',
        'C',
        0x01,
        0x00,
        0x00,
        0x00,
        0xde,
        0xad,
        0xbe,
        0xef,
    };

    Web::HTML::StorageSerializationRecord storage_record { MUST(ByteBuffer::copy(payload.span())) };
    auto decoded_storage_record = ipc_round_trip(storage_record);
    EXPECT_EQ(decoded_storage_record.data.span(), payload);

    IPC::MessageDataType ipc_payload;
    ipc_payload.append(payload.data(), payload.size());

    Web::HTML::IPCSerializationRecord ipc_record { move(ipc_payload) };
    auto decoded_ipc_record = ipc_round_trip(ipc_record);
    EXPECT_EQ(decoded_ipc_record.data.span(), payload);
}

TEST_CASE(storage_serializable_object_is_framed_with_interface_then_version)
{
    (void)test_realm();
    auto point = Web::Geometry::DOMPoint::create();
    point->set_x(1.0);
    point->set_y(2.0);
    point->set_z(3.0);
    point->set_w(4.0);

    auto record = storage_serialize(point);

    Vector<u8> expected;
    expected.append('L');
    expected.append('B');
    expected.append('S');
    expected.append('C');
    append_storage_leb128(expected, 1); // storage format version
    append_storage_leb128(expected, 0); // storage format flags
    expected.append(to_underlying(ValueTag::SerializableObject));
    append_storage_ascii_utf16(expected, "DOMPoint"sv);
    append_storage_leb128(expected, 1); // per-type payload version (storage only)
    append_little_endian_double(expected, 1.0);
    append_little_endian_double(expected, 2.0);
    append_little_endian_double(expected, 3.0);
    append_little_endian_double(expected, 4.0);

    EXPECT_EQ(record.data.span(), expected.span());
}

TEST_CASE(ipc_serializable_object_has_no_version_framing)
{
    auto point = Web::Geometry::DOMPoint::create();
    point->set_x(1.0);
    point->set_y(2.0);
    point->set_z(3.0);
    point->set_w(4.0);

    auto record = structured_serialize(test_realm().vm(), point);
    Web::HTML::StructuredSerializeReader reader { record };

    EXPECT_EQ(MUST(reader.decode<u8>()), to_underlying(ValueTag::SerializableObject));
    EXPECT_EQ(MUST(reader.decode<Web::Bindings::InterfaceName>()), Web::Bindings::InterfaceName::DOMPoint);
    EXPECT_EQ(MUST(reader.decode<double>()), 1.0);
    EXPECT_EQ(MUST(reader.decode<double>()), 2.0);
    EXPECT_EQ(MUST(reader.decode<double>()), 3.0);
    EXPECT_EQ(MUST(reader.decode<double>()), 4.0);
}

TEST_CASE(webassembly_module_encodes_to_frozen_ipc_bytes)
{
    auto value = MUST(principal_storage_deserialize(webassembly_module_storage_record(s_minimal_webassembly_module_bytes.span())));
    auto module = GC::Ref { unwrap_wrappable<Web::WebAssembly::Module>(value) };

    auto record = structured_serialize(test_realm().vm(), module);
    Web::HTML::StructuredSerializeReader reader { record };

    EXPECT_EQ(MUST(reader.decode<u8>()), to_underlying(ValueTag::SerializableObject));
    EXPECT_EQ(MUST(reader.decode<Web::Bindings::InterfaceName>()), Web::Bindings::InterfaceName::Module);
    EXPECT_EQ(MUST(reader.decode<ByteBuffer>()).span(), s_minimal_webassembly_module_bytes.span());
}

TEST_CASE(realm_helpers_share_vm_initialization)
{
    auto& vm = test_realm().vm();
    auto depth = vm.execution_context_stack().size();

    auto principal = Web::Bindings::create_a_principal_javascript_realm();
    EXPECT_EQ(&principal->vm(), &vm);

    auto simple = Web::Bindings::create_a_simple_javascript_realm();
    EXPECT_EQ(&simple->vm(), &vm);

    while (vm.execution_context_stack().size() > depth)
        vm.pop_execution_context();
}

TEST_CASE(duplicate_typed_array_reference_resolves_back_to_the_view)
{
    // A held view must resolve back to the view, not its earlier-serialized buffer.
    auto& realm = test_realm();

    auto view = MUST(JS::Uint8Array::create(realm, 4));
    JS::Value view_value { view };
    JS::Value elements[] { view_value, view_value };
    auto array = JS::Array::create_from(realm, elements);

    auto decoded = MUST(storage_deserialize(storage_serialize(array)));
    EXPECT(decoded.is_object());
    auto& decoded_array = as<JS::Array>(decoded.as_object());

    auto first = MUST(decoded_array.get(0u));
    auto second = MUST(decoded_array.get(1u));
    EXPECT(first.is_object());
    EXPECT(second.is_object());

    EXPECT(is<JS::Uint8Array>(first.as_object()));
    EXPECT(is<JS::Uint8Array>(second.as_object()));
    EXPECT_EQ(&first.as_object(), &second.as_object());
}

// Changing a storage golden is a wire-layout change.
static_assert(Web::HTML::storage_format_version == 1, "Storage goldens are pinned to format version 1; a golden change is a wire change — regenerate (pre-release) or bump the version and migrate (post-release).");

template<typename T>
static void expect_storage_encoding(GC::Ref<T> object, Web::HTML::StorageSerializationRecord const& expected)
{
    auto record = storage_serialize(object);
    EXPECT_EQ(record.data.span(), expected.data.span());
}

TEST_CASE(geometry_serializables_encode_to_frozen_bytes)
{
    (void)test_realm();
    {
        auto point = Web::Geometry::DOMPoint::create();
        point->set_x(1.5);
        point->set_y(-2.5);
        point->set_z(3.5);
        point->set_w(-4.5);
        Vector<u8> body;
        for (double element : { 1.5, -2.5, 3.5, -4.5 })
            append_little_endian_double(body, element);
        expect_storage_encoding(point, serializable_storage_record("DOMPoint"sv, 1, body));

        auto read_only = Web::Geometry::DOMPointReadOnly::create(1.5, -2.5, 3.5, -4.5);
        expect_storage_encoding(read_only, serializable_storage_record("DOMPointReadOnly"sv, 1, body));
    }

    {
        auto rect = Web::Geometry::DOMRect::create();
        rect->set_x(10.0);
        rect->set_y(20.0);
        rect->set_width(30.0);
        rect->set_height(40.0);
        Vector<u8> body;
        for (double element : { 10.0, 20.0, 30.0, 40.0 })
            append_little_endian_double(body, element);
        expect_storage_encoding(rect, serializable_storage_record("DOMRect"sv, 1, body));

        auto read_only = Web::Geometry::DOMRectReadOnly::create(10.0, 20.0, 30.0, 40.0);
        expect_storage_encoding(read_only, serializable_storage_record("DOMRectReadOnly"sv, 1, body));
    }

    {
        // Identity 2D matrices serialize six components.
        auto matrix = Web::Geometry::DOMMatrixReadOnly::create();
        Vector<u8> body;
        body.append(1);
        for (double element : { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 })
            append_little_endian_double(body, element);
        expect_storage_encoding(matrix, serializable_storage_record("DOMMatrixReadOnly"sv, 1, body));

        auto mutable_matrix = Web::Geometry::DOMMatrix::create();
        expect_storage_encoding(mutable_matrix, serializable_storage_record("DOMMatrix"sv, 1, body));
    }

    {
        // DOMQuad serializes its four corner points as nested DOMPoint sub-values.
        auto quad = Web::Geometry::DOMQuad::create();
        Vector<u8> body;
        for (auto point : Array { quad->p1(), quad->p2(), quad->p3(), quad->p4() }) {
            Vector<u8> point_body;
            append_little_endian_double(point_body, point->x());
            append_little_endian_double(point_body, point->y());
            append_little_endian_double(point_body, point->z());
            append_little_endian_double(point_body, point->w());
            frozen_serializable_value(body, "DOMPoint"sv, 1, point_body);
        }
        expect_storage_encoding(quad, serializable_storage_record("DOMQuad"sv, 1, body));
    }
}

TEST_CASE(image_serializables_encode_to_frozen_bytes)
{
    auto& realm = test_realm();

    {
        auto pixels = MUST(JS::Uint8ClampedArray::create(realm, 4));
        auto image = MUST(Web::HTML::ImageData::create(realm, pixels, 1));

        // Build the data sub-value from the live typed array.
        auto data_body = serialized_object_body(pixels);
        Vector<u8> body;
        body.append(data_body.data(), data_body.size());
        append_storage_signed_leb128(body, 1); // width
        append_storage_signed_leb128(body, 1); // height
        append_storage_string(body, "srgb"sv);
        expect_storage_encoding(image, serializable_storage_record("ImageData"sv, 1, body));
    }

    {
        auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 2, 2 }));
        auto image_bitmap = Web::HTML::ImageBitmap::create();
        image_bitmap->set_bitmap(bitmap);

        // Use the bitmap's actual pitch and data size.
        ReadonlyBytes data { bitmap->scanline_u8(0), bitmap->data_size() };
        auto expected = image_bitmap_record(bitmap->width(), bitmap->height(), bitmap->pitch(), "BGRA8888"sv, "premultiplied"sv, data);
        expect_storage_encoding(image_bitmap, expected);
    }
}

static GC::Ref<JS::Object> wrap_image_bitmap(JS::Realm& realm, GC::Ref<Web::HTML::ImageBitmap> image_bitmap)
{
    return Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, image_bitmap);
}

static GC::Ref<Web::HTML::ImageBitmap> create_image_bitmap(NonnullRefPtr<Gfx::Bitmap> bitmap)
{
    auto image_bitmap = Web::HTML::ImageBitmap::create();
    image_bitmap->set_bitmap(move(bitmap));
    return image_bitmap;
}

static Vector<JS::Value> deserialize_transfer_data_holders(JS::Realm& realm, Web::HTML::SerializedTransferRecord& record)
{
    Vector<JS::Value> values;
    for (auto& holder : record.transfer_data_holders) {
        Web::HTML::TransferDataDecoder decoder { move(holder) };
        decoder.set_shared_buffers(record.shared_buffers);
        values.append(MUST(Web::HTML::structured_deserialize_with_transfer_internal(decoder, realm)));
    }
    return values;
}

struct TransferredThroughIPC {
    size_t payload_size { 0 };
    size_t attachment_count { 0 };
    GC::RootVector<JS::Value> transferred_values;
};

// Sends a transfer record through IPC encoding the way postMessage() does, and deserializes what arrives.
static TransferredThroughIPC transfer_through_ipc(JS::Realm& realm, JS::Value value, ReadonlySpan<GC::Ref<JS::Object>> transfer)
{
    auto record = MUST(Web::HTML::structured_serialize_with_transfer(realm, value, transfer));
    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode(record));

    TransferredThroughIPC result;
    result.payload_size = message.data().size();
    result.attachment_count = message.attachments().size();

    FixedMemoryStream stream { message.data().span() };
    Queue<IPC::Attachment> attachments;
    for (auto& attachment : message.take_attachments())
        attachments.enqueue(move(attachment));
    IPC::Decoder decoder { stream, attachments };
    auto received_record = MUST(decoder.decode<Web::HTML::SerializedTransferRecord>());
    auto deserialized = MUST(Web::HTML::structured_deserialize_with_transfer(received_record, realm));
    // The record can outlive its delivery, so it must not keep shared memory alive for the values.
    EXPECT(received_record.shared_buffers.is_empty());
    for (auto& value : deserialized.transferred_values)
        result.transferred_values.append(value.ptr());
    return result;
}

TEST_CASE(image_bitmap_transfer_keeps_pixels_out_of_the_message_payload)
{
    auto& realm = test_realm();
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Unpremultiplied, { 1024, 768 }));
    bitmap->set_pixel(0, 0, Gfx::Color::Red);
    bitmap->set_pixel(1023, 767, Gfx::Color::Blue);
    auto source = create_image_bitmap(bitmap);
    auto wrapper = wrap_image_bitmap(realm, source);
    Array<GC::Ref<JS::Object>, 1> transfer { wrapper };

    auto result = transfer_through_ipc(realm, wrapper, transfer);
    EXPECT(source->is_detached());
    EXPECT_EQ(source->bitmap(), nullptr);
    EXPECT(result.payload_size < 4096);
    EXPECT_EQ(result.attachment_count, 1u);

    auto& received = unwrap_wrappable<Web::HTML::ImageBitmap>(result.transferred_values[0]);
    // A bitmap that has the shared memory to itself is backed by it, so that passing it on does not copy it.
    EXPECT(received.bitmap()->anonymous_buffer().is_valid());
    EXPECT_EQ(received.width(), 1024u);
    EXPECT_EQ(received.height(), 768u);
    EXPECT_EQ(received.bitmap()->format(), Gfx::BitmapFormat::RGBA8888);
    EXPECT_EQ(received.bitmap()->alpha_type(), Gfx::AlphaType::Unpremultiplied);
    EXPECT_EQ(received.bitmap()->get_pixel(0, 0), Gfx::Color::Red);
    EXPECT_EQ(received.bitmap()->get_pixel(1023, 767), Gfx::Color::Blue);
    received.close();
    EXPECT_EQ(received.bitmap(), nullptr);
}

TEST_CASE(any_number_of_transferred_image_bitmaps_share_one_attachment)
{
    auto& realm = test_realm();
    static constexpr size_t count = IPC::MAX_MESSAGE_FD_COUNT + 1;
    GC::RootVector<GC::Ref<JS::Object>> transfer;
    GC::RootVector<JS::Value> values;
    for (size_t i = 0; i < count; ++i) {
        auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 1, 1 }));
        bitmap->set_pixel(0, 0, Gfx::Color(i % 256, i / 256, 0));
        auto wrapper = wrap_image_bitmap(realm, create_image_bitmap(bitmap));
        transfer.append(wrapper);
        values.append(wrapper);
    }
    auto buffer = MUST(JS::ArrayBuffer::create(realm, 1));
    auto port = Web::HTML::MessagePort::create(Web::DOM::EventTarget::create());
    transfer.append(buffer);
    transfer.append(Web::Bindings::message_port(realm, port));

#if !defined(AK_OS_WINDOWS)
    // Receiving the batch must not retain one descriptor per bitmap, even with a small descriptor limit.
    rlimit original_limit {};
    VERIFY(getrlimit(RLIMIT_NOFILE, &original_limit) == 0);
    auto limit = original_limit;
    limit.rlim_cur = min<rlim_t>(original_limit.rlim_cur, 256);
    VERIFY(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    ScopeGuard restore_limit = [&] { VERIFY(setrlimit(RLIMIT_NOFILE, &original_limit) == 0); };
#endif

    auto result = transfer_through_ipc(realm, JS::Array::create_from(realm, values), transfer);
    EXPECT_EQ(result.attachment_count, 1u);
    EXPECT_EQ(result.transferred_values.size(), count + 2);
    for (size_t i = 0; i < count; ++i) {
        auto& received = unwrap_wrappable<Web::HTML::ImageBitmap>(result.transferred_values[i]);
        EXPECT(!received.bitmap()->anonymous_buffer().is_valid());
        EXPECT_EQ(received.bitmap()->get_pixel(0, 0), Gfx::Color(i % 256, i / 256, 0));
        received.close();
    }
}

TEST_CASE(image_bitmap_with_padded_rows_transfers_its_pixels)
{
    auto& realm = test_realm();
    Array<u32, 6> pixels { 0xffff0000, 0xff00ff00, 0, 0xff0000ff, 0xffffffff, 0 };
    auto bitmap = MUST(Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 2, 2 }, 3 * sizeof(u32), pixels.data()));
    auto wrapper = wrap_image_bitmap(realm, create_image_bitmap(bitmap));
    Array<GC::Ref<JS::Object>, 1> transfer { wrapper };

    auto result = transfer_through_ipc(realm, wrapper, transfer);
    auto& received = unwrap_wrappable<Web::HTML::ImageBitmap>(result.transferred_values[0]);
    EXPECT_EQ(received.bitmap()->get_pixel(0, 0), Gfx::Color::Red);
    EXPECT_EQ(received.bitmap()->get_pixel(1, 0), Gfx::Color::Green);
    EXPECT_EQ(received.bitmap()->get_pixel(0, 1), Gfx::Color::Blue);
    EXPECT_EQ(received.bitmap()->get_pixel(1, 1), Gfx::Color::White);
    received.close();
}

TEST_CASE(image_bitmaps_queued_on_a_transferred_message_port_keep_their_pixels)
{
    auto& realm = test_realm();
    auto* window = Web::HTML::window_from_global_object(realm.global_object());
    VERIFY(window);
    auto port = Web::HTML::MessagePort::create(*window);
    static constexpr size_t count = IPC::MAX_MESSAGE_FD_COUNT + 1;
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 1, 1 }));
    bitmap->set_pixel(0, 0, Gfx::Color::Blue);
    GC::RootVector<GC::Ref<JS::Object>> transfer;
    GC::RootVector<JS::Value> values;
    for (size_t i = 0; i < count; ++i) {
        auto wrapper = wrap_image_bitmap(realm, create_image_bitmap(bitmap));
        transfer.append(wrapper);
        values.append(wrapper);
    }
    MUST(port->post_message(realm, JS::Array::create_from(realm, values), transfer));
    EXPECT_EQ(port->pending_outgoing_message_count(), 1u);

    auto port_wrapper = Web::Bindings::message_port(realm, port);
    Array<GC::Ref<JS::Object>, 1> port_transfer { port_wrapper };
    auto record = MUST(Web::HTML::structured_serialize_with_transfer(realm, port_wrapper, port_transfer));
    EXPECT(port->is_detached());
    EXPECT_EQ(record.transfer_data_holders[0].buffer().attachments().size(), 1u);

    Web::HTML::TransferDataDecoder port_decoder { move(record.transfer_data_holders[0]) };
    EXPECT_EQ(MUST(port_decoder.decode<Web::HTML::TransferType>()), Web::HTML::TransferType::MessagePort);
    EXPECT(MUST(port_decoder.decode<Vector<Web::HTML::SerializedTransferRecord>>()).is_empty());
    auto queued_messages = MUST(port_decoder.decode<Vector<Web::HTML::SerializedTransferRecord>>());
    EXPECT_EQ(queued_messages.size(), 1u);
    auto received_values = deserialize_transfer_data_holders(realm, queued_messages[0]);
    EXPECT_EQ(received_values.size(), count);
    for (auto value : received_values) {
        auto& received = unwrap_wrappable<Web::HTML::ImageBitmap>(value);
        EXPECT_EQ(received.bitmap()->get_pixel(0, 0), Gfx::Color::Blue);
        received.close();
    }
}

#if !defined(AK_OS_WINDOWS)
TEST_CASE(image_bitmap_transfer_inlines_pixels_without_shared_memory)
{
    auto& realm = test_realm();
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 1, 1 }));
    bitmap->set_pixel(0, 0, Gfx::Color::Green);
    auto wrapper = wrap_image_bitmap(realm, create_image_bitmap(bitmap));
    Array<GC::Ref<JS::Object>, 1> transfer { wrapper };

    // Use up every descriptor, so that no shared memory can be created.
    rlimit original_limit {};
    VERIFY(getrlimit(RLIMIT_NOFILE, &original_limit) == 0);
    auto limit = original_limit;
    limit.rlim_cur = min<rlim_t>(original_limit.rlim_cur, 32);
    VERIFY(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    Vector<int, 32> descriptors;
    auto restore_descriptors_and_limit = [&] {
        for (auto descriptor : descriptors)
            VERIFY(close(descriptor) == 0);
        descriptors.clear();
        VERIFY(setrlimit(RLIMIT_NOFILE, &original_limit) == 0);
    };
    ArmedScopeGuard restore_guard = restore_descriptors_and_limit;
    for (;;) {
        auto descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (descriptor < 0)
            break;
        descriptors.append(descriptor);
    }
    VERIFY(errno == EMFILE);
    auto record = Web::HTML::structured_serialize_with_transfer(realm, wrapper, transfer);
    restore_guard.disarm();
    restore_descriptors_and_limit();

    EXPECT(!record.is_error());
    auto& transfer_record = record.value();
    EXPECT(transfer_record.shared_buffers.is_empty());
    EXPECT(transfer_record.transfer_data_holders[0].buffer().attachments().is_empty());
    auto received_values = deserialize_transfer_data_holders(realm, transfer_record);
    auto& received = unwrap_wrappable<Web::HTML::ImageBitmap>(received_values[0]);
    EXPECT_EQ(received.bitmap()->get_pixel(0, 0), Gfx::Color::Green);
    received.close();
}
#endif

TEST_CASE(image_bitmap_transfer_rejects_invalid_shared_pixels)
{
    auto& realm = test_realm();
    Vector<Core::AnonymousBuffer> shared_buffers;
    shared_buffers.append(MUST(Core::AnonymousBuffer::create_with_size(64, Core::AnonymousBuffer::Sealability::Sealable)));
    auto receive = [&](Vector<Core::AnonymousBuffer> const* buffers, u32 buffer_index, u64 offset, Gfx::BitmapFormat format, int width) {
        Web::HTML::TransferDataEncoder holder;
        MUST(holder.encode(true));
        MUST(holder.encode(buffer_index));
        MUST(holder.encode(offset));
        MUST(holder.encode(width));
        MUST(holder.encode(1));
        MUST(holder.encode(format));
        MUST(holder.encode(Gfx::AlphaType::Premultiplied));
        Web::HTML::TransferDataDecoder decoder { move(holder) };
        if (buffers)
            decoder.set_shared_buffers(*buffers);
        auto receiver = Web::HTML::ImageBitmap::create();
        return receiver->transfer_receiving_steps(realm, decoder);
    };
    EXPECT(!receive(&shared_buffers, 0, 0, Gfx::BitmapFormat::BGRA8888, 16).is_error());
    EXPECT(receive(nullptr, 0, 0, Gfx::BitmapFormat::BGRA8888, 16).is_error());
    EXPECT(receive(&shared_buffers, 1, 0, Gfx::BitmapFormat::BGRA8888, 16).is_error());
    EXPECT(receive(&shared_buffers, 0, 4, Gfx::BitmapFormat::BGRA8888, 16).is_error());
    EXPECT(receive(&shared_buffers, 0, 1, Gfx::BitmapFormat::BGRA8888, 1).is_error());
    EXPECT(receive(&shared_buffers, 0, NumericLimits<u64>::max() - 1, Gfx::BitmapFormat::BGRA8888, 1).is_error());
    EXPECT(receive(&shared_buffers, 0, 0, Gfx::BitmapFormat::Invalid, 1).is_error());
}

#if !defined(AK_OS_WINDOWS)
TEST_CASE(image_bitmap_transfer_protects_against_backing_truncation)
{
    auto& realm = test_realm();
    auto source = create_image_bitmap(MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 16, 1 })));
    source->bitmap()->set_pixel(0, 0, Gfx::Color::Red);
    auto wrapper = wrap_image_bitmap(realm, source);
    Array<GC::Ref<JS::Object>, 1> transfer { wrapper };
    auto record = MUST(Web::HTML::structured_serialize_with_transfer(realm, wrapper, transfer));
    EXPECT_EQ(record.shared_buffers.size(), 1u);
#    if !defined(AK_OS_LINUX)
    // Use a resizable file to model a hostile sender rather than relying on platform shm resize behavior.
    char path[] = "/tmp/ladybird-image-bitmap-XXXXXX";
    auto fd = MUST(Core::System::mkstemp(path));
    MUST(Core::System::unlink(StringView { path, sizeof(path) - 1 }));
    VERIFY(ftruncate(fd, static_cast<off_t>(record.shared_buffers[0].size())) == 0);
    auto backing = MUST(Core::AnonymousBuffer::create_from_anon_fd(fd, record.shared_buffers[0].size()));
    record.shared_buffers[0].bytes().copy_to({ backing.data<u8>(), backing.size() });
    record.shared_buffers[0] = move(backing);
#    endif
    auto const& buffer = record.shared_buffers[0];
    Web::HTML::TransferDataDecoder decoder { move(record.transfer_data_holders[0]) };
    EXPECT_EQ(MUST(decoder.decode<Web::HTML::TransferType>()), Web::HTML::TransferType::ImageBitmap);
    decoder.set_shared_buffers(record.shared_buffers);
    auto receiver = Web::HTML::ImageBitmap::create();
    MUST(receiver->transfer_receiving_steps(realm, decoder));
#    if defined(AK_OS_LINUX)
    auto const seals = fcntl(buffer.fd(), F_GET_SEALS);
    EXPECT(seals >= 0);
    EXPECT_EQ(seals & (F_SEAL_SHRINK | F_SEAL_GROW), F_SEAL_SHRINK | F_SEAL_GROW);
    EXPECT_EQ(ftruncate(buffer.fd(), 0), -1);
    EXPECT_EQ(errno, EPERM);
    EXPECT_EQ(ftruncate(buffer.fd(), 128), -1);
    EXPECT_EQ(errno, EPERM);
#    else
    EXPECT_EQ(ftruncate(buffer.fd(), 0), 0);
#    endif
    EXPECT_EQ(receiver->bitmap()->get_pixel(0, 0), Gfx::Color::Red);
    receiver->close();
}
#endif

#if defined(AK_OS_LINUX)
TEST_CASE(image_bitmap_transfer_rejects_unsealed_backing)
{
    auto& realm = test_realm();
    // Even a correctly sized descriptor with sealing permanently disabled must be rejected.
    Vector<Core::AnonymousBuffer> shared_buffers;
    shared_buffers.append(MUST(Core::AnonymousBuffer::create_with_size(64)));
    Web::HTML::TransferDataEncoder holder;
    MUST(holder.encode(true));
    MUST(holder.encode(0u));
    MUST(holder.encode(static_cast<u64>(0)));
    MUST(holder.encode(16));
    MUST(holder.encode(1));
    MUST(holder.encode(Gfx::BitmapFormat::BGRA8888));
    MUST(holder.encode(Gfx::AlphaType::Premultiplied));
    Web::HTML::TransferDataDecoder decoder { move(holder) };
    decoder.set_shared_buffers(shared_buffers);
    auto receiver = Web::HTML::ImageBitmap::create();
    EXPECT(receiver->transfer_receiving_steps(realm, decoder).is_error());
    EXPECT_EQ(receiver->bitmap(), nullptr);
    EXPECT_EQ(ftruncate(shared_buffers[0].fd(), 0), 0);
}
#endif

TEST_CASE(image_bitmap_transfer_rejects_invalid_inline_pixels)
{
    auto& realm = test_realm();
    auto receive = [&](Gfx::BitmapFormat format, size_t pitch, ReadonlyBytes pixels) {
        Web::HTML::TransferDataEncoder holder;
        MUST(holder.encode(false));
        MUST(holder.encode(true));
        MUST(holder.encode(1));
        MUST(holder.encode(1));
        MUST(holder.encode(static_cast<u64>(pitch)));
        MUST(holder.encode(format));
        MUST(holder.encode(Gfx::AlphaType::Premultiplied));
        MUST(holder.encode(pixels));
        Web::HTML::TransferDataDecoder decoder { move(holder) };
        auto receiver = Web::HTML::ImageBitmap::create();
        return receiver->transfer_receiving_steps(realm, decoder);
    };
    Array<u8, 4> pixels {};
    EXPECT(!receive(Gfx::BitmapFormat::BGRA8888, 4, pixels).is_error());
    EXPECT(receive(Gfx::BitmapFormat::Invalid, 4, pixels).is_error());
    EXPECT(receive(Gfx::BitmapFormat::BGRA8888, 1, pixels).is_error());
    EXPECT(receive(Gfx::BitmapFormat::BGRA8888, 4, pixels.span().slice(0, 1)).is_error());
    EXPECT(receive(Gfx::BitmapFormat::BGRA8888, NumericLimits<size_t>::max(), pixels).is_error());
}

TEST_CASE(file_api_serializables_encode_to_frozen_bytes)
{
    (void)test_realm();
    {
        auto blob = Web::FileAPI::Blob::create(MUST(ByteBuffer::copy("hello"sv.bytes())), "text/plain"_utf16);
        Vector<u8> body;
        append_storage_string(body, "text/plain"sv);
        append_storage_bytes(body, "hello"sv.bytes());
        expect_storage_encoding(blob, serializable_storage_record("Blob"sv, 1, body));
    }

    auto file = MUST(Web::FileAPI::File::create({ { "data"_utf16 } }, "f.txt"_utf16));
    {
        Vector<u8> body;
        append_storage_string(body, ""sv); // default type
        append_storage_bytes(body, "data"sv.bytes());
        append_storage_string(body, "f.txt"sv);
        append_storage_signed_leb128(body, file->last_modified());
        expect_storage_encoding(file, serializable_storage_record("File"sv, 1, body));
    }

    {
        auto list = Web::FileAPI::FileList::create();
        list->add_file(file);
        Vector<u8> body;
        append_storage_leb128(body, 1);
        Vector<u8> file_body;
        append_storage_string(file_body, ""sv);
        append_storage_bytes(file_body, "data"sv.bytes());
        append_storage_string(file_body, "f.txt"sv);
        append_storage_signed_leb128(file_body, file->last_modified());
        frozen_serializable_value(body, "File"sv, 1, file_body);
        expect_storage_encoding(list, serializable_storage_record("FileList"sv, 1, body));
    }
}

TEST_CASE(webidl_serializables_encode_to_frozen_bytes)
{
    (void)test_realm();
    {
        auto exception = Web::WebIDL::DOMException::create("AbortError"_utf16_fly_string, "stop"_utf16);
        Vector<u8> body;
        append_storage_string(body, "AbortError"sv);
        append_storage_string(body, "stop"sv);
        expect_storage_encoding(exception, serializable_storage_record("DOMException"sv, 1, body));
    }

    {
        auto error = Web::WebIDL::QuotaExceededError::create("over"_utf16);
        Vector<u8> body;
        append_storage_string(body, "QuotaExceededError"sv);
        append_storage_string(body, "over"sv);
        body.append(0); // quota absent
        body.append(0); // requested absent
        expect_storage_encoding(error, serializable_storage_record("QuotaExceededError"sv, 1, body));
    }
}

TEST_CASE(crypto_key_variants_encode_to_frozen_bytes)
{
    // Encode each reachable CryptoKey variant from a live key.
    for (auto const& golden : crypto_algorithm_goldens()) {
        auto key = build_live_crypto_key(golden.type, golden.usage, golden.algorithm, golden.handle);
        expect_storage_encoding(key, crypto_key_full_record(golden.type, golden.usage, golden.algorithm, golden.handle));
    }

    // RsaKeyAlgorithm is encode-only.
    {
        Array<u8, 3> exponent { 0x01, 0x00, 0x01 };
        Array<u8, 4> modulus { 0xC0, 0x00, 0x00, 0x01 };
        auto algorithm = alg_rsa_key_algorithm("RSASSA-PKCS1-v1_5"sv, 2048, exponent);
        auto handle = handle_rsa_public_key(modulus, exponent);
        auto key = build_live_crypto_key_with_rsa_key_algorithm("public"sv, "verify"sv, "RSASSA-PKCS1-v1_5"sv, 2048, exponent, modulus);
        expect_storage_encoding(key, crypto_key_full_record("public"sv, "verify"sv, algorithm, handle));
    }
}

TEST_CASE(serializable_encode_completeness)
{
    // Every registry entry must have an encode golden.
    Array covered {
        "Blob"sv,
        "CryptoKey"sv,
        "DOMException"sv,
        "DOMMatrix"sv,
        "DOMMatrixReadOnly"sv,
        "DOMPoint"sv,
        "DOMPointReadOnly"sv,
        "DOMQuad"sv,
        "DOMRect"sv,
        "DOMRectReadOnly"sv,
        "File"sv,
        "FileList"sv,
        "ImageBitmap"sv,
        "ImageData"sv,
        "Module"sv,
        "QuotaExceededError"sv,
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

TEST_CASE(serializable_payload_version_bumps_require_migration_support)
{
    // The decoder requires an exact payload-version match, so a bump alone orphans a type's stored records.
    // Payload versions are owned by each implementation in this branch; the registry intentionally only maps
    // interface names to storage identifiers. Keep this check focused on the registry's completeness.
    for (auto const& entry : Web::HTML::serializable_storage_registry())
        EXPECT(!entry.identifier.is_empty());
}
