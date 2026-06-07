/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/BitCast.h>
#include <AK/Endian.h>
#include <AK/LEB128.h>
#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <AK/StdLibExtras.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/CryptoKey.h>
#include <LibWeb/Bindings/IntrinsicDefinitions.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PredefinedColorSpace.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/Crypto/CryptoKeySerializationTags.h>
#include <LibWeb/Crypto/KeyAlgorithms.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>

using Web::HTML::ValueTag;

template<typename Record>
inline Record ipc_round_trip(Record const& record)
{
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(record));

    FixedMemoryStream stream { buffer.data().span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    return MUST(decoder.decode<Record>());
}

template<typename T>
inline T structured_ipc_round_trip(T const& value)
{
    auto writer = Web::HTML::StructuredSerializeWriter::create_ipc();
    writer.encode(value);

    auto record = writer.take_ipc_record();
    Web::HTML::StructuredSerializeReader reader { record };
    EXPECT_EQ(reader.type(), Web::HTML::SerializationType::IPC);
    return MUST(reader.decode<T>());
}

template<typename T>
inline T structured_storage_round_trip(T const& value)
{
    auto writer = Web::HTML::StructuredSerializeWriter::create_storage();
    writer.encode(value);

    auto record = writer.take_storage_record();
    Web::HTML::StructuredSerializeReader reader { record };
    EXPECT_EQ(reader.type(), Web::HTML::SerializationType::Storage);
    return MUST(reader.decode<T>());
}

template<typename T>
inline void expect_structured_round_trip(T const& value)
{
    EXPECT_EQ(structured_ipc_round_trip(value), value);
    EXPECT_EQ(structured_storage_round_trip(value), value);
}

template<typename T>
inline ByteBuffer structured_ipc_buffer_round_trip(T const& value)
{
    auto writer = Web::HTML::StructuredSerializeWriter::create_ipc();
    writer.encode(value);

    auto record = writer.take_ipc_record();
    Web::HTML::StructuredSerializeReader reader { record };
    EXPECT_EQ(reader.type(), Web::HTML::SerializationType::IPC);
    return MUST(reader.decode<ByteBuffer>());
}

template<typename T>
inline ByteBuffer structured_storage_buffer_round_trip(T const& value)
{
    auto writer = Web::HTML::StructuredSerializeWriter::create_storage();
    writer.encode(value);

    auto record = writer.take_storage_record();
    Web::HTML::StructuredSerializeReader reader { record };
    EXPECT_EQ(reader.type(), Web::HTML::SerializationType::Storage);
    return MUST(reader.decode<ByteBuffer>());
}

inline void expect_utf16_equals(Utf16String const& actual, Utf16String const& expected)
{
    EXPECT(actual.utf16_view() == expected.utf16_view());
}

inline void expect_optional_utf16_equals(Optional<Utf16String> const& actual, Optional<Utf16String> const& expected)
{
    EXPECT_EQ(actual.has_value(), expected.has_value());
    if (actual.has_value())
        expect_utf16_equals(actual.value(), expected.value());
}

template<typename T>
inline void append_little_endian(Vector<u8>& bytes, T value)
{
    AK::LittleEndian<T> const little_endian { value };
    bytes.append(reinterpret_cast<u8 const*>(&little_endian), sizeof(little_endian));
}

inline void append_little_endian_u16(Vector<u8>& bytes, u16 value) { append_little_endian(bytes, value); }
inline void append_little_endian_u32(Vector<u8>& bytes, u32 value) { append_little_endian(bytes, value); }
inline void append_little_endian_u64(Vector<u8>& bytes, u64 value) { append_little_endian(bytes, value); }

inline void append_storage_leb128(Vector<u8>& bytes, u64 value) { LEB128<u64>::append_to(bytes, value); }
inline void append_storage_signed_leb128(Vector<u8>& bytes, i64 value) { LEB128<i64>::append_to(bytes, value); }

inline void append_storage_record_header(Vector<u8>& bytes)
{
    bytes.append('L');
    bytes.append('B');
    bytes.append('S');
    bytes.append('C');
    append_storage_leb128(bytes, 1); // storage format version
    append_storage_leb128(bytes, 0); // storage format flags
}

