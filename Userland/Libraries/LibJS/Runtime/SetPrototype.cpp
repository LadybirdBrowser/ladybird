/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/KeyedCollections.h>
#include <LibJS/Runtime/SetIterator.h>
#include <LibJS/Runtime/SetPrototype.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

JS_DEFINE_ALLOCATOR(SetPrototype);

SetPrototype::SetPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void SetPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;

    define_native_function(realm, vm.names.add, add, 1, attr);
    define_native_function(realm, vm.names.clear, clear, 0, attr);
    define_native_function(realm, vm.names.delete_, delete_, 1, attr);
    define_native_function(realm, vm.names.entries, entries, 0, attr);
    define_native_function(realm, vm.names.forEach, for_each, 1, attr);
    define_native_function(realm, vm.names.has, has, 1, attr);
    define_native_function(realm, vm.names.values, values, 0, attr);
    define_native_function(realm, vm.names.union_, union_, 1, attr);
    define_native_function(realm, vm.names.intersection, intersection, 1, attr);
    define_native_function(realm, vm.names.difference, difference, 1, attr);
    define_native_function(realm, vm.names.symmetricDifference, symmetric_difference, 1, attr);
    define_native_function(realm, vm.names.isSubsetOf, is_subset_of, 1, attr);
    define_native_function(realm, vm.names.isSupersetOf, is_superset_of, 1, attr);
    define_native_function(realm, vm.names.isDisjointFrom, is_disjoint_from, 1, attr);
    define_native_accessor(realm, vm.names.size, size_getter, {}, Attribute::Configurable);

    define_direct_property(vm.names.keys, get_without_side_effects(vm.names.values), attr);

    // 24.2.3.11 Set.prototype [ @@iterator ] ( ), https://tc39.es/ecma262/#sec-set.prototype-@@iterator
    define_direct_property(vm.well_known_symbol_iterator(), get_without_side_effects(vm.names.values), attr);

    // 24.2.3.12 Set.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-set.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.Set.as_string()), Attribute::Configurable);
}

// 24.2.3.1 Set.prototype.add ( value ), https://tc39.es/ecma262/#sec-set.prototype.add
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::add)
{
    auto value = vm.argument(0);

    // 1. Let S be the this value.
    // 2. Perform ? RequireInternalSlot(S, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Set value to CanonicalizeKeyedCollectionKey(value).
    value = canonicalize_keyed_collection_key(value);

    // 4. For each element e of S.[[SetData]], do
    //     a. If e is not empty and SameValue(e, value) is true, then
    //         i. Return S.
    // 5. Append value to S.[[SetData]].
    set->set_add(value);

    // 6. Return S.
    return set;
}

// 24.2.3.2 Set.prototype.clear ( ), https://tc39.es/ecma262/#sec-set.prototype.clear
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::clear)
{
    // 1. Let S be the this value.
    // 2. Perform ? RequireInternalSlot(S, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. For each element e of S.[[SetData]], do
    //     a. Replace the element of S.[[SetData]] whose value is e with an element whose value is empty.
    set->set_clear();

    // 4. Return undefined.
    return js_undefined();
}

// 24.2.3.4 Set.prototype.delete ( value ), https://tc39.es/ecma262/#sec-set.prototype.delete
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::delete_)
{
    auto value = vm.argument(0);

    // 1. Let S be the this value.
    // 2. Perform ? RequireInternalSlot(S, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Set value to CanonicalizeKeyedCollectionKey(value).
    value = canonicalize_keyed_collection_key(value);

    // 4. For each element e of S.[[SetData]], do
    //     a. If e is not empty and SameValue(e, value) is true, then
    //         i. Replace the element of S.[[SetData]] whose value is e with an element whose value is empty.
    //         ii. Return true.
    // 5. Return false.
    return Value(set->set_remove(value));
}

// 24.2.3.5 Set.prototype.entries ( ), https://tc39.es/ecma262/#sec-set.prototype.entries
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::entries)
{
    auto& realm = *vm.current_realm();

    // 1. Let S be the this value.
    auto set = TRY(typed_this_object(vm));

    // 2. Return ? CreateSetIterator(S, key+value).
    return SetIterator::create(realm, set, Object::PropertyKind::KeyAndValue);
}

