/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibJS/Embedding/Layout.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/ByteLength.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

// A typed array of the Rust runtime, whose kind, element size, [[ArrayLength]] and [[ByteOffset]] are read in place.
class JS_API TypedArrayBase : public Object {
public:
    enum class Kind : u8 {
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    ClassName,
        JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
    };

    static bool is_engine_class_of(Object const& object) { return object.has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_IS_TYPED_ARRAY); }

    // Restores a view from its [[ArrayLength]], [[ByteLength]] and [[ByteOffset]], as StructuredDeserialize does. The
    // caller must already have checked that the view fits inside the buffer.
    static GC::Ref<TypedArrayBase> create_from_slots(Realm&, Kind, ArrayBuffer&, ByteLength array_length, ByteLength byte_length, u32 byte_offset);

    ByteLength const& array_length() const
    {
        return *reinterpret_cast<ByteLength const*>(reinterpret_cast<u8 const*>(this) + JS_LAYOUT_TYPED_ARRAY_ARRAY_LENGTH_OFFSET);
    }

    // The runtime keeps [[ByteLength]] where only it reads it, so this returns a copy.
    ByteLength byte_length() const;

    u32 byte_offset() const
    {
        static_assert(JS_LAYOUT_TYPED_ARRAY_BYTE_OFFSET_SIZE == sizeof(u32));
        u32 byte_offset;
        __builtin_memcpy(&byte_offset, reinterpret_cast<u8 const*>(this) + JS_LAYOUT_TYPED_ARRAY_BYTE_OFFSET_OFFSET, sizeof(byte_offset));
        return byte_offset;
    }

    ArrayBuffer* viewed_array_buffer() const;

    [[nodiscard]] Kind kind() const
    {
        static_assert(JS_LAYOUT_TYPED_ARRAY_KIND_SIZE == sizeof(Kind));
        return static_cast<Kind>(*(reinterpret_cast<u8 const*>(this) + JS_LAYOUT_TYPED_ARRAY_KIND_OFFSET));
    }

    u32 element_size() const
    {
        static_assert(JS_LAYOUT_TYPED_ARRAY_ELEMENT_SIZE_SIZE == sizeof(u8));
        return *(reinterpret_cast<u8 const*>(this) + JS_LAYOUT_TYPED_ARRAY_ELEMENT_SIZE_OFFSET);
    }

private:
    // array_length() reads the runtime's [[ArrayLength]] slot as a ByteLength, whose Variant<Auto, Detached, u32> keeps
    // the length first and then the index of its alternative, as the slot does.
    static_assert(sizeof(ByteLength) == 8 && alignof(ByteLength) == alignof(u32));
    static_assert(JS_LAYOUT_TYPED_ARRAY_ARRAY_LENGTH_SIZE == sizeof(u32));
    static_assert(JS_LAYOUT_TYPED_ARRAY_ARRAY_LENGTH_OFFSET % alignof(ByteLength) == 0);
    static_assert(JS_LAYOUT_TYPED_ARRAY_ARRAY_LENGTH_ALTERNATIVE_INDEX_OFFSET == JS_LAYOUT_TYPED_ARRAY_ARRAY_LENGTH_OFFSET + sizeof(u32));
    static_assert(JS_LAYOUT_TYPED_ARRAY_ARRAY_LENGTH_U32_ALTERNATIVE_INDEX == 2);
};

