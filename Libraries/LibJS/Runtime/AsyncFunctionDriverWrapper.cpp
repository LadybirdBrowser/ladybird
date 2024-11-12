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

JS_DEFINE_ALLOCATOR(AsyncFunctionDriverWrapper);

NonnullGCPtr<Promise> AsyncFunctionDriverWrapper::create(Realm& realm, GeneratorObject* generator_object)
{
    auto top_level_promise = Promise::create(realm);
    // Note: The top_level_promise is also kept alive by this Wrapper
    auto wrapper = realm.heap().allocate<AsyncFunctionDriverWrapper>(realm, realm, *generator_object, *top_level_promise);
    // Prime the generator:
    // This runs until the first `await value;`
    wrapper->continue_async_execution(realm.vm(), js_undefined(), true, IsInitialExecution::Yes);

    return top_level_promise;
}

AsyncFunctionDriverWrapper::AsyncFunctionDriverWrapper(Realm& realm, NonnullGCPtr<GeneratorObject> generator_object, NonnullGCPtr<Promise> top_level_promise)
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

    // 2. Let promise be ? PromiseResolve(%Promise%, value).
    auto* promise_object = TRY(promise_resolve(vm, realm.intrinsics().promise_constructor(), value));

    // 3. Let fulfilledClosure be a new Abstract Closure with parameters (v) that captures asyncContext and performs the
    //    following steps when called:
    auto fulfilled_closure = [this](VM& vm) -> ThrowCompletionOr<Value> {
        auto value = vm.argument(0);

        // a. Let prevContext be the running execution context.
        auto& prev_context = vm.running_execution_context();

        // FIXME: b. Suspend prevContext.

        // c. Push asyncContext onto the execution context stack; asyncContext is now the running execution context.
        TRY(vm.push_execution_context(*m_suspended_execution_context, {}));

        // d. Resume the suspended evaluation of asyncContext using NormalCompletion(v) as the result of the operation that
        //    suspended it.
        continue_async_execution(vm, value, true);

        // e. Assert: When we reach this step, asyncContext has already been removed from the execution context stack and
        //    prevContext is the currently running execution context.
        VERIFY(&vm.running_execution_context() == &prev_context);

        // f. Return undefined.
        return js_undefined();
    };

    // 4. Let onFulfilled be CreateBuiltinFunction(fulfilledClosure, 1, "", « »).
    auto on_fulfilled = NativeFunction::create(realm, move(fulfilled_closure), 1, "");

    // 5. Let rejectedClosure be a new Abstract Closure with parameters (reason) that captures asyncContext and performs the
    //    following steps when called:
    auto rejected_closure = [this](VM& vm) -> ThrowCompletionOr<Value> {
        auto reason = vm.argument(0);

        // a. Let prevContext be the running execution context.
        auto& prev_context = vm.running_execution_context();

        // FIXME: b. Suspend prevContext.

        // c. Push asyncContext onto the execution context stack; asyncContext is now the running execution context.
        TRY(vm.push_execution_context(*m_suspended_execution_context, {}));

        // d. Resume the suspended evaluation of asyncContext using ThrowCompletion(reason) as the result of the operation that
        //    suspended it.
        continue_async_execution(vm, reason, false);

        // e. Assert: When we reach this step, asyncContext has already been removed from the execution context stack and
        //    prevContext is the currently running execution context.
        VERIFY(&vm.running_execution_context() == &prev_context);

        // f. Return undefined.
        return js_undefined();
    };

    // 6. Let onRejected be CreateBuiltinFunction(rejectedClosure, 1, "", « »).
    auto on_rejected = NativeFunction::create(realm, move(rejected_closure), 1, "");

    // 7. Perform PerformPromiseThen(promise, onFulfilled, onRejected).
    m_current_promise = verify_cast<Promise>(promise_object);
    m_current_promise->perform_then(on_fulfilled, on_rejected, {});

    // 8. Remove asyncContext from the execution context stack and restore the execution context that is at the top of the
    //    execution context stack as the running execution context.
    // NOTE: This is done later on for us in continue_async_execution.

    // NOTE: None of these are necessary. 10-12 are handled by step d of the above lambdas.
    // 9. Let callerContext be the running execution context.
    // 10. Resume callerContext passing empty. If asyncContext is ever resumed again, let completion be the Completion Record with which it is resumed.
    // 11. Assert: If control reaches here, then asyncContext is the running execution context again.
    // 12. Return completion.
    return {};
}

void AsyncFunctionDriverWrapper::continue_async_execution(VM& vm, Value value, bool is_successful, IsInitialExecution is_initial_execution)
{
    auto generator_result = is_successful
        ? m_generator_object->resume(vm, value, {})
        : m_generator_object->resume_abrupt(vm, throw_completion(value), {});

    auto result = [&, this]() -> ThrowCompletionOr<void> {
        while (true) {
            if (generator_result.is_throw_completion())
                return generator_result.throw_completion();

            auto result = generator_result.release_value();
            VERIFY(result.is_object());

            auto promise_value = TRY(result.get(vm, vm.names.value));

            if (TRY(result.get(vm, vm.names.done)).to_boolean()) {
                // When returning a promise, we need to unwrap it.
                if (promise_value.is_object() && is<Promise>(promise_value.as_object())) {
                    auto& returned_promise = static_cast<Promise&>(promise_value.as_object());
                    if (returned_promise.state() == Promise::State::Fulfilled) {
                        m_top_level_promise->fulfill(returned_promise.result());
                        return {};
                    }
                    if (returned_promise.state() == Promise::State::Rejected)
                        return throw_completion(returned_promise.result());

                    // The promise is still pending but there's nothing more to do here.
                    return {};
                }

                // We hit a `return value;`
                m_top_level_promise->fulfill(promise_value);
                return {};
            }

            // We hit `await Promise`
            auto await_result = this->await(promise_value);
            if (await_result.is_throw_completion()) {
                generator_result = m_generator_object->resume_abrupt(vm, await_result.release_error(), {});
                continue;
            }
            return {};
        }
    }();

    if (result.is_throw_completion()) {
        m_top_level_promise->reject(result.throw_completion().value().value_or(js_undefined()));
    }

    // For the initial execution, the execution context will be popped for us later on by ECMAScriptFunctionObject.
    if (is_initial_execution == IsInitialExecution::No)
        vm.pop_execution_context();
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
}

}
