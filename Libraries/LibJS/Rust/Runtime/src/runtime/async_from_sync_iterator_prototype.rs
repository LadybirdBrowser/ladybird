/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call_function_object;
use crate::runtime::async_from_sync_iterator::AsyncFromSyncIterator;
use crate::runtime::completion::{Completion, CompletionType, Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::iterator::{
    IteratorRecord, IteratorRecordImpl, create_iterator_result_object, iterator_close, iterator_complete,
    iterator_next, iterator_value,
};
use crate::runtime::native_function::{NativeFunction, raw_native};
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::promise::{Promise, promise_resolve};
use crate::runtime::promise_capability::{
    PromiseCapability, new_promise_capability, try_or_must_reject, try_or_reject,
};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;

// 27.1.4.2 The %AsyncFromSyncIteratorPrototype% Object, https://tc39.es/ecma262/#sec-%asyncfromsynciteratorprototype%-object
#[repr(C)]
#[derive(Trace)]
pub struct AsyncFromSyncIteratorPrototype {
    base: Object,
}

define_object_class!(AsyncFromSyncIteratorPrototype, extends: [Object], methods: {
    initialize: AsyncFromSyncIteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

const DISPLAY_NAME: &str = "AsyncFromSyncIterator";

#[derive(Clone, Copy, PartialEq, Eq)]
enum CloseOnRejection {
    No,
    Yes,
}

/// The value of a newly created TypeError object, for the steps that reject a capability with one.
fn new_type_error(vm: &Vm, error_type: ErrorType, argument: &str) -> Value {
    let Err(throw) = vm.throw_completion::<()>(ErrorKind::TypeError, error_type, &[&argument]) else {
        unreachable!("throw_completion always throws");
    };
    throw.value()
}

// 27.1.4.4 AsyncFromSyncIteratorContinuation ( result, promiseCapability, syncIteratorRecord, closeOnRejection ), https://tc39.es/ecma262/#sec-asyncfromsynciteratorcontinuation
fn async_from_sync_iterator_continuation(
    vm: &Vm,
    result: Gc<Object>,
    promise_capability: Gc<PromiseCapability>,
    sync_iterator_record: Gc<IteratorRecord>,
    close_on_rejection: CloseOnRejection,
) -> Gc<Object> {
    let realm = vm.current_realm().expect("there is a current realm");

    // 1. NOTE: Because promiseCapability is derived from the intrinsic %Promise%, the calls to promiseCapability.[[Reject]]
    //    entailed by the use IfAbruptRejectPromise below are guaranteed not to throw.

    // 2. Let done be Completion(IteratorComplete(result)).
    // 3. IfAbruptRejectPromise(done, promiseCapability).
    let done = try_or_must_reject!(vm, promise_capability, iterator_complete(vm, result));

    // 4. Let value be Completion(IteratorValue(result)).
    // 5. IfAbruptRejectPromise(value, promiseCapability).
    let value = try_or_must_reject!(vm, promise_capability, iterator_value(vm, result));

    // 6. Let valueWrapper be Completion(PromiseResolve(%Promise%, value)).
    let mut value_wrapper_completion =
        promise_resolve(vm, realm.intrinsics().promise_constructor(vm).upcast(), value).map(Value::from_object);

    // 7. If valueWrapper is an abrupt completion, done is false, and closeOnRejection is true, then
    if let Err(throw) = value_wrapper_completion
        && !done
        && close_on_rejection == CloseOnRejection::Yes
    {
        // a. Set valueWrapper to Completion(IteratorClose(syncIteratorRecord, valueWrapper)).
        value_wrapper_completion = iterator_close(vm, &sync_iterator_record, throw.into()).into_throw_completion_or();
    }

    // 8. IfAbruptRejectPromise(valueWrapper, promiseCapability).
    let value_wrapper = try_or_must_reject!(vm, promise_capability, value_wrapper_completion);

    // 9. Let unwrap be a new Abstract Closure with parameters (value) that captures done and performs the following steps when called:
    // 10. Let onFulfilled be CreateBuiltinFunction(unwrap, 1, "", « »).
    // 11. NOTE: onFulfilled is used when processing the "value" property of an IteratorResult object in order to wait for its value if it is a promise and re-package the result in a new "unwrapped" IteratorResult object.
    let on_fulfilled = NativeFunction::create_anonymous(
        vm,
        done,
        |vm, &done: &bool| {
            // a. Return CreateIterResultObject(value, done).
            let realm = vm.current_realm().expect("there is a current realm");
            Ok(Value::from_object(create_iterator_result_object(
                vm,
                realm,
                vm.argument(0),
                done,
            )))
        },
        1,
    );

    // 12. If done is true, or if closeOnRejection is false, then
    let on_rejected = if done || close_on_rejection == CloseOnRejection::No {
        // a. Let onRejected be undefined.
        Value::UNDEFINED
    }
    // 13. Else,
    else {
        // a. Let closeIterator be a new Abstract Closure with parameters (error) that captures syncIteratorRecord and performs the following steps when called:
        // b. Let onRejected be CreateBuiltinFunction(closeIterator, 1, "", « »).
        // c. NOTE: onRejected is used to close the Iterator when the "value" property of an IteratorResult object it
        //    yields is a rejected promise.
        Value::from_object(NativeFunction::create_anonymous(
            vm,
            sync_iterator_record,
            |vm, &sync_iterator_record: &Gc<IteratorRecord>| {
                let error = vm.argument(0);

                // i. Return ? IteratorClose(syncIteratorRecord, ThrowCompletion(error)).
                crate::embedding::completion::log_exception_if_enabled(vm, error);
                iterator_close(vm, &sync_iterator_record, Completion::new(CompletionType::Throw, error))
                    .into_throw_completion_or()
            },
            1,
        ))
    };

    // 14. Perform PerformPromiseThen(valueWrapper, onFulfilled, onRejected, promiseCapability).
    value_wrapper
        .as_object()
        .downcast::<Promise>()
        .expect("PromiseResolve of %Promise% returns a Promise")
        .perform_then(
            vm,
            Value::from_object(on_fulfilled),
            on_rejected,
            Some(promise_capability),
        );

    // 15. Return promiseCapability.[[Promise]].
    promise_capability.promise()
}

impl AsyncFromSyncIteratorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AsyncFromSyncIteratorPrototype> {
        realm.create_object(
            vm,
            AsyncFromSyncIteratorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().async_iterator_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &vm.names.next,
            raw_native!(AsyncFromSyncIteratorPrototype::next),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &vm.names.return_,
            raw_native!(AsyncFromSyncIteratorPrototype::return_),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &vm.names.throw_,
            raw_native!(AsyncFromSyncIteratorPrototype::throw_),
            1,
            attr,
            None,
        );
    }

    // 27.1.4.2.1 %AsyncFromSyncIteratorPrototype%.next ( [ value ] ), https://tc39.es/ecma262/#sec-%asyncfromsynciteratorprototype%.next
    fn next(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("there is a current realm");

        // 1. Let O be the this value.
        // 2. Assert: O is an Object that has a [[SyncIteratorRecord]] internal slot.
        let this_object = typed_this_object::<AsyncFromSyncIterator>(vm, DISPLAY_NAME).must();

        // 3. Let promiseCapability be ! NewPromiseCapability(%Promise%).
        let promise_capability =
            new_promise_capability(vm, Value::from_object(realm.intrinsics().promise_constructor(vm))).must();

        // 4. Let syncIteratorRecord be O.[[SyncIteratorRecord]].
        let sync_iterator_record = this_object.sync_iterator_record();

        // 5. If value is present, then
        //     a. Let result be Completion(IteratorNext(syncIteratorRecord, value)).
        // 6. Else,
        //     a. Let result be Completion(IteratorNext(syncIteratorRecord)).
        // 7. IfAbruptRejectPromise(result, promiseCapability).
        let value = (vm.argument_count() > 0).then(|| vm.argument(0));
        let result = try_or_reject!(vm, promise_capability, iterator_next(vm, &sync_iterator_record, value));

        // 8. Return AsyncFromSyncIteratorContinuation(result, promiseCapability, syncIteratorRecord, true).
        Ok(Value::from_object(async_from_sync_iterator_continuation(
            vm,
            result,
            promise_capability,
            sync_iterator_record,
            CloseOnRejection::Yes,
        )))
    }

    // 27.1.4.2.2 %AsyncFromSyncIteratorPrototype%.return ( [ value ] ), https://tc39.es/ecma262/#sec-%asyncfromsynciteratorprototype%.return
    fn return_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("there is a current realm");

        // 1. Let O be the this value.
        // 2. Assert: O is an Object that has a [[SyncIteratorRecord]] internal slot.
        let this_object = typed_this_object::<AsyncFromSyncIterator>(vm, DISPLAY_NAME).must();

        // 3. Let promiseCapability be ! NewPromiseCapability(%Promise%).
        let promise_capability =
            new_promise_capability(vm, Value::from_object(realm.intrinsics().promise_constructor(vm))).must();

        // 4. Let syncIteratorRecord be O.[[SyncIteratorRecord]].
        let sync_iterator_record = this_object.sync_iterator_record();

        // 5. Let syncIterator be syncIteratorRecord.[[Iterator]].
        let sync_iterator = Value::from_object(sync_iterator_record.iterator());

        // 6. Let return be Completion(GetMethod(syncIterator, "return")).
        // 7. IfAbruptRejectPromise(return, promiseCapability).
        let return_method = try_or_reject!(vm, promise_capability, sync_iterator.get_method(vm, &vm.names.return_));

        // 8. If return is undefined, then
        let Some(return_method) = return_method else {
            // a. Let iteratorResult be CreateIteratorResultObject(value, true).
            let iterator_result = create_iterator_result_object(vm, realm, vm.argument(0), true);

            // b. Perform ! Call(promiseCapability.[[Resolve]], undefined, « iteratorResult »).
            call_function_object(
                vm,
                promise_capability.resolve(),
                Value::UNDEFINED,
                &[Value::from_object(iterator_result)],
            )
            .must();

            // c. Return promiseCapability.[[Promise]].
            return Ok(Value::from_object(promise_capability.promise()));
        };

        // 9. If value is present, then
        //     a. Let result be Completion(Call(return, syncIterator, « value »)).
        // 10. Else,
        //     a. Let result be Completion(Call(return, syncIterator)).
        // 11. IfAbruptRejectPromise(result, promiseCapability).
        let result = if vm.argument_count() > 0 {
            call_function_object(vm, return_method, sync_iterator, &[vm.argument(0)])
        } else {
            call_function_object(vm, return_method, sync_iterator, &[])
        };
        let result = try_or_reject!(vm, promise_capability, result);

        // 12. If Type(result) is not Object, then
        if !result.is_object() {
            let error = new_type_error(vm, ErrorType::NotAnObject, "SyncIteratorReturnResult");
            // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « a newly created TypeError object »).
            call_function_object(vm, promise_capability.reject(), Value::UNDEFINED, &[error]).must();

            // b. Return promiseCapability.[[Promise]].
            return Ok(Value::from_object(promise_capability.promise()));
        }

        // 13. Return AsyncFromSyncIteratorContinuation(result, promiseCapability, syncIteratorRecord, false).
        Ok(Value::from_object(async_from_sync_iterator_continuation(
            vm,
            result.as_object(),
            promise_capability,
            sync_iterator_record,
            CloseOnRejection::No,
        )))
    }

    // 27.1.4.2.3 %AsyncFromSyncIteratorPrototype%.throw ( [ value ] ), https://tc39.es/ecma262/#sec-%asyncfromsynciteratorprototype%.throw
    fn throw_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("there is a current realm");

        // 1. Let O be the this value.
        // 2. Assert: O is an Object that has a [[SyncIteratorRecord]] internal slot.
        let this_object = typed_this_object::<AsyncFromSyncIterator>(vm, DISPLAY_NAME).must();

        // 3. Let promiseCapability be ! NewPromiseCapability(%Promise%).
        let promise_capability =
            new_promise_capability(vm, Value::from_object(realm.intrinsics().promise_constructor(vm))).must();

        // 4. Let syncIteratorRecord be O.[[SyncIteratorRecord]].
        let sync_iterator_record = this_object.sync_iterator_record();

        // 5. Let syncIterator be syncIteratorRecord.[[Iterator]].
        let sync_iterator = Value::from_object(sync_iterator_record.iterator());

        // 6. Let throw be Completion(GetMethod(syncIterator, "throw")).
        // 7. IfAbruptRejectPromise(throw, promiseCapability).
        let throw_method = try_or_reject!(vm, promise_capability, sync_iterator.get_method(vm, &vm.names.throw_));

        // 8. If throw is undefined, then
        let Some(throw_method) = throw_method else {
            // a. NOTE: If syncIterator does not have a throw method, close it to give it a chance to clean up before we reject the capability.

            // b. Let closeCompletion be NormalCompletion(empty).
            let close_completion = Completion::normal(Value::UNDEFINED);

            // c. Let result be Completion(IteratorClose(syncIteratorRecord, closeCompletion)).
            // d. IfAbruptRejectPromise(result, promiseCapability).
            try_or_reject!(
                vm,
                promise_capability,
                iterator_close(vm, &sync_iterator_record, close_completion).into_throw_completion_or()
            );

            // e. NOTE: The next step throws a TypeError to indicate that there was a protocol violation: syncIterator does not have a throw method.
            // f. NOTE: If closing syncIterator does not throw then the result of that operation is ignored, even if it yields a rejected promise.

            // g. Perform ! Call(promiseCapability.[[Reject]], undefined, « a newly created TypeError object »).
            let error = new_type_error(vm, ErrorType::IsUndefined, "throw method");
            call_function_object(vm, promise_capability.reject(), Value::UNDEFINED, &[error]).must();

            // h. Return promiseCapability.[[Promise]].
            return Ok(Value::from_object(promise_capability.promise()));
        };

        // 9. If value is present, then
        //     a. Let result be Completion(Call(throw, syncIterator, « value »)).
        // 10. Else,
        //     a. Let result be Completion(Call(throw, syncIterator)).
        // 11. IfAbruptRejectPromise(result, promiseCapability).
        let result = if vm.argument_count() > 0 {
            call_function_object(vm, throw_method, sync_iterator, &[vm.argument(0)])
        } else {
            call_function_object(vm, throw_method, sync_iterator, &[])
        };
        let result = try_or_reject!(vm, promise_capability, result);

        // 12. If result is not an Object, then
        if !result.is_object() {
            // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « a newly created TypeError object »).
            let error = new_type_error(vm, ErrorType::NotAnObject, "SyncIteratorThrowResult");
            call_function_object(vm, promise_capability.reject(), Value::UNDEFINED, &[error]).must();

            // b. Return promiseCapability.[[Promise]].
            return Ok(Value::from_object(promise_capability.promise()));
        }

        // 13. Return AsyncFromSyncIteratorContinuation(result, promiseCapability, syncIteratorRecord, true).
        Ok(Value::from_object(async_from_sync_iterator_continuation(
            vm,
            result.as_object(),
            promise_capability,
            sync_iterator_record,
            CloseOnRejection::Yes,
        )))
    }
}

// 27.1.4.1 CreateAsyncFromSyncIterator ( syncIteratorRecord ), https://tc39.es/ecma262/#sec-createasyncfromsynciterator
pub fn create_async_from_sync_iterator(vm: &Vm, sync_iterator_record: Gc<IteratorRecord>) -> IteratorRecordImpl {
    let realm = vm.current_realm().expect("there is a current realm");

    // 1. Let asyncIterator be OrdinaryObjectCreate(%AsyncFromSyncIteratorPrototype%, « [[SyncIteratorRecord]] »).
    // 2. Set asyncIterator.[[SyncIteratorRecord]] to syncIteratorRecord.
    let async_iterator = AsyncFromSyncIterator::create(vm, realm, sync_iterator_record);

    // 3. Let nextMethod be ! Get(asyncIterator, "next").
    let next_method = async_iterator.get(vm, &vm.names.next).must();

    // 4. Let iteratorRecord be the Iterator Record { [[Iterator]]: asyncIterator, [[NextMethod]]: nextMethod, [[Done]]: false }.
    // 5. Return iteratorRecord.
    IteratorRecordImpl::new(Some(async_iterator.upcast()), next_method, false)
}
