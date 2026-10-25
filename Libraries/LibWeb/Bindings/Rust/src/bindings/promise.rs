/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Promises as implementations hold them, https://webidl.spec.whatwg.org/#idl-promise: the PromiseCapability records
//! that LibWeb's C++ code calls WebIDL::Promise.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{
    Array, EmbedderCell, EmbedderClass, EmbedderValue, Gc, Must, NativeFunction, Object, Promise, PromiseCapability,
    PromiseState, PropertyKey, Realm, RejectionOperation, Trace, Value, Visitor, Vm, call, new_promise_capability,
};

use crate::bindings::interface::{with_temporary_execution_context, with_temporary_execution_context_and_callbacks};
use crate::bindings::{CallContext, Exception, WebIdlResult};

/// A Promise<T> value, https://webidl.spec.whatwg.org/#idl-promise
pub(crate) type WebIdlPromise = Gc<PromiseCapability>;

unsafe extern "C" {
    /// Queues a microtask that calls the function, a native function with no arguments, while an execution context of
    /// the realm is on the stack, as HTML::queue_a_microtask() queues one without a document.
    fn web_bindings_queue_a_microtask(realm: *mut c_void, function: *mut c_void);
}

/// The promise that LibWeb's C++ code returns, a WebIDL::Promise.
///
/// # Safety
///
/// `promise` must be a live WebIDL::Promise.
pub(crate) unsafe fn promise_from_abi(promise: *mut c_void) -> WebIdlPromise {
    let promise = NonNull::new(promise.cast::<PromiseCapability>()).expect("LibWeb returns a promise");
    // SAFETY: The caller passes a live promise capability.
    unsafe { Gc::from_non_null(promise) }
}

/// A new promise in the realm, https://webidl.spec.whatwg.org/#a-new-promise
pub(crate) fn create_promise(vm: &Vm, realm: Gc<Realm>) -> WebIdlPromise {
    // 1. Let constructor be realm.[[Intrinsics]].[[%Promise%]].
    let constructor = realm.intrinsics().promise_constructor(vm);

    // 2. Return ? NewPromiseCapability(constructor).
    // NB: When called with %Promise%, NewPromiseCapability can't throw.
    new_promise_capability(vm, Value::from_object(constructor)).must()
}

/// A promise resolved with the value in the realm, https://webidl.spec.whatwg.org/#a-promise-resolved-with
pub(crate) fn create_resolved_promise(vm: &Vm, realm: Gc<Realm>, value: Value) -> WebIdlPromise {
    // 1. Let value be the result of converting x to an ECMAScript value.
    // 2. Let constructor be realm.[[Intrinsics]].[[%Promise%]].
    // 3. Let promiseCapability be ? NewPromiseCapability(constructor).
    let promise = create_promise(vm, realm);

    // 4. Perform ! Call(promiseCapability.[[Resolve]], undefined, « value »).
    resolve_promise(vm, promise, value);

    // 5. Return promiseCapability.
    promise
}

/// A promise rejected with the reason in the realm, https://webidl.spec.whatwg.org/#a-promise-rejected-with
pub(crate) fn create_rejected_promise(vm: &Vm, realm: Gc<Realm>, reason: Value) -> WebIdlPromise {
    // 1. Let constructor be realm.[[Intrinsics]].[[%Promise%]].
    // 2. Let promiseCapability be ? NewPromiseCapability(constructor).
    let promise = create_promise(vm, realm);

    // 3. Perform ! Call(promiseCapability.[[Reject]], undefined, « r »).
    reject_promise(vm, promise, reason);

    // 4. Return promiseCapability.
    promise
}

/// A promise rejected with the exception, which is created in the realm of the promise.
pub(crate) fn create_rejected_promise_from_exception(vm: &Vm, realm: Gc<Realm>, exception: Exception) -> WebIdlPromise {
    let reason = exception_value(vm, realm, exception);
    create_rejected_promise(vm, realm, reason)
}

/// The value of an exception that is created in the realm.
pub(crate) fn exception_value(vm: &Vm, realm: Gc<Realm>, exception: Exception) -> Value {
    let _type_error_realm = vm.type_error_realm_scope(realm);
    with_temporary_execution_context(realm, || exception.throw(&CallContext::new(vm, realm)).value())
}

/// The realm of the promise's [[Promise]], which its reactions run in.
pub(crate) fn promise_realm(promise: WebIdlPromise) -> Gc<Realm> {
    promise.promise().shape().realm()
}

/// Resolves the promise with the value, https://webidl.spec.whatwg.org/#resolve
pub(crate) fn resolve_promise(vm: &Vm, promise: WebIdlPromise, value: Value) {
    // 1. If x is not given, then let it be the undefined value.
    // 2. Let value be the result of converting x to an ECMAScript value.
    // 3. Perform ! Call(p.[[Resolve]], undefined, « value »).
    call(
        vm,
        Value::from_object(promise.resolve().upcast::<Object>()),
        Value::UNDEFINED,
        &[value],
    )
    .must();
}

