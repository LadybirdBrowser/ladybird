/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;
use core::ptr::NonNull;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::heap_function::create_heap_function;
use crate::interpreter::execution_context::OwnedExecutionContext;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::{Completion, CompletionType, ThrowCompletionOr};
use crate::runtime::generator_object::{GeneratingFunction, GeneratorObject, GeneratorState, IterationResult};
use crate::runtime::native_function::NativeFunction;
use crate::runtime::promise::{Promise, PromiseState, promise_resolve};
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct AsyncFunctionDriverWrapper {
    base: Promise,
    generator_object: Cell<Gc<GeneratorObject>>,
    top_level_promise: Cell<Gc<Promise>>,
    current_promise: Cell<Option<Gc<Promise>>>,
    suspended_execution_context: GcRefCell<Option<OwnedExecutionContext>>,
    on_settled: Cell<Option<Gc<NativeFunction>>>,
    is_initial_execution: Cell<bool>,
}

define_cell!(AsyncFunctionDriverWrapper, Object, extends: [Promise, Object]);

impl Deref for AsyncFunctionDriverWrapper {
    type Target = Promise;

    fn deref(&self) -> &Promise {
        &self.base
    }
}

/// Whether awaiting `promise` can skip PromiseResolve and the reaction, since it is an already-settled intrinsic Promise
/// whose constructor is the intrinsic %Promise%.
fn is_settled_intrinsic_promise(vm: &Vm, realm: Gc<Realm>, promise: &Promise) -> bool {
    let promise_prototype = realm.intrinsics().promise_prototype(vm);
    promise.state() != PromiseState::Pending
        && promise.shape().property_count() == 0
        && promise.shape().prototype() == Some(promise_prototype)
        && promise_prototype.get_without_side_effects(vm, &vm.names.constructor)
            == Value::from_object(realm.intrinsics().promise_constructor(vm))
}

impl AsyncFunctionDriverWrapper {
    // 15.8.4 Runtime Semantics: EvaluateAsyncFunctionBody, https://tc39.es/ecma262/#sec-runtime-semantics-evaluateasyncfunctionbody
    // 27.7.5.1 AsyncFunctionStart ( promiseCapability, asyncFunctionBody ), https://tc39.es/ecma262/#sec-async-functions-abstract-operations-async-function-start
    /// Starts an async function whose bytecode just ran in its call's execution context `context`, from
    /// FunctionDeclarationInstantiation until it first awaited, returned or threw (`result`), and returns its promise.
    ///
    /// OPTIMIZATION: The spec runs the body in a copy of the call's execution context (asyncContext). The bytecode runs
    ///               it in the call's own context until it first awaits, and the async function gets its copy and a
    ///               driver only then, since nothing resumes the call's context. A function that never awaits needs
    ///               neither.
    pub fn start(
        vm: &Vm,
        realm: Gc<Realm>,
        generating_function: GeneratingFunction,
        context: &ExecutionContext,
        result: Result<Value, Value>,
    ) -> Gc<Promise> {
        // 1. Let promiseCapability be ! NewPromiseCapability(%Promise%).
        let top_level_promise = Promise::create(vm, realm);

        let value = match result {
            // 3. If completion is an abrupt completion, then
            //     a. Perform ! Call(promiseCapability.[[Reject]], undefined, « completion.[[Value]] »).
            // NB: The same goes for a throw completion of the body (AsyncBlockStart).
            Err(exception) => {
                top_level_promise.reject(vm, exception);
                return top_level_promise;
            }
            Ok(value) if value.is_empty() => Value::UNDEFINED,
            Ok(value) => value,
        };

        // NB: The body returned without awaiting.
        if context.yield_continuation.get() == ExecutionContext::NO_YIELD_CONTINUATION {
            // NB: Nothing else resolves the promise, so it needs no resolving functions.
            top_level_promise.resolve(vm, value);
            return top_level_promise;
        }

        // NB: The body awaits `value`, suspended like a generator at a yield.
        let generator_object = GeneratorObject::create(vm, realm, generating_function, context.copy());
        generator_object.set_generator_state(GeneratorState::SuspendedYield);
        // Note: The top_level_promise is also kept alive by this Wrapper
        let wrapper = realm.create_object(
            vm,
            AsyncFunctionDriverWrapper {
                base: Promise::new(vm, Self::CLASS, realm.intrinsics().promise_prototype(vm)),
                generator_object: Cell::new(generator_object),
                top_level_promise: Cell::new(top_level_promise),
                current_promise: Cell::new(None),
                suspended_execution_context: GcRefCell::new(None),
                on_settled: Cell::new(None),
                is_initial_execution: Cell::new(true),
            },
        );
        Self::run(wrapper, vm, Ok(IterationResult::new(value, false)));

        top_level_promise
    }

