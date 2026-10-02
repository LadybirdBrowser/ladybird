/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::heap_function::create_heap_function;
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    call, call_function_object, ordinary_create_from_constructor_of, species_constructor,
};
use crate::runtime::aggregate_error::AggregateError;
use crate::runtime::completion::{Must, Throw, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::iterator::{IteratorHint, IteratorRecord, get_iterator, iterator_close, iterator_step_value};
use crate::runtime::job_callback::JobCallback;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::promise::{Promise, PromiseState, RejectionOperation, promise_resolve};
use crate::runtime::promise_capability::{PromiseCapability, new_promise_capability, try_or_reject};
use crate::runtime::promise_prototype::PromisePrototype;
use crate::runtime::promise_resolving_element_functions::{
    PromiseAllResolveElementFunction, PromiseAllSettledRejectElementFunction, PromiseAllSettledResolveElementFunction,
    PromiseAnyRejectElementFunction, PromiseValueList, RemainingElements,
};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

// 27.2.4.1.1 GetPromiseResolve ( promiseConstructor ), https://tc39.es/ecma262/#sec-getpromiseresolve
fn get_promise_resolve(vm: &Vm, constructor: Value) -> ThrowCompletionOr<Value> {
    assert!(constructor.is_constructor());

    // 1. Let promiseResolve be ? Get(promiseConstructor, "resolve").
    let promise_resolve = constructor.get(vm, &vm.names.resolve)?;

    // 2. If IsCallable(promiseResolve) is false, throw a TypeError exception.
    if !promise_resolve.is_function() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&promise_resolve]);
    }

    // 3. Return promiseResolve.
    Ok(promise_resolve)
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ResultVisibility {
    Observable,
    Unobservable,
}

fn result_visibility_of(vm: &Vm, realm: Gc<Realm>, constructor: Value) -> ResultVisibility {
    if constructor.as_function() == realm.intrinsics().promise_constructor(vm).upcast() {
        ResultVisibility::Unobservable
    } else {
        ResultVisibility::Observable
    }
}

fn invoke_then(
    vm: &Vm,
    realm: Gc<Realm>,
    promise_value: Value,
    on_fulfilled: Value,
    on_rejected: Value,
    result_visibility: ResultVisibility,
) -> ThrowCompletionOr<Value> {
    let then = promise_value.get(vm, &vm.names.then)?;

    // OPTIMIZATION: Promise combinators do not use the result of invoking the intrinsic Promise.prototype.then. If its
    //               species is also the intrinsic Promise, omit the otherwise unused promise and resolving functions.
    let promise = promise_value
        .is_object()
        .then(|| promise_value.as_object().downcast::<Promise>())
        .flatten();
    if result_visibility == ResultVisibility::Unobservable
        && let Some(promise) = promise
        && PromisePrototype::is_original_then_function(vm, then)
        && then.as_function().realm() == Some(realm)
    {
        let promise_constructor = realm.intrinsics().promise_constructor(vm).upcast();
        let constructor = species_constructor(vm, &promise, promise_constructor)?;
        if constructor == promise_constructor {
            return Ok(promise.perform_then(vm, on_fulfilled, on_rejected, None));
        }

        let result_capability = new_promise_capability(vm, Value::from_object(constructor))?;
        return Ok(promise.perform_then(vm, on_fulfilled, on_rejected, Some(result_capability)));
    }

    call(vm, then, promise_value, &[on_fulfilled, on_rejected])
}

fn run_promise_all_resolve_element_job(
    vm: &Vm,
    values: Gc<PromiseValueList>,
    resolve_values_array: Gc<JobCallback>,
    remaining_elements_count: Gc<RemainingElements>,
    index: usize,
    value: Value,
) -> ThrowCompletionOr<Value> {
    // 5. Set values[thisIndex] to value.
    values.set(index, value);

    // 6. Set remainingElementsCount.[[Value]] to remainingElementsCount.[[Value]] - 1.
    // 7. If remainingElementsCount.[[Value]] = 0, then
    if remaining_elements_count.decrement() == 0 {
        // a. Let valuesArray be CreateArrayFromList(values).
        let values_array = values.create_array(vm, vm.current_realm().expect("there is a current realm"));

        // b. Return ? Call(resultCapability.[[Resolve]], undefined, « valuesArray »).
        return vm.host_call_job_callback()(
            vm,
            resolve_values_array,
            Value::UNDEFINED,
            &[Value::from_object(values_array)],
        );
    }

    // 8. Return undefined.
    Ok(Value::UNDEFINED)
}

