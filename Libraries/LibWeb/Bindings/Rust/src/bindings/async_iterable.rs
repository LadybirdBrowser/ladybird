/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The asynchronous iterators of the interfaces implemented in Rust, https://webidl.spec.whatwg.org/#idl-async-iterable,
//! for a value asynchronously iterable declaration.

use core::cell::Cell;
use core::ptr::NonNull;

use libjs_rust::api::{
    Attribute, EmbedderCell, EmbedderClass, EmbedderValue, ErrorKind, ErrorType, Gc, HostObject, JSHostClass, Must,
    NativeFunction, Object, Promise, PropertyAttributes, PropertyKey, RawNativeFunctionPointer, Realm, Throw,
    ThrowCompletionOr, Trace, Value, Vm, call, create_iterator_result_object, host_data_of, is_host_instance_of,
};

use crate::bindings::CallContext;
use crate::bindings::interface::{Interface, define_operation};
use crate::bindings::promise::{NativeSteps, WebIdlPromise, create_promise};
use crate::bindings::services::web_bindings_ensure_web_iterator_prototype;

/// What the steps that get the next iteration result fulfill their promise with at the end of the iteration,
/// https://webidl.spec.whatwg.org/#end-of-iteration
pub(crate) const END_OF_ITERATION: Value = Value::EMPTY;

/// What the implementation of an interface with a value asynchronously iterable declaration implements, for a default
/// asynchronous iterator object of the interface whose state its asynchronous iterator initialization steps created.
pub(crate) trait AsyncIterable: Interface {
    type IteratorState: Trace + 'static;

    /// The steps to get the next iteration result, https://webidl.spec.whatwg.org/#dfn-get-the-next-iteration-result,
    /// which return a promise fulfilled with the next value or END_OF_ITERATION.
    fn get_the_next_iteration_result(cx: &CallContext, state: &Self::IteratorState) -> WebIdlPromise;

    /// The asynchronous iterator return algorithm, https://webidl.spec.whatwg.org/#asynchronous-iterator-return, of an
    /// interface whose IDL marks it [DefinesAsyncIteratorReturn].
    fn asynchronous_iterator_return(cx: &CallContext, state: &Self::IteratorState, value: Value) -> WebIdlPromise;
}

/// What the generated bindings of an interface with an asynchronously iterable declaration define: the classes of its
/// default asynchronous iterator objects.
pub(crate) trait DefaultAsyncIteratorClasses: AsyncIterable {
    /// The class of the cells of the default asynchronous iterator objects of the interface.
    const ASYNC_ITERATOR_CELL_CLASS: &'static EmbedderClass<DefaultAsyncIterator<Self>>;

    /// The class of the default asynchronous iterator objects of the interface.
    fn async_iterator_class() -> &'static JSHostClass;
}

/// A default asynchronous iterator object, https://webidl.spec.whatwg.org/#default-asynchronous-iterator-object,
/// which is the host data of a host object of the asynchronous iterator class of its interface.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DefaultAsyncIterator<T: AsyncIterable> {
    realm: Gc<Realm>,
    state: T::IteratorState,
    ongoing_promise: Cell<Option<Gc<Promise>>>,
    is_finished: Cell<bool>,
}

impl<T: DefaultAsyncIteratorClasses> EmbedderValue for DefaultAsyncIterator<T> {
    const CLASS: &'static EmbedderClass<Self> = T::ASYNC_ITERATOR_CELL_CLASS;
}

type IteratorCell<T> = Gc<EmbedderCell<DefaultAsyncIterator<T>>>;

/// Defines the "values" and %Symbol.asyncIterator% methods of an interface with a value asynchronously iterable
/// declaration on its interface prototype object, https://webidl.spec.whatwg.org/#define-the-asynchronous-iteration-methods
pub(crate) fn define_async_iterator_properties(
    vm: &Vm,
    realm: Gc<Realm>,
    prototype: Gc<Object>,
    values: RawNativeFunctionPointer,
    length: i32,
) {
    define_operation(vm, realm, prototype, "values", values, length);
    prototype.define_direct_property(
        vm,
        &PropertyKey::from(vm.well_known_symbols().async_iterator),
        prototype.get_without_side_effects(vm, &vm.names.values),
        PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE),
    );
}

