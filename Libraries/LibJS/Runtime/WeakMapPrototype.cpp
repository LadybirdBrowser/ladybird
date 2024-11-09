/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/WeakMapPrototype.h>

namespace JS {

JS_DEFINE_ALLOCATOR(WeakMapPrototype);

WeakMapPrototype::WeakMapPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void WeakMapPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;

    define_native_function(realm, vm.names.delete_, delete_, 1, attr);
    define_native_function(realm, vm.names.get, get, 1, attr);
    define_native_function(realm, vm.names.has, has, 1, attr);
    define_native_function(realm, vm.names.set, set, 2, attr);

    // 24.3.3.6 WeakMap.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-weakmap.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.WeakMap.as_string()), Attribute::Configurable);
}

// 24.3.3.2 WeakMap.prototype.delete ( key ), https://tc39.es/ecma262/#sec-weakmap.prototype.delete
JS_DEFINE_NATIVE_FUNCTION(WeakMapPrototype::delete_)
{
    auto key = vm.argument(0);

    // 1. Let M be the this value.
    // 2. Perform ? RequireInternalSlot(M, [[WeakMapData]]).
    auto weak_map = TRY(typed_this_object(vm));

    // 3. If CanBeHeldWeakly(key) is false, return false.
    if (!can_be_held_weakly(key))
        return Value(false);

    // 4. For each Record { [[Key]], [[Value]] } p of M.[[WeakMapData]], do
    //     a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, then
    //         i. Set p.[[Key]] to empty.
    //         ii. Set p.[[Value]] to empty.
    //         iii. Return true.
    // 5. Return false.
    return Value(weak_map->values().remove(&key.as_cell()));
}

// 24.3.3.3 WeakMap.prototype.get ( key ), https://tc39.es/ecma262/#sec-weakmap.prototype.get
JS_DEFINE_NATIVE_FUNCTION(WeakMapPrototype::get)
{
    auto key = vm.argument(0);

    // 1. Let M be the this value.
    // 2. Perform ? RequireInternalSlot(M, [[WeakMapData]]).
    auto weak_map = TRY(typed_this_object(vm));

    // 3. If CanBeHeldWeakly(key) is false, return undefined.
    if (!can_be_held_weakly(key))
        return js_undefined();

    // 4. For each Record { [[Key]], [[Value]] } p of M.[[WeakMapData]], do
    //     a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, return p.[[Value]].
    auto& values = weak_map->values();
    auto result = values.find(&key.as_cell());
    if (result != values.end())
        return result->value;

    // 5. Return undefined.
    return js_undefined();
}

// 24.3.3.4 WeakMap.prototype.has ( key ), https://tc39.es/ecma262/#sec-weakmap.prototype.has
JS_DEFINE_NATIVE_FUNCTION(WeakMapPrototype::has)
{
    auto key = vm.argument(0);

    // 1. Let M be the this value.
    // 2. Perform ? RequireInternalSlot(M, [[WeakMapData]]).
    auto weak_map = TRY(typed_this_object(vm));

    // 3. If CanBeHeldWeakly(key) is false, return false.
    if (!can_be_held_weakly(key))
        return Value(false);

    // 4. For each Record { [[Key]], [[Value]] } p of M.[[WeakMapData]], do
    //     a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, return true.
    auto& values = weak_map->values();
    auto result = values.find(&key.as_cell());
    if (result != values.end())
        return Value(true);

    // 5. Return false.
    return Value(false);
}

// 24.3.3.5 WeakMap.prototype.set ( key, value ), https://tc39.es/ecma262/#sec-weakmap.prototype.set
JS_DEFINE_NATIVE_FUNCTION(WeakMapPrototype::set)
{
    auto key = vm.argument(0);
    auto value = vm.argument(1);

    // 1. Let M be the this value.
    // 2. Perform ? RequireInternalSlot(M, [[WeakMapData]]).
    auto weak_map = TRY(typed_this_object(vm));

    // 3. If CanBeHeldWeakly(key) is false, throw a TypeError exception.
    if (!can_be_held_weakly(key))
        return vm.throw_completion<TypeError>(ErrorType::CannotBeHeldWeakly, value.to_string_without_side_effects());

    // 4. For each Record { [[Key]], [[Value]] } p of M.[[WeakMapData]], do
    //    a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, then
    //        i. Set p.[[Value]] to value.
    //        ii. Return M.
    // 5. Let p be the Record { [[Key]]: key, [[Value]]: value }.
    // 6. Append p to M.[[WeakMapData]].
    weak_map->values().set(&key.as_cell(), value);

    // 7. Return M.
    return weak_map;
}

}
