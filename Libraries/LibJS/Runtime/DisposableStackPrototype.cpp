/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/DisposableStackConstructor.h>
#include <LibJS/Runtime/DisposableStackPrototype.h>
#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

GC_DEFINE_ALLOCATOR(DisposableStackPrototype);

DisposableStackPrototype::DisposableStackPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void DisposableStackPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.adopt, adopt, 2, attr);
    define_native_function(realm, vm.names.defer, defer, 1, attr);
    define_native_function(realm, vm.names.dispose, dispose, 0, attr);
    define_native_accessor(realm, vm.names.disposed, disposed_getter, {}, attr);
    define_native_function(realm, vm.names.move, move_, 0, attr);
    define_native_function(realm, vm.names.use, use, 1, attr);

    // 12.3.3.7 DisposableStack.prototype [ @@dispose ] (), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype-@@dispose
    define_direct_property(vm.well_known_symbol_dispose(), get_without_side_effects(vm.names.dispose), attr);

    // 12.3.3.8 DisposableStack.prototype [ @@toStringTag ], https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype-@@toStringTag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.DisposableStack.as_string()), Attribute::Configurable);
}

// 12.3.3.1 DisposableStack.prototype.adopt( value, onDispose ), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.adopt
JS_DEFINE_NATIVE_FUNCTION(DisposableStackPrototype::adopt)
{
    auto& realm = *vm.current_realm();

    auto value = vm.argument(0);
    auto on_dispose = vm.argument(1);

    // 1. Let disposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
    auto disposable_stack = TRY(typed_this_object(vm));

    // 3. If disposableStack.[[DisposableState]] is disposed, throw a ReferenceError exception.
    if (disposable_stack->disposable_state() == DisposableStack::DisposableState::Disposed)
        return vm.throw_completion<ReferenceError>(ErrorType::DisposableStackAlreadyDisposed);

    // 4. If IsCallable(onDispose) is false, throw a TypeError exception.
    if (!on_dispose.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, on_dispose);

    // 5. Let closure be a new Abstract Closure with no parameters that captures value and onDispose and performs the following steps when called:
    auto closure = [value, on_dispose](VM& vm) mutable -> ThrowCompletionOr<Value> {
        //     a. Return ? Call(onDispose, undefined, « value »).
        return TRY(call(vm, on_dispose.as_function(), js_undefined(), value));
    };

    // 6. Let F be CreateBuiltinFunction(closure, 0, "", « »).
    auto function = NativeFunction::create(realm, move(closure), 0);

    // 7. Perform ? AddDisposableResource(disposableStack.[[DisposeCapability]], undefined, sync-dispose, F).
    TRY(add_disposable_resource(vm, disposable_stack->dispose_capability(), js_undefined(), Environment::InitializeBindingHint::SyncDispose, function));

    // 8. Return value.
    return value;
}

// 12.3.3.2 DisposableStack.prototype.defer( onDispose ), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.defer
JS_DEFINE_NATIVE_FUNCTION(DisposableStackPrototype::defer)
{
    auto on_dispose = vm.argument(0);

    // 1. Let disposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
    auto disposable_stack = TRY(typed_this_object(vm));

    // 3. If disposableStack.[[DisposableState]] is disposed, throw a ReferenceError exception.
    if (disposable_stack->disposable_state() == DisposableStack::DisposableState::Disposed)
        return vm.throw_completion<ReferenceError>(ErrorType::DisposableStackAlreadyDisposed);

    // 4. If IsCallable(onDispose) is false, throw a TypeError exception.
    if (!on_dispose.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, on_dispose);

    // 5. Perform ? AddDisposableResource(disposableStack.[[DisposeCapability]], undefined, sync-dispose, onDispose).
    TRY(add_disposable_resource(vm, disposable_stack->dispose_capability(), js_undefined(), Environment::InitializeBindingHint::SyncDispose, on_dispose.as_function()));

    // 6. Return undefined.
    return js_undefined();
}

