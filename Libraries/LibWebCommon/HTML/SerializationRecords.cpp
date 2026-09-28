/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LEB128.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/HTML/SerializationRecords.h>

namespace Web::HTML {

SameProcessSerializationSideTable::~SameProcessSerializationSideTable() = default;

void append_storage_format_prologue(ByteBuffer& data)
{
    data.append(storage_format_magic.data(), storage_format_magic.size());
    LEB128<u64>::append_to(data, storage_format_version);
    LEB128<u64>::append_to(data, storage_format_flags);
}

static StorageSerializationRecord storage_serialization_record_for_primitive(ValueTag tag)
{
    ByteBuffer data;
    append_storage_format_prologue(data);
    data.append(to_underlying(tag));
    return StorageSerializationRecord { move(data) };
}

StorageSerializationRecord storage_serialization_record_for_undefined()
{
    return storage_serialization_record_for_primitive(ValueTag::UndefinedPrimitive);
}

StorageSerializationRecord storage_serialization_record_for_null()
{
    return storage_serialization_record_for_primitive(ValueTag::NullPrimitive);
}

TransferDataEncoder::TransferDataEncoder()
    : m_encoder(m_buffer)
{
}

TransferDataEncoder::TransferDataEncoder(IPC::MessageBuffer&& buffer)
    : m_buffer(move(buffer))
    , m_encoder(m_buffer)
{
}

IPC::MessageBuffer const& TransferDataEncoder::buffer() const
{
    return m_buffer;
}

IPC::MessageBuffer TransferDataEncoder::take_buffer() const
{
    VERIFY(!m_buffer_has_been_taken);
    m_buffer_has_been_taken = true;
    return move(m_buffer);
}

void TransferDataEncoder::append(IPCSerializationRecord&& record)
{
    VERIFY(!m_buffer_has_been_taken);
    MUST(m_buffer.append_data(record.data.data(), record.data.size()));
}

void TransferDataEncoder::encode_unsigned_big_integer(::Crypto::UnsignedBigInteger const& value)
{
    auto buffer = MUST(ByteBuffer::create_zeroed(value.byte_length()));
    auto written = value.export_data(buffer.bytes());
    VERIFY(written.size() == buffer.size());
    MUST(m_encoder.encode(buffer));
}

void TransferDataEncoder::extend(Vector<TransferDataEncoder> data_holders)
{
    for (auto& data_holder : data_holders)
        MUST(m_buffer.extend(data_holder.take_buffer()));
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::TransferDataEncoder const& data_holder)
{
    auto buffer = data_holder.take_buffer();
    auto data = buffer.take_data();
    auto attachments = buffer.take_attachments();

    TRY(encoder.encode(data));
    TRY(encoder.encode(static_cast<u32>(attachments.size())));
    for (auto& attachment : attachments)
        TRY(encoder.append_attachment(move(attachment)));

    return {};
}

template<>
ErrorOr<Web::HTML::TransferDataEncoder> decode(Decoder& decoder)
{
    auto data = TRY(decoder.decode<MessageDataType>());
    auto attachment_count = TRY(decoder.decode<u32>());

    Vector<Attachment> attachments;
    TRY(attachments.try_ensure_capacity(attachment_count));
    for (u32 i = 0; i < attachment_count; ++i)
        attachments.unchecked_append(TRY(decoder.attachments().try_dequeue()));

    IPC::MessageBuffer buffer { move(data), move(attachments) };
    return Web::HTML::TransferDataEncoder { move(buffer) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::IPCSerializationRecord const& record)
{
    // record.same_process_side_table is deliberately not encoded: it is a same-process aliasing side
    // table, so a record that crosses a process boundary arrives with an empty side table by
    // construction and deserializes SharedArrayBuffers from the self-contained byte copy in
    // record.data instead.
    TRY(encoder.encode(record.data));
    return {};
}

template<>
ErrorOr<Web::HTML::IPCSerializationRecord> decode(Decoder& decoder)
{
    auto data = TRY(decoder.decode<MessageDataType>());
    return Web::HTML::IPCSerializationRecord { move(data) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::StorageSerializationRecord const& record)
{
    TRY(encoder.encode(record.data));
    return {};
}

template<>
ErrorOr<Web::HTML::StorageSerializationRecord> decode(Decoder& decoder)
{
    auto data = TRY(decoder.decode<ByteBuffer>());
    return Web::HTML::StorageSerializationRecord { move(data) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::SerializedTransferRecord const& record)
{
    TRY(encoder.encode(record.serialized));
    TRY(encoder.encode(record.transfer_data_holders));
    TRY(encoder.encode(record.shared_buffers));
    return {};
}

template<>
ErrorOr<Web::HTML::SerializedTransferRecord> decode(Decoder& decoder)
{
    auto serialized = TRY(decoder.decode<Web::HTML::IPCSerializationRecord>());
    auto transfer_data_holders = TRY(decoder.decode<Vector<Web::HTML::TransferDataEncoder>>());
    auto shared_buffers = TRY(decoder.decode<Vector<Core::AnonymousBuffer>>());

    return Web::HTML::SerializedTransferRecord { move(serialized), move(transfer_data_holders), move(shared_buffers) };
}

}
