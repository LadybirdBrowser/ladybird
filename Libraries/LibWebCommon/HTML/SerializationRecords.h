/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCrypto/Forward.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Forward.h>
#include <LibIPC/Message.h>
#include <LibWebCommon/Export.h>

namespace Web::HTML {

// NB: References that are only meaningful inside the process that produced a serialization record, such as LibWeb's
//     same-process SharedArrayBuffer aliases. It is never IPC-encoded, and is opaque outside of LibWeb.
class WEBCOMMON_API SameProcessSerializationSideTable : public RefCounted<SameProcessSerializationSideTable> {
public:
    virtual ~SameProcessSerializationSideTable();
};

struct IPCSerializationRecord {
    IPC::MessageDataType data;

    // Same-process SharedArrayBuffer aliases. This is intentionally not IPC-encoded; cross-process
    // records fall back to the byte copy in `data`.
    RefPtr<SameProcessSerializationSideTable const> same_process_side_table;

    IPCSerializationRecord() = default;
    explicit IPCSerializationRecord(IPC::MessageDataType data)
        : data(move(data))
    {
    }
};

struct StorageSerializationRecord {
    AK_ALLOC_WITH_KMALLOC;

    ByteBuffer data;

    StorageSerializationRecord() = default;
    explicit StorageSerializationRecord(ByteBuffer data)
        : data(move(data))
    {
    }

    bool is_empty() const { return data.is_empty(); }

    bool operator==(StorageSerializationRecord const&) const = default;
};

class WEBCOMMON_API TransferDataEncoder {
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
    void encode_unsigned_big_integer(::Crypto::UnsignedBigInteger const&);

    IPC::MessageBuffer const& buffer() const;
    IPC::MessageBuffer take_buffer() const;

private:
    mutable IPC::MessageBuffer m_buffer;
    mutable bool m_buffer_has_been_taken { false };
    IPC::Encoder m_encoder;
};

struct SerializedTransferRecord {
    IPCSerializationRecord serialized;
    Vector<TransferDataEncoder> transfer_data_holders;
    // AD-HOC: Cross-process shared memory backing serialized SharedArrayBuffers — referenced by index from the main
    //         record (whose bytes can't carry file descriptors). Unlike the record's same-process side table, this one
    //         is IPC-encoded, so it reaches an agent in another process.
    Vector<Core::AnonymousBuffer> shared_buffers;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::IPCSerializationRecord const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::IPCSerializationRecord> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::StorageSerializationRecord const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::StorageSerializationRecord> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::TransferDataEncoder const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::TransferDataEncoder> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::SerializedTransferRecord const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::SerializedTransferRecord> decode(Decoder&);

}
