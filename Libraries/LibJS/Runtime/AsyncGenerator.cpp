/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AsyncGenerator.h>
#include <LibJS/Runtime/AsyncGeneratorPrototype.h>
#include <LibJS/Runtime/AsyncGeneratorRequest.h>
#include <LibJS/Runtime/CompletionCell.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/GeneratorResult.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/PromiseConstructor.h>

namespace JS {

GC_DEFINE_ALLOCATOR(AsyncGenerator);

ThrowCompletionOr<GC::Ref<AsyncGenerator>> AsyncGenerator::create(Realm& realm, Value initial_value, ECMAScriptFunctionObject* generating_function, NonnullOwnPtr<ExecutionContext> execution_context)
{
    auto& vm = realm.vm();
    // This is "g1.prototype" in figure-2 (https://tc39.es/ecma262/img/figure-2.png)
    static Bytecode::PropertyLookupCache cache;
    auto generating_function_prototype = TRY(generating_function->get(vm.names.prototype, cache));
    auto generating_function_prototype_object = TRY(generating_function_prototype.to_object(vm));
    auto object = realm.create<AsyncGenerator>(realm, generating_function_prototype_object, move(execution_context));
    object->m_generating_function = generating_function;
    object->m_previous_value = initial_value;
    return object;
}

AsyncGenerator::AsyncGenerator(Realm&, Object& prototype, NonnullOwnPtr<ExecutionContext> context)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_async_generator_context(move(context))
{
}

AsyncGenerator::~AsyncGenerator() = default;

void AsyncGenerator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& request : m_async_generator_queue) {
        visitor.visit(request.completion.value());
        visitor.visit(request.capability);
    }
    visitor.visit(m_generating_function);
    visitor.visit(m_previous_value);
    visitor.visit(m_current_promise);
    m_async_generator_context->visit_edges(visitor);
}

// 27.6.3.4 AsyncGeneratorEnqueue ( generator, completion, promiseCapability ), https://tc39.es/ecma262/#sec-asyncgeneratorenqueue
void AsyncGenerator::async_generator_enqueue(Completion completion, GC::Ref<PromiseCapability> promise_capability)
{
    // 1. Let request be AsyncGeneratorRequest { [[Completion]]: completion, [[Capability]]: promiseCapability }.
    auto request = AsyncGeneratorRequest { .completion = move(completion), .capability = promise_capability };

    // 2. Append request to generator.[[AsyncGeneratorQueue]].
    m_async_generator_queue.append(move(request));

    // 3. Return unused.
}

void AsyncGenerator::set_async_generator_state(Badge<AsyncGeneratorPrototype>, AsyncGenerator::State value)
{
    m_async_generator_state = value;
}

