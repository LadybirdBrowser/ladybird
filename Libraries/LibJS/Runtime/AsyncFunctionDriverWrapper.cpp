/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AsyncFunctionDriverWrapper.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

GC_DEFINE_ALLOCATOR(AsyncFunctionDriverWrapper);

GC::Ref<Promise> AsyncFunctionDriverWrapper::create(Realm& realm, GeneratorObject* generator_object)
{
    auto top_level_promise = Promise::create(realm);
    // Note: The top_level_promise is also kept alive by this Wrapper
    auto wrapper = realm.create<AsyncFunctionDriverWrapper>(realm, *generator_object, *top_level_promise);
    // Prime the generator:
    // This runs until the first `await value;`
    wrapper->continue_async_execution(realm.vm(), js_undefined(), true);

    return top_level_promise;
}

AsyncFunctionDriverWrapper::AsyncFunctionDriverWrapper(Realm& realm, GC::Ref<GeneratorObject> generator_object, GC::Ref<Promise> top_level_promise)
    : Promise(realm.intrinsics().promise_prototype())
    , m_generator_object(generator_object)
    , m_top_level_promise(top_level_promise)
{
}

// 27.7.5.3 Await ( value ), https://tc39.es/ecma262/#await
ThrowCompletionOr<void> AsyncFunctionDriverWrapper::await(JS::Value value)
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    // 1. Let asyncContext be the running execution context.
    if (!m_suspended_execution_context)
        m_suspended_execution_context = vm.running_execution_context().copy();

    // OPTIMIZATION: Fast path for non-thenable values.
    //
    // Per spec, PromiseResolve wraps non-Promise values in a new resolved promise,
    // then PerformPromiseThen attaches reaction handlers and schedules a microtask.
    // This creates 10+ GC objects per await.
    //
    // Since primitives can never have a "then" property, and already-settled native
    // Promises with the %Promise% constructor don't need wrapping, we can skip all
    // of that machinery and directly schedule the async function's continuation.
    //
    // For pending promises, or promises with a non-standard constructor, we fall
    // through to the spec-compliant slow path.
    if (!value.is_object()) {
        // Primitive values are never thenable -- schedule resume directly.
        schedule_resume(value, true);
        return {};
    }

    if (auto promise = value.as_if<Promise>()) {
        // Already-settled native Promises whose constructor is the intrinsic %Promise%.
        auto* promise_prototype = realm.intrinsics().promise_prototype().ptr();
        if (promise->state() != Promise::State::Pending
            && promise->shape().property_count() == 0
            && promise->shape().prototype() == promise_prototype
            && promise_prototype->get_without_side_effects(vm.names.constructor) == Value(realm.intrinsics().promise_constructor())) {
            schedule_resume(promise->result(), promise->state() == Promise::State::Fulfilled);
            promise->set_is_handled();
            return {};
        }
    }

    // 2. Let promise be ? PromiseResolve(%Promise%, value).
    auto* promise_object = TRY(promise_resolve(vm, realm.intrinsics().promise_constructor(), value));

    // 3. Let fulfilledClosure be a new Abstract Closure with parameters (v) that captures asyncContext and performs the
    //    following steps when called:
    // 5. Let rejectedClosure be a new Abstract Closure with parameters (reason) that captures asyncContext and performs the
    //    following steps when called:
    // OPTIMIZATION: onRejected and onFulfilled are identical other than the resumption value passed to continue_async_execution.
    //               To avoid allocated two GC functions down this path, we combine both callbacks into one function.
    auto settled_closure = [this](VM& vm) -> ThrowCompletionOr<Value> {
        auto reason = vm.argument(0);

        // The currently awaited promise is settled when this reaction runs, so we can use
        // its state to decide whether to resume with a normal or throw completion.
        VERIFY(m_current_promise);
        auto is_successful = m_current_promise->state() == Promise::State::Fulfilled;
        VERIFY(is_successful || m_current_promise->state() == Promise::State::Rejected);

        // a. Let prevContext be the running execution context.
        auto& prev_context = vm.running_execution_context();

        // b. Suspend prevContext.
        // c. Push asyncContext onto the execution context stack; asyncContext is now the running execution context.
        TRY(vm.push_execution_context(*m_suspended_execution_context, {}));

        // 3.d. Resume the suspended evaluation of asyncContext using NormalCompletion(v) as the result of the operation that
        //      suspended it.
        // 5.d. Resume the suspended evaluation of asyncContext using ThrowCompletion(reason) as the result of the operation that
        //      suspended it.
        continue_async_execution(vm, reason, is_successful);
        vm.pop_execution_context();

        // e. Assert: When we reach this step, asyncContext has already been removed from the execution context stack and
        //    prevContext is the currently running execution context.
        VERIFY(&vm.running_execution_context() == &prev_context);

        // f. Return undefined.
        return js_undefined();
    };

    // 4. Let onFulfilled be CreateBuiltinFunction(fulfilledClosure, 1, "", « »).
    // 6. Let onRejected be CreateBuiltinFunction(rejectedClosure, 1, "", « »).
    if (!m_on_settled)
        m_on_settled = NativeFunction::create(realm, move(settled_closure), 1);

    // 7. Perform PerformPromiseThen(promise, onFulfilled, onRejected).
    m_current_promise = as<Promise>(promise_object);
    m_current_promise->perform_then(m_on_settled, m_on_settled, {});

    // NOTE: None of these are necessary. 8-12 are handled by step d of the above lambdas.
    // 8. Remove asyncContext from the execution context stack and restore the execution context that is at the top of the
    //    execution context stack as the running execution context.
    // 9. Let callerContext be the running execution context.
    // 10. Resume callerContext passing empty. If asyncContext is ever resumed again, let completion be the Completion Record with which it is resumed.
    // 11. Assert: If control reaches here, then asyncContext is the running execution context again.
    // 12. Return completion.
    return {};
}