#[allow(clippy::too_many_arguments)]
fn invoke_promise_all_then(
    vm: &Vm,
    realm: Gc<Realm>,
    promise_value: Value,
    values: Gc<PromiseValueList>,
    result_capability: Gc<PromiseCapability>,
    remaining_elements_count: Gc<RemainingElements>,
    index: usize,
    result_visibility: ResultVisibility,
    resolve_values_array: &Cell<Option<Gc<JobCallback>>>,
) -> ThrowCompletionOr<Value> {
    // OPTIMIZATION: Share the host callback used to resolve the final values array across all internal reaction jobs.
    if result_visibility == ResultVisibility::Unobservable && resolve_values_array.get().is_none() {
        let resolve_values_array_function = NativeFunction::create_anonymous(
            vm,
            result_capability,
            |vm, &result_capability: &Gc<PromiseCapability>| {
                call_function_object(vm, result_capability.resolve(), Value::UNDEFINED, &[vm.argument(0)])
            },
            1,
        );
        resolve_values_array.set(Some(vm.host_make_job_callback()(
            vm,
            resolve_values_array_function.upcast(),
        )));
    }

    let then = promise_value.get(vm, &vm.names.then)?;

    let create_on_fulfilled = || {
        Value::from_object(PromiseAllResolveElementFunction::create(
            vm,
            realm,
            index,
            values,
            result_capability,
            remaining_elements_count,
        ))
    };

    // OPTIMIZATION: Queue the intrinsic Promise.all reaction directly for an already-settled promise, avoiding the
    //               otherwise unobservable per-element resolve function, JobCallback, and PromiseReaction allocations.
    let promise = promise_value
        .is_object()
        .then(|| promise_value.as_object().downcast::<Promise>())
        .flatten();
    if result_visibility == ResultVisibility::Unobservable
        && let Some(promise) = promise
        && PromisePrototype::is_original_then_function(vm, then)
        && then.as_function().realm() == Some(realm)
    {
        let promise_constructor = realm.intrinsics().promise_constructor(vm).upcast();
        let constructor = species_constructor(vm, &promise, promise_constructor)?;
        if constructor == promise_constructor {
            if promise.state() == PromiseState::Fulfilled {
                let value = promise.result();
                let resolve_values_array = resolve_values_array
                    .get()
                    .expect("the resolve values array callback exists for an unobservable result");
                let job = create_heap_function(
                    vm,
                    (values, resolve_values_array, remaining_elements_count, index, value),
                    |vm, &(values, resolve_values_array, remaining_elements_count, index, value)| {
                        run_promise_all_resolve_element_job(
                            vm,
                            values,
                            resolve_values_array,
                            remaining_elements_count,
                            index,
                            value,
                        )
                    },
                );
                vm.host_enqueue_promise_job()(vm, job, Some(realm));
                promise.set_is_handled();
                return Ok(Value::UNDEFINED);
            }

            if promise.state() == PromiseState::Rejected {
                let reason = promise.result();
                if !promise.is_handled() {
                    vm.host_promise_rejection_tracker()(vm, promise, RejectionOperation::Handle);
                }

                let reject = result_capability.reject();
                let job = create_heap_function(vm, (reject, reason), |vm, &(reject, reason)| {
                    call_function_object(vm, reject, Value::UNDEFINED, &[reason])
                });
                vm.host_enqueue_promise_job()(vm, job, Some(realm));
                promise.set_is_handled();
                return Ok(Value::UNDEFINED);
            }

            return Ok(promise.perform_then(
                vm,
                create_on_fulfilled(),
                Value::from_object(result_capability.reject()),
                None,
            ));
        }

        let dependent_capability = new_promise_capability(vm, Value::from_object(constructor))?;
        return Ok(promise.perform_then(
            vm,
            create_on_fulfilled(),
            Value::from_object(result_capability.reject()),
            Some(dependent_capability),
        ));
    }

    call(
        vm,
        then,
        promise_value,
        &[create_on_fulfilled(), Value::from_object(result_capability.reject())],
    )
}