/// A newly created default asynchronous iterator object for the interface in the realm of the call context, with the
/// state that the asynchronous iterator initialization steps of the interface created.
pub(crate) fn create_default_async_iterator<T: DefaultAsyncIteratorClasses>(
    cx: &CallContext,
    state: T::IteratorState,
) -> Value {
    let vm = cx.vm();
    let realm = cx.realm();
    let iterator = T::ASYNC_ITERATOR_CELL_CLASS.allocate(vm, |_| DefaultAsyncIterator {
        realm,
        state,
        ongoing_promise: Cell::new(None),
        is_finished: Cell::new(false),
    });
    // SAFETY: The realm is live, and the interface is asynchronously iterable.
    let prototype = unsafe { web_bindings_ensure_web_iterator_prototype(realm.as_ptr().cast(), T::INTERFACE_NAME) };
    let prototype = NonNull::new(prototype.cast::<Object>()).expect("LibWeb returns the iterator prototype object");
    // SAFETY: LibWeb returns a live object, the iterator class has no hooks, and the host data is a live cell.
    let object = unsafe {
        HostObject::create(
            vm,
            realm,
            T::async_iterator_class(),
            Some(Gc::from_non_null(prototype)),
            None,
            Some(iterator.as_non_null().cast()),
        )
    };
    Value::from_object(object.upcast::<Object>())
}

/// Validates the this value of the "next" and "return" methods of the asynchronous iterator prototype object, steps
/// 3 to 7: the default asynchronous iterator object, or the promise of thisValidationPromiseCapability, which it
/// rejected.
fn validate_this<T: DefaultAsyncIteratorClasses>(
    vm: &Vm,
    this_validation_promise_capability: WebIdlPromise,
) -> Result<IteratorCell<T>, Value> {
    let reject = |error: Value| {
        call(
            vm,
            Value::from_object(this_validation_promise_capability.reject().upcast::<Object>()),
            Value::UNDEFINED,
            &[error],
        )
        .must();
        Value::from_object(this_validation_promise_capability.promise())
    };

    // 3. Let thisValue be the this value.
    let this_value = vm.this_value();

    // 4. Let object be Completion(ToObject(thisValue)).
    // 5. IfAbruptRejectPromise(object, thisValidationPromiseCapability).
    let object = this_value.to_object(vm).map_err(|throw| reject(throw.value()))?;

    // 6. If object is a platform object, then perform a security check, passing:
    //     * the platform object object,
    //     * the identifier "next", and
    //     * the type "method".
    // NB: A default asynchronous iterator object is not a platform object.

    // 7. If object is not a default asynchronous iterator object for interface, then:
    let Some(iterator) = default_async_iterator_of::<T>(object) else {
        // 1. Let error be a new TypeError.
        let name = format!("{}AsyncIterator", T::NAME);
        let error = vm
            .throw_completion::<()>(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&name])
            .expect_err("throwing a completion throws");

        // 2. Perform ! Call(thisValidationPromiseCapability.[[Reject]], undefined, « error »).
        // 3. Return thisValidationPromiseCapability.[[Promise]].
        return Err(reject(error.value()));
    };
    Ok(iterator)
}

fn default_async_iterator_of<T: DefaultAsyncIteratorClasses>(object: Gc<Object>) -> Option<IteratorCell<T>> {
    if !is_host_instance_of(&object, T::async_iterator_class()) {
        return None;
    }
    let host_data = host_data_of(&object).expect("a default asynchronous iterator object holds its cell");
    // SAFETY: The object is a host object of the asynchronous iterator class, whose host data is the cell of a default
    //         asynchronous iterator.
    unsafe { T::ASYNC_ITERATOR_CELL_CLASS.downcast(host_data) }
}