// The folded string header: (length << 1) | is_ascii, matching the production encoder.
inline void append_storage_string_header(Vector<u8>& bytes, bool is_ascii, size_t length)
{
    append_storage_leb128(bytes, (static_cast<u64>(length) << 1) | (is_ascii ? 1u : 0u));
}

inline void append_storage_ascii_utf16(Vector<u8>& bytes, StringView ascii)
{
    append_storage_string_header(bytes, true, ascii.length());
    bytes.append(ascii.bytes().data(), ascii.length());
}

inline Vector<u8> expected_storage_identifier_record(StringView identifier)
{
    Vector<u8> expected;
    append_storage_record_header(expected);
    append_storage_ascii_utf16(expected, identifier);
    return expected;
}

template<typename T>
inline void expect_storage_identifier_encoding(T const& value, StringView identifier)
{
    auto writer = Web::HTML::StructuredSerializeWriter::create_storage();
    writer.encode(value);

    auto record = writer.take_storage_record();
    auto expected = expected_storage_identifier_record(identifier);
    EXPECT_EQ(record.data.span(), expected.span());

    Web::HTML::StructuredSerializeReader reader { record };
    EXPECT_EQ(MUST(reader.decode<T>()), value);
}

template<typename T>
inline ErrorOr<T> decode_unknown_storage_identifier(StringView identifier)
{
    auto storage_record_bytes = expected_storage_identifier_record(identifier);
    Web::HTML::StorageSerializationRecord record { MUST(ByteBuffer::copy(storage_record_bytes.span())) };
    Web::HTML::StructuredSerializeReader reader { record };
    return reader.decode<T>();
}

// CryptoKey needs the principal realm below because it is [SecureContext].
inline JS::Realm& test_realm()
{
    static GC::Root<JS::Realm> realm;
    if (!realm)
        realm = Web::Bindings::create_a_simple_javascript_realm();
    return *realm;
}

inline Web::HTML::StorageSerializationRecord storage_serialize(GC::Ref<JS::Object> object)
{
    return MUST(Web::HTML::structured_serialize_for_storage(test_realm().vm(), object));
}

inline Web::WebIDL::ExceptionOr<JS::Value> storage_deserialize(Web::HTML::StorageSerializationRecord const& record)
{
    auto& realm = test_realm();
    Web::HTML::StructuredSerializeReader reader { record };
    Web::HTML::DeserializationMemory memory;
    return Web::HTML::structured_deserialize_internal(realm.vm(), reader, realm, memory, Web::HTML::CheckFullyConsumed::Yes);
}

inline void append_little_endian_double(Vector<u8>& bytes, double value)
{
    append_little_endian_u64(bytes, bit_cast<u64>(value));
}

inline void append_storage_utf16(Vector<u8>& bytes, ReadonlySpan<u16> code_units)
{
    append_storage_string_header(bytes, false, code_units.size());
    for (u16 code_unit : code_units)
        append_little_endian_u16(bytes, code_unit);
}

// ASCII strings use the storage fast path; other strings are UTF-16LE.
inline void append_storage_string(Vector<u8>& bytes, StringView string)
{
    if (string.is_ascii()) {
        append_storage_ascii_utf16(bytes, string);
        return;
    }
    auto utf16 = Utf16String::from_utf8(string);
    auto view = utf16.utf16_view();
    append_storage_string_header(bytes, false, view.length_in_code_units());
    for (size_t i = 0; i < view.length_in_code_units(); ++i)
        append_little_endian_u16(bytes, view.code_unit_at(i));
}

// The ByteBuffer/ReadonlyBytes wire form: a leb128 length followed by the raw octets.
inline void append_storage_bytes(Vector<u8>& bytes, ReadonlyBytes payload)
{
    append_storage_leb128(bytes, payload.size());
    bytes.append(payload.data(), payload.size());
}

inline Web::HTML::StorageSerializationRecord serializable_storage_record(StringView identifier, u64 payload_version, ReadonlyBytes body = {})
{
    Vector<u8> bytes;
    append_storage_record_header(bytes);
    bytes.append(to_underlying(ValueTag::SerializableObject));
    append_storage_ascii_utf16(bytes, identifier);
    append_storage_leb128(bytes, payload_version);
    bytes.append(body.data(), body.size());
    return Web::HTML::StorageSerializationRecord { MUST(ByteBuffer::copy(bytes.span())) };
}