fn perform_promise_common(
    vm: &Vm,
    iterator_record: Gc<IteratorRecord>,
    constructor: Value,
    result_capability: Gc<PromiseCapability>,
    promise_resolve: Value,
    end_of_list: impl FnOnce(Gc<PromiseValueList>) -> ThrowCompletionOr<Value>,
    mut invoke_element_function: impl FnMut(
        Gc<PromiseValueList>,
        Gc<RemainingElements>,
        Value,
        usize,
    ) -> ThrowCompletionOr<Value>,
) -> ThrowCompletionOr<Value> {
    assert!(constructor.is_constructor());
    assert!(promise_resolve.is_function());

    // 1. Let values be a new empty List.
    let values = PromiseValueList::create(vm);

    // 2. Let remainingElementsCount be the Record { [[Value]]: 1 }.
    let remaining_elements_count = RemainingElements::create(vm, 1);

    // 3. Let index be 0.
    let mut index = 0;

    // 4. Repeat,
    loop {
        // a. Let next be ? IteratorStepValue(iteratorRecord).
        let next = iterator_step_value(vm, &iterator_record)?;

        // b. If next is DONE, then
        let Some(next) = next else {
            // i. Set remainingElementsCount.[[Value]] to remainingElementsCount.[[Value]] - 1.
            // ii. If remainingElementsCount.[[Value]] = 0, then
            if remaining_elements_count.decrement() == 0 {
                // 1-2. are handled in `end_of_list`
                return end_of_list(values);
            }

            // iii. Return resultCapability.[[Promise]].
            return Ok(Value::from_object(result_capability.promise()));
        };

        // c. Append undefined to values.
        values.append(Value::UNDEFINED);

        // d. Let nextPromise be ? Call(promiseResolve, constructor, « next »).
        let next_promise = call(vm, promise_resolve, constructor, &[next])?;

        // e-l. are handled in `invoke_element_function`

        // m. Set remainingElementsCount.[[Value]] to remainingElementsCount.[[Value]] + 1.
        remaining_elements_count.increment();

        // n. Perform ? Invoke(nextPromise, "then", « ... »).
        invoke_element_function(values, remaining_elements_count, next_promise, index)?;

        // o. Set index to index + 1.
        index += 1;
    }
}

// 27.2.4.1.2 PerformPromiseAll ( iteratorRecord, constructor, resultCapability, promiseResolve ), https://tc39.es/ecma262/#sec-performpromiseall
fn perform_promise_all(
    vm: &Vm,
    iterator_record: Gc<IteratorRecord>,
    constructor: Value,
    result_capability: Gc<PromiseCapability>,
    promise_resolve: Value,
) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("there is a current realm");
    let result_visibility = result_visibility_of(vm, realm, constructor);
    let resolve_values_array: Cell<Option<Gc<JobCallback>>> = Cell::new(None);

    perform_promise_common(
        vm,
        iterator_record,
        constructor,
        result_capability,
        promise_resolve,
        |values| {
            // 1. Let valuesArray be CreateArrayFromList(values).
            let values_array = values.create_array(vm, realm);

            // 2. Perform ? Call(resultCapability.[[Resolve]], undefined, « valuesArray »).
            call_function_object(
                vm,
                result_capability.resolve(),
                Value::UNDEFINED,
                &[Value::from_object(values_array)],
            )?;

            // iv. Return resultCapability.[[Promise]].
            Ok(Value::from_object(result_capability.promise()))
        },
        |values, remaining_elements_count, next_promise, index| {
            invoke_promise_all_then(
                vm,
                realm,
                next_promise,
                values,
                result_capability,
                remaining_elements_count,
                index,
                result_visibility,
                &resolve_values_array,
            )
        },
    )
}

