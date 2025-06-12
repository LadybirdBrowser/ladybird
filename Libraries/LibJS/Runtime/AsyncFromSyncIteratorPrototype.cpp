/*
 * Copyright (c) 2021, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AsyncFromSyncIteratorPrototype.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseConstructor.h>

namespace JS {

GC_DEFINE_ALLOCATOR(AsyncFromSyncIteratorPrototype);

AsyncFromSyncIteratorPrototype::AsyncFromSyncIteratorPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().async_iterator_prototype())
{
}

void AsyncFromSyncIteratorPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.next, next, 1, attr);
    define_native_function(realm, vm.names.return_, return_, 1, attr);
    define_native_function(realm, vm.names.throw_, throw_, 1, attr);
}

enum class CloseOnRejection {
    No,
    Yes,
};

// 27.1.4.4 AsyncFromSyncIteratorContinuation ( result, promiseCapability, syncIteratorRecord, closeOnRejection ), https://tc39.es/ecma262/#sec-asyncfromsynciteratorcontinuation
static Object* async_from_sync_iterator_continuation(VM& vm, Object& result, PromiseCapability& promise_capability, IteratorRecord const& sync_iterator_record, CloseOnRejection close_on_rejection)
{
    auto& realm = *vm.current_realm();

    // 1. NOTE: Because promiseCapability is derived from the intrinsic %Promise%, the calls to promiseCapability.[[Reject]]
    //    entailed by the use IfAbruptRejectPromise below are guaranteed not to throw.

    // 2. Let done be Completion(IteratorComplete(result)).
    // 3. IfAbruptRejectPromise(done, promiseCapability).
    auto done = TRY_OR_MUST_REJECT(vm, &promise_capability, iterator_complete(vm, result));

    // 4. Let value be Completion(IteratorValue(result)).
    // 5. IfAbruptRejectPromise(value, promiseCapability).
    auto value = TRY_OR_MUST_REJECT(vm, &promise_capability, iterator_value(vm, result));

    // 6. Let valueWrapper be Completion(PromiseResolve(%Promise%, value)).
    auto value_wrapper_completion = [&]() -> ThrowCompletionOr<JS::Value> {
        return TRY(promise_resolve(vm, realm.intrinsics().promise_constructor(), value));
    }();

    // 7. If valueWrapper is an abrupt completion, done is false, and closeOnRejection is true, then
    if (value_wrapper_completion.is_error() && !done && close_on_rejection == CloseOnRejection::Yes) {
        // a. Set valueWrapper to Completion(IteratorClose(syncIteratorRecord, valueWrapper)).
        value_wrapper_completion = iterator_close(vm, sync_iterator_record, value_wrapper_completion);
    }

    // 8. IfAbruptRejectPromise(valueWrapper, promiseCapability).
    auto value_wrapper = TRY_OR_MUST_REJECT(vm, &promise_capability, value_wrapper_completion);

    // 9. Let unwrap be a new Abstract Closure with parameters (value) that captures done and performs the following steps when called:
    auto unwrap = [done](VM& vm) -> ThrowCompletionOr<Value> {
        // a. Return CreateIterResultObject(value, done).
        return create_iterator_result_object(vm, vm.argument(0), done).ptr();
    };

    // 10. Let onFulfilled be CreateBuiltinFunction(unwrap, 1, "", « »).
    // 11. NOTE: onFulfilled is used when processing the "value" property of an IteratorResult object in order to wait for its value if it is a promise and re-package the result in a new "unwrapped" IteratorResult object.
    auto on_fulfilled = NativeFunction::create(realm, move(unwrap), 1);

    Value on_rejected;

    // 12. If done is true, or if closeOnRejection is false, then
    if (done || close_on_rejection == CloseOnRejection::No) {
        // a. Let onRejected be undefined.
        on_rejected = js_undefined();
    }
    // 13. Else,
    else {
        // a. Let closeIterator be a new Abstract Closure with parameters (error) that captures syncIteratorRecord and performs the following steps when called:
        auto close_iterator = [&sync_iterator_record](VM& vm) -> ThrowCompletionOr<Value> {
            auto error = vm.argument(0);

            // i. Return ? IteratorClose(syncIteratorRecord, ThrowCompletion(error)).
            return iterator_close(vm, sync_iterator_record, throw_completion(error));
        };

        // b. Let onRejected be CreateBuiltinFunction(closeIterator, 1, "", « »).
        on_rejected = NativeFunction::create(realm, move(close_iterator), 1);

        // c. NOTE: onRejected is used to close the Iterator when the "value" property of an IteratorResult object it
        //    yields is a rejected promise.
    }

    // 14. Perform PerformPromiseThen(valueWrapper, onFulfilled, onRejected, promiseCapability).
    as<Promise>(value_wrapper.as_object()).perform_then(on_fulfilled, on_rejected, promise_capability);

    // 15. Return promiseCapability.[[Promise]].
    return promise_capability.promise();
}

// 27.1.4.2.1 %AsyncFromSyncIteratorPrototype%.next ( [ value ] ), https://tc39.es/ecma262/#sec-%asyncfromsynciteratorprototype%.next
JS_DEFINE_NATIVE_FUNCTION(AsyncFromSyncIteratorPrototype::next)
{
    auto& realm = *vm.current_realm();

    // 1. Let O be the this value.
    // 2. Assert: O is an Object that has a [[SyncIteratorRecord]] internal slot.
    auto this_object = MUST(typed_this_object(vm));

    // 3. Let promiseCapability be ! NewPromiseCapability(%Promise%).
    auto promise_capability = MUST(new_promise_capability(vm, realm.intrinsics().promise_constructor()));

    // 4. Let syncIteratorRecord be O.[[SyncIteratorRecord]].
    auto& sync_iterator_record = this_object->sync_iterator_record();

    // 5. If value is present, then
    //     a. Let result be Completion(IteratorNext(syncIteratorRecord, value)).
    // 6. Else,
    //     a. Let result be Completion(IteratorNext(syncIteratorRecord)).
    // 7. IfAbruptRejectPromise(result, promiseCapability).
    auto result = TRY_OR_REJECT(vm, promise_capability,
        (vm.argument_count() > 0 ? iterator_next(vm, sync_iterator_record, vm.argument(0))
                                 : iterator_next(vm, sync_iterator_record)));

    // 8. Return AsyncFromSyncIteratorContinuation(result, promiseCapability, syncIteratorRecord, true).
    return async_from_sync_iterator_continuation(vm, result, promise_capability, sync_iterator_record, CloseOnRejection::Yes);
}

// 27.1.4.2.2 %AsyncFromSyncIteratorPrototype%.return ( [ value ] ), https://tc39.es/ecma262/#sec-%asyncfromsynciteratorprototype%.return
JS_DEFINE_NATIVE_FUNCTION(AsyncFromSyncIteratorPrototype::return_)
{
    auto& realm = *vm.current_realm();

    // 1. Let O be the this value.
    // 2. Assert: O is an Object that has a [[SyncIteratorRecord]] internal slot.
    auto this_object = MUST(typed_this_object(vm));

    // 3. Let promiseCapability be ! NewPromiseCapability(%Promise%).
    auto promise_capability = MUST(new_promise_capability(vm, realm.intrinsics().promise_constructor()));

    // 4. Let syncIteratorRecord be O.[[SyncIteratorRecord]].
    auto& sync_iterator_record = this_object->sync_iterator_record();

    // 5. Let syncIterator be syncIteratorRecord.[[Iterator]].
    auto sync_iterator = sync_iterator_record.iterator;

    // 6. Let return be Completion(GetMethod(syncIterator, "return")).
    // 7. IfAbruptRejectPromise(return, promiseCapability).
    auto return_method = TRY_OR_REJECT(vm, promise_capability, Value(sync_iterator).get_method(vm, vm.names.return_));

    // 8. If return is undefined, then
    if (return_method == nullptr) {
        // a. Let iteratorResult be CreateIteratorResultObject(value, true).
        auto iterator_result = create_iterator_result_object(vm, vm.argument(0), true);

        // b. Perform ! Call(promiseCapability.[[Resolve]], undefined, « iteratorResult »).
        MUST(call(vm, *promise_capability->resolve(), js_undefined(), iterator_result));

        // c. Return promiseCapability.[[Promise]].
        return promise_capability->promise();
    }

    // 9. If value is present, then
    //     a. Let result be Completion(Call(return, syncIterator, « value »)).
    // 10. Else,
    //     a. Let result be Completion(Call(return, syncIterator)).
    // 11. IfAbruptRejectPromise(result, promiseCapability).
    auto result = TRY_OR_REJECT(vm, promise_capability,
        (vm.argument_count() > 0 ? call(vm, return_method, sync_iterator, vm.argument(0))
                                 : call(vm, return_method, sync_iterator)));

    // 12. If Type(result) is not Object, then
    if (!result.is_object()) {
        auto error = TypeError::create(realm, TRY_OR_THROW_OOM(vm, String::formatted(ErrorType::NotAnObject.message(), "SyncIteratorReturnResult")));
        // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « a newly created TypeError object »).
        MUST(call(vm, *promise_capability->reject(), js_undefined(), error));

        // b. Return promiseCapability.[[Promise]].
        return promise_capability->promise();
    }

    // 13. Return AsyncFromSyncIteratorContinuation(result, promiseCapability, syncIteratorRecord, false).
    return async_from_sync_iterator_continuation(vm, result.as_object(), promise_capability, sync_iterator_record, CloseOnRejection::No);
}

// 27.1.4.2.3 %AsyncFromSyncIteratorPrototype%.throw ( [ value ] ), https://tc39.es/ecma262/#sec-%asyncfromsynciteratorprototype%.throw
JS_DEFINE_NATIVE_FUNCTION(AsyncFromSyncIteratorPrototype::throw_)
{
    auto& realm = *vm.current_realm();

    // 1. Let O be the this value.
    // 2. Assert: O is an Object that has a [[SyncIteratorRecord]] internal slot.
    auto this_object = MUST(typed_this_object(vm));

    // 3. Let promiseCapability be ! NewPromiseCapability(%Promise%).
    auto promise_capability = MUST(new_promise_capability(vm, realm.intrinsics().promise_constructor()));

    // 4. Let syncIteratorRecord be O.[[SyncIteratorRecord]].
    auto& sync_iterator_record = this_object->sync_iterator_record();

    // 5. Let syncIterator be syncIteratorRecord.[[Iterator]].
    auto sync_iterator = sync_iterator_record.iterator;

    // 6. Let throw be Completion(GetMethod(syncIterator, "throw")).
    // 7. IfAbruptRejectPromise(throw, promiseCapability).
    auto throw_method = TRY_OR_REJECT(vm, promise_capability, Value(sync_iterator).get_method(vm, vm.names.throw_));

    // 8. If throw is undefined, then
    if (throw_method == nullptr) {
        // a. NOTE: If syncIterator does not have a throw method, close it to give it a chance to clean up before we reject the capability.

        // b. Let closeCompletion be NormalCompletion(empty).
        auto close_completion = normal_completion({});

        // c. Let result be Completion(IteratorClose(syncIteratorRecord, closeCompletion)).
        // d. IfAbruptRejectPromise(result, promiseCapability).
        TRY_OR_REJECT(vm, promise_capability, iterator_close(vm, sync_iterator_record, close_completion));

        // e. NOTE: The next step throws a TypeError to indicate that there was a protocol violation: syncIterator does not have a throw method.
        // f. NOTE: If closing syncIterator does not throw then the result of that operation is ignored, even if it yields a rejected promise.

        // g. Perform ! Call(promiseCapability.[[Reject]], undefined, « a newly created TypeError object »).
        auto error = TypeError::create(realm, MUST(String::formatted(ErrorType::IsUndefined.message(), "throw method")));
        MUST(call(vm, *promise_capability->reject(), js_undefined(), error));

        // h. Return promiseCapability.[[Promise]].
        return promise_capability->promise();
    }

    // 9. If value is present, then
    //     a. Let result be Completion(Call(throw, syncIterator, « value »)).
    // 10. Else,
    //     a. Let result be Completion(Call(throw, syncIterator)).
    // 11. IfAbruptRejectPromise(result, promiseCapability).
    auto result = TRY_OR_REJECT(vm, promise_capability,
        (vm.argument_count() > 0 ? call(vm, throw_method, sync_iterator, vm.argument(0))
                                 : call(vm, throw_method, sync_iterator)));

    // 12. If result is not an Object, then
    if (!result.is_object()) {
        // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « a newly created TypeError object »).
        auto error = TypeError::create(realm, MUST(String::formatted(ErrorType::NotAnObject.message(), "SyncIteratorThrowResult")));
        MUST(call(vm, *promise_capability->reject(), js_undefined(), error));

        // b. Return promiseCapability.[[Promise]].
        return promise_capability->promise();
    }

    // 13. Return AsyncFromSyncIteratorContinuation(result, promiseCapability, syncIteratorRecord, true).
    return async_from_sync_iterator_continuation(vm, result.as_object(), promise_capability, sync_iterator_record, CloseOnRejection::Yes);
}

// 27.1.4.1 CreateAsyncFromSyncIterator ( syncIteratorRecord ), https://tc39.es/ecma262/#sec-createasyncfromsynciterator
GC::Ref<IteratorRecord> create_async_from_sync_iterator(VM& vm, GC::Ref<IteratorRecord> sync_iterator_record)
{
    auto& realm = *vm.current_realm();

    // 1. Let asyncIterator be OrdinaryObjectCreate(%AsyncFromSyncIteratorPrototype%, « [[SyncIteratorRecord]] »).
    // 2. Set asyncIterator.[[SyncIteratorRecord]] to syncIteratorRecord.
    auto async_iterator = AsyncFromSyncIterator::create(realm, sync_iterator_record);

    // 3. Let nextMethod be ! Get(asyncIterator, "next").
    auto next_method = MUST(async_iterator->get(vm.names.next));

    // 4. Let iteratorRecord be the Iterator Record { [[Iterator]]: asyncIterator, [[NextMethod]]: nextMethod, [[Done]]: false }.
    auto iterator_record = vm.heap().allocate<IteratorRecord>(async_iterator, next_method, false);

    // 5. Return iteratorRecord.
    return iterator_record;
}

}
