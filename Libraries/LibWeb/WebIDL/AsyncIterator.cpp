/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibWeb/WebIDL/AsyncIterator.h>

namespace Web::WebIDL {

GC_DEFINE_ALLOCATOR(AsyncIterator);

AsyncIterator::AsyncIterator(JS::Realm& realm, JS::Object::PropertyKind iteration_kind)
    : PlatformObject(realm)
    , m_kind(iteration_kind)
{
}

AsyncIterator::~AsyncIterator() = default;

void AsyncIterator::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_ongoing_promise);
}

// https://webidl.spec.whatwg.org/#ref-for-dfn-asynchronous-iterator-prototype-object%E2%91%A2
JS::ThrowCompletionOr<GC::Ref<JS::Object>> AsyncIterator::iterator_next_impl()
{
    auto& realm = this->realm();
    auto& vm = this->vm();

    // 8. Let nextSteps be the following steps:
    auto next_steps = [this](JS::VM& vm) {
        auto& realm = this->realm();

        // 1. Let nextPromiseCapability be ! NewPromiseCapability(%Promise%).
        auto next_promise_capability = WebIDL::create_promise(realm);

        // 2. If object’s is finished is true, then:
        if (m_is_finished) {
            // 1. Let result be CreateIteratorResultObject(undefined, true).
            auto result = JS::create_iterator_result_object(vm, JS::js_undefined(), true);

            // 2. Perform ! Call(nextPromiseCapability.[[Resolve]], undefined, « result »).
            MUST(JS::call(vm, *next_promise_capability->resolve(), JS::js_undefined(), result));

            // 3. Return nextPromiseCapability.[[Promise]].
            return next_promise_capability->promise();
        }

        // 3. Let kind be object’s kind.

        // 4. Let nextPromise be the result of getting the next iteration result with object’s target and object.
        auto& next_promise = as<JS::Promise>(*next_iteration_result(realm)->promise());

        // 5. Let fulfillSteps be the following steps, given next:
        auto fulfill_steps = [this](JS::VM& vm) {
            auto next = vm.argument(0);

            // 1. Set object’s ongoing promise to null.
            m_ongoing_promise = nullptr;

            // 2. If next is end of iteration, then:
            if (next.is_special_empty_value()) {
                // 1. Set object’s is finished to true.
                m_is_finished = true;

                // 2. Return CreateIteratorResultObject(undefined, true).
                return JS::create_iterator_result_object(vm, JS::js_undefined(), true);
            }
            // FIXME: 2. Otherwise, if interface has a pair asynchronously iterable declaration:
            else if (false) {
                // 1. Assert: next is a value pair.
                // 2. Return the iterator result for next and kind.
            }
            // Otherwise:
            else {
                // 1. Assert: interface has a value asynchronously iterable declaration.
                // 2. Assert: next is a value of the type that appears in the declaration.

                // 3. Let value be next, converted to a JavaScript value.
                // 4. Return CreateIteratorResultObject(value, false).
                return JS::create_iterator_result_object(vm, next, false);
            }
        };

        // 6. Let onFulfilled be CreateBuiltinFunction(fulfillSteps, « »).
        auto on_fulfilled = JS::NativeFunction::create(realm, move(fulfill_steps), 0);

        // 7. Let rejectSteps be the following steps, given reason:
        auto reject_steps = [this](JS::VM& vm) {
            auto reason = vm.argument(0);

            // 1. Set object’s ongoing promise to null.
            m_ongoing_promise = nullptr;

            // 2. Set object’s is finished to true.
            m_is_finished = true;

            // 3. Throw reason.
            return JS::throw_completion(reason);
        };

        // 8. Let onRejected be CreateBuiltinFunction(rejectSteps, « »).
        auto on_rejected = JS::NativeFunction::create(realm, move(reject_steps), 0);

        // 9. Perform PerformPromiseThen(nextPromise, onFulfilled, onRejected, nextPromiseCapability).
        next_promise.perform_then(on_fulfilled, on_rejected, next_promise_capability);

        // 10. Return nextPromiseCapability.[[Promise]].
        return next_promise_capability->promise();
    };

    // 9. Let ongoingPromise be object’s ongoing promise.
    // 10. If ongoingPromise is not null, then:
    if (m_ongoing_promise) {
        // 1. Let afterOngoingPromiseCapability be ! NewPromiseCapability(%Promise%).
        auto after_ongoing_promise_capability = WebIDL::create_promise(realm);

        // 2. Let onSettled be CreateBuiltinFunction(nextSteps, « »).
        auto on_settled = JS::NativeFunction::create(realm, move(next_steps), 0);

        // 3. Perform PerformPromiseThen(ongoingPromise, onSettled, onSettled, afterOngoingPromiseCapability).
        m_ongoing_promise->perform_then(on_settled, on_settled, after_ongoing_promise_capability);

        // 4. Set object’s ongoing promise to afterOngoingPromiseCapability.[[Promise]].
        m_ongoing_promise = as<JS::Promise>(*after_ongoing_promise_capability->promise());
    }
    // 11. Otherwise:
    else {
        // 1. Set object’s ongoing promise to the result of running nextSteps.
        m_ongoing_promise = as<JS::Promise>(*next_steps(vm));
    }

    // 12. Return object’s ongoing promise.
    return m_ongoing_promise.as_nonnull();
}

