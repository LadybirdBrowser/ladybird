/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/JobCallback.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseJobs.h>
#include <LibJS/Runtime/PromiseReaction.h>
#include <LibJS/Runtime/PromiseResolvingFunction.h>

namespace JS {

JS_DEFINE_ALLOCATOR(Promise);

// 27.2.4.7.1 PromiseResolve ( C, x ), https://tc39.es/ecma262/#sec-promise-resolve
ThrowCompletionOr<Object*> promise_resolve(VM& vm, Object& constructor, Value value)
{
    // 1. If IsPromise(x) is true, then
    if (value.is_object() && is<Promise>(value.as_object())) {
        // a. Let xConstructor be ? Get(x, "constructor").
        auto value_constructor = TRY(value.as_object().get(vm.names.constructor));

        // b. If SameValue(xConstructor, C) is true, return x.
        if (same_value(value_constructor, &constructor))
            return &static_cast<Promise&>(value.as_object());
    }

    // 2. Let promiseCapability be ? NewPromiseCapability(C).
    auto promise_capability = TRY(new_promise_capability(vm, &constructor));

    // 3. Perform ? Call(promiseCapability.[[Resolve]], undefined, « x »).
    (void)TRY(call(vm, *promise_capability->resolve(), js_undefined(), value));

    // 4. Return promiseCapability.[[Promise]].
    return promise_capability->promise().ptr();
}

NonnullGCPtr<Promise> Promise::create(Realm& realm)
{
    return realm.heap().allocate<Promise>(realm, realm.intrinsics().promise_prototype());
}

// 27.2 Promise Objects, https://tc39.es/ecma262/#sec-promise-objects
Promise::Promise(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

// 27.2.1.3 CreateResolvingFunctions ( promise ), https://tc39.es/ecma262/#sec-createresolvingfunctions
Promise::ResolvingFunctions Promise::create_resolving_functions()
{
    dbgln_if(PROMISE_DEBUG, "[Promise @ {} / create_resolving_functions()]", this);

    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    // 1. Let alreadyResolved be the Record { [[Value]]: false }.
    auto already_resolved = vm.heap().allocate_without_realm<AlreadyResolved>();

    // 2. Let stepsResolve be the algorithm steps defined in Promise Resolve Functions.
    // 3. Let lengthResolve be the number of non-optional parameters of the function definition in Promise Resolve Functions.
    // 4. Let resolve be CreateBuiltinFunction(stepsResolve, lengthResolve, "", « [[Promise]], [[AlreadyResolved]] »).
    // 5. Set resolve.[[Promise]] to promise.
    // 6. Set resolve.[[AlreadyResolved]] to alreadyResolved.

    // 27.2.1.3.2 Promise Resolve Functions, https://tc39.es/ecma262/#sec-promise-resolve-functions
    auto resolve_function = PromiseResolvingFunction::create(realm, *this, *already_resolved, [](auto& vm, auto& promise, auto& already_resolved) {
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / PromiseResolvingFunction]: Resolve function was called", &promise);

        auto& realm = *vm.current_realm();

        auto resolution = vm.argument(0);

        // 1. Let F be the active function object.
        // 2. Assert: F has a [[Promise]] internal slot whose value is an Object.
        // 3. Let promise be F.[[Promise]].

        // 4. Let alreadyResolved be F.[[AlreadyResolved]].
        // 5. If alreadyResolved.[[Value]] is true, return undefined.
        if (already_resolved.value) {
            dbgln_if(PROMISE_DEBUG, "[Promise @ {} / PromiseResolvingFunction]: Promise is already resolved, returning undefined", &promise);
            return js_undefined();
        }

        // 6. Set alreadyResolved.[[Value]] to true.
        already_resolved.value = true;

        // 7. If SameValue(resolution, promise) is true, then
        if (resolution.is_object() && &resolution.as_object() == &promise) {
            dbgln_if(PROMISE_DEBUG, "[Promise @ {} / PromiseResolvingFunction]: Promise can't be resolved with itself, rejecting with error", &promise);

            // a. Let selfResolutionError be a newly created TypeError object.
            auto self_resolution_error = TypeError::create(realm, "Cannot resolve promise with itself"sv);

            // b. Perform RejectPromise(promise, selfResolutionError).
            promise.reject(self_resolution_error);

            // c. Return undefined.
            return js_undefined();
        }

        // 8. If Type(resolution) is not Object, then
        if (!resolution.is_object()) {
            // a. Perform FulfillPromise(promise, resolution).
            dbgln_if(PROMISE_DEBUG, "[Promise @ {} / PromiseResolvingFunction]: Resolution is not an object, fulfilling with {}", &promise, resolution);
            promise.fulfill(resolution);

            // b. Return undefined.
            return js_undefined();
        }

        // 9. Let then be Completion(Get(resolution, "then")).
        auto then = resolution.as_object().get(vm.names.then);

        // 10. If then is an abrupt completion, then
        if (then.is_throw_completion()) {
            // a. Perform RejectPromise(promise, then.[[Value]]).
            dbgln_if(PROMISE_DEBUG, "[Promise @ {} / PromiseResolvingFunction]: Exception while getting 'then' property, rejecting with error", &promise);
            promise.reject(*then.throw_completion().value());

            // b. Return undefined.
            return js_undefined();
        }

        // 11. Let thenAction be then.[[Value]].
        auto then_action = then.release_value();

        // 12. If IsCallable(thenAction) is false, then
        if (!then_action.is_function()) {
            // a. Perform FulfillPromise(promise, resolution).
            dbgln_if(PROMISE_DEBUG, "[Promise @ {} / PromiseResolvingFunction]: Then action is not a function, fulfilling with {}", &promise, resolution);
            promise.fulfill(resolution);

            // b. Return undefined.
            return js_undefined();
        }

        // 13. Let thenJobCallback be HostMakeJobCallback(thenAction).
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / PromiseResolvingFunction]: Creating JobCallback for then action @ {}", &promise, &then_action.as_function());
        auto then_job_callback = vm.host_make_job_callback(then_action.as_function());

        // 14. Let job be NewPromiseResolveThenableJob(promise, resolution, thenJobCallback).
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / PromiseResolvingFunction]: Creating PromiseJob for thenable {}", &promise, resolution);
        auto job = create_promise_resolve_thenable_job(vm, promise, resolution, move(then_job_callback));

        // 15. Perform HostEnqueuePromiseJob(job.[[Job]], job.[[Realm]]).
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / PromiseResolvingFunction]: Enqueuing job @ {} in realm {}", &promise, &job.job, job.realm.ptr());
        vm.host_enqueue_promise_job(move(job.job), job.realm);

