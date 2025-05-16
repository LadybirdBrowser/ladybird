/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayPrototype.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

GC_DEFINE_ALLOCATOR(Array);

// 10.4.2.2 ArrayCreate ( length [ , proto ] ), https://tc39.es/ecma262/#sec-arraycreate
ThrowCompletionOr<GC::Ref<Array>> Array::create(Realm& realm, u64 length, Object* prototype)
{
    auto& vm = realm.vm();

    // 1. If length > 2^32 - 1, throw a RangeError exception.
    if (length > NumericLimits<u32>::max())
        return vm.throw_completion<RangeError>(ErrorType::InvalidLength, "array");

    // 2. If proto is not present, set proto to %Array.prototype%.
    if (!prototype)
        prototype = realm.intrinsics().array_prototype();

    // 3. Let A be MakeBasicObject(« [[Prototype]], [[Extensible]] »).
    // 4. Set A.[[Prototype]] to proto.
    // 5. Set A.[[DefineOwnProperty]] as specified in 10.4.2.1.
    auto array = realm.create<Array>(*prototype);

    // 6. Perform ! OrdinaryDefineOwnProperty(A, "length", PropertyDescriptor { [[Value]]: 𝔽(length), [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: false }).
    MUST(array->internal_define_own_property(vm.names.length, { .value = Value(length), .writable = true, .enumerable = false, .configurable = false }));

    // 7. Return A.
    return array;
}

// 7.3.18 CreateArrayFromList ( elements ), https://tc39.es/ecma262/#sec-createarrayfromlist
GC::Ref<Array> Array::create_from(Realm& realm, ReadonlySpan<Value> elements)
{
    // 1. Let array be ! ArrayCreate(0).
    auto array = MUST(Array::create(realm, 0));

    // 2. Let n be 0.
    // 3. For each element e of elements, do
    for (u32 n = 0; n < elements.size(); ++n) {
        // a. Perform ! CreateDataPropertyOrThrow(array, ! ToString(𝔽(n)), e).
        MUST(array->create_data_property_or_throw(n, elements[n]));

        // b. Set n to n + 1.
    }

    // 4. Return array.
    return array;
}

Array::Array(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
    m_has_magical_length_property = true;
}

// 10.4.2.4 ArraySetLength ( A, Desc ), https://tc39.es/ecma262/#sec-arraysetlength
ThrowCompletionOr<bool> Array::set_length(PropertyDescriptor const& property_descriptor)
{
    auto& vm = this->vm();

    // 1. If Desc does not have a [[Value]] field, then
    // a. Return ! OrdinaryDefineOwnProperty(A, "length", Desc).
    // 2. Let newLenDesc be a copy of Desc.
    // NOTE: Handled by step 16

    size_t new_length = indexed_properties().array_like_size();
    if (property_descriptor.value.has_value()) {
        // 3. Let newLen be ? ToUint32(Desc.[[Value]]).
        new_length = TRY(property_descriptor.value->to_u32(vm));
        // 4. Let numberLen be ? ToNumber(Desc.[[Value]]).
        auto number_length = TRY(property_descriptor.value->to_number(vm));
        // 5. If newLen is not the same value as numberLen, throw a RangeError exception.
        if (new_length != number_length.as_double())
            return vm.throw_completion<RangeError>(ErrorType::InvalidLength, "array");
    }

    // 6. Set newLenDesc.[[Value]] to newLen.
    // 7. Let oldLenDesc be OrdinaryGetOwnProperty(A, "length").
    // 8. Assert: IsDataDescriptor(oldLenDesc) is true.
    // 9. Assert: oldLenDesc.[[Configurable]] is false.
    // 10. Let oldLen be oldLenDesc.[[Value]].
    // 11. If newLen ≥ oldLen, then
    // a. Return ! OrdinaryDefineOwnProperty(A, "length", newLenDesc).
    // 12. If oldLenDesc.[[Writable]] is false, return false.
    // NOTE: Handled by step 16

    // 13. If newLenDesc does not have a [[Writable]] field or newLenDesc.[[Writable]] true, let newWritable be true.
    // 14. Else,
    // a. NOTE: Setting the [[Writable]] attribute to false is deferred in case any elements cannot be deleted.
    // b. Let newWritable be false.
    auto new_writable = property_descriptor.writable.value_or(true);

    // c. Set newLenDesc.[[Writable]] to true.
    // 15. Let succeeded be ! OrdinaryDefineOwnProperty(A, "length", newLenDesc).
    // 16. If succeeded is false, return false.
    // NOTE: Because the length property does not actually exist calling OrdinaryDefineOwnProperty
    // will result in unintended behavior, so instead we only implement here the small subset of
    // checks performed inside of it that would have mattered to us:

    // 10.1.6.3 ValidateAndApplyPropertyDescriptor ( O, P, extensible, Desc, current ), https://tc39.es/ecma262/#sec-validateandapplypropertydescriptor
    // 5. If current.[[Configurable]] is false, then
    // a. If Desc has a [[Configurable]] field and Desc.[[Configurable]] is true, return false.
    if (property_descriptor.configurable.has_value() && *property_descriptor.configurable)
        return false;
    // b. If Desc has an [[Enumerable]] field and SameValue(Desc.[[Enumerable]], current.[[Enumerable]]) is false, return false.
    if (property_descriptor.enumerable.has_value() && *property_descriptor.enumerable)
        return false;
    // c. If IsGenericDescriptor(Desc) is false and SameValue(IsAccessorDescriptor(Desc), IsAccessorDescriptor(current)) is false, return false.
    if (!property_descriptor.is_generic_descriptor() && property_descriptor.is_accessor_descriptor())
        return false;
    // NOTE: Step d. doesn't apply here.
    // e. Else if current.[[Writable]] is false, then
    if (!m_length_writable) {
        // i. If Desc has a [[Writable]] field and Desc.[[Writable]] is true, return false.
        if (property_descriptor.writable.has_value() && *property_descriptor.writable)
            return false;
        // ii. If Desc has a [[Value]] field and SameValue(Desc.[[Value]], current.[[Value]]) is false, return false.
        if (new_length != indexed_properties().array_like_size())
            return false;
    }

    // 17. For each own property key P of A that is an array index, whose numeric value is greater than or equal to newLen, in descending numeric index order, do
    // a. Let deleteSucceeded be ! A.[[Delete]](P).
    // b. If deleteSucceeded is false, then
    // i. Set newLenDesc.[[Value]] to ! ToUint32(P) + 1𝔽.
    bool success = indexed_properties().set_array_like_size(new_length);

    // ii. If newWritable is false, set newLenDesc.[[Writable]] to false.
    // iii. Perform ! OrdinaryDefineOwnProperty(A, "length", newLenDesc).
    // NOTE: Handled by step 18

    // 18. If newWritable is false, then
    // a. Set succeeded to ! OrdinaryDefineOwnProperty(A, "length", PropertyDescriptor { [[Writable]]: false }).
    // b. Assert: succeeded is true.
    if (!new_writable)
        m_length_writable = false;

    // NOTE: Continuation of step #17
    // iv. Return false.
    if (!success)
        return false;

    // 19. Return true.
    return true;
}

