/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Result.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

// Structured serialize is an entirely different format from IPC because:
// - It contains representation of type information
// - It may contain circularities
// - It is restricted to JS values

namespace Web::HTML {

struct TransferDataHolder {
    Vector<u32> data;
    Vector<IPC::File> fds;
};

struct SerializedTransferRecord {
    SerializationRecord serialized;
    Vector<TransferDataHolder> transfer_data_holders;
};

struct DeserializedTransferRecord {
    JS::Value deserialized;
    Vector<GC::Root<JS::Object>> transferred_values;
};

struct DeserializedRecord {
    Optional<JS::Value> value;
    size_t position;
};

enum class TransferType : u8 {
    MessagePort,
    ArrayBuffer,
    ResizableArrayBuffer,
};

WebIDL::ExceptionOr<SerializationRecord> structured_serialize(JS::VM& vm, JS::Value);
WebIDL::ExceptionOr<SerializationRecord> structured_serialize_for_storage(JS::VM& vm, JS::Value);
WebIDL::ExceptionOr<SerializationRecord> structured_serialize_internal(JS::VM& vm, JS::Value, bool for_storage, SerializationMemory&);

WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM& vm, SerializationRecord const& serialized, JS::Realm& target_realm, Optional<DeserializationMemory> = {});
WebIDL::ExceptionOr<DeserializedRecord> structured_deserialize_internal(JS::VM& vm, ReadonlySpan<u32> const& serialized, JS::Realm& target_realm, DeserializationMemory& memory, Optional<size_t> position = {});

void serialize_boolean_primitive(SerializationRecord& serialized, JS::Value& value);
void serialize_number_primitive(SerializationRecord& serialized, JS::Value& value);
WebIDL::ExceptionOr<void> serialize_big_int_primitive(JS::VM& vm, SerializationRecord& serialized, JS::Value& value);
WebIDL::ExceptionOr<void> serialize_string_primitive(JS::VM& vm, SerializationRecord& serialized, JS::Value& value);
void serialize_boolean_object(SerializationRecord& serialized, JS::Value& value);
void serialize_number_object(SerializationRecord& serialized, JS::Value& value);
WebIDL::ExceptionOr<void> serialize_big_int_object(JS::VM& vm, SerializationRecord& serialized, JS::Value& value);
WebIDL::ExceptionOr<void> serialize_string_object(JS::VM& vm, SerializationRecord& serialized, JS::Value& value);
void serialize_date_object(SerializationRecord& serialized, JS::Value& value);
WebIDL::ExceptionOr<void> serialize_reg_exp_object(JS::VM& vm, SerializationRecord& serialized, JS::Value& value);

template<typename T>
requires(IsIntegral<T> || IsFloatingPoint<T>)
void serialize_primitive_type(SerializationRecord& serialized, T value)
{
    if constexpr (sizeof(T) < sizeof(u32)) {
        // NOTE: If the value is smaller than a u32, we can just store it directly.
        serialized.append(static_cast<u32>(value));
        return;
    }
    serialized.append(bit_cast<u32*>(&value), sizeof(T) / 4);
}

template<typename T>
requires(IsEnum<T>)
void serialize_enum(SerializationRecord& serialized, T value)
{
    serialize_primitive_type<UnderlyingType<T>>(serialized, to_underlying(value));
}

WebIDL::ExceptionOr<void> serialize_bytes(JS::VM& vm, Vector<u32>& vector, ReadonlyBytes bytes);
WebIDL::ExceptionOr<void> serialize_string(JS::VM& vm, Vector<u32>& vector, StringView);
WebIDL::ExceptionOr<void> serialize_string(JS::VM& vm, Vector<u32>& vector, String const& string);
WebIDL::ExceptionOr<void> serialize_string(JS::VM& vm, Vector<u32>& vector, JS::PrimitiveString const& primitive_string);
WebIDL::ExceptionOr<void> serialize_array_buffer(JS::VM& vm, Vector<u32>& vector, JS::ArrayBuffer const& array_buffer, bool for_storage);
template<OneOf<JS::TypedArrayBase, JS::DataView> ViewType>
WebIDL::ExceptionOr<void> serialize_viewed_array_buffer(JS::VM& vm, Vector<u32>& vector, ViewType const& view, bool for_storage, SerializationMemory& memory);

bool deserialize_boolean_primitive(ReadonlySpan<u32> const& serialized, size_t& position);
double deserialize_number_primitive(ReadonlySpan<u32> const& serialized, size_t& position);
GC::Ref<JS::BooleanObject> deserialize_boolean_object(JS::Realm& realm, ReadonlySpan<u32> const& serialized, size_t& position);
GC::Ref<JS::NumberObject> deserialize_number_object(JS::Realm& realm, ReadonlySpan<u32> const& serialized, size_t& position);
WebIDL::ExceptionOr<GC::Ref<JS::BigIntObject>> deserialize_big_int_object(JS::Realm& realm, ReadonlySpan<u32> const& serialized, size_t& position);
WebIDL::ExceptionOr<GC::Ref<JS::StringObject>> deserialize_string_object(JS::Realm& realm, ReadonlySpan<u32> const& serialized, size_t& position);
GC::Ref<JS::Date> deserialize_date_object(JS::Realm& realm, ReadonlySpan<u32> const& serialized, size_t& position);
WebIDL::ExceptionOr<GC::Ref<JS::RegExpObject>> deserialize_reg_exp_object(JS::Realm& realm, ReadonlySpan<u32> const& serialized, size_t& position);

template<typename T>
requires(IsIntegral<T> || IsFloatingPoint<T> || IsEnum<T>)
T deserialize_primitive_type(ReadonlySpan<u32> const& serialized, size_t& position)
{
    T value;
    // NOTE: Make sure we always round up, otherwise Ts that are less than 32 bit will end up with a size of 0.
    auto size = 1 + ((sizeof(value) - 1) / 4);
    VERIFY(position + size <= serialized.size());
    memcpy(&value, serialized.offset_pointer(position), sizeof(value));
    position += size;
    return value;
}

WebIDL::ExceptionOr<ByteBuffer> deserialize_bytes(JS::VM& vm, ReadonlySpan<u32> vector, size_t& position);
WebIDL::ExceptionOr<String> deserialize_string(JS::VM& vm, ReadonlySpan<u32> vector, size_t& position);
WebIDL::ExceptionOr<GC::Ref<JS::PrimitiveString>> deserialize_string_primitive(JS::VM& vm, ReadonlySpan<u32> vector, size_t& position);
WebIDL::ExceptionOr<GC::Ref<JS::BigInt>> deserialize_big_int_primitive(JS::VM& vm, ReadonlySpan<u32> vector, size_t& position);

WebIDL::ExceptionOr<SerializedTransferRecord> structured_serialize_with_transfer(JS::VM& vm, JS::Value value, Vector<GC::Root<JS::Object>> const& transfer_list);
WebIDL::ExceptionOr<DeserializedTransferRecord> structured_deserialize_with_transfer(JS::VM& vm, SerializedTransferRecord&);

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, ::Web::HTML::SerializedTransferRecord const&);

template<>
ErrorOr<void> encode(Encoder&, ::Web::HTML::TransferDataHolder const&);

template<>
ErrorOr<::Web::HTML::SerializedTransferRecord> decode(Decoder&);

template<>
ErrorOr<::Web::HTML::TransferDataHolder> decode(Decoder&);

}