inline Web::HTML::StorageSerializationRecord storage_record_with_value(Vector<u8> const& value_bytes)
{
    Vector<u8> bytes;
    append_storage_record_header(bytes);
    bytes.append(value_bytes.data(), value_bytes.size());
    return Web::HTML::StorageSerializationRecord { MUST(ByteBuffer::copy(bytes.span())) };
}

inline void frozen_serializable_value(Vector<u8>& bytes, StringView identifier, u64 payload_version, ReadonlyBytes body = {})
{
    bytes.append(to_underlying(ValueTag::SerializableObject));
    append_storage_ascii_utf16(bytes, identifier);
    append_storage_leb128(bytes, payload_version);
    bytes.append(body.data(), body.size());
}

// Strip the variable-width "LBSC" header so the value can be embedded in another record.
inline ByteBuffer serialized_object_body(GC::Ref<JS::Object> object)
{
    Vector<u8> header;
    append_storage_record_header(header);
    auto record = storage_serialize(object);
    return MUST(ByteBuffer::copy(record.data.bytes().slice(header.size())));
}

inline void append_byte_length(Vector<u8>& bytes, Optional<u32> length)
{
    bytes.append(length.has_value() ? 0 : 1);
    if (length.has_value())
        append_storage_leb128(bytes, *length);
}

enum class BackingBuffer : u8 {
    Fixed,
    Resizable,
};

inline void append_array_buffer_view_value(Vector<u8>& value, StringView constructor, BackingBuffer backing, ReadonlyBytes backing_bytes, Optional<u32> byte_length, u32 byte_offset, Optional<u32> array_length = {})
{
    value.append(to_underlying(ValueTag::ArrayBufferView));
    value.append(to_underlying(backing == BackingBuffer::Resizable ? ValueTag::ResizeableArrayBuffer : ValueTag::ArrayBuffer));
    append_storage_leb128(value, backing_bytes.size());
    value.append(backing_bytes.data(), backing_bytes.size());
    if (backing == BackingBuffer::Resizable)
        append_storage_leb128(value, backing_bytes.size()); // max byte length
    append_storage_ascii_utf16(value, constructor);
    append_byte_length(value, byte_length);
    append_storage_leb128(value, byte_offset);
    if (constructor != "DataView"sv)
        append_byte_length(value, array_length);
}

inline Web::HTML::StorageSerializationRecord array_buffer_view_record(StringView constructor, BackingBuffer backing, u32 buffer_size, Optional<u32> byte_length, u32 byte_offset, Optional<u32> array_length = {})
{
    auto backing_bytes = MUST(ByteBuffer::create_zeroed(buffer_size));
    Vector<u8> value;
    append_array_buffer_view_value(value, constructor, backing, backing_bytes, byte_length, byte_offset, array_length);
    return storage_record_with_value(value);
}

inline Web::HTML::StorageSerializationRecord image_bitmap_record(i32 width, i32 height, u64 pitch, StringView format, StringView alpha_type, ReadonlyBytes data)
{
    Vector<u8> body;
    body.append(1);
    append_storage_signed_leb128(body, width);
    append_storage_signed_leb128(body, height);
    append_storage_leb128(body, pitch);
    append_storage_ascii_utf16(body, format);
    append_storage_ascii_utf16(body, alpha_type);
    append_storage_bytes(body, data);
    return serializable_storage_record("ImageBitmap"sv, 1, body);
}

// Encodes ImageData fields independently so tests can make the dimensions disagree with the array.
inline Web::HTML::StorageSerializationRecord image_data_record(GC::Ref<JS::Object> data_array, i32 width, i32 height, StringView color_space)
{
    Vector<u8> body;
    auto data_bytes = serialized_object_body(data_array);
    body.append(data_bytes.data(), data_bytes.size());
    append_storage_signed_leb128(body, width);
    append_storage_signed_leb128(body, height);
    append_storage_ascii_utf16(body, color_space);
    return serializable_storage_record("ImageData"sv, 1, body);
}

