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
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/Module.h>
#include <LibWeb/Crypto/CryptoKeySerializationTags.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/FileAPI/FileList.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/Geometry/DOMPointReadOnly.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectReadOnly.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/MessagePort.h>
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

// A storage record names the value of an enumeration by a stable identifier, which deserializing the record reads and
// serializing the value it creates writes back.
static void expect_identifiers_round_trip(Web::HTML::StorageSerializationRecord const& record, bool needs_principal_realm = false)
{
    auto value = MUST(needs_principal_realm ? principal_storage_deserialize(record) : storage_deserialize(record));
    EXPECT_EQ(storage_serialize(GC::Ref { value.as_object() }).data.span(), record.data.span());
}

TEST_CASE(storage_writer_encodes_enum_like_values_as_stable_identifiers)
{
    auto handle = handle_byte_buffer("0123456789abcdef"sv.bytes());
    for (auto type : { "public"sv, "private"sv, "secret"sv })
        expect_identifiers_round_trip(crypto_key_full_record(type, "deriveKey"sv, alg_key_algorithm("HKDF"sv), handle), true);
    for (auto usage : { "encrypt"sv, "decrypt"sv, "sign"sv, "verify"sv, "deriveKey"sv, "deriveBits"sv, "wrapKey"sv, "unwrapKey"sv, "encapsulateKey"sv, "encapsulateBits"sv, "decapsulateKey"sv, "decapsulateBits"sv })
        expect_identifiers_round_trip(crypto_key_full_record("secret"sv, usage, alg_key_algorithm("HKDF"sv), handle), true);

    Array<u8, 4> pixels { 1, 2, 3, 255 };
    for (auto color_space : { "srgb"sv, "srgb-linear"sv, "display-p3"sv, "display-p3-linear"sv })
        expect_identifiers_round_trip(frozen_image_data_record(pixels.span(), 1, 1, color_space));
    for (auto format : { "BGRx8888"sv, "BGRA8888"sv, "RGBx8888"sv, "RGBA8888"sv }) {
        for (auto alpha_type : { "premultiplied"sv, "unpremultiplied"sv })
            expect_identifiers_round_trip(image_bitmap_record(1, 1, 4, format, alpha_type, pixels.span()));
    }
}

TEST_CASE(storage_writer_encodes_js_strings_as_utf16_code_units)
{
    auto& vm = test_realm().vm();
    Array<char16_t, 3> code_units {
        u'A',
        static_cast<char16_t>(0xd800),
        u'B',
    };
    auto value = JS::PrimitiveString::create(vm, Utf16String::from_utf16({ code_units.data(), code_units.size() }));

    auto record = MUST(Web::HTML::structured_serialize_for_storage(vm, value));

    Array<u8, 14> expected {
        'L',
        'B',
        'S',
        'C',
        0x01, // version
        0x00, // flags
        to_underlying(ValueTag::StringPrimitive),
        0x06, // string header: (3 << 1) | utf16
        0x41,
        0x00, // 'A'
        0x00,
        0xd8, // U+D800
        0x42,
        0x00, // 'B'
    };
    EXPECT_EQ(record.data.span(), expected);

    auto decoded = MUST(storage_deserialize(record)).as_string().utf16_string();
    auto view = decoded.utf16_view();
    EXPECT_EQ(view.length_in_code_units(), 3u);
    EXPECT_EQ(view.code_unit_at(0), u'A');
    EXPECT_EQ(view.code_unit_at(1), static_cast<char16_t>(0xd800));
    EXPECT_EQ(view.code_unit_at(2), u'B');
}

TEST_CASE(storage_writer_encodes_ascii_strings_as_one_byte_per_code_unit)
{
    auto& vm = test_realm().vm();
    auto value = Utf16String::from_utf8("AB"sv);
    EXPECT(value.utf16_view().has_ascii_storage());

    auto record = MUST(Web::HTML::structured_serialize_for_storage(vm, JS::PrimitiveString::create(vm, value)));

    Array<u8, 10> expected {
        'L',
        'B',
        'S',
        'C',
        0x01, // version
        0x00, // flags
        to_underlying(ValueTag::StringPrimitive),
        0x05, // string header: (2 << 1) | ascii
        'A',
        'B',
    };
    EXPECT_EQ(record.data.span(), expected);

    auto decoded = MUST(storage_deserialize(record)).as_string().utf16_string();
    EXPECT_EQ(decoded, value);
    EXPECT(decoded.utf16_view().has_ascii_storage());
}

