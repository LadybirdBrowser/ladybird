/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/MemoryStream.h>
#include <AK/Vector.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibJS/Forward.h>
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
        MUST(m_encoder.encode(value));
    }

    void append(SerializationRecord&&);
    void extend(Vector<TransferDataEncoder>);

    IPC::MessageBuffer const& buffer() const { return m_buffer; }
    IPC::MessageBuffer take_buffer() { return move(m_buffer); }

private:
    IPC::MessageBuffer m_buffer;
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

    WebIDL::ExceptionOr<ByteBuffer> decode_buffer(JS::Realm&);

private:
    IPC::MessageBuffer m_buffer;

    FixedMemoryStream m_stream;
    Queue<IPC::File> m_files;

    IPC::Decoder m_decoder;
};

struct SerializedTransferRecord {
    SerializationRecord serialized;
    Vector<TransferDataEncoder> transfer_data_holders;
};

struct DeserializedTransferRecord {
    JS::Value deserialized;
    Vector<GC::Root<JS::Object>> transferred_values;
};

WebIDL::ExceptionOr<SerializationRecord> structured_serialize(JS::VM&, JS::Value);
WebIDL::ExceptionOr<SerializationRecord> structured_serialize_for_storage(JS::VM&, JS::Value);
WebIDL::ExceptionOr<SerializationRecord> structured_serialize_internal(JS::VM&, JS::Value, bool for_storage, SerializationMemory&);

WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM&, SerializationRecord const&, JS::Realm&, Optional<DeserializationMemory> = {});
WebIDL::ExceptionOr<JS::Value> structured_deserialize_internal(JS::VM&, TransferDataDecoder&, JS::Realm&, DeserializationMemory&);

WEB_API WebIDL::ExceptionOr<SerializedTransferRecord> structured_serialize_with_transfer(JS::VM&, JS::Value, Vector<GC::Root<JS::Object>> const& transfer_list);
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