// A fully frozen ImageData record whose data sub-value is a bare Uint8ClampedArray view.
inline Web::HTML::StorageSerializationRecord frozen_image_data_record(ReadonlyBytes pixels, i32 width, i32 height, StringView color_space)
{
    Vector<u8> body;
    append_array_buffer_view_value(body, "Uint8ClampedArray"sv, BackingBuffer::Fixed, pixels, static_cast<u32>(pixels.size()), 0, static_cast<u32>(pixels.size()));
    append_storage_signed_leb128(body, width);
    append_storage_signed_leb128(body, height);
    append_storage_string(body, color_space);
    return serializable_storage_record("ImageData"sv, 1, body);
}

// CryptoKey is [SecureContext], so decode it through a principal realm.
inline Web::WebIDL::ExceptionOr<JS::Value> crypto_storage_deserialize(Web::HTML::StorageSerializationRecord const& record)
{
    static GC::Root<JS::Realm> realm;
    auto& vm = test_realm().vm();
    if (!realm) {
        auto depth = vm.execution_context_stack().size();
        realm = Web::Bindings::create_a_principal_javascript_realm();
        while (vm.execution_context_stack().size() > depth)
            vm.pop_execution_context();
    }

    auto& settings = Web::Bindings::principal_host_defined_environment_settings_object(*realm);
    auto depth = vm.execution_context_stack().size();
    vm.push_execution_context(settings.realm_execution_context());

    Web::HTML::StructuredSerializeReader reader { record };
    Web::HTML::DeserializationMemory memory;
    auto result = Web::HTML::structured_deserialize_internal(vm, reader, *realm, memory, Web::HTML::CheckFullyConsumed::Yes);

    while (vm.execution_context_stack().size() > depth)
        vm.pop_execution_context();
    return result;
}

// `declared_handle_size` lets tests overstate the raw ByteBuffer handle length.
inline Web::HTML::StorageSerializationRecord crypto_key_secret_record(ReadonlyBytes handle, u64 declared_handle_size)
{
    Vector<u8> body;
    append_storage_string(body, "secret"sv);    // [[type]] (KeyType storage identifier)
    body.append(0);                             // [[extractable]] = false
    body.append(0);                             // algorithm: KeyAlgorithmTag::KeyAlgorithm (base)
    append_storage_string(body, "HKDF"sv);      // algorithm name
    append_storage_leb128(body, 1);             // [[usages]] count
    append_storage_string(body, "deriveKey"sv); // usages[0]
    body.append(0);                             // [[handle]]: HandleTag::ByteBuffer
    append_storage_leb128(body, declared_handle_size);
    body.append(handle.data(), handle.size());
    return serializable_storage_record("CryptoKey"sv, 1, body);
}

// UnsignedBigInteger stores big-endian octets in ByteBuffer form.
inline void append_storage_big_integer(Vector<u8>& bytes, ReadonlyBytes big_endian_octets)
{
    append_storage_bytes(bytes, big_endian_octets);
}

// Assemble a CryptoKey body from independently built algorithm and handle payloads.
inline Vector<u8> crypto_key_body(StringView type, StringView usage, Vector<u8> const& algorithm, Vector<u8> const& handle, bool extractable = false)
{
    Vector<u8> body;
    append_storage_string(body, type);
    body.append(extractable ? 1 : 0);
    body.append(algorithm.data(), algorithm.size());
    append_storage_leb128(body, 1);
    append_storage_string(body, usage);
    body.append(handle.data(), handle.size());
    return body;
}

inline Web::HTML::StorageSerializationRecord crypto_key_full_record(StringView type, StringView usage, Vector<u8> const& algorithm, Vector<u8> const& handle, bool extractable = false)
{
    return serializable_storage_record("CryptoKey"sv, 1, crypto_key_body(type, usage, algorithm, handle, extractable));
}

inline Vector<u8> alg_key_algorithm(StringView name)
{
    Vector<u8> bytes;
    bytes.append(0); // KeyAlgorithmTag::KeyAlgorithm
    append_storage_string(bytes, name);
    return bytes;
}

inline Vector<u8> alg_rsa_key_algorithm(StringView name, u32 modulus_length, ReadonlyBytes public_exponent)
{
    Vector<u8> bytes;
    bytes.append(1); // KeyAlgorithmTag::RsaKeyAlgorithm
    append_storage_string(bytes, name);
    append_storage_leb128(bytes, modulus_length);
    append_storage_big_integer(bytes, public_exponent);
    return bytes;
}

