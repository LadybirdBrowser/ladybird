/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;
use core::ptr::NonNull;
use std::collections::VecDeque;

use libjs_runtime_macros::Trace;

use crate::bytecode::executable::Executable;
use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::execution_context::OwnedExecutionContext;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{call_function_object, get_prototype_from_constructor};
use crate::runtime::async_generator_request::AsyncGeneratorRequest;
use crate::runtime::completion::{Completion, CompletionType, Must, ThrowCompletionOr};
use crate::runtime::generator_object::GeneratingFunction;
use crate::runtime::iterator::create_iterator_result_object;
use crate::runtime::native_function::NativeFunction;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::promise::{Promise, promise_resolve};
use crate::runtime::promise_capability::PromiseCapability;
use crate::runtime::realm::Realm;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Trace)]
pub enum AsyncGeneratorState {
    SuspendedStart,
    SuspendedYield,
    Executing,
    DrainingQueue,
    Completed,
}

// 27.6.2 Properties of AsyncGenerator Instances, https://tc39.es/ecma262/#sec-properties-of-asyncgenerator-intances
#[repr(C)]
#[derive(Trace)]
pub struct AsyncGenerator {
    base: Object,
    async_generator_state: Cell<AsyncGeneratorState>, // [[AsyncGeneratorState]]
    async_generator_context: GcRefCell<Option<OwnedExecutionContext>>, // [[AsyncGeneratorContext]]
    async_generator_queue: GcRefCell<VecDeque<AsyncGeneratorRequest>>, // [[AsyncGeneratorQueue]]
    generator_brand: Option<&'static str>,            // [[GeneratorBrand]]

    generating_executable: Cell<Gc<Executable>>,
    yield_continuation: Cell<u32>,
    current_promise: Cell<Option<Gc<Promise>>>,
    pending_completion_value: Cell<Value>,
    #[gc(untraced)]
    pending_completion_type: Cell<CompletionType>,
}

define_cell!(AsyncGenerator, Object, extends: [Object], finalize: finalize);

impl Finalize for AsyncGenerator {
    fn finalize(&self) {
        drop(self.async_generator_queue.replace(VecDeque::new()));
        drop(self.async_generator_context.replace(None));
    }
}

impl Deref for AsyncGenerator {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl AsyncGenerator {
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        generating_function: GeneratingFunction,
        execution_context: OwnedExecutionContext,
    ) -> Gc<AsyncGenerator> {
        // 2. Let _generator_ be ? OrdinaryCreateFromConstructor(_functionObject_, *"%AsyncGeneratorPrototype%"*,
        //    « [[AsyncGeneratorState]], [[AsyncGeneratorContext]], [[AsyncGeneratorQueue]], [[GeneratorBrand]] »).
        let generating_function_prototype_object =
            get_prototype_from_constructor(vm, generating_function.as_function_object(), |intrinsics, _| {
                intrinsics.async_generator_prototype()
            })
            .must();

        let generating_executable = generating_function.bytecode_executable(vm);

        let yield_continuation = execution_context.yield_continuation.get();
        realm.create_object(
            vm,
            AsyncGenerator {
                base: Object::new_with_realm_and_prototype(
                    vm,
                    Self::CLASS,
                    realm,
                    Some(generating_function_prototype_object),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                async_generator_state: Cell::new(AsyncGeneratorState::SuspendedStart),
                async_generator_context: GcRefCell::new(Some(execution_context)),
                async_generator_queue: GcRefCell::new(VecDeque::new()),
                generator_brand: None,
                generating_executable: Cell::new(generating_executable),
                yield_continuation: Cell::new(yield_continuation),
                current_promise: Cell::new(None),
                pending_completion_value: Cell::new(Value::UNDEFINED),
                pending_completion_type: Cell::new(CompletionType::Normal),
            },
        )
    }

    fn as_gc(&self) -> Gc<AsyncGenerator> {
        // SAFETY: Async generators only exist as cells once constructed.
        unsafe { Gc::from_ref(self) }
    }

    /// [[AsyncGeneratorContext]]. It lives out of line until the generator is finalized, so the pointer stays valid for
    /// as long as the generator does.
    fn async_generator_context(&self) -> NonNull<ExecutionContext> {
        self.async_generator_context
            .borrow()
            .as_ref()
            .expect("a live async generator has its execution context")
            .as_non_null()
    }

    fn async_generator_context_ref(&self) -> &ExecutionContext {
        // SAFETY: Only finalizing the generator frees its context, and the generator outlives the reference.
        unsafe { self.async_generator_context().as_ref() }
    }

    pub fn async_generator_state(&self) -> AsyncGeneratorState {
        self.async_generator_state.get()
    }

    pub fn set_async_generator_state(&self, value: AsyncGeneratorState) {
        self.async_generator_state.set(value);
    }

    pub fn generator_brand(&self) -> Option<&'static str> {
        self.generator_brand
    }

