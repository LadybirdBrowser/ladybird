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
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{construct, ordinary_create_from_constructor_of};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::NativeFunction;
use crate::runtime::promise::Promise;

// 27.2.1.1 PromiseCapability Records, https://tc39.es/ecma262/#sec-promisecapability-records
#[repr(C)]
#[derive(Trace)]
pub struct PromiseCapability {
    header: CellHeader,
    promise: Cell<Gc<Object>>,
    resolve: Cell<Gc<FunctionObject>>,
    reject: Cell<Gc<FunctionObject>>,
}

define_cell!(PromiseCapability, Other);

impl PromiseCapability {
    pub fn create(
        vm: &Vm,
        promise: Gc<Object>,
        resolve: Gc<FunctionObject>,
        reject: Gc<FunctionObject>,
    ) -> Gc<PromiseCapability> {
        vm.heap().allocate(PromiseCapability {
            header: CellHeader::for_class(Self::CLASS),
            promise: Cell::new(promise),
            resolve: Cell::new(resolve),
            reject: Cell::new(reject),
        })
    }

    pub fn promise(&self) -> Gc<Object> {
        self.promise.get()
    }

    pub fn resolve(&self) -> Gc<FunctionObject> {
        self.resolve.get()
    }

    pub fn reject(&self) -> Gc<FunctionObject> {
        self.reject.get()
    }
}

// 27.2.1.1.1 IfAbruptRejectPromise ( value, capability ), https://tc39.es/ecma262/#sec-ifabruptrejectpromise
/// TRY_OR_REJECT(vm, capability, expression), for a function that returns ThrowCompletionOr<Value>.
macro_rules! try_or_reject {
    ($vm:expr, $capability:expr, $expression:expr) => {
        match $expression {
            Ok(value) => value,
            // 1. If value is an abrupt completion, then
            Err(throw) => {
                let throw: $crate::runtime::completion::Throw = throw;
                // a. Perform ? Call(capability.[[Reject]], undefined, « value.[[Value]] »).
                $crate::runtime::abstract_operations::call_function_object(
                    $vm,
                    $capability.reject(),
                    $crate::layout::value::Value::UNDEFINED,
                    &[throw.value()],
                )?;

                // b. Return capability.[[Promise]].
                return Ok($crate::layout::value::Value::from_object($capability.promise()));
            }
        }
        // 2. Else if value is a Completion Record, set value to value.[[Value]].
    };
}

// 27.2.1.1.1 IfAbruptRejectPromise ( value, capability ), https://tc39.es/ecma262/#sec-ifabruptrejectpromise
/// TRY_OR_MUST_REJECT(vm, capability, expression), for a function that returns the capability's promise, and whose
/// capability's reject function cannot throw.
macro_rules! try_or_must_reject {
    ($vm:expr, $capability:expr, $expression:expr) => {
        match $expression {
            Ok(value) => value,
            // 1. If value is an abrupt completion, then
            Err(throw) => {
                let throw: $crate::runtime::completion::Throw = throw;
                // a. Perform ? Call(capability.[[Reject]], undefined, « value.[[Value]] »).
                $crate::runtime::completion::Must::must($crate::runtime::abstract_operations::call_function_object(
                    $vm,
                    $capability.reject(),
                    $crate::layout::value::Value::UNDEFINED,
                    &[throw.value()],
                ));

                // b. Return capability.[[Promise]].
                return $capability.promise();
            }
        }
        // 2. Else if value is a Completion Record, set value to value.[[Value]].
    };
}

pub(crate) use {try_or_must_reject, try_or_reject};

/// The Record { [[Resolve]], [[Reject]] } the executor of NewPromiseCapability fills in.
#[repr(C)]
#[derive(Trace)]
struct ResolvingFunctions {
    header: CellHeader,
    resolve: Cell<Value>,
    reject: Cell<Value>,
}

define_cell!(ResolvingFunctions, Other);