// 23.1.3.30.1 SortIndexedProperties ( obj, len, SortCompare, holes ), https://tc39.es/ecma262/#sec-sortindexedproperties
ThrowCompletionOr<GC::RootVector<Value>> sort_indexed_properties(VM& vm, Object const& object, size_t length, Function<ThrowCompletionOr<double>(Value, Value)> const& sort_compare, Holes holes)
{
    // 1. Let items be a new empty List.
    auto items = GC::RootVector<Value> { vm.heap() };

    // 2. Let k be 0.
    // 3. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        bool k_read;

        // b. If holes is skip-holes, then
        if (holes == Holes::SkipHoles) {
            // i. Let kRead be ? HasProperty(obj, Pk).
            k_read = TRY(object.has_property(property_key));
        }
        // c. Else,
        else {
            // i. Assert: holes is read-through-holes.
            VERIFY(holes == Holes::ReadThroughHoles);

            // ii. Let kRead be true.
            k_read = true;
        }

        // d. If kRead is true, then
        if (k_read) {
            // i. Let kValue be ? Get(obj, Pk).
            auto k_value = TRY(object.get(property_key));

            // ii. Append kValue to items.
            items.append(k_value);
        }

        // e. Set k to k + 1.
    }

    // 4. Sort items using an implementation-defined sequence of calls to SortCompare. If any such call returns an abrupt completion, stop before performing any further calls to SortCompare or steps in this algorithm and return that Completion Record.

    // Perform sorting by merge sort. This isn't as efficient compared to quick sort, but
    // quicksort can't be used in all cases because the spec requires Array.prototype.sort()
    // to be stable. FIXME: when initially scanning through the array, maintain a flag
    // for if an unstable sort would be indistinguishable from a stable sort (such as just
    // just strings or numbers), and in that case use quick sort instead for better performance.
    TRY(array_merge_sort(vm, sort_compare, items));

    // 5. Return items.
    return items;
}

