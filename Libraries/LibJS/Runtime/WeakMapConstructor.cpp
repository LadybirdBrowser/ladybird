/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/WeakMap.h>
#include <LibJS/Runtime/WeakMapConstructor.h>

namespace JS {

JS_DEFINE_ALLOCATOR(WeakMapConstructor);

WeakMapConstructor::WeakMapConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.WeakMap.as_string(), realm.intrinsics().function_prototype())
{
}

void WeakMapConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 24.3.2.1 WeakMap.prototype, https://tc39.es/ecma262/#sec-weakmap.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().weak_map_prototype(), 0);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 24.3.1.1 WeakMap ( [ iterable ] ), https://tc39.es/ecma262/#sec-weakmap-iterable
ThrowCompletionOr<Value> WeakMapConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.WeakMap);
}

// 24.3.1.1 WeakMap ( [ iterable ] ), https://tc39.es/ecma262/#sec-weakmap-iterable
ThrowCompletionOr<NonnullGCPtr<Object>> WeakMapConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();
    auto iterable = vm.argument(0);

    // 2. Let map be ? OrdinaryCreateFromConstructor(NewTarget, "%WeakMap.prototype%", « [[WeakMapData]] »).
    // 3. Set map.[[WeakMapData]] to a new empty List.
    auto map = TRY(ordinary_create_from_constructor<WeakMap>(vm, new_target, &Intrinsics::weak_map_prototype));

    // 4. If iterable is either undefined or null, return map.
    if (iterable.is_nullish())
        return map;

    // 5. Let adder be ? Get(map, "set").
    auto adder = TRY(map->get(vm.names.set));

    // 6. If IsCallable(adder) is false, throw a TypeError exception.
    if (!adder.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, "'set' property of WeakMap");

    // 7. Return ? AddEntriesFromIterable(map, iterable, adder).
    (void)TRY(get_iterator_values(vm, iterable, [&](Value iterator_value) -> Optional<Completion> {
        if (!iterator_value.is_object())
            return vm.throw_completion<TypeError>(ErrorType::NotAnObject, ByteString::formatted("Iterator value {}", iterator_value.to_string_without_side_effects()));

        auto key = TRY(iterator_value.as_object().get(0));
        auto value = TRY(iterator_value.as_object().get(1));
        TRY(JS::call(vm, adder.as_function(), map, key, value));

        return {};
    }));

    return map;
}

}
