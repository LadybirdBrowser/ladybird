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
    define_native_function(realm, vm.names.difference, difference, 1, attr);
    define_native_function(realm, vm.names.entries, entries, 0, attr);
    define_native_function(realm, vm.names.forEach, for_each, 1, attr);
    define_native_function(realm, vm.names.has, has, 1, attr);
    define_native_function(realm, vm.names.intersection, intersection, 1, attr);
    define_native_function(realm, vm.names.isDisjointFrom, is_disjoint_from, 1, attr);
    define_native_function(realm, vm.names.isSubsetOf, is_subset_of, 1, attr);
    define_native_function(realm, vm.names.isSupersetOf, is_superset_of, 1, attr);
    define_native_accessor(realm, vm.names.size, size_getter, {}, Attribute::Configurable);
    define_native_function(realm, vm.names.symmetricDifference, symmetric_difference, 1, attr);
    define_native_function(realm, vm.names.union_, union_, 1, attr);
    define_native_function(realm, vm.names.values, values, 0, attr);

    define_direct_property(vm.names.keys, get_without_side_effects(vm.names.values), attr);

    // 24.2.3.18 Set.prototype [ @@iterator ] ( ), https://tc39.es/ecma262/#sec-set.prototype-@@iterator
    define_direct_property(vm.well_known_symbol_iterator(), get_without_side_effects(vm.names.values), attr);

    // 24.2.3.19 Set.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-set.prototype-@@tostringtag
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

// 24.2.4.5 Set.prototype.difference ( other ), https://tc39.es/ecma262/#sec-set.prototype.difference
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::difference)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let otherRec be ? GetSetRecord(other).
    auto other_record = TRY(get_set_record(vm, vm.argument(0)));

    // 4. Let resultSetData be a copy of O.[[SetData]].
    auto result = set->copy();

    // 5. If SetDataSize(O.[[SetData]]) ≤ otherRec.[[Size]], then
    if (set->set_size() <= other_record.size) {
        // a. Let thisSize be the number of elements in O.[[SetData]].
        // b. Let index be 0.
        // c. Repeat, while index < thisSize,
        for (auto const& element : *set) {
            // i. Let e be resultSetData[index].
            // ii. If e is not EMPTY, then
            //     1. Let inOther be ToBoolean(? Call(otherRec.[[Has]], otherRec.[[SetObject]], « e »)).
            auto in_other = TRY(call(vm, *other_record.has, other_record.set_object, element.key)).to_boolean();

            //     2. If inOther is true, then
            if (in_other) {
                // a. Set resultSetData[index] to EMPTY.
                result->set_remove(element.key);
            }

            // iii. Set index to index + 1.
        }
    }
    // 6. Else,
    else {
        // a. Let keysIter be ? GetIteratorFromMethod(otherRec.[[SetObject]], otherRec.[[Keys]]).
        auto keys_iterator = TRY(get_iterator_from_method(vm, other_record.set_object, other_record.keys));

        // b. Let next be NOT-STARTED.
        Optional<Value> next;

        // c. Repeat, while next is not DONE,
        do {
            // i. Set next to ? IteratorStepValue(keysIter).
            next = TRY(iterator_step_value(vm, keys_iterator));

            // ii. If next is not DONE, then
            if (next.has_value()) {
                // 1. Set next to CanonicalizeKeyedCollectionKey(next).
                next = canonicalize_keyed_collection_key(*next);

                // 2. Let valueIndex be SetDataIndex(resultSetData, next).
                // 3. If valueIndex is not NOT-FOUND, then
                if (result->set_has(*next)) {
                    // a. Set resultSetData[valueIndex] to EMPTY.
                    result->set_remove(*next);
                }
            }
        } while (next.has_value());
    }

    // 7. Let result be OrdinaryObjectCreate(%Set.prototype%, « [[SetData]] »).
    // 8. Set result.[[SetData]] to resultSetData.

    // 9. Return result.
    return result;
}

// 24.2.3.6 Set.prototype.entries ( ), https://tc39.es/ecma262/#sec-set.prototype.entries
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::entries)
{
    auto& realm = *vm.current_realm();

    // 1. Let S be the this value.
    auto set = TRY(typed_this_object(vm));

    // 2. Return ? CreateSetIterator(S, key+value).
    return SetIterator::create(realm, set, Object::PropertyKind::KeyAndValue);
}