// 27.7.5.3 Await ( value ), https://tc39.es/ecma262/#await
ThrowCompletionOr<void> AsyncGenerator::await(Value value)
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    // 1. Let asyncContext be the running execution context.
    auto& async_context = vm.running_execution_context();

    // 2. Let promise be ? PromiseResolve(%Promise%, value).
    auto* promise_object = TRY(promise_resolve(vm, realm.intrinsics().promise_constructor(), value));

    // 3. Let fulfilledClosure be a new Abstract Closure with parameters (v) that captures asyncContext and performs the
    //    following steps when called:
    auto fulfilled_closure = [this, &async_context](VM& vm) -> ThrowCompletionOr<Value> {
        auto value = vm.argument(0);

        // a. Let prevContext be the running execution context.
        auto& prev_context = vm.running_execution_context();

        // b. Suspend prevContext.
        // c. Push asyncContext onto the execution context stack; asyncContext is now the running execution context.
        TRY(vm.push_execution_context(async_context, {}));

        // d. Resume the suspended evaluation of asyncContext using NormalCompletion(v) as the result of the operation that
        //    suspended it.
        execute(vm, normal_completion(value));

        // e. Assert: When we reach this step, asyncContext has already been removed from the execution context stack and
        //    prevContext is the currently running execution context.
        VERIFY(&vm.running_execution_context() == &prev_context);

        // f. Return undefined.
        return js_undefined();
    };

    // 4. Let onFulfilled be CreateBuiltinFunction(fulfilledClosure, 1, "", « »).
    auto on_fulfilled = NativeFunction::create(realm, move(fulfilled_closure), 1);

    // 5. Let rejectedClosure be a new Abstract Closure with parameters (reason) that captures asyncContext and performs the
    //    following steps when called:
    auto rejected_closure = [this, &async_context](VM& vm) -> ThrowCompletionOr<Value> {
        auto reason = vm.argument(0);

        // a. Let prevContext be the running execution context.
        auto& prev_context = vm.running_execution_context();

        // b. Suspend prevContext.
        // c. Push asyncContext onto the execution context stack; asyncContext is now the running execution context.
        TRY(vm.push_execution_context(async_context, {}));

        // d. Resume the suspended evaluation of asyncContext using ThrowCompletion(reason) as the result of the operation that
        //    suspended it.
        execute(vm, throw_completion(reason));

        // e. Assert: When we reach this step, asyncContext has already been removed from the execution context stack and
        //    prevContext is the currently running execution context.
        VERIFY(&vm.running_execution_context() == &prev_context);

        // f. Return undefined.
        return js_undefined();
    };

    // 6. Let onRejected be CreateBuiltinFunction(rejectedClosure, 1, "", « »).
    auto on_rejected = NativeFunction::create(realm, move(rejected_closure), 1);

    // 7. Perform PerformPromiseThen(promise, onFulfilled, onRejected).
    m_current_promise = as<Promise>(promise_object);
    m_current_promise->perform_then(on_fulfilled, on_rejected, {});

    // 8. Remove asyncContext from the execution context stack and restore the execution context that is at the top of the
    //    execution context stack as the running execution context.
    vm.pop_execution_context();

    // NOTE: None of these are necessary. 10-12 are handled by step d of the above lambdas.
    // 9. Let callerContext be the running execution context.
    // 10. Resume callerContext passing empty. If asyncContext is ever resumed again, let completion be the Completion Record with which it is resumed.
    // 11. Assert: If control reaches here, then asyncContext is the running execution context again.
    // 12. Return completion.
    return {};
}

