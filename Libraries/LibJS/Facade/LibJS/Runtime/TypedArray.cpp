/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBufferABIConversions.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<TypedArrayBase> TypedArrayBase::create_from_slots(Realm& realm, Kind kind, ArrayBuffer& array_buffer, ByteLength array_length, ByteLength byte_length, u32 byte_offset)
{
    auto element_size = typed_array_element_size(kind);
    VERIFY(array_length.is_auto() == byte_length.is_auto());
    VERIFY(byte_offset % element_size == 0);
    if (!array_length.is_auto()) {
        Checked<u32> byte_length_of_array_length = array_length.length();
        byte_length_of_array_length *= element_size;
        VERIFY(!byte_length_of_array_length.has_overflow());
        VERIFY(byte_length_of_array_length.value() == byte_length.length());
    }

    auto* typed_array = js_typed_array_create_from_slots(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), to_underlying(kind), array_buffer_to_abi(array_buffer), byte_length_to_abi(array_length), byte_length_to_abi(byte_length), byte_offset);
    VERIFY(typed_array);
    return *cell_from_abi<TypedArrayBase>(typed_array);
}

ByteLength TypedArrayBase::byte_length() const
{
    return byte_length_from_abi(js_typed_array_byte_length(object_to_abi(*this)));
}

ArrayBuffer* TypedArrayBase::viewed_array_buffer() const
{
    return &array_buffer_from_abi(js_typed_array_viewed_array_buffer(object_to_abi(*this)));
}

static JSTypedArrayWithBufferWitness witness_record_to_abi(TypedArrayWithBufferWitness const& record)
{
    return {
        .typed_array = object_to_abi(*record.object),
        .cached_buffer_byte_length = byte_length_to_abi(record.cached_buffer_byte_length),
    };
}

TypedArrayWithBufferWitness make_typed_array_with_buffer_witness_record(TypedArrayBase const& typed_array, ArrayBuffer::Order order)
{
    auto record = js_typed_array_make_witness_record(object_to_abi(typed_array), order_to_abi(order));
    return {
        .object = *cell_from_abi<TypedArrayBase>(record.typed_array),
        .cached_buffer_byte_length = byte_length_from_abi(record.cached_buffer_byte_length),
    };
}

u32 typed_array_byte_length(TypedArrayWithBufferWitness const& typed_array_record)
{
    auto record = witness_record_to_abi(typed_array_record);
    return js_typed_array_byte_length_of_witness(&record);
}

u32 typed_array_length(TypedArrayWithBufferWitness const& typed_array_record)
{
    auto record = witness_record_to_abi(typed_array_record);
    return js_typed_array_length_of_witness(&record);
}

bool is_typed_array_out_of_bounds(TypedArrayWithBufferWitness const& typed_array_record)
{
    auto record = witness_record_to_abi(typed_array_record);
    return js_typed_array_is_out_of_bounds(&record);
}

ThrowCompletionOr<TypedArrayBase*> typed_array_from(VM& vm, Value typed_array_value)
{
    // ToObject of any other primitive is a new wrapper object, which is never a typed array.
    if (typed_array_value.is_nullish())
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefined);
    if (!typed_array_value.is_object() || !is<TypedArrayBase>(typed_array_value.as_object()))
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "TypedArray"sv);

    return &static_cast<TypedArrayBase&>(typed_array_value.as_object());
}

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type)                                                                                                         \
    ThrowCompletionOr<GC::Ref<ClassName>> ClassName::create(Realm& realm, u32 length)                                                                                                       \
    {                                                                                                                                                                                       \
        return completion_from_abi<GC::Ref<ClassName>>(js_typed_array_create(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), to_underlying(Kind::ClassName), length));                  \
    }                                                                                                                                                                                       \
                                                                                                                                                                                            \
    GC::Ref<ClassName> ClassName::create(Realm& realm, u32 length, ArrayBuffer& array_buffer)                                                                                               \
    {                                                                                                                                                                                       \
        auto* typed_array = js_typed_array_create_on_buffer(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), to_underlying(Kind::ClassName), length, array_buffer_to_abi(array_buffer)); \
        VERIFY(typed_array);                                                                                                                                                                \
        return *cell_from_abi<ClassName>(typed_array);                                                                                                                                      \
    }
JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

}