        // 16. Return undefined.
        return js_undefined();
    });
    resolve_function->define_direct_property(vm.names.name, PrimitiveString::create(vm, String {}), Attribute::Configurable);

    // 7. Let stepsReject be the algorithm steps defined in Promise Reject Functions.
    // 8. Let lengthReject be the number of non-optional parameters of the function definition in Promise Reject Functions.
    // 9. Let reject be CreateBuiltinFunction(stepsReject, lengthReject, "", « [[Promise]], [[AlreadyResolved]] »).
    // 10. Set reject.[[Promise]] to promise.
    // 11. Set reject.[[AlreadyResolved]] to alreadyResolved.

    // 27.2.1.3.1 Promise Reject Functions, https://tc39.es/ecma262/#sec-promise-reject-functions
    auto reject_function = PromiseResolvingFunction::create(realm, *this, *already_resolved, [](auto& vm, auto& promise, auto& already_resolved) {
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / PromiseResolvingFunction]: Reject function was called", &promise);

        auto reason = vm.argument(0);

        // 1. Let F be the active function object.
        // 2. Assert: F has a [[Promise]] internal slot whose value is an Object.
        // 3. Let promise be F.[[Promise]].

        // 4. Let alreadyResolved be F.[[AlreadyResolved]].
        // 5. If alreadyResolved.[[Value]] is true, return undefined.
        if (already_resolved.value)
            return js_undefined();

        // 6. Set alreadyResolved.[[Value]] to true.
        already_resolved.value = true;

        // 7. Perform RejectPromise(promise, reason).
        promise.reject(reason);

        // 8. Return undefined.
        return js_undefined();
    });
    reject_function->define_direct_property(vm.names.name, PrimitiveString::create(vm, String {}), Attribute::Configurable);

    // 12. Return the Record { [[Resolve]]: resolve, [[Reject]]: reject }.
    return { *resolve_function, *reject_function };
}