// 27.2.4.2.1 PerformPromiseAllSettled ( iteratorRecord, constructor, resultCapability, promiseResolve ), https://tc39.es/ecma262/#sec-performpromiseallsettled
fn perform_promise_all_settled(
    vm: &Vm,
    iterator_record: Gc<IteratorRecord>,
    constructor: Value,
    result_capability: Gc<PromiseCapability>,
    promise_resolve: Value,
) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("there is a current realm");
    let result_visibility = result_visibility_of(vm, realm, constructor);

    perform_promise_common(
        vm,
        iterator_record,
        constructor,
        result_capability,
        promise_resolve,
        |values| {
            let values_array = values.create_array(vm, realm);

            call_function_object(
                vm,
                result_capability.resolve(),
                Value::UNDEFINED,
                &[Value::from_object(values_array)],
            )?;

            Ok(Value::from_object(result_capability.promise()))
        },
        |values, remaining_elements_count, next_promise, index| {
            // j. Let stepsFulfilled be the algorithm steps defined in Promise.allSettled Resolve Element Functions.
            // k. Let lengthFulfilled be the number of non-optional parameters of the function definition in Promise.allSettled Resolve Element Functions.
            // l. Let onFulfilled be CreateBuiltinFunction(stepsFulfilled, lengthFulfilled, "", « [[AlreadyCalled]], [[Index]], [[Values]], [[Capability]], [[RemainingElements]] »).
            // m. Let alreadyCalled be the Record { [[Value]]: false }.
            // n. Set onFulfilled.[[AlreadyCalled]] to alreadyCalled.
            // o. Set onFulfilled.[[Index]] to index.
            // p. Set onFulfilled.[[Values]] to values.
            // q. Set onFulfilled.[[Capability]] to resultCapability.
            // r. Set onFulfilled.[[RemainingElements]] to remainingElementsCount.
            let on_fulfilled = PromiseAllSettledResolveElementFunction::create(
                vm,
                realm,
                index,
                values,
                result_capability,
                remaining_elements_count,
            );

            // s. Let stepsRejected be the algorithm steps defined in Promise.allSettled Reject Element Functions.
            // t. Let lengthRejected be the number of non-optional parameters of the function definition in Promise.allSettled Reject Element Functions.
            // u. Let onRejected be CreateBuiltinFunction(stepsRejected, lengthRejected, "", « [[AlreadyCalled]], [[Index]], [[Values]], [[Capability]], [[RemainingElements]] »).
            // v. Set onRejected.[[AlreadyCalled]] to alreadyCalled.
            // w. Set onRejected.[[Index]] to index.
            // x. Set onRejected.[[Values]] to values.
            // y. Set onRejected.[[Capability]] to resultCapability.
            // z. Set onRejected.[[RemainingElements]] to remainingElementsCount.
            let on_rejected = PromiseAllSettledRejectElementFunction::create(
                vm,
                realm,
                index,
                values,
                result_capability,
                remaining_elements_count,
            );

            // ab. Perform ? Invoke(nextPromise, "then", « onFulfilled, onRejected »).
            invoke_then(
                vm,
                realm,
                next_promise,
                Value::from_object(on_fulfilled),
                Value::from_object(on_rejected),
                result_visibility,
            )
        },
    )
}

// 27.2.4.3.1 PerformPromiseAny ( iteratorRecord, constructor, resultCapability, promiseResolve ), https://tc39.es/ecma262/#sec-performpromiseany
fn perform_promise_any(
    vm: &Vm,
    iterator_record: Gc<IteratorRecord>,
    constructor: Value,
    result_capability: Gc<PromiseCapability>,
    promise_resolve: Value,
) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("there is a current realm");
    let result_visibility = result_visibility_of(vm, realm, constructor);

    perform_promise_common(
        vm,
        iterator_record,
        constructor,
        result_capability,
        promise_resolve,
        |errors| {
            // 1. Let error be a newly created AggregateError object.
            let error = AggregateError::create(vm, realm);

            // 2. Perform ! DefinePropertyOrThrow(error, "errors", PropertyDescriptor { [[Configurable]]: true, [[Enumerable]]: false, [[Writable]]: true, [[Value]]: CreateArrayFromList(errors) }).
            let errors_array = errors.create_array(vm, realm);
            let mut descriptor = PropertyDescriptor {
                value: Some(Value::from_object(errors_array)),
                writable: Some(true),
                enumerable: Some(false),
                configurable: Some(true),
                ..Default::default()
            };
            error
                .define_property_or_throw(vm, &vm.names.errors, &mut descriptor)
                .must();

            // 3. Return ThrowCompletion(error).
            Err(Throw::new(Value::from_object(error)))
        },
        |errors, remaining_elements_count, next_promise, index| {
            // j. Let stepsRejected be the algorithm steps defined in Promise.any Reject Element Functions.
            // k. Let lengthRejected be the number of non-optional parameters of the function definition in Promise.any Reject Element Functions.
            // l. Let onRejected be CreateBuiltinFunction(stepsRejected, lengthRejected, "", « [[AlreadyCalled]], [[Index]], [[Errors]], [[Capability]], [[RemainingElements]] »).
            // m. Set onRejected.[[AlreadyCalled]] to false.
            // n. Set onRejected.[[Index]] to index.
            // o. Set onRejected.[[Errors]] to errors.
            // p. Set onRejected.[[Capability]] to resultCapability.
            // q. Set onRejected.[[RemainingElements]] to remainingElementsCount.
            let on_rejected = PromiseAnyRejectElementFunction::create(
                vm,
                realm,
                index,
                errors,
                result_capability,
                remaining_elements_count,
            );

            // s. Perform ? Invoke(nextPromise, "then", « resultCapability.[[Resolve]], onRejected »).
            invoke_then(
                vm,
                realm,
                next_promise,
                Value::from_object(result_capability.resolve()),
                Value::from_object(on_rejected),
                result_visibility,
            )
        },
    )
}