// 24.2.3.6 Set.prototype.forEach ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-set.prototype.foreach
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::for_each)
{
    auto callback_fn = vm.argument(0);
    auto this_arg = vm.argument(1);

    // 1. Let S be the this value.
    // 2. Perform ? RequireInternalSlot(S, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
    if (!callback_fn.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, vm.argument(0).to_string_without_side_effects());

    // 4. Let entries be S.[[SetData]].
    // 5. Let numEntries be the number of elements in entries.
    // 6. Let index be 0.
    // 7. Repeat, while index < numEntries,
    for (auto& entry : *set) {
        // a. Let e be entries[index].
        // b. Set index to index + 1.
        // c. If e is not empty, then
        // NOTE: This is handled in Map::IteratorImpl.

        // i. Perform ? Call(callbackfn, thisArg, « e, e, S »).
        TRY(call(vm, callback_fn.as_function(), this_arg, entry.key, entry.key, set));

        // ii. NOTE: The number of elements in entries may have increased during execution of callbackfn.
        // iii. Set numEntries to the number of elements in entries.
        // NOTE: This is handled in Map::IteratorImpl.
    }

    // 8. Return undefined.
    return js_undefined();
}

// 24.2.3.7 Set.prototype.has ( value ), https://tc39.es/ecma262/#sec-set.prototype.has
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::has)
{
    auto value = vm.argument(0);

    // 1. Let S be the this value.
    // 2. Perform ? RequireInternalSlot(S, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Set value to CanonicalizeKeyedCollectionKey(value).
    value = canonicalize_keyed_collection_key(value);

    // 4. For each element e of S.[[SetData]], do
    //     a. If e is not empty and SameValue(e, value) is true, return true.
    // 5. Return false.
    return Value(set->set_has(value));
}

// 24.2.3.10 Set.prototype.values ( ), https://tc39.es/ecma262/#sec-set.prototype.values
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::values)
{
    auto& realm = *vm.current_realm();

    // 1. Let S be the this value.
    // NOTE: CreateSetIterator checks the presence of a [[SetData]] slot, so we can do this here.
    auto set = TRY(typed_this_object(vm));

    // 2. Return ? CreateSetIterator(S, value).
    return SetIterator::create(realm, set, Object::PropertyKind::Value);
}

// 24.2.3.9 get Set.prototype.size, https://tc39.es/ecma262/#sec-get-set.prototype.size
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::size_getter)
{
    // 1. Let S be the this value.
    // 2. Perform ? RequireInternalSlot(S, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let count be 0.
    // 4. For each element e of S.[[SetData]], do
    //     a. If e is not empty, set count to count + 1.
    auto count = set->set_size();

    // 5. Return 𝔽(count).
    return Value(count);
}

// 8 Set Records, https://tc39.es/proposal-set-methods/#sec-set-records
struct SetRecord {
    NonnullGCPtr<Object const> set;    // [[Set]]
    double size { 0 };                 // [[Size]
    NonnullGCPtr<FunctionObject> has;  // [[Has]]
    NonnullGCPtr<FunctionObject> keys; // [[Keys]]
};

// 9 GetSetRecord ( obj ), https://tc39.es/proposal-set-methods/#sec-getsetrecord
static ThrowCompletionOr<SetRecord> get_set_record(VM& vm, Value value)
{
    // 1. If obj is not an Object, throw a TypeError exception.
    if (!value.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, value.to_string_without_side_effects());
    auto const& object = value.as_object();

    // 2. Let rawSize be ? Get(obj, "size").
    auto raw_size = TRY(object.get(vm.names.size));

    // 3. Let numSize be ? ToNumber(rawSize).
    auto number_size = TRY(raw_size.to_number(vm));

    // 4. NOTE: If rawSize is undefined, then numSize will be NaN.
    // 5. If numSize is NaN, throw a TypeError exception.
    if (number_size.is_nan())
        return vm.throw_completion<TypeError>(ErrorType::NumberIsNaN, "size"sv);

    // 6. Let intSize be ! ToIntegerOrInfinity(numSize).
    auto integer_size = MUST(number_size.to_integer_or_infinity(vm));

    // 7. If intSize < 0, throw a RangeError exception.
    if (integer_size < 0)
        return vm.throw_completion<RangeError>(ErrorType::NumberIsNegative, "size"sv);

    // 8. Let has be ? Get(obj, "has").
    auto has = TRY(object.get(vm.names.has));

    // 9. If IsCallable(has) is false, throw a TypeError exception.
    if (!has.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, has.to_string_without_side_effects());

    // 10. Let keys be ? Get(obj, "keys").
    auto keys = TRY(object.get(vm.names.keys));

    // 11. If IsCallable(keys) is false, throw a TypeError exception.
    if (!keys.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, keys.to_string_without_side_effects());

    // 12. Return a new Set Record { [[Set]]: obj, [[Size]]: intSize, [[Has]]: has, [[Keys]]: keys }.
    return SetRecord { .set = object, .size = integer_size, .has = has.as_function(), .keys = keys.as_function() };
}