// 23.1.3.30.2 CompareArrayElements ( x, y, comparefn ), https://tc39.es/ecma262/#sec-comparearrayelements
ThrowCompletionOr<double> compare_array_elements(VM& vm, Value x, Value y, FunctionObject* comparefn)
{
    // 1. If x and y are both undefined, return +0𝔽.
    if (x.is_undefined() && y.is_undefined())
        return 0;

    // 2. If x is undefined, return 1𝔽.
    if (x.is_undefined())
        return 1;

    // 3. If y is undefined, return -1𝔽.
    if (y.is_undefined())
        return -1;

    // 4. If comparefn is not undefined, then
    if (comparefn != nullptr) {
        // a. Let v be ? ToNumber(? Call(comparefn, undefined, « x, y »)).
        auto value = TRY(call(vm, comparefn, js_undefined(), x, y));
        auto value_number = TRY(value.to_number(vm));

        // b. If v is NaN, return +0𝔽.
        if (value_number.is_nan())
            return 0;

        // c. Return v.
        return value_number.as_double();
    }

    // 5. Let xString be ? ToString(x).
    auto x_string = PrimitiveString::create(vm, TRY(x.to_string(vm)));

    // 6. Let yString be ? ToString(y).
    auto y_string = PrimitiveString::create(vm, TRY(y.to_string(vm)));

    // 7. Let xSmaller be ! IsLessThan(xString, yString, true).
    auto x_smaller = MUST(is_less_than(vm, x_string, y_string, true));

    // 8. If xSmaller is true, return -1𝔽.
    if (x_smaller == TriState::True)
        return -1;

    // 9. Let ySmaller be ! IsLessThan(yString, xString, true).
    auto y_smaller = MUST(is_less_than(vm, y_string, x_string, true));

    // 10. If ySmaller is true, return 1𝔽.
    if (y_smaller == TriState::True)
        return 1;

    // 11. Return +0𝔽.
    return 0;
}

// NON-STANDARD: Used to return the value of the ephemeral length property
ThrowCompletionOr<Optional<PropertyDescriptor>> Array::internal_get_own_property(PropertyKey const& property_key) const
{
    auto& vm = this->vm();
    if (property_key.is_string() && property_key.as_string() == vm.names.length.as_string())
        return PropertyDescriptor { .value = Value(indexed_properties().array_like_size()), .writable = m_length_writable, .enumerable = false, .configurable = false };

    return Object::internal_get_own_property(property_key);
}

// 10.4.2.1 [[DefineOwnProperty]] ( P, Desc ), https://tc39.es/ecma262/#sec-array-exotic-objects-defineownproperty-p-desc
ThrowCompletionOr<bool> Array::internal_define_own_property(PropertyKey const& property_key, PropertyDescriptor const& property_descriptor, Optional<PropertyDescriptor>* precomputed_get_own_property)
{
    auto& vm = this->vm();

    // 1. If P is "length", then
    if (property_key.is_string() && property_key.as_string() == vm.names.length.as_string()) {
        // a. Return ? ArraySetLength(A, Desc).
        return set_length(property_descriptor);
    }

    // 2. Else if P is an array index, then
    if (property_key.is_number()) {
        // a. Let oldLenDesc be OrdinaryGetOwnProperty(A, "length").
        // b. Assert: IsDataDescriptor(oldLenDesc) is true.
        // c. Assert: oldLenDesc.[[Configurable]] is false.
        // d. Let oldLen be oldLenDesc.[[Value]].
        // e. Assert: oldLen is a non-negative integral Number.
        // f. Let index be ! ToUint32(P).

        // g. If index ≥ oldLen and oldLenDesc.[[Writable]] is false, return false.
        if (property_key.as_number() >= indexed_properties().array_like_size() && !m_length_writable)
            return false;

        // h. Let succeeded be ! OrdinaryDefineOwnProperty(A, P, Desc).
        auto succeeded = MUST(Object::internal_define_own_property(property_key, property_descriptor, precomputed_get_own_property));

        // i. If succeeded is false, return false.
        if (!succeeded)
            return false;

        // j. If index ≥ oldLen, then
        // i. Set oldLenDesc.[[Value]] to index + 1𝔽.
        // ii. Set succeeded to ! OrdinaryDefineOwnProperty(A, "length", oldLenDesc).
        // iii. Assert: succeeded is true.

        // k. Return true.
        return true;
    }

    // 3. Return ? OrdinaryDefineOwnProperty(A, P, Desc).
    return Object::internal_define_own_property(property_key, property_descriptor, precomputed_get_own_property);
}

// NON-STANDARD: Used to reject deletes to ephemeral (non-configurable) length property
ThrowCompletionOr<bool> Array::internal_delete(PropertyKey const& property_key)
{
    auto& vm = this->vm();
    if (property_key.is_string() && property_key.as_string() == vm.names.length.as_string())
        return false;
    return Object::internal_delete(property_key);
}

// NON-STANDARD: Used to inject the ephemeral length property's key
ThrowCompletionOr<GC::RootVector<Value>> Array::internal_own_property_keys() const
{
    auto& vm = this->vm();
    auto keys = TRY(Object::internal_own_property_keys());
    // FIXME: This is pretty expensive, find a better way to do this
    keys.insert(indexed_properties().real_size(), PrimitiveString::create(vm, vm.names.length.as_string()));
    return { move(keys) };
}

}
