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
use crate::runtime::abstract_operations::{call_function_object, construct, create_list_from_array_like};
use crate::runtime::array::Array;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{
    MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, PropertyLookupPhase, define_object_class,
};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_descriptor::{from_property_descriptor, to_property_descriptor};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct ReflectObject {
    base: Object,
}

define_object_class!(ReflectObject, extends: [Object], methods: {
    initialize: ReflectObject::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl ReflectObject {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ReflectObject> {
        realm.create_object(
            vm,
            ReflectObject {
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
        let define_native_function = |property_key: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, property_key, function, length, attr, None);
        };
        define_native_function(&names.apply, raw_native!(ReflectObject::apply), 3);
        define_native_function(&names.construct, raw_native!(ReflectObject::construct), 2);
        define_native_function(&names.defineProperty, raw_native!(ReflectObject::define_property), 3);
        define_native_function(&names.deleteProperty, raw_native!(ReflectObject::delete_property), 2);
        define_native_function(&names.get, raw_native!(ReflectObject::get), 2);
        define_native_function(
            &names.getOwnPropertyDescriptor,
            raw_native!(ReflectObject::get_own_property_descriptor),
            2,
        );
        define_native_function(&names.getPrototypeOf, raw_native!(ReflectObject::get_prototype_of), 1);
        define_native_function(&names.has, raw_native!(ReflectObject::has), 2);
        define_native_function(&names.isExtensible, raw_native!(ReflectObject::is_extensible), 1);
        define_native_function(&names.ownKeys, raw_native!(ReflectObject::own_keys), 1);
        define_native_function(
            &names.preventExtensions,
            raw_native!(ReflectObject::prevent_extensions),
            1,
        );
        define_native_function(&names.set, raw_native!(ReflectObject::set), 3);
        define_native_function(&names.setPrototypeOf, raw_native!(ReflectObject::set_prototype_of), 2);

        // 28.1.14 Reflect [ @@toStringTag ], https://tc39.es/ecma262/#sec-reflect-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(vm, names.Reflect.as_string())),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 28.1.1 Reflect.apply ( target, thisArgument, argumentsList ), https://tc39.es/ecma262/#sec-reflect.apply
    fn apply(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);
        let this_argument = vm.argument(1);
        let arguments_list = vm.argument(2);

        // 1. If IsCallable(target) is false, throw a TypeError exception.
        if !target.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&target]);
        }

        // 2. Let args be ? CreateListFromArrayLike(argumentsList).
        let args = create_list_from_array_like(vm, arguments_list, None)?;

        // 3. Perform PrepareForTailCall().
        // 4. Return ? Call(target, thisArgument, args).
        args.with_values(|args| call_function_object(vm, target.as_function(), this_argument, args))
    }

    // 28.1.2 Reflect.construct ( target, argumentsList [ , newTarget ] ), https://tc39.es/ecma262/#sec-reflect.construct
    fn construct(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);
        let arguments_list = vm.argument(1);
        let mut new_target = vm.argument(2);

        // 1. If IsConstructor(target) is false, throw a TypeError exception.
        if !target.is_constructor() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAConstructor, &[&target]);
        }

        // 2. If newTarget is not present, set newTarget to target.
        if vm.argument_count() < 3 {
            new_target = target;
        }
        // 3. Else if IsConstructor(newTarget) is false, throw a TypeError exception.
        else if !new_target.is_constructor() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAConstructor, &[&new_target]);
        }

        // 4. Let args be ? CreateListFromArrayLike(argumentsList).
        let args = create_list_from_array_like(vm, arguments_list, None)?;

        // 5. Return ? Construct(target, args, newTarget).
        Ok(Value::from_object(construct(
            vm,
            target.as_function(),
            &args.to_vec(),
            Some(new_target.as_function()),
        )?))
    }

    // 28.1.3 Reflect.defineProperty ( target, propertyKey, attributes ), https://tc39.es/ecma262/#sec-reflect.defineproperty
    fn define_property(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);
        let property_key = vm.argument(1);
        let attributes = vm.argument(2);

        // 1. If Type(target) is not Object, throw a TypeError exception.
        if !target.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&target]);
        }

        // 2. Let key be ? ToPropertyKey(propertyKey).
        let key = property_key.to_property_key(vm)?;

        // 3. Let desc be ? ToPropertyDescriptor(attributes).
        let mut descriptor = to_property_descriptor(vm, attributes)?;

        // 4. Return ? target.[[DefineOwnProperty]](key, desc).
        Ok(Value::from_bool(target.as_object().internal_define_own_property(
            vm,
            &key,
            &mut descriptor,
            None,
        )?))
    }

    // 28.1.4 Reflect.deleteProperty ( target, propertyKey ), https://tc39.es/ecma262/#sec-reflect.deleteproperty
    fn delete_property(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);
        let property_key = vm.argument(1);

        // 1. If Type(target) is not Object, throw a TypeError exception.
        if !target.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&target]);
        }

        // 2. Let key be ? ToPropertyKey(propertyKey).
        let key = property_key.to_property_key(vm)?;

        // 3. Return ? target.[[Delete]](key).
        Ok(Value::from_bool(target.as_object().internal_delete(vm, &key)?))
    }

    // 28.1.5 Reflect.get ( target, propertyKey [ , receiver ] ), https://tc39.es/ecma262/#sec-reflect.get
    fn get(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);
        let property_key = vm.argument(1);
        let mut receiver = vm.argument(2);

        // 1. If Type(target) is not Object, throw a TypeError exception.
        if !target.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&target]);
        }

        // 2. Let key be ? ToPropertyKey(propertyKey).
        let key = property_key.to_property_key(vm)?;

        // 3. If receiver is not present, then
        if vm.argument_count() < 3 {
            // a. Set receiver to target.
            receiver = target;
        }

        // 4. Return ? target.[[Get]](key, receiver).
        target
            .as_object()
            .internal_get(vm, &key, receiver, None, PropertyLookupPhase::OwnProperty)
    }

    // 28.1.6 Reflect.getOwnPropertyDescriptor ( target, propertyKey ), https://tc39.es/ecma262/#sec-reflect.getownpropertydescriptor
    fn get_own_property_descriptor(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);
        let property_key = vm.argument(1);

        // 1. If Type(target) is not Object, throw a TypeError exception.
        if !target.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&target]);
        }

        // 2. Let key be ? ToPropertyKey(propertyKey).
        let key = property_key.to_property_key(vm)?;

        // 3. Let desc be ? target.[[GetOwnProperty]](key).
        let descriptor = target.as_object().internal_get_own_property(vm, &key)?;

        // 4. Return FromPropertyDescriptor(desc).
        Ok(from_property_descriptor(vm, &descriptor))
    }

    // 28.1.7 Reflect.getPrototypeOf ( target ), https://tc39.es/ecma262/#sec-reflect.getprototypeof
    fn get_prototype_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);

        // 1. If Type(target) is not Object, throw a TypeError exception.
        if !target.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&target]);
        }

        // 2. Return ? target.[[GetPrototypeOf]]().
        Ok(target
            .as_object()
            .internal_get_prototype_of(vm)?
            .map_or(Value::NULL, Value::from_object))
    }

    // 28.1.8 Reflect.has ( target, propertyKey ), https://tc39.es/ecma262/#sec-reflect.has
    fn has(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);
        let property_key = vm.argument(1);

        // 1. If Type(target) is not Object, throw a TypeError exception.
        if !target.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&target]);
        }

        // 2. Let key be ? ToPropertyKey(propertyKey).
        let key = property_key.to_property_key(vm)?;

        // 3. Return ? target.[[HasProperty]](key).
        Ok(Value::from_bool(target.as_object().internal_has_property(vm, &key)?))
    }

    // 28.1.9 Reflect.isExtensible ( target ), https://tc39.es/ecma262/#sec-reflect.isextensible
    fn is_extensible(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);

        // 1. If Type(target) is not Object, throw a TypeError exception.
        if !target.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&target]);
        }

        // 2. Return ? target.[[IsExtensible]]().
        Ok(Value::from_bool(target.as_object().internal_is_extensible(vm)?))
    }

    // 28.1.10 Reflect.ownKeys ( target ), https://tc39.es/ecma262/#sec-reflect.ownkeys
    fn own_keys(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a native function runs in a realm");

        let target = vm.argument(0);

        // 1. If Type(target) is not Object, throw a TypeError exception.
        if !target.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&target]);
        }

        // 2. Let keys be ? target.[[OwnPropertyKeys]]().
        let keys = target.as_object().internal_own_property_keys(vm)?;

        // 3. Return CreateArrayFromList(keys).
        Ok(Value::from_object(Array::create_from_list(vm, realm, &keys)))
    }

    // 28.1.11 Reflect.preventExtensions ( target ), https://tc39.es/ecma262/#sec-reflect.preventextensions
    fn prevent_extensions(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);

        // 1. If Type(target) is not Object, throw a TypeError exception.
        if !target.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&target]);
        }

        // 2. Return ? target.[[PreventExtensions]]().
        Ok(Value::from_bool(target.as_object().internal_prevent_extensions(vm)?))
    }

    // 28.1.12 Reflect.set ( target, propertyKey, V [ , receiver ] ), https://tc39.es/ecma262/#sec-reflect.set
    fn set(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);
        let property_key = vm.argument(1);
        let value = vm.argument(2);
        let mut receiver = vm.argument(3);

        // 1. If Type(target) is not Object, throw a TypeError exception.
        if !target.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&target]);
        }

        // 2. Let key be ? ToPropertyKey(propertyKey).
        let key = property_key.to_property_key(vm)?;

        // 3. If receiver is not present, then
        if vm.argument_count() < 4 {
            // a. Set receiver to target.
            receiver = target;
        }

        // 4. Return ? target.[[Set]](key, V, receiver).
        Ok(Value::from_bool(target.as_object().internal_set(
            vm,
            &key,
            value,
            receiver,
            None,
            PropertyLookupPhase::OwnProperty,
        )?))
    }

    // 28.1.13 Reflect.setPrototypeOf ( target, proto ), https://tc39.es/ecma262/#sec-reflect.setprototypeof
    fn set_prototype_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);
        let prototype = vm.argument(1);

        // 1. If Type(target) is not Object, throw a TypeError exception.
        if !target.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&target]);
        }

        // 2. If Type(proto) is not Object and proto is not null, throw a TypeError exception.
        if !prototype.is_object() && !prototype.is_null() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ObjectPrototypeWrongType, &[]);
        }

        // 3. Return ? target.[[SetPrototypeOf]](proto).
        Ok(Value::from_bool(target.as_object().internal_set_prototype_of(
            vm,
            (!prototype.is_null()).then(|| prototype.as_object()),
        )?))
    }
}
