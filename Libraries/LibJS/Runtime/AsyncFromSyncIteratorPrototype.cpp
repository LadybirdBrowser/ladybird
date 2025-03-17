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

// 27.1.4.4 AsyncFromSyncIteratorContinuation ( result, promiseCapability ), https://tc39.es/ecma262/#sec-asyncfromsynciteratorcontinuation
static Object* async_from_sync_iterator_continuation(VM& vm, Object& result, PromiseCapability& promise_capability)
{
    auto& realm = *vm.current_realm();

    // 1. NOTE: Because promiseCapability is derived from the intrinsic %Promise%, the calls to promiseCapability.[[Reject]] entailed by the use IfAbruptRejectPromise below are guaranteed not to throw.
    // 2. Let done be Completion(IteratorComplete(result)).
    // 3. IfAbruptRejectPromise(done, promiseCapability).
    auto done = TRY_OR_MUST_REJECT(vm, &promise_capability, iterator_complete(vm, result));

    // 4. Let value be Completion(IteratorValue(result)).
    // 5. IfAbruptRejectPromise(value, promiseCapability).
    auto value = TRY_OR_MUST_REJECT(vm, &promise_capability, iterator_value(vm, result));

    // 6. Let valueWrapper be PromiseResolve(%Promise%, value).
    // 7. IfAbruptRejectPromise(valueWrapper, promiseCapability).
    auto value_wrapper = TRY_OR_MUST_REJECT(vm, &promise_capability, promise_resolve(vm, realm.intrinsics().promise_constructor(), value));

    // 8. Let unwrap be a new Abstract Closure with parameters (value) that captures done and performs the following steps when called:
    auto unwrap = [done](VM& vm) -> ThrowCompletionOr<Value> {
        // a. Return CreateIterResultObject(value, done).
        return create_iterator_result_object(vm, vm.argument(0), done).ptr();
    };

    // 9. Let onFulfilled be CreateBuiltinFunction(unwrap, 1, "", « »).
    // 10. NOTE: onFulfilled is used when processing the "value" property of an IteratorResult object in order to wait for its value if it is a promise and re-package the result in a new "unwrapped" IteratorResult object.
    auto on_fulfilled = NativeFunction::create(realm, move(unwrap), 1);

    // 11. Perform PerformPromiseThen(valueWrapper, onFulfilled, undefined, promiseCapability).
    as<Promise>(value_wrapper)->perform_then(move(on_fulfilled), js_undefined(), &promise_capability);

    // 12. Return promiseCapability.[[Promise]].
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

    // 8. Return AsyncFromSyncIteratorContinuation(result, promiseCapability).
    return async_from_sync_iterator_continuation(vm, result, promise_capability);
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

    // 4. Let syncIterator be O.[[SyncIteratorRecord]].[[Iterator]].
    auto sync_iterator = this_object->sync_iterator_record().iterator;

    // 5. Let return be Completion(GetMethod(syncIterator, "return")).
    // 6. IfAbruptRejectPromise(return, promiseCapability).
    auto return_method = TRY_OR_REJECT(vm, promise_capability, Value(sync_iterator).get_method(vm, vm.names.return_));

    // 7. If return is undefined, then
    if (return_method == nullptr) {
        // a. Let iterResult be CreateIterResultObject(value, true).
        auto iter_result = create_iterator_result_object(vm, vm.argument(0), true);

        // b. Perform ! Call(promiseCapability.[[Resolve]], undefined, « iterResult »).
        MUST(call(vm, *promise_capability->resolve(), js_undefined(), iter_result));

        // c. Return promiseCapability.[[Promise]].
        return promise_capability->promise();
    }

    // 8. If value is present, then
    //     a. Let result be Completion(Call(return, syncIterator, « value »)).
    // 9. Else,
    //     a. Let result be Completion(Call(return, syncIterator)).
    // 10. IfAbruptRejectPromise(result, promiseCapability).
    auto result = TRY_OR_REJECT(vm, promise_capability,
        (vm.argument_count() > 0 ? call(vm, return_method, sync_iterator, vm.argument(0))
                                 : call(vm, return_method, sync_iterator)));

    // 11. If Type(result) is not Object, then
    if (!result.is_object()) {
        auto error = TypeError::create(realm, TRY_OR_THROW_OOM(vm, String::formatted(ErrorType::NotAnObject.message(), "SyncIteratorReturnResult")));
        // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « a newly created TypeError object »).
        MUST(call(vm, *promise_capability->reject(), js_undefined(), error));
        // b. Return promiseCapability.[[Promise]].
        return promise_capability->promise();
    }

    // 12. Return AsyncFromSyncIteratorContinuation(result, promiseCapability).
    return async_from_sync_iterator_continuation(vm, result.as_object(), promise_capability);
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

    // 4. Let syncIterator be O.[[SyncIteratorRecord]].[[Iterator]].
    auto sync_iterator = this_object->sync_iterator_record().iterator;

    // 5. Let throw be Completion(GetMethod(syncIterator, "throw")).
    // 6. IfAbruptRejectPromise(throw, promiseCapability).
    auto throw_method = TRY_OR_REJECT(vm, promise_capability, Value(sync_iterator).get_method(vm, vm.names.throw_));

    // 7. If throw is undefined, then
    if (throw_method == nullptr) {
        // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « value »).
        MUST(call(vm, *promise_capability->reject(), js_undefined(), vm.argument(0)));
        // b. Return promiseCapability.[[Promise]].
        return promise_capability->promise();
    }
    // 8. If value is present, then
    //     a. Let result be Completion(Call(throw, syncIterator, « value »)).
    // 9. Else,
    //     a. Let result be Completion(Call(throw, syncIterator)).
    // 10. IfAbruptRejectPromise(result, promiseCapability).
    auto result = TRY_OR_REJECT(vm, promise_capability,
        (vm.argument_count() > 0 ? call(vm, throw_method, sync_iterator, vm.argument(0))
                                 : call(vm, throw_method, sync_iterator)));

    // 11. If Type(result) is not Object, then
    if (!result.is_object()) {
        auto error = TypeError::create(realm, TRY_OR_THROW_OOM(vm, String::formatted(ErrorType::NotAnObject.message(), "SyncIteratorThrowResult")));

        // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « a newly created TypeError object »).
        MUST(call(vm, *promise_capability->reject(), js_undefined(), error));

        // b. Return promiseCapability.[[Promise]].
        return promise_capability->promise();
    }

    // 12. Return AsyncFromSyncIteratorContinuation(result, promiseCapability).
    return async_from_sync_iterator_continuation(vm, result.as_object(), promise_capability);
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