// 24.2.3.7 Set.prototype.forEach ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-set.prototype.foreach
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

// 24.2.3.8 Set.prototype.has ( value ), https://tc39.es/ecma262/#sec-set.prototype.has
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

// 24.2.4.9 Set.prototype.intersection ( other ), https://tc39.es/ecma262/#sec-set.prototype.intersection
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

    // 5. If SetDataSize(O.[[SetData]]) ≤ otherRec.[[Size]], then
    if (set->set_size() <= other_record.size) {
        // a. Let thisSize be the number of elements in O.[[SetData]].
        // b. Let index be 0.
        // c. Repeat, while index < thisSize,
        for (auto const& element : *set) {
            // i. Let e be O.[[SetData]][index].
            // ii. Set index to index + 1.
            // iii. If e is not empty, then
            //     1. Let inOther be ToBoolean(? Call(otherRec.[[Has]], otherRec.[[SetObject]], « e »)).
            auto in_other = TRY(call(vm, *other_record.has, other_record.set_object, element.key)).to_boolean();

            //     2. If inOther is true, then
            if (in_other) {
                // a. NOTE: It is possible for earlier calls to otherRec.[[Has]] to remove and re-add an element of O.[[SetData]], which can cause the same element to be visited twice during this iteration.
                // b. If SetDataHas(resultSetData, e) is false, then
                if (!set_data_has(result, element.key)) {
                    // i. Append e to resultSetData.
                    result->set_add(element.key);
                }
            }

            //     3. NOTE: The number of elements in O.[[SetData]] may have increased during execution of otherRec.[[Has]].
            //     4. Set thisSize to the number of elements in O.[[SetData]].
        }
    }
    // 6. Else,
    else {
        // a. Let keysIter be ? GetIteratorFromMethod(otherRec.[[SetObject]], otherRec.[[Keys]]).
        auto keys_iterator = TRY(get_iterator_from_method(vm, other_record.set_object, other_record.keys));

        // b. Let next be NOT-STARTED.
        Optional<Value> next;

        // c. Repeat, while next is not DONE,
        do {
            // i. Set next to ? IteratorStepValue(keysIter).
            next = TRY(iterator_step_value(vm, keys_iterator));

            // ii. If next is not DONE, then
            if (next.has_value()) {
                // 1. Set next to CanonicalizeKeyedCollectionKey(next).
                next = canonicalize_keyed_collection_key(*next);

                // 2. Let inThis be SetDataHas(O.[[SetData]], next).
                auto in_this = set_data_has(set, *next);

                // 3. If inThis is true, then
                if (in_this) {
                    // a. NOTE: Because other is an arbitrary object, it is possible for its "keys" iterator to produce the same value more than once.

                    // b. If SetDataHas(resultSetData, next) is false, then
                    if (!set_data_has(result, *next)) {
                        // i. Append next to resultSetData.
                        result->set_add(*next);
                    }
                }
            }
        } while (next.has_value());
    }

    // 7. Let result be OrdinaryObjectCreate(%Set.prototype%, « [[SetData]] »).
    // 8. Set result.[[SetData]] to resultSetData.

    // 9. Return result.
    return result;
}

