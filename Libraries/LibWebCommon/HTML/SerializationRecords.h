/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
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

enum class ValueTag : u8 {
    // These values are part of the stable storage serialization format.
    // Do not reorder or reuse values; leave removed tags reserved.
    Empty = 0, // Unused, for ease of catching bugs.

    UndefinedPrimitive = 1,
    NullPrimitive = 2,
    BooleanPrimitive = 3,
    NumberPrimitive = 4,
    StringPrimitive = 5,
    BigIntPrimitive = 6,

    BooleanObject = 7,
    NumberObject = 8,
    StringObject = 9,
    BigIntObject = 10,
    DateObject = 11,
    RegExpObject = 12,
    MapObject = 13,
    SetObject = 14,
    ArrayObject = 15,
    ErrorObject = 16,
    Object = 17,
    ObjectReference = 18,

    GrowableSharedArrayBuffer = 19,
    SharedArrayBuffer = 20,
    ResizeableArrayBuffer = 21,
    ArrayBuffer = 22,
    ArrayBufferView = 23,

    SerializableObject = 24,

    Int32Primitive = 25,

    // Object/Array property-list terminator, kept outside the value-tag range.
    EndObject = 0xFF,
};

// The on-disk version of the LBSC storage format. Bumping this is a deliberate wire change.
static constexpr u64 storage_format_version = 1;

static constexpr Array<u8, 4> storage_format_magic = { 'L', 'B', 'S', 'C' };

// Flags are for optional envelope features (e.g. compression); changes to the value encoding bump the version instead.
static constexpr u64 storage_format_flags = 0;

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

// Writes the magic, version, and flags that every storage serialization record begins with.
WEBCOMMON_API void append_storage_format_prologue(ByteBuffer&);

// NB: StructuredSerializeForStorage(undefined) and StructuredSerializeForStorage(null), for processes without a JS VM.
WEBCOMMON_API StorageSerializationRecord storage_serialization_record_for_undefined();
WEBCOMMON_API StorageSerializationRecord storage_serialization_record_for_null();

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
