/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{call, species_constructor};
use crate::runtime::completion::{Throw, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{NativeFunction, RawNativeFunction, RawNativeFunctionPointer, raw_native};
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::promise::{Promise, promise_resolve};
use crate::runtime::promise_capability::new_promise_capability;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct PromisePrototype {
    base: Object,
}

define_object_class!(PromisePrototype, extends: [Object], methods: {
    initialize: PromisePrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

const DISPLAY_NAME: &str = "Promise";

/// The behaviour of Promise.prototype.then, which is_original_then_function() recognizes.
fn then_native_function() -> RawNativeFunctionPointer {
    raw_native!(PromisePrototype::then)
}

impl PromisePrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PromisePrototype> {
        realm.create_object(
            vm,
            PromisePrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().object_prototype(vm),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    pub fn is_original_then_function(vm: &Vm, value: Value) -> bool {
        if !value.is_object() {
            return false;
        }
        let Some(function) = value.as_object().downcast::<RawNativeFunction>() else {
            return false;
        };
        let (Some(native_function), Some(then)) = (function.native_function(vm), then_native_function()) else {
            return false;
        };
        core::ptr::fn_addr_eq(native_function, then)
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(vm, realm, &names.then, then_native_function(), 2, attr, None);
        object.define_native_function(
            vm,
            realm,
            &names.catch_,
            raw_native!(PromisePrototype::catch_),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.finally,
            raw_native!(PromisePrototype::finally),
            1,
            attr,
            None,
        );

        // 27.2.5.5 Promise.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-promise.prototype-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(vm, names.Promise.as_string())),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 27.2.5.4 Promise.prototype.then ( onFulfilled, onRejected ), https://tc39.es/ecma262/#sec-promise.prototype.then
    fn then(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("there is a current realm");

        let on_fulfilled = vm.argument(0);
        let on_rejected = vm.argument(1);

        // 1. Let promise be the this value.
        // 2. If IsPromise(promise) is false, throw a TypeError exception.
        let promise = typed_this_object::<Promise>(vm, DISPLAY_NAME)?;

        // 3. Let C be ? SpeciesConstructor(promise, %Promise%).
        let constructor = species_constructor(vm, &promise, realm.intrinsics().promise_constructor(vm).upcast())?;

        // 4. Let resultCapability be ? NewPromiseCapability(C).
        let result_capability = new_promise_capability(vm, Value::from_object(constructor))?;

        // 5. Return PerformPromiseThen(promise, onFulfilled, onRejected, resultCapability).
        Ok(promise.perform_then(vm, on_fulfilled, on_rejected, Some(result_capability)))
    }

    // 27.2.5.1 Promise.prototype.catch ( onRejected ), https://tc39.es/ecma262/#sec-promise.prototype.catch
    fn catch_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let on_rejected = vm.argument(0);

        // 1. Let promise be the this value.
        let this_value = vm.this_value();

        // 2. Return ? Invoke(promise, "then", « undefined, onRejected »).
        this_value.invoke(vm, &vm.names.then, &[Value::UNDEFINED, on_rejected])
    }

    // 27.2.5.3 Promise.prototype.finally ( onFinally ), https://tc39.es/ecma262/#sec-promise.prototype.finally
    fn finally(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("there is a current realm");

        let on_finally = vm.argument(0);

        // 1. Let promise be the this value.
        let promise = vm.this_value();

        // 2. If Type(promise) is not Object, throw a TypeError exception.
        if !promise.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&promise]);
        }

        // 3. Let C be ? SpeciesConstructor(promise, %Promise%).
        let constructor = species_constructor(
            vm,
            &promise.as_object(),
            realm.intrinsics().promise_constructor(vm).upcast(),
        )?;

        // 4. Assert: IsConstructor(C) is true.

        // 5. If IsCallable(onFinally) is false, then
        let (then_finally, catch_finally) = if !on_finally.is_function() {
            // a. Let thenFinally be onFinally.
            // b. Let catchFinally be onFinally.
            (on_finally, on_finally)
        }
        // 6. Else,
        else {
            // a. Let thenFinallyClosure be a new Abstract Closure with parameters (value) that captures onFinally and C and performs the following steps when called:
            // b. Let thenFinally be CreateBuiltinFunction(thenFinallyClosure, 1, "", « »).
            let then_finally = NativeFunction::create_anonymous(
                vm,
                (constructor, on_finally),
                |vm, &(constructor, on_finally): &(Gc<FunctionObject>, Value)| {
                    let value = vm.argument(0);

                    // i. Let result be ? Call(onFinally, undefined).
                    let result = call(vm, on_finally, Value::UNDEFINED, &[])?;

                    // ii. Let promise be ? PromiseResolve(C, result).
                    let promise = promise_resolve(vm, constructor.upcast(), result)?;

                    // iii. Let returnValue be a new Abstract Closure with no parameters that captures value and performs the following steps when called:
                    // iv. Let valueThunk be CreateBuiltinFunction(returnValue, 0, "", « »).
                    let value_thunk = NativeFunction::create_anonymous(
                        vm,
                        value,
                        |_, &value: &Value| {
                            // 1. Return value.
                            Ok(value)
                        },
                        0,
                    );

                    // v. Return ? Invoke(promise, "then", « valueThunk »).
                    Value::from_object(promise).invoke(vm, &vm.names.then, &[Value::from_object(value_thunk)])
                },
                1,
            );

            // c. Let catchFinallyClosure be a new Abstract Closure with parameters (reason) that captures onFinally and C and performs the following steps when called:
            // d. Let catchFinally be CreateBuiltinFunction(catchFinallyClosure, 1, "", « »).
            let catch_finally = NativeFunction::create_anonymous(
                vm,
                (constructor, on_finally),
                |vm, &(constructor, on_finally): &(Gc<FunctionObject>, Value)| {
                    let reason = vm.argument(0);

                    // i. Let result be ? Call(onFinally, undefined).
                    let result = call(vm, on_finally, Value::UNDEFINED, &[])?;

                    // ii. Let promise be ? PromiseResolve(C, result).
                    let promise = promise_resolve(vm, constructor.upcast(), result)?;

                    // iii. Let throwReason be a new Abstract Closure with no parameters that captures reason and performs the following steps when called:
                    // iv. Let thrower be CreateBuiltinFunction(throwReason, 0, "", « »).
                    let thrower = NativeFunction::create_anonymous(
                        vm,
                        reason,
                        |_, &reason: &Value| {
                            // 1. Return ThrowCompletion(reason).
                            Err(Throw::new(reason))
                        },
                        0,
                    );

                    // v. Return ? Invoke(promise, "then", « thrower »).
                    Value::from_object(promise).invoke(vm, &vm.names.then, &[Value::from_object(thrower)])
                },
                1,
            );

            (Value::from_object(then_finally), Value::from_object(catch_finally))
        };

        // 7. Return ? Invoke(promise, "then", « thenFinally, catchFinally »).
        promise.invoke(vm, &vm.names.then, &[then_finally, catch_finally])
    }
}
