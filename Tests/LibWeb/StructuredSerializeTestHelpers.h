/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/BitCast.h>
#include <AK/ByteBuffer.h>
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
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/Crypto/CryptoKeySerializationTags.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/FontPlugin.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>

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
inline void append_storage_string_header(Vector<u8>& bytes, bool is_latin1, size_t length)
{
    append_storage_leb128(bytes, (static_cast<u64>(length) << 1) | (is_latin1 ? 1u : 0u));
}

inline void append_storage_ascii_utf16(Vector<u8>& bytes, StringView ascii)
{
    append_storage_string_header(bytes, true, ascii.length());
    bytes.append(ascii.bytes().data(), ascii.length());
}

// CryptoKey needs the principal realm below because it is [SecureContext].
inline JS::Realm& test_realm()
{
    static GC::Root<JS::Realm> realm;
    if (!realm)
        realm = Web::Bindings::create_a_simple_javascript_realm();
    return *realm;
}

// A data holder as C++ carries it: the bytes and attachments that its writers added, then its channel ends, of which
// this one has none.
inline IPC::MessageBuffer carried_transfer_data_holder(IPC::MessageBuffer data_holder)
{
    IPC::MessageBuffer carried;
    IPC::Encoder encoder { carried };
    MUST(encoder.encode(data_holder.data().span()));
    auto attachments = data_holder.take_attachments();
    MUST(encoder.encode_size(attachments.size()));
    for (auto& attachment : attachments)
        MUST(encoder.append_attachment(move(attachment)));
    MUST(encoder.encode_size(0));
    return carried;
}

// The bytes that the writers of a data holder added.
inline ByteBuffer transfer_data_holder_bytes(IPC::MessageBuffer const& carried)
{
    FixedMemoryStream stream { carried.data().span() };
    Queue<IPC::Attachment> no_attachments;
    IPC::Decoder decoder { stream, no_attachments };
    return MUST(decoder.decode<ByteBuffer>());
}

// StructuredDeserializeWithTransfer of a record whose [[Serialized]] is undefined and whose only data holder is the
// bytes: the transferred value, or the exception.
inline Web::WebIDL::ExceptionOr<JS::Value> receive_transfer_data_holder(IPC::MessageBuffer data_holder, Vector<Core::AnonymousBuffer> shared_buffers = {})
{
    auto& realm = test_realm();
    Web::HTML::SerializedTransferRecord record;
    record.serialized = MUST(Web::HTML::structured_serialize(realm.vm(), JS::js_undefined()));
    record.transfer_data_holders.append(Web::HTML::TransferDataEncoder { carried_transfer_data_holder(move(data_holder)) });
    record.shared_buffers = move(shared_buffers);
    auto deserialized = TRY(Web::HTML::structured_deserialize_with_transfer(record, realm));
    return JS::Value { deserialized.transferred_values[0].ptr() };
}

// Reads the bytes and the attachments of a transfer data holder, which it takes.
struct TransferDataHolderReader {
    explicit TransferDataHolderReader(Web::HTML::TransferDataEncoder const& data_holder)
        : buffer(data_holder.take_buffer())
        , bytes(transfer_data_holder_bytes(buffer))
        , stream(bytes.bytes())
    {
        for (auto& attachment : buffer.take_attachments())
            attachments.enqueue(move(attachment));
    }

    IPC::MessageBuffer buffer;
    ByteBuffer bytes;
    FixedMemoryStream stream;
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
};

template<typename T>
requires(IsBaseOf<Web::Bindings::Wrappable, T> || Web::Bindings::RustInterfaceHandle<T>)
inline T& unwrap_wrappable(JS::Value value)
{
    VERIFY(value.is_object());
    auto* object = Web::Bindings::impl_from<T>(&value.as_object());
    VERIFY(object);
    return *object;
}

inline Web::HTML::StorageSerializationRecord storage_serialize(GC::Ref<JS::Object> object)
{
    return MUST(Web::HTML::structured_serialize_for_storage(test_realm().vm(), object));
}

template<typename T>
requires(IsConvertible<T*, Web::Bindings::Wrappable*>)
inline Web::HTML::StorageSerializationRecord storage_serialize(GC::Ref<T> object)
{
    auto& realm = test_realm();
    auto wrapper = Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, GC::Ref<Web::Bindings::Wrappable> { object });
    return MUST(Web::HTML::structured_serialize_for_storage(realm.vm(), JS::Value(wrapper.ptr())));
}