/// Rejects the promise with the reason, https://webidl.spec.whatwg.org/#reject
pub(crate) fn reject_promise(vm: &Vm, promise: WebIdlPromise, reason: Value) {
    // 1. Perform ! Call(p.[[Reject]], undefined, « r »).
    call(
        vm,
        Value::from_object(promise.reject().upcast::<Object>()),
        Value::UNDEFINED,
        &[reason],
    )
    .must();
}

/// Rejects the promise with the exception, which is created in the realm of the promise.
pub(crate) fn reject_promise_with_exception(vm: &Vm, promise: WebIdlPromise, exception: Exception) {
    let reason = exception_value(vm, promise_realm(promise), exception);
    reject_promise(vm, promise, reason);
}

/// The steps that a reaction to a promise performs given what the reaction captures and the value or the reason.
pub(crate) type ReactionSteps<C> = fn(&CallContext, C, Value) -> WebIdlResult<Value>;

/// What a native function that runs steps given by a plain function captures: the state that the steps are given, and
/// the steps, which the native function's behaviour itself cannot capture.
pub(crate) struct NativeSteps<C, S> {
    pub(crate) captures: C,
    pub(crate) steps: S,
}

// SAFETY: The steps are a plain function, which holds no cells.
unsafe impl<C: Trace, S> Trace for NativeSteps<C, S> {
    fn trace(&self, visitor: &mut Visitor) {
        self.captures.trace(visitor);
    }
}

/// Reacts to the promise with the steps, https://webidl.spec.whatwg.org/#dfn-perform-steps-once-promise-is-settled,
/// which are given the captures.
pub(crate) fn react_to_promise<C: Trace + Copy + 'static>(
    vm: &Vm,
    promise: WebIdlPromise,
    captures: C,
    on_fulfilled: Option<ReactionSteps<C>>,
    on_rejected: Option<ReactionSteps<C>>,
) -> WebIdlPromise {
    let realm = promise_realm(promise);
    let name = PropertyKey::from_utf8("");

    // 1. Let onFulfilledSteps be the following steps given argument V:
    let on_fulfilled_steps = |vm: &Vm, function: &NativeSteps<(C, Gc<Realm>), Option<ReactionSteps<C>>>| {
        let (captures, realm) = function.captures;
        let cx = CallContext::new(vm, realm);

        // 1. Let value be the result of converting V to an IDL value of type T.
        let value = vm.argument(0);

        // 2. If there is a set of steps to be run if the promise was fulfilled, then let result be the result of
        //    performing them, given value if T is not undefined. Otherwise, let result be value.
        let result = match function.steps {
            Some(steps) => steps(&cx, captures, value).map_err(|exception| exception.throw(&cx))?,
            None => value,
        };

        // 3. Return result, converted to an ECMAScript value.
        Ok(result)
    };

    // 2. Let onFulfilled be CreateBuiltinFunction(onFulfilledSteps, 1, "", « »):
    let on_fulfilled_function = NativeFunction::create(
        vm,
        NativeSteps {
            captures: (captures, realm),
            steps: on_fulfilled,
        },
        on_fulfilled_steps,
        1,
        &name,
        Some(realm),
        None,
        None,
    );

    // 3. Let onRejectedSteps be the following steps given argument R:
    let on_rejected_steps = |vm: &Vm, function: &NativeSteps<(C, Gc<Realm>), Option<ReactionSteps<C>>>| {
        let (captures, realm) = function.captures;
        let cx = CallContext::new(vm, realm);

        // 1. Let reason be the result of converting R to an IDL value of type any.
        let reason = vm.argument(0);

        // 2. If there is a set of steps to be run if the promise was rejected, then let result be the result of
        //    performing them, given reason. Otherwise, let result be a promise rejected with reason.
        let result = match function.steps {
            Some(steps) => steps(&cx, captures, reason).map_err(|exception| exception.throw(&cx))?,
            None => Value::from_object(create_rejected_promise(vm, realm, reason).promise()),
        };

        // 3. Return result, converted to an ECMAScript value.
        Ok(result)
    };

    // 4. Let onRejected be CreateBuiltinFunction(onRejectedSteps, 1, "", « »):
    let on_rejected_function = NativeFunction::create(
        vm,
        NativeSteps {
            captures: (captures, realm),
            steps: on_rejected,
        },
        on_rejected_steps,
        1,
        &name,
        Some(realm),
        None,
        None,
    );

    // 5. Let constructor be promise.[[Promise]].[[Realm]].[[Intrinsics]].[[%Promise%]].
    // 6. Let newCapability be ? NewPromiseCapability(constructor).
    let new_capability = create_promise(vm, realm);

    // 7. Perform PerformPromiseThen(promise.[[Promise]], onFulfilled, onRejected, newCapability).
    promise_object(promise).perform_then(
        vm,
        Value::from_object(on_fulfilled_function.upcast::<Object>()),
        Value::from_object(on_rejected_function.upcast::<Object>()),
        Some(new_capability),
    );

    // 8. Return newCapability.
    new_capability
}

