/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

bool g_log_all_js_exceptions = false;

Completion::Completion(ThrowCompletionOr<Value> const& throw_completion_or_value)
{
    if (throw_completion_or_value.is_throw_completion()) {
        m_type = Type::Throw;
        m_value = throw_completion_or_value.throw_completion().value();
    } else {
        m_type = Type::Normal;
        m_value = throw_completion_or_value.value();
    }
}

// 6.2.3.1 Await, https://tc39.es/ecma262/#await
// FIXME: This no longer matches the spec!
ThrowCompletionOr<Value> await(VM& vm, Value value)
{
    auto& realm = *vm.current_realm();

    // 1. Let asyncContext be the running execution context.
    // NOTE: This is not needed, as we don't suspend anything.

    // 2. Let promise be ? PromiseResolve(%Promise%, value).
    auto* promise_object = TRY(promise_resolve(vm, realm.intrinsics().promise_constructor(), value));

    IGNORE_USE_IN_ESCAPING_LAMBDA Optional<bool> success;
    IGNORE_USE_IN_ESCAPING_LAMBDA Value result;

    // 3. Let fulfilledClosure be a new Abstract Closure with parameters (value) that captures asyncContext and performs the following steps when called:
    auto fulfilled_closure = [&success, &result](VM& vm) -> ThrowCompletionOr<Value> {
        // a. Let prevContext be the running execution context.
        // b. Suspend prevContext.
        // FIXME: We don't have this concept yet.

        // NOTE: Since we don't support context suspension, we exfiltrate the result to await()'s scope instead
        success = true;
        result = vm.argument(0);

        // c. Push asyncContext onto the execution context stack; asyncContext is now the running execution context.
        // NOTE: This is not done, because we're not suspending anything (see above).

        // d. Resume the suspended evaluation of asyncContext using NormalCompletion(value) as the result of the operation that suspended it.
        // e. Assert: When we reach this step, asyncContext has already been removed from the execution context stack and prevContext is the currently running execution context.
        // FIXME: We don't have this concept yet.

        // f. Return undefined.
        return js_undefined();
    };

    // 4. Let onFulfilled be CreateBuiltinFunction(fulfilledClosure, 1, "", « »).
    auto on_fulfilled = NativeFunction::create(realm, move(fulfilled_closure), 1);

    // 5. Let rejectedClosure be a new Abstract Closure with parameters (reason) that captures asyncContext and performs the following steps when called:
    auto rejected_closure = [&success, &result](VM& vm) -> ThrowCompletionOr<Value> {
        // a. Let prevContext be the running execution context.
        // b. Suspend prevContext.
        // FIXME: We don't have this concept yet.

        // NOTE: Since we don't support context suspension, we exfiltrate the result to await()'s scope instead
        success = false;
        result = vm.argument(0);

        // c. Push asyncContext onto the execution context stack; asyncContext is now the running execution context.
        // NOTE: This is not done, because we're not suspending anything (see above).

        // d. Resume the suspended evaluation of asyncContext using ThrowCompletion(reason) as the result of the operation that suspended it.
        // e. Assert: When we reach this step, asyncContext has already been removed from the execution context stack and prevContext is the currently running execution context.
        // FIXME: We don't have this concept yet.

        // f. Return undefined.
        return js_undefined();
    };

    // 6. Let onRejected be CreateBuiltinFunction(rejectedClosure, 1, "", « »).
    auto on_rejected = NativeFunction::create(realm, move(rejected_closure), 1);

    // 7. Perform PerformPromiseThen(promise, onFulfilled, onRejected).
    auto promise = as<Promise>(promise_object);
    promise->perform_then(on_fulfilled, on_rejected, {});

    // FIXME: Since we don't support context suspension, we attempt to "wait" for the promise to resolve
    //        by syncronously running all queued promise jobs.
    if (auto* custom_data = vm.custom_data()) {
        // Embedder case (i.e. LibWeb). Runs all promise jobs by performing a microtask checkpoint.
        custom_data->spin_event_loop_until(GC::create_function(vm.heap(), [success] {
            return success.has_value();
        }));
    } else {
        // No embbedder, standalone LibJS implementation
        vm.run_queued_promise_jobs();
    }

    // 8. Remove asyncContext from the execution context stack and restore the execution context that is at the top of the execution context stack as the running execution context.
    // NOTE: Since we don't push any EC, this step is not performed.

    // 9. Set the code evaluation state of asyncContext such that when evaluation is resumed with a Completion Record completion, the following steps of the algorithm that invoked Await will be performed, with completion available.
    // 10. Return NormalCompletion(unused).
    // 11. NOTE: This returns to the evaluation of the operation that had most previously resumed evaluation of asyncContext.

    // Make sure that the promise _actually_ resolved.
    // Note that this is checked down the chain (result.is_empty()) anyway, but let's make the source of the issue more clear.
    VERIFY(success.has_value());

    if (success.value())
        return result;
    return throw_completion(result);
}

static void log_exception(Value value)
{
    if (!value.is_object()) {
        dbgln("\033[31;1mTHROW!\033[0m {}", value);
        return;
    }

    auto& object = value.as_object();
    auto& vm = object.vm();
    dbgln("\033[31;1mTHROW!\033[0m {}", object.get(vm.names.message).value());
    vm.dump_backtrace();
}

// 6.2.4.2 ThrowCompletion ( value ), https://tc39.es/ecma262/#sec-throwcompletion
Completion throw_completion(Value value)
{
    if (g_log_all_js_exceptions)
        log_exception(value);

    // 1. Return Completion Record { [[Type]]: throw, [[Value]]: value, [[Target]]: empty }.
    return { Completion::Type::Throw, value };
}

}
