/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, class_of, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    DisposeCapability, add_disposable_resource, call_function_object, dispose_resources, new_dispose_capability,
};
use crate::runtime::completion::{Completion, Throw, ThrowCompletionOr};
use crate::runtime::environment::InitializeBindingHint;
use crate::runtime::suppressed_error::SuppressedError;
use libjs_abi::value as nan_box;

/// The [[DisposableResourceStack]] of a Block, ForStatement, ForInOfStatement iteration, function body or module that
/// contains `using` or `await using` declarations. The bytecode keeps it in a register rather than in an environment,
/// since the bindings of these declarations are often locals that live in no environment.
///
/// It also keeps the state of a DisposeResources of an `await using` scope, which the bytecode performs one step at a
/// time so that it can await between the steps.
#[repr(C)]
#[derive(Trace)]
pub struct DisposableResourceStack {
    header: CellHeader,
    dispose_capability: GcRefCell<DisposeCapability>,
    async_disposal_started: Cell<bool>,
    needs_await: Cell<bool>,
    has_awaited: Cell<bool>,
    is_awaiting_dispose_result: Cell<bool>,
    // The [[Value]] of outputCompletion while it is a throw completion, and empty while it is a normal completion.
    output_completion_value: Cell<Value>,
}

define_cell!(DisposableResourceStack, Other, finalize: finalize);

impl Finalize for DisposableResourceStack {
    fn finalize(&self) {
        drop(self.dispose_capability.replace(new_dispose_capability()));
    }
}

/// What the bytecode does after a step of an asynchronous DisposeResources.
pub enum AsyncDisposeResourcesStep {
    /// Await the value, then perform the next step with the completion of the await.
    Await(Value),
    /// The resources are disposed of, and the completion of the scope stays as it was.
    Done,
}

impl DisposableResourceStack {
    pub fn create(vm: &Vm) -> Gc<DisposableResourceStack> {
        vm.heap().allocate(|_| DisposableResourceStack {
            header: CellHeader::for_class(Self::CLASS),
            dispose_capability: GcRefCell::new(new_dispose_capability()),
            async_disposal_started: Cell::new(false),
            needs_await: Cell::new(false),
            has_awaited: Cell::new(false),
            is_awaiting_dispose_result: Cell::new(false),
            output_completion_value: Cell::new(Value::EMPTY),
        })
    }

    /// The stack as the interpreter keeps it in a register: a cell value without a more specific tag.
    pub fn to_value(stack: Gc<DisposableResourceStack>) -> Value {
        Value::with_cell_tag(nan_box::IS_CELL_BIT, stack)
    }

    /// The stack a register holds, which must be one.
    pub fn from_value(value: Value) -> Gc<DisposableResourceStack> {
        assert!(value.is_cell());
        // SAFETY: The value holds a cell, and every cell starts with its class, which is checked below.
        let stack = unsafe { value.cell::<DisposableResourceStack>() };
        assert!(
            class_of(stack).is_subclass_of(Self::CLASS),
            "the cell is not a disposable resource stack"
        );
        stack
    }

    pub fn add_disposable_resource(&self, vm: &Vm, value: Value, hint: InitializeBindingHint) -> ThrowCompletionOr<()> {
        add_disposable_resource(vm, &self.dispose_capability, value, hint, None)
    }

    pub fn dispose_resources(&self, vm: &Vm, completion: Completion) -> Completion {
        // NB: Every resource of a scope without `await using` declarations has the sync-dispose kind, so this never
        //     awaits.
        dispose_resources(vm, &self.dispose_capability, completion)
    }