    pub fn set_pending_completion(&self, completion: Completion) {
        self.pending_completion_value.set(completion.value());
        self.pending_completion_type.set(completion.completion_type());
    }

    pub fn pending_completion_value(&self) -> Value {
        self.pending_completion_value.get()
    }

    pub fn pending_completion_type(&self) -> CompletionType {
        self.pending_completion_type.get()
    }

    pub fn set_pending_completion_type(&self, completion_type: CompletionType) {
        self.pending_completion_type.set(completion_type);
    }

    pub fn clear_pending_completion(&self) {
        self.pending_completion_value.set(Value::UNDEFINED);
        self.pending_completion_type.set(CompletionType::Normal);
    }

    // 27.6.3.4 AsyncGeneratorEnqueue ( generator, completion, promiseCapability ), https://tc39.es/ecma262/#sec-asyncgeneratorenqueue
    pub fn async_generator_enqueue(&self, completion: Completion, promise_capability: Gc<PromiseCapability>) {
        // 1. Let request be AsyncGeneratorRequest { [[Completion]]: completion, [[Capability]]: promiseCapability }.
        let request = AsyncGeneratorRequest {
            completion,
            capability: promise_capability,
        };

        // 2. Append request to generator.[[AsyncGeneratorQueue]].
        self.async_generator_queue.borrow_mut().push_back(request);

        // 3. Return unused.
    }

    fn queue_is_empty(&self) -> bool {
        self.async_generator_queue.borrow().is_empty()
    }

    fn first_request(&self) -> AsyncGeneratorRequest {
        *self
            .async_generator_queue
            .borrow()
            .front()
            .expect("the queue is not empty")
    }

    // 27.7.5.3 Await ( value ), https://tc39.es/ecma262/#await
    fn r#await(&self, vm: &Vm, value: Value) -> ThrowCompletionOr<()> {
        let realm = vm.current_realm().expect("there is a current realm");

        // 1. Let asyncContext be the running execution context.
        // NB: That is the generator's own context, which execute() runs in.
        debug_assert!(vm.running_execution_context() == Some(self.async_generator_context()));

        // 2. Let promise be ? PromiseResolve(%Promise%, value).
        let promise_object = promise_resolve(vm, realm.intrinsics().promise_constructor(vm).upcast(), value)?;

        // 3. Let fulfilledClosure be a new Abstract Closure with parameters (v) that captures asyncContext and performs the
        //    following steps when called:
        // 4. Let onFulfilled be CreateBuiltinFunction(fulfilledClosure, 1, "", « »).
        let on_fulfilled = NativeFunction::create_anonymous(
            vm,
            self.as_gc(),
            |vm, &generator: &Gc<AsyncGenerator>| {
                let value = vm.argument(0);

                // a. Let prevContext be the running execution context.
                let prev_context = vm.running_execution_context();

                // b. Suspend prevContext.
                // c. Push asyncContext onto the execution context stack; asyncContext is now the running execution context.
                vm.push_execution_context_checking_stack_space(generator.async_generator_context())?;

                // d. Resume the suspended evaluation of asyncContext using NormalCompletion(value) as the result of the operation that
                //    suspended it.
                generator.execute(vm, Completion::normal(value));

                // e. Assert: When we reach this step, asyncContext has already been removed from the execution context stack and
                //    prevContext is the currently running execution context.
                assert!(vm.running_execution_context() == prev_context);

                // f. Return undefined.
                Ok(Value::UNDEFINED)
            },
            1,
        );

