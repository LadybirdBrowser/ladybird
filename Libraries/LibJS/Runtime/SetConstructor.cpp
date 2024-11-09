/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/SetConstructor.h>

namespace JS {

JS_DEFINE_ALLOCATOR(SetConstructor);

SetConstructor::SetConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Set.as_string(), realm.intrinsics().function_prototype())
{
}

void SetConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 24.2.2.1 Set.prototype, https://tc39.es/ecma262/#sec-set.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().set_prototype(), 0);

    define_native_accessor(realm, vm.well_known_symbol_species(), symbol_species_getter, {}, Attribute::Configurable);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 24.2.1.1 Set ( [ iterable ] ), https://tc39.es/ecma262/#sec-set-iterable
ThrowCompletionOr<Value> SetConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.Set);
}

// 24.2.1.1 Set ( [ iterable ] ), https://tc39.es/ecma262/#sec-set-iterable
ThrowCompletionOr<NonnullGCPtr<Object>> SetConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();
    auto iterable = vm.argument(0);

    // 2. Let set be ? OrdinaryCreateFromConstructor(NewTarget, "%Set.prototype%", « [[SetData]] »).
    auto set = TRY(ordinary_create_from_constructor<Set>(vm, new_target, &Intrinsics::set_prototype));

    // 3. Set set.[[SetData]] to a new empty List.

    // 4. If iterable is either undefined or null, return set.
    if (iterable.is_nullish())
        return set;

    // 5. Let adder be ? Get(set, "add").
    auto adder = TRY(set->get(vm.names.add));

    // 6. If IsCallable(adder) is false, throw a TypeError exception.
    if (!adder.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, "'add' property of Set");

    // 7. Let iteratorRecord be ? GetIterator(iterable, sync).
    // 8. Repeat,
    (void)TRY(get_iterator_values(vm, iterable, [&](Value next) -> Optional<Completion> {
        // a. Let next be ? IteratorStepValue(iteratorRecord).
        // b. If next is DONE, return set.
        // c. Let status be Completion(Call(adder, set, « nextValue »)).
        // d. IfAbruptCloseIterator(status, iteratorRecord).
        TRY(JS::call(vm, adder.as_function(), set, next));
        return {};
    }));

    // b. If next is false, return set.
    return set;
}

// 24.2.2.2 get Set [ @@species ], https://tc39.es/ecma262/#sec-get-set-@@species
JS_DEFINE_NATIVE_FUNCTION(SetConstructor::symbol_species_getter)
{
    // 1. Return the this value.
    return vm.this_value();
}

}
