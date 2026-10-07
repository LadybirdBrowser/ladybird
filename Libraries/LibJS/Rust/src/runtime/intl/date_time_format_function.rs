/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intl::date_time_format::{
    DateTimeFormat, FormattableDateTime, format_date_time, to_date_time_formattable,
};
use crate::runtime::native_function::{
    NATIVE_FUNCTION_METHODS, NATIVE_FUNCTION_VIRTUAL_METHODS, NativeFunction, NativeFunctionMethods,
};
use crate::runtime::object::ObjectMethods;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct DateTimeFormatFunction {
    base: NativeFunction,
    date_time_format: Cell<Gc<DateTimeFormat>>, // [[DateTimeFormat]]
}

static DATE_TIME_FORMAT_FUNCTION_VIRTUAL_METHODS: NativeFunctionMethods = NativeFunctionMethods {
    call: DateTimeFormatFunction::call,
    ..NATIVE_FUNCTION_VIRTUAL_METHODS
};

static DATE_TIME_FORMAT_FUNCTION_METHODS: ObjectMethods = ObjectMethods {
    initialize: DateTimeFormatFunction::initialize,
    native_function: Some(&DATE_TIME_FORMAT_FUNCTION_VIRTUAL_METHODS),
    ..NATIVE_FUNCTION_METHODS
};

define_cell!(
    DateTimeFormatFunction,
    Object,
    extends: [NativeFunction, FunctionObject, Object],
    methods: DATE_TIME_FORMAT_FUNCTION_METHODS
);

impl Deref for DateTimeFormatFunction {
    type Target = NativeFunction;

    fn deref(&self) -> &NativeFunction {
        &self.base
    }
}

impl DateTimeFormatFunction {
    // 11.5.4 DateTime Format Functions, https://tc39.es/ecma402/#sec-datetime-format-functions
    // 15.6.3 DateTime Format Functions, https://tc39.es/proposal-temporal/#sec-datetime-format-functions
    pub fn create(vm: &Vm, realm: Gc<Realm>, date_time_format: Gc<DateTimeFormat>) -> Gc<DateTimeFormatFunction> {
        realm.create_object(
            vm,
            DateTimeFormatFunction {
                base: NativeFunction::new_with_prototype(vm, Self::CLASS, realm.function_prototype()),
                date_time_format: Cell::new(date_time_format),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        (NATIVE_FUNCTION_METHODS.initialize)(object, vm, realm);
        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
        object.define_direct_property(
            vm,
            &vm.names.name,
            Value::from_string(vm.empty_string()),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        assert!(function.is::<DateTimeFormatFunction>());
        // SAFETY: The function is a DateTimeFormatFunction, which starts with its NativeFunction.
        let function = unsafe { &*core::ptr::from_ref(function).cast::<DateTimeFormatFunction>() };
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        let date_value = vm.argument(0);

        // 1. Let dtf be F.[[DateTimeFormat]].
        // 2. Assert: Type(dtf) is Object and dtf has an [[InitializedDateTimeFormat]] internal slot.
        let date_time_format = function.date_time_format.get();

        // 3. If date is not provided or is undefined, then
        let date = if date_value.is_undefined() {
            // a. Let x be ! Call(%Date.now%, undefined).
            FormattableDateTime::Number(
                call(
                    vm,
                    Value::from_object(realm.intrinsics().date_constructor_now_function().upcast::<Object>()),
                    Value::UNDEFINED,
                    &[],
                )
                .must()
                .as_f64(),
            )
        }
        // 4. Else,
        else {
            // a. Let x be ? ToDateTimeFormattable(date).
            to_date_time_formattable(vm, date_value)?
        };

        // 5. Return ? FormatDateTime(dtf, x).
        let formatted = format_date_time(vm, &date_time_format, &date)?;
        Ok(Value::from_string(PrimitiveString::create(vm, formatted)))
    }
}
