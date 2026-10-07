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
use crate::runtime::abstract_operations::{
    add_disposable_resource, call_function_object, dispose_resources, new_dispose_capability,
    ordinary_create_from_constructor_of,
};
use crate::runtime::async_disposable_stack::{AsyncDisposableStack, AsyncDisposableState};
use crate::runtime::completion::{Completion, Must, ThrowCompletionOr};
use crate::runtime::environment::InitializeBindingHint;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, raw_native};
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::promise_capability::{new_promise_capability, try_or_reject};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct AsyncDisposableStackPrototype {
    base: Object,
}

define_object_class!(AsyncDisposableStackPrototype, extends: [Object], methods: {
    initialize: AsyncDisposableStackPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

const DISPLAY_NAME: &str = "AsyncDisposableStack";

fn current_realm(vm: &Vm) -> Gc<Realm> {
    vm.current_realm()
        .expect("a built-in function runs in an execution context with a realm")
}

impl AsyncDisposableStackPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AsyncDisposableStackPrototype> {
        realm.create_object(
            vm,
            AsyncDisposableStackPrototype {
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
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define = |property_key: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, property_key, function, length, attr, None);
        };
        define(&names.adopt, raw_native!(AsyncDisposableStackPrototype::adopt), 2);
        define(&names.defer, raw_native!(AsyncDisposableStackPrototype::defer), 1);
        define(
            &names.disposeAsync,
            raw_native!(AsyncDisposableStackPrototype::dispose_async),
            0,
        );
        object.define_native_accessor(
            vm,
            realm,
            &names.disposed,
            raw_native!(AsyncDisposableStackPrototype::disposed_getter),
            None,
            attr,
        );
        define(&names.move_, raw_native!(AsyncDisposableStackPrototype::move_), 0);
        define(&names.use_, raw_native!(AsyncDisposableStackPrototype::use_), 1);

        // 12.4.3.7 AsyncDisposableStack.prototype [ @@asyncDispose ] (), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype-@@asyncDispose
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().async_dispose),
            object.get_without_side_effects(vm, &names.disposeAsync),
            attr,
        );

        // 12.4.3.8 AsyncDisposableStack.prototype [ @@toStringTag ], https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype-@@toStringTag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("AsyncDisposableStack"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 12.4.3.1 AsyncDisposableStack.prototype.adopt( value, onDisposeAsync ), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.adopt
    fn adopt(vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);
        let on_dispose_async = vm.argument(1);

        // 1. Let asyncDisposableStack be the this value.
        // 2. Perform ? RequireInternalSlot(asyncDisposableStack, [[AsyncDisposableState]]).
        let async_disposable_stack = typed_this_object::<AsyncDisposableStack>(vm, DISPLAY_NAME)?;

        // 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, throw a ReferenceError exception.
        if async_disposable_stack.async_disposable_state() == AsyncDisposableState::Disposed {
            return vm.throw_completion(
                ErrorKind::ReferenceError,
                ErrorType::AsyncDisposableStackAlreadyDisposed,
                &[],
            );
        }

        // 4. If IsCallable(onDisposeAsync) is false, throw a TypeError exception.
        if !on_dispose_async.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&on_dispose_async]);
        }

        // 5. Let closure be a new Abstract Closure with no parameters that captures value and onDisposeAsync and performs the following steps when called:
        let closure = |vm: &Vm, &(value, on_dispose_async): &(Value, Gc<FunctionObject>)| {
            // a. Return ? Call(onDisposeAsync, undefined, « value »).
            call_function_object(vm, on_dispose_async, Value::UNDEFINED, &[value])
        };

        // 6. Let F be CreateBuiltinFunction(closure, 0, "", « »).
        let function = NativeFunction::create(
            vm,
            (value, on_dispose_async.as_function()),
            closure,
            0,
            &PropertyKey::from(Utf16FlyString::default()),
            None,
            None,
            None,
        );

        // 7. Perform ? AddDisposableResource(asyncDisposableStack.[[DisposeCapability]], undefined, async-dispose, F).
        add_disposable_resource(
            vm,
            async_disposable_stack.dispose_capability(),
            Value::UNDEFINED,
            InitializeBindingHint::AsyncDispose,
            Some(function.as_function_object_gc()),
        )?;

        // 8. Return value.
        Ok(value)
    }

    // 12.4.3.2 AsyncDisposableStack.prototype.defer( onDisposeAsync ), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.defer
    fn defer(vm: &Vm) -> ThrowCompletionOr<Value> {
        let on_dispose_async = vm.argument(0);

        // 1. Let asyncDisposableStack be the this value.
        // 2. Perform ? RequireInternalSlot(asyncDisposableStack, [[AsyncDisposableState]]).
        let async_disposable_stack = typed_this_object::<AsyncDisposableStack>(vm, DISPLAY_NAME)?;

        // 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, throw a ReferenceError exception.
        if async_disposable_stack.async_disposable_state() == AsyncDisposableState::Disposed {
            return vm.throw_completion(
                ErrorKind::ReferenceError,
                ErrorType::AsyncDisposableStackAlreadyDisposed,
                &[],
            );
        }

        // 4. If IsCallable(onDisposeAsync) is false, throw a TypeError exception.
        if !on_dispose_async.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&on_dispose_async]);
        }

        // 5. Perform ? AddDisposableResource(asyncDisposableStack.[[DisposeCapability]], undefined, async-dispose, onDisposeAsync).
        add_disposable_resource(
            vm,
            async_disposable_stack.dispose_capability(),
            Value::UNDEFINED,
            InitializeBindingHint::AsyncDispose,
            Some(on_dispose_async.as_function()),
        )?;

        // 6. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 12.4.3.3 AsyncDisposableStack.prototype.disposeAsync(), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.disposeAsync
    fn dispose_async(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        // 1. Let asyncDisposableStack be the this value.
        let this_value = vm.this_value();
        let async_disposable_stack = this_value
            .is_object()
            .then(|| this_value.as_object().downcast::<AsyncDisposableStack>())
            .flatten();

        // 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
        let promise_capability =
            new_promise_capability(vm, Value::from_object(realm.intrinsics().promise_constructor(vm))).must();

        // 3. If asyncDisposableStack does not have an [[AsyncDisposableState]] internal slot, then
        let Some(async_disposable_stack) = async_disposable_stack else {
            // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « a newly created TypeError object »).
            let error = vm
                .throw_completion::<()>(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&DISPLAY_NAME])
                .expect_err("throw_completion() throws");
            call_function_object(vm, promise_capability.reject(), Value::UNDEFINED, &[error.value()]).must();

            // b. Return promiseCapability.[[Promise]].
            return Ok(Value::from_object(promise_capability.promise()));
        };

        // 4. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, then
        if async_disposable_stack.async_disposable_state() == AsyncDisposableState::Disposed {
            // a. Perform ! Call(promiseCapability.[[Resolve]], undefined, « undefined »).
            call_function_object(vm, promise_capability.resolve(), Value::UNDEFINED, &[Value::UNDEFINED]).must();

            // b. Return promiseCapability.[[Promise]].
            return Ok(Value::from_object(promise_capability.promise()));
        }

        // 5. Set asyncDisposableStack.[[AsyncDisposableState]] to disposed.
        async_disposable_stack.set_disposed();

        // 6. Let result be DisposeResources(asyncDisposableStack.[[DisposeCapability]], NormalCompletion(undefined)).
        // 7. IfAbruptRejectPromise(result, promiseCapability).
        let result = try_or_reject!(
            vm,
            promise_capability,
            dispose_resources(
                vm,
                async_disposable_stack.dispose_capability(),
                Completion::normal(Value::UNDEFINED),
            )
            .into_throw_completion_or()
        );

        // 8. Perform ! Call(promiseCapability.[[Resolve]], undefined, « result »).
        call_function_object(vm, promise_capability.resolve(), Value::UNDEFINED, &[result]).must();

        // 9. Return promiseCapability.[[Promise]].
        Ok(Value::from_object(promise_capability.promise()))
    }

    // 12.4.3.4 get AsyncDisposableStack.prototype.disposed, https://tc39.es/proposal-explicit-resource-management/#sec-get-asyncdisposablestack.prototype.disposed
    fn disposed_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let asyncDisposableStack be the this value.
        // 2. Perform ? RequireInternalSlot(asyncDisposableStack, [[AsyncDisposableState]]).
        let async_disposable_stack = typed_this_object::<AsyncDisposableStack>(vm, DISPLAY_NAME)?;

        // 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, return true.
        // 4. Otherwise, return false.
        Ok(Value::from_bool(
            async_disposable_stack.async_disposable_state() == AsyncDisposableState::Disposed,
        ))
    }

    // 12.4.3.5 AsyncDisposableStack.prototype.move(), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.move
    fn move_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        // 1. Let asyncDisposableStack be the this value.
        // 2. Perform ? RequireInternalSlot(asyncDisposableStack, [[AsyncDisposableState]]).
        let async_disposable_stack = typed_this_object::<AsyncDisposableStack>(vm, DISPLAY_NAME)?;

        // 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, throw a ReferenceError exception.
        if async_disposable_stack.async_disposable_state() == AsyncDisposableState::Disposed {
            return vm.throw_completion(
                ErrorKind::ReferenceError,
                ErrorType::AsyncDisposableStackAlreadyDisposed,
                &[],
            );
        }

        // 4. Let newAsyncDisposableStack be ? OrdinaryCreateFromConstructor(%AsyncDisposableStack%, "%AsyncDisposableStack.prototype%", « [[AsyncDisposableState]], [[DisposeCapability]] »).
        // 5. Set newAsyncDisposableStack.[[AsyncDisposableState]] to pending.
        let new_async_disposable_stack = ordinary_create_from_constructor_of(
            vm,
            realm,
            realm.intrinsics().async_disposable_stack_constructor(vm).upcast(),
            Intrinsics::async_disposable_stack_prototype,
            |prototype| AsyncDisposableStack::new(vm, new_dispose_capability(), prototype),
        )?;

        // 6. Set newAsyncDisposableStack.[[DisposeCapability]] to asyncDisposableStack.[[DisposeCapability]].
        // 7. Set asyncDisposableStack.[[DisposeCapability]] to NewDisposeCapability().
        // NB: The resources move between the stacks once both exist, so that one of them always keeps them alive.
        let dispose_capability = async_disposable_stack
            .dispose_capability()
            .replace(new_dispose_capability());
        new_async_disposable_stack
            .dispose_capability()
            .replace(dispose_capability);

        // 8. Set asyncDisposableStack.[[AsyncDisposableState]] to disposed.
        async_disposable_stack.set_disposed();

        // 9. Return newAsyncDisposableStack.
        Ok(Value::from_object(new_async_disposable_stack))
    }

    // 12.4.3.6 AsyncDisposableStack.prototype.use( value ), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.use
    fn use_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 1. Let asyncDisposableStack be the this value.
        // 2. Perform ? RequireInternalSlot(asyncDisposableStack, [[AsyncDisposableState]]).
        let async_disposable_stack = typed_this_object::<AsyncDisposableStack>(vm, DISPLAY_NAME)?;

        // 3. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, throw a ReferenceError exception.
        if async_disposable_stack.async_disposable_state() == AsyncDisposableState::Disposed {
            return vm.throw_completion(
                ErrorKind::ReferenceError,
                ErrorType::AsyncDisposableStackAlreadyDisposed,
                &[],
            );
        }

        // 4. Perform ? AddDisposableResource(asyncDisposableStack.[[DisposeCapability]], value, async-dispose).
        add_disposable_resource(
            vm,
            async_disposable_stack.dispose_capability(),
            value,
            InitializeBindingHint::AsyncDispose,
            None,
        )?;

        // 5. Return value.
        Ok(value)
    }
}