inline Vector<u8> alg_rsa_hashed_key_algorithm(StringView name, u32 modulus_length, ReadonlyBytes public_exponent, StringView hash_name)
{
    Vector<u8> bytes;
    bytes.append(2); // KeyAlgorithmTag::RsaHashedKeyAlgorithm
    append_storage_string(bytes, name);
    append_storage_leb128(bytes, modulus_length);
    append_storage_big_integer(bytes, public_exponent);
    append_storage_string(bytes, hash_name);
    return bytes;
}

inline Vector<u8> alg_ec_key_algorithm(StringView name, StringView named_curve)
{
    Vector<u8> bytes;
    bytes.append(3); // KeyAlgorithmTag::EcKeyAlgorithm
    append_storage_string(bytes, name);
    append_storage_string(bytes, named_curve); // named_curve is a Utf16String field
    return bytes;
}

inline Vector<u8> alg_aes_key_algorithm(StringView name, u16 length)
{
    Vector<u8> bytes;
    bytes.append(4); // KeyAlgorithmTag::AesKeyAlgorithm
    append_storage_string(bytes, name);
    append_storage_leb128(bytes, length);
    return bytes;
}

inline Vector<u8> alg_hmac_key_algorithm(StringView name, StringView hash_name, u32 length)
{
    Vector<u8> bytes;
    bytes.append(5); // KeyAlgorithmTag::HmacKeyAlgorithm
    append_storage_string(bytes, name);
    append_storage_string(bytes, hash_name);
    append_storage_leb128(bytes, length);
    return bytes;
}

inline Vector<u8> alg_kmac_key_algorithm(StringView name, u32 length)
{
    Vector<u8> bytes;
    bytes.append(6); // KeyAlgorithmTag::KmacKeyAlgorithm
    append_storage_string(bytes, name);
    append_storage_leb128(bytes, length);
    return bytes;
}

// Fixed octet content keeps handle goldens stable.
inline Vector<u8> handle_byte_buffer(ReadonlyBytes octets)
{
    Vector<u8> bytes;
    bytes.append(0); // HandleTag::ByteBuffer
    append_storage_bytes(bytes, octets);
    return bytes;
}

inline Vector<u8> handle_rsa_public_key(ReadonlyBytes modulus, ReadonlyBytes public_exponent)
{
    Vector<u8> bytes;
    bytes.append(1); // HandleTag::RSAPublicKey
    append_storage_big_integer(bytes, modulus);
    append_storage_big_integer(bytes, public_exponent);
    return bytes;
}

inline Vector<u8> handle_rsa_private_key(ReadonlySpan<ReadonlyBytes> components)
{
    VERIFY(components.size() == 8);
    Vector<u8> bytes;
    bytes.append(2); // HandleTag::RSAPrivateKey
    for (auto component : components)
        append_storage_big_integer(bytes, component);
    return bytes;
}

inline void append_ec_public_coordinates(Vector<u8>& bytes, ReadonlyBytes x, ReadonlyBytes y)
{
    append_storage_bytes(bytes, x);
    append_storage_bytes(bytes, y);
}

inline Vector<u8> handle_ec_public_key(ReadonlyBytes x, ReadonlyBytes y)
{
    Vector<u8> bytes;
    bytes.append(3); // HandleTag::ECPublicKey
    append_ec_public_coordinates(bytes, x, y);
    return bytes;
}

inline Vector<u8> handle_ec_private_key(ReadonlyBytes d, Optional<Vector<int>> parameters, Optional<Array<ReadonlyBytes, 2>> public_coordinates)
{
    Vector<u8> bytes;
    bytes.append(4); // HandleTag::ECPrivateKey
    append_storage_bytes(bytes, d);
    bytes.append(parameters.has_value() ? 1 : 0);
    if (parameters.has_value()) {
        append_storage_leb128(bytes, parameters->size());
        for (int parameter : *parameters)
            append_storage_signed_leb128(bytes, parameter);
    }
    bytes.append(public_coordinates.has_value() ? 1 : 0);
    if (public_coordinates.has_value())
        append_ec_public_coordinates(bytes, (*public_coordinates)[0], (*public_coordinates)[1]);
    return bytes;
}

