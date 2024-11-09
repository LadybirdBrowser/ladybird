/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayIteratorPrototype.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/TypedArray.h>

namespace JS {

JS_DEFINE_ALLOCATOR(ArrayIteratorPrototype);

ArrayIteratorPrototype::ArrayIteratorPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().iterator_prototype())
{
}

void ArrayIteratorPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    define_native_function(realm, vm.names.next, next, 0, Attribute::Configurable | Attribute::Writable);

    // 23.1.5.2.2 %ArrayIteratorPrototype% [ @@toStringTag ], https://tc39.es/ecma262/#sec-%arrayiteratorprototype%-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Array Iterator"_string), Attribute::Configurable);
}

// 23.1.5.2.1 %ArrayIteratorPrototype%.next ( ), https://tc39.es/ecma262/#sec-%arrayiteratorprototype%.next
// FIXME: This seems to be CreateArrayIterator (https://tc39.es/ecma262/#sec-createarrayiterator) instead of %ArrayIteratorPrototype%.next.
JS_DEFINE_NATIVE_FUNCTION(ArrayIteratorPrototype::next)
{
    auto& realm = *vm.current_realm();

    auto iterator = TRY(typed_this_value(vm));
    auto target_array = iterator->array();
    if (target_array.is_undefined())
        return create_iterator_result_object(vm, js_undefined(), true);
    VERIFY(target_array.is_object());
    auto& array = target_array.as_object();

    auto index = iterator->index();
    auto iteration_kind = iterator->iteration_kind();

    size_t length;

    // i. If array has a [[TypedArrayName]] internal slot, then
    if (array.is_typed_array()) {
        auto& typed_array = static_cast<TypedArrayBase&>(array);

        // 1. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(array, seq-cst).
        auto typed_array_record = make_typed_array_with_buffer_witness_record(typed_array, ArrayBuffer::SeqCst);

        // 2. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
        if (is_typed_array_out_of_bounds(typed_array_record))
            return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

        // 3. Let len be TypedArrayLength(taRecord).
        length = typed_array_length(typed_array_record);
    }
    // ii. Else,
    else {
        // 1. Let len be ? LengthOfArrayLike(array).
        length = TRY(length_of_array_like(vm, array));
    }

    // iii. If index ≥ len, return NormalCompletion(undefined).
    if (index >= length) {
        iterator->m_array = js_undefined();
        return create_iterator_result_object(vm, js_undefined(), true);
    }

    // iv. Let indexNumber be 𝔽(index).

    Value result;

    // v. If kind is key, then
    if (iteration_kind == Object::PropertyKind::Key) {
        // 1. Let result be indexNumber.
        result = Value(static_cast<i32>(index));
    }
    // vi. Else,
    else {
        // 1. Let elementKey be ! ToString(indexNumber).
        // 2. Let elementValue be ? Get(array, elementKey).
        auto element_value = TRY([&]() -> ThrowCompletionOr<Value> {
            // OPTIMIZATION: For objects that don't interfere with indexed property access, we try looking directly at storage.
            if (!array.may_interfere_with_indexed_property_access() && array.indexed_properties().has_index(index)) {
                auto value = array.indexed_properties().get(index)->value;
                if (!value.is_accessor()) {
                    return value;
                }
            }

            return array.get(index);
        }());

        // 3. If kind is value, then
        if (iteration_kind == Object::PropertyKind::Value) {
            // a. Let result be elementValue.
            result = element_value;
        }
        // 4. Else,
        else {
            // a. Assert: kind is key+value.
            VERIFY(iteration_kind == Object::PropertyKind::KeyAndValue);

            // b. Let result be CreateArrayFromList(« indexNumber, elementValue »).
            result = Array::create_from(realm, { Value(static_cast<i32>(index)), element_value });
        }
    }

    // viii. Set index to index + 1.
    ++iterator->m_index;

    // vii. Perform ? GeneratorYield(CreateIterResultObject(result, false)).
    return create_iterator_result_object(vm, result, false);
}

}