static_assert(to_underlying(TypedArrayBase::Kind::Uint8Array) == JS_LAYOUT_TYPED_ARRAY_KIND_UINT8);
static_assert(to_underlying(TypedArrayBase::Kind::Uint8ClampedArray) == JS_LAYOUT_TYPED_ARRAY_KIND_UINT8_CLAMPED);
static_assert(to_underlying(TypedArrayBase::Kind::Uint16Array) == JS_LAYOUT_TYPED_ARRAY_KIND_UINT16);
static_assert(to_underlying(TypedArrayBase::Kind::Uint32Array) == JS_LAYOUT_TYPED_ARRAY_KIND_UINT32);
static_assert(to_underlying(TypedArrayBase::Kind::BigUint64Array) == JS_LAYOUT_TYPED_ARRAY_KIND_BIG_UINT64);
static_assert(to_underlying(TypedArrayBase::Kind::Int8Array) == JS_LAYOUT_TYPED_ARRAY_KIND_INT8);
static_assert(to_underlying(TypedArrayBase::Kind::Int16Array) == JS_LAYOUT_TYPED_ARRAY_KIND_INT16);
static_assert(to_underlying(TypedArrayBase::Kind::Int32Array) == JS_LAYOUT_TYPED_ARRAY_KIND_INT32);
static_assert(to_underlying(TypedArrayBase::Kind::BigInt64Array) == JS_LAYOUT_TYPED_ARRAY_KIND_BIG_INT64);
static_assert(to_underlying(TypedArrayBase::Kind::Float16Array) == JS_LAYOUT_TYPED_ARRAY_KIND_FLOAT16);
static_assert(to_underlying(TypedArrayBase::Kind::Float32Array) == JS_LAYOUT_TYPED_ARRAY_KIND_FLOAT32);
static_assert(to_underlying(TypedArrayBase::Kind::Float64Array) == JS_LAYOUT_TYPED_ARRAY_KIND_FLOAT64);

// https://tc39.es/ecma262/#table-the-typedarray-constructors
constexpr u32 typed_array_element_size(TypedArrayBase::Kind kind)
{
    switch (kind) {
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    case TypedArrayBase::Kind::ClassName:                                           \
        return sizeof(Conditional<IsSame<ClampedU8, Type>, u8, Type>);
        JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
    }
    VERIFY_NOT_REACHED();
}

// https://tc39.es/ecma262/#table-the-typedarray-constructors
constexpr StringView typed_array_element_name(TypedArrayBase::Kind kind)
{
    switch (kind) {
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    case TypedArrayBase::Kind::ClassName:                                           \
        return #ClassName##sv;
        JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
    }
    VERIFY_NOT_REACHED();
}

// 10.4.5.9 TypedArray With Buffer Witness Records, https://tc39.es/ecma262/#sec-typedarray-with-buffer-witness-records
struct TypedArrayWithBufferWitness {
    GC::Ref<TypedArrayBase const> object; // [[Object]]
    ByteLength cached_buffer_byte_length; // [[CachedBufferByteLength]]
};

JS_API TypedArrayWithBufferWitness make_typed_array_with_buffer_witness_record(TypedArrayBase const&, ArrayBuffer::Order);
JS_API u32 typed_array_byte_length(TypedArrayWithBufferWitness const&);
JS_API u32 typed_array_length(TypedArrayWithBufferWitness const&);
JS_API bool is_typed_array_out_of_bounds(TypedArrayWithBufferWitness const&);

template<typename T>
class TypedArray : public TypedArrayBase {
};

JS_API ThrowCompletionOr<TypedArrayBase*> typed_array_from(VM&, Value);

#define JS_DECLARE_TYPED_ARRAY(ClassName, snake_name, PrototypeName, ConstructorName, Type)                                                            \
    class JS_API ClassName final : public TypedArray<Type> {                                                                                           \
    public:                                                                                                                                            \
        static bool is_engine_class_of(Object const& object)                                                                                           \
        {                                                                                                                                              \
            return TypedArrayBase::is_engine_class_of(object) && static_cast<TypedArrayBase const&>(object).kind() == TypedArrayBase::Kind::ClassName; \
        }                                                                                                                                              \
                                                                                                                                                       \
        static ThrowCompletionOr<GC::Ref<ClassName>> create(Realm&, u32 length);                                                                       \
        static GC::Ref<ClassName> create(Realm&, u32 length, ArrayBuffer& buffer);                                                                     \
    };                                                                                                                                                 \
    class ConstructorName final : public NativeFunction {                                                                                              \
    };

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    JS_DECLARE_TYPED_ARRAY(ClassName, snake_name, PrototypeName, ConstructorName, Type)
JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

}
