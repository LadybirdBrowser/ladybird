/*
 * Copyright (c) 2021-2024, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/AggregateError.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibJS/Runtime/PromiseResolvingElementFunctions.h>

namespace JS {

JS_DEFINE_ALLOCATOR(PromiseConstructor);

// 27.2.4.1.1 GetPromiseResolve ( promiseConstructor ), https://tc39.es/ecma262/#sec-getpromiseresolve
static ThrowCompletionOr<Value> get_promise_resolve(VM& vm, Value constructor)
{
    VERIFY(constructor.is_constructor());

    // 1. Let promiseResolve be ? Get(promiseConstructor, "resolve").
    auto promise_resolve = TRY(constructor.get(vm, vm.names.resolve));

    // 2. If IsCallable(promiseResolve) is false, throw a TypeError exception.
    if (!promise_resolve.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, promise_resolve.to_string_without_side_effects());

    // 3. Return promiseResolve.
    return promise_resolve;
}

using EndOfElementsCallback = Function<ThrowCompletionOr<Value>(PromiseValueList&)>;
using InvokeElementFunctionCallback = Function<ThrowCompletionOr<Value>(PromiseValueList&, RemainingElements&, Value, size_t)>;

static ThrowCompletionOr<Value> perform_promise_common(VM& vm, IteratorRecord& iterator_record, Value constructor, PromiseCapability const& result_capability, Value promise_resolve, EndOfElementsCallback end_of_list, InvokeElementFunctionCallback invoke_element_function)
{
    VERIFY(constructor.is_constructor());
    VERIFY(promise_resolve.is_function());

    // 1. Let values be a new empty List.
    auto values = vm.heap().allocate_without_realm<PromiseValueList>();

    // 2. Let remainingElementsCount be the Record { [[Value]]: 1 }.
    auto remaining_elements_count = vm.heap().allocate_without_realm<RemainingElements>(1);

    // 3. Let index be 0.
    size_t index = 0;

    // 4. Repeat,
    while (true) {
        // a. Let next be ? IteratorStepValue(iteratorRecord).
        auto next = TRY(iterator_step_value(vm, iterator_record));

        // b. If next is DONE, then
        if (!next.has_value()) {
            // i. Set remainingElementsCount.[[Value]] to remainingElementsCount.[[Value]] - 1.
            // ii. If remainingElementsCount.[[Value]] = 0, then
            if (--remaining_elements_count->value == 0) {
                // 1-2. are handled in `end_of_list`
                return TRY(end_of_list(*values));
            }

            // iii. Return resultCapability.[[Promise]].
            return result_capability.promise();
        }

        // c. Append undefined to values.
        values->values().append(js_undefined());

        // d. Let nextPromise be ? Call(promiseResolve, constructor, « next »).
        auto next_promise = TRY(call(vm, promise_resolve.as_function(), constructor, next.release_value()));

        // e-l. are handled in `invoke_element_function`

        // m. Set remainingElementsCount.[[Value]] to remainingElementsCount.[[Value]] + 1.
        ++remaining_elements_count->value;

        // n. Perform ? Invoke(nextPromise, "then", « ... »).
        TRY(invoke_element_function(*values, *remaining_elements_count, next_promise, index));

        // o. Set index to index + 1.
        ++index;
    }
}

// 27.2.4.1.2 PerformPromiseAll ( iteratorRecord, constructor, resultCapability, promiseResolve ), https://tc39.es/ecma262/#sec-performpromiseall
static ThrowCompletionOr<Value> perform_promise_all(VM& vm, IteratorRecord& iterator_record, Value constructor, PromiseCapability const& result_capability, Value promise_resolve)
{
    auto& realm = *vm.current_realm();

    return perform_promise_common(
        vm, iterator_record, constructor, result_capability, promise_resolve,
        [&](PromiseValueList& values) -> ThrowCompletionOr<Value> {
            // 1. Let valuesArray be CreateArrayFromList(values).
            auto values_array = Array::create_from(realm, values.values());

            // 2. Perform ? Call(resultCapability.[[Resolve]], undefined, « valuesArray »).
            TRY(call(vm, *result_capability.resolve(), js_undefined(), values_array));

            // iv. Return resultCapability.[[Promise]].
            return result_capability.promise();
        },
        [&](PromiseValueList& values, RemainingElements& remaining_elements_count, Value next_promise, size_t index) {
            // j. Let steps be the algorithm steps defined in Promise.all Resolve Element Functions.
            // k. Let length be the number of non-optional parameters of the function definition in Promise.all Resolve Element Functions.
            // l. Let onFulfilled be CreateBuiltinFunction(steps, length, "", « [[AlreadyCalled]], [[Index]], [[Values]], [[Capability]], [[RemainingElements]] »).
            // m. Set onFulfilled.[[AlreadyCalled]] to false.
            // n. Set onFulfilled.[[Index]] to index.
            // o. Set onFulfilled.[[Values]] to values.
            // p. Set onFulfilled.[[Capability]] to resultCapability.
            // q. Set onFulfilled.[[RemainingElements]] to remainingElementsCount.
            auto on_fulfilled = PromiseAllResolveElementFunction::create(realm, index, values, result_capability, remaining_elements_count);
            on_fulfilled->define_direct_property(vm.names.name, PrimitiveString::create(vm, String {}), Attribute::Configurable);

            // s. Perform ? Invoke(nextPromise, "then", « onFulfilled, resultCapability.[[Reject]] »).
            return next_promise.invoke(vm, vm.names.then, on_fulfilled, result_capability.reject());
        });
}

// 27.2.4.2.1 PerformPromiseAllSettled ( iteratorRecord, constructor, resultCapability, promiseResolve ), https://tc39.es/ecma262/#sec-performpromiseallsettled
static ThrowCompletionOr<Value> perform_promise_all_settled(VM& vm, IteratorRecord& iterator_record, Value constructor, PromiseCapability const& result_capability, Value promise_resolve)
{
    auto& realm = *vm.current_realm();

    return perform_promise_common(
        vm, iterator_record, constructor, result_capability, promise_resolve,
        [&](PromiseValueList& values) -> ThrowCompletionOr<Value> {
            auto values_array = Array::create_from(realm, values.values());

            TRY(call(vm, *result_capability.resolve(), js_undefined(), values_array));

            return result_capability.promise();
        },
        [&](PromiseValueList& values, RemainingElements& remaining_elements_count, Value next_promise, size_t index) {
            // j. Let stepsFulfilled be the algorithm steps defined in Promise.allSettled Resolve Element Functions.
            // k. Let lengthFulfilled be the number of non-optional parameters of the function definition in Promise.allSettled Resolve Element Functions.
            // l. Let onFulfilled be CreateBuiltinFunction(stepsFulfilled, lengthFulfilled, "", « [[AlreadyCalled]], [[Index]], [[Values]], [[Capability]], [[RemainingElements]] »).
            // m. Let alreadyCalled be the Record { [[Value]]: false }.
            // n. Set onFulfilled.[[AlreadyCalled]] to alreadyCalled.
            // o. Set onFulfilled.[[Index]] to index.
            // p. Set onFulfilled.[[Values]] to values.
            // q. Set onFulfilled.[[Capability]] to resultCapability.
            // r. Set onFulfilled.[[RemainingElements]] to remainingElementsCount.
            auto on_fulfilled = PromiseAllSettledResolveElementFunction::create(realm, index, values, result_capability, remaining_elements_count);
            on_fulfilled->define_direct_property(vm.names.name, PrimitiveString::create(vm, String {}), Attribute::Configurable);

            // s. Let stepsRejected be the algorithm steps defined in Promise.allSettled Reject Element Functions.
            // t. Let lengthRejected be the number of non-optional parameters of the function definition in Promise.allSettled Reject Element Functions.
            // u. Let onRejected be CreateBuiltinFunction(stepsRejected, lengthRejected, "", « [[AlreadyCalled]], [[Index]], [[Values]], [[Capability]], [[RemainingElements]] »).
            // v. Set onRejected.[[AlreadyCalled]] to alreadyCalled.
            // w. Set onRejected.[[Index]] to index.
            // x. Set onRejected.[[Values]] to values.
            // y. Set onRejected.[[Capability]] to resultCapability.
            // z. Set onRejected.[[RemainingElements]] to remainingElementsCount.
            auto on_rejected = PromiseAllSettledRejectElementFunction::create(realm, index, values, result_capability, remaining_elements_count);
            on_rejected->define_direct_property(vm.names.name, PrimitiveString::create(vm, String {}), Attribute::Configurable);

            // ab. Perform ? Invoke(nextPromise, "then", « onFulfilled, onRejected »).
            return next_promise.invoke(vm, vm.names.then, on_fulfilled, on_rejected);
        });
}

// 27.2.4.3.1 PerformPromiseAny ( iteratorRecord, constructor, resultCapability, promiseResolve ), https://tc39.es/ecma262/#sec-performpromiseany
static ThrowCompletionOr<Value> perform_promise_any(VM& vm, IteratorRecord& iterator_record, Value constructor, PromiseCapability& result_capability, Value promise_resolve)
{
    auto& realm = *vm.current_realm();

    return perform_promise_common(
        vm, iterator_record, constructor, result_capability, promise_resolve,
        [&](PromiseValueList& errors) -> ThrowCompletionOr<Value> {
            // 1. Let error be a newly created AggregateError object.
            auto error = AggregateError::create(realm);

            // 2. Perform ! DefinePropertyOrThrow(error, "errors", PropertyDescriptor { [[Configurable]]: true, [[Enumerable]]: false, [[Writable]]: true, [[Value]]: CreateArrayFromList(errors) }).
            auto errors_array = Array::create_from(realm, errors.values());
            MUST(error->define_property_or_throw(vm.names.errors, { .value = errors_array, .writable = true, .enumerable = false, .configurable = true }));

            // 3. Return ThrowCompletion(error).
            return throw_completion(error);
        },
        [&](PromiseValueList& errors, RemainingElements& remaining_elements_count, Value next_promise, size_t index) {
            // j. Let stepsRejected be the algorithm steps defined in Promise.any Reject Element Functions.
            // k. Let lengthRejected be the number of non-optional parameters of the function definition in Promise.any Reject Element Functions.
            // l. Let onRejected be CreateBuiltinFunction(stepsRejected, lengthRejected, "", « [[AlreadyCalled]], [[Index]], [[Errors]], [[Capability]], [[RemainingElements]] »).
            // m. Set onRejected.[[AlreadyCalled]] to false.
            // n. Set onRejected.[[Index]] to index.
            // o. Set onRejected.[[Errors]] to errors.
            // p. Set onRejected.[[Capability]] to resultCapability.
            // q. Set onRejected.[[RemainingElements]] to remainingElementsCount.
            auto on_rejected = PromiseAnyRejectElementFunction::create(realm, index, errors, result_capability, remaining_elements_count);
            on_rejected->define_direct_property(vm.names.name, PrimitiveString::create(vm, String {}), Attribute::Configurable);

            // s. Perform ? Invoke(nextPromise, "then", « resultCapability.[[Resolve]], onRejected »).
            return next_promise.invoke(vm, vm.names.then, result_capability.resolve(), on_rejected);
        });
}

// 27.2.4.5.1 PerformPromiseRace ( iteratorRecord, constructor, resultCapability, promiseResolve ), https://tc39.es/ecma262/#sec-performpromiserace
static ThrowCompletionOr<Value> perform_promise_race(VM& vm, IteratorRecord& iterator_record, Value constructor, PromiseCapability const& result_capability, Value promise_resolve)
{
    return perform_promise_common(
        vm, iterator_record, constructor, result_capability, promise_resolve,
        [&](PromiseValueList&) -> ThrowCompletionOr<Value> {
            // ii. Return resultCapability.[[Promise]].
            return result_capability.promise();
        },
        [&](PromiseValueList&, RemainingElements&, Value next_promise, size_t) {
            // i. Perform ? Invoke(nextPromise, "then", « resultCapability.[[Resolve]], resultCapability.[[Reject]] »).
            return next_promise.invoke(vm, vm.names.then, result_capability.resolve(), result_capability.reject());
        });
}

PromiseConstructor::PromiseConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Promise.as_string(), realm.intrinsics().function_prototype())
{
}

void PromiseConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 27.2.4.4 Promise.prototype, https://tc39.es/ecma262/#sec-promise.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().promise_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.all, all, 1, attr);
    define_native_function(realm, vm.names.allSettled, all_settled, 1, attr);
    define_native_function(realm, vm.names.any, any, 1, attr);
    define_native_function(realm, vm.names.race, race, 1, attr);
    define_native_function(realm, vm.names.reject, reject, 1, attr);
    define_native_function(realm, vm.names.resolve, resolve, 1, attr);
    define_native_function(realm, vm.names.try_, try_, 1, attr);
    define_native_function(realm, vm.names.withResolvers, with_resolvers, 0, attr);

    define_native_accessor(realm, vm.well_known_symbol_species(), symbol_species_getter, {}, Attribute::Configurable);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// 27.2.3.1 Promise ( executor ), https://tc39.es/ecma262/#sec-promise-executor
ThrowCompletionOr<Value> PromiseConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.Promise);
}

// 27.2.3.1 Promise ( executor ), https://tc39.es/ecma262/#sec-promise-executor
ThrowCompletionOr<NonnullGCPtr<Object>> PromiseConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto executor = vm.argument(0);

    // 2. If IsCallable(executor) is false, throw a TypeError exception.
    if (!executor.is_function())
        return vm.throw_completion<TypeError>(ErrorType::PromiseExecutorNotAFunction);

    // 3. Let promise be ? OrdinaryCreateFromConstructor(NewTarget, "%Promise.prototype%", « [[PromiseState]], [[PromiseResult]], [[PromiseFulfillReactions]], [[PromiseRejectReactions]], [[PromiseIsHandled]] »).
    // 4. Set promise.[[PromiseState]] to pending.
    // 5. Set promise.[[PromiseFulfillReactions]] to a new empty List.
    // 6. Set promise.[[PromiseRejectReactions]] to a new empty List.
    // 7. Set promise.[[PromiseIsHandled]] to false.
    auto promise = TRY(ordinary_create_from_constructor<Promise>(vm, new_target, &Intrinsics::promise_prototype));

    // 8. Let resolvingFunctions be CreateResolvingFunctions(promise).
    auto [resolve_function, reject_function] = promise->create_resolving_functions();

    // 9. Let completion be Completion(Call(executor, undefined, « resolvingFunctions.[[Resolve]], resolvingFunctions.[[Reject]] »)).
    auto completion = JS::call(vm, executor.as_function(), js_undefined(), resolve_function, reject_function);

    // 10. If completion is an abrupt completion, then
    if (completion.is_error()) {
        // a. Perform ? Call(resolvingFunctions.[[Reject]], undefined, « completion.[[Value]] »).
        TRY(JS::call(vm, *reject_function, js_undefined(), *completion.release_error().value()));
    }

    // 11. Return promise.
    return promise;
}

// 27.2.4.1 Promise.all ( iterable ), https://tc39.es/ecma262/#sec-promise.all
JS_DEFINE_NATIVE_FUNCTION(PromiseConstructor::all)
{
    // 1. Let C be the this value.
    auto constructor = TRY(vm.this_value().to_object(vm));

    // 2. Let promiseCapability be ? NewPromiseCapability(C).
    auto promise_capability = TRY(new_promise_capability(vm, constructor));

    // 3. Let promiseResolve be Completion(GetPromiseResolve(C)).
    // 4. IfAbruptRejectPromise(promiseResolve, promiseCapability).
    auto promise_resolve = TRY_OR_REJECT(vm, promise_capability, get_promise_resolve(vm, constructor));

    // 5. Let iteratorRecord be Completion(GetIterator(iterable, sync)).
    // 6. IfAbruptRejectPromise(iteratorRecord, promiseCapability).
    auto iterator_record = TRY_OR_REJECT(vm, promise_capability, get_iterator(vm, vm.argument(0), IteratorHint::Sync));

    // 7. Let result be Completion(PerformPromiseAll(iteratorRecord, C, promiseCapability, promiseResolve)).
    auto result = perform_promise_all(vm, iterator_record, constructor, promise_capability, promise_resolve);

    // 8. If result is an abrupt completion, then
    if (result.is_error()) {
        // a. If iteratorRecord.[[Done]] is false, set result to Completion(IteratorClose(iteratorRecord, result)).
        if (!iterator_record->done)
            result = iterator_close(vm, iterator_record, result.release_error());

        // b. IfAbruptRejectPromise(result, promiseCapability).
        TRY_OR_REJECT(vm, promise_capability, result);
    }

    // 9. Return ? result.
    return result;
}

// 27.2.4.2 Promise.allSettled ( iterable ), https://tc39.es/ecma262/#sec-promise.allsettled
JS_DEFINE_NATIVE_FUNCTION(PromiseConstructor::all_settled)
{
    // 1. Let C be the this value.
    auto constructor = TRY(vm.this_value().to_object(vm));

    // 2. Let promiseCapability be ? NewPromiseCapability(C).
    auto promise_capability = TRY(new_promise_capability(vm, constructor));

    // 3. Let promiseResolve be Completion(GetPromiseResolve(C)).
    // 4. IfAbruptRejectPromise(promiseResolve, promiseCapability).
    auto promise_resolve = TRY_OR_REJECT(vm, promise_capability, get_promise_resolve(vm, constructor));

    // 5. Let iteratorRecord be Completion(GetIterator(iterable, sync)).
    // 6. IfAbruptRejectPromise(iteratorRecord, promiseCapability).
    auto iterator_record = TRY_OR_REJECT(vm, promise_capability, get_iterator(vm, vm.argument(0), IteratorHint::Sync));

    // 7. Let result be Completion(PerformPromiseAllSettled(iteratorRecord, C, promiseCapability, promiseResolve)).
    auto result = perform_promise_all_settled(vm, iterator_record, constructor, promise_capability, promise_resolve);

    // 8. If result is an abrupt completion, then
    if (result.is_error()) {
        // a. If iteratorRecord.[[Done]] is false, set result to Completion(IteratorClose(iteratorRecord, result)).
        if (!iterator_record->done)
            result = iterator_close(vm, iterator_record, result.release_error());

        // b. IfAbruptRejectPromise(result, promiseCapability).
        TRY_OR_REJECT(vm, promise_capability, result);
    }

    // 9. Return ? result.
    return result;
}

// 27.2.4.3 Promise.any ( iterable ), https://tc39.es/ecma262/#sec-promise.any
JS_DEFINE_NATIVE_FUNCTION(PromiseConstructor::any)
{
    // 1. Let C be the this value.
    auto constructor = TRY(vm.this_value().to_object(vm));

    // 2. Let promiseCapability be ? NewPromiseCapability(C).
    auto promise_capability = TRY(new_promise_capability(vm, constructor));

    // 3. Let promiseResolve be Completion(GetPromiseResolve(C)).
    // 4. IfAbruptRejectPromise(promiseResolve, promiseCapability).
    auto promise_resolve = TRY_OR_REJECT(vm, promise_capability, get_promise_resolve(vm, constructor));

    // 5. Let iteratorRecord be Completion(GetIterator(iterable, sync)).
    // 6. IfAbruptRejectPromise(iteratorRecord, promiseCapability).
    auto iterator_record = TRY_OR_REJECT(vm, promise_capability, get_iterator(vm, vm.argument(0), IteratorHint::Sync));

    // 7. Let result be Completion(PerformPromiseAny(iteratorRecord, C, promiseCapability, promiseResolve)).
    auto result = perform_promise_any(vm, iterator_record, constructor, promise_capability, promise_resolve);

    // 8. If result is an abrupt completion, then
    if (result.is_error()) {
        // a. If iteratorRecord.[[Done]] is false, set result to Completion(IteratorClose(iteratorRecord, result)).
        if (!iterator_record->done)
            result = iterator_close(vm, iterator_record, result.release_error());

        // b. IfAbruptRejectPromise(result, promiseCapability).
        TRY_OR_REJECT(vm, promise_capability, result);
    }

    // 9. Return ? result.
    return result;
}

// 27.2.4.5 Promise.race ( iterable ), https://tc39.es/ecma262/#sec-promise.race
JS_DEFINE_NATIVE_FUNCTION(PromiseConstructor::race)
{
    // 1. Let C be the this value.
    auto constructor = TRY(vm.this_value().to_object(vm));

    // 2. Let promiseCapability be ? NewPromiseCapability(C).
    auto promise_capability = TRY(new_promise_capability(vm, constructor));

    // 3. Let promiseResolve be Completion(GetPromiseResolve(C)).
    // 4. IfAbruptRejectPromise(promiseResolve, promiseCapability).
    auto promise_resolve = TRY_OR_REJECT(vm, promise_capability, get_promise_resolve(vm, constructor));

    // 5. Let iteratorRecord be Completion(GetIterator(iterable, sync)).
    // 6. IfAbruptRejectPromise(iteratorRecord, promiseCapability).
    auto iterator_record = TRY_OR_REJECT(vm, promise_capability, get_iterator(vm, vm.argument(0), IteratorHint::Sync));

    // 7. Let result be Completion(PerformPromiseRace(iteratorRecord, C, promiseCapability, promiseResolve)).
    auto result = perform_promise_race(vm, iterator_record, constructor, promise_capability, promise_resolve);

    // 8. If result is an abrupt completion, then
    if (result.is_error()) {
        // a. If iteratorRecord.[[Done]] is false, set result to Completion(IteratorClose(iteratorRecord, result)).
        if (!iterator_record->done)
            result = iterator_close(vm, iterator_record, result.release_error());

        // b. IfAbruptRejectPromise(result, promiseCapability).
        TRY_OR_REJECT(vm, promise_capability, result);
    }

    // 9. Return ? result.
    return result;
}

// 27.2.4.6 Promise.reject ( r ), https://tc39.es/ecma262/#sec-promise.reject
JS_DEFINE_NATIVE_FUNCTION(PromiseConstructor::reject)
{
    auto reason = vm.argument(0);

    // 1. Let C be the this value.
    auto constructor = TRY(vm.this_value().to_object(vm));

    // 2. Let promiseCapability be ? NewPromiseCapability(C).
    auto promise_capability = TRY(new_promise_capability(vm, constructor));

    // 3. Perform ? Call(promiseCapability.[[Reject]], undefined, « r »).
    [[maybe_unused]] auto result = TRY(JS::call(vm, *promise_capability->reject(), js_undefined(), reason));

    // 4. Return promiseCapability.[[Promise]].
    return promise_capability->promise();
}

// 27.2.4.7 Promise.resolve ( x ), https://tc39.es/ecma262/#sec-promise.resolve
JS_DEFINE_NATIVE_FUNCTION(PromiseConstructor::resolve)
{
    auto value = vm.argument(0);

    // 1. Let C be the this value.
    auto constructor = vm.this_value();

    // 2. If Type(C) is not Object, throw a TypeError exception.
    if (!constructor.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, constructor.to_string_without_side_effects());

    // 3. Return ? PromiseResolve(C, x).
    return TRY(promise_resolve(vm, constructor.as_object(), value));
}

// 27.2.4.8 Promise.try ( callback, ...args ), https://tc39.es/ecma262/#sec-promise.try
JS_DEFINE_NATIVE_FUNCTION(PromiseConstructor::try_)
{
    auto callback = vm.argument(0);
    Span<Value> args;
    if (vm.argument_count() > 1) {
        args = vm.running_execution_context().arguments.span().slice(1, vm.argument_count() - 1);
    }

    // 1. Let C be the this value.
    auto constructor = vm.this_value();

    // 2. If C is not an Object, throw a TypeError exception.
    if (!constructor.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, constructor.to_string_without_side_effects());

    // 3. Let promiseCapability be ? NewPromiseCapability(C).
    auto promise_capability = TRY(new_promise_capability(vm, constructor));

    // 4. Let status be Completion(Call(callback, undefined, args)).
    auto status = JS::call(vm, callback, js_undefined(), args);

    // 5. If status is an abrupt completion, then
    if (status.is_throw_completion()) {
        // a. Perform ? Call(promiseCapability.[[Reject]], undefined, « status.[[Value]] »).
        TRY(JS::call(vm, *promise_capability->reject(), js_undefined(), *status.throw_completion().value()));
    }
    // 6. Else,
    else {
        // a. Perform ? Call(promiseCapability.[[Resolve]], undefined, « status.[[Value]] »).
        TRY(JS::call(vm, *promise_capability->resolve(), js_undefined(), status.value()));
    }

    // 7. Return promiseCapability.[[Promise]].
    return promise_capability->promise();
}

// 27.2.4.9 Promise.withResolvers ( ), https://tc39.es/ecma262/#sec-promise.withResolvers
JS_DEFINE_NATIVE_FUNCTION(PromiseConstructor::with_resolvers)
{
    auto& realm = *vm.current_realm();

    // 1. Let C be the this value.
    auto constructor = vm.this_value();

    // 2. Let promiseCapability be ? NewPromiseCapability(C).
    auto promise_capability = TRY(new_promise_capability(vm, constructor));

    // 3. Let obj be OrdinaryObjectCreate(%Object.prototype%).
    auto object = Object::create(realm, realm.intrinsics().object_prototype());

    // 4. Perform ! CreateDataPropertyOrThrow(obj, "promise", promiseCapability.[[Promise]]).
    MUST(object->create_data_property_or_throw(vm.names.promise, promise_capability->promise()));

    // 5. Perform ! CreateDataPropertyOrThrow(obj, "resolve", promiseCapability.[[Resolve]]).
    MUST(object->create_data_property_or_throw(vm.names.resolve, promise_capability->resolve()));

    // 6. Perform ! CreateDataPropertyOrThrow(obj, "reject", promiseCapability.[[Reject]]).
    MUST(object->create_data_property_or_throw(vm.names.reject, promise_capability->reject()));

    // 7. Return obj.
    return object;
}

// 27.2.4.10 get Promise [ @@species ], https://tc39.es/ecma262/#sec-get-promise-@@species
JS_DEFINE_NATIVE_FUNCTION(PromiseConstructor::symbol_species_getter)
{
    // 1. Return the this value.
    return vm.this_value();
}

}
