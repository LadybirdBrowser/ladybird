/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::gc::heap_function::{HeapFunction, create_heap_function};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{call_function_object, get_function_realm};
use crate::runtime::completion::{Completion, CompletionType, ThrowCompletionOr};
use crate::runtime::error::InternalError;
use crate::runtime::job_callback::JobCallback;
use crate::runtime::promise::Promise;
use crate::runtime::promise_reaction::{PromiseReaction, PromiseReactionType};
use crate::runtime::realm::Realm;

/// The Record { [[Job]], [[Realm]] } of the abstract operations that create promise jobs.
pub struct PromiseJob {
    pub job: Gc<HeapFunction>,
    pub realm: Option<Gc<Realm>>,
}

// 27.2.2.1 NewPromiseReactionJob ( reaction, argument ), https://tc39.es/ecma262/#sec-newpromisereactionjob
fn run_reaction_job(vm: &Vm, reaction: Gc<PromiseReaction>, argument: Value) -> ThrowCompletionOr<Value> {
    // a. Let promiseCapability be reaction.[[Capability]].
    let promise_capability = reaction.capability();

    // b. Let type be reaction.[[Type]].
    let reaction_type = reaction.reaction_type();

    // c. Let handler be reaction.[[Handler]].
    let handler = reaction.handler();

    let handler_result: Completion = match handler {
        // d. If handler is empty, then
        None => {
            // i. If type is Fulfill, let handlerResult be NormalCompletion(argument).
            if reaction_type == PromiseReactionType::Fulfill {
                Completion::normal(argument)
            }
            // ii. Else,
            else {
                // 1. Assert: type is Reject.
                assert!(reaction_type == PromiseReactionType::Reject);

                // 2. Let handlerResult be ThrowCompletion(argument).
                crate::embedding::completion::log_exception_if_enabled(vm, argument);
                Completion::new(CompletionType::Throw, argument)
            }
        }
        // e. Else, let handlerResult be Completion(HostCallJobCallback(handler, undefined, « argument »)).
        Some(handler) => vm.host_call_job_callback()(vm, handler, Value::UNDEFINED, &[argument]).into(),
    };

    // f. If promiseCapability is undefined, then
    let Some(promise_capability) = promise_capability else {
        // i. Assert: handlerResult is not an abrupt completion.
        // NB: Like MUST_OR_THROW_INTERNAL_ERROR, this lets the InternalError of a handler that ran out of stack through.
        if handler_result.is_error() {
            let value = handler_result.value();
            assert!(value.is_object() && value.as_object().is::<InternalError>());
            return Err(handler_result.release_error());
        }

        // ii. Return empty.
        return Ok(Value::UNDEFINED);
    };

    // g. Assert: promiseCapability is a PromiseCapability Record.

    // h. If handlerResult is an abrupt completion, then
    if handler_result.is_abrupt() {
        // i. Return ? Call(promiseCapability.[[Reject]], undefined, « handlerResult.[[Value]] »).
        call_function_object(
            vm,
            promise_capability.reject(),
            Value::UNDEFINED,
            &[handler_result.value()],
        )
    }
    // i. Else,
    else {
        // i. Return ? Call(promiseCapability.[[Resolve]], undefined, « handlerResult.[[Value]] »).
        call_function_object(
            vm,
            promise_capability.resolve(),
            Value::UNDEFINED,
            &[handler_result.value()],
        )
    }
}

