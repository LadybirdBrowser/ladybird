/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Error.h>
#include <AK/MemoryStream.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/TypeCasts.h>
#include <AK/Vector.h>
#include <LibCrypto/Forward.h>
#include <LibGC/Ptr.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/IntrinsicDefinitions.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

class PlatformObject;

}

namespace Web::HTML {

class StructuredSerializeDataDecoder;
class StructuredSerializeDataEncoder;

class WEB_API TransferDataEncoder {
public:
    explicit TransferDataEncoder();
    explicit TransferDataEncoder(IPC::MessageBuffer&&);

    template<typename T>
    ErrorOr<void> encode(T const& value)
    {
        VERIFY(!m_buffer_has_been_taken);
        return m_encoder.encode(value);
    }

    void append(IPCSerializationRecord&&);
    void extend(Vector<TransferDataEncoder>);

    IPC::MessageBuffer const& buffer() const;
    IPC::MessageBuffer take_buffer() const;

private:
    mutable IPC::MessageBuffer m_buffer;
    mutable bool m_buffer_has_been_taken { false };
    IPC::Encoder m_encoder;
};

class WEB_API TransferDataDecoder {
public:
    explicit TransferDataDecoder(IPCSerializationRecord const&);
    explicit TransferDataDecoder(TransferDataEncoder&&);

    template<typename T>
    ErrorOr<T> decode()
    {
        return m_decoder.decode<T>();
    }

private:
    IPC::MessageBuffer m_buffer;

    FixedMemoryStream m_stream;
    Queue<IPC::Attachment> m_attachments;

    IPC::Decoder m_decoder;
};

class WEB_API StructuredSerializeWriter {
public:
    static StructuredSerializeWriter create_ipc();
    static StructuredSerializeWriter create_storage();
    ~StructuredSerializeWriter();

    SerializationType type() const;

    template<typename T>
    void encode(T const&);

    void append(IPCSerializationRecord&&);

    IPCSerializationRecord take_ipc_record();
    StorageSerializationRecord take_storage_record();

private:
    explicit StructuredSerializeWriter(NonnullOwnPtr<StructuredSerializeDataEncoder>);

    NonnullOwnPtr<StructuredSerializeDataEncoder> m_encoder;
};

class WEB_API StructuredSerializeReader {
public:
    explicit StructuredSerializeReader(IPCSerializationRecord const&);
    explicit StructuredSerializeReader(StorageSerializationRecord const&);
    explicit StructuredSerializeReader(TransferDataEncoder&&);
    ~StructuredSerializeReader();

    SerializationType type() const;
    bool is_at_end() const;

    template<typename T>
    ErrorOr<T> decode();

private:
    NonnullOwnPtr<StructuredSerializeDataDecoder> m_decoder;
};

struct SerializedTransferRecord {
    IPCSerializationRecord serialized;
    Vector<TransferDataEncoder> transfer_data_holders;
};

struct DeserializedTransferRecord {
    JS::Value deserialized;
    Vector<GC::Root<JS::Object>> transferred_values;
};

WEB_API WebIDL::ExceptionOr<IPCSerializationRecord> structured_serialize(JS::VM&, JS::Value);
WEB_API WebIDL::ExceptionOr<StorageSerializationRecord> structured_serialize_for_storage(JS::VM&, JS::Value);
WebIDL::ExceptionOr<void> structured_serialize_internal(JS::VM&, StructuredSerializeWriter&, JS::Value, bool for_storage, SerializationMemory&);

WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM&, IPCSerializationRecord const&, JS::Realm&, Optional<DeserializationMemory> = {});
WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM&, StorageSerializationRecord const&, JS::Realm&, Optional<DeserializationMemory> = {});
WEB_API WebIDL::ExceptionOr<JS::Value> structured_deserialize_internal(JS::VM&, StructuredSerializeReader&, JS::Realm&, DeserializationMemory&, CheckFullyConsumed = CheckFullyConsumed::No);

WEB_API WebIDL::Exception data_clone_error_from_serialization_error(JS::Realm&, AK::Error const&);

void encode_unsigned_big_integer(StructuredSerializeWriter&, ::Crypto::UnsignedBigInteger const&);
WebIDL::ExceptionOr<::Crypto::UnsignedBigInteger> decode_unsigned_big_integer(StructuredSerializeReader&, JS::Realm&);

void encode_signed_big_integer(StructuredSerializeWriter&, ::Crypto::SignedBigInteger const&);
WebIDL::ExceptionOr<::Crypto::SignedBigInteger> decode_signed_big_integer(StructuredSerializeReader&, JS::Realm&);

template<typename T>
WebIDL::ExceptionOr<T> decode_or_throw_data_clone_error(JS::Realm& realm, StructuredSerializeReader& reader)
{
    auto result = reader.decode<T>();
    if (result.is_error())
        return data_clone_error_from_serialization_error(realm, result.release_error());
    return result.release_value();
}

// Decode stored text to UTF-8 and reject lone surrogates as DataCloneError.
WEB_API WebIDL::ExceptionOr<String> decode_utf8_text_or_throw_data_clone_error(JS::Realm& realm, StructuredSerializeReader& reader);

// Pairs each serializable interface with its storage identifier and empty-instance factory.
struct SerializableRegistryEntry {
    Bindings::InterfaceName interface_name;
    StringView identifier;
    GC::Ref<Bindings::PlatformObject> (*create)(JS::Realm&);
};

WEB_API ReadonlySpan<SerializableRegistryEntry> serializable_storage_registry();

template<typename T>
WebIDL::ExceptionOr<GC::Ref<T>> deserialize_nested_as(JS::VM& vm, StructuredSerializeReader& reader, JS::Realm& realm, DeserializationMemory& memory)
{
    auto value = TRY(structured_deserialize_internal(vm, reader, realm, memory));
    auto* object = value.is_object() ? as_if<T>(value.as_object()) : nullptr;
    if (!object)
        return data_clone_error_from_serialization_error(realm, AK::Error::from_string_literal("Nested serializable has an unexpected type"));
    return GC::Ref<T> { *object };
}

template<typename T>
WebIDL::ExceptionOr<void> encode_or_throw_data_clone_error(JS::Realm& realm, TransferDataEncoder& encoder, T const& value)
{
    auto result = encoder.encode(value);
    if (result.is_error())
        return data_clone_error_from_serialization_error(realm, result.release_error());
    return {};
}

template<typename T>
WebIDL::ExceptionOr<T> decode_or_throw_data_clone_error(JS::Realm& realm, TransferDataDecoder& decoder)
{
    auto result = decoder.decode<T>();
    if (result.is_error())
        return data_clone_error_from_serialization_error(realm, result.release_error());
    return result.release_value();
}

WEB_API WebIDL::ExceptionOr<SerializedTransferRecord> structured_serialize_with_transfer(JS::VM&, JS::Value, ReadonlySpan<GC::Ref<JS::Object>> transfer_list);
WebIDL::ExceptionOr<DeserializedTransferRecord> structured_deserialize_with_transfer(SerializedTransferRecord&, JS::Realm&);
WEB_API WebIDL::ExceptionOr<JS::Value> structured_deserialize_with_transfer_internal(TransferDataDecoder&, JS::Realm&);

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::TransferDataEncoder const&);

template<>
WEB_API ErrorOr<Web::HTML::TransferDataEncoder> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Web::HTML::SerializedTransferRecord const&);

template<>
ErrorOr<Web::HTML::SerializedTransferRecord> decode(Decoder&);

}
