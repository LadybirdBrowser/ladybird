/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/DisposableStack.h>
#include <LibJS/Runtime/DisposableStackConstructor.h>

namespace JS {

GC_DEFINE_ALLOCATOR(DisposableStackConstructor);

DisposableStackConstructor::DisposableStackConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.DisposableStack.as_string(), realm.intrinsics().function_prototype())
{
}

void DisposableStackConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 12.3.2.1 DisposableStack.prototype, https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().disposable_stack_prototype(), 0);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 12.3.1.1 DisposableStack ( ), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack
ThrowCompletionOr<Value> DisposableStackConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.DisposableStack);
}

// 12.3.1.1 DisposableStack ( ), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack
ThrowCompletionOr<GC::Ref<Object>> DisposableStackConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 2. Let disposableStack be ? OrdinaryCreateFromConstructor(NewTarget, "%DisposableStack.prototype%", « [[DisposableState]], [[DisposeCapability]] »).
    // 3. Set disposableStack.[[DisposableState]] to pending.
    // 4. Set disposableStack.[[DisposeCapability]] to NewDisposeCapability().
    // 5. Return disposableStack.
    return TRY(ordinary_create_from_constructor<DisposableStack>(vm, new_target, &Intrinsics::disposable_stack_prototype, new_dispose_capability()));
}

}