// 27.2.4.5.1 PerformPromiseRace ( iteratorRecord, constructor, resultCapability, promiseResolve ), https://tc39.es/ecma262/#sec-performpromiserace
fn perform_promise_race(
    vm: &Vm,
    iterator_record: Gc<IteratorRecord>,
    constructor: Value,
    result_capability: Gc<PromiseCapability>,
    promise_resolve: Value,
) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("there is a current realm");
    let result_visibility = result_visibility_of(vm, realm, constructor);

    perform_promise_common(
        vm,
        iterator_record,
        constructor,
        result_capability,
        promise_resolve,
        |_| {
            // ii. Return resultCapability.[[Promise]].
            Ok(Value::from_object(result_capability.promise()))
        },
        |_, _, next_promise, _| {
            // i. Perform ? Invoke(nextPromise, "then", « resultCapability.[[Resolve]], resultCapability.[[Reject]] »).
            invoke_then(
                vm,
                realm,
                next_promise,
                Value::from_object(result_capability.resolve()),
                Value::from_object(result_capability.reject()),
                result_visibility,
            )
        },
    )
}

/// The perform_promise_* function a combinator runs on the iterator of its argument.
type PerformPromiseCombinator =
    fn(&Vm, Gc<IteratorRecord>, Value, Gc<PromiseCapability>, Value) -> ThrowCompletionOr<Value>;

/// Steps 1 to 9 of Promise.all, Promise.allSettled, Promise.any and Promise.race, which only differ in the operation
/// they perform.
fn promise_combinator(vm: &Vm, perform: PerformPromiseCombinator) -> ThrowCompletionOr<Value> {
    // 1. Let C be the this value.
    let constructor = Value::from_object(vm.this_value().to_object(vm)?);

    // 2. Let promiseCapability be ? NewPromiseCapability(C).
    let promise_capability = new_promise_capability(vm, constructor)?;

    // 3. Let promiseResolve be Completion(GetPromiseResolve(C)).
    // 4. IfAbruptRejectPromise(promiseResolve, promiseCapability).
    let promise_resolve = try_or_reject!(vm, promise_capability, get_promise_resolve(vm, constructor));

    // 5. Let iteratorRecord be Completion(GetIterator(iterable, sync)).
    // 6. IfAbruptRejectPromise(iteratorRecord, promiseCapability).
    let iterator_record = try_or_reject!(
        vm,
        promise_capability,
        get_iterator(vm, vm.argument(0), IteratorHint::Sync)
    );

    // 7. Let result be Completion(PerformPromiseAll(iteratorRecord, C, promiseCapability, promiseResolve)).
    let mut result = perform(vm, iterator_record, constructor, promise_capability, promise_resolve);

    // 8. If result is an abrupt completion, then
    if let Err(throw) = result {
        // a. If iteratorRecord.[[Done]] is false, set result to Completion(IteratorClose(iteratorRecord, result)).
        if !iterator_record.done() {
            result = iterator_close(vm, &iterator_record, throw.into()).into_throw_completion_or();
        }

        // b. IfAbruptRejectPromise(result, promiseCapability).
        try_or_reject!(vm, promise_capability, result);
    }

    // 9. Return ? result.
    result
}