    fn suspended_execution_context(&self) -> NonNull<ExecutionContext> {
        self.suspended_execution_context
            .borrow()
            .as_ref()
            .expect("the async function has suspended")
            .as_non_null()
    }

    // 27.7.5.3 Await ( value ), https://tc39.es/ecma262/#await
    fn r#await(this: Gc<AsyncFunctionDriverWrapper>, vm: &Vm, value: Value) -> ThrowCompletionOr<()> {
        let realm = vm.current_realm().expect("there is a current realm");

        // 1. Let asyncContext be the running execution context.
        // NB: The generator object keeps the context the async function resumes in. This one only runs the steps that
        //     settle what it awaits, as the running context of the function's realm and script, so it holds none of
        //     the values of the function's frame.
        if this.suspended_execution_context.borrow().is_none() {
            let copy = vm
                .running_execution_context_if_any()
                .expect("an async function awaits in an execution context")
                .copy_without_values();
            *this.suspended_execution_context.borrow_mut(vm) = Some(copy);
        }

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
        if !value.is_object() {
            // Primitive values are never thenable -- schedule resume directly.
            Self::schedule_resume(this, vm, value, true);
            return Ok(());
        }

        if let Some(promise) = value.as_object().downcast::<Promise>() {
            // Already-settled native Promises whose constructor is the intrinsic %Promise%.
            if is_settled_intrinsic_promise(vm, realm, &promise) {
                Self::schedule_resume(this, vm, promise.result(), promise.state() == PromiseState::Fulfilled);
                promise.set_is_handled();
                return Ok(());
            }
        }

        // 2. Let promise be ? PromiseResolve(%Promise%, value).
        let promise_object = promise_resolve(vm, realm.intrinsics().promise_constructor(vm).upcast(), value)?;

        // 3. Let fulfilledClosure be a new Abstract Closure with parameters (v) that captures asyncContext and performs the
        //    following steps when called:
        // 5. Let rejectedClosure be a new Abstract Closure with parameters (reason) that captures asyncContext and performs the
        //    following steps when called:
        // OPTIMIZATION: onRejected and onFulfilled are identical other than the resumption value passed to continue_async_execution.
        //               To avoid allocated two GC functions down this path, we combine both callbacks into one function.
        // 4. Let onFulfilled be CreateBuiltinFunction(fulfilledClosure, 1, "", « »).
        // 6. Let onRejected be CreateBuiltinFunction(rejectedClosure, 1, "", « »).
        if this.on_settled.get().is_none() {
            let on_settled = NativeFunction::create_anonymous(
                vm,
                this,
                |vm, &this: &Gc<AsyncFunctionDriverWrapper>| {
                    let reason = vm.argument(0);

                    // The currently awaited promise is settled when this reaction runs, so we can use
                    // its state to decide whether to resume with a normal or throw completion.
                    let current_promise = this
                        .current_promise
                        .get()
                        .expect("an awaited promise settles the await");
                    let is_successful = current_promise.state() == PromiseState::Fulfilled;
                    assert!(is_successful || current_promise.state() == PromiseState::Rejected);

                    // a. Let prevContext be the running execution context.
                    let prev_context = vm.running_execution_context();

                    // b. Suspend prevContext.
                    // c. Push asyncContext onto the execution context stack; asyncContext is now the running execution context.
                    vm.push_execution_context_checking_stack_space(this.suspended_execution_context())?;

                    // 3.d. Resume the suspended evaluation of asyncContext using NormalCompletion(v) as the result of the operation that
                    //      suspended it.
                    // 5.d. Resume the suspended evaluation of asyncContext using ThrowCompletion(reason) as the result of the operation that
                    //      suspended it.
                    Self::continue_async_execution(this, vm, reason, is_successful);
                    vm.pop_execution_context();

                    // e. Assert: When we reach this step, asyncContext has already been removed from the execution context stack and
                    //    prevContext is the currently running execution context.
                    assert!(vm.running_execution_context() == prev_context);

                    // f. Return undefined.
                    Ok(Value::UNDEFINED)
                },
                1,
            );
            this.on_settled.set(Some(on_settled));
        }