inline Vector<u8> handle_single_buffer(u8 tag, ReadonlyBytes octets)
{
    Vector<u8> bytes;
    bytes.append(tag);
    append_storage_bytes(bytes, octets);
    return bytes;
}

inline Vector<u8> handle_triple_buffer(u8 tag, ReadonlyBytes seed, ReadonlyBytes public_key, ReadonlyBytes private_key)
{
    Vector<u8> bytes;
    bytes.append(tag);
    append_storage_bytes(bytes, seed);
    append_storage_bytes(bytes, public_key);
    append_storage_bytes(bytes, private_key);
    return bytes;
}

// Reachable CryptoKey variants paired with slot-consistent type/usage/handle data.
struct CryptoAlgorithmGolden {
    Web::Crypto::KeyAlgorithmTag tag;
    StringView type;
    StringView usage;
    Vector<u8> algorithm;
    Vector<u8> handle;
};

struct CryptoHandleGolden {
    Web::Crypto::HandleTag tag;
    StringView type;
    StringView usage;
    Vector<u8> algorithm;
    Vector<u8> handle;
};

// 32 nonzero EC coordinate octets so x_bytes()/y_bytes() keep their full length (no leading-zero trimming).
inline Vector<u8> ec_coordinate(u8 fill)
{
    Vector<u8> bytes;
    for (size_t i = 0; i < 32; ++i)
        bytes.append(fill);
    return bytes;
}

inline Vector<CryptoAlgorithmGolden> crypto_algorithm_goldens()
{
    using Web::Crypto::KeyAlgorithmTag;
    auto secret_handle = handle_byte_buffer("0123456789abcdef"sv.bytes());
    auto x = ec_coordinate(0x11);
    auto y = ec_coordinate(0x22);
    Array<u8, 3> exponent { 0x01, 0x00, 0x01 }; // 65537
    Array<u8, 4> modulus { 0xC0, 0x00, 0x00, 0x01 };

    Vector<CryptoAlgorithmGolden> goldens;
    goldens.append({ KeyAlgorithmTag::KeyAlgorithm, "secret"sv, "deriveKey"sv, alg_key_algorithm("HKDF"sv), secret_handle });
    goldens.append({ KeyAlgorithmTag::RsaHashedKeyAlgorithm, "public"sv, "verify"sv,
        alg_rsa_hashed_key_algorithm("RSASSA-PKCS1-v1_5"sv, 2048, exponent, "SHA-256"sv), handle_rsa_public_key(modulus, exponent) });
    goldens.append({ KeyAlgorithmTag::EcKeyAlgorithm, "public"sv, "verify"sv, alg_ec_key_algorithm("ECDSA"sv, "P-256"sv), handle_ec_public_key(x, y) });
    goldens.append({ KeyAlgorithmTag::AesKeyAlgorithm, "secret"sv, "encrypt"sv, alg_aes_key_algorithm("AES-CBC"sv, 128), secret_handle });
    goldens.append({ KeyAlgorithmTag::HmacKeyAlgorithm, "secret"sv, "sign"sv, alg_hmac_key_algorithm("HMAC"sv, "SHA-256"sv, 256), secret_handle });
    goldens.append({ KeyAlgorithmTag::KmacKeyAlgorithm, "secret"sv, "sign"sv, alg_kmac_key_algorithm("KMAC128"sv, 128), secret_handle });
    return goldens;
}