#[repr(C)]
#[derive(Trace)]
pub struct PromiseConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    PromiseConstructor,
    initialize: PromiseConstructor::initialize,
    call: PromiseConstructor::call,
    construct: PromiseConstructor::construct
);

impl PromiseConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PromiseConstructor> {
        realm.create_object(
            vm,
            PromiseConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Promise.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 27.2.4.4 Promise.prototype, https://tc39.es/ecma262/#sec-promise.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().promise_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.all,
            raw_native!(PromiseConstructor::all),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.allSettled,
            raw_native!(PromiseConstructor::all_settled),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.any,
            raw_native!(PromiseConstructor::any),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.race,
            raw_native!(PromiseConstructor::race),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.reject,
            raw_native!(PromiseConstructor::reject),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.resolve,
            raw_native!(PromiseConstructor::resolve),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.try_,
            raw_native!(PromiseConstructor::try_),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.withResolvers,
            raw_native!(PromiseConstructor::with_resolvers),
            0,
            attr,
            None,
        );

        object.define_native_accessor(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().species),
            raw_native!(PromiseConstructor::symbol_species_getter),
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 27.2.3.1 Promise ( executor ), https://tc39.es/ecma262/#sec-promise-executor
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(ErrorKind::TypeError, ErrorType::ConstructorWithoutNew, &[&"Promise"])
    }

    // 27.2.3.1 Promise ( executor ), https://tc39.es/ecma262/#sec-promise-executor
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("there is a current realm");

        let executor = vm.argument(0);

        // 2. If IsCallable(executor) is false, throw a TypeError exception.
        if !executor.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::PromiseExecutorNotAFunction, &[]);
        }

        // 3. Let promise be ? OrdinaryCreateFromConstructor(NewTarget, "%Promise.prototype%", « [[PromiseState]], [[PromiseResult]], [[PromiseFulfillReactions]], [[PromiseRejectReactions]], [[PromiseIsHandled]] »).
        // 4. Set promise.[[PromiseState]] to pending.
        // 5. Set promise.[[PromiseFulfillReactions]] to a new empty List.
        // 6. Set promise.[[PromiseRejectReactions]] to a new empty List.
        // 7. Set promise.[[PromiseIsHandled]] to false.
        let promise = ordinary_create_from_constructor_of(
            vm,
            realm,
            new_target,
            |intrinsics, vm| intrinsics.promise_prototype(vm),
            |prototype| Promise::new(vm, Promise::CLASS, prototype),
        )?;

        // 8. Let resolvingFunctions be CreateResolvingFunctions(promise).
        let resolving_functions = promise.create_resolving_functions(vm);

        // 9. Let completion be Completion(Call(executor, undefined, « resolvingFunctions.[[Resolve]], resolvingFunctions.[[Reject]] »)).
        let completion = call_function_object(
            vm,
            executor.as_function(),
            Value::UNDEFINED,
            &[
                Value::from_object(resolving_functions.resolve),
                Value::from_object(resolving_functions.reject),
            ],
        );

        // 10. If completion is an abrupt completion, then
        if let Err(throw) = completion {
            // a. Perform ? Call(resolvingFunctions.[[Reject]], undefined, « completion.[[Value]] »).
            call_function_object(vm, resolving_functions.reject, Value::UNDEFINED, &[throw.value()])?;
        }

        // 11. Return promise.
        Ok(promise.upcast())
    }

    // 27.2.4.1 Promise.all ( iterable ), https://tc39.es/ecma262/#sec-promise.all
    fn all(vm: &Vm) -> ThrowCompletionOr<Value> {
        promise_combinator(vm, perform_promise_all)
    }

    // 27.2.4.2 Promise.allSettled ( iterable ), https://tc39.es/ecma262/#sec-promise.allsettled
    fn all_settled(vm: &Vm) -> ThrowCompletionOr<Value> {
        promise_combinator(vm, perform_promise_all_settled)
    }

    // 27.2.4.3 Promise.any ( iterable ), https://tc39.es/ecma262/#sec-promise.any
    fn any(vm: &Vm) -> ThrowCompletionOr<Value> {
        promise_combinator(vm, perform_promise_any)
    }

    // 27.2.4.5 Promise.race ( iterable ), https://tc39.es/ecma262/#sec-promise.race
    fn race(vm: &Vm) -> ThrowCompletionOr<Value> {
        promise_combinator(vm, perform_promise_race)
    }

    // 27.2.4.6 Promise.reject ( r ), https://tc39.es/ecma262/#sec-promise.reject
    fn reject(vm: &Vm) -> ThrowCompletionOr<Value> {
        let reason = vm.argument(0);

        // 1. Let C be the this value.
        let constructor = vm.this_value().to_object(vm)?;

        // 2. Let promiseCapability be ? NewPromiseCapability(C).
        let promise_capability = new_promise_capability(vm, Value::from_object(constructor))?;

        // 3. Perform ? Call(promiseCapability.[[Reject]], undefined, « r »).
        call_function_object(vm, promise_capability.reject(), Value::UNDEFINED, &[reason])?;

        // 4. Return promiseCapability.[[Promise]].
        Ok(Value::from_object(promise_capability.promise()))
    }

    // 27.2.4.7 Promise.resolve ( x ), https://tc39.es/ecma262/#sec-promise.resolve
    fn resolve(vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 1. Let C be the this value.
        let constructor = vm.this_value();

        // 2. If Type(C) is not Object, throw a TypeError exception.
        if !constructor.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&constructor]);
        }

        // 3. Return ? PromiseResolve(C, x).
        Ok(Value::from_object(promise_resolve(vm, constructor.as_object(), value)?))
    }

    // 27.2.4.8 Promise.try ( callback, ...args ), https://tc39.es/ecma262/#sec-promise.try
    fn try_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let callback = vm.argument(0);
        let args = MarkedVec::new(vm);
        for index in 1..vm.argument_count() {
            args.push(vm.argument(index));
        }

        // 1. Let C be the this value.
        let constructor = vm.this_value();

        // 2. If C is not an Object, throw a TypeError exception.
        if !constructor.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&constructor]);
        }

        // 3. Let promiseCapability be ? NewPromiseCapability(C).
        let promise_capability = new_promise_capability(vm, constructor)?;

        // 4. Let status be Completion(Call(callback, undefined, args)).
        let status = call(vm, callback, Value::UNDEFINED, &args.to_vec());

        match status {
            // 5. If status is an abrupt completion, then
            Err(throw) => {
                // a. Perform ? Call(promiseCapability.[[Reject]], undefined, « status.[[Value]] »).
                call_function_object(vm, promise_capability.reject(), Value::UNDEFINED, &[throw.value()])?;
            }
            // 6. Else,
            Ok(value) => {
                // a. Perform ? Call(promiseCapability.[[Resolve]], undefined, « status.[[Value]] »).
                call_function_object(vm, promise_capability.resolve(), Value::UNDEFINED, &[value])?;
            }
        }

        // 7. Return promiseCapability.[[Promise]].
        Ok(Value::from_object(promise_capability.promise()))
    }

    // 27.2.4.9 Promise.withResolvers ( ), https://tc39.es/ecma262/#sec-promise.withResolvers
    fn with_resolvers(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("there is a current realm");

        // 1. Let C be the this value.
        let constructor = vm.this_value();

        // 2. Let promiseCapability be ? NewPromiseCapability(C).
        let promise_capability = new_promise_capability(vm, constructor)?;

        // 3. Let obj be OrdinaryObjectCreate(%Object.prototype%).
        let object = Object::create(vm, realm, Some(realm.intrinsics().object_prototype(vm)));

        // 4. Perform ! CreateDataPropertyOrThrow(obj, "promise", promiseCapability.[[Promise]]).
        object
            .create_data_property_or_throw(vm, &vm.names.promise, Value::from_object(promise_capability.promise()))
            .must();

        // 5. Perform ! CreateDataPropertyOrThrow(obj, "resolve", promiseCapability.[[Resolve]]).
        object
            .create_data_property_or_throw(vm, &vm.names.resolve, Value::from_object(promise_capability.resolve()))
            .must();

        // 6. Perform ! CreateDataPropertyOrThrow(obj, "reject", promiseCapability.[[Reject]]).
        object
            .create_data_property_or_throw(vm, &vm.names.reject, Value::from_object(promise_capability.reject()))
            .must();

        // 7. Return obj.
        Ok(Value::from_object(object))
    }

    // 27.2.4.10 get Promise [ @@species ], https://tc39.es/ecma262/#sec-get-promise-@@species
    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn symbol_species_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return the this value.
        Ok(vm.this_value())
    }
}