// 24.2.4.10 Set.prototype.isDisjointFrom ( other ), https://tc39.es/ecma262/#sec-set.prototype.isdisjointfrom
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::is_disjoint_from)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let otherRec be ? GetSetRecord(other).
    auto other_record = TRY(get_set_record(vm, vm.argument(0)));

    // 4. If SetDataSize(O.[[SetData]]) ≤ otherRec.[[Size]], then
    if (set->set_size() <= other_record.size) {
        // a. Let thisSize be the number of elements in O.[[SetData]].
        // b. Let index be 0.
        // c. Repeat, while index < thisSize,
        for (auto const& element : *set) {
            // i. Let e be O.[[SetData]][index].
            // ii. Set index to index + 1.
            // iii. If e is not empty, then
            //     1. Let inOther be ToBoolean(? Call(otherRec.[[Has]], otherRec.[[SetObject]], « e »)).
            auto in_other = TRY(call(vm, *other_record.has, other_record.set_object, element.key)).to_boolean();

            //     2. If inOther is true, return false.
            if (in_other)
                return false;

            //     3. NOTE: The number of elements in O.[[SetData]] may have increased during execution of otherRec.[[Has]].
            //     4. Set thisSize to the number of elements in O.[[SetData]].
        }
    }
    // 5. Else,
    else {
        // a. Let keysIter be ? GetIteratorFromMethod(otherRec.[[SetObject]], otherRec.[[Keys]]).
        auto keys_iterator = TRY(get_iterator_from_method(vm, other_record.set_object, other_record.keys));

        // b. Let next be NOT-STARTED.
        Optional<Value> next;

        // c. Repeat, while next is not DONE,
        do {
            // i. Set next to ? IteratorStepValue(keysIter).
            next = TRY(iterator_step_value(vm, keys_iterator));

            // ii. If next is not DONE, then
            if (next.has_value()) {
                // 1. If SetDataHas(O.[[SetData]], next) is true, then
                if (set_data_has(set, *next)) {
                    // a. Perform ? IteratorClose(keysIter, NormalCompletion(UNUSED)).
                    TRY(iterator_close(vm, keys_iterator, normal_completion({})));

                    // b. Return false.
                    return false;
                }
            }
        } while (next.has_value());
    }

    // 6. Return true.
    return true;
}

// 24.2.4.11 Set.prototype.isSubsetOf ( other ), https://tc39.es/ecma262/#sec-set.prototype.issubsetof
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::is_subset_of)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let otherRec be ? GetSetRecord(other).
    auto other_record = TRY(get_set_record(vm, vm.argument(0)));

    // 4. If SetDataSize(O.[[SetData]]) > otherRec.[[Size]], return false.
    if (set->set_size() > other_record.size)
        return false;

    // 5. Let thisSize be the number of elements in O.[[SetData]].
    // 6. Let index be 0.
    // 7. Repeat, while index < thisSize,
    for (auto const& element : *set) {
        // a. Let e be O.[[SetData]][index].
        // b. Set index to index + 1.
        // c. If e is not empty, then
        //     i. Let inOther be ToBoolean(? Call(otherRec.[[Has]], otherRec.[[SetObject]], « e »)).
        auto in_other = TRY(call(vm, *other_record.has, other_record.set_object, element.key)).to_boolean();

        //     ii. If inOther is false, return false.
        if (!in_other)
            return false;

        //     iii. NOTE: The number of elements in O.[[SetData]] may have increased during execution of otherRec.[[Has]].
        //     iv. Set thisSize to the number of elements in O.[[SetData]].
    }

    // 8. Return true.
    return true;
}

// 24.2.4.12 Set.prototype.isSupersetOf ( other ), https://tc39.es/ecma262/#sec-set.prototype.issupersetof
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::is_superset_of)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let otherRec be ? GetSetRecord(other).
    auto other_record = TRY(get_set_record(vm, vm.argument(0)));

    // 4. If SetDataSize(O.[[SetData]]) < otherRec.[[Size]], return false.
    if (set->set_size() < other_record.size)
        return false;

    // 5. Let keysIter be ? GetIteratorFromMethod(otherRec.[[SetObject]], otherRec.[[Keys]]).
    auto keys_iterator = TRY(get_iterator_from_method(vm, other_record.set_object, other_record.keys));

    // 6. Let next be NOT-STARTED.
    Optional<Value> next;

    // 7. Repeat, while next is not DONE,
    do {
        // a. Set next to ? IteratorStepValue(keysIter).
        next = TRY(iterator_step_value(vm, keys_iterator));

        // b. If next is not DONE, then
        if (next.has_value()) {
            // i. If SetDataHas(O.[[SetData]], next) is false, then
            if (!set_data_has(set, *next)) {
                // 1. Perform ? IteratorClose(keysIter, NormalCompletion(UNUSED)).
                TRY(iterator_close(vm, keys_iterator, normal_completion({})));

                // 2. Return false.
                return false;
            }
        }
    } while (next.has_value());

    // 8. Return true.
    return true;
}