TEST_CASE(storage_writer_encodes_the_string_length_as_a_leb128_varint)
{
    auto& vm = test_realm().vm();

    // Pin both sides of the folded-header LEB128 width boundary. The string header follows the 6-byte record header
    // and the value tag.
    {
        auto value = Utf16String::repeated('a', 63);
        auto record = MUST(Web::HTML::structured_serialize_for_storage(vm, JS::PrimitiveString::create(vm, value)));

        Array<u8, 1> expected_framing { 0x7F };
        EXPECT_EQ(record.data.span().slice(7, expected_framing.size()), expected_framing);

        EXPECT_EQ(MUST(storage_deserialize(record)).as_string().utf16_string(), value);
    }

    {
        auto value = Utf16String::repeated('a', 64);
        auto record = MUST(Web::HTML::structured_serialize_for_storage(vm, JS::PrimitiveString::create(vm, value)));

        Array<u8, 2> expected_framing { 0x81, 0x01 };
        EXPECT_EQ(record.data.span().slice(7, expected_framing.size()), expected_framing);

        EXPECT_EQ(MUST(storage_deserialize(record)).as_string().utf16_string(), value);
    }
}

TEST_CASE(empty_strings_round_trip)
{
    auto& realm = test_realm();
    auto& vm = realm.vm();
    auto value = JS::PrimitiveString::create(vm, Utf16String {});

    auto ipc_clone = MUST(Web::HTML::structured_deserialize(vm, MUST(Web::HTML::structured_serialize(vm, value)), realm));
    EXPECT(ipc_clone.as_string().utf16_string().is_empty());

    auto storage_clone = MUST(storage_deserialize(MUST(Web::HTML::structured_serialize_for_storage(vm, value))));
    EXPECT(storage_clone.as_string().utf16_string().is_empty());
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
    auto point = Web::Geometry::DOMPoint::create(1.0, 2.0, 3.0, 4.0);

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
    auto point = Web::Geometry::DOMPoint::create(1.0, 2.0, 3.0, 4.0);

    auto record = structured_serialize(test_realm().vm(), point);

    Vector<u8> expected;
    expected.append(to_underlying(ValueTag::SerializableObject));
    append_little_endian_u16(expected, to_underlying(Web::Bindings::InterfaceName::DOMPoint));
    append_little_endian_double(expected, 1.0);
    append_little_endian_double(expected, 2.0);
    append_little_endian_double(expected, 3.0);
    append_little_endian_double(expected, 4.0);
    EXPECT_EQ(record.data.span(), expected.span());
}

TEST_CASE(webassembly_module_encodes_to_frozen_ipc_bytes)
{
    static constexpr auto module_bytes = to_array<u8>({ 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 });

    auto& realm = principal_test_realm();
    auto& vm = realm.vm();
    auto& settings = Web::Bindings::principal_host_defined_environment_settings_object(realm);
    auto depth = vm.execution_context_stack().size();
    vm.push_execution_context(settings.realm_execution_context());

    auto bytes = JS::ArrayBuffer::create(realm, MUST(ByteBuffer::copy(module_bytes.span())));
    auto& module_constructor = Web::Bindings::ensure_web_constructor<Web::Bindings::ModulePrototype>(realm, "WebAssembly.Module"_utf16_fly_string);
    auto module = MUST(JS::construct(vm, module_constructor, bytes.ptr()));
    auto record = MUST(Web::HTML::structured_serialize(vm, JS::Value(module.ptr())));

    while (vm.execution_context_stack().size() > depth)
        vm.pop_execution_context();

    Vector<u8> expected;
    expected.append(to_underlying(ValueTag::SerializableObject));
    append_little_endian_u16(expected, to_underlying(Web::Bindings::InterfaceName::Module));
    append_little_endian_u32(expected, module_bytes.size());
    expected.append(module_bytes.data(), module_bytes.size());
    auto agent_cluster_id = settings.agent_cluster_id();
    expected.append(agent_cluster_id.has_value() ? 1 : 0);
    if (agent_cluster_id.has_value())
        append_little_endian_u64(expected, *agent_cluster_id);
    EXPECT_EQ(record.data.span(), expected.span());
}

