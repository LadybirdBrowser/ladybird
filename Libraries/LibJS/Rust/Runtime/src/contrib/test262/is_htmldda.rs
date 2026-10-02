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
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{
    NATIVE_FUNCTION_METHODS, NATIVE_FUNCTION_VIRTUAL_METHODS, NativeFunction, NativeFunctionMethods,
};
use crate::runtime::object::{Object, ObjectMethods};
use crate::runtime::realm::Realm;

/// An object with an [[IsHTMLDDA]] internal slot, as test262's INTERPRETING.md asks hosts that can to provide.
#[repr(C)]
#[derive(Trace)]
pub struct IsHTMLDDA {
    base: NativeFunction,
}

static IS_HTMLDDA_VIRTUAL_METHODS: NativeFunctionMethods = NativeFunctionMethods {
    call: IsHTMLDDA::call,
    ..NATIVE_FUNCTION_VIRTUAL_METHODS
};

static IS_HTMLDDA_METHODS: ObjectMethods = ObjectMethods {
    native_function: Some(&IS_HTMLDDA_VIRTUAL_METHODS),
    ..NATIVE_FUNCTION_METHODS
};

define_cell!(
    IsHTMLDDA,
    Object,
    extends: [NativeFunction, FunctionObject, Object],
    methods: IS_HTMLDDA_METHODS
);

impl Deref for IsHTMLDDA {
    type Target = NativeFunction;

    fn deref(&self) -> &NativeFunction {
        &self.base
    }
}

impl IsHTMLDDA {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<IsHTMLDDA> {
        // NativeFunction without prototype is currently not possible (only due to the lack of a ctor that supports it)
        let base = NativeFunction::new_with_name(
            vm,
            Self::CLASS,
            Utf16FlyString::from_utf8("IsHTMLDDA"),
            realm.function_prototype(),
        );
        base.set_is_htmldda();
        realm.create_object(vm, IsHTMLDDA { base })
    }

    #[allow(clippy::unnecessary_wraps, reason = "NativeFunction::call returns a completion")]
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        if vm.argument_count() == 0 {
            return Ok(Value::NULL);
        }
        if vm.argument(0).is_string() && vm.argument(0).as_string().is_empty() {
            return Ok(Value::NULL);
        }
        // Not sure if this really matters, INTERPRETING.md simply says:
        // * IsHTMLDDA - (present only in implementations that can provide it) an object that:
        //   a. has an [[IsHTMLDDA]] internal slot, and
        //   b. when called with no arguments or with the first argument "" (an empty string) returns null.
        Ok(Value::UNDEFINED)
    }
}
