/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::Deref;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::environment::{ENVIRONMENT_METHODS, Environment, EnvironmentMethods, InitializeBindingHint};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::object::ShouldThrowExceptions;
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IsWithEnvironment {
    No,
    Yes,
}

// 9.1.1.2 Object Environment Records, https://tc39.es/ecma262/#sec-object-environment-records
#[repr(C)]
#[derive(Trace)]
pub struct ObjectEnvironment {
    base: Environment,
    binding_object: Gc<Object>,
    with_environment: bool,
}

define_cell!(ObjectEnvironment, Other, extends: [Environment]);

impl Deref for ObjectEnvironment {
    type Target = Environment;

    fn deref(&self) -> &Environment {
        &self.base
    }
}

fn object_environment(environment: &Environment) -> &ObjectEnvironment {
    environment
        .downcast_ref::<ObjectEnvironment>()
        .expect("only object environments have the object environment methods")
}

/// The methods JS::ObjectEnvironment overrides.
pub const OBJECT_ENVIRONMENT_METHODS: EnvironmentMethods = EnvironmentMethods {
    with_base_object: |environment| object_environment(environment).with_base_object(),
    has_binding: |environment, vm, name, _| object_environment(environment).has_binding(vm, name),
    create_mutable_binding: |environment, vm, name, can_be_deleted| {
        object_environment(environment).create_mutable_binding(vm, name, can_be_deleted)
    },
    create_immutable_binding: |environment, vm, name, strict| {
        object_environment(environment).create_immutable_binding(vm, name, strict)
    },
    initialize_binding: |environment, vm, name, value, hint| {
        object_environment(environment).initialize_binding(vm, name, value, hint)
    },
    set_mutable_binding: |environment, vm, name, value, strict| {
        object_environment(environment).set_mutable_binding(vm, name, value, strict)
    },
    get_binding_value: |environment, vm, name, strict| {
        object_environment(environment).get_binding_value(vm, name, strict)
    },
    delete_binding: |environment, vm, name| object_environment(environment).delete_binding(vm, name),
    is_object_environment: true,
    ..ENVIRONMENT_METHODS
};

impl ObjectEnvironment {
    /// The constructor that NewObjectEnvironment and NewGlobalEnvironment allocate through.
    pub(crate) fn create(
        vm: &Vm,
        binding_object: Gc<Object>,
        is_with_environment: IsWithEnvironment,
        outer_environment: Option<Gc<Environment>>,
    ) -> Gc<ObjectEnvironment> {
        vm.heap().allocate(ObjectEnvironment {
            base: Environment::new(Self::CLASS, outer_environment, false),
            binding_object,
            with_environment: is_with_environment == IsWithEnvironment::Yes,
        })
    }

    // 9.1.1.2.10 WithBaseObject ( ), https://tc39.es/ecma262/#sec-object-environment-records-withbaseobject
    pub fn with_base_object(&self) -> Option<Gc<Object>> {
        if self.is_with_environment() {
            return Some(self.binding_object);
        }
        None
    }

    // [[BindingObject]], The binding object of this Environment Record.
    pub fn binding_object(&self) -> Gc<Object> {
        self.binding_object
    }

    // [[IsWithEnvironment]], Indicates whether this Environment Record is created for a with statement.
    pub fn is_with_environment(&self) -> bool {
        self.with_environment
    }

    // 9.1.1.2.1 HasBinding ( N ), https://tc39.es/ecma262/#sec-object-environment-records-hasbinding-n
    pub fn has_binding(&self, vm: &Vm, name: &Utf16FlyString) -> ThrowCompletionOr<bool> {
        let name_key = PropertyKey::from(name.clone());

        // 1. Let bindingObject be envRec.[[BindingObject]].

        // 2. Let foundBinding be ? HasProperty(bindingObject, N).
        let found_binding = self.binding_object.has_property(vm, &name_key)?;

        // 3. If foundBinding is false, return false.
        if !found_binding {
            return Ok(false);
        }

        // 4. If envRec.[[IsWithEnvironment]] is false, return true.
        if !self.with_environment {
            return Ok(true);
        }

        // 5. Let unscopables be ? Get(bindingObject, @@unscopables).
        let unscopables = self
            .binding_object
            .get(vm, &PropertyKey::from_symbol(vm.well_known_symbols().unscopables))?;

        // 6. If Type(unscopables) is Object, then
        if unscopables.is_object() {
            // a. Let blocked be ToBoolean(? Get(unscopables, N)).
            let blocked = unscopables.as_object().get(vm, &name_key)?.to_boolean();

            // b. If blocked is true, return false.
            if blocked {
                return Ok(false);
            }
        }

        // 7. Return true.
        Ok(true)
    }

    // 9.1.1.2.2 CreateMutableBinding ( N, D ), https://tc39.es/ecma262/#sec-object-environment-records-createmutablebinding-n-d
    pub fn create_mutable_binding(
        &self,
        vm: &Vm,
        name: &Utf16FlyString,
        can_be_deleted: bool,
    ) -> ThrowCompletionOr<()> {
        // 1. Let bindingObject be envRec.[[BindingObject]].
        // 2. Perform ? DefinePropertyOrThrow(bindingObject, N, PropertyDescriptor { [[Value]]: undefined, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: D }).
        let mut descriptor = PropertyDescriptor {
            value: Some(Value::UNDEFINED),
            writable: Some(true),
            enumerable: Some(true),
            configurable: Some(can_be_deleted),
            ..Default::default()
        };
        self.binding_object
            .define_property_or_throw(vm, &PropertyKey::from(name.clone()), &mut descriptor)?;

        // 3. Return unused.
        Ok(())
    }