inline Vector<CryptoHandleGolden> crypto_handle_goldens()
{
    using Web::Crypto::HandleTag;
    auto buffer = "0123456789abcdef"sv.bytes();
    Array<u8, 3> exponent { 0x01, 0x00, 0x01 };
    Array<u8, 4> modulus { 0xC0, 0x00, 0x00, 0x01 };
    auto x = ec_coordinate(0x11);
    auto y = ec_coordinate(0x22);
    auto d = ec_coordinate(0x33);
    auto seed = "seedseedseedseed"sv.bytes();
    auto pub = "publicpublicpubl"sv.bytes();
    auto priv = "privprivprivpriv"sv.bytes();
    auto okp = "okpokpokpokpokpo"sv.bytes();
    Array<ReadonlyBytes, 8> rsa_components { modulus, exponent, exponent, modulus, modulus, exponent, exponent, modulus };

    Vector<CryptoHandleGolden> goldens;
    goldens.append({ HandleTag::ByteBuffer, "secret"sv, "deriveKey"sv, alg_key_algorithm("HKDF"sv), handle_byte_buffer(buffer) });
    goldens.append({ HandleTag::RSAPublicKey, "public"sv, "verify"sv,
        alg_rsa_hashed_key_algorithm("RSASSA-PKCS1-v1_5"sv, 2048, exponent, "SHA-256"sv), handle_rsa_public_key(modulus, exponent) });
    goldens.append({ HandleTag::RSAPrivateKey, "private"sv, "sign"sv,
        alg_rsa_hashed_key_algorithm("RSASSA-PKCS1-v1_5"sv, 2048, exponent, "SHA-256"sv), handle_rsa_private_key(rsa_components) });
    goldens.append({ HandleTag::ECPublicKey, "public"sv, "verify"sv, alg_ec_key_algorithm("ECDSA"sv, "P-256"sv), handle_ec_public_key(x, y) });
    goldens.append({ HandleTag::ECPrivateKey, "private"sv, "sign"sv, alg_ec_key_algorithm("ECDSA"sv, "P-256"sv), handle_ec_private_key(d, {}, {}) });
    goldens.append({ HandleTag::MLDSAPublicKey, "public"sv, "verify"sv, alg_key_algorithm("ML-DSA-44"sv), handle_single_buffer(5, pub) });
    goldens.append({ HandleTag::MLDSAPrivateKey, "private"sv, "sign"sv, alg_key_algorithm("ML-DSA-44"sv), handle_triple_buffer(6, seed, pub, priv) });
    goldens.append({ HandleTag::MLKEMPublicKey, "public"sv, "encapsulateKey"sv, alg_key_algorithm("ML-KEM-512"sv), handle_single_buffer(7, pub) });
    goldens.append({ HandleTag::MLKEMPrivateKey, "private"sv, "decapsulateKey"sv, alg_key_algorithm("ML-KEM-512"sv), handle_triple_buffer(8, seed, pub, priv) });
    goldens.append({ HandleTag::OKPPublicKey, "public"sv, "verify"sv, alg_key_algorithm("Ed25519"sv), handle_single_buffer(9, okp) });
    goldens.append({ HandleTag::OKPPrivateKey, "private"sv, "sign"sv, alg_key_algorithm("Ed25519"sv), handle_single_buffer(10, okp) });
    return goldens;
}

// Deserialize first so encode goldens use slot-consistent CryptoKeys.
inline GC::Ref<Web::Crypto::CryptoKey> build_live_crypto_key(StringView type, StringView usage, Vector<u8> const& algorithm, Vector<u8> const& handle)
{
    auto value = MUST(crypto_storage_deserialize(crypto_key_full_record(type, usage, algorithm, handle)));
    return as<Web::Crypto::CryptoKey>(value.as_object());
}

// Fabricate RsaKeyAlgorithm directly to pin the encode-only tag-1 branch.
inline GC::Ref<Web::Crypto::CryptoKey> build_live_crypto_key_with_rsa_key_algorithm(StringView type, StringView usage, StringView name, u32 modulus_length, ReadonlyBytes public_exponent, ReadonlyBytes modulus)
{
    auto& realm = test_realm();
    auto exponent = ::Crypto::UnsignedBigInteger::import_data(public_exponent);
    auto modulus_value = ::Crypto::UnsignedBigInteger::import_data(modulus);

    auto key = Web::Crypto::CryptoKey::create(realm, ::Crypto::PK::RSAPublicKey { modulus_value, exponent });
    key->set_type(type == "public"sv ? Web::Bindings::KeyType::Public : (type == "private"sv ? Web::Bindings::KeyType::Private : Web::Bindings::KeyType::Secret));
    key->set_extractable(false);
    key->set_usages({ usage == "verify"sv ? Web::Bindings::KeyUsage::Verify : Web::Bindings::KeyUsage::Sign });

    auto algorithm = Web::Crypto::RsaKeyAlgorithm::create(realm);
    algorithm->set_name(MUST(String::from_utf8(name)));
    algorithm->set_modulus_length(modulus_length);
    MUST(algorithm->set_public_exponent(exponent));
    key->set_algorithm(algorithm);
    return key;
}