void AsyncFunctionDriverWrapper::schedule_resume(Value value, bool is_fulfilled)
{
    auto& vm = this->vm();
    vm.host_enqueue_promise_job(
        GC::create_function(vm.heap(), [this, value, is_fulfilled, &vm]() -> ThrowCompletionOr<Value> {
            TRY(vm.push_execution_context(*m_suspended_execution_context, {}));
            continue_async_execution(vm, value, is_fulfilled);
            vm.pop_execution_context();
            return js_undefined();
        }),
        vm.current_realm());
}

void AsyncFunctionDriverWrapper::continue_async_execution(VM& vm, Value value, bool is_successful)
{
    auto generator_result = is_successful
        ? m_generator_object->resume(vm, value, {})
        : m_generator_object->resume_abrupt(vm, throw_completion(value), {});

    auto result = [&, this]() -> ThrowCompletionOr<void> {
        while (true) {
            if (generator_result.is_throw_completion())
                return generator_result.throw_completion();

            auto result = generator_result.release_value();
            auto promise_value = result.value;
            if (result.done) {
                // When returning a promise, we need to unwrap it.
                if (auto returned_promise = promise_value.as_if<Promise>()) {
                    if (returned_promise->state() == Promise::State::Fulfilled) {
                        m_top_level_promise->fulfill(returned_promise->result());
                        return {};
                    }
                    if (returned_promise->state() == Promise::State::Rejected)
                        return throw_completion(returned_promise->result());

                    // The promise is still pending but there's nothing more to do here.
                    return {};
                }

                // We hit a `return value;`
                m_top_level_promise->fulfill(promise_value);
                return {};
            }

            // We hit `await value`
            //
            // OPTIMIZATION: Synchronous await fast path.
            // If we're not in the initial execution (i.e. we were resumed from a microtask)
            // and the microtask queue is empty, we can resolve the await synchronously
            // without suspending. This is safe because no other microtask can observe the
            // difference in execution order.
            if (!m_is_initial_execution && vm.host_promise_job_queue_is_empty()) {
                auto& realm = *vm.current_realm();
                if (!promise_value.is_object()) {
                    // Primitive values are never thenable.
                    generator_result = m_generator_object->resume(vm, promise_value, {});
                    continue;
                }
                if (auto promise = promise_value.as_if<Promise>()) {
                    auto* promise_prototype = realm.intrinsics().promise_prototype().ptr();
                    if (promise->state() != Promise::State::Pending
                        && promise->shape().property_count() == 0
                        && promise->shape().prototype() == promise_prototype
                        && promise_prototype->get_without_side_effects(vm.names.constructor) == Value(realm.intrinsics().promise_constructor())) {
                        auto is_fulfilled = promise->state() == Promise::State::Fulfilled;
                        promise->set_is_handled();
                        if (is_fulfilled) {
                            generator_result = m_generator_object->resume(vm, promise->result(), {});
                        } else {
                            generator_result = m_generator_object->resume_abrupt(vm, throw_completion(promise->result()), {});
                        }
                        continue;
                    }
                }
            }

            auto await_result = this->await(promise_value);
            if (await_result.is_throw_completion()) {
                generator_result = m_generator_object->resume_abrupt(vm, await_result.release_error(), {});
                continue;
            }
            m_is_initial_execution = false;
            return {};
        }
    }();

    if (result.is_throw_completion()) {
        m_top_level_promise->reject(result.throw_completion().value());
    }
}

void AsyncFunctionDriverWrapper::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_generator_object);
    visitor.visit(m_top_level_promise);
    if (m_current_promise)
        visitor.visit(m_current_promise);
    if (m_suspended_execution_context)
        m_suspended_execution_context->visit_edges(visitor);
    visitor.visit(m_on_settled);
}

}