/// Performs the steps upon fulfillment of the promise, https://webidl.spec.whatwg.org/#upon-fulfillment
pub(crate) fn upon_fulfillment<C: Trace + Copy + 'static>(
    vm: &Vm,
    promise: WebIdlPromise,
    captures: C,
    steps: ReactionSteps<C>,
) -> WebIdlPromise {
    // 1. Return the result of reacting to promise:
    //    - If promise was fulfilled with value v, then:
    //      1. Perform steps with v.
    react_to_promise(vm, promise, captures, Some(steps), None)
}

/// Performs the steps upon rejection of the promise, https://webidl.spec.whatwg.org/#upon-rejection
pub(crate) fn upon_rejection<C: Trace + Copy + 'static>(
    vm: &Vm,
    promise: WebIdlPromise,
    captures: C,
    steps: ReactionSteps<C>,
) -> WebIdlPromise {
    // 1. Return the result of reacting to promise:
    //    - If promise was rejected with reason r, then:
    //      1. Perform steps with r.
    react_to_promise(vm, promise, captures, None, Some(steps))
}

/// Marks the promise as handled, https://webidl.spec.whatwg.org/#mark-a-promise-as-handled
pub(crate) fn mark_promise_as_handled(vm: &Vm, promise: WebIdlPromise) {
    // To mark as handled a Promise<T> promise, set promise.[[Promise]].[[PromiseIsHandled]] to true.
    let promise_object = promise_object(promise);

    // AD-HOC: If the promise was already rejected, tell the host it is now handled, matching what
    //         PerformPromiseThen does when a handler is attached to a rejected promise. This pulls the
    //         promise out of the about-to-be-notified rejected promises list, so specs that reject a
    //         promise and then mark it as handled (the Streams API does this on every reader and writer
    //         release) don't leave a trail of queued unhandled-rejection notification tasks behind.
    if promise_object.state() == PromiseState::Rejected && !promise_object.is_handled() {
        vm.host_promise_rejection_tracker()(vm, promise_object, RejectionOperation::Handle);
    }

    promise_object.set_is_handled();
}

pub(crate) fn is_promise_fulfilled(promise: WebIdlPromise) -> bool {
    promise_object(promise).state() == PromiseState::Fulfilled
}

/// A promise that settles once every promise has, https://webidl.spec.whatwg.org/#waiting-for-all-promise
pub(crate) fn get_promise_for_wait_for_all(vm: &Vm, realm: Gc<Realm>, promises: &[WebIdlPromise]) -> WebIdlPromise {
    // 1. Let promise be a new promise of type Promise<sequence<T>> in realm.
    let promise = create_promise(vm, realm);

    // 2. Let successSteps be the following steps, given results:
    // 3. Let failureSteps be the following steps, given reason:
    // 4. Wait for all with promises, given successSteps and failureSteps.
    wait_for_all(vm, realm, promises, promise);

    // 5. Return promise.
    promise
}

/// The state of waiting for all of some promises, https://webidl.spec.whatwg.org/#wait-for-all, whose success and
/// failure steps settle `promise`.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct WaitForAll {
    promise: WebIdlPromise,
    realm: Gc<Realm>,
    rejected: Cell<bool>,
    result: Gc<Array>,
    #[gc(untraced)]
    total: usize,
    #[gc(untraced)]
    fulfilled_count: Cell<usize>,
}

static WAIT_FOR_ALL_CLASS: EmbedderClass<WaitForAll> = EmbedderClass::new("WaitForAll");

impl EmbedderValue for WaitForAll {
    const CLASS: &'static EmbedderClass<Self> = &WAIT_FOR_ALL_CLASS;
}

impl WaitForAll {
    // 2. Let successSteps be the following steps, given results:
    fn success_steps(&self, vm: &Vm) {
        with_temporary_execution_context_and_callbacks(self.realm, || {
            // 1. Resolve promise with results.
            resolve_promise(vm, self.promise, Value::from_object(self.result.upcast::<Object>()));
        });
    }

    // 3. Let failureSteps be the following steps, given reason:
    fn failure_steps(&self, vm: &Vm, reason: Value) {
        with_temporary_execution_context_and_callbacks(self.realm, || {
            // 1. Reject promise with reason.
            reject_promise(vm, self.promise, reason);
        });
    }
}