void AsyncGenerator::execute(VM& vm, Completion completion)
{
    while (true) {
        // Loosely based on step 4 of https://tc39.es/ecma262/#sec-asyncgeneratorstart
        auto generated_value = [](Value value) -> Value {
            if (value.is_cell())
                return static_cast<GeneratorResult const&>(value.as_cell()).result();
            return value.is_special_empty_value() ? js_undefined() : value;
        };

        auto generated_continuation = [&](Value value) -> Optional<size_t> {
            if (value.is_cell()) {
                auto number_value = static_cast<GeneratorResult const&>(value.as_cell()).continuation();
                if (number_value.is_null())
                    return {};
                return static_cast<size_t>(number_value.as_double());
            }
            return {};
        };

        auto generated_is_await = [](Value value) -> bool {
            if (value.is_cell())
                return static_cast<GeneratorResult const&>(value.as_cell()).is_await();
            return false;
        };

        auto completion_cell = heap().allocate<CompletionCell>(completion);

        auto& bytecode_interpreter = vm.bytecode_interpreter();

        auto const continuation_address = generated_continuation(m_previous_value);

        // We should never enter `execute` again after the generator is complete.
        VERIFY(continuation_address.has_value());

        auto next_result = bytecode_interpreter.run_executable(vm.running_execution_context(), *m_generating_function->bytecode_executable(), continuation_address, completion_cell);

        auto result_value = move(next_result.value);
        if (!result_value.is_throw_completion()) {
            m_previous_value = result_value.release_value();
            auto value = generated_value(m_previous_value);
            bool is_await = generated_is_await(m_previous_value);

            if (is_await) {
                auto await_result = this->await(value);
                if (await_result.is_throw_completion()) {
                    completion = await_result.release_error();
                    continue;
                }
                return;
            }
        }

        bool done = result_value.is_throw_completion() || !generated_continuation(m_previous_value).has_value();
        if (!done) {
            // 27.6.3.8 AsyncGeneratorYield ( value ), https://tc39.es/ecma262/#sec-asyncgeneratoryield
            // 1. Let genContext be the running execution context.
            // 2. Assert: genContext is the execution context of a generator.
            // 3. Let generator be the value of the Generator component of genContext.
            // 4. Assert: GetGeneratorKind() is async.
            // NOTE: genContext is `m_async_generator_context`, generator is `this`.

            // 5. Let completion be NormalCompletion(value).
            auto value = generated_value(m_previous_value);
            auto yield_completion = normal_completion(value);

            // 6. Assert: The execution context stack has at least two elements.
            VERIFY(vm.execution_context_stack().size() >= 2);

            // 7. Let previousContext be the second to top element of the execution context stack.
            auto& previous_context = vm.execution_context_stack().at(vm.execution_context_stack().size() - 2);

            // 8. Let previousRealm be previousContext's Realm.
            auto previous_realm = previous_context->realm;

            // 9. Perform AsyncGeneratorCompleteStep(generator, completion, false, previousRealm).
            complete_step(yield_completion, false, previous_realm.ptr());

            // 10. Let queue be generator.[[AsyncGeneratorQueue]].
            auto& queue = m_async_generator_queue;

            // 11. If queue is not empty, then
            if (!queue.is_empty()) {
                // a. NOTE: Execution continues without suspending the generator.
                // b. Let toYield be the first element of queue.
                auto& to_yield = queue.first();

                // c. Let resumptionValue be Completion(toYield.[[Completion]]).
                completion = Completion(to_yield.completion);

                // d. Return ? AsyncGeneratorUnwrapYieldResumption(resumptionValue).
                // NOTE: AsyncGeneratorUnwrapYieldResumption is performed inside the continuation block inside the generator,
                //       so we just need to enter the generator again.
                continue;
            }
            // 12. Else,
            else {
                // a. Set generator.[[AsyncGeneratorState]] to suspendedYield.
                m_async_generator_state = State::SuspendedYield;

                // b. Remove genContext from the execution context stack and restore the execution context that is at the top of the
                //    execution context stack as the running execution context.
                vm.pop_execution_context();

                // c. Let callerContext be the running execution context.
                // d. Resume callerContext passing undefined. If genContext is ever resumed again, let resumptionValue be the Completion Record with which it is resumed.
                // e. Assert: If control reaches here, then genContext is the running execution context again.
                // f. Return ? AsyncGeneratorUnwrapYieldResumption(resumptionValue).
                // NOTE: e-f are performed whenever someone calls `execute` again.
                return;
            }
        }

        // 27.6.3.2 AsyncGeneratorStart ( generator, generatorBody ), https://tc39.es/ecma262/#sec-asyncgeneratorstart
        // 4.e. Assert: If we return here, the async generator either threw an exception or performed either an implicit or explicit return.
        // 4.f. Remove acGenContext from the execution context stack and restore the execution context that is at the top of the execution context stack as the running execution context.
        vm.pop_execution_context();

        // 4.g. Set acGenerator.[[AsyncGeneratorState]] to completed.
        m_async_generator_state = State::Completed;

        // 4.h. If result.[[Type]] is normal, set result to NormalCompletion(undefined).
        // 4.i. If result.[[Type]] is return, set result to NormalCompletion(result.[[Value]]).
        Completion result;
        if (!result_value.is_throw_completion()) {
            result = normal_completion(generated_value(m_previous_value));
        } else {
            result = result_value.release_error();
        }

        // 4.j. Perform AsyncGeneratorCompleteStep(acGenerator, result, true).
        complete_step(result, true);

        // 4.k. Perform AsyncGeneratorDrainQueue(acGenerator).
        drain_queue();

        // 4.l. Return undefined.
        return;
    }
}

// 27.6.3.6 AsyncGeneratorResume ( generator, completion ), https://tc39.es/ecma262/#sec-asyncgeneratorresume
ThrowCompletionOr<void> AsyncGenerator::resume(VM& vm, Completion completion)
{
    // 1. Assert: generator.[[AsyncGeneratorState]] is either suspendedStart or suspendedYield.
    VERIFY(m_async_generator_state == State::SuspendedStart || m_async_generator_state == State::SuspendedYield);

    // 2. Let genContext be generator.[[AsyncGeneratorContext]].
    auto& generator_context = m_async_generator_context;

    // 3. Let callerContext be the running execution context.
    auto const& caller_context = vm.running_execution_context();

    // 4. Suspend callerContext.
    // 5. Set generator.[[AsyncGeneratorState]] to executing.
    m_async_generator_state = State::Executing;

    // 6. Push genContext onto the execution context stack; genContext is now the running execution context.
    TRY(vm.push_execution_context(*generator_context, {}));

    // 7. Resume the suspended evaluation of genContext using completion as the result of the operation that suspended
    //    it. Let result be the Completion Record returned by the resumed computation.
    // 8. Assert: result is never an abrupt completion.
    execute(vm, completion);

    // 9. Assert: When we return here, genContext has already been removed from the execution context stack and
    //    callerContext is the currently running execution context.
    VERIFY(&vm.running_execution_context() == &caller_context);

    // 10. Return unused.
    return {};
}