// 1 Set.prototype.union ( other ), https://tc39.es/proposal-set-methods/#sec-set.prototype.union
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::union_)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let otherRec be ? GetSetRecord(other).
    auto other_record = TRY(get_set_record(vm, vm.argument(0)));

    // 4. Let keysIter be ? GetIteratorFromMethod(otherRec.[[Set]], otherRec.[[Keys]]).
    auto keys_iterator = TRY(get_iterator_from_method(vm, other_record.set, other_record.keys));

    // 5. Let resultSetData be a copy of O.[[SetData]].
    auto result = set->copy();

    // 6. Let next be true.
    auto next = true;

    // 7. Repeat, while next is not false,
    while (next) {
        // a. Set next to ? IteratorStep(keysIter).
        auto iterator_result = TRY(iterator_step(vm, keys_iterator));
        next = iterator_result;

        // b. If next is not false, then
        if (next) {
            // i. Let nextValue be ? IteratorValue(next).
            auto next_value = TRY(iterator_value(vm, *iterator_result));

            // ii. If nextValue is -0𝔽, set nextValue to +0𝔽.
            if (next_value.is_negative_zero())
                next_value = Value(0);

            // iii. If SetDataHas(resultSetData, nextValue) is false, then
            //     1. Append nextValue to resultSetData.
            result->set_add(next_value);
        }
    }

    // 8. Let result be OrdinaryObjectCreate(%Set.prototype%, « [[SetData]] »).
    // 9. Set result.[[SetData]] to resultSetData.

    // 10. Return result.
    return result;
}

// 2 Set.prototype.intersection ( other ), https://tc39.es/proposal-set-methods/#sec-set.prototype.intersection
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::intersection)
{
    auto& realm = *vm.current_realm();

    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let otherRec be ? GetSetRecord(other).
    auto other_record = TRY(get_set_record(vm, vm.argument(0)));

    // 4. Let resultSetData be a new empty List.
    auto result = Set::create(realm);

    // 5. Let thisSize be the number of elements in O.[[SetData]].
    auto this_size = set->set_size();

    // 6. If thisSize ≤ otherRec.[[Size]], then
    if (this_size <= other_record.size) {
        // a. For each element e of O.[[SetData]], do
        for (auto& element : *set) {
            // i. If e is not empty, then
            //     1. Let inOther be ToBoolean(? Call(otherRec.[[Has]], otherRec.[[Set]], « e »)).
            auto in_other = TRY(call(vm, *other_record.has, other_record.set, element.key)).to_boolean();
            //     2. If inOther is true, then
            if (in_other) {
                // a. Append e to resultSetData.
                result->set_add(element.key);
            }
        }
    }
    // 7. Else,
    else {
        // a. Let keysIter be ? GetIteratorFromMethod(otherRec.[[Set]], otherRec.[[Keys]]).
        auto keys_iterator = TRY(get_iterator_from_method(vm, other_record.set, other_record.keys));

        // b. Let next be true.
        auto next = true;

        // c. Repeat, while next is not false,
        while (next) {
            // i. Set next to ? IteratorStep(keysIter).
            auto iterator_result = TRY(iterator_step(vm, keys_iterator));
            next = iterator_result;

            // ii. If next is not false, then
            if (next) {
                // 1. Let nextValue be ? IteratorValue(next).
                auto next_value = TRY(iterator_value(vm, *iterator_result));

                // 2. If nextValue is -0𝔽, set nextValue to +0𝔽.
                if (next_value.is_negative_zero())
                    next_value = Value(0);

                // 3. NOTE: Because other is an arbitrary object, it is possible for its "keys" iterator to produce the same value more than once.
                // 4. Let alreadyInResult be SetDataHas(resultSetData, nextValue).
                // 5. Let inThis be SetDataHas(O.[[SetData]], nextValue).
                auto in_this = set->set_has(next_value);
                // 6. If alreadyInResult is false and inThis is true, then
                if (in_this) {
                    // a. Append nextValue to resultSetData.
                    result->set_add(next_value);
                }
            }
        }

        // d. NOTE: It is possible for resultSetData not to be a subset of O.[[SetData]] at this point because arbitrary code may have been executed by the iterator, including code which modifies O.[[SetData]].

        // e. Sort the elements of resultSetData so that all elements which are also in O.[[SetData]] are ordered as they are in O.[[SetData]], and any additional elements are moved to the end of the list in the same order as they were before sorting resultSetData.
        // FIXME: This is not possible with the current underlying m_values implementation
    }

    // 8. Let result be OrdinaryObjectCreate(%Set.prototype%, « [[SetData]] »).
    // 9. Set result.[[SetData]] to resultSetData.

    // 10. Return result.
    return result;
}

