/*
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibGC/Forward.h>
#include <LibIPC/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

using DeserializationMemory = GC::RootVector<JS::Value>;
using SerializationMemory = HashMap<GC::Root<JS::Value>, u32>;

struct IPCSerializationRecord {
    IPC::MessageDataType data;

    IPCSerializationRecord() = default;
    explicit IPCSerializationRecord(IPC::MessageDataType data)
        : data(move(data))
    {
    }
};

struct StorageSerializationRecord {
    ByteBuffer data;

    StorageSerializationRecord() = default;
    explicit StorageSerializationRecord(ByteBuffer data)
        : data(move(data))
    {
    }

    bool is_empty() const { return data.is_empty(); }

    bool operator==(StorageSerializationRecord const&) const = default;
};

enum class SerializationType : u8 {
    IPC,
    Storage,
};

enum class CheckFullyConsumed : u8 {
    No,
    Yes,
};

enum class TransferType : u8 {
    Unknown = 0,
    MessagePort = 1,
    ArrayBuffer = 2,
    ResizableArrayBuffer = 3,
    ReadableStream = 4,
    WritableStream = 5,
    TransformStream = 6,
    ImageBitmap = 7,
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

// A storage-golden expectation, not a deserializer rule.
enum class Expectation : u8 {
    PositiveGolden,
    DocumentedReject,
};

struct ValueTagExpectation {
    ValueTag tag;
    Expectation expectation;
};

// Only tags listed here decode as values. Empty and EndObject are reserved.
static constexpr Array<ValueTagExpectation, 25> s_value_tag_expectations { {
    { ValueTag::UndefinedPrimitive, Expectation::PositiveGolden },
    { ValueTag::NullPrimitive, Expectation::PositiveGolden },
    { ValueTag::BooleanPrimitive, Expectation::PositiveGolden },
    { ValueTag::NumberPrimitive, Expectation::PositiveGolden },
    { ValueTag::StringPrimitive, Expectation::PositiveGolden },
    { ValueTag::BigIntPrimitive, Expectation::PositiveGolden },
    { ValueTag::BooleanObject, Expectation::PositiveGolden },
    { ValueTag::NumberObject, Expectation::PositiveGolden },
    { ValueTag::StringObject, Expectation::PositiveGolden },
    { ValueTag::BigIntObject, Expectation::PositiveGolden },
    { ValueTag::DateObject, Expectation::PositiveGolden },
    { ValueTag::RegExpObject, Expectation::PositiveGolden },
    { ValueTag::MapObject, Expectation::PositiveGolden },
    { ValueTag::SetObject, Expectation::PositiveGolden },
    { ValueTag::ArrayObject, Expectation::PositiveGolden },
    { ValueTag::ErrorObject, Expectation::PositiveGolden },
    { ValueTag::Object, Expectation::PositiveGolden },
    { ValueTag::ObjectReference, Expectation::PositiveGolden },
    { ValueTag::GrowableSharedArrayBuffer, Expectation::DocumentedReject },
    { ValueTag::SharedArrayBuffer, Expectation::DocumentedReject },
    { ValueTag::ResizeableArrayBuffer, Expectation::PositiveGolden },
    { ValueTag::ArrayBuffer, Expectation::PositiveGolden },
    { ValueTag::ArrayBufferView, Expectation::PositiveGolden },
    { ValueTag::SerializableObject, Expectation::PositiveGolden },
    { ValueTag::Int32Primitive, Expectation::PositiveGolden },
} };

static constexpr bool value_tag_is_decodable(ValueTag tag)
{
    for (auto const& entry : s_value_tag_expectations) {
        if (entry.tag == tag)
            return true;
    }
    return false;
}

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::IPCSerializationRecord const&);

template<>
WEB_API ErrorOr<Web::HTML::IPCSerializationRecord> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::StorageSerializationRecord const&);

template<>
WEB_API ErrorOr<Web::HTML::StorageSerializationRecord> decode(Decoder&);

}