/// The realm that the "next" and "return" methods create their promise in, as LibWeb's C++ bindings do: the realm of
/// the this value, or the current realm for a this value that is not an object.
fn this_value_realm(vm: &Vm) -> Gc<Realm> {
    let this_value = vm.this_value();
    if this_value.is_object() {
        return this_value.as_object().shape().realm();
    }
    vm.current_realm().expect("a native function runs in a realm")
}

/// The steps of a built-in function that the iterator creates, given its captures.
type BuiltinSteps<C> = fn(&Vm, C) -> ThrowCompletionOr<Value>;

fn native_function<C: Trace + Copy + 'static>(
    vm: &Vm,
    realm: Gc<Realm>,
    captures: C,
    behaviour: BuiltinSteps<C>,
) -> Value {
    let function = NativeFunction::create(
        vm,
        NativeSteps {
            captures,
            steps: behaviour,
        },
        |vm: &Vm, function: &NativeSteps<C, BuiltinSteps<C>>| (function.steps)(vm, function.captures),
        0,
        &PropertyKey::from_utf8(""),
        Some(realm),
        None,
        None,
    );
    Value::from_object(function.upcast::<Object>())
}

/// Runs the steps after the ongoing promise of the iterator settles, or now if it has none, and makes their promise
/// the ongoing promise, steps 9 to 11 of the "next" and "return" methods.
fn chain_onto_ongoing_promise<T: DefaultAsyncIteratorClasses, C: Trace + Copy + 'static>(
    vm: &Vm,
    iterator: IteratorCell<T>,
    captures: C,
    steps: BuiltinSteps<C>,
) {
    let realm = iterator.realm;

    // 9. Let ongoingPromise be object’s ongoing promise.
    // 10. If ongoingPromise is not null, then:
    if let Some(ongoing_promise) = iterator.ongoing_promise.get() {
        // 1. Let afterOngoingPromiseCapability be ! NewPromiseCapability(%Promise%).
        let after_ongoing_promise_capability = create_promise(vm, realm);

        // 2. Let onSettled be CreateBuiltinFunction(steps, « »).
        let on_settled = native_function(vm, realm, captures, steps);

        // 3. Perform PerformPromiseThen(ongoingPromise, onSettled, onSettled, afterOngoingPromiseCapability).
        ongoing_promise.perform_then(vm, on_settled, on_settled, Some(after_ongoing_promise_capability));

        // 4. Set object’s ongoing promise to afterOngoingPromiseCapability.[[Promise]].
        iterator
            .ongoing_promise
            .set(Some(promise_of(after_ongoing_promise_capability.promise())));
    }
    // 11. Otherwise:
    else {
        // 1. Set object’s ongoing promise to the result of running steps.
        let result = steps(vm, captures).must();
        iterator.ongoing_promise.set(Some(promise_of(result.as_object())));
    }
}

fn promise_of(object: Gc<Object>) -> Gc<Promise> {
    object.downcast::<Promise>().expect("the steps return a promise")
}