        // 5. Let rejectedClosure be a new Abstract Closure with parameters (reason) that captures asyncContext and performs the
        //    following steps when called:
        // 6. Let onRejected be CreateBuiltinFunction(rejectedClosure, 1, "", « »).
        let on_rejected = NativeFunction::create_anonymous(
            vm,
            self.as_gc(),
            |vm, &generator: &Gc<AsyncGenerator>| {
                let reason = vm.argument(0);

                // a. Let prevContext be the running execution context.
                let prev_context = vm.running_execution_context();

                // b. Suspend prevContext.
                // c. Push asyncContext onto the execution context stack; asyncContext is now the running execution context.
                vm.push_execution_context_checking_stack_space(generator.async_generator_context())?;

                // d. Resume the suspended evaluation of asyncContext using ThrowCompletion(reason) as the result of the operation that
                //    suspended it.
                generator.execute(vm, Completion::new(CompletionType::Throw, reason));

                // e. Assert: When we reach this step, asyncContext has already been removed from the execution context stack and
                //    prevContext is the currently running execution context.
                assert!(vm.running_execution_context() == prev_context);

                // f. Return undefined.
                Ok(Value::UNDEFINED)
            },
            1,
        );

        // 7. Perform PerformPromiseThen(promise, onFulfilled, onRejected).
        let current_promise = promise_object
            .downcast::<Promise>()
            .expect("PromiseResolve of %Promise% returns a Promise");
        self.current_promise.set(Some(current_promise));
        current_promise.perform_then(
            vm,
            Value::from_object(on_fulfilled),
            Value::from_object(on_rejected),
            None,
        );

        // 8. Remove asyncContext from the execution context stack and restore the execution context that is at the top of the
        //    execution context stack as the running execution context.
        vm.pop_execution_context();

