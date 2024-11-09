/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/WeakSetPrototype.h>

namespace JS {

JS_DEFINE_ALLOCATOR(WeakSetPrototype);

WeakSetPrototype::WeakSetPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void WeakSetPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;

    define_native_function(realm, vm.names.add, add, 1, attr);
    define_native_function(realm, vm.names.delete_, delete_, 1, attr);
    define_native_function(realm, vm.names.has, has, 1, attr);

    // 24.4.3.5 WeakSet.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-weakset.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.WeakSet.as_string()), Attribute::Configurable);
}

// 24.4.3.1 WeakSet.prototype.add ( value ), https://tc39.es/ecma262/#sec-weakset.prototype.add
JS_DEFINE_NATIVE_FUNCTION(WeakSetPrototype::add)
{
    auto value = vm.argument(0);

    // 1. Let S be the this value.
    // 2. Perform ? RequireInternalSlot(S, [[WeakSetData]]).
    auto weak_set = TRY(typed_this_object(vm));

    // 3. If CanBeHeldWeakly(value) is false, throw a TypeError exception.
    if (!can_be_held_weakly(value))
        return vm.throw_completion<TypeError>(ErrorType::CannotBeHeldWeakly, value.to_string_without_side_effects());

    // 4. For each element e of S.[[WeakSetData]], do
    //     a. If e is not empty and SameValue(e, value) is true, then
    //         i. Return S.
    // 5. Append value to S.[[WeakSetData]].
    weak_set->values().set(&value.as_cell(), AK::HashSetExistingEntryBehavior::Keep);

    // 6. Return S.
    return weak_set;
}

// 24.4.3.3 WeakSet.prototype.delete ( value ), https://tc39.es/ecma262/#sec-weakset.prototype.delete
JS_DEFINE_NATIVE_FUNCTION(WeakSetPrototype::delete_)
{
    auto value = vm.argument(0);

    // 1. Let S be the this value.
    // 2. Perform ? RequireInternalSlot(S, [[WeakSetData]]).
    auto weak_set = TRY(typed_this_object(vm));

    // 3. If CanBeHeldWeakly(value) is false, return false.
    if (!can_be_held_weakly(value))
        return Value(false);

    // 4. For each element e of S.[[WeakSetData]], do
    //     a. If e is not empty and SameValue(e, value) is true, then
    //         i. Replace the element of S.[[WeakSetData]] whose value is e with an element whose value is empty.
    //         ii. Return true.
    // 5. Return false.
    return Value(weak_set->values().remove(&value.as_cell()));
}

// 24.4.3.4 WeakSet.prototype.has ( value ), https://tc39.es/ecma262/#sec-weakset.prototype.has
JS_DEFINE_NATIVE_FUNCTION(WeakSetPrototype::has)
{
    auto value = vm.argument(0);

    // 1. Let S be the this value.
    // 2. Perform ? RequireInternalSlot(S, [[WeakSetData]]).
    auto weak_set = TRY(typed_this_object(vm));

    // 3. If CanBeHeldWeakly(value) is false, return false.
    if (!can_be_held_weakly(value))
        return Value(false);

    // 4. For each element e of S.[[WeakSetData]], do
    //     a. If e is not empty and SameValue(e, value) is true, return true.
    auto& values = weak_set->values();
    auto result = values.find(&value.as_cell());
    if (result != values.end())
        return Value(true);

    // 5. Return false.
    return Value(false);
}

}
