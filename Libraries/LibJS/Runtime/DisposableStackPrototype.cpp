/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/DisposableStack.h>
#include <LibJS/Runtime/DisposableStackConstructor.h>
#include <LibJS/Runtime/DisposableStackPrototype.h>
#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

JS_DEFINE_ALLOCATOR(DisposableStackPrototype);

DisposableStackPrototype::DisposableStackPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void DisposableStackPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;

    define_native_accessor(realm, vm.names.disposed, disposed_getter, {}, attr);
    define_native_function(realm, vm.names.dispose, dispose, 0, attr);
    define_native_function(realm, vm.names.use, use, 1, attr);
    define_native_function(realm, vm.names.adopt, adopt, 2, attr);
    define_native_function(realm, vm.names.defer, defer, 1, attr);
    define_native_function(realm, vm.names.move, move_, 0, attr);

    // 11.3.3.7 DisposableStack.prototype [ @@dispose ] (), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype-@@dispose
    define_direct_property(vm.well_known_symbol_dispose(), get_without_side_effects(vm.names.dispose), attr);

    // 11.3.3.8 DisposableStack.prototype [ @@toStringTag ], https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype-@@toStringTag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.DisposableStack.as_string()), Attribute::Configurable);
}

// 11.3.3.1 get DisposableStack.prototype.disposed, https://tc39.es/proposal-explicit-resource-management/#sec-get-disposablestack.prototype.disposed
JS_DEFINE_NATIVE_FUNCTION(DisposableStackPrototype::disposed_getter)
{
    // 1. Let disposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
    auto disposable_stack = TRY(typed_this_object(vm));

    // 3. If disposableStack.[[DisposableState]] is disposed, return true.
    if (disposable_stack->disposable_state() == DisposableStack::DisposableState::Disposed)
        return Value(true);

    // 4. Otherwise, return false.
    return Value(false);
}

// 11.3.3.2 DisposableStack.prototype.dispose (), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.dispose
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

    // 5. Return DisposeResources(disposableStack, NormalCompletion(undefined)).
    return *TRY(dispose_resources(vm, disposable_stack->disposable_resource_stack(), Completion { js_undefined() }));
}

// 11.3.3.3 DisposableStack.prototype.use( value ), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.use
JS_DEFINE_NATIVE_FUNCTION(DisposableStackPrototype::use)
{
    auto value = vm.argument(0);

    // 1. Let disposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
    auto disposable_stack = TRY(typed_this_object(vm));

    // 3. If disposableStack.[[DisposableState]] is disposed, throw a ReferenceError exception.
    if (disposable_stack->disposable_state() == DisposableStack::DisposableState::Disposed)
        return vm.throw_completion<ReferenceError>(ErrorType::DisposableStackAlreadyDisposed);

    // 4. If value is neither null nor undefined, then
    if (!value.is_nullish()) {
        // a. If Type(value) is not Object, throw a TypeError exception.
        if (!value.is_object())
            return vm.throw_completion<TypeError>(ErrorType::NotAnObject, value.to_string_without_side_effects());

        // FIXME: This should be TRY in the spec
        // b. Let method be GetDisposeMethod(value, sync-dispose).
        auto method = TRY(get_dispose_method(vm, value, Environment::InitializeBindingHint::SyncDispose));

        // c. If method is undefined, then
        if (!method.ptr()) {
            // i. Throw a TypeError exception.
            return vm.throw_completion<TypeError>(ErrorType::NoDisposeMethod, value.to_string_without_side_effects());
        }
        // d. Else,
        // i. Perform ? AddDisposableResource(disposableStack, value, sync-dispose, method).
        // FIXME: Fairly sure this can't fail, see https://github.com/tc39/proposal-explicit-resource-management/pull/142
        MUST(add_disposable_resource(vm, disposable_stack->disposable_resource_stack(), value, Environment::InitializeBindingHint::SyncDispose, method));
    }

    // 5. Return value.
    return value;
}

// 11.3.3.4 DisposableStack.prototype.adopt( value, onDispose ), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.adopt
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
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, on_dispose.to_string_without_side_effects());

    // 5. Let F be a new built-in function object as defined in 11.3.3.4.1.
    // 6. Set F.[[Argument]] to value.
    // 7. Set F.[[OnDisposeCallback]] to onDispose.
    // 11.3.3.4.1 DisposableStack Adopt Callback Functions, https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack-adopt-callback-functions
    // A DisposableStack adopt callback function is an anonymous built-in function object that has [[Argument]] and [[OnDisposeCallback]] internal slots.
    auto function = NativeFunction::create(
        realm, [argument = make_handle(value), callback = make_handle(on_dispose)](VM& vm) {
            // When a DisposableStack adopt callback function is called, the following steps are taken:
            // 1. Let F be the active function object.
            // 2. Assert: IsCallable(F.[[OnDisposeCallback]]) is true.
            VERIFY(callback.value().is_function());

            // 3. Return Call(F.[[OnDisposeCallback]], undefined, « F.[[Argument]] »).
            return call(vm, callback.value(), js_undefined(), argument.value());
        },
        0, "");

    // 8. Perform ? AddDisposableResource(disposableStack, undefined, sync-dispose, F).
    TRY(add_disposable_resource(vm, disposable_stack->disposable_resource_stack(), js_undefined(), Environment::InitializeBindingHint::SyncDispose, function));

    // 9. Return value.
    return value;
}

// 11.3.3.5 DisposableStack.prototype.defer( onDispose ), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.defer
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
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, on_dispose.to_string_without_side_effects());

    // 5. Perform ? AddDisposableResource(disposableStack, undefined, sync-dispose, onDispose).
    TRY(add_disposable_resource(vm, disposable_stack->disposable_resource_stack(), js_undefined(), Environment::InitializeBindingHint::SyncDispose, &on_dispose.as_function()));

    // 6. Return undefined.
    return js_undefined();
}

// 11.3.3.6 DisposableStack.prototype.move(), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.move
JS_DEFINE_NATIVE_FUNCTION(DisposableStackPrototype::move_)
{
    // 1. Let disposableStack be the this value.
    // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
    auto disposable_stack = TRY(typed_this_object(vm));

    // 3. If disposableStack.[[DisposableState]] is disposed, throw a ReferenceError exception.
    if (disposable_stack->disposable_state() == DisposableStack::DisposableState::Disposed)
        return vm.throw_completion<ReferenceError>(ErrorType::DisposableStackAlreadyDisposed);

    // 4. Let newDisposableStack be ? OrdinaryCreateFromConstructor(%DisposableStack%, "%DisposableStack.prototype%", « [[DisposableState]], [[DisposableResourceStack]] »).
    auto new_disposable_stack = TRY(ordinary_create_from_constructor<DisposableStack>(vm, *vm.current_realm()->intrinsics().disposable_stack_constructor(), &Intrinsics::disposable_stack_prototype, disposable_stack->disposable_resource_stack()));

    // 5. Set newDisposableStack.[[DisposableState]] to pending.
    // 6. Set newDisposableStack.[[DisposableResourceStack]] to disposableStack.[[DisposableResourceStack]].
    // NOTE: Already done in the constructor

    // 7. Set disposableStack.[[DisposableResourceStack]] to a new empty List.
    disposable_stack->disposable_resource_stack().clear();

    // 8. Set disposableStack.[[DisposableState]] to disposed.
    disposable_stack->set_disposed();

    // 9. Return newDisposableStack.
    return new_disposable_stack;
}

}