template<Web::Bindings::RustInterfaceHandle T>
inline Web::HTML::StorageSerializationRecord storage_serialize(GC::Ref<T> object)
{
    auto& realm = test_realm();
    auto wrapper = Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, object);
    return MUST(Web::HTML::structured_serialize_for_storage(realm.vm(), JS::Value(static_cast<JS::Object*>(wrapper.ptr()))));
}

template<typename T>
requires(IsConvertible<T*, Web::Bindings::Wrappable*>)
inline Web::HTML::IPCSerializationRecord structured_serialize(JS::VM& vm, GC::Ref<T> object)
{
    auto& realm = test_realm();
    auto wrapper = Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, GC::Ref<Web::Bindings::Wrappable> { object });
    return MUST(Web::HTML::structured_serialize(vm, JS::Value(wrapper.ptr())));
}

template<Web::Bindings::RustInterfaceHandle T>
inline Web::HTML::IPCSerializationRecord structured_serialize(JS::VM& vm, GC::Ref<T> object)
{
    auto& realm = test_realm();
    auto wrapper = Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, object);
    return MUST(Web::HTML::structured_serialize(vm, JS::Value(static_cast<JS::Object*>(wrapper.ptr()))));
}

// The value of the property of the object, which is a wrapper that the test reads as script would.
inline JS::Value property_of(JS::Value object, StringView name)
{
    VERIFY(object.is_object());
    return MUST(object.as_object().get(Utf16FlyString::from_utf8(name)));
}

inline double number_property(JS::Value object, StringView name)
{
    return property_of(object, name).as_double();
}

inline Utf16String string_property(JS::Value object, StringView name)
{
    return property_of(object, name).as_string().utf16_string();
}

