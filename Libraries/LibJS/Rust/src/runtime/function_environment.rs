/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::declarative_environment::{DECLARATIVE_ENVIRONMENT_METHODS, DeclarativeEnvironment};
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;
use crate::runtime::environment::{Environment, EnvironmentMethods, ThisBindingStatus};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;

#[repr(C)]
#[derive(Trace)]
pub struct FunctionEnvironment {
    base: DeclarativeEnvironment,
    this_value: Cell<Value>,                           // [[ThisValue]]
    function_object: Cell<Option<Gc<FunctionObject>>>, // [[FunctionObject]]
    new_target: Cell<Value>,                           // [[NewTarget]]
}

define_cell!(FunctionEnvironment, Other, extends: [DeclarativeEnvironment, Environment], finalize: finalize);

impl Finalize for FunctionEnvironment {
    fn finalize(&self) {
        self.base.finalize();
    }
}

impl Deref for FunctionEnvironment {
    type Target = DeclarativeEnvironment;

    fn deref(&self) -> &DeclarativeEnvironment {
        &self.base
    }
}

fn function_environment(environment: &Environment) -> &FunctionEnvironment {
    environment
        .downcast_ref::<FunctionEnvironment>()
        .expect("only function environments have the function environment methods")
}

/// The methods JS::FunctionEnvironment overrides.
pub const FUNCTION_ENVIRONMENT_METHODS: EnvironmentMethods = EnvironmentMethods {
    has_this_binding: |environment| function_environment(environment).has_this_binding(),
    get_this_binding: |environment, vm| function_environment(environment).get_this_binding(vm),
    is_function_environment: true,
    ..DECLARATIVE_ENVIRONMENT_METHODS
};

impl FunctionEnvironment {
    pub fn create(vm: &Vm, outer_environment: Option<Gc<Environment>>) -> Gc<FunctionEnvironment> {
        vm.heap().allocate(FunctionEnvironment {
            base: DeclarativeEnvironment::new(Self::CLASS, outer_environment),
            this_value: Cell::new(Value::UNDEFINED),
            function_object: Cell::new(None),
            new_target: Cell::new(Value::UNDEFINED),
        })
    }

    pub fn this_binding_status(&self) -> ThisBindingStatus {
        ThisBindingStatus::from_raw(self.base.base.this_binding_status.get())
    }

    pub fn set_this_binding_status(&self, status: ThisBindingStatus) {
        self.base.base.this_binding_status.set(status as u8);
    }

    pub fn function_object(&self) -> Gc<FunctionObject> {
        self.function_object
            .get()
            .expect("a function environment has a function object")
    }

    pub fn set_function_object(&self, function: Gc<FunctionObject>) {
        self.function_object.set(Some(function));
    }

    pub fn new_target(&self) -> Value {
        self.new_target.get()
    }

    pub fn set_new_target(&self, new_target: Value) {
        assert!(!new_target.is_empty());
        self.new_target.set(new_target);
    }

    // 9.1.1.3.5 GetSuperBase ( ), https://tc39.es/ecma262/#sec-getsuperbase
    pub fn get_super_base(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        let function_object = self.function_object();

        // 1. Let home be envRec.[[FunctionObject]].[[HomeObject]].
        let Some(ecmascript_function_object) = as_ecmascript_function_object(function_object) else {
            return Ok(Value::UNDEFINED);
        };

        let home_object = ecmascript_function_object.home_object();

        // 2. If home is undefined, return undefined.
        let Some(home_object) = home_object else {
            return Ok(Value::UNDEFINED);
        };

        // 3. Assert: Type(home) is Object.

        // 4. Return ? home.[[GetPrototypeOf]]().
        Ok(home_object
            .internal_get_prototype_of(vm)?
            .map_or(Value::NULL, Value::from_object))
    }

    // 9.1.1.3.2 HasThisBinding ( ), https://tc39.es/ecma262/#sec-function-environment-records-hasthisbinding
    pub fn has_this_binding(&self) -> bool {
        self.this_binding_status() != ThisBindingStatus::Lexical
    }

    // 9.1.1.3.3 HasSuperBinding ( ), https://tc39.es/ecma262/#sec-function-environment-records-hassuperbinding
    pub fn has_super_binding(&self) -> bool {
        if self.this_binding_status() == ThisBindingStatus::Lexical {
            return false;
        }
        as_ecmascript_function_object(self.function_object())
            .is_some_and(|ecmascript_function_object| ecmascript_function_object.home_object().is_some())
    }

    // 9.1.1.3.4 GetThisBinding ( ), https://tc39.es/ecma262/#sec-function-environment-records-getthisbinding
    pub fn get_this_binding(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Assert: envRec.[[ThisBindingStatus]] is not lexical.
        assert!(self.this_binding_status() != ThisBindingStatus::Lexical);

        // 2. If envRec.[[ThisBindingStatus]] is uninitialized, throw a ReferenceError exception.
        if self.this_binding_status() == ThisBindingStatus::Uninitialized {
            return vm.throw_completion(ErrorKind::ReferenceError, ErrorType::ThisHasNotBeenInitialized, &[]);
        }

        // 3. Return envRec.[[ThisValue]].
        Ok(self.this_value.get())
    }

    // 9.1.1.3.1 BindThisValue ( V ), https://tc39.es/ecma262/#sec-bindthisvalue
    pub fn bind_this_value(&self, vm: &Vm, this_value: Value) -> ThrowCompletionOr<Value> {
        assert!(!this_value.is_empty());

        // 1. Assert: envRec.[[ThisBindingStatus]] is not lexical.
        assert!(self.this_binding_status() != ThisBindingStatus::Lexical);

        // 2. If envRec.[[ThisBindingStatus]] is initialized, throw a ReferenceError exception.
        if self.this_binding_status() == ThisBindingStatus::Initialized {
            return vm.throw_completion(ErrorKind::ReferenceError, ErrorType::ThisIsAlreadyInitialized, &[]);
        }

        // 3. Set envRec.[[ThisValue]] to V.
        self.this_value.set(this_value);

        // 4. Set envRec.[[ThisBindingStatus]] to initialized.
        self.set_this_binding_status(ThisBindingStatus::Initialized);

        // 5. Return V.
        Ok(this_value)
    }
}
