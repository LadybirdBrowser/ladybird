/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/AsyncDisposableStackConstructor.h>
#include <LibJS/Runtime/AsyncDisposableStackPrototype.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseConstructor.h>

namespace JS {

GC_DEFINE_ALLOCATOR(AsyncDisposableStackPrototype);

AsyncDisposableStackPrototype::AsyncDisposableStackPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void AsyncDisposableStackPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.adopt, adopt, 2, attr);
    define_native_function(realm, vm.names.defer, defer, 1, attr);
    define_native_function(realm, vm.names.disposeAsync, dispose_async, 0, attr);
    define_native_accessor(realm, vm.names.disposed, disposed_getter, {}, attr);
    define_native_function(realm, vm.names.move, move_, 0, attr);
    define_native_function(realm, vm.names.use, use, 1, attr);

    // 12.4.3.7 AsyncDisposableStack.prototype [ @@asyncDispose ] (), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype-@@asyncDispose
    define_direct_property(vm.well_known_symbol_async_dispose(), get_without_side_effects(vm.names.disposeAsync), attr);

    // 12.4.3.8 AsyncDisposableStack.prototype [ @@toStringTag ], https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype-@@toStringTag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.AsyncDisposableStack.as_string()), Attribute::Configurable);
}

// 12.4.3.1 AsyncDisposableStack.prototype.adopt( value, onDisposeAsync ), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.adopt
JS_DEFINE_NATIVE_FUNCTION(AsyncDisposableStackPrototype::adopt)
{
    auto& realm = *vm.current_realm();

    auto value = vm.argument(0);
    auto on_dispose_async = vm.argument(1);

    // 1. Let asyncDisposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(asyncDisposableStack, [[AsyncDisposableState]]).
    auto async_disposable_stack = TRY(typed_this_object(vm));

    // 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, throw a ReferenceError exception.
    if (async_disposable_stack->async_disposable_state() == AsyncDisposableStack::AsyncDisposableState::Disposed)
        return vm.throw_completion<ReferenceError>(ErrorType::AsyncDisposableStackAlreadyDisposed);

    // 4. If IsCallable(onDisposeAsync) is false, throw a TypeError exception.
    if (!on_dispose_async.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, on_dispose_async);

    // 5. Let closure be a new Abstract Closure with no parameters that captures value and onDisposeAsync and performs the following steps when called:
    auto closure = [value, on_dispose_async](VM& vm) mutable -> ThrowCompletionOr<Value> {
        // a. Return ? Call(onDisposeAsync, undefined, « value »).
        return TRY(call(vm, on_dispose_async.as_function(), js_undefined(), value));
    };

    // 6. Let F be CreateBuiltinFunction(closure, 0, "", « »).
    auto function = NativeFunction::create(realm, move(closure), 0);

    // 7. Perform ? AddDisposableResource(asyncDisposableStack.[[DisposeCapability]], undefined, async-dispose, F).
    TRY(add_disposable_resource(vm, async_disposable_stack->dispose_capability(), js_undefined(), Environment::InitializeBindingHint::AsyncDispose, function));

    // 8. Return value.
    return value;
}

// 12.4.3.2 AsyncDisposableStack.prototype.defer( onDisposeAsync ), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.defer
JS_DEFINE_NATIVE_FUNCTION(AsyncDisposableStackPrototype::defer)
{
    auto on_dispose_async = vm.argument(0);

    // 1. Let asyncDisposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(asyncDisposableStack, [[AsyncDisposableState]]).
    auto async_disposable_stack = TRY(typed_this_object(vm));

    // 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, throw a ReferenceError exception.
    if (async_disposable_stack->async_disposable_state() == AsyncDisposableStack::AsyncDisposableState::Disposed)
        return vm.throw_completion<ReferenceError>(ErrorType::AsyncDisposableStackAlreadyDisposed);

    // 4. If IsCallable(onDisposeAsync) is false, throw a TypeError exception.
    if (!on_dispose_async.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, on_dispose_async);

    // 5. Perform ? AddDisposableResource(asyncDisposableStack.[[DisposeCapability]], undefined, async-dispose, onDisposeAsync).
    TRY(add_disposable_resource(vm, async_disposable_stack->dispose_capability(), js_undefined(), Environment::InitializeBindingHint::AsyncDispose, on_dispose_async.as_function()));

    // 6. Return undefined.
    return js_undefined();
}