    // 9.1.1.2.3 CreateImmutableBinding ( N, S ), https://tc39.es/ecma262/#sec-object-environment-records-createimmutablebinding-n-s
    pub fn create_immutable_binding(&self, _vm: &Vm, _name: &Utf16FlyString, _strict: bool) -> ThrowCompletionOr<()> {
        // "The CreateImmutableBinding concrete method of an object Environment Record is never used within this specification."
        unreachable!("CreateImmutableBinding is never performed on an object environment")
    }

    // 9.1.1.2.4 InitializeBinding ( N, V ), https://tc39.es/ecma262/#sec-object-environment-records-initializebinding-n-v
    pub fn initialize_binding(
        &self,
        vm: &Vm,
        name: &Utf16FlyString,
        value: Value,
        hint: InitializeBindingHint,
    ) -> ThrowCompletionOr<()> {
        // 1. Assert: hint is normal.
        assert!(hint == InitializeBindingHint::Normal);

        // 2. Perform ? envRec.SetMutableBinding(N, V, false).
        self.set_mutable_binding(vm, name, value, false)?;

        // 2. Return unused.
        Ok(())
    }

    // 9.1.1.2.5 SetMutableBinding ( N, V, S ), https://tc39.es/ecma262/#sec-object-environment-records-setmutablebinding-n-v-s
    pub fn set_mutable_binding(
        &self,
        vm: &Vm,
        name: &Utf16FlyString,
        value: Value,
        strict: bool,
    ) -> ThrowCompletionOr<()> {
        let name_key = PropertyKey::from(name.clone());

        // OPTIMIZATION: For non-with environments in non-strict mode, we don't need the separate HasProperty check since we only use that
        //               information to throw errors in strict mode.
        //               We can't do this for with environments, since it would be observable (e.g via a Proxy)
        // FIXME: I think we could combine HasProperty and Set in strict mode if Set would return a bit more failure information.
        if !self.with_environment && !strict {
            return self.binding_object.set(vm, &name_key, value, ShouldThrowExceptions::No);
        }

        // 1. Let bindingObject be envRec.[[BindingObject]].
        // 2. Let stillExists be ? HasProperty(bindingObject, N).
        let still_exists = self.binding_object.has_property(vm, &name_key)?;

        // 3. If stillExists is false and S is true, throw a ReferenceError exception.
        if !still_exists && strict {
            return vm.throw_completion(ErrorKind::ReferenceError, ErrorType::UnknownIdentifier, &[name]);
        }

        // 4. Perform ? Set(bindingObject, N, V, S).
        let result_or_error = self.binding_object.set(
            vm,
            &name_key,
            value,
            if strict {
                ShouldThrowExceptions::Yes
            } else {
                ShouldThrowExceptions::No
            },
        );

        // Note: Nothing like this in the spec, this is here to produce nicer errors instead of the generic one thrown by Object::set().
        if let Err(error) = result_or_error
            && strict
        {
            let property_or_error = self.binding_object.internal_get_own_property(vm, &name_key);
            // Return the initial error instead of masking it with the new error
            let Ok(property) = property_or_error else {
                return Err(error);
            };
            if let Some(property) = property
                && !property.writable.unwrap_or(true)
            {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::DescWriteNonWritable, &[name]);
            }
        }

        result_or_error?;

        // 5. Return unused.
        Ok(())
    }

    // 9.1.1.2.6 GetBindingValue ( N, S ), https://tc39.es/ecma262/#sec-object-environment-records-getbindingvalue-n-s
    pub fn get_binding_value(&self, vm: &Vm, name: &Utf16FlyString, strict: bool) -> ThrowCompletionOr<Value> {
        let name_key = PropertyKey::from(name.clone());

        // OPTIMIZATION: For non-with environments in non-strict mode, we don't need the separate HasProperty check
        //               since Get will return undefined for missing properties anyway. So we take advantage of this
        //               to avoid doing both HasProperty and Get.
        //               We can't do this for with environments, since it would be observable (e.g via a Proxy)
        // FIXME: We could combine HasProperty and Get in non-strict mode if Get would return a bit more failure information.
        if !self.with_environment && !strict {
            return self.binding_object.get(vm, &name_key);
        }

        // 1. Let bindingObject be envRec.[[BindingObject]].
        // 2. Let value be ? HasProperty(bindingObject, N).
        let value = self.binding_object.has_property(vm, &name_key)?;

        // 3. If value is false, then
        if !value {
            // a. If S is false, return undefined; otherwise throw a ReferenceError exception.
            if !strict {
                return Ok(Value::UNDEFINED);
            }
            return vm.throw_completion(ErrorKind::ReferenceError, ErrorType::UnknownIdentifier, &[name]);
        }

        // 4. Return ? Get(bindingObject, N).
        self.binding_object.get(vm, &name_key)
    }

    // 9.1.1.2.7 DeleteBinding ( N ), https://tc39.es/ecma262/#sec-object-environment-records-deletebinding-n
    pub fn delete_binding(&self, vm: &Vm, name: &Utf16FlyString) -> ThrowCompletionOr<bool> {
        // 1. Let bindingObject be envRec.[[BindingObject]].
        // 2. Return ? bindingObject.[[Delete]](N).
        self.binding_object
            .internal_delete(vm, &PropertyKey::from(name.clone()))
    }
}