// 27.2.2.1 NewPromiseReactionJob ( reaction, argument ), https://tc39.es/ecma262/#sec-newpromisereactionjob
pub fn create_promise_reaction_job(vm: &Vm, reaction: Gc<PromiseReaction>, argument: Value) -> PromiseJob {
    // 1. Let job be a new Job Abstract Closure with no parameters that captures reaction and argument and performs the following steps when called:
    //    See run_reaction_job for "the following steps".
    let job = create_heap_function(vm, (reaction, argument), |vm, &(reaction, argument)| {
        run_reaction_job(vm, reaction, argument)
    });

    // 2. Let handlerRealm be null.
    let mut handler_realm = None;

    // 3. If reaction.[[Handler]] is not empty, then
    if let Some(handler) = reaction.handler() {
        // a. Let getHandlerRealmResult be Completion(GetFunctionRealm(reaction.[[Handler]].[[Callback]])).
        let get_handler_realm_result = get_function_realm(vm, handler.callback());

        // b. If getHandlerRealmResult is a normal completion, set handlerRealm to getHandlerRealmResult.[[Value]].
        handler_realm = match get_handler_realm_result {
            Ok(realm) => Some(realm),
            // c. Else, set handlerRealm to the current Realm Record.
            Err(_) => vm.current_realm(),
        };

        // d. NOTE: handlerRealm is never null unless the handler is undefined. When the handler is a revoked Proxy and no ECMAScript code runs, handlerRealm is used to create error objects.
    }

    // 4. Return the Record { [[Job]]: job, [[Realm]]: handlerRealm }.
    PromiseJob {
        job,
        realm: handler_realm,
    }
}

// 27.2.2.2 NewPromiseResolveThenableJob ( promiseToResolve, thenable, then ), https://tc39.es/ecma262/#sec-newpromiseresolvethenablejob
fn run_resolve_thenable_job(
    vm: &Vm,
    promise_to_resolve: Gc<Promise>,
    thenable: Value,
    then: Gc<JobCallback>,
) -> ThrowCompletionOr<Value> {
    // a. Let resolvingFunctions be CreateResolvingFunctions(promiseToResolve).
    let resolving_functions = promise_to_resolve.create_resolving_functions(vm);

    // b. Let thenCallResult be Completion(HostCallJobCallback(then, thenable, « resolvingFunctions.[[Resolve]], resolvingFunctions.[[Reject]] »)).
    let arguments = [
        Value::from_object(resolving_functions.resolve),
        Value::from_object(resolving_functions.reject),
    ];
    let then_call_result = vm.host_call_job_callback()(vm, then, thenable, &arguments);

    // c. If thenCallResult is an abrupt completion, then
    if let Err(throw) = then_call_result {
        // i. Return ? Call(resolvingFunctions.[[Reject]], undefined, « thenCallResult.[[Value]] »).
        return call_function_object(vm, resolving_functions.reject, Value::UNDEFINED, &[throw.value()]);
    }

    // d. Return ? thenCallResult.
    then_call_result
}

// 27.2.2.2 NewPromiseResolveThenableJob ( promiseToResolve, thenable, then ), https://tc39.es/ecma262/#sec-newpromiseresolvethenablejob
pub fn create_promise_resolve_thenable_job(
    vm: &Vm,
    promise_to_resolve: Gc<Promise>,
    thenable: Value,
    then: Gc<JobCallback>,
) -> PromiseJob {
    // 2. Let getThenRealmResult be Completion(GetFunctionRealm(then.[[Callback]])).
    let get_then_realm_result = get_function_realm(vm, then.callback());

    let then_realm = match get_then_realm_result {
        // 3. If getThenRealmResult is a normal completion, let thenRealm be getThenRealmResult.[[Value]].
        Ok(realm) => realm,
        // 4. Else, let thenRealm be the current Realm Record.
        Err(_) => vm.current_realm().expect("there is a current realm"),
    };

    // 5. NOTE: thenRealm is never null. When then.[[Callback]] is a revoked Proxy and no code runs, thenRealm is used to create error objects.

    // 1. Let job be a new Job Abstract Closure with no parameters that captures promiseToResolve, thenable, and then and performs the following steps when called:
    //    See run_resolve_thenable_job() for "the following steps".
    let job = create_heap_function(
        vm,
        (promise_to_resolve, thenable, then),
        |vm, &(promise_to_resolve, thenable, then)| run_resolve_thenable_job(vm, promise_to_resolve, thenable, then),
    );

    // 6. Return the Record { [[Job]]: job, [[Realm]]: thenRealm }.
    PromiseJob {
        job,
        realm: Some(then_realm),
    }
}