// https://webidl.spec.whatwg.org/#ref-for-asynchronous-iterator-return
JS::ThrowCompletionOr<GC::Ref<JS::Object>> AsyncIterator::iterator_return_impl(GC::Ref<WebIDL::Promise> return_promise_capability, JS::Value value)
{
    auto& realm = this->realm();
    auto& vm = this->vm();

    // 8. Let returnSteps be the following steps:
    auto return_steps = [this, value](JS::VM& vm) {
        auto& realm = this->realm();

        // 1. Let returnPromiseCapability be ! NewPromiseCapability(%Promise%).
        auto return_promise_capability = WebIDL::create_promise(realm);

        // 2. If object’s is finished is true, then:
        if (m_is_finished) {
            // 1. Let result be CreateIteratorResultObject(value, true).
            auto result = JS::create_iterator_result_object(vm, value, true);

            // 2. Perform ! Call(returnPromiseCapability.[[Resolve]], undefined, « result »).
            MUST(JS::call(vm, *return_promise_capability->resolve(), JS::js_undefined(), result));

            // 3. Return returnPromiseCapability.[[Promise]].
            return return_promise_capability->promise();
        }

        // 3. Set object’s is finished to true.
        m_is_finished = true;

        // 4. Return the result of running the asynchronous iterator return algorithm for interface, given object’s target, object, and value.
        return iterator_return(realm, value)->promise();
    };

    // 9. Let ongoingPromise be object’s ongoing promise.
    // 10. If ongoingPromise is not null, then:
    if (m_ongoing_promise) {
        // 1. Let afterOngoingPromiseCapability be ! NewPromiseCapability(%Promise%).
        auto after_ongoing_promise_capability = WebIDL::create_promise(realm);

        // 2. Let onSettled be CreateBuiltinFunction(returnSteps, « »).
        auto on_settled = JS::NativeFunction::create(realm, move(return_steps), 0);

        // 3. Perform PerformPromiseThen(ongoingPromise, onSettled, onSettled, afterOngoingPromiseCapability).
        m_ongoing_promise->perform_then(on_settled, on_settled, after_ongoing_promise_capability);

        // 4. Set object’s ongoing promise to afterOngoingPromiseCapability.[[Promise]].
        m_ongoing_promise = as<JS::Promise>(*after_ongoing_promise_capability->promise());
    }
    // 11. Otherwise:
    else {
        // 1. Set object’s ongoing promise to the result of running returnSteps.
        m_ongoing_promise = as<JS::Promise>(*return_steps(vm));
    }

    // 12. Let fulfillSteps be the following steps:
    auto fulfill_steps = [value](JS::VM& vm) {
        // 1. Return CreateIteratorResultObject(value, true).
        return JS::create_iterator_result_object(vm, value, true);
    };

    // 13. Let onFulfilled be CreateBuiltinFunction(fulfillSteps, « »).
    auto on_fulfilled = JS::NativeFunction::create(realm, move(fulfill_steps), 0);

    // 14. Perform PerformPromiseThen(object’s ongoing promise, onFulfilled, undefined, returnPromiseCapability).
    m_ongoing_promise->perform_then(on_fulfilled, JS::js_undefined(), return_promise_capability);

    // 15. Return returnPromiseCapability.[[Promise]].
    return return_promise_capability->promise();
}

GC::Ref<WebIDL::Promise> AsyncIterator::iterator_return(JS::Realm&, JS::Value)
{
    // If this is reached, a `return` data property was generated for your async iterator, but you neglected to override this method.
    VERIFY_NOT_REACHED();
}

}