// 27.6.3.9 AsyncGeneratorAwaitReturn ( generator ), https://tc39.es/ecma262/#sec-asyncgeneratorawaitreturn
// With unmerged broken promise fixup from https://github.com/tc39/ecma262/pull/2683
void AsyncGenerator::await_return()
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    // 1. Let queue be generator.[[AsyncGeneratorQueue]].
    auto& queue = m_async_generator_queue;

    // 2. Assert: queue is not empty.
    VERIFY(!queue.is_empty());

    // 3. Let next be the first element of queue.
    auto& next = m_async_generator_queue.first();

    // 4. Let completion be Completion(next.[[Completion]]).
    auto completion = Completion(next.completion);

    // 5. Assert: completion.[[Type]] is return.
    VERIFY(completion.type() == Completion::Type::Return);

    // 6. Let promiseCompletion be Completion(PromiseResolve(%Promise%, _completion_.[[Value]])).
    auto promise_completion = promise_resolve(vm, realm.intrinsics().promise_constructor(), completion.value());

    // 7. If promiseCompletion is an abrupt completion, then
    if (promise_completion.is_throw_completion()) {
        // a. Set generator.[[AsyncGeneratorState]] to completed.
        m_async_generator_state = State::Completed;

        // b. Perform AsyncGeneratorCompleteStep(generator, promiseCompletion, true).
        complete_step(promise_completion.release_error(), true);

        // c. Perform AsyncGeneratorDrainQueue(generator).
        drain_queue();

        // d. Return unused.
        return;
    }

    // 8. Assert: promiseCompletion.[[Type]] is normal.
    VERIFY(!promise_completion.is_throw_completion());

    // 9. Let promise be promiseCompletion.[[Value]].
    auto* promise = promise_completion.release_value();

    // 10. Let fulfilledClosure be a new Abstract Closure with parameters (value) that captures generator and performs
    //    the following steps when called:
    auto fulfilled_closure = [this](VM& vm) -> ThrowCompletionOr<Value> {
        // a. Set generator.[[AsyncGeneratorState]] to completed.
        m_async_generator_state = State::Completed;

        // b. Let result be NormalCompletion(value).
        auto result = normal_completion(vm.argument(0));

        // c. Perform AsyncGeneratorCompleteStep(generator, result, true).
        complete_step(result, true);

        // d. Perform AsyncGeneratorDrainQueue(generator).
        drain_queue();

        // e. Return undefined.
        return js_undefined();
    };

    // 11. Let onFulfilled be CreateBuiltinFunction(fulfilledClosure, 1, "", « »).
    auto on_fulfilled = NativeFunction::create(realm, move(fulfilled_closure), 1);

    // 12. Let rejectedClosure be a new Abstract Closure with parameters (reason) that captures generator and performs
    //    the following steps when called:
    auto rejected_closure = [this](VM& vm) -> ThrowCompletionOr<Value> {
        // a. Set generator.[[AsyncGeneratorState]] to completed.
        m_async_generator_state = State::Completed;

        // b. Let result be ThrowCompletion(reason).
        auto result = throw_completion(vm.argument(0));

        // c. Perform AsyncGeneratorCompleteStep(generator, result, true).
        complete_step(result, true);

        // d. Perform AsyncGeneratorDrainQueue(generator).
        drain_queue();

        // e. Return undefined.
        return js_undefined();
    };

    // 13. Let onRejected be CreateBuiltinFunction(rejectedClosure, 1, "", « »).
    auto on_rejected = NativeFunction::create(realm, move(rejected_closure), 1);

    // 14. Perform PerformPromiseThen(promise, onFulfilled, onRejected).
    // NOTE: await_return should only be called when the generator is in SuspendedStart or Completed state,
    //       so an await shouldn't be running currently, so it should be safe to overwrite m_current_promise.
    m_current_promise = as<Promise>(promise);
    m_current_promise->perform_then(on_fulfilled, on_rejected, {});

    // 15. Return unused.
    return;
}