// 12.4.3.3 AsyncDisposableStack.prototype.disposeAsync(), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.disposeAsync
JS_DEFINE_NATIVE_FUNCTION(AsyncDisposableStackPrototype::dispose_async)
{
    auto& realm = *vm.current_realm();

    // 1. Let asyncDisposableStack be the this value.
    auto async_disposable_stack_value = vm.this_value();

    // 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
    auto promise_capability = MUST(new_promise_capability(vm, realm.intrinsics().promise_constructor()));

    // 3. If asyncDisposableStack does not have an [[AsyncDisposableState]] internal slot, then
    if (!async_disposable_stack_value.is_object() || !is<AsyncDisposableStack>(async_disposable_stack_value.as_object())) {
        // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « a newly created TypeError object »).
        auto error = TypeError::create(realm, MUST(String::formatted(ErrorType::NotAnObjectOfType.message(), display_name())));
        MUST(call(vm, *promise_capability->reject(), js_undefined(), error));

        // b. Return promiseCapability.[[Promise]].
        return promise_capability->promise();
    }

    auto& async_disposable_stack = static_cast<AsyncDisposableStack&>(async_disposable_stack_value.as_object());

    // 4. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, then
    if (async_disposable_stack.async_disposable_state() == AsyncDisposableStack::AsyncDisposableState::Disposed) {
        // a. Perform ! Call(promiseCapability.[[Resolve]], undefined, « undefined »).
        MUST(call(vm, *promise_capability->resolve(), js_undefined(), js_undefined()));

        // b. Return promiseCapability.[[Promise]].
        return promise_capability->promise();
    }

    // 5. Set asyncDisposableStack.[[AsyncDisposableState]] to disposed.
    async_disposable_stack.set_disposed();

    // 6. Let result be DisposeResources(asyncDisposableStack.[[DisposeCapability]], NormalCompletion(undefined)).
    // 7. IfAbruptRejectPromise(result, promiseCapability).
    auto result = TRY_OR_REJECT(vm, promise_capability, dispose_resources(vm, async_disposable_stack.dispose_capability(), normal_completion(js_undefined())));

    // 8. Perform ! Call(promiseCapability.[[Resolve]], undefined, « result »).
    MUST(call(vm, *promise_capability->resolve(), js_undefined(), *result));

    // 9. Return promiseCapability.[[Promise]].
    return promise_capability->promise();
}

// 12.4.3.4 get AsyncDisposableStack.prototype.disposed, https://tc39.es/proposal-explicit-resource-management/#sec-get-asyncdisposablestack.prototype.disposed
JS_DEFINE_NATIVE_FUNCTION(AsyncDisposableStackPrototype::disposed_getter)
{
    // 1. Let asyncDisposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(asyncDisposableStack, [[AsyncDisposableState]]).
    auto async_disposable_stack = TRY(typed_this_object(vm));

    // 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, return true.
    if (async_disposable_stack->async_disposable_state() == AsyncDisposableStack::AsyncDisposableState::Disposed)
        return true;

    // 4. Otherwise, return false.
    return false;
}

// 12.4.3.5 AsyncDisposableStack.prototype.move(), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.move
JS_DEFINE_NATIVE_FUNCTION(AsyncDisposableStackPrototype::move_)
{
    auto& realm = *vm.current_realm();

    // 1. Let asyncDisposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(asyncDisposableStack, [[AsyncDisposableState]]).
    auto async_disposable_stack = TRY(typed_this_object(vm));

    // 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, throw a ReferenceError exception.
    if (async_disposable_stack->async_disposable_state() == AsyncDisposableStack::AsyncDisposableState::Disposed)
        return vm.throw_completion<ReferenceError>(ErrorType::AsyncDisposableStackAlreadyDisposed);

    // 4. Let newAsyncDisposableStack be ? OrdinaryCreateFromConstructor(%AsyncDisposableStack%, "%AsyncDisposableStack.prototype%", « [[AsyncDisposableState]], [[DisposeCapability]] »).
    // 5. Set newAsyncDisposableStack.[[AsyncDisposableState]] to pending.
    // 6. Set newAsyncDisposableStack.[[DisposeCapability]] to asyncDisposableStack.[[DisposeCapability]].
    auto new_async_disposable_stack = TRY(ordinary_create_from_constructor<AsyncDisposableStack>(vm, realm.intrinsics().async_disposable_stack_constructor(), &Intrinsics::async_disposable_stack_prototype, move(async_disposable_stack->dispose_capability())));

    // 7. Set asyncDisposableStack.[[DisposeCapability]] to NewDisposeCapability().
    async_disposable_stack->dispose_capability() = new_dispose_capability();

    // 8. Set asyncDisposableStack.[[AsyncDisposableState]] to disposed.
    async_disposable_stack->set_disposed();

    // 9. Return newAsyncDisposableStack.
    return new_async_disposable_stack;
}

// 12.4.3.6 AsyncDisposableStack.prototype.use( value ), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.use
JS_DEFINE_NATIVE_FUNCTION(AsyncDisposableStackPrototype::use)
{
    auto value = vm.argument(0);

    // 1. Let asyncDisposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(asyncDisposableStack, [[AsyncDisposableState]]).
    auto async_disposable_stack = TRY(typed_this_object(vm));

    // 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, throw a ReferenceError exception.
    if (async_disposable_stack->async_disposable_state() == AsyncDisposableStack::AsyncDisposableState::Disposed)
        return vm.throw_completion<ReferenceError>(ErrorType::AsyncDisposableStackAlreadyDisposed);

    // 4. Perform ? AddDisposableResource(asyncDisposableStack.[[DisposeCapability]], value, async-dispose).
    TRY(add_disposable_resource(vm, async_disposable_stack->dispose_capability(), value, Environment::InitializeBindingHint::AsyncDispose));

    // 5. Return value.
    return value;
}

}
