/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/MemoryStream.h>
#include <AK/Vector.h>
#include <LibCrypto/Forward.h>
#include <LibGC/RootVector.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/IntrinsicDefinitions.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

class WEB_API TransferDataEncoder {
public:
    explicit TransferDataEncoder();
    explicit TransferDataEncoder(IPC::MessageBuffer&&);

    template<typename T>
    void encode(T const& value)
    {
        VERIFY(!m_buffer_has_been_taken);
        MUST(m_encoder.encode(value));
    }

    void encode_unsigned_big_integer(::Crypto::UnsignedBigInteger const&);

    void append(SerializationRecord&&);
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
    explicit TransferDataDecoder(SerializationRecord const&);
    explicit TransferDataDecoder(TransferDataEncoder&&);

    template<typename T>
    T decode()
    {
        static_assert(!IsSame<T, ByteBuffer>, "Use decode_buffer to handle OOM");
        return MUST(m_decoder.decode<T>());
    }

    WebIDL::ExceptionOr<ByteBuffer> decode_buffer();
    WebIDL::ExceptionOr<::Crypto::UnsignedBigInteger> decode_unsigned_big_integer();

private:
    IPC::MessageBuffer m_buffer;

    FixedMemoryStream m_stream;
    Queue<IPC::Attachment> m_attachments;

    IPC::Decoder m_decoder;
};

struct SerializedTransferRecord {
    SerializationRecord serialized;
    Vector<TransferDataEncoder> transfer_data_holders;
};

struct StructuredSerializeOptions {
    GC::RootVector<GC::Ref<JS::Object>> transfer;
};

struct DeserializedTransferRecord {
    JS::Value deserialized;
    Vector<GC::Root<JS::Object>> transferred_values;
};

WebIDL::ExceptionOr<SerializationRecord> structured_serialize(JS::VM&, JS::Value);
WebIDL::ExceptionOr<SerializationRecord> structured_serialize(JS::Realm&, JS::Value);
WebIDL::ExceptionOr<SerializationRecord> structured_serialize_for_storage(JS::VM&, JS::Value);
WebIDL::ExceptionOr<SerializationRecord> structured_serialize_for_storage(JS::Realm&, JS::Value);
WebIDL::ExceptionOr<SerializationRecord> structured_serialize_internal(JS::Realm&, JS::Value, bool for_storage, SerializationMemory&);
WebIDL::ExceptionOr<SerializationRecord> structured_serialize_internal(JS::VM&, JS::Value, bool for_storage, SerializationMemory&);

WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM&, SerializationRecord const&, JS::Realm&, Optional<DeserializationMemory> = {});
WebIDL::ExceptionOr<JS::Value> structured_deserialize_internal(JS::VM&, TransferDataDecoder&, JS::Realm&, DeserializationMemory&);

WEB_API WebIDL::ExceptionOr<SerializedTransferRecord> structured_serialize_with_transfer(JS::Realm&, JS::Value, ReadonlySpan<GC::Ref<JS::Object>> transfer_list);
WebIDL::ExceptionOr<DeserializedTransferRecord> structured_deserialize_with_transfer(SerializedTransferRecord&, JS::Realm&);
WEB_API WebIDL::ExceptionOr<JS::Value> structured_deserialize_with_transfer_internal(TransferDataDecoder&, JS::Realm&);

}

namespace Web::Bindings {

class PlatformObject;
class Serializable;
class Transferable;

struct SerializablePlatformObject {
    Serializable* serializable { nullptr };
    InterfaceName interface_name;
    GC::Ptr<JS::Realm> realm;
};

WEB_API Transferable* transferable_from_object(JS::Object&);
WEB_API Optional<SerializablePlatformObject> serializable_from_object(JS::Object&);
WEB_API bool is_platform_object(JS::Object const&);
WEB_API GC::Ref<PlatformObject> create_serialized_platform_object(InterfaceName, JS::Realm&);
WEB_API WebIDL::ExceptionOr<GC::Ref<PlatformObject>> create_transferred_platform_object(HTML::TransferType, JS::Realm&, HTML::TransferDataDecoder&);

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