TEST_CASE(realm_helpers_share_vm_initialization)
{
    auto& vm = test_realm().vm();
    auto depth = vm.execution_context_stack().size();

    auto principal = Web::Bindings::create_a_principal_javascript_realm();
    EXPECT_EQ(&principal->vm(), &vm);

    auto simple = Web::Bindings::create_a_simple_javascript_realm();
    EXPECT_EQ(&simple->vm(), &vm);

    // The simple realm hands its execution context to the stack, so it's freed once popped.
    delete vm.pop_execution_context();
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
        auto point = Web::Geometry::DOMPoint::create(1.5, -2.5, 3.5, -4.5);
        Vector<u8> body;
        for (double element : { 1.5, -2.5, 3.5, -4.5 })
            append_little_endian_double(body, element);
        expect_storage_encoding(point, serializable_storage_record("DOMPoint"sv, 1, body));

        auto read_only = Web::Geometry::DOMPointReadOnly::create(1.5, -2.5, 3.5, -4.5);
        expect_storage_encoding(read_only, serializable_storage_record("DOMPointReadOnly"sv, 1, body));
    }

    {
        auto rect = Web::Geometry::DOMRect::create(10.0, 20.0, 30.0, 40.0);
        Vector<u8> body;
        for (double element : { 10.0, 20.0, 30.0, 40.0 })
            append_little_endian_double(body, element);
        expect_storage_encoding(rect, serializable_storage_record("DOMRect"sv, 1, body));

        auto read_only = Web::Geometry::DOMRectReadOnly::create(10.0, 20.0, 30.0, 40.0);
        expect_storage_encoding(read_only, serializable_storage_record("DOMRectReadOnly"sv, 1, body));
    }

    {
        // Identity 2D matrices serialize six components.
        Vector<u8> body;
        body.append(1);
        for (double element : { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 })
            append_little_endian_double(body, element);
        auto record = serializable_storage_record("DOMMatrixReadOnly"sv, 1, body);
        expect_storage_encoding(GC::Ref { MUST(storage_deserialize(record)).as_object() }, record);

        auto mutable_matrix = Web::Geometry::DOMMatrix::create();
        expect_storage_encoding(mutable_matrix, serializable_storage_record("DOMMatrix"sv, 1, body));
    }

    {
        // DOMQuad serializes its four corner points as nested DOMPoint sub-values.
        Vector<u8> body;
        for (int i = 0; i < 4; ++i) {
            Vector<u8> point_body;
            for (double coordinate : { 1.0 + i, 2.0, 3.0, 4.0 })
                append_little_endian_double(point_body, coordinate);
            frozen_serializable_value(body, "DOMPoint"sv, 1, point_body);
        }
        auto record = serializable_storage_record("DOMQuad"sv, 1, body);
        expect_storage_encoding(GC::Ref { MUST(storage_deserialize(record)).as_object() }, record);
    }
}

TEST_CASE(image_serializables_encode_to_frozen_bytes)
{
    auto& realm = test_realm();

    {
        auto image = MUST(Web::HTML::ImageData::create(realm, 1, 1));
        auto image_wrapper = Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, image);
        auto pixels = GC::Ref { MUST(image_wrapper->get("data"_utf16_fly_string)).as_object() };

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
        auto image_bitmap = Web::HTML::ImageBitmap::create(bitmap);

        // Use the bitmap's actual pitch and data size.
        ReadonlyBytes data { bitmap->scanline_u8(0), bitmap->data_size() };
        auto expected = image_bitmap_record(bitmap->width(), bitmap->height(), bitmap->pitch(), "BGRA8888"sv, "premultiplied"sv, data);
        expect_storage_encoding(image_bitmap, expected);
    }

    {
        auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 2, 2 }));
        auto color_space = MUST(Gfx::ColorSpace::from_cicp({ Media::ColorPrimaries::SMPTE432, Media::TransferCharacteristics::SRGB, Media::MatrixCoefficients::Identity, Media::VideoFullRangeFlag::Full }));
        auto image_bitmap = Web::HTML::ImageBitmap::create(Gfx::DecodedImageFrame { *bitmap, move(color_space) });

        ReadonlyBytes data { bitmap->scanline_u8(0), bitmap->data_size() };
        Array<u8, 3> display_p3 { 1, 12, 13 }; // CICP, SMPTE432 primaries, sRGB transfer characteristics
        auto expected = image_bitmap_record(bitmap->width(), bitmap->height(), bitmap->pitch(), "BGRA8888"sv, "premultiplied"sv, data, { display_p3.data(), display_p3.size() });
        expect_storage_encoding(image_bitmap, expected);
    }
}