inline Web::WebIDL::ExceptionOr<JS::Value> storage_deserialize(Web::HTML::StorageSerializationRecord const& record)
{
    auto& realm = test_realm();
    return Web::HTML::structured_deserialize(realm.vm(), record, realm);
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

template<typename T>
requires(IsConvertible<T*, Web::Bindings::Wrappable*> || Web::Bindings::RustInterfaceHandle<T>)
inline ByteBuffer serialized_object_body(GC::Ref<T> object)
{
    auto header_size = 6uz;
    auto record = storage_serialize(object);
    return MUST(ByteBuffer::copy(record.data.bytes().slice(header_size)));
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

// A bitmap in sRGB has no color space bytes. Any other color space follows the pixels.
inline Web::HTML::StorageSerializationRecord image_bitmap_record(i32 width, i32 height, u64 pitch, StringView format, StringView alpha_type, ReadonlyBytes data, ReadonlyBytes color_space = {})
{
    Vector<u8> body;
    body.append(color_space.is_empty() ? 1 : 2);
    append_storage_signed_leb128(body, width);
    append_storage_signed_leb128(body, height);
    append_storage_leb128(body, pitch);
    append_storage_ascii_utf16(body, format);
    append_storage_ascii_utf16(body, alpha_type);
    append_storage_bytes(body, data);
    body.append(color_space.data(), color_space.size());
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

// CryptoKey is [SecureContext], and WebAssembly module compilation consults Content Security Policy, so they need a principal realm.
inline JS::Realm& principal_test_realm()
{
    static GC::Root<JS::Realm> realm;
    if (!realm) {
        auto& vm = test_realm().vm();
        auto depth = vm.execution_context_stack().size();
        realm = Web::Bindings::create_a_principal_javascript_realm();
        while (vm.execution_context_stack().size() > depth)
            vm.pop_execution_context();

        // The Content Security Policy requires a document.
        static Web::Platform::FontPlugin font_plugin { false };
        Web::Platform::FontPlugin::install(font_plugin);

        auto* window = Web::HTML::window_from_global_object(realm->global_object());
        VERIFY(window);

        auto& settings = Web::Bindings::principal_host_defined_environment_settings_object(*realm);
        auto document = Web::DOM::Document::create(Web::Bindings::principal_host_defined_page(*realm), *window, settings.creation_url);
        window->set_associated_document(document);
        document->set_window(*window);
    }
    return *realm;
}

inline Web::WebIDL::ExceptionOr<JS::Value> principal_storage_deserialize(Web::HTML::StorageSerializationRecord const& record)
{
    auto& realm = principal_test_realm();
    auto& vm = realm.vm();
    auto& settings = Web::Bindings::principal_host_defined_environment_settings_object(realm);
    auto depth = vm.execution_context_stack().size();
    vm.push_execution_context(settings.realm_execution_context());

    auto result = Web::HTML::structured_deserialize(vm, record, realm);

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

// A big integer is stored as its big-endian octets.
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

// The base point of P-256, which EC key handles must hold a point on.
inline Vector<u8> p256_private_value_one()
{
    auto bytes = ec_coordinate(0);
    bytes.last() = 1;
    return bytes;
}

inline Vector<u8> p256_base_point_x()
{
    return { 0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2, 0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96 };
}

inline Vector<u8> p256_base_point_y()
{
    return { 0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16, 0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5 };
}

inline Vector<CryptoAlgorithmGolden> crypto_algorithm_goldens()
{
    using Web::Crypto::KeyAlgorithmTag;
    auto secret_handle = handle_byte_buffer("0123456789abcdef"sv.bytes());
    auto x = p256_base_point_x();
    auto y = p256_base_point_y();
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

// ML-DSA and ML-KEM key handles must hold real keys, so these come from fixed seeds, made by OpenSSL as Web Crypto
// makes them.
struct ModuleLatticeKey {
    ByteBuffer seed;
    ByteBuffer public_key;
    ByteBuffer expanded_key;
};

inline ByteBuffer octet_string_param_of(EVP_PKEY* key, char const* name)
{
    size_t size = 0;
    VERIFY(EVP_PKEY_get_octet_string_param(key, name, nullptr, 0, &size) == 1);
    auto bytes = MUST(ByteBuffer::create_uninitialized(size));
    VERIFY(EVP_PKEY_get_octet_string_param(key, name, bytes.data(), bytes.size(), &size) == 1);
    return bytes;
}

inline ModuleLatticeKey module_lattice_key_from_seed(char const* algorithm, ReadonlyBytes seed)
{
    auto* context = EVP_PKEY_CTX_new_from_name(nullptr, algorithm, nullptr);
    VERIFY(context);
    VERIFY(EVP_PKEY_fromdata_init(context) == 1);
    OSSL_PARAM params[] {
        OSSL_PARAM_octet_string("seed", const_cast<u8*>(seed.data()), seed.size()),
        OSSL_PARAM_END,
    };
    EVP_PKEY* key = nullptr;
    VERIFY(EVP_PKEY_fromdata(context, &key, EVP_PKEY_KEYPAIR, params) == 1);
    EVP_PKEY_CTX_free(context);
    ModuleLatticeKey result {
        MUST(ByteBuffer::copy(seed)),
        octet_string_param_of(key, OSSL_PKEY_PARAM_PUB_KEY),
        octet_string_param_of(key, OSSL_PKEY_PARAM_PRIV_KEY),
    };
    EVP_PKEY_free(key);
    return result;
}

inline ModuleLatticeKey mldsa_44_key(u8 first_seed_byte = 0)
{
    Array<u8, 32> seed;
    for (size_t i = 0; i < seed.size(); ++i)
        seed[i] = first_seed_byte + i;
    return module_lattice_key_from_seed("ML-DSA-44", seed);
}

inline ModuleLatticeKey mlkem_512_key()
{
    Array<u8, 64> seed;
    for (size_t i = 0; i < seed.size(); ++i)
        seed[i] = i;
    return module_lattice_key_from_seed("ML-KEM-512", seed);
}

// The key pair of RFC 8032's first Ed25519 test vector.
inline Vector<u8> ed25519_public_key()
{
    return { 0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a };
}

inline Vector<u8> ed25519_private_key()
{
    return { 0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60 };
}

inline Vector<CryptoHandleGolden> crypto_handle_goldens()
{
    using Web::Crypto::HandleTag;
    auto buffer = "0123456789abcdef"sv.bytes();
    Array<u8, 3> exponent { 0x01, 0x00, 0x01 };
    Array<u8, 4> modulus { 0xC0, 0x00, 0x00, 0x01 };
    auto x = p256_base_point_x();
    auto y = p256_base_point_y();
    // NB: The base point is the public key of the private value 1.
    auto d = p256_private_value_one();
    auto mldsa = mldsa_44_key();
    auto mlkem = mlkem_512_key();
    auto okp_public_key = ed25519_public_key();
    auto okp_private_key = ed25519_private_key();
    Array<ReadonlyBytes, 8> rsa_components { modulus, exponent, exponent, modulus, modulus, exponent, exponent, modulus };

    Vector<CryptoHandleGolden> goldens;
    goldens.append({ HandleTag::ByteBuffer, "secret"sv, "deriveKey"sv, alg_key_algorithm("HKDF"sv), handle_byte_buffer(buffer) });
    goldens.append({ HandleTag::RSAPublicKey, "public"sv, "verify"sv,
        alg_rsa_hashed_key_algorithm("RSASSA-PKCS1-v1_5"sv, 2048, exponent, "SHA-256"sv), handle_rsa_public_key(modulus, exponent) });
    goldens.append({ HandleTag::RSAPrivateKey, "private"sv, "sign"sv,
        alg_rsa_hashed_key_algorithm("RSASSA-PKCS1-v1_5"sv, 2048, exponent, "SHA-256"sv), handle_rsa_private_key(rsa_components) });
    goldens.append({ HandleTag::ECPublicKey, "public"sv, "verify"sv, alg_ec_key_algorithm("ECDSA"sv, "P-256"sv), handle_ec_public_key(x, y) });
    goldens.append({ HandleTag::ECPrivateKey, "private"sv, "sign"sv, alg_ec_key_algorithm("ECDSA"sv, "P-256"sv), handle_ec_private_key(d, {}, Array<ReadonlyBytes, 2> { x, y }) });
    goldens.append({ HandleTag::MLDSAPublicKey, "public"sv, "verify"sv, alg_key_algorithm("ML-DSA-44"sv), handle_single_buffer(5, mldsa.public_key) });
    goldens.append({ HandleTag::MLDSAPrivateKey, "private"sv, "sign"sv, alg_key_algorithm("ML-DSA-44"sv), handle_triple_buffer(6, mldsa.seed, mldsa.public_key, mldsa.expanded_key) });
    goldens.append({ HandleTag::MLKEMPublicKey, "public"sv, "encapsulateKey"sv, alg_key_algorithm("ML-KEM-512"sv), handle_single_buffer(7, mlkem.public_key) });
    goldens.append({ HandleTag::MLKEMPrivateKey, "private"sv, "decapsulateKey"sv, alg_key_algorithm("ML-KEM-512"sv), handle_triple_buffer(8, mlkem.seed, mlkem.public_key, mlkem.expanded_key) });
    goldens.append({ HandleTag::OKPPublicKey, "public"sv, "verify"sv, alg_key_algorithm("Ed25519"sv), handle_single_buffer(9, okp_public_key) });
    goldens.append({ HandleTag::OKPPrivateKey, "private"sv, "sign"sv, alg_key_algorithm("Ed25519"sv), handle_single_buffer(10, okp_private_key) });
    return goldens;
}

// Deserialize first so encode goldens use slot-consistent CryptoKeys.
inline GC::Ref<JS::Object> build_live_crypto_key(StringView type, StringView usage, Vector<u8> const& algorithm, Vector<u8> const& handle)
{
    auto value = MUST(principal_storage_deserialize(crypto_key_full_record(type, usage, algorithm, handle)));
    return value.as_object();
}

// Fabricate RsaKeyAlgorithm directly to pin the encode-only tag-1 branch.
inline GC::Ref<JS::Object> build_live_crypto_key_with_rsa_key_algorithm(StringView type, StringView usage, StringView name, u32 modulus_length, ReadonlyBytes public_exponent, ReadonlyBytes modulus)
{
    // NB: A stored record cannot hold an RsaKeyAlgorithm, so the key is decoded with an RsaHashedKeyAlgorithm and given
    //     the RsaKeyAlgorithm afterwards.
    auto key = build_live_crypto_key(type, usage, alg_rsa_hashed_key_algorithm(name, modulus_length, public_exponent, "SHA-256"sv), handle_rsa_public_key(modulus, public_exponent));
    unwrap_wrappable<Web::Crypto::CryptoKey>(key).remove_algorithm_hash_for_testing();
    return key;
}
