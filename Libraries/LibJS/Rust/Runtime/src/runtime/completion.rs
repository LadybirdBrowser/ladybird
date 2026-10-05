/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::value::Value;
use crate::runtime::native_function::NativeFunction;
use crate::runtime::promise::{Promise, promise_resolve};

/// A thrown value. Throws are kept distinct from values so that one cannot be mistaken for a normal result.
#[derive(Clone, Copy, Debug)]
pub struct Throw(Value);

impl Throw {
    pub fn new(value: Value) -> Self {
        debug_assert!(!value.is_empty());
        Self(value)
    }

    pub fn value(self) -> Value {
        self.0
    }
}

/// The result of an operation that completes normally with a T or throws.
pub type ThrowCompletionOr<T> = Result<T, Throw>;

/// MUST() of the C++ runtime: the spec's `!`, for an operation that cannot throw here.
pub trait Must<T> {
    fn must(self) -> T;
}

impl<T> Must<T> for ThrowCompletionOr<T> {
    #[track_caller]
    fn must(self) -> T {
        match self {
            Ok(value) => value,
            Err(throw) => panic!("an operation that cannot throw threw {:?}", throw.value()),
        }
    }
}

pub use libjs_abi::CompletionType;

/// The [[Type]] of a completion as the bytecode encodes it, in the operands of IteratorClose and SetCompletionType.
pub fn completion_type_from_bytecode(encoded: u32) -> CompletionType {
    match encoded {
        0 => CompletionType::Empty,
        1 => CompletionType::Normal,
        2 => CompletionType::Break,
        3 => CompletionType::Continue,
        4 => CompletionType::Return,
        5 => CompletionType::Throw,
        _ => unreachable!("the bytecode encodes a completion type"),
    }
}

// 6.2.4 The Completion Record Specification Type, https://tc39.es/ecma262/#sec-completion-record-specification-type
/// A completion record, for the operations that take or return one rather than a ThrowCompletionOr, like
/// IteratorClose. There is no [[Target]], since control flow is handled in bytecode.
#[derive(Clone, Copy, Debug)]
pub struct Completion {
    completion_type: CompletionType, // [[Type]]
    value: Value,                    // [[Value]]
}

impl Completion {
    pub fn new(completion_type: CompletionType, value: Value) -> Self {
        debug_assert!(completion_type != CompletionType::Empty);
        debug_assert!(!value.is_empty());
        Self { completion_type, value }
    }

    // 5.2.3.1 Implicit Completion Values, https://tc39.es/ecma262/#sec-implicit-completion-values
    pub fn normal(value: Value) -> Self {
        Self::new(CompletionType::Normal, value)
    }

    pub fn completion_type(&self) -> CompletionType {
        self.completion_type
    }

    pub fn value(&self) -> Value {
        self.value
    }

    /// "abrupt completion refers to any completion with a [[Type]] value other than normal"
    pub fn is_abrupt(&self) -> bool {
        self.completion_type != CompletionType::Normal
    }

    pub fn is_error(&self) -> bool {
        self.completion_type == CompletionType::Throw
    }

    pub fn release_error(self) -> Throw {
        assert!(self.is_error());
        Throw::new(self.value)
    }

    /// The completion the way TRY() and ASM_TRY() see a C++ Completion: a throw completion is an error, and any other
    /// completion its value.
    pub fn into_throw_completion_or(self) -> ThrowCompletionOr<Value> {
        if self.is_error() {
            return Err(Throw::new(self.value));
        }
        Ok(self.value)
    }
}

/// The outcome of the promise await() waits for, which its reactions report back.
#[repr(C)]
#[derive(Trace)]
struct AwaitedCompletion {
    header: CellHeader,
    success: Cell<Option<bool>>,
    result: Cell<Value>,
}

define_cell!(AwaitedCompletion, Other);