// 24.2.3.14 get Set.prototype.size, https://tc39.es/ecma262/#sec-get-set.prototype.size
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

// 24.2.4.15 Set.prototype.symmetricDifference ( other ), https://tc39.es/ecma262/#sec-set.prototype.symmetricdifference
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::symmetric_difference)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let otherRec be ? GetSetRecord(other).
    auto other_record = TRY(get_set_record(vm, vm.argument(0)));

    // 4. Let keysIter be ? GetIteratorFromMethod(otherRec.[[SetObject]], otherRec.[[Keys]]).
    auto keys_iterator = TRY(get_iterator_from_method(vm, other_record.set_object, other_record.keys));

    // 5. Let resultSetData be a copy of O.[[SetData]].
    auto result = set->copy();

    // 6. Let next be NOT-STARTED.
    Optional<Value> next;

    // 7. Repeat, while next is not DONE,
    do {
        // a. Set next to ? IteratorStepValue(keysIter).
        next = TRY(iterator_step_value(vm, keys_iterator));

        // b. If next is not DONE, then
        if (next.has_value()) {
            // i. Set next to CanonicalizeKeyedCollectionKey(next).
            next = canonicalize_keyed_collection_key(*next);

            // ii. Let resultIndex be SetDataIndex(resultSetData, next).
            // iii. If resultIndex is not-found, let alreadyInResult be false. Otherwise let alreadyInResult be true.
            auto already_in_result = result->set_has(*next);

            // iv. If SetDataHas(O.[[SetData]], next) is true, then
            if (set_data_has(set, *next)) {
                // 1. If alreadyInResult is true, set resultSetData[resultIndex] to empty.
                if (already_in_result)
                    result->set_remove(*next);
            }
            // v. Else,
            else {
                // 1. If alreadyInResult is false, append next to resultSetData.
                if (!already_in_result)
                    result->set_add(*next);
            }
        }
    } while (next.has_value());

    // 8. Let result be OrdinaryObjectCreate(%Set.prototype%, « [[SetData]] »).
    // 9. Set result.[[SetData]] to resultSetData.

    // 10. Return result.
    return result;
}

// 24.2.4.16 Set.prototype.union ( other ), https://tc39.es/ecma262/#sec-set.prototype.union
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::union_)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
    auto set = TRY(typed_this_object(vm));

    // 3. Let otherRec be ? GetSetRecord(other).
    auto other_record = TRY(get_set_record(vm, vm.argument(0)));

    // 4. Let keysIter be ? GetIteratorFromMethod(otherRec.[[SetObject]], otherRec.[[Keys]]).
    auto keys_iterator = TRY(get_iterator_from_method(vm, other_record.set_object, other_record.keys));

    // 5. Let resultSetData be a copy of O.[[SetData]].
    auto result = set->copy();

    // 6. Let next be NOT-STARTED.
    Optional<Value> next;

    // 7. Repeat, while next is not DONE,
    do {
        // a. Set next to ? IteratorStepValue(keysIter).
        next = TRY(iterator_step_value(vm, keys_iterator));

        // b. If next is not DONE, then
        if (next.has_value()) {
            // i. Set next to CanonicalizeKeyedCollectionKey(next).
            next = canonicalize_keyed_collection_key(*next);

            // ii. If SetDataHas(resultSetData, next) is false, then
            if (!set_data_has(result, *next)) {
                // 1. Append next to resultSetData.
                result->set_add(*next);
            }
        }
    } while (next.has_value());

    // 8. Let result be OrdinaryObjectCreate(%Set.prototype%, « [[SetData]] »).
    // 9. Set result.[[SetData]] to resultSetData.

    // 10. Return result.
    return result;
}

// 24.2.3.17 Set.prototype.values ( ), https://tc39.es/ecma262/#sec-set.prototype.values
JS_DEFINE_NATIVE_FUNCTION(SetPrototype::values)
{
    auto& realm = *vm.current_realm();

    // 1. Let S be the this value.
    // NOTE: CreateSetIterator checks the presence of a [[SetData]] slot, so we can do this here.
    auto set = TRY(typed_this_object(vm));

    // 2. Return ? CreateSetIterator(S, value).
    return SetIterator::create(realm, set, Object::PropertyKind::Value);
}

}