// 27.6.3.5 AsyncGeneratorCompleteStep ( generator, completion, done [ , realm ] ), https://tc39.es/ecma262/#sec-asyncgeneratorcompletestep
void AsyncGenerator::complete_step(Completion completion, bool done, Realm* realm)
{
    auto& vm = this->vm();

    // 1. Assert: generator.[[AsyncGeneratorQueue]] is not empty.
    VERIFY(!m_async_generator_queue.is_empty());

    // 2. Let next be the first element of generator.[[AsyncGeneratorQueue]].
    // 3. Remove the first element from generator.[[AsyncGeneratorQueue]].
    auto next = m_async_generator_queue.take_first();

    // 4. Let promiseCapability be next.[[Capability]].
    auto promise_capability = next.capability;

    // 5. Let value be completion.[[Value]].
    auto value = completion.value();

    // 6. If completion.[[Type]] is throw, then
    if (completion.type() == Completion::Type::Throw) {
        // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « value »).
        MUST(call(vm, *promise_capability->reject(), js_undefined(), value));
    }
    // 7. Else,
    else {
        // a. Assert: completion.[[Type]] is normal.
        VERIFY(completion.type() == Completion::Type::Normal);

        GC::Ptr<Object> iterator_result;

        // b. If realm is present, then
        if (realm) {
            // i. Let oldRealm be the running execution context's Realm.
            auto old_realm = vm.running_execution_context().realm;

            // ii. Set the running execution context's Realm to realm.
            vm.running_execution_context().realm = realm;

            // iii. Let iteratorResult be CreateIterResultObject(value, done).
            iterator_result = create_iterator_result_object(vm, value, done);

            // iv. Set the running execution context's Realm to oldRealm.
            vm.running_execution_context().realm = old_realm;
        }
        // c. Else,
        else {
            // i. Let iteratorResult be CreateIterResultObject(value, done).
            iterator_result = create_iterator_result_object(vm, value, done);
        }

        VERIFY(iterator_result);

        // d. Perform ! Call(promiseCapability.[[Resolve]], undefined, « iteratorResult »).
        MUST(call(vm, *promise_capability->resolve(), js_undefined(), iterator_result));
    }

    // 8. Return unused.
}

// 27.6.3.10 AsyncGeneratorDrainQueue ( generator ), https://tc39.es/ecma262/#sec-asyncgeneratordrainqueue
void AsyncGenerator::drain_queue()
{
    // 1. Assert: generator.[[AsyncGeneratorState]] is completed.
    VERIFY(m_async_generator_state == State::Completed);

    // 2. Let queue be generator.[[AsyncGeneratorQueue]].
    auto& queue = m_async_generator_queue;

    // 3. If queue is empty, return unused.
    if (queue.is_empty())
        return;

    // 4. Let done be false.
    bool done = false;

    // 5. Repeat, while done is false,
    while (!done) {
        // a. Let next be the first element of queue.
        auto& next = m_async_generator_queue.first();

        // b. Let completion be Completion(next.[[Completion]]).
        auto completion = Completion(next.completion);

        // c. If completion.[[Type]] is return, then
        if (completion.type() == Completion::Type::Return) {
            // i. Set generator.[[AsyncGeneratorState]] to awaiting-return.
            m_async_generator_state = State::AwaitingReturn;

            // ii. Perform AsyncGeneratorAwaitReturn(generator).
            await_return();

            // iii. Set done to true.
            done = true;
        }
        // d. Else,
        else {
            // i. If completion.[[Type]] is normal, then
            if (completion.type() == Completion::Type::Normal) {
                // 1. Set completion to NormalCompletion(undefined).
                completion = normal_completion(js_undefined());
            }

            // ii. Perform AsyncGeneratorCompleteStep(generator, completion, true).
            complete_step(completion, true);

            // iii. If queue is empty, set done to true.
            if (queue.is_empty())
                done = true;
        }
    }

    // 6. Return unused.
}

}