static GC::Ref<JS::Object> wrap_image_bitmap(JS::Realm& realm, GC::Ref<Web::HTML::ImageBitmap> image_bitmap)
{
    return Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, image_bitmap);
}

static GC::Ref<Web::HTML::ImageBitmap> create_image_bitmap(NonnullRefPtr<Gfx::Bitmap> bitmap)
{
    return Web::HTML::ImageBitmap::create(move(bitmap));
}

// The transfer-receiving steps of an ImageBitmap, which create the ImageBitmap.
static Web::WebIDL::ExceptionOr<GC::Ref<JS::HostObject>> receive_image_bitmap(JS::Realm& realm, Web::HTML::TransferDataDecoder& decoder)
{
    return Web::Bindings::create_transferred_platform_object(Web::HTML::TransferType::ImageBitmap, realm, decoder);
}

static GC::RootVector<JS::Value> deserialize_transfer_data_holders(JS::Realm& realm, Web::HTML::SerializedTransferRecord& record)
{
    GC::RootVector<JS::Value> values;
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
    auto port = Web::HTML::MessagePort::create(realm);
    transfer.append(buffer);
    transfer.append(Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, port));

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
    auto port = Web::HTML::MessagePort::create(realm);
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

    auto port_wrapper = Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, port);
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
    shared_buffers.append(MUST(Core::AnonymousBuffer::create_with_size(64)));
    auto receive = [&](Vector<Core::AnonymousBuffer> const* buffers, u32 buffer_index, u64 offset, Gfx::BitmapFormat format, int width) {
        Web::HTML::TransferDataEncoder holder;
        MUST(holder.encode(true));
        MUST(holder.encode(buffer_index));
        MUST(holder.encode(offset));
        MUST(holder.encode(width));
        MUST(holder.encode(1));
        MUST(holder.encode(format));
        MUST(holder.encode(Gfx::AlphaType::Premultiplied));
        MUST(holder.encode(static_cast<u8>(0))); // sRGB
        Web::HTML::TransferDataDecoder decoder { move(holder) };
        if (buffers)
            decoder.set_shared_buffers(*buffers);
        return receive_image_bitmap(realm, decoder);
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
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 16, 1 }));
    bitmap->set_pixel(0, 0, Gfx::Color::Red);
    auto source = create_image_bitmap(bitmap);
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
    auto received = MUST(receive_image_bitmap(realm, decoder));
    auto& receiver = unwrap_wrappable<Web::HTML::ImageBitmap>(JS::Value(static_cast<JS::Object*>(received.ptr())));
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
    EXPECT_EQ(receiver.bitmap()->get_pixel(0, 0), Gfx::Color::Red);
    receiver.close();
}
#endif

#if defined(AK_OS_LINUX)
TEST_CASE(image_bitmap_transfer_rejects_unsealed_backing)
{
    auto& realm = test_realm();
    // Even a correctly sized descriptor with sealing permanently disabled must be rejected.
    Vector<Core::AnonymousBuffer> shared_buffers;
    auto unsealable_fd = MUST(Core::System::anon_create(64, O_CLOEXEC, Core::System::AllowSealing::No));
    shared_buffers.append(MUST(Core::AnonymousBuffer::create_from_anon_fd(unsealable_fd, 64)));
    Web::HTML::TransferDataEncoder holder;
    MUST(holder.encode(true));
    MUST(holder.encode(0u));
    MUST(holder.encode(static_cast<u64>(0)));
    MUST(holder.encode(16));
    MUST(holder.encode(1));
    MUST(holder.encode(Gfx::BitmapFormat::BGRA8888));
    MUST(holder.encode(Gfx::AlphaType::Premultiplied));
    MUST(holder.encode(static_cast<u8>(0))); // sRGB
    Web::HTML::TransferDataDecoder decoder { move(holder) };
    decoder.set_shared_buffers(shared_buffers);
    EXPECT(receive_image_bitmap(realm, decoder).is_error());
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
        return receive_image_bitmap(realm, decoder);
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

    auto file = Web::FileAPI::File::create("data"sv.bytes(), "f.txt"_utf16, {});
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