// 27.2.1.4 FulfillPromise ( promise, value ), https://tc39.es/ecma262/#sec-fulfillpromise
void Promise::fulfill(Value value)
{
    dbgln_if(PROMISE_DEBUG, "[Promise @ {} / fulfill()]: Fulfilling promise with value {}", this, value);

    // 1. Assert: The value of promise.[[PromiseState]] is pending.
    VERIFY(m_state == State::Pending);
    VERIFY(!value.is_empty());

    // 2. Let reactions be promise.[[PromiseFulfillReactions]].
    // NOTE: This is a noop, we do these steps in a slightly different order.

    // 3. Set promise.[[PromiseResult]] to value.
    m_result = value;

    // 4. Set promise.[[PromiseFulfillReactions]] to undefined.
    // 5. Set promise.[[PromiseRejectReactions]] to undefined.

    // 6. Set promise.[[PromiseState]] to fulfilled.
    m_state = State::Fulfilled;

    // 7. Perform TriggerPromiseReactions(reactions, value).
    trigger_reactions();
    m_fulfill_reactions.clear();
    m_reject_reactions.clear();

    // 8. Return unused.
}

// 27.2.1.7 RejectPromise ( promise, reason ), https://tc39.es/ecma262/#sec-rejectpromise
void Promise::reject(Value reason)
{

    dbgln_if(PROMISE_DEBUG, "[Promise @ {} / reject()]: Rejecting promise with reason {}", this, reason);
    auto& vm = this->vm();

    // 1. Assert: The value of promise.[[PromiseState]] is pending.
    VERIFY(m_state == State::Pending);
    VERIFY(!reason.is_empty());

    // 2. Let reactions be promise.[[PromiseRejectReactions]].
    // NOTE: This is a noop, we do these steps in a slightly different order.

    // 3. Set promise.[[PromiseResult]] to reason.
    m_result = reason;

    // 4. Set promise.[[PromiseFulfillReactions]] to undefined.
    // 5. Set promise.[[PromiseRejectReactions]] to undefined.

    // 6. Set promise.[[PromiseState]] to rejected.
    m_state = State::Rejected;

    // 7. If promise.[[PromiseIsHandled]] is false, perform HostPromiseRejectionTracker(promise, "reject").
    if (!m_is_handled)
        vm.host_promise_rejection_tracker(*this, RejectionOperation::Reject);

    // 8. Perform TriggerPromiseReactions(reactions, reason).
    trigger_reactions();
    m_fulfill_reactions.clear();
    m_reject_reactions.clear();

    // 9. Return unused.
}

// 27.2.1.8 TriggerPromiseReactions ( reactions, argument ), https://tc39.es/ecma262/#sec-triggerpromisereactions
void Promise::trigger_reactions() const
{
    VERIFY(is_settled());
    auto& vm = this->vm();
    auto& reactions = m_state == State::Fulfilled
        ? m_fulfill_reactions
        : m_reject_reactions;

    // 1. For each element reaction of reactions, do
    for (auto& reaction : reactions) {
        // a. Let job be NewPromiseReactionJob(reaction, argument).
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / trigger_reactions()]: Creating PromiseJob for PromiseReaction @ {} with argument {}", this, &reaction, m_result);
        auto [job, realm] = create_promise_reaction_job(vm, *reaction, m_result);

        // b. Perform HostEnqueuePromiseJob(job.[[Job]], job.[[Realm]]).
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / trigger_reactions()]: Enqueuing job @ {} in realm {}", this, &job, realm.ptr());
        vm.host_enqueue_promise_job(move(job), realm);
    }

    if constexpr (PROMISE_DEBUG) {
        if (reactions.is_empty())
            dbgln("[Promise @ {} / trigger_reactions()]: No reactions!", this);
    }

    // 2. Return unused.
}