// 3 Set.prototype.difference ( other ), https://tc39.es/proposal-set-methods/#sec-set.prototype.difference
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::difference)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let otherRec be ? GetSetRecord(other).
    auto other_record = TRY(get_set_record(vm, vm.argument(0)));

    // 4. Let resultSetData be a copy of O.[[SetData]].
    auto result = set->copy();

    // 5. Let thisSize be the number of elements in O.[[SetData]].
    auto this_size = set->set_size();

    // 6. If thisSize ≤ otherRec.[[Size]], then
    if (this_size <= other_record.size) {
        // a. For each element e of resultSetData, do
        for (auto& element : *set) {
            // i. If e is not empty, then
            // 1.     Let inOther be ToBoolean(? Call(otherRec.[[Has]], otherRec.[[Set]], « e »)).
            auto in_other = TRY(call(vm, *other_record.has, other_record.set, element.key)).to_boolean();
            // 2.     If inOther is true, then
            if (in_other) {
                // a. Remove e from resultSetData.
                result->set_remove(element.key);
            }
        }
    }
    // 7. Else,
    else {
        // a. Let keysIter be ? GetIteratorFromMethod(otherRec.[[Set]], otherRec.[[Keys]]).
        auto keys_iterator = TRY(get_iterator_from_method(vm, other_record.set, other_record.keys));

        // b. Let next be true.
        auto next = true;

        // c. Repeat, while next is not false,
        while (next) {
            // i. Set next to ? IteratorStep(keysIter).
            auto iterator_result = TRY(iterator_step(vm, keys_iterator));
            next = iterator_result;

            // ii. If next is not false, then
            if (next) {
                // 1. Let nextValue be ? IteratorValue(next).
                auto next_value = TRY(iterator_value(vm, *iterator_result));

                // 2. If nextValue is -0𝔽, set nextValue to +0𝔽.
                if (next_value.is_negative_zero())
                    next_value = Value(0);

                // 3. If SetDataHas(resultSetData, nextValue) is true, then
                if (result->set_has(next_value)) {
                    // a. Remove nextValue from resultSetData.
                    result->set_remove(next_value);
                }
            }
        }
    }

    // 8. Let result be OrdinaryObjectCreate(%Set.prototype%, « [[SetData]] »).
    // 9. Set result.[[SetData]] to resultSetData.

    // 10. Return result.
    return result;
}

// 4 Set.prototype.symmetricDifference ( other ), https://tc39.es/proposal-set-methods/#sec-set.prototype.symmetricdifference
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::symmetric_difference)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let otherRec be ? GetSetRecord(other).
    auto other_record = TRY(get_set_record(vm, vm.argument(0)));

    // 4. Let keysIter be ? GetIteratorFromMethod(otherRec.[[Set]], otherRec.[[Keys]]).
    auto keys_iterator = TRY(get_iterator_from_method(vm, other_record.set, other_record.keys));

    // 5. Let resultSetData be a copy of O.[[SetData]].
    auto result = set->copy();

    // 6. Let next be true.
    auto next = true;

    // 7. Repeat, while next is not false,
    while (next) {
        // a. Set next to ? IteratorStep(keysIter).
        auto iterator_result = TRY(iterator_step(vm, keys_iterator));
        next = iterator_result;

        // b. If next is not false, then
        if (next) {
            // i. Let nextValue be ? IteratorValue(next).
            auto next_value = TRY(iterator_value(vm, *iterator_result));

            // ii. If nextValue is -0𝔽, set nextValue to +0𝔽.
            if (next_value.is_negative_zero())
                next_value = Value(0);

            // iii. Let inResult be SetDataHas(resultSetData, nextValue).
            // iv. If SetDataHas(O.[[SetData]], nextValue) is true, then
            if (set->set_has(next_value)) {
                // 1. If inResult is true, remove nextValue from resultSetData.
                result->set_remove(next_value);
            }
            // v. Else,
            else {
                // 1. If inResult is false, append nextValue to resultSetData.
                result->set_add(next_value);
            }
        }
    }

    // 8. Let result be OrdinaryObjectCreate(%Set.prototype%, « [[SetData]] »).
    // 9. Set result.[[SetData]] to resultSetData.

    // 10. Return result.
    return result;
}

