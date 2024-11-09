/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/WeakSet.h>
#include <LibJS/Runtime/WeakSetConstructor.h>

namespace JS {

JS_DEFINE_ALLOCATOR(WeakSetConstructor);

WeakSetConstructor::WeakSetConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.WeakSet.as_string(), realm.intrinsics().function_prototype())
{
}

void WeakSetConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 24.4.2.1 WeakSet.prototype, https://tc39.es/ecma262/#sec-weakset.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().weak_set_prototype(), 0);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 24.4.1.1 WeakSet ( [ iterable ] ), https://tc39.es/ecma262/#sec-weakset-iterable
ThrowCompletionOr<Value> WeakSetConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.WeakSet);
}

// 24.4.1.1 WeakSet ( [ iterable ] ), https://tc39.es/ecma262/#sec-weakset-iterable
ThrowCompletionOr<NonnullGCPtr<Object>> WeakSetConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();
    auto iterable = vm.argument(0);

    // 2. Let set be ? OrdinaryCreateFromConstructor(NewTarget, "%WeakSet.prototype%", « [[WeakSetData]] »).
    // 3. Set set.[[WeakSetData]] to a new empty List.
    auto set = TRY(ordinary_create_from_constructor<WeakSet>(vm, new_target, &Intrinsics::weak_set_prototype));

    // 4. If iterable is either undefined or null, return set.
    if (iterable.is_nullish())
        return set;

    // 5. Let adder be ? Get(set, "add").
    auto adder = TRY(set->get(vm.names.add));

    // 6. If IsCallable(adder) is false, throw a TypeError exception.
    if (!adder.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, "'add' property of WeakSet");

    // 7. Let iteratorRecord be ? GetIterator(iterable, sync).
    // 8. Repeat,
    (void)TRY(get_iterator_values(vm, iterable, [&](Value next) -> Optional<Completion> {
        // a. Let next be ? IteratorStepValue(iteratorRecord).
        // c. If next is DONE, return set.
        // c. Let status be Completion(Call(adder, set, « nextValue »)).
        // d. IfAbruptCloseIterator(status, iteratorRecord).
        TRY(JS::call(vm, adder.as_function(), set, next));
        return {};
    }));

    // b. If next is false, return set.
    return set;
}

}
