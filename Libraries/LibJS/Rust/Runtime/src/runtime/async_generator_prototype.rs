/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call_function_object;
use crate::runtime::async_generator::{AsyncGenerator, AsyncGeneratorState};
use crate::runtime::completion::{Completion, CompletionType, Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::iterator::create_iterator_result_object;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::promise_capability::{new_promise_capability, try_or_reject};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

/// %AsyncGeneratorPrototype%.
#[repr(C)]
#[derive(Trace)]
pub struct AsyncGeneratorPrototype {
    base: Object,
}

define_object_class!(AsyncGeneratorPrototype, extends: [Object], methods: {
    initialize: AsyncGeneratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

// 27.6.3.3 AsyncGeneratorValidate ( generator, generatorBrand ), https://tc39.es/ecma262/#sec-asyncgeneratorvalidate
fn async_generator_validate(
    vm: &Vm,
    generator: Value,
    generator_brand: Option<&str>,
) -> ThrowCompletionOr<Gc<AsyncGenerator>> {
    // 1. Perform ? RequireInternalSlot(generator, [[AsyncGeneratorContext]]).
    // 2. Perform ? RequireInternalSlot(generator, [[AsyncGeneratorState]]).
    // 3. Perform ? RequireInternalSlot(generator, [[AsyncGeneratorQueue]]).
    let async_generator = generator
        .is_object()
        .then(|| generator.as_object().downcast::<AsyncGenerator>())
        .flatten();
    let Some(async_generator) = async_generator else {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"AsyncGenerator"]);
    };

    // 4. If generator.[[GeneratorBrand]] is not generatorBrand, throw a TypeError exception.
    if async_generator.generator_brand() != generator_brand {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::GeneratorBrandMismatch,
            &[
                &async_generator.generator_brand().unwrap_or("emp"),
                &generator_brand.unwrap_or("emp"),
            ],
        );
    }

    // 5. Return unused.
    Ok(async_generator)
}

impl AsyncGeneratorPrototype {
    // 27.6.1 Properties of the AsyncGenerator Prototype Object, https://tc39.es/ecma262/#sec-properties-of-asyncgenerator-prototype
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AsyncGeneratorPrototype> {
        realm.create_object(
            vm,
            AsyncGeneratorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().async_iterator_prototype(),
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
            &vm.names.next,
            raw_native!(AsyncGeneratorPrototype::next),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &vm.names.return_,
            raw_native!(AsyncGeneratorPrototype::return_),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &vm.names.throw_,
            raw_native!(AsyncGeneratorPrototype::throw_),
            1,
            attr,
            None,
        );

        // 27.6.1.5 AsyncGenerator.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-asyncgenerator-prototype-tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("AsyncGenerator"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 27.6.1.2 AsyncGenerator.prototype.next ( value ), https://tc39.es/ecma262/#sec-asyncgenerator-prototype-next
    fn next(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("there is a current realm");

        // 1. Let generator be the this value.
        let generator_this_value = vm.this_value();

        // 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
        let promise_capability =
            new_promise_capability(vm, Value::from_object(realm.intrinsics().promise_constructor(vm))).must();

        // 3. Let result be Completion(AsyncGeneratorValidate(generator, empty)).
        // 4. IfAbruptRejectPromise(result, promiseCapability).
        let generator = try_or_reject!(
            vm,
            promise_capability,
            async_generator_validate(vm, generator_this_value, None)
        );

        // 5. Let state be generator.[[AsyncGeneratorState]].
        let state = generator.async_generator_state();

        // 6. If state is completed, then
        if state == AsyncGeneratorState::Completed {
            // a. Let iteratorResult be CreateIterResultObject(undefined, true).
            let iterator_result = create_iterator_result_object(vm, realm, Value::UNDEFINED, true);

            // b. Perform ! Call(promiseCapability.[[Resolve]], undefined, « iteratorResult »).
            call_function_object(
                vm,
                promise_capability.resolve(),
                Value::UNDEFINED,
                &[Value::from_object(iterator_result)],
            )
            .must();

            // c. Return promiseCapability.[[Promise]].
            return Ok(Value::from_object(promise_capability.promise()));
        }

        // 7. Let completion be NormalCompletion(value).
        let completion = Completion::normal(vm.argument(0));

        // 8. Perform AsyncGeneratorEnqueue(generator, completion, promiseCapability).
        generator.async_generator_enqueue(completion, promise_capability);

        // 9. If state is either suspendedStart or suspendedYield, then
        if state == AsyncGeneratorState::SuspendedStart || state == AsyncGeneratorState::SuspendedYield {
            // a. Perform AsyncGeneratorResume(generator, completion).
            try_or_reject!(vm, promise_capability, generator.resume(vm, completion));
        }
        // 10. Else,
        else {
            // a. Assert: state is either executing or draining-queue.
            assert!(state == AsyncGeneratorState::Executing || state == AsyncGeneratorState::DrainingQueue);
        }

        // 11. Return promiseCapability.[[Promise]].
        Ok(Value::from_object(promise_capability.promise()))
    }

    // 27.9.1.3 %AsyncGeneratorPrototype%.return ( value ), https://tc39.es/ecma262/#sec-asyncgenerator-prototype-return
    fn return_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("there is a current realm");

        // 1. Let gen be the this value.
        let generator_this_value = vm.this_value();

        // 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
        let promise_capability =
            new_promise_capability(vm, Value::from_object(realm.intrinsics().promise_constructor(vm))).must();

        // 3. Let result be Completion(AsyncGeneratorValidate(gen, empty)).
        // 4. IfAbruptRejectPromise(result, promiseCapability).
        let generator = try_or_reject!(
            vm,
            promise_capability,
            async_generator_validate(vm, generator_this_value, None)
        );

        // 5. Let completion be ReturnCompletion(value).
        let completion = Completion::new(CompletionType::Return, vm.argument(0));

        // 6. Perform AsyncGeneratorEnqueue(gen, completion, promiseCapability).
        generator.async_generator_enqueue(completion, promise_capability);

        // 7. Let state be gen.[[AsyncGeneratorState]].
        let state = generator.async_generator_state();

        // 8. If state is either suspended-start or completed, then
        if state == AsyncGeneratorState::SuspendedStart || state == AsyncGeneratorState::Completed {
            // a. Set gen.[[AsyncGeneratorState]] to draining-queue.
            generator.set_async_generator_state(AsyncGeneratorState::DrainingQueue);

            // b. Perform AsyncGeneratorAwaitReturn(gen).
            generator.await_return(vm);
        }
        // 9. Else if state is suspended-yield, then
        else if state == AsyncGeneratorState::SuspendedYield {
            // a. Perform AsyncGeneratorResume(gen, completion).
            try_or_reject!(vm, promise_capability, generator.resume(vm, completion));
        }
        // 10. Else,
        else {
            // a. Assert: state is either executing or draining-queue.
            assert!(state == AsyncGeneratorState::Executing || state == AsyncGeneratorState::DrainingQueue);
        }

        // 11. Return promiseCapability.[[Promise]].
        Ok(Value::from_object(promise_capability.promise()))
    }