// 12.3.3.3 DisposableStack.prototype.dispose (), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.dispose
JS_DEFINE_NATIVE_FUNCTION(DisposableStackPrototype::dispose)
{
    // 1. Let disposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
    auto disposable_stack = TRY(typed_this_object(vm));

    // 3. If disposableStack.[[DisposableState]] is disposed, return undefined.
    if (disposable_stack->disposable_state() == DisposableStack::DisposableState::Disposed)
        return js_undefined();

    // 4. Set disposableStack.[[DisposableState]] to disposed.
    disposable_stack->set_disposed();

    // 5. Return DisposeResources(disposableStack.[[DisposeCapability]], NormalCompletion(undefined)).
    return *TRY(dispose_resources(vm, disposable_stack->dispose_capability(), normal_completion(js_undefined())));
}

// 12.3.3.4 get DisposableStack.prototype.disposed, https://tc39.es/proposal-explicit-resource-management/#sec-get-disposablestack.prototype.disposed
JS_DEFINE_NATIVE_FUNCTION(DisposableStackPrototype::disposed_getter)
{
    // 1. Let disposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
    auto disposable_stack = TRY(typed_this_object(vm));

    // 3. If disposableStack.[[DisposableState]] is disposed, return true.
    if (disposable_stack->disposable_state() == DisposableStack::DisposableState::Disposed)
        return true;

    // 4. Otherwise, return false.
    return false;
}

// 12.3.3.5 DisposableStack.prototype.move(), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.move
JS_DEFINE_NATIVE_FUNCTION(DisposableStackPrototype::move_)
{
    auto& realm = *vm.current_realm();

    // 1. Let disposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
    auto disposable_stack = TRY(typed_this_object(vm));

    // 3. If disposableStack.[[DisposableState]] is disposed, throw a ReferenceError exception.
    if (disposable_stack->disposable_state() == DisposableStack::DisposableState::Disposed)
        return vm.throw_completion<ReferenceError>(ErrorType::DisposableStackAlreadyDisposed);

    // 4. Let newDisposableStack be ? OrdinaryCreateFromConstructor(%DisposableStack%, "%DisposableStack.prototype%", « [[DisposableState]], [[DisposeCapability]] »).
    // 5. Set newDisposableStack.[[DisposableState]] to pending.
    // 6. Set newDisposableStack.[[DisposeCapability]] to disposableStack.[[DisposeCapability]].
    auto new_disposable_stack = TRY(ordinary_create_from_constructor<DisposableStack>(vm, realm.intrinsics().disposable_stack_constructor(), &Intrinsics::disposable_stack_prototype, move(disposable_stack->dispose_capability())));

    // 7. Set disposableStack.[[DisposeCapability]] to NewDisposeCapability().
    disposable_stack->dispose_capability() = new_dispose_capability();

    // 8. Set disposableStack.[[DisposableState]] to disposed.
    disposable_stack->set_disposed();

    // 9. Return newDisposableStack.
    return new_disposable_stack;
}

// 12.3.3.6 DisposableStack.prototype.use( value ), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.use
JS_DEFINE_NATIVE_FUNCTION(DisposableStackPrototype::use)
{
    auto value = vm.argument(0);

    // 1. Let disposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
    auto disposable_stack = TRY(typed_this_object(vm));

    // 3. If disposableStack.[[DisposableState]] is disposed, throw a ReferenceError exception.
    if (disposable_stack->disposable_state() == DisposableStack::DisposableState::Disposed)
        return vm.throw_completion<ReferenceError>(ErrorType::DisposableStackAlreadyDisposed);

    // 4. Perform ? AddDisposableResource(disposableStack.[[DisposeCapability]], value, sync-dispose).
    TRY(add_disposable_resource(vm, disposable_stack->dispose_capability(), value, Environment::InitializeBindingHint::SyncDispose));

    // 5. Return value.
    return value;
}

}
