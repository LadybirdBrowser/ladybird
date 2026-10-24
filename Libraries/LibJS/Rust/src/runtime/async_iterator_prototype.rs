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
use crate::runtime::abstract_operations::call_function_object;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::native_function::{NativeFunction, raw_native};
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::promise::{Promise, promise_resolve};
use crate::runtime::promise_capability::{new_promise_capability, try_or_reject};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

/// %AsyncIteratorPrototype%.
#[repr(C)]
#[derive(Trace)]
pub struct AsyncIteratorPrototype {
    base: Object,
}

define_object_class!(AsyncIteratorPrototype, extends: [Object], methods: {
    initialize: AsyncIteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl AsyncIteratorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AsyncIteratorPrototype> {
        realm.create_object(
            vm,
            AsyncIteratorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().async_iterator),
            raw_native!(AsyncIteratorPrototype::symbol_async_iterator),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().async_dispose),
            raw_native!(AsyncIteratorPrototype::symbol_async_dispose),
            0,
            attr,
            None,
        );
    }

    // 27.1.3.1 %AsyncIteratorPrototype% [ %Symbol.asyncDispose% ] ( ), https://tc39.es/ecma262/#sec-%asynciteratorprototype%-%symbol.asyncdispose%
    fn symbol_async_dispose(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("there is a current realm");
        let promise_constructor = realm.intrinsics().promise_constructor(vm);

        // 1. Let obj be the this value.
        let object = vm.this_value();

        // 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
        let promise_capability = new_promise_capability(vm, Value::from_object(promise_constructor)).must();

        // 3. Let return be Completion(GetMethod(obj, "return")).
        // 4. IfAbruptRejectPromise(return, promiseCapability).
        let return_method = try_or_reject!(vm, promise_capability, object.get_method(vm, &vm.names.return_));

        match return_method {
            // 5. If return is undefined, then
            None => {
                // a. Perform ! Call(promiseCapability.[[Resolve]], undefined, « undefined »).
                call_function_object(vm, promise_capability.resolve(), Value::UNDEFINED, &[Value::UNDEFINED]).must();
            }
            // 6. Else,
            Some(return_method) => {
                // a. Let result be Completion(Call(return, obj, « »)).
                // b. IfAbruptRejectPromise(result, promiseCapability).
                let result = try_or_reject!(
                    vm,
                    promise_capability,
                    call_function_object(vm, return_method, object, &[])
                );

                // c. Let resultWrapper be Completion(PromiseResolve(%Promise%, result)).
                // d. IfAbruptRejectPromise(resultWrapper, promiseCapability).
                let result_wrapper = try_or_reject!(
                    vm,
                    promise_capability,
                    promise_resolve(vm, promise_constructor.upcast(), result)
                );

                // e. Let unwrap be a new Abstract Closure with no parameters that captures nothing and performs the
                //    following steps when called:
                // f. Let onFulfilled be CreateBuiltinFunction(unwrap, 1, "", « »).
                let on_fulfilled = NativeFunction::create_anonymous(
                    vm,
                    (),
                    |_vm, &()| {
                        // i. Return undefined.
                        Ok(Value::UNDEFINED)
                    },
                    1,
                );

                // g. Perform PerformPromiseThen(resultWrapper, onFulfilled, undefined, promiseCapability).
                result_wrapper
                    .downcast::<Promise>()
                    .expect("PromiseResolve of %Promise% returns a Promise")
                    .perform_then(
                        vm,
                        Value::from_object(on_fulfilled),
                        Value::UNDEFINED,
                        Some(promise_capability),
                    );
            }
        }

        // 7. Return promiseCapability.[[Promise]].
        Ok(Value::from_object(promise_capability.promise()))
    }

    // 27.1.3.1 %AsyncIteratorPrototype% [ @@asyncIterator ] ( ), https://tc39.es/ecma262/#sec-asynciteratorprototype-asynciterator
    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn symbol_async_iterator(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return the this value.
        Ok(vm.this_value())
    }
}
