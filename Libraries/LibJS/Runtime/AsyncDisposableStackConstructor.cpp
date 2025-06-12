/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/AsyncDisposableStack.h>
#include <LibJS/Runtime/AsyncDisposableStackConstructor.h>

namespace JS {

GC_DEFINE_ALLOCATOR(AsyncDisposableStackConstructor);

AsyncDisposableStackConstructor::AsyncDisposableStackConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.AsyncDisposableStack.as_string(), realm.intrinsics().function_prototype())
{
}

void AsyncDisposableStackConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 12.4.2.1 AsyncDisposableStack.prototype, https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().async_disposable_stack_prototype(), 0);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 12.4.1.1 AsyncDisposableStack ( ), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack
ThrowCompletionOr<Value> AsyncDisposableStackConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.AsyncDisposableStack);
}

// 12.4.1.1 AsyncDisposableStack ( ), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack
ThrowCompletionOr<GC::Ref<Object>> AsyncDisposableStackConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 2. Let asyncDisposableStack be ? OrdinaryCreateFromConstructor(NewTarget, "%AsyncDisposableStack.prototype%", « [[AsyncDisposableState]], [[DisposeCapability]] »).
    // 3. Set asyncDisposableStack.[[AsyncDisposableState]] to pending.
    // 4. Set asyncDisposableStack.[[DisposeCapability]] to NewDisposeCapability().
    // 5. Return asyncDisposableStack.
    return TRY(ordinary_create_from_constructor<AsyncDisposableStack>(vm, new_target, &Intrinsics::async_disposable_stack_prototype, new_dispose_capability()));
}

}