    /// Performs DisposeResources up to its next Await, or to its end.
    ///
    /// The first step receives the completion of the scope, and each later step the completion of the Await that the
    /// previous step asked for. Throws the completion of the DisposeResources if it is a throw completion.
    // https://tc39.es/ecma262/#sec-disposeresources
    pub fn async_dispose_resources_step(
        &self,
        vm: &Vm,
        completion: Completion,
        await_completion: Completion,
    ) -> ThrowCompletionOr<AsyncDisposeResourcesStep> {
        if !self.async_disposal_started.get() {
            self.async_disposal_started.set(true);

            // 1. Let needsAwait be false.
            // 2. Let hasAwaited be false.
            // NB: These are the initial values of the fields.

            // 3. Let outputCompletion be completion.
            if completion.is_error() {
                self.output_completion_value.set(completion.value());
            }
        } else if self.is_awaiting_dispose_result.get() {
            self.is_awaiting_dispose_result.set(false);

            // NB: This is the completion of the Await of step 4.e.ii.1, which continues at step 4.e.iii.
            if await_completion.is_error() {
                self.dispose_method_threw(vm, await_completion.value());
            }
        }

        // 4. For each element resource of disposableResourceStack, in reverse List order, do
        // NB: Each resource is taken off the stack when it is disposed of, which leaves the next one at its end.
        loop {
            let Some(resource) = self
                .dispose_capability
                .borrow()
                .disposable_resource_stack
                .as_ref()
                .and_then(|stack| stack.last().copied())
            else {
                break;
            };

            // a. Let value be resource.[[ResourceValue]].
            let value = resource.resource_value;

            // b. Let kind be resource.[[Kind]].
            let kind = resource.hint;

            // c. Let method be resource.[[DisposeMethod]].
            let method = resource.dispose_method;

            // d. If kind is sync-dispose, needsAwait is true, and hasAwaited is false, then
            if kind == InitializeBindingHint::SyncDispose && self.needs_await.get() && !self.has_awaited.get() {
                // i. Perform ! Await(undefined).
                // ii. Set needsAwait to false.
                // NB: The bytecode performs the Await after this step. The resource stays on the stack, so the next
                //     step disposes of it.
                self.needs_await.set(false);
                return Ok(AsyncDisposeResourcesStep::Await(Value::UNDEFINED));
            }

            self.dispose_capability
                .borrow_mut(vm)
                .disposable_resource_stack
                .as_mut()
                .expect("the stack has the resource")
                .pop();

            // e. If method is not undefined, then
            if let Some(method) = method {
                // i. Let result be Completion(Call(method, value)).
                let result = call_function_object(vm, method, value, &[]);

                match result {
                    // ii. If result is a normal completion and kind is async-dispose, then
                    Ok(result_value) if kind == InitializeBindingHint::AsyncDispose => {
                        // 1. Set result to Completion(Await(result.[[Value]])).
                        // 2. Set hasAwaited to true.
                        // NB: The bytecode performs the Await after this step, and the next step continues at step
                        //     4.e.iii with its completion.
                        self.is_awaiting_dispose_result.set(true);
                        self.has_awaited.set(true);
                        return Ok(AsyncDisposeResourcesStep::Await(result_value));
                    }
                    Ok(_) => {}
                    // iii. If result is a throw completion, then
                    Err(throw) => self.dispose_method_threw(vm, throw.value()),
                }
            }
            // f. Else,
            else {
                // i. Assert: kind is async-dispose.
                assert!(kind == InitializeBindingHint::AsyncDispose);

                // ii. Set needsAwait to true.
                self.needs_await.set(true);

                // iii. NOTE: This can only indicate a case where either null or undefined was the initialized value of
                //      an await using declaration.
            }
        }

        // 5. If needsAwait is true and hasAwaited is false, then
        if self.needs_await.get() && !self.has_awaited.get() {
            // NB: The next step comes back here with an empty stack, and must not await again.
            self.needs_await.set(false);

            // a. Perform ! Await(undefined).
            return Ok(AsyncDisposeResourcesStep::Await(Value::UNDEFINED));
        }

        // 6. NOTE: At this point disposableResourceStack will never be used again. The contents of
        //    disposableResourceStack can be discarded in implementations, such as by garbage collection.
        self.dispose_capability.borrow_mut(vm).disposable_resource_stack = None;

        // 7. Return ? outputCompletion.
        let output_completion_value = self.output_completion_value.get();
        if !output_completion_value.is_empty() {
            return Err(Throw::new(output_completion_value));
        }
        Ok(AsyncDisposeResourcesStep::Done)
    }

    // Steps 4.e.iii.1-2 of DisposeResources, for a result that is a throw completion.
    fn dispose_method_threw(&self, vm: &Vm, result: Value) {
        let output_completion_value = self.output_completion_value.get();

        // 1. If outputCompletion is a throw completion, then
        if !output_completion_value.is_empty() {
            // a. Set result to result.[[Value]].
            // b. Let suppressed be outputCompletion.[[Value]].
            let suppressed = output_completion_value;

            // c. Let error be a newly created SuppressedError object.
            let error = SuppressedError::create(vm, vm.current_realm().expect("DisposeResources runs in a realm"));

            // d. Perform CreateNonEnumerableDataPropertyOrThrow(error, "error", result).
            error.create_non_enumerable_data_property_or_throw(vm, &vm.names.error, result);

            // e. Perform CreateNonEnumerableDataPropertyOrThrow(error, "suppressed", suppressed).
            error.create_non_enumerable_data_property_or_throw(vm, &vm.names.suppressed, suppressed);

            // f. Set outputCompletion to ThrowCompletion(error).
            crate::embedding::completion::log_exception_if_enabled(vm, Value::from_object(error));
            self.output_completion_value.set(Value::from_object(error));
        }
        // 2. Else,
        else {
            // a. Set outputCompletion to result.
            self.output_completion_value.set(result);
        }
    }
}
