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
use crate::runtime::boolean_object::BooleanObject;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;

/// %Boolean.prototype%, which is a Boolean object whose [[BooleanData]] is false.
#[repr(C)]
#[derive(Trace)]
pub struct BooleanPrototype {
    base: BooleanObject,
}

define_object_class!(BooleanPrototype, extends: [BooleanObject, Object], methods: {
    initialize: BooleanPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

// thisBooleanValue ( value ), https://tc39.es/ecma262/#thisbooleanvalue
fn this_boolean_value(vm: &Vm, value: Value) -> ThrowCompletionOr<bool> {
    // 1. If value is a Boolean, return value.
    if value.is_boolean() {
        return Ok(value.as_bool());
    }

    // 2. If value is an Object and value has a [[BooleanData]] internal slot, then
    if value.is_object()
        && let Some(boolean) = value.as_object().downcast::<BooleanObject>()
    {
        // a. Let b be value.[[BooleanData]].
        // b. Assert: b is a Boolean.
        // c. Return b.
        return Ok(boolean.boolean());
    }

    // 3. Throw a TypeError exception.
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"Boolean"])
}

impl BooleanPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<BooleanPrototype> {
        realm.create_object(
            vm,
            BooleanPrototype {
                base: BooleanObject::new(vm, Self::CLASS, false, realm.object_prototype()),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &vm.names.toString,
            raw_native!(BooleanPrototype::to_string),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &vm.names.valueOf,
            raw_native!(BooleanPrototype::value_of),
            0,
            attr,
            None,
        );
    }

    // 20.3.3.2 Boolean.prototype.toString ( ), https://tc39.es/ecma262/#sec-boolean.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let b be ? thisBooleanValue(this value).
        let b = this_boolean_value(vm, vm.this_value())?;

        // 2. If b is true, return "true"; else return "false".
        Ok(Value::from_string(PrimitiveString::create_from_utf8(
            vm,
            if b { "true" } else { "false" },
        )))
    }

    // 20.3.3.3 Boolean.prototype.valueOf ( ), https://tc39.es/ecma262/#sec-boolean.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? thisBooleanValue(this value).
        Ok(Value::from_bool(this_boolean_value(vm, vm.this_value())?))
    }
}