        // NOTE: None of these are necessary. 10-12 are handled by step d of the above lambdas.
        // 9. Let callerContext be the running execution context.
        // 10. Resume callerContext passing empty. If asyncContext is ever resumed again, let completion be the Completion Record with which it is resumed.
        // 11. Assert: If control reaches here, then asyncContext is the running execution context again.
        // 12. Return completion.
        Ok(())
    }

    fn execute(&self, vm: &Vm, mut completion: Completion) {
        loop {
            self.set_pending_completion(completion);

            // We should never enter `execute` again after the generator is complete.
            let yield_continuation = self.yield_continuation.get();
            assert!(yield_continuation != ExecutionContext::NO_YIELD_CONTINUATION);

            // Clear yield state so that a normal return (no yield) is detected as done.
            self.async_generator_context_ref()
                .yield_continuation
                .set(ExecutionContext::NO_YIELD_CONTINUATION);

            let result_value = vm.run_executable_with_initial_accumulator_value(
                vm.running_execution_context()
                    .expect("the async generator's context is running"),
                self.generating_executable.get(),
                yield_continuation,
                Value::from_object(self.as_gc()),
            );
            self.clear_pending_completion();

            let value = match result_value {
                Err(exception) => {
                    self.yield_continuation.set(ExecutionContext::NO_YIELD_CONTINUATION);

                    // 27.9.3.2 AsyncGeneratorStart ( gen, genBody ), https://tc39.es/ecma262/#sec-asyncgeneratorstart
                    // 4.e. Assert: If we return here, the async generator either threw an exception or performed either an implicit or explicit return.
                    // 4.f. Remove acGenContext from the execution context stack and restore the execution context
                    //      that is at the top of the execution context stack as the running execution context.
                    vm.pop_execution_context();
                    self.async_generator_state.set(AsyncGeneratorState::DrainingQueue);
                    self.complete_step(vm, Completion::new(CompletionType::Throw, exception), true, None);
                    self.drain_queue(vm);
                    return;
                }
                Ok(value) => value,
            };

            let value = if value.is_empty() { Value::UNDEFINED } else { value };

            self.yield_continuation
                .set(self.async_generator_context_ref().yield_continuation.get());
            let is_await = self.async_generator_context_ref().yield_is_await.get();

            if is_await {
                if let Err(throw) = self.r#await(vm, value) {
                    completion = throw.into();
                    continue;
                }
                return;
            }

            let done = self.yield_continuation.get() == ExecutionContext::NO_YIELD_CONTINUATION;
            if !done {
                // 27.6.3.8 AsyncGeneratorYield ( value ), https://tc39.es/ecma262/#sec-asyncgeneratoryield
                // 1. Let genContext be the running execution context.
                // 2. Assert: genContext is the execution context of a generator.
                // 3. Let generator be the value of the Generator component of genContext.
                // 4. Assert: GetGeneratorKind() is async.
                // NOTE: genContext is `m_async_generator_context`, generator is `this`.

                // 5. Let completion be NormalCompletion(value).
                let yield_completion = Completion::normal(value);

                // 6. Assert: The execution context stack has at least two elements.
                let previous_context = vm
                    .previous_execution_context()
                    .expect("the async generator was resumed from another context");

                // 7. Let previousContext be the second to top element of the execution context stack.
                // 8. Let previousRealm be previousContext's Realm.
                // SAFETY: The contexts on the execution context stack are live.
                let previous_realm = unsafe { previous_context.as_ref() }.realm.get();

                // 9. Perform AsyncGeneratorCompleteStep(generator, completion, false, previousRealm).
                self.complete_step(vm, yield_completion, false, previous_realm);

                // 10. Let queue be generator.[[AsyncGeneratorQueue]].
                // 11. If queue is not empty, then
                if !self.queue_is_empty() {
                    // a. NOTE: Execution continues without suspending the generator.
                    // b. Let toYield be the first element of queue.
                    let to_yield = self.first_request();

                    // c. Let resumptionValue be Completion(toYield.[[Completion]]).
                    completion = to_yield.completion;

                    // d. Return ? AsyncGeneratorUnwrapYieldResumption(resumptionValue).
                    // NOTE: AsyncGeneratorUnwrapYieldResumption is performed inside the continuation block inside the generator,
                    //       so we just need to enter the generator again.
                    continue;
                }
                // 12. Else,
                else {
                    // a. Set generator.[[AsyncGeneratorState]] to suspendedYield.
                    self.async_generator_state.set(AsyncGeneratorState::SuspendedYield);

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

            // 27.9.3.2 AsyncGeneratorStart ( gen, genBody ), https://tc39.es/ecma262/#sec-asyncgeneratorstart
            // 4.e. Assert: If we return here, the async generator either threw an exception or performed either an implicit or explicit return.
            // 4.f. Remove acGenContext from the execution context stack and restore the execution context that is at the top of the execution context stack as the running execution context.
            vm.pop_execution_context();

            // 4.g. Set acGen.[[AsyncGeneratorState]] to draining-queue.
            self.async_generator_state.set(AsyncGeneratorState::DrainingQueue);

            // 4.h. If result is a normal completion, set result to NormalCompletion(undefined).
            // 4.i. If result is a return completion, set result to NormalCompletion(result.[[Value]]).
            // 4.j. Perform AsyncGeneratorCompleteStep(acGen, result, true).
            self.complete_step(vm, Completion::normal(value), true, None);

            // 4.k. Perform AsyncGeneratorDrainQueue(acGen).
            self.drain_queue(vm);

            // 4.l. Let callerContext be the running execution context.
            // 4.m. Resume callerContext, passing NormalCompletion(undefined).
            // 4.n. Assert: This step is never reached.
            return;
        }
    }

    // 27.6.3.6 AsyncGeneratorResume ( generator, completion ), https://tc39.es/ecma262/#sec-asyncgeneratorresume
    pub fn resume(&self, vm: &Vm, completion: Completion) -> ThrowCompletionOr<()> {
        // 1. Assert: generator.[[AsyncGeneratorState]] is either suspendedStart or suspendedYield.
        assert!(matches!(
            self.async_generator_state.get(),
            AsyncGeneratorState::SuspendedStart | AsyncGeneratorState::SuspendedYield
        ));

        // 2. Let genContext be generator.[[AsyncGeneratorContext]].
        let generator_context = self.async_generator_context();

        // 3. Let callerContext be the running execution context.
        let caller_context = vm.running_execution_context();

        // 4. Suspend callerContext.
        // 5. Set generator.[[AsyncGeneratorState]] to executing.
        self.async_generator_state.set(AsyncGeneratorState::Executing);

        // 6. Push genContext onto the execution context stack; genContext is now the running execution context.
        vm.push_execution_context_checking_stack_space(generator_context)?;

        // 7. Resume the suspended evaluation of genContext using completion as the result of the operation that suspended
        //    it. Let result be the Completion Record returned by the resumed computation.
        // 8. Assert: result is never an abrupt completion.
        self.execute(vm, completion);

        // 9. Assert: When we return here, genContext has already been removed from the execution context stack and
        //    callerContext is the currently running execution context.
        assert!(vm.running_execution_context() == caller_context);

        // 10. Return unused.
        Ok(())
    }

    // 27.9.3.9 AsyncGeneratorAwaitReturn ( gen ), https://tc39.es/ecma262/#sec-asyncgeneratorawaitreturn
    pub fn await_return(&self, vm: &Vm) {
        let realm = vm.current_realm().expect("there is a current realm");

        // 1. Assert: gen.[[AsyncGeneratorState]] is draining-queue.
        assert!(self.async_generator_state.get() == AsyncGeneratorState::DrainingQueue);

        // 2. Let queue be gen.[[AsyncGeneratorQueue]].
        // 3. Assert: queue is not empty.
        assert!(!self.queue_is_empty());

        // 4. Let next be the first element of queue.
        let next = self.first_request();

        // 5. Let completion be Completion(next.[[Completion]]).
        let completion = next.completion;

        // 6. Assert: completion is a return completion.
        assert!(completion.completion_type() == CompletionType::Return);

        // 7. Let promiseCompletion be Completion(PromiseResolve(%Promise%, completion.[[Value]])).
        let promise_completion = promise_resolve(
            vm,
            realm.intrinsics().promise_constructor(vm).upcast(),
            completion.value(),
        );

        // 8. If promiseCompletion is an abrupt completion, then
        // 9. Assert: promiseCompletion is a normal completion.
        // 10. Let promise be promiseCompletion.[[Value]].
        let promise = match promise_completion {
            Err(throw) => {
                // a. Perform AsyncGeneratorCompleteStep(gen, promiseCompletion, true).
                self.complete_step(vm, throw.into(), true, None);

                // b. Perform AsyncGeneratorDrainQueue(gen).
                self.drain_queue(vm);

                // c. Return unused.
                return;
            }
            Ok(promise) => promise,
        };

        // 11. Let fulfilledClosure be a new Abstract Closure with parameters (value) that captures gen and performs
        //    the following steps when called:
        // 12. Let onFulfilled be CreateBuiltinFunction(fulfilledClosure, 1, "", « »).
        let on_fulfilled = NativeFunction::create_anonymous(
            vm,
            self.as_gc(),
            |vm, &generator: &Gc<AsyncGenerator>| {
                // a. Assert: gen.[[AsyncGeneratorState]] is draining-queue.
                assert!(generator.async_generator_state.get() == AsyncGeneratorState::DrainingQueue);

                // b. Let result be NormalCompletion(value).
                let result = Completion::normal(vm.argument(0));

                // c. Perform AsyncGeneratorCompleteStep(gen, result, true).
                generator.complete_step(vm, result, true, None);

                // d. Perform AsyncGeneratorDrainQueue(gen).
                generator.drain_queue(vm);

                // e. Return NormalCompletion(undefined).
                Ok(Value::UNDEFINED)
            },
            1,
        );

        // 13. Let rejectedClosure be a new Abstract Closure with parameters (reason) that captures gen and performs
        //    the following steps when called:
        // 14. Let onRejected be CreateBuiltinFunction(rejectedClosure, 1, "", « »).
        let on_rejected = NativeFunction::create_anonymous(
            vm,
            self.as_gc(),
            |vm, &generator: &Gc<AsyncGenerator>| {
                // a. Assert: gen.[[AsyncGeneratorState]] is draining-queue.
                assert!(generator.async_generator_state.get() == AsyncGeneratorState::DrainingQueue);

                // b. Let result be ThrowCompletion(reason).
                let result = Completion::new(CompletionType::Throw, vm.argument(0));

                // c. Perform AsyncGeneratorCompleteStep(gen, result, true).
                generator.complete_step(vm, result, true, None);

                // d. Perform AsyncGeneratorDrainQueue(gen).
                generator.drain_queue(vm);

                // e. Return NormalCompletion(undefined).
                Ok(Value::UNDEFINED)
            },
            1,
        );

        // 15. Perform PerformPromiseThen(promise, onFulfilled, onRejected).
        // NOTE: await_return should only be called when the generator is draining its queue,
        //       so an await shouldn't be running currently, so it should be safe to overwrite m_current_promise.
        let current_promise = promise
            .downcast::<Promise>()
            .expect("PromiseResolve of %Promise% returns a Promise");
        self.current_promise.set(Some(current_promise));
        current_promise.perform_then(
            vm,
            Value::from_object(on_fulfilled),
            Value::from_object(on_rejected),
            None,
        );

        // 16. Return unused.
    }

    // 27.6.3.5 AsyncGeneratorCompleteStep ( generator, completion, done [ , realm ] ), https://tc39.es/ecma262/#sec-asyncgeneratorcompletestep
    pub fn complete_step(&self, vm: &Vm, completion: Completion, done: bool, realm: Option<Gc<Realm>>) {
        // 1. Assert: generator.[[AsyncGeneratorQueue]] is not empty.
        // 2. Let next be the first element of generator.[[AsyncGeneratorQueue]].
        // 3. Remove the first element from generator.[[AsyncGeneratorQueue]].
        let next = self
            .async_generator_queue
            .borrow_mut()
            .pop_front()
            .expect("the queue is not empty");

        // 4. Let promiseCapability be next.[[Capability]].
        let promise_capability = next.capability;

        // 5. Let value be completion.[[Value]].
        let value = completion.value();

        // 6. If completion.[[Type]] is throw, then
        if completion.completion_type() == CompletionType::Throw {
            // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « value »).
            call_function_object(vm, promise_capability.reject(), Value::UNDEFINED, &[value]).must();
        }
        // 7. Else,
        else {
            // a. Assert: completion.[[Type]] is normal.
            assert!(completion.completion_type() == CompletionType::Normal);

            // b. If realm is present, then
            //    i. Let oldRealm be the running execution context's Realm.
            //    ii. Set the running execution context's Realm to realm.
            //    iii. Let iteratorResult be CreateIterResultObject(value, done).
            //    iv. Set the running execution context's Realm to oldRealm.
            // c. Else,
            //    i. Let iteratorResult be CreateIterResultObject(value, done).
            let iterator_result_realm = realm.unwrap_or_else(|| vm.current_realm().expect("there is a current realm"));
            let iterator_result = create_iterator_result_object(vm, iterator_result_realm, value, done);

            // d. Perform ! Call(promiseCapability.[[Resolve]], undefined, « iteratorResult »).
            call_function_object(
                vm,
                promise_capability.resolve(),
                Value::UNDEFINED,
                &[Value::from_object(iterator_result)],
            )
            .must();
        }

        // 8. Return unused.
    }

    // 27.9.3.10 AsyncGeneratorDrainQueue ( gen ), https://tc39.es/ecma262/#sec-asyncgeneratordrainqueue
    pub fn drain_queue(&self, vm: &Vm) {
        // 1. Assert: gen.[[AsyncGeneratorState]] is draining-queue.
        assert!(self.async_generator_state.get() == AsyncGeneratorState::DrainingQueue);

        // 2. Let queue be gen.[[AsyncGeneratorQueue]].
        // 3. Repeat, while queue is not empty,
        while !self.queue_is_empty() {
            // a. Let next be the first element of queue.
            let next = self.first_request();

            // b. Let completion be Completion(next.[[Completion]]).
            let mut completion = next.completion;

            // c. If completion is a return completion, then
            if completion.completion_type() == CompletionType::Return {
                // i. Perform AsyncGeneratorAwaitReturn(gen).
                self.await_return(vm);

                // ii. Return unused.
                return;
            }

            // d. If completion is a normal completion, then
            if completion.completion_type() == CompletionType::Normal {
                // i. Set completion to NormalCompletion(undefined).
                completion = Completion::normal(Value::UNDEFINED);
            }

            // e. Perform AsyncGeneratorCompleteStep(gen, completion, true).
            self.complete_step(vm, completion, true, None);
        }

        // 4. Set gen.[[AsyncGeneratorState]] to completed.
        self.async_generator_state.set(AsyncGeneratorState::Completed);

        // 5. Return unused.
    }
}