// 6.2.3.1 Await, https://tc39.es/ecma262/#await
// FIXME: This no longer matches the spec!
pub fn r#await(vm: &Vm, value: Value) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("there is a current realm");

    // 1. Let asyncContext be the running execution context.
    // NOTE: This is not needed, as we don't suspend anything.

    // 2. Let promise be ? PromiseResolve(%Promise%, value).
    let promise_object = promise_resolve(vm, realm.intrinsics().promise_constructor(vm).upcast(), value)?;

    let awaited_completion = vm.heap().allocate(AwaitedCompletion {
        header: CellHeader::for_class(AwaitedCompletion::CLASS),
        success: Cell::new(None),
        result: Cell::new(Value::UNDEFINED),
    });

    // 3. Let fulfilledClosure be a new Abstract Closure with parameters (value) that captures asyncContext and performs the following steps when called:
    // 4. Let onFulfilled be CreateBuiltinFunction(fulfilledClosure, 1, "", « »).
    let on_fulfilled = NativeFunction::create_anonymous(
        vm,
        awaited_completion,
        |vm, &awaited_completion: &Gc<AwaitedCompletion>| {
            // a. Let prevContext be the running execution context.
            // b. Suspend prevContext.
            // FIXME: We don't have this concept yet.

            // NOTE: Since we don't support context suspension, we exfiltrate the result to await()'s scope instead
            awaited_completion.success.set(Some(true));
            awaited_completion.result.set(vm.argument(0));

            // c. Push asyncContext onto the execution context stack; asyncContext is now the running execution context.
            // NOTE: This is not done, because we're not suspending anything (see above).

            // d. Resume the suspended evaluation of asyncContext using NormalCompletion(value) as the result of the operation that suspended it.
            // e. Assert: When we reach this step, asyncContext has already been removed from the execution context stack and prevContext is the currently running execution context.
            // FIXME: We don't have this concept yet.

            // f. Return undefined.
            Ok(Value::UNDEFINED)
        },
        1,
    );

    // 5. Let rejectedClosure be a new Abstract Closure with parameters (reason) that captures asyncContext and performs the following steps when called:
    // 6. Let onRejected be CreateBuiltinFunction(rejectedClosure, 1, "", « »).
    let on_rejected = NativeFunction::create_anonymous(
        vm,
        awaited_completion,
        |vm, &awaited_completion: &Gc<AwaitedCompletion>| {
            // a. Let prevContext be the running execution context.
            // b. Suspend prevContext.
            // FIXME: We don't have this concept yet.

            // NOTE: Since we don't support context suspension, we exfiltrate the result to await()'s scope instead
            awaited_completion.success.set(Some(false));
            awaited_completion.result.set(vm.argument(0));

            // c. Push asyncContext onto the execution context stack; asyncContext is now the running execution context.
            // NOTE: This is not done, because we're not suspending anything (see above).

            // d. Resume the suspended evaluation of asyncContext using ThrowCompletion(reason) as the result of the operation that suspended it.
            // e. Assert: When we reach this step, asyncContext has already been removed from the execution context stack and prevContext is the currently running execution context.
            // FIXME: We don't have this concept yet.

            // f. Return undefined.
            Ok(Value::UNDEFINED)
        },
        1,
    );

    // 7. Perform PerformPromiseThen(promise, onFulfilled, onRejected).
    promise_object
        .downcast::<Promise>()
        .expect("PromiseResolve of %Promise% returns a Promise")
        .perform_then(
            vm,
            Value::from_object(on_fulfilled),
            Value::from_object(on_rejected),
            None,
        );

    // FIXME: Since we don't support context suspension, we attempt to "wait" for the promise to resolve
    //        by synchronously running all queued promise jobs.
    if let Some(agent) = vm.agent().embedder_agent {
        // Embedder case (i.e. LibWeb). Runs all promise jobs by performing a microtask checkpoint.
        // NB: The C++ goal condition captures a copy of the outcome taken before the promise settles, so it never
        //     becomes true. This one reads the outcome itself.
        agent.spin_event_loop_until(vm, &|| awaited_completion.success.get().is_some());
    } else {
        // No embedder, standalone LibJS implementation
        vm.run_queued_promise_jobs();
    }

    // 8. Remove asyncContext from the execution context stack and restore the execution context that is at the top of the execution context stack as the running execution context.
    // NOTE: Since we don't push any EC, this step is not performed.

    // 9. Set the code evaluation state of asyncContext such that when evaluation is resumed with a Completion Record completion, the following steps of the algorithm that invoked Await will be performed, with completion available.
    // 10. Return NormalCompletion(unused).
    // 11. NOTE: This returns to the evaluation of the operation that had most previously resumed evaluation of asyncContext.

    // Make sure that the promise _actually_ resolved.
    // Note that this is checked down the chain (result.is_empty()) anyway, but let's make the source of the issue more clear.
    let success = awaited_completion.success.get().expect("the awaited promise settled");

    let result = awaited_completion.result.get();
    if success {
        return Ok(result);
    }
    crate::embedding::completion::log_exception_if_enabled(vm, result);
    Err(Throw::new(result))
}

impl From<Throw> for Completion {
    fn from(throw: Throw) -> Self {
        Self::new(CompletionType::Throw, throw.value())
    }
}

impl From<ThrowCompletionOr<Value>> for Completion {
    fn from(result: ThrowCompletionOr<Value>) -> Self {
        match result {
            Ok(value) => Self::normal(value),
            Err(throw) => throw.into(),
        }
    }
}