    // 27.6.1.4 AsyncGenerator.prototype.throw ( exception ), https://tc39.es/ecma262/#sec-asyncgenerator-prototype-throw
    fn throw_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("there is a current realm");

        let exception = vm.argument(0);

        // 1. Let generator be the this value.
        let generator_this_value = vm.this_value();

        // 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
        let promise_capability =
            new_promise_capability(vm, Value::from_object(realm.intrinsics().promise_constructor(vm))).must();

        // 3. Let result be Completion(AsyncGeneratorValidate(generator, empty)).
        // 4. IfAbruptRejectPromise(result, promiseCapability).
        let generator = try_or_reject!(
            vm,
            promise_capability,
            async_generator_validate(vm, generator_this_value, None)
        );

        // 5. Let state be generator.[[AsyncGeneratorState]].
        let mut state = generator.async_generator_state();

        // 6. If state is suspendedStart, then
        if state == AsyncGeneratorState::SuspendedStart {
            // a. Set generator.[[AsyncGeneratorState]] to completed.
            generator.set_async_generator_state(AsyncGeneratorState::Completed);

            // b. Set state to completed.
            state = AsyncGeneratorState::Completed;
        }

        // 7. If state is completed, then
        if state == AsyncGeneratorState::Completed {
            // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « exception »).
            call_function_object(vm, promise_capability.reject(), Value::UNDEFINED, &[exception]).must();

            // b. Return promiseCapability.[[Promise]].
            return Ok(Value::from_object(promise_capability.promise()));
        }

        // 8. Let completion be ThrowCompletion(exception).
        crate::embedding::completion::log_exception_if_enabled(vm, exception);
        let completion = Completion::new(CompletionType::Throw, exception);

        // 9. Perform AsyncGeneratorEnqueue(generator, completion, promiseCapability).
        generator.async_generator_enqueue(completion, promise_capability);

        // 10. If state is suspendedYield, then
        if state == AsyncGeneratorState::SuspendedYield {
            // a. Perform AsyncGeneratorResume(generator, completion).
            try_or_reject!(vm, promise_capability, generator.resume(vm, completion));
        }
        // 11. Else,
        else {
            // a. Assert: state is either executing or draining-queue.
            assert!(state == AsyncGeneratorState::Executing || state == AsyncGeneratorState::DrainingQueue);
        }

        // 12. Return promiseCapability.[[Promise]].
        Ok(Value::from_object(promise_capability.promise()))
    }
}