/// The "next" method of the asynchronous iterator prototype object of the interface,
/// https://webidl.spec.whatwg.org/#es-asynchronous-iterator-prototype-object
pub(crate) fn async_iterator_next<T: DefaultAsyncIteratorClasses>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let interface be the interface for which the asynchronous iterator prototype object exists.
    // 2. Let thisValidationPromiseCapability be ! NewPromiseCapability(%Promise%).
    let this_validation_promise_capability = create_promise(vm, this_value_realm(vm));

    // 3. Let thisValue be the this value.
    // 4. Let object be Completion(ToObject(thisValue)).
    // 5. IfAbruptRejectPromise(object, thisValidationPromiseCapability).
    // 6. If object is a platform object, then perform a security check, passing:
    // 7. If object is not a default asynchronous iterator object for interface, then:
    let iterator = match validate_this::<T>(vm, this_validation_promise_capability) {
        Ok(iterator) => iterator,
        Err(promise) => return Ok(promise),
    };

    // 8. Let nextSteps be the following steps:
    fn next_steps<T: DefaultAsyncIteratorClasses>(vm: &Vm, iterator: IteratorCell<T>) -> ThrowCompletionOr<Value> {
        let realm = iterator.realm;

        // 1. Let nextPromiseCapability be ! NewPromiseCapability(%Promise%).
        let next_promise_capability = create_promise(vm, realm);

        // 2. If object’s is finished is true, then:
        if iterator.is_finished.get() {
            // 1. Let result be CreateIteratorResultObject(undefined, true).
            let result = create_iterator_result_object(vm, realm, Value::UNDEFINED, true);

            // 2. Perform ! Call(nextPromiseCapability.[[Resolve]], undefined, « result »).
            call(
                vm,
                Value::from_object(next_promise_capability.resolve().upcast::<Object>()),
                Value::UNDEFINED,
                &[Value::from_object(result)],
            )
            .must();

            // 3. Return nextPromiseCapability.[[Promise]].
            return Ok(Value::from_object(next_promise_capability.promise()));
        }

        // 3. Let kind be object’s kind.
        // 4. Let nextPromise be the result of getting the next iteration result with object’s target and object.
        let next_promise = T::get_the_next_iteration_result(&CallContext::new(vm, realm), &iterator.state);

        // 5. Let fulfillSteps be the following steps, given next:
        fn fulfill_steps<T: DefaultAsyncIteratorClasses>(
            vm: &Vm,
            iterator: IteratorCell<T>,
        ) -> ThrowCompletionOr<Value> {
            let next = vm.argument(0);

            // 1. Set object’s ongoing promise to null.
            iterator.ongoing_promise.set(None);

            // 2. If next is end of iteration, then:
            if next == END_OF_ITERATION {
                // 1. Set object’s is finished to true.
                iterator.is_finished.set(true);

                // 2. Return CreateIteratorResultObject(undefined, true).
                return Ok(Value::from_object(create_iterator_result_object(
                    vm,
                    iterator.realm,
                    Value::UNDEFINED,
                    true,
                )));
            }

            // 3. Otherwise, if interface has a pair asynchronously iterable declaration:
            // NB: The bindings only handle a value asynchronously iterable declaration.

            // 4. Otherwise:
            //    1. Assert: interface has a value asynchronously iterable declaration.
            //    2. Assert: next is a value of the type that appears in the declaration.
            //    3. Let value be next, converted to a JavaScript value.
            //    4. Return CreateIteratorResultObject(value, false).
            Ok(Value::from_object(create_iterator_result_object(
                vm,
                iterator.realm,
                next,
                false,
            )))
        }

        // 6. Let onFulfilled be CreateBuiltinFunction(fulfillSteps, « »).
        let on_fulfilled = native_function(vm, realm, iterator, fulfill_steps::<T>);

        // 7. Let rejectSteps be the following steps, given reason:
        fn reject_steps<T: DefaultAsyncIteratorClasses>(
            vm: &Vm,
            iterator: IteratorCell<T>,
        ) -> ThrowCompletionOr<Value> {
            // 1. Set object’s ongoing promise to null.
            iterator.ongoing_promise.set(None);

            // 2. Set object’s is finished to true.
            iterator.is_finished.set(true);

            // 3. Throw reason.
            Err(Throw::new(vm.argument(0)))
        }

        // 8. Let onRejected be CreateBuiltinFunction(rejectSteps, « »).
        let on_rejected = native_function(vm, realm, iterator, reject_steps::<T>);

        // 9. Perform PerformPromiseThen(nextPromise, onFulfilled, onRejected, nextPromiseCapability).
        promise_of(next_promise.promise()).perform_then(vm, on_fulfilled, on_rejected, Some(next_promise_capability));

        // 10. Return nextPromiseCapability.[[Promise]].
        Ok(Value::from_object(next_promise_capability.promise()))
    }

    // 9. Let ongoingPromise be object’s ongoing promise.
    // 10. If ongoingPromise is not null, then:
    // 11. Otherwise:
    chain_onto_ongoing_promise(vm, iterator, iterator, next_steps::<T>);

    // 12. Return object’s ongoing promise.
    let ongoing_promise = iterator
        .ongoing_promise
        .get()
        .expect("the iterator has an ongoing promise");
    Ok(Value::from_object(ongoing_promise.upcast::<Object>()))
}