        // 7. Perform PerformPromiseThen(promise, onFulfilled, onRejected).
        let current_promise = promise_object
            .downcast::<Promise>()
            .expect("PromiseResolve of %Promise% returns a Promise");
        this.current_promise.set(Some(current_promise));
        let on_settled = Value::from_object(this.on_settled.get().expect("the settled function was just created"));
        current_promise.perform_then(vm, on_settled, on_settled, None);

        // NOTE: None of these are necessary. 8-12 are handled by step d of the above lambdas.
        // 8. Remove asyncContext from the execution context stack and restore the execution context that is at the top of the
        //    execution context stack as the running execution context.
        // 9. Let callerContext be the running execution context.
        // 10. Resume callerContext passing empty. If asyncContext is ever resumed again, let completion be the Completion Record with which it is resumed.
        // 11. Assert: If control reaches here, then asyncContext is the running execution context again.
        // 12. Return completion.
        Ok(())
    }

    fn schedule_resume(this: Gc<AsyncFunctionDriverWrapper>, vm: &Vm, value: Value, is_fulfilled: bool) {
        let job = create_heap_function(
            vm,
            (this, value, is_fulfilled),
            |vm, &(this, value, is_fulfilled): &(Gc<AsyncFunctionDriverWrapper>, Value, bool)| {
                vm.push_execution_context_checking_stack_space(this.suspended_execution_context())?;
                Self::continue_async_execution(this, vm, value, is_fulfilled);
                vm.pop_execution_context();
                Ok(Value::UNDEFINED)
            },
        );
        vm.host_enqueue_promise_job()(vm, job, vm.current_realm());
    }

    fn continue_async_execution(this: Gc<AsyncFunctionDriverWrapper>, vm: &Vm, value: Value, is_successful: bool) {
        let generator_object = this.generator_object.get();
        let generator_result = if is_successful {
            generator_object.resume(vm, value, None)
        } else {
            crate::embedding::completion::log_exception_if_enabled(vm, value);
            generator_object.resume_abrupt(vm, Completion::new(CompletionType::Throw, value), None)
        };
        Self::run(this, vm, generator_result);
    }

    /// Continues the async function from what its generator last returned (`generator_result`): settles its promise if
    /// it completed, and awaits the value it awaits otherwise, resuming it right away where it can.
    fn run(this: Gc<AsyncFunctionDriverWrapper>, vm: &Vm, mut generator_result: ThrowCompletionOr<IterationResult>) {
        let generator_object = this.generator_object.get();
        let result: ThrowCompletionOr<()> = loop {
            let result: IterationResult = match generator_result {
                Err(throw) => break Err(throw),
                Ok(result) => result,
            };
            let promise_value = result.value;
            if result.done {
                // AsyncBlockStart resolves return completions via promiseCapability.[[Resolve]], which adopts returned
                // thenables without turning `return value;` into `return await value;`.
                // NB: Nothing else resolves the promise, so it needs no resolving functions.
                this.top_level_promise.get().resolve(vm, promise_value);
                break Ok(());
            }

            // We hit `await value`
            //
            // OPTIMIZATION: Synchronous await fast path.
            // If we're not in the initial execution (i.e. we were resumed from a microtask)
            // and the microtask queue is empty, we can resolve the await synchronously
            // without suspending. This is safe because no other microtask can observe the
            // difference in execution order.
            if !this.is_initial_execution.get() && vm.host_promise_job_queue_is_empty()(vm) {
                let realm = vm.current_realm().expect("there is a current realm");
                if !promise_value.is_object() {
                    // Primitive values are never thenable.
                    generator_result = generator_object.resume(vm, promise_value, None);
                    continue;
                }
                if let Some(promise) = promise_value.as_object().downcast::<Promise>()
                    && is_settled_intrinsic_promise(vm, realm, &promise)
                {
                    let is_fulfilled = promise.state() == PromiseState::Fulfilled;
                    promise.set_is_handled();
                    generator_result = if is_fulfilled {
                        generator_object.resume(vm, promise.result(), None)
                    } else {
                        crate::embedding::completion::log_exception_if_enabled(vm, promise.result());
                        generator_object.resume_abrupt(
                            vm,
                            Completion::new(CompletionType::Throw, promise.result()),
                            None,
                        )
                    };
                    continue;
                }
            }

            if let Err(throw) = Self::r#await(this, vm, promise_value) {
                generator_result = generator_object.resume_abrupt(vm, throw.into(), None);
                continue;
            }
            this.is_initial_execution.set(false);
            break Ok(());
        };

        if let Err(throw) = result {
            this.top_level_promise.get().reject(vm, throw.value());
        }
    }
}
