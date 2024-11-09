/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/JobCallback.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseJobs.h>
#include <LibJS/Runtime/PromiseReaction.h>

namespace JS {

// 27.2.2.1 NewPromiseReactionJob ( reaction, argument ), https://tc39.es/ecma262/#sec-newpromisereactionjob
static ThrowCompletionOr<Value> run_reaction_job(VM& vm, PromiseReaction& reaction, Value argument)
{
    // a. Let promiseCapability be reaction.[[Capability]].
    auto promise_capability = reaction.capability();

    // b. Let type be reaction.[[Type]].
    auto type = reaction.type();

    // c. Let handler be reaction.[[Handler]].
    auto handler = reaction.handler();

    Completion handler_result;

    // d. If handler is empty, then
    if (!handler) {
        dbgln_if(PROMISE_DEBUG, "run_reaction_job: Handler is empty");

        // i. If type is Fulfill, let handlerResult be NormalCompletion(argument).
        if (type == PromiseReaction::Type::Fulfill) {
            dbgln_if(PROMISE_DEBUG, "run_reaction_job: Reaction type is Type::Fulfill, setting handler result to {}", argument);
            handler_result = normal_completion(argument);
        }
        // ii. Else,
        else {
            // 1. Assert: type is Reject.
            VERIFY(type == PromiseReaction::Type::Reject);

            // 2. Let handlerResult be ThrowCompletion(argument).
            dbgln_if(PROMISE_DEBUG, "run_reaction_job: Reaction type is Type::Reject, throwing exception with argument {}", argument);
            handler_result = throw_completion(argument);
        }
    }
    // e. Else, let handlerResult be Completion(HostCallJobCallback(handler, undefined, « argument »)).
    else {
        dbgln_if(PROMISE_DEBUG, "run_reaction_job: Calling handler callback {} @ {} with argument {}", handler->callback().class_name(), &handler->callback(), argument);
        handler_result = vm.host_call_job_callback(*handler, js_undefined(), ReadonlySpan<Value> { &argument, 1 });
    }

    // f. If promiseCapability is undefined, then
    if (promise_capability == nullptr) {
        // i. Assert: handlerResult is not an abrupt completion.
        VERIFY(!handler_result.is_abrupt());

        // ii. Return empty.
        dbgln_if(PROMISE_DEBUG, "run_reaction_job: Reaction has no PromiseCapability, returning empty value");
        // TODO: This can't return an empty value at the moment, because the implicit conversion to Completion would fail.
        //       Change it back when this is using completions (`return normal_completion({})`)
        return js_undefined();
    }

    // g. Assert: promiseCapability is a PromiseCapability Record.

    // h. If handlerResult is an abrupt completion, then
    if (handler_result.is_abrupt()) {
        // i. Return ? Call(promiseCapability.[[Reject]], undefined, « handlerResult.[[Value]] »).
        auto reject_function = promise_capability->reject();
        dbgln_if(PROMISE_DEBUG, "run_reaction_job: Calling PromiseCapability's reject function @ {}", reject_function.ptr());
        return call(vm, *reject_function, js_undefined(), *handler_result.value());
    }
    // i. Else,
    else {
        // i. Return ? Call(promiseCapability.[[Resolve]], undefined, « handlerResult.[[Value]] »).
        auto resolve_function = promise_capability->resolve();
        dbgln_if(PROMISE_DEBUG, "[PromiseReactionJob]: Calling PromiseCapability's resolve function @ {}", resolve_function.ptr());
        return call(vm, *resolve_function, js_undefined(), *handler_result.value());
    }
}

// 27.2.2.1 NewPromiseReactionJob ( reaction, argument ), https://tc39.es/ecma262/#sec-newpromisereactionjob
PromiseJob create_promise_reaction_job(VM& vm, PromiseReaction& reaction, Value argument)
{
    // 1. Let job be a new Job Abstract Closure with no parameters that captures reaction and argument and performs the following steps when called:
    //    See run_reaction_job for "the following steps".
    auto job = create_heap_function(vm.heap(), [&vm, &reaction, argument] {
        return run_reaction_job(vm, reaction, argument);
    });

    // 2. Let handlerRealm be null.
    Realm* handler_realm { nullptr };

    // 3. If reaction.[[Handler]] is not empty, then
    auto handler = reaction.handler();
    if (handler) {
        // a. Let getHandlerRealmResult be Completion(GetFunctionRealm(reaction.[[Handler]].[[Callback]])).
        auto get_handler_realm_result = get_function_realm(vm, handler->callback());

        // b. If getHandlerRealmResult is a normal completion, set handlerRealm to getHandlerRealmResult.[[Value]].
        if (!get_handler_realm_result.is_throw_completion()) {
            handler_realm = get_handler_realm_result.release_value();
        } else {
            // c. Else, set handlerRealm to the current Realm Record.
            handler_realm = vm.current_realm();
        }

        // d. NOTE: handlerRealm is never null unless the handler is undefined. When the handler is a revoked Proxy and no ECMAScript code runs, handlerRealm is used to create error objects.
    }

    // 4. Return the Record { [[Job]]: job, [[Realm]]: handlerRealm }.
    return { job, handler_realm };
}

// 27.2.2.2 NewPromiseResolveThenableJob ( promiseToResolve, thenable, then ), https://tc39.es/ecma262/#sec-newpromiseresolvethenablejob
static ThrowCompletionOr<Value> run_resolve_thenable_job(VM& vm, Promise& promise_to_resolve, Value thenable, JobCallback& then)
{
    // a. Let resolvingFunctions be CreateResolvingFunctions(promiseToResolve).
    auto [resolve_function, reject_function] = promise_to_resolve.create_resolving_functions();

    // b. Let thenCallResult be Completion(HostCallJobCallback(then, thenable, « resolvingFunctions.[[Resolve]], resolvingFunctions.[[Reject]] »)).
    dbgln_if(PROMISE_DEBUG, "run_resolve_thenable_job: Calling then job callback for thenable {}", &thenable);
    AK::Array<Value, 2> arguments;
    arguments[0] = Value(resolve_function);
    arguments[1] = Value(reject_function);
    auto then_call_result = vm.host_call_job_callback(then, thenable, arguments.span());

    // c. If thenCallResult is an abrupt completion, then
    if (then_call_result.is_error()) {
        // i. Return ? Call(resolvingFunctions.[[Reject]], undefined, « thenCallResult.[[Value]] »).
        dbgln_if(PROMISE_DEBUG, "run_resolve_thenable_job: then_call_result is an abrupt completion, calling reject function with value {}", *then_call_result.throw_completion().value());
        return call(vm, *reject_function, js_undefined(), *then_call_result.throw_completion().value());
    }

    // d. Return ? thenCallResult.
    dbgln_if(PROMISE_DEBUG, "run_resolve_thenable_job: Returning then call result {}", then_call_result.value());
    return then_call_result;
}

// 27.2.2.2 NewPromiseResolveThenableJob ( promiseToResolve, thenable, then ), https://tc39.es/ecma262/#sec-newpromiseresolvethenablejob
PromiseJob create_promise_resolve_thenable_job(VM& vm, Promise& promise_to_resolve, Value thenable, JS::NonnullGCPtr<JobCallback> then)
{
    // 2. Let getThenRealmResult be Completion(GetFunctionRealm(then.[[Callback]])).
    auto get_then_realm_result = get_function_realm(vm, then->callback());

    Realm* then_realm;

    // 3. If getThenRealmResult is a normal completion, let thenRealm be getThenRealmResult.[[Value]].
    if (!get_then_realm_result.is_throw_completion()) {
        then_realm = get_then_realm_result.release_value();
    } else {
        // 4. Else, let thenRealm be the current Realm Record.
        then_realm = vm.current_realm();
    }

    // 5. NOTE: thenRealm is never null. When then.[[Callback]] is a revoked Proxy and no code runs, thenRealm is used to create error objects.
    VERIFY(then_realm);

    // 1. Let job be a new Job Abstract Closure with no parameters that captures promiseToResolve, thenable, and then and performs the following steps when called:
    //    See run_resolve_thenable_job() for "the following steps".
    auto job = create_heap_function(vm.heap(), [&vm, &promise_to_resolve, thenable, then]() mutable {
        return run_resolve_thenable_job(vm, promise_to_resolve, thenable, then);
    });

    // 6. Return the Record { [[Job]]: job, [[Realm]]: thenRealm }.
    return { job, then_realm };
}

}