// 27.2.5.4.1 PerformPromiseThen ( promise, onFulfilled, onRejected [ , resultCapability ] ), https://tc39.es/ecma262/#sec-performpromisethen
Value Promise::perform_then(Value on_fulfilled, Value on_rejected, GCPtr<PromiseCapability> result_capability)
{
    auto& vm = this->vm();

    // 1. Assert: IsPromise(promise) is true.
    // 2. If resultCapability is not present, then
    //     a. Set resultCapability to undefined.

    // 3. If IsCallable(onFulfilled) is false, then
    //     a. Let onFulfilledJobCallback be empty.
    GCPtr<JobCallback> on_fulfilled_job_callback;

    // 4. Else,
    if (on_fulfilled.is_function()) {
        // a. Let onFulfilledJobCallback be HostMakeJobCallback(onFulfilled).
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / perform_then()]: Creating JobCallback for on_fulfilled function @ {}", this, &on_fulfilled.as_function());
        on_fulfilled_job_callback = vm.host_make_job_callback(on_fulfilled.as_function());
    }

    // 5. If IsCallable(onRejected) is false, then
    //     a. Let onRejectedJobCallback be empty.
    GCPtr<JobCallback> on_rejected_job_callback;

    // 6. Else,
    if (on_rejected.is_function()) {
        // a. Let onRejectedJobCallback be HostMakeJobCallback(onRejected).
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / perform_then()]: Creating JobCallback for on_rejected function @ {}", this, &on_rejected.as_function());
        on_rejected_job_callback = vm.host_make_job_callback(on_rejected.as_function());
    }

    // 7. Let fulfillReaction be the PromiseReaction { [[Capability]]: resultCapability, [[Type]]: Fulfill, [[Handler]]: onFulfilledJobCallback }.
    auto fulfill_reaction = PromiseReaction::create(vm, PromiseReaction::Type::Fulfill, result_capability, move(on_fulfilled_job_callback));

    // 8. Let rejectReaction be the PromiseReaction { [[Capability]]: resultCapability, [[Type]]: Reject, [[Handler]]: onRejectedJobCallback }.
    auto reject_reaction = PromiseReaction::create(vm, PromiseReaction::Type::Reject, result_capability, move(on_rejected_job_callback));

    switch (m_state) {
    // 9. If promise.[[PromiseState]] is pending, then
    case Promise::State::Pending:
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / perform_then()]: state is State::Pending, adding fulfill/reject reactions", this);

        // a. Append fulfillReaction as the last element of the List that is promise.[[PromiseFulfillReactions]].
        m_fulfill_reactions.append(fulfill_reaction);

        // b. Append rejectReaction as the last element of the List that is promise.[[PromiseRejectReactions]].
        m_reject_reactions.append(reject_reaction);
        break;
    // 10. Else if promise.[[PromiseState]] is fulfilled, then
    case Promise::State::Fulfilled: {
        // a. Let value be promise.[[PromiseResult]].
        auto value = m_result;

        // b. Let fulfillJob be NewPromiseReactionJob(fulfillReaction, value).
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / perform_then()]: State is State::Fulfilled, creating PromiseJob for PromiseReaction @ {} with argument {}", this, fulfill_reaction.ptr(), value);
        auto [fulfill_job, realm] = create_promise_reaction_job(vm, fulfill_reaction, value);

        // c. Perform HostEnqueuePromiseJob(fulfillJob.[[Job]], fulfillJob.[[Realm]]).
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / perform_then()]: Enqueuing job @ {} in realm {}", this, &fulfill_job, realm.ptr());
        vm.host_enqueue_promise_job(move(fulfill_job), realm);
        break;
    }
    // 11. Else,
    case Promise::State::Rejected: {
        // a. Assert: The value of promise.[[PromiseState]] is rejected.

        // b. Let reason be promise.[[PromiseResult]].
        auto reason = m_result;

        // c. If promise.[[PromiseIsHandled]] is false, perform HostPromiseRejectionTracker(promise, "handle").
        if (!m_is_handled)
            vm.host_promise_rejection_tracker(*this, RejectionOperation::Handle);

        // d. Let rejectJob be NewPromiseReactionJob(rejectReaction, reason).
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / perform_then()]: State is State::Rejected, creating PromiseJob for PromiseReaction @ {} with argument {}", this, reject_reaction.ptr(), reason);
        auto [reject_job, realm] = create_promise_reaction_job(vm, *reject_reaction, reason);

        // e. Perform HostEnqueuePromiseJob(rejectJob.[[Job]], rejectJob.[[Realm]]).
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / perform_then()]: Enqueuing job @ {} in realm {}", this, &reject_job, realm.ptr());
        vm.host_enqueue_promise_job(move(reject_job), realm);
        break;
    }
    default:
        VERIFY_NOT_REACHED();
    }

    // 12. Set promise.[[PromiseIsHandled]] to true.
    m_is_handled = true;

    // 13. If resultCapability is undefined, then
    if (result_capability == nullptr) {
        // a. Return undefined.
        dbgln_if(PROMISE_DEBUG, "[Promise @ {} / perform_then()]: No result PromiseCapability, returning undefined", this);
        return js_undefined();
    }

    // 14. Else,
    //     a. Return resultCapability.[[Promise]].
    dbgln_if(PROMISE_DEBUG, "[Promise @ {} / perform_then()]: Returning Promise @ {} from result PromiseCapability @ {}", this, result_capability->promise().ptr(), result_capability.ptr());
    return result_capability->promise();
}

void Promise::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_result);
    visitor.visit(m_fulfill_reactions);
    visitor.visit(m_reject_reactions);
}

}
