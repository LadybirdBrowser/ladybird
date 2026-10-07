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
use crate::runtime::completion::{Completion, ThrowCompletionOr};
use crate::runtime::disposable_stack::{DisposableStack, DisposableState};
use crate::runtime::environment::InitializeBindingHint;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, raw_native};
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct DisposableStackPrototype {
    base: Object,
}

define_object_class!(DisposableStackPrototype, extends: [Object], methods: {
    initialize: DisposableStackPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

const DISPLAY_NAME: &str = "DisposableStack";

fn current_realm(vm: &Vm) -> Gc<Realm> {
    vm.current_realm()
        .expect("a built-in function runs in an execution context with a realm")
}

impl DisposableStackPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<DisposableStackPrototype> {
        realm.create_object(
            vm,
            DisposableStackPrototype {
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
        define(&names.adopt, raw_native!(DisposableStackPrototype::adopt), 2);
        define(&names.defer, raw_native!(DisposableStackPrototype::defer), 1);
        define(&names.dispose, raw_native!(DisposableStackPrototype::dispose), 0);
        object.define_native_accessor(
            vm,
            realm,
            &names.disposed,
            raw_native!(DisposableStackPrototype::disposed_getter),
            None,
            attr,
        );
        define(&names.move_, raw_native!(DisposableStackPrototype::move_), 0);
        define(&names.use_, raw_native!(DisposableStackPrototype::use_), 1);

        // 12.3.3.7 DisposableStack.prototype [ @@dispose ] (), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype-@@dispose
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().dispose),
            object.get_without_side_effects(vm, &names.dispose),
            attr,
        );

        // 12.3.3.8 DisposableStack.prototype [ @@toStringTag ], https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype-@@toStringTag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("DisposableStack"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 12.3.3.1 DisposableStack.prototype.adopt( value, onDispose ), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.adopt
    fn adopt(vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);
        let on_dispose = vm.argument(1);

        // 1. Let disposableStack be the this value.
        // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
        let disposable_stack = typed_this_object::<DisposableStack>(vm, DISPLAY_NAME)?;

        // 3. If disposableStack.[[DisposableState]] is disposed, throw a ReferenceError exception.
        if disposable_stack.disposable_state() == DisposableState::Disposed {
            return vm.throw_completion(
                ErrorKind::ReferenceError,
                ErrorType::DisposableStackAlreadyDisposed,
                &[],
            );
        }

        // 4. If IsCallable(onDispose) is false, throw a TypeError exception.
        if !on_dispose.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&on_dispose]);
        }

        // 5. Let closure be a new Abstract Closure with no parameters that captures value and onDispose and performs the following steps when called:
        let closure = |vm: &Vm, &(value, on_dispose): &(Value, Gc<FunctionObject>)| {
            // a. Return ? Call(onDispose, undefined, « value »).
            call_function_object(vm, on_dispose, Value::UNDEFINED, &[value])
        };

        // 6. Let F be CreateBuiltinFunction(closure, 0, "", « »).
        let function = NativeFunction::create(
            vm,
            (value, on_dispose.as_function()),
            closure,
            0,
            &PropertyKey::from(Utf16FlyString::default()),
            None,
            None,
            None,
        );

        // 7. Perform ? AddDisposableResource(disposableStack.[[DisposeCapability]], undefined, sync-dispose, F).
        add_disposable_resource(
            vm,
            disposable_stack.dispose_capability(),
            Value::UNDEFINED,
            InitializeBindingHint::SyncDispose,
            Some(function.as_function_object_gc()),
        )?;

        // 8. Return value.
        Ok(value)
    }

    // 12.3.3.2 DisposableStack.prototype.defer( onDispose ), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.defer
    fn defer(vm: &Vm) -> ThrowCompletionOr<Value> {
        let on_dispose = vm.argument(0);

        // 1. Let disposableStack be the this value.
        // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
        let disposable_stack = typed_this_object::<DisposableStack>(vm, DISPLAY_NAME)?;

        // 3. If disposableStack.[[DisposableState]] is disposed, throw a ReferenceError exception.
        if disposable_stack.disposable_state() == DisposableState::Disposed {
            return vm.throw_completion(
                ErrorKind::ReferenceError,
                ErrorType::DisposableStackAlreadyDisposed,
                &[],
            );
        }

        // 4. If IsCallable(onDispose) is false, throw a TypeError exception.
        if !on_dispose.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&on_dispose]);
        }

        // 5. Perform ? AddDisposableResource(disposableStack.[[DisposeCapability]], undefined, sync-dispose, onDispose).
        add_disposable_resource(
            vm,
            disposable_stack.dispose_capability(),
            Value::UNDEFINED,
            InitializeBindingHint::SyncDispose,
            Some(on_dispose.as_function()),
        )?;

        // 6. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 12.3.3.3 DisposableStack.prototype.dispose (), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.dispose
    fn dispose(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let disposableStack be the this value.
        // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
        let disposable_stack = typed_this_object::<DisposableStack>(vm, DISPLAY_NAME)?;

        // 3. If disposableStack.[[DisposableState]] is disposed, return undefined.
        if disposable_stack.disposable_state() == DisposableState::Disposed {
            return Ok(Value::UNDEFINED);
        }

        // 4. Set disposableStack.[[DisposableState]] to disposed.
        disposable_stack.set_disposed();

        // 5. Return DisposeResources(disposableStack.[[DisposeCapability]], NormalCompletion(undefined)).
        dispose_resources(
            vm,
            disposable_stack.dispose_capability(),
            Completion::normal(Value::UNDEFINED),
        )
        .into_throw_completion_or()
    }

    // 12.3.3.4 get DisposableStack.prototype.disposed, https://tc39.es/proposal-explicit-resource-management/#sec-get-disposablestack.prototype.disposed
    fn disposed_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let disposableStack be the this value.
        // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
        let disposable_stack = typed_this_object::<DisposableStack>(vm, DISPLAY_NAME)?;

        // 3. If disposableStack.[[DisposableState]] is disposed, return true.
        // 4. Otherwise, return false.
        Ok(Value::from_bool(
            disposable_stack.disposable_state() == DisposableState::Disposed,
        ))
    }

    // 12.3.3.5 DisposableStack.prototype.move(), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.move
    fn move_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        // 1. Let disposableStack be the this value.
        // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
        let disposable_stack = typed_this_object::<DisposableStack>(vm, DISPLAY_NAME)?;

        // 3. If disposableStack.[[DisposableState]] is disposed, throw a ReferenceError exception.
        if disposable_stack.disposable_state() == DisposableState::Disposed {
            return vm.throw_completion(
                ErrorKind::ReferenceError,
                ErrorType::DisposableStackAlreadyDisposed,
                &[],
            );
        }

        // 4. Let newDisposableStack be ? OrdinaryCreateFromConstructor(%DisposableStack%, "%DisposableStack.prototype%", « [[DisposableState]], [[DisposeCapability]] »).
        // 5. Set newDisposableStack.[[DisposableState]] to pending.
        let new_disposable_stack = ordinary_create_from_constructor_of(
            vm,
            realm,
            realm.intrinsics().disposable_stack_constructor(vm).upcast(),
            Intrinsics::disposable_stack_prototype,
            |prototype| DisposableStack::new(vm, new_dispose_capability(), prototype),
        )?;

        // 6. Set newDisposableStack.[[DisposeCapability]] to disposableStack.[[DisposeCapability]].
        // 7. Set disposableStack.[[DisposeCapability]] to NewDisposeCapability().
        // NB: The resources move between the stacks once both exist, so that one of them always keeps them alive.
        let dispose_capability = disposable_stack.dispose_capability().replace(new_dispose_capability());
        new_disposable_stack.dispose_capability().replace(dispose_capability);

        // 8. Set disposableStack.[[DisposableState]] to disposed.
        disposable_stack.set_disposed();

        // 9. Return newDisposableStack.
        Ok(Value::from_object(new_disposable_stack))
    }

    // 12.3.3.6 DisposableStack.prototype.use( value ), https://tc39.es/proposal-explicit-resource-management/#sec-disposablestack.prototype.use
    fn use_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 1. Let disposableStack be the this value.
        // 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
        let disposable_stack = typed_this_object::<DisposableStack>(vm, DISPLAY_NAME)?;

        // 3. If disposableStack.[[DisposableState]] is disposed, throw a ReferenceError exception.
        if disposable_stack.disposable_state() == DisposableState::Disposed {
            return vm.throw_completion(
                ErrorKind::ReferenceError,
                ErrorType::DisposableStackAlreadyDisposed,
                &[],
            );
        }

        // 4. Perform ? AddDisposableResource(disposableStack.[[DisposeCapability]], value, sync-dispose).
        add_disposable_resource(
            vm,
            disposable_stack.dispose_capability(),
            value,
            InitializeBindingHint::SyncDispose,
            None,
        )?;

        // 5. Return value.
        Ok(value)
    }
}