// 27.2.1.5 NewPromiseCapability ( C ), https://tc39.es/ecma262/#sec-newpromisecapability
pub fn new_promise_capability(vm: &Vm, constructor: Value) -> ThrowCompletionOr<Gc<PromiseCapability>> {
    let realm = vm.current_realm().expect("there is a current realm");

    // 1. If IsConstructor(C) is false, throw a TypeError exception.
    if !constructor.is_constructor() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAConstructor, &[&constructor]);
    }

    // 2. NOTE: C is assumed to be a constructor function that supports the parameter conventions of the Promise constructor (see 27.2.3.1).

    // OPTIMIZATION: Constructing the intrinsic Promise only creates a Promise and its resolving functions before invoking
    //               the executor below. Create the resulting capability directly to avoid the temporary executor closure.
    if constructor.as_function() == realm.intrinsics().promise_constructor(vm).upcast() {
        let promise = ordinary_create_from_constructor_of(
            vm,
            realm,
            constructor.as_function(),
            |intrinsics, vm| intrinsics.promise_prototype(vm),
            |prototype| Promise::new(vm, Promise::CLASS, prototype),
        )?;
        let resolving_functions = promise.create_resolving_functions(vm);
        return Ok(PromiseCapability::create(
            vm,
            promise.upcast(),
            resolving_functions.resolve,
            resolving_functions.reject,
        ));
    }

    // 3. Let resolvingFunctions be the Record { [[Resolve]]: undefined, [[Reject]]: undefined }.
    let resolving_functions = vm.heap().allocate(ResolvingFunctions {
        header: CellHeader::for_class(ResolvingFunctions::CLASS),
        resolve: Cell::new(Value::UNDEFINED),
        reject: Cell::new(Value::UNDEFINED),
    });

    // 4. Let executorClosure be a new Abstract Closure with parameters (resolve, reject) that captures resolvingFunctions and performs the following steps when called:
    // 5. Let executor be CreateBuiltinFunction(executorClosure, 2, "", « »).
    let executor = NativeFunction::create_anonymous(
        vm,
        resolving_functions,
        |vm, resolving_functions| {
            let resolve = vm.argument(0);
            let reject = vm.argument(1);

            // No idea what other engines say here.
            // a. If promiseCapability.[[Resolve]] is not undefined, throw a TypeError exception.
            if !resolving_functions.resolve.get().is_undefined() {
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::GetCapabilitiesExecutorCalledMultipleTimes,
                    &[],
                );
            }

            // b. If promiseCapability.[[Reject]] is not undefined, throw a TypeError exception.
            if !resolving_functions.reject.get().is_undefined() {
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::GetCapabilitiesExecutorCalledMultipleTimes,
                    &[],
                );
            }

            // c. Set promiseCapability.[[Resolve]] to resolve.
            resolving_functions.resolve.set(resolve);

            // d. Set promiseCapability.[[Reject]] to reject.
            resolving_functions.reject.set(reject);

            // e. Return undefined.
            Ok(Value::UNDEFINED)
        },
        2,
    );

    // 6. Let promise be ? Construct(C, « executor »).
    let promise = construct(vm, constructor.as_function(), &[Value::from_object(executor)], None)?;

    //  7. If IsCallable(resolvingFunctions.[[Resolve]]) is false, throw a TypeError exception.
    let resolve = resolving_functions.resolve.get();
    if !resolve.is_function() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::NotAFunction,
            &[&"Promise capability resolve value"],
        );
    }

    // 8. If IsCallable(resolvingFunctions.[[Reject]]) is false, throw a TypeError exception.
    let reject = resolving_functions.reject.get();
    if !reject.is_function() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::NotAFunction,
            &[&"Promise capability reject value"],
        );
    }

    // 9. Return the PromiseCapability Record { [[Promise]]: promise, [[Resolve]]: resolvingFunctions.[[Resolve]], [[Reject]]: resolvingFunctions.[[Reject]] }.
    Ok(PromiseCapability::create(
        vm,
        promise,
        resolve.as_function(),
        reject.as_function(),
    ))
}
