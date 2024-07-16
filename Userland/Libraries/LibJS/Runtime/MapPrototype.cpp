/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/KeyedCollections.h>
#include <LibJS/Runtime/MapIterator.h>
#include <LibJS/Runtime/MapPrototype.h>

namespace JS {

JS_DEFINE_ALLOCATOR(MapPrototype);

MapPrototype::MapPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void MapPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;

    define_native_function(realm, vm.names.clear, clear, 0, attr);
    define_native_function(realm, vm.names.delete_, delete_, 1, attr);
    define_native_function(realm, vm.names.entries, entries, 0, attr);
    define_native_function(realm, vm.names.forEach, for_each, 1, attr);
    define_native_function(realm, vm.names.get, get, 1, attr);
    define_native_function(realm, vm.names.has, has, 1, attr);
    define_native_function(realm, vm.names.keys, keys, 0, attr);
    define_native_function(realm, vm.names.set, set, 2, attr);
    define_native_function(realm, vm.names.values, values, 0, attr);

    define_native_accessor(realm, vm.names.size, size_getter, {}, Attribute::Configurable);

    define_direct_property(vm.well_known_symbol_iterator(), get_without_side_effects(vm.names.entries), attr);
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.Map.as_string()), Attribute::Configurable);
}

// 24.1.3.1 Map.prototype.clear ( ), https://tc39.es/ecma262/#sec-map.prototype.clear
JS_DEFINE_NATIVE_FUNCTION(MapPrototype::clear)
{
    // 1. Let M be the this value.
    // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
    auto map = TRY(typed_this_object(vm));

    // 3. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
    //     a. Set p.[[Key]] to empty.
    //     b. Set p.[[Value]] to empty.
    map->map_clear();

    // 4. Return undefined.
    return js_undefined();
}

// 24.1.3.3 Map.prototype.delete ( key ), https://tc39.es/ecma262/#sec-map.prototype.delete
JS_DEFINE_NATIVE_FUNCTION(MapPrototype::delete_)
{
    auto key = vm.argument(0);

    // 1. Let M be the this value.
    // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
    auto map = TRY(typed_this_object(vm));

    // 3. Set key to CanonicalizeKeyedCollectionKey(key).
    key = canonicalize_keyed_collection_key(key);

    // 3. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
    //     a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, then
    //         i. Set p.[[Key]] to empty.
    //         ii. Set p.[[Value]] to empty.
    //         iii. Return true.
    // 4. Return false.
    return Value(map->map_remove(key));
}

// 24.1.3.4 Map.prototype.entries ( ), https://tc39.es/ecma262/#sec-map.prototype.entries
JS_DEFINE_NATIVE_FUNCTION(MapPrototype::entries)
{
    auto& realm = *vm.current_realm();

    // 1. Let M be the this value.
    auto map = TRY(typed_this_object(vm));

    // 2. Return ? CreateMapIterator(M, key+value).
    return MapIterator::create(realm, *map, Object::PropertyKind::KeyAndValue);
}

// 24.1.3.5 Map.prototype.forEach ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-map.prototype.foreach
JS_DEFINE_NATIVE_FUNCTION(MapPrototype::for_each)
{
    auto callbackfn = vm.argument(0);
    auto this_arg = vm.argument(1);

    // 1. Let M be the this value.
    // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
    auto map = TRY(typed_this_object(vm));

    // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
    if (!callbackfn.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, callbackfn.to_string_without_side_effects());

    // 4. Let entries be M.[[MapData]].
    // 5. Let numEntries be the number of elements in entries.
    // 6. Let index be 0.
    // 7. Repeat, while index < numEntries,
    for (auto& entry : *map) {
        // i. Let e be entries[index].
        // b. Set index to index + 1.
        // c. If e.[[Key]] is not empty, then
        // NOTE: This is handled by Map's IteratorImpl.

        // i. Perform ? Call(callbackfn, thisArg, « e.[[Value]], e.[[Key]], M »).
        TRY(call(vm, callbackfn.as_function(), this_arg, entry.value, entry.key, map));

        // ii. NOTE: The number of elements in entries may have increased during execution of callbackfn.
        // iii. Set numEntries to the number of elements in entries.
    }

    // 8. Return undefined.
    return js_undefined();
}

