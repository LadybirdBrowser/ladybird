/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayIterator.h>
#include <LibJS/Runtime/TypedArray.h>

namespace JS {

GC_DEFINE_ALLOCATOR(ArrayIterator);

// 23.1.5.1 CreateArrayIterator ( array, kind ), https://tc39.es/ecma262/#sec-createarrayiterator
GC::Ref<ArrayIterator> ArrayIterator::create(Realm& realm, Value array, Object::PropertyKind iteration_kind)
{
    // 1. Let iterator be OrdinaryObjectCreate(%ArrayIteratorPrototype%, « [[IteratedArrayLike]], [[ArrayLikeNextIndex]], [[ArrayLikeIterationKind]] »).
    // 2. Set iterator.[[IteratedArrayLike]] to array.
    // 3. Set iterator.[[ArrayLikeNextIndex]] to 0.
    // 4. Set iterator.[[ArrayLikeIterationKind]] to kind.
    // 5. Return iterator.
    return realm.create<ArrayIterator>(array, iteration_kind, realm.intrinsics().array_iterator_prototype());
}

ArrayIterator::ArrayIterator(Value array, Object::PropertyKind iteration_kind, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_array(array)
    , m_iteration_kind(iteration_kind)
{
}

void ArrayIterator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_array);
}

ThrowCompletionOr<void> ArrayIterator::next(VM& vm, bool& done, Value& value)
{
    // 1. Let O be the this value.
    // 2. If O is not an Object, throw a TypeError exception.
    // 3. If O does not have all of the internal slots of an Array Iterator Instance (23.1.5.3), throw a TypeError exception.

    // 4. Let array be O.[[IteratedArrayLike]].
    auto target_array = m_array;

    // 5. If array is undefined, return CreateIteratorResultObject(undefined, true).
    if (target_array.is_undefined()) {
        value = js_undefined();
        done = true;
        return {};
    }

    VERIFY(target_array.is_object());
    auto& array = target_array.as_object();

    // 6. Let index be O.[[ArrayLikeNextIndex]].
    auto index = m_index;

    // 7. Let kind be O.[[ArrayLikeIterationKind]].
    auto kind = m_iteration_kind;

    size_t length = 0;

    // 8. If array has a [[TypedArrayName]] internal slot, then
    if (array.is_typed_array()) {
        auto& typed_array = static_cast<TypedArrayBase&>(array);

        // a. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(array, SEQ-CST).
        auto typed_array_record = make_typed_array_with_buffer_witness_record(typed_array, ArrayBuffer::SeqCst);

        // b. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
        if (is_typed_array_out_of_bounds(typed_array_record))
            return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

        // c. Let len be TypedArrayLength(taRecord).
        length = typed_array_length(typed_array_record);
    }
    // 9. Else,
    else {
        // a. Let len be ? LengthOfArrayLike(array).
        length = TRY(length_of_array_like(vm, array));
    }

    // 10. If index ≥ len, then
    if (index >= length) {
        // a. Set O.[[IteratedArrayLike]] to undefined.
        m_array = js_undefined();

        // b. Return CreateIteratorResultObject(undefined, true).
        value = js_undefined();
        done = true;
        return {};
    }

    // 11. Set O.[[ArrayLikeNextIndex]] to index + 1.
    m_index++;

    // 12. Let indexNumber be 𝔽(index).

    Value result;

    // 13. If kind is KEY, then
    if (kind == PropertyKind::Key) {
        // a. Let result be indexNumber.
        result = Value { static_cast<i32>(index) };
    }
    // 14. Else,
    else {
        // a. Let elementKey be ! ToString(indexNumber).
        // b. Let elementValue be ? Get(array, elementKey).
        auto element_value = TRY([&]() -> ThrowCompletionOr<Value> {
            // OPTIMIZATION: For objects that don't interfere with indexed property access, we try looking directly at storage.
            if (!array.may_interfere_with_indexed_property_access() && array.indexed_properties().has_index(index)) {
                if (auto value = array.indexed_properties().get(index)->value; !value.is_accessor())
                    return value;
            }

            return array.get(index);
        }());

        // c. If kind is VALUE, then
        if (kind == PropertyKind::Value) {
            // i. Let result be elementValue.
            result = element_value;
        }
        // d. Else,
        else {
            // i. Assert: kind is KEY+VALUE.
            VERIFY(kind == PropertyKind::KeyAndValue);

            // ii. Let result be CreateArrayFromList(« indexNumber, elementValue »).
            result = Array::create_from(*vm.current_realm(), { Value(static_cast<i32>(index)), element_value });
        }
    }

    // 15. Return CreateIteratorResultObject(result, false).
    value = result;
    return {};
}

}
