/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibJS/Runtime/PromisePrototype.h>

namespace JS {

JS_DEFINE_ALLOCATOR(PromisePrototype);

PromisePrototype::PromisePrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void PromisePrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.then, then, 2, attr);
    define_native_function(realm, vm.names.catch_, catch_, 1, attr);
    define_native_function(realm, vm.names.finally, finally, 1, attr);

    // 27.2.5.5 Promise.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-promise.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.Promise.as_string()), Attribute::Configurable);
}

// 27.2.5.4 Promise.prototype.then ( onFulfilled, onRejected ), https://tc39.es/ecma262/#sec-promise.prototype.then
JS_DEFINE_NATIVE_FUNCTION(PromisePrototype::then)
{
    auto& realm = *vm.current_realm();

    auto on_fulfilled = vm.argument(0);
    auto on_rejected = vm.argument(1);

    // 1. Let promise be the this value.
    // 2. If IsPromise(promise) is false, throw a TypeError exception.
    auto promise = TRY(typed_this_object(vm));

    // 3. Let C be ? SpeciesConstructor(promise, %Promise%).
    auto* constructor = TRY(species_constructor(vm, promise, realm.intrinsics().promise_constructor()));

    // 4. Let resultCapability be ? NewPromiseCapability(C).
    auto result_capability = TRY(new_promise_capability(vm, constructor));

    // 5. Return PerformPromiseThen(promise, onFulfilled, onRejected, resultCapability).
    return promise->perform_then(on_fulfilled, on_rejected, result_capability);
}

// 27.2.5.1 Promise.prototype.catch ( onRejected ), https://tc39.es/ecma262/#sec-promise.prototype.catch
JS_DEFINE_NATIVE_FUNCTION(PromisePrototype::catch_)
{
    auto on_rejected = vm.argument(0);

    // 1. Let promise be the this value.
    auto this_value = vm.this_value();

    // 2. Return ? Invoke(promise, "then", « undefined, onRejected »).
    return TRY(this_value.invoke(vm, vm.names.then, js_undefined(), on_rejected));
}

// 27.2.5.3 Promise.prototype.finally ( onFinally ), https://tc39.es/ecma262/#sec-promise.prototype.finally
JS_DEFINE_NATIVE_FUNCTION(PromisePrototype::finally)
{
    auto& realm = *vm.current_realm();

    auto on_finally = vm.argument(0);

    // 1. Let promise be the this value.
    auto promise = vm.this_value();

    // 2. If Type(promise) is not Object, throw a TypeError exception.
    if (!promise.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, promise.to_string_without_side_effects());

    // 3. Let C be ? SpeciesConstructor(promise, %Promise%).
    auto* constructor = TRY(species_constructor(vm, promise.as_object(), realm.intrinsics().promise_constructor()));

    // 4. Assert: IsConstructor(C) is true.
    VERIFY(constructor);

    Value then_finally;
    Value catch_finally;

    // 5. If IsCallable(onFinally) is false, then
    if (!on_finally.is_function()) {
        // a. Let thenFinally be onFinally.
        then_finally = on_finally;

        // b. Let catchFinally be onFinally.
        catch_finally = on_finally;
    }
    // 6. Else,
    else {
        // a. Let thenFinallyClosure be a new Abstract Closure with parameters (value) that captures onFinally and C and performs the following steps when called:
        auto then_finally_closure = [constructor, on_finally](auto& vm) -> ThrowCompletionOr<Value> {
            auto& realm = *vm.current_realm();
            auto value = vm.argument(0);

            // i. Let result be ? Call(onFinally, undefined).
            auto result = TRY(call(vm, on_finally, js_undefined()));

            // ii. Let promise be ? PromiseResolve(C, result).
            auto promise = TRY(promise_resolve(vm, *constructor, result));

            // iii. Let returnValue be a new Abstract Closure with no parameters that captures value and performs the following steps when called:
            auto return_value = [value](auto&) -> ThrowCompletionOr<Value> {
                // 1. Return value.
                return value;
            };

            // iv. Let valueThunk be CreateBuiltinFunction(returnValue, 0, "", « »).
            auto value_thunk = NativeFunction::create(realm, move(return_value), 0, "");

            // v. Return ? Invoke(promise, "then", « valueThunk »).
            return TRY(Value(promise).invoke(vm, vm.names.then, value_thunk));
        };

        // b. Let thenFinally be CreateBuiltinFunction(thenFinallyClosure, 1, "", « »).
        then_finally = NativeFunction::create(realm, move(then_finally_closure), 1, "");

        // c. Let catchFinallyClosure be a new Abstract Closure with parameters (reason) that captures onFinally and C and performs the following steps when called:
        auto catch_finally_closure = [constructor, on_finally](auto& vm) -> ThrowCompletionOr<Value> {
            auto& realm = *vm.current_realm();
            auto reason = vm.argument(0);

            // i. Let result be ? Call(onFinally, undefined).
            auto result = TRY(call(vm, on_finally, js_undefined()));

            // ii. Let promise be ? PromiseResolve(C, result).
            auto promise = TRY(promise_resolve(vm, *constructor, result));

            // iii. Let throwReason be a new Abstract Closure with no parameters that captures reason and performs the following steps when called:
            auto throw_reason = [reason](auto&) -> ThrowCompletionOr<Value> {
                // 1. Return ThrowCompletion(reason).
                return throw_completion(reason);
            };

            // iv. Let thrower be CreateBuiltinFunction(throwReason, 0, "", « »).
            auto thrower = NativeFunction::create(realm, move(throw_reason), 0, "");

            // v. Return ? Invoke(promise, "then", « thrower »).
            return TRY(Value(promise).invoke(vm, vm.names.then, thrower));
        };

        // d. Let catchFinally be CreateBuiltinFunction(catchFinallyClosure, 1, "", « »).
        catch_finally = NativeFunction::create(realm, move(catch_finally_closure), 1, "");
    }

    // 7. Return ? Invoke(promise, "then", « thenFinally, catchFinally »).
    return TRY(promise.invoke(vm, vm.names.then, then_finally, catch_finally));
}

}
