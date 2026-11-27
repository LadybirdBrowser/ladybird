/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::embedding::host::host_object::host_object_has_impure_lookup_internal_methods;
use crate::gc::class::{Class, ExternalMemorySize, GcCell, define_cell};
use crate::gc::defer_gc::DeferGc;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::heap_function::create_heap_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call_function_object;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::TypeError;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::module_namespace_object::ModuleNamespaceObject;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::promise_capability::{PromiseCapability, new_promise_capability};
use crate::runtime::promise_jobs::{
    ThenAction, create_promise_reaction_job, create_promise_resolve_thenable_job, perform_promise_resolve_thenable,
};
use crate::runtime::promise_reaction::{PromiseReaction, PromiseReactionType};
use crate::runtime::promise_resolving_function::PromiseResolvingFunction;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::special_property_cache::{self, SpecialPropertyKey};
use crate::runtime::value::same_value;

// 27.2.4.7.1 PromiseResolve ( C, x ), https://tc39.es/ecma262/#sec-promise-resolve
pub fn promise_resolve(vm: &Vm, constructor: Gc<Object>, value: Value) -> ThrowCompletionOr<Gc<Object>> {
    let realm = vm.current_realm().expect("there is a current realm");

    // 1. If IsPromise(x) is true, then
    if value.is_object()
        && let Some(promise) = value.as_object().downcast::<Promise>()
    {
        // a. Let xConstructor be ? Get(x, "constructor").
        let value_constructor = special_property_cache::get(vm, &value.as_object(), SpecialPropertyKey::Constructor)?;

        // b. If SameValue(xConstructor, C) is true, return x.
        if same_value(value_constructor, Value::from_object(constructor)) {
            return Ok(promise.upcast());
        }
    }

    // OPTIMIZATION: Resolving an intrinsic Promise with a primitive cannot invoke user code, and the capability's resolving
    //               functions do not escape. Create and fulfill the Promise directly instead of allocating those functions.
    if constructor == realm.intrinsics().promise_constructor(vm).upcast() && !value.is_object() {
        let promise = Promise::create(vm, realm);
        promise.fulfill(vm, value);
        return Ok(promise.upcast());
    }

    // 2. Let promiseCapability be ? NewPromiseCapability(C).
    let promise_capability = new_promise_capability(vm, Value::from_object(constructor))?;

    // 3. Perform ? Call(promiseCapability.[[Resolve]], undefined, « x »).
    call_function_object(vm, promise_capability.resolve(), Value::UNDEFINED, &[value])?;

    // 4. Return promiseCapability.[[Promise]].
    Ok(promise_capability.promise())
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PropertyAccessKind {
    Any,
    Set,
    Get,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ThenCallTiming {
    Sync,
    Deferred,
}

// PropertyAccessCouldRunUserCode ( o, propertyKey, kind ), https://tc39.es/proposal-thenable-curtailment/#sec-has-property-which-could-run-user-code
pub fn property_access_could_run_user_code(
    vm: &Vm,
    mut object: Gc<Object>,
    property_key: &PropertyKey,
    kind: PropertyAccessKind,
) -> bool {
    loop {
        // 1. Assert: The following steps do not run user code.

        // 2. If o has the [[GetPrototypeOf]] and [[GetOwnProperty]] internal methods as defined in 10.5, return true.
        if object.is_proxy_object() {
            return true;
        }

        // 3. If o has the [[GetPrototypeOf]] and [[GetOwnProperty]] internal methods as defined in 10.4.6, return true.
        if object.is::<ModuleNamespaceObject>() {
            return true;
        }

        // 4. If HostObjectHasImpureLookupInternalMethods(o) is true, return true.
        if host_object_has_impure_lookup_internal_methods(&object) {
            return true;
        }

        // 5. Let desc be ! o.[[GetOwnProperty]](propertyKey).
        let descriptor = object.internal_get_own_property(vm, property_key).must();

        // 6. If desc is not undefined, then
        if let Some(descriptor) = descriptor {
            // a. If IsAccessorDescriptor(desc) is true, then
            if descriptor.is_accessor_descriptor() {
                // i. If kind is either any or get, and desc.[[Get]] is not undefined, return true.
                if matches!(kind, PropertyAccessKind::Any | PropertyAccessKind::Get)
                    && matches!(descriptor.get, Some(Some(_)))
                {
                    return true;
                }

                // ii. If kind is either any or set, and desc.[[Set]] is not undefined, return true.
                if matches!(kind, PropertyAccessKind::Any | PropertyAccessKind::Set)
                    && matches!(descriptor.set, Some(Some(_)))
                {
                    return true;
                }
            }

            // b. Return false.
            return false;
        }

        // 7. Let proto be ! o.[[GetPrototypeOf]]().
        let prototype = object.internal_get_prototype_of(vm).must();

        // 8. If proto is null, return false.
        let Some(prototype) = prototype else {
            return false;
        };

        // 9. Return PropertyAccessCouldRunUserCode(proto, propertyKey, kind).
        object = prototype;
    }
}

// RequiresDeferredPromiseResolution ( value ), https://tc39.es/proposal-thenable-curtailment/#sec-requires-deferred-promise-resolution
fn requires_deferred_promise_resolution(vm: &Vm, value: Value) -> bool {
    // 1. If value is not an Object, return false.
    if !value.is_object() {
        return false;
    }
    let object = value.as_object();

    // OPTIMIZATION: The special property cache only knows what Get gives without running code.
    if let Some(then_value) = special_property_cache::known(&object, SpecialPropertyKey::Then) {
        return then_value.is_function();
    }

    // 2. If PropertyAccessCouldRunUserCode(value, "then", get) is true, return true.
    if property_access_could_run_user_code(vm, object, &vm.names.then, PropertyAccessKind::Get) {
        return true;
    }

    // 3. Let thenValue be ! Get(value, "then").
    // AD-HOC: Get throws when it runs out of native stack. The deferred steps then perform it again.
    let Ok(then_value) = special_property_cache::get(vm, &object, SpecialPropertyKey::Then) else {
        return true;
    };

    // 4. If IsCallable(thenValue) is true, return true.
    // 5. Return false.
    then_value.is_function()
}

// SafePromiseResolve ( promiseCapability, resolution ), https://tc39.es/proposal-thenable-curtailment/#sec-safe-promise-resolve
pub fn safe_promise_resolve(
    vm: &Vm,
    promise_capability: Gc<PromiseCapability>,
    resolution: Value,
) -> ThrowCompletionOr<Value> {
    // 1. If RequiresDeferredPromiseResolution(resolution) is false, then
    if !requires_deferred_promise_resolution(vm, resolution) {
        // a. Return ? Call(promiseCapability.[[Resolve]], undefined, « resolution »).
        return call_function_object(vm, promise_capability.resolve(), Value::UNDEFINED, &[resolution]);
    }

    // 2. Let promise be promiseCapability.[[Promise]].
    let promise = promise_capability
        .promise()
        .downcast::<Promise>()
        .expect("the capability has a Promise");

    // 3. Let deferredSteps be a new Abstract Closure that captures promise and resolution and performs the following steps when called:
    let deferred_steps = create_heap_function(vm, (promise, resolution), |vm, &(promise, resolution)| {
        // a. Perform ? PerformPromiseResolution(promise, resolution, deferred).
        promise.perform_promise_resolution(vm, resolution, ThenCallTiming::Deferred);

        // b. Return undefined.
        Ok(Value::UNDEFINED)
    });

    // 4. Let deferredThen be CreateBuiltinFunction(deferredSteps, 0, "", « »).
    // 5. Let wrapper be OrdinaryObjectCreate(null).
    // 6. Perform ! CreateDataPropertyOrThrow(wrapper, "then", deferredThen).
    // 7. Return ? Call(promiseCapability.[[Resolve]], undefined, « wrapper »).
    // AD-HOC: Resolving promise with wrapper marks promiseCapability as already resolved and enqueues a job that
    //         performs deferredSteps. We do both directly, because an embedder may resolve a promise outside of script,
    //         where there is no current realm and HostMakeJobCallback has no state to capture.
    let resolve_function = promise_capability
        .resolve()
        .downcast::<PromiseResolvingFunction>()
        .expect("the capability has the resolve function of a Promise");
    if resolve_function.mark_already_resolved() {
        vm.host_enqueue_promise_job()(vm, deferred_steps, Some(promise.shape().realm()));
    }
    Ok(Value::UNDEFINED)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Trace)]
pub enum PromiseState {
    Pending,
    Fulfilled,
    Rejected,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RejectionOperation {
    Reject,
    Handle,
}

/// The Record { [[Resolve]], [[Reject]] } CreateResolvingFunctions returns.
pub struct ResolvingFunctions {
    pub resolve: Gc<FunctionObject>,
    pub reject: Gc<FunctionObject>,
}

// 27.2 Promise Objects, https://tc39.es/ecma262/#sec-promise-objects
#[repr(C)]
#[derive(Trace)]
pub struct Promise {
    base: Object,
    // 27.2.6 Properties of Promise Instances, https://tc39.es/ecma262/#sec-properties-of-promise-instances
    state: Cell<PromiseState>, // [[PromiseState]]
    is_handled: Cell<bool>,    // [[PromiseIsHandled]]
    /// Whether something may resolve the promise: resolving functions CreateResolvingFunctions made for it, or the
    /// direct resolve() of a promise nothing else resolves.
    has_resolver: Cell<bool>,
    result: Cell<Value>, // [[PromiseResult]]
    #[gc(owned = "only the mutator changes the reactions, and only the promise's destructor frees them")]
    fulfill_reactions: GcRefCell<Vec<Gc<PromiseReaction>>>, // [[PromiseFulfillReactions]]
    #[gc(owned = "only the mutator changes the reactions, and only the promise's destructor frees them")]
    reject_reactions: GcRefCell<Vec<Gc<PromiseReaction>>>, // [[PromiseRejectReactions]]
}

define_cell!(
    Promise,
    Object,
    extends: [Object],
    external_memory_size: external_memory_size
);

// Promise::external_memory_size(), which counts the reactions in place of the storage Object counts.
impl ExternalMemorySize for Promise {
    fn external_memory_size(&self) -> usize {
        self.fulfill_reactions.borrow().capacity() * size_of::<Gc<PromiseReaction>>()
            + self.reject_reactions.borrow().capacity() * size_of::<Gc<PromiseReaction>>()
    }
}

impl Deref for Promise {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Promise {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<Promise> {
        realm.create_object(vm, |defer_gc| {
            Promise::new(
                defer_gc,
                vm,
                realm,
                Self::CLASS,
                realm.intrinsics().promise_prototype(vm),
            )
        })
    }

    /// A promise with `prototype`, for `class`, which is Promise or a class that extends it.
    pub fn new(_: &DeferGc<'_>, vm: &Vm, realm: Gc<Realm>, class: &'static Class, prototype: Gc<Object>) -> Promise {
        Promise {
            base: Object::new_with_prototype(
                vm,
                class,
                realm,
                Some(prototype),
                MayInterfereWithIndexedPropertyAccess::No,
            ),
            state: Cell::new(PromiseState::Pending),
            is_handled: Cell::new(false),
            has_resolver: Cell::new(false),
            result: Cell::new(Value::UNDEFINED),
            fulfill_reactions: GcRefCell::new(Vec::new()),
            reject_reactions: GcRefCell::new(Vec::new()),
        }
    }

    pub fn as_gc(&self) -> Gc<Promise> {
        Gc::from_ref(self)
    }

    pub fn state(&self) -> PromiseState {
        self.state.get()
    }

    pub fn result(&self) -> Value {
        self.result.get()
    }

    pub fn is_handled(&self) -> bool {
        self.is_handled.get()
    }

    pub fn set_is_handled(&self) {
        self.is_handled.set(true);
    }

    /// Sets [[PromiseIsHandled]] to true, first telling the host when that handles a rejection, as PerformPromiseThen
    /// does when it adds a reaction to a settled promise.
    pub fn mark_handled(&self, vm: &Vm) {
        if self.state() == PromiseState::Rejected && !self.is_handled() {
            vm.host_promise_rejection_tracker()(vm, self.as_gc(), RejectionOperation::Handle);
        }
        self.set_is_handled();
    }

    fn is_settled(&self) -> bool {
        matches!(self.state.get(), PromiseState::Fulfilled | PromiseState::Rejected)
    }

    // 27.2.1.3 CreateResolvingFunctions ( promise ), https://tc39.es/ecma262/#sec-createresolvingfunctions
    pub fn create_resolving_functions(&self, vm: &Vm) -> ResolvingFunctions {
        self.create_resolving_functions_in_realm(vm, vm.current_realm().expect("there is a current realm"))
    }

    pub fn create_resolving_functions_in_realm(&self, vm: &Vm, realm: Gc<Realm>) -> ResolvingFunctions {
        self.has_resolver.set(true);

        // 1. Let alreadyResolved be the Record { [[Value]]: false }.
        // NOTE: This is stored directly on the resolve function. The reject function keeps the resolve function alive and
        //       uses the same bit.

        // 2. Let stepsResolve be the algorithm steps defined in Promise Resolve Functions.
        // 3. Let lengthResolve be the number of non-optional parameters of the function definition in Promise Resolve Functions.
        // 4. Let resolve be CreateBuiltinFunction(stepsResolve, lengthResolve, "", « [[Promise]], [[AlreadyResolved]] »).
        // 5. Set resolve.[[Promise]] to promise.
        // 6. Set resolve.[[AlreadyResolved]] to alreadyResolved.

        // 27.2.1.3.2 Promise Resolve Functions, https://tc39.es/ecma262/#sec-promise-resolve-functions
        let resolve_function = PromiseResolvingFunction::create_resolve(vm, realm, self.as_gc());

        // 7. Let stepsReject be the algorithm steps defined in Promise Reject Functions.
        // 8. Let lengthReject be the number of non-optional parameters of the function definition in Promise Reject Functions.
        // 9. Let reject be CreateBuiltinFunction(stepsReject, lengthReject, "", « [[Promise]], [[AlreadyResolved]] »).
        // 10. Set reject.[[Promise]] to promise.
        // 11. Set reject.[[AlreadyResolved]] to alreadyResolved.

        // 27.2.1.3.1 Promise Reject Functions, https://tc39.es/ecma262/#sec-promise-reject-functions
        let reject_function = PromiseResolvingFunction::create_reject(vm, realm, self.as_gc(), resolve_function);

        // 12. Return the Record { [[Resolve]]: resolve, [[Reject]]: reject }.
        ResolvingFunctions {
            resolve: resolve_function.upcast(),
            reject: reject_function.upcast(),
        }
    }

    // 27.2.1.3.2 Promise Resolve Functions, https://tc39.es/ecma262/#sec-promise-resolve-functions
    pub fn resolve_function_steps(vm: &Vm, promise: Gc<Promise>, already_resolved: &Cell<bool>) -> Value {
        let resolution = vm.argument(0);

        // 1. Let F be the active function object.
        // 2. Assert: F has a [[Promise]] internal slot whose value is an Object.
        // 3. Let promise be F.[[Promise]].

        // 4. Let alreadyResolved be F.[[AlreadyResolved]].
        // 5. If alreadyResolved.[[Value]] is true, return undefined.
        if already_resolved.get() {
            return Value::UNDEFINED;
        }

        // 6. Set alreadyResolved.[[Value]] to true.
        already_resolved.set(true);

        // NB: PerformPromiseResolution performs steps 7 to 16.
        promise.perform_promise_resolution(vm, resolution, ThenCallTiming::Sync);
        Value::UNDEFINED
    }

    /// Resolves the promise with `resolution` like the resolve function of new resolving functions would, for a promise
    /// that has none and that nothing else resolves.
    ///
    /// OPTIMIZATION: Nothing can call resolving functions that were never created, so resolving the promise directly is
    ///               the same as creating them and calling the resolve function, which finds alreadyResolved false.
    pub fn resolve(&self, vm: &Vm, resolution: Value) {
        assert!(
            !self.has_resolver.get(),
            "only a promise that nothing else resolves is resolved directly"
        );
        self.has_resolver.set(true);
        self.perform_promise_resolution(vm, resolution, ThenCallTiming::Sync);
    }

    // PerformPromiseResolution ( promise, resolution, thenCallTiming ), https://tc39.es/proposal-thenable-curtailment/#sec-perform-promise-resolution
    pub fn perform_promise_resolution(&self, vm: &Vm, resolution: Value, then_call_timing: ThenCallTiming) {
        let realm = vm.current_realm().expect("there is a current realm");
        let promise = self.as_gc();

        // 1. Assert: promise has a [[PromiseState]] internal slot.

        // 2. If SameValue(resolution, promise) is true, then
        if resolution.is_object() && resolution.as_object() == promise.upcast() {
            // a. Let selfResolutionError be a newly created TypeError object.
            let self_resolution_error =
                TypeError::create_with_message(vm, realm, Utf16String::from_utf8("Cannot resolve promise with itself"));

            // b. Perform RejectPromise(promise, selfResolutionError).
            promise.reject(vm, Value::from_object(self_resolution_error));

            // c. Return unused.
            return;
        }

        // 3. If resolution is not an Object, then
        if !resolution.is_object() {
            // a. Perform FulfillPromise(promise, resolution).
            promise.fulfill(vm, resolution);

            // b. Return unused.
            return;
        }

        // 4. Let then be Completion(Get(resolution, "then")).
        let then = special_property_cache::get(vm, &resolution.as_object(), SpecialPropertyKey::Then);

        // 5. If then is an abrupt completion, then
        let then_action = match then {
            Err(throw) => {
                // a. Perform RejectPromise(promise, then.[[Value]]).
                promise.reject(vm, throw.value());

                // b. Return unused.
                return;
            }
            // 6. Let thenAction be then.[[Value]].
            Ok(then_action) => then_action,
        };

        // 7. If IsCallable(thenAction) is false, then
        if !then_action.is_function() {
            // a. Perform FulfillPromise(promise, resolution).
            promise.fulfill(vm, resolution);

            // b. Return unused.
            return;
        }

        // 8. If thenCallTiming is deferred, then
        if then_call_timing == ThenCallTiming::Deferred {
            // a. Perform ! PerformPromiseResolveThenable(promise, resolution, thenAction).
            perform_promise_resolve_thenable(vm, promise, resolution, ThenAction::Function(then_action.as_function()))
                .must();

            // b. Return unused.
            return;
        }

        // 9. Let thenJobCallback be HostMakeJobCallback(thenAction).
        let then_job_callback = vm.host_make_job_callback()(vm, then_action.as_function());

        // 10. Let job be NewPromiseResolveThenableJob(promise, resolution, thenJobCallback).
        let job = create_promise_resolve_thenable_job(vm, promise, resolution, then_job_callback);

        // 11. Perform HostEnqueuePromiseJob(job.[[Job]], job.[[Realm]]).
        vm.host_enqueue_promise_job()(vm, job.job, job.realm);

        // 12. Return unused.
    }

    // 27.2.1.3.1 Promise Reject Functions, https://tc39.es/ecma262/#sec-promise-reject-functions
    pub fn reject_function_steps(vm: &Vm, promise: Gc<Promise>, already_resolved: &Cell<bool>) -> Value {
        let reason = vm.argument(0);

        // NB: An exception that rejects a promise is no longer thrown, even when the promise is already resolved.
        vm.forget_throw_site_of(reason);

        // 1. Let F be the active function object.
        // 2. Assert: F has a [[Promise]] internal slot whose value is an Object.
        // 3. Let promise be F.[[Promise]].

        // 4. Let alreadyResolved be F.[[AlreadyResolved]].
        // 5. If alreadyResolved.[[Value]] is true, return undefined.
        if already_resolved.get() {
            return Value::UNDEFINED;
        }

        // 6. Set alreadyResolved.[[Value]] to true.
        already_resolved.set(true);

        // 7. Perform RejectPromise(promise, reason).
        promise.reject(vm, reason);

        // 8. Return undefined.
        Value::UNDEFINED
    }

    // 27.2.1.4 FulfillPromise ( promise, value ), https://tc39.es/ecma262/#sec-fulfillpromise
    pub fn fulfill(&self, vm: &Vm, value: Value) {
        // 1. Assert: The value of promise.[[PromiseState]] is pending.
        assert!(self.state.get() == PromiseState::Pending);

        // 2. Let reactions be promise.[[PromiseFulfillReactions]].
        // NOTE: This is a noop, we do these steps in a slightly different order.

        // 3. Set promise.[[PromiseResult]] to value.
        self.result.set(value);

        // 4. Set promise.[[PromiseFulfillReactions]] to undefined.
        // 5. Set promise.[[PromiseRejectReactions]] to undefined.

        // 6. Set promise.[[PromiseState]] to fulfilled.
        self.state.set(PromiseState::Fulfilled);

        // 7. Perform TriggerPromiseReactions(reactions, value).
        self.trigger_reactions(vm);
        drop(self.fulfill_reactions.replace(Vec::new()));
        drop(self.reject_reactions.replace(Vec::new()));

        // 8. Return unused.
    }

    // 27.2.1.7 RejectPromise ( promise, reason ), https://tc39.es/ecma262/#sec-rejectpromise
    pub fn reject(&self, vm: &Vm, reason: Value) {
        // NB: An exception that rejects a promise is no longer thrown.
        vm.forget_throw_site_of(reason);

        // 1. Assert: The value of promise.[[PromiseState]] is pending.
        assert!(self.state.get() == PromiseState::Pending);

        // 2. Let reactions be promise.[[PromiseRejectReactions]].
        // NOTE: This is a noop, we do these steps in a slightly different order.

        // 3. Set promise.[[PromiseResult]] to reason.
        self.result.set(reason);

        // 4. Set promise.[[PromiseFulfillReactions]] to undefined.
        // 5. Set promise.[[PromiseRejectReactions]] to undefined.

        // 6. Set promise.[[PromiseState]] to rejected.
        self.state.set(PromiseState::Rejected);

        // 7. If promise.[[PromiseIsHandled]] is false, perform HostPromiseRejectionTracker(promise, "reject").
        if !self.is_handled.get() {
            vm.host_promise_rejection_tracker()(vm, self.as_gc(), RejectionOperation::Reject);
        }

        // 8. Perform TriggerPromiseReactions(reactions, reason).
        self.trigger_reactions(vm);
        drop(self.fulfill_reactions.replace(Vec::new()));
        drop(self.reject_reactions.replace(Vec::new()));

        // 9. Return unused.
    }

    // 27.2.1.8 TriggerPromiseReactions ( reactions, argument ), https://tc39.es/ecma262/#sec-triggerpromisereactions
    fn trigger_reactions(&self, vm: &Vm) {
        assert!(self.is_settled());
        let reactions = if self.state.get() == PromiseState::Fulfilled {
            &self.fulfill_reactions
        } else {
            &self.reject_reactions
        };

        // 1. For each element reaction of reactions, do
        let reaction_count = reactions.borrow().len();
        for index in 0..reaction_count {
            let reaction = reactions.borrow()[index];

            // a. Let job be NewPromiseReactionJob(reaction, argument).
            let job = create_promise_reaction_job(vm, reaction, self.result.get());

            // b. Perform HostEnqueuePromiseJob(job.[[Job]], job.[[Realm]]).
            vm.host_enqueue_promise_job()(vm, job.job, job.realm);
        }

        // 2. Return unused.
    }

    // 27.2.5.4.1 PerformPromiseThen ( promise, onFulfilled, onRejected [ , resultCapability ] ), https://tc39.es/ecma262/#sec-performpromisethen
    pub fn perform_then(
        &self,
        vm: &Vm,
        on_fulfilled: Value,
        on_rejected: Value,
        result_capability: Option<Gc<PromiseCapability>>,
    ) -> Value {
        // 1. Assert: IsPromise(promise) is true.
        // 2. If resultCapability is not present, then
        //     a. Set resultCapability to undefined.

        // OPTIMIZATION: A settled promise only uses one of these reactions. Create both reactions for a pending promise, but
        //               defer their creation until the promise state determines which ones are needed.
        let create_fulfill_reaction = || {
            // 3. If IsCallable(onFulfilled) is false, then
            //     a. Let onFulfilledJobCallback be empty.
            // 4. Else,
            //     a. Let onFulfilledJobCallback be HostMakeJobCallback(onFulfilled).
            let on_fulfilled_job_callback = on_fulfilled
                .is_function()
                .then(|| vm.host_make_job_callback()(vm, on_fulfilled.as_function()));

            // 7. Let fulfillReaction be the PromiseReaction Record { [[Capability]]: resultCapability, [[Type]]: fulfill, [[Handler]]: onFulfilledJobCallback }.
            PromiseReaction::create(
                vm,
                PromiseReactionType::Fulfill,
                result_capability,
                on_fulfilled_job_callback,
            )
        };

        let create_reject_reaction = || {
            // 5. If IsCallable(onRejected) is false, then
            //     a. Let onRejectedJobCallback be empty.
            // 6. Else,
            //     a. Let onRejectedJobCallback be HostMakeJobCallback(onRejected).
            let on_rejected_job_callback = on_rejected
                .is_function()
                .then(|| vm.host_make_job_callback()(vm, on_rejected.as_function()));

            // 8. Let rejectReaction be the PromiseReaction Record { [[Capability]]: resultCapability, [[Type]]: reject, [[Handler]]: onRejectedJobCallback }.
            PromiseReaction::create(
                vm,
                PromiseReactionType::Reject,
                result_capability,
                on_rejected_job_callback,
            )
        };

        match self.state.get() {
            // 9. If promise.[[PromiseState]] is pending, then
            PromiseState::Pending => {
                // a. Append fulfillReaction as the last element of the List that is promise.[[PromiseFulfillReactions]].
                let fulfill_reaction = create_fulfill_reaction();
                self.fulfill_reactions.borrow_mut(vm).push(fulfill_reaction);

                // b. Append rejectReaction as the last element of the List that is promise.[[PromiseRejectReactions]].
                let reject_reaction = create_reject_reaction();
                self.reject_reactions.borrow_mut(vm).push(reject_reaction);
            }
            // 10. Else if promise.[[PromiseState]] is fulfilled, then
            PromiseState::Fulfilled => {
                // a. Let value be promise.[[PromiseResult]].
                let value = self.result.get();

                // b. Let fulfillJob be NewPromiseReactionJob(fulfillReaction, value).
                let fulfill_reaction = create_fulfill_reaction();
                let fulfill_job = create_promise_reaction_job(vm, fulfill_reaction, value);

                // c. Perform HostEnqueuePromiseJob(fulfillJob.[[Job]], fulfillJob.[[Realm]]).
                vm.host_enqueue_promise_job()(vm, fulfill_job.job, fulfill_job.realm);
            }
            // 11. Else,
            PromiseState::Rejected => {
                // a. Assert: The value of promise.[[PromiseState]] is rejected.

                // b. Let reason be promise.[[PromiseResult]].
                let reason = self.result.get();

                // c. If promise.[[PromiseIsHandled]] is false, perform HostPromiseRejectionTracker(promise, "handle").
                if !self.is_handled.get() {
                    vm.host_promise_rejection_tracker()(vm, self.as_gc(), RejectionOperation::Handle);
                }

                // d. Let rejectJob be NewPromiseReactionJob(rejectReaction, reason).
                let reject_reaction = create_reject_reaction();
                let reject_job = create_promise_reaction_job(vm, reject_reaction, reason);

                // e. Perform HostEnqueuePromiseJob(rejectJob.[[Job]], rejectJob.[[Realm]]).
                vm.host_enqueue_promise_job()(vm, reject_job.job, reject_job.realm);
            }
        }

        // 12. Set promise.[[PromiseIsHandled]] to true.
        self.is_handled.set(true);

        // 13. If resultCapability is undefined, then
        let Some(result_capability) = result_capability else {
            // a. Return undefined.
            return Value::UNDEFINED;
        };

        // 14. Else,
        //     a. Return resultCapability.[[Promise]].
        Value::from_object(result_capability.promise())
    }
}