// 5 Set.prototype.isSubsetOf ( other ), https://tc39.es/proposal-set-methods/#sec-set.prototype.issubsetof
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::is_subset_of)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let otherRec be ? GetSetRecord(other).
    auto other_record = TRY(get_set_record(vm, vm.argument(0)));

    // 4. Let thisSize be the number of elements in O.[[SetData]].
    auto this_size = set->set_size();

    // 5. If thisSize > otherRec.[[Size]], return false.
    if (this_size > other_record.size)
        return false;

    // 6. For each element e of O.[[SetData]], do
    for (auto& element : *set) {
        // a. Let inOther be ToBoolean(? Call(otherRec.[[Has]], otherRec.[[Set]], « e »)).
        auto in_other = TRY(call(vm, *other_record.has, other_record.set, element.key)).to_boolean();

        // b. If inOther is false, return false.
        if (!in_other)
            return false;
    }

    // 7. Return true.
    return true;
}

// 6 Set.prototype.isSupersetOf ( other ), https://tc39.es/proposal-set-methods/#sec-set.prototype.issupersetof
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::is_superset_of)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let otherRec be ? GetSetRecord(other).
    auto other_record = TRY(get_set_record(vm, vm.argument(0)));

    // 4. Let thisSize be the number of elements in O.[[SetData]].
    auto this_size = set->set_size();

    // 5. If thisSize < otherRec.[[Size]], return false.
    if (this_size < other_record.size)
        return false;

    // 6. Let keysIter be ? GetIteratorFromMethod(otherRec.[[Set]], otherRec.[[Keys]]).
    auto keys_iterator = TRY(get_iterator_from_method(vm, other_record.set, other_record.keys));

    // 7. Let next be true.
    auto next = true;

    // 8. Repeat, while next is not false,
    while (next) {
        // a. Set next to ? IteratorStep(keysIter).
        auto iterator_result = TRY(iterator_step(vm, keys_iterator));
        next = iterator_result;

        // b. If next is not false, then
        if (next) {
            // i. Let nextValue be ? IteratorValue(next).
            auto next_value = TRY(iterator_value(vm, *iterator_result));

            // ii. If SetDataHas(O.[[SetData]], nextValue) is false, return false.
            if (!set->set_has(next_value))
                return false;
        }
    }

    // 9. Return true.
    return true;
}

// 7 Set.prototype.isDisjointFrom ( other ), https://tc39.es/proposal-set-methods/#sec-set.prototype.isdisjointfrom
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::is_disjoint_from)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let otherRec be ? GetSetRecord(other).
    auto other_record = TRY(get_set_record(vm, vm.argument(0)));

    // 4. Let thisSize be the number of elements in O.[[SetData]].
    auto this_size = set->set_size();

    // 5. If thisSize ≤ otherRec.[[Size]], then
    if (this_size <= other_record.size) {
        // a. For each element e of O.[[SetData]], do
        for (auto& element : *set) {
            // i. If e is not empty, then
            //     1. Let inOther be ToBoolean(? Call(otherRec.[[Has]], otherRec.[[Set]], « e »)).
            auto in_other = TRY(call(vm, *other_record.has, other_record.set, element.key)).to_boolean();
            //     2. If inOther is true, return false.
            if (in_other)
                return false;
        }
    }
    // 6. Else,
    else {
        // a. Let keysIter be ? GetIteratorFromMethod(otherRec.[[Set]], otherRec.[[Keys]]).
        auto keys_iterator = TRY(get_iterator_from_method(vm, other_record.set, other_record.keys));

        // b. Let next be true.
        auto next = true;

        // c. Repeat, while next is not false,
        while (next) {
            // i. Set next to ? IteratorStep(keysIter).
            auto iterator_result = TRY(iterator_step(vm, keys_iterator));
            next = iterator_result;

            // ii. If next is not false, then
            if (next) {
                // 1. Let nextValue be ? IteratorValue(next).
                auto next_value = TRY(iterator_value(vm, *iterator_result));

                // 2. If SetDataHas(O.[[SetData]], nextValue) is true, return false.
                if (set->set_has(next_value))
                    return false;
            }
        }
    }

    // 7. Return true.
    return true;
}

}