/// Waits for all of the promises, https://webidl.spec.whatwg.org/#wait-for-all, with the success and failure steps of
/// getting a promise to wait for all, which settle `promise`.
fn wait_for_all(vm: &Vm, realm: Gc<Realm>, promises: &[WebIdlPromise], promise: WebIdlPromise) {
    let name = PropertyKey::from_utf8("");

    // 1. Let fullfilledCount be 0.
    // 2. Let rejected be false.
    // 5. Let total be promises’s size.
    // 8. Let result be a list containing total null values.
    let total = promises.len();
    let result = Array::create_from(vm, realm, &vec![Value::NULL; total]);
    let state = WAIT_FOR_ALL_CLASS.allocate(vm, |_| WaitForAll {
        promise,
        realm,
        rejected: Cell::new(false),
        result,
        total,
        fulfilled_count: Cell::new(0),
    });

    // 3. Let rejectionHandlerSteps be the following steps given arg:
    let rejection_handler_steps = |vm: &Vm, state: &Gc<EmbedderCell<WaitForAll>>| {
        // 1. If rejected is true, abort these steps.
        if state.rejected.get() {
            return Ok(Value::UNDEFINED);
        }

        // 2. Set rejected to true.
        state.rejected.set(true);

        // 3. Perform failureSteps given arg.
        state.failure_steps(vm, vm.argument(0));
        Ok(Value::UNDEFINED)
    };

    // 4. Let rejectionHandler be CreateBuiltinFunction(rejectionHandlerSteps, 1, "", « »):
    let rejection_handler =
        NativeFunction::create(vm, state, rejection_handler_steps, 1, &name, Some(realm), None, None);

    // 6. If total is 0, then:
    if total == 0 {
        // 1. Queue a microtask to perform successSteps given « ».
        queue_a_microtask(vm, realm, state, |vm, state| state.success_steps(vm));

        // 2. Return.
        return;
    }

    // 7. Let index be 0.
    // 9. For each promise of promises:
    for (index, each_promise) in promises.iter().enumerate() {
        // 1. Let promiseIndex be index.
        let promise_index = u32::try_from(index).expect("a list of promises fits an array");

        // 2. Let fulfillmentHandler be the following steps given arg:
        let fulfillment_handler_steps = |vm: &Vm, captures: &(Gc<EmbedderCell<WaitForAll>>, u32)| {
            let (state, promise_index) = *captures;

            // 1. Set result[promiseIndex] to arg.
            state
                .result
                .create_data_property(vm, &PropertyKey::from(promise_index), vm.argument(0), None, None)
                .must();

            // 2. Set fullfilledCount to fullfilledCount + 1.
            state.fulfilled_count.set(state.fulfilled_count.get() + 1);

            // 3. If fullfilledCount equals total, then perform successSteps given result.
            if state.fulfilled_count.get() == state.total {
                state.success_steps(vm);
            }
            Ok(Value::UNDEFINED)
        };

        // 3. Let fulfillmentHandler be CreateBuiltinFunction(fulfillmentHandler, 1, "", « »):
        let fulfillment_handler = NativeFunction::create(
            vm,
            (state, promise_index),
            fulfillment_handler_steps,
            1,
            &name,
            Some(realm),
            None,
            None,
        );

        // 4. Perform PerformPromiseThen(promise, fulfillmentHandler, rejectionHandler).
        promise_object(*each_promise).perform_then(
            vm,
            Value::from_object(fulfillment_handler.upcast::<Object>()),
            Value::from_object(rejection_handler.upcast::<Object>()),
            None,
        );

        // 5. Set index to index + 1.
    }
}

/// Queues a microtask that performs the steps with the captures,
/// https://html.spec.whatwg.org/multipage/webappapis.html#queue-a-microtask
pub(crate) fn queue_a_microtask<C: Trace + Copy + 'static>(vm: &Vm, realm: Gc<Realm>, captures: C, steps: fn(&Vm, C)) {
    let function = NativeFunction::create(
        vm,
        NativeSteps { captures, steps },
        |vm: &Vm, function: &NativeSteps<C, fn(&Vm, C)>| {
            (function.steps)(vm, function.captures);
            Ok(Value::UNDEFINED)
        },
        0,
        &PropertyKey::from_utf8(""),
        Some(realm),
        None,
        None,
    );
    // SAFETY: The realm and the function are live.
    unsafe { web_bindings_queue_a_microtask(realm.as_ptr().cast(), function.as_ptr().cast()) };
}

/// The [[Promise]] of a promise capability that LibWeb created, which is a Promise.
fn promise_object(promise: WebIdlPromise) -> Gc<Promise> {
    promise
        .promise()
        .downcast::<Promise>()
        .expect("the promise of a capability that the bindings create is a Promise")
}