// 24.1.3.6 Map.prototype.get ( key ), https://tc39.es/ecma262/#sec-map.prototype.get
JS_DEFINE_NATIVE_FUNCTION(MapPrototype::get)
{
    auto key = vm.argument(0);

    // 1. Let M be the this value.
    // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
    auto map = TRY(typed_this_object(vm));

    // 3. Set key to CanonicalizeKeyedCollectionKey(key).
    key = canonicalize_keyed_collection_key(key);

    // 3. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
    //    a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, return p.[[Value]].
    if (auto result = map->map_get(key); result.has_value())
        return result.release_value();

    // 4. Return undefined.
    return js_undefined();
}

// 24.1.3.7 Map.prototype.has ( key ), https://tc39.es/ecma262/#sec-map.prototype.has
JS_DEFINE_NATIVE_FUNCTION(MapPrototype::has)
{
    auto key = vm.argument(0);

    // 1. Let M be the this value.
    // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
    auto map = TRY(typed_this_object(vm));

    // 3. Set key to CanonicalizeKeyedCollectionKey(key).
    key = canonicalize_keyed_collection_key(key);

    // 3. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
    //    a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, return true.
    // 4. Return false.
    return map->map_has(key);
}

// 24.1.3.8 Map.prototype.keys ( ), https://tc39.es/ecma262/#sec-map.prototype.keys
JS_DEFINE_NATIVE_FUNCTION(MapPrototype::keys)
{
    auto& realm = *vm.current_realm();

    // 1. Let M be the this value.
    auto map = TRY(typed_this_object(vm));

    // 2. Return ? CreateMapIterator(M, key).
    return MapIterator::create(realm, *map, Object::PropertyKind::Key);
}

// 24.1.3.9 Map.prototype.set ( key, value ), https://tc39.es/ecma262/#sec-map.prototype.set
JS_DEFINE_NATIVE_FUNCTION(MapPrototype::set)
{
    auto key = vm.argument(0);
    auto value = vm.argument(1);

    // 1. Let M be the this value.
    // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
    auto map = TRY(typed_this_object(vm));

    // 3. Set key to CanonicalizeKeyedCollectionKey(key).
    key = canonicalize_keyed_collection_key(key);

    // 4. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
    //     a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, then
    //         i. Set p.[[Value]] to value.
    //         ii. Return M.
    // 5. Let p be the Record { [[Key]]: key, [[Value]]: value }.
    // 6. Append p to M.[[MapData]].
    map->map_set(key, value);

    // 7. Return M.
    return map;
}

// 24.1.3.10 get Map.prototype.size, https://tc39.es/ecma262/#sec-get-map.prototype.size
JS_DEFINE_NATIVE_FUNCTION(MapPrototype::size_getter)
{
    // 1. Let M be the this value.
    // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
    auto map = TRY(typed_this_object(vm));

    // 3. Let count be 0.
    // 4. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
    //    a. If p.[[Key]] is not empty, set count to count + 1.
    auto count = map->map_size();

    // 5. Return 𝔽(count).
    return Value(count);
}

// 24.1.3.11 Map.prototype.values ( ), https://tc39.es/ecma262/#sec-map.prototype.values
JS_DEFINE_NATIVE_FUNCTION(MapPrototype::values)
{
    auto& realm = *vm.current_realm();

    // 1. Let M be the this value.
    auto map = TRY(typed_this_object(vm));

    // 2. Return ? CreateMapIterator(M, value).
    return MapIterator::create(realm, *map, Object::PropertyKind::Value);
}

}