/// The "return" method of the asynchronous iterator prototype object of an interface whose IDL marks it
/// [DefinesAsyncIteratorReturn], https://webidl.spec.whatwg.org/#es-asynchronous-iterator-prototype-object
pub(crate) fn async_iterator_return<T: DefaultAsyncIteratorClasses>(vm: &Vm) -> ThrowCompletionOr<Value> {
    let value = vm.argument(0);

    // 1. Let interface be the interface for which the asynchronous iterator prototype object exists.
    // 2. Let returnPromiseCapability be ! NewPromiseCapability(%Promise%).
    let return_promise_capability = create_promise(vm, this_value_realm(vm));

    // 3. Let thisValue be the this value.
    // 4. Let object be Completion(ToObject(thisValue)).
    // 5. IfAbruptRejectPromise(object, returnPromiseCapability).
    // 6. If object is a platform object, then perform a security check, passing:
    // 7. If object is not a default asynchronous iterator object for interface, then:
    let iterator = match validate_this::<T>(vm, return_promise_capability) {
        Ok(iterator) => iterator,
        Err(promise) => return Ok(promise),
    };
    let realm = iterator.realm;

    // 8. Let returnSteps be the following steps:
    fn return_steps<T: DefaultAsyncIteratorClasses>(
        vm: &Vm,
        (iterator, value): (IteratorCell<T>, Value),
    ) -> ThrowCompletionOr<Value> {
        let realm = iterator.realm;

        // 1. Let returnPromiseCapability be ! NewPromiseCapability(%Promise%).
        let return_promise_capability = create_promise(vm, realm);

        // 2. If object’s is finished is true, then:
        if iterator.is_finished.get() {
            // 1. Let result be CreateIteratorResultObject(value, true).
            let result = create_iterator_result_object(vm, realm, value, true);

            // 2. Perform ! Call(returnPromiseCapability.[[Resolve]], undefined, « result »).
            call(
                vm,
                Value::from_object(return_promise_capability.resolve().upcast::<Object>()),
                Value::UNDEFINED,
                &[Value::from_object(result)],
            )
            .must();

            // 3. Return returnPromiseCapability.[[Promise]].
            return Ok(Value::from_object(return_promise_capability.promise()));
        }

        // 3. Set object’s is finished to true.
        iterator.is_finished.set(true);

        // 4. Return the result of running the asynchronous iterator return algorithm for interface, given object’s
        //    target, object, and value.
        let promise = T::asynchronous_iterator_return(&CallContext::new(vm, realm), &iterator.state, value);
        Ok(Value::from_object(promise.promise()))
    }

    // 9. Let ongoingPromise be object’s ongoing promise.
    // 10. If ongoingPromise is not null, then:
    // 11. Otherwise:
    chain_onto_ongoing_promise(vm, iterator, (iterator, value), return_steps::<T>);

    // 12. Let fulfillSteps be the following steps:
    fn fulfill_steps(vm: &Vm, (realm, value): (Gc<Realm>, Value)) -> ThrowCompletionOr<Value> {
        // 1. Return CreateIteratorResultObject(value, true).
        Ok(Value::from_object(create_iterator_result_object(
            vm, realm, value, true,
        )))
    }

    // 13. Let onFulfilled be CreateBuiltinFunction(fulfillSteps, « »).
    let on_fulfilled = native_function(vm, realm, (realm, value), fulfill_steps);

    // 14. Perform PerformPromiseThen(object’s ongoing promise, onFulfilled, undefined, returnPromiseCapability).
    let ongoing_promise = iterator
        .ongoing_promise
        .get()
        .expect("the iterator has an ongoing promise");
    ongoing_promise.perform_then(vm, on_fulfilled, Value::UNDEFINED, Some(return_promise_capability));

    // 15. Return returnPromiseCapability.[[Promise]].
    Ok(Value::from_object(return_promise_capability.promise()))
}
