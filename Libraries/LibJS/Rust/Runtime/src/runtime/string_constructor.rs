/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::utf16::Utf16View;

#[repr(C)]
#[derive(Trace)]
pub struct StringConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    StringConstructor,
    initialize: StringConstructor::initialize,
    call: StringConstructor::call,
    construct: StringConstructor::construct
);

impl StringConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<StringConstructor> {
        realm.create_object(
            vm,
            StringConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.String.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 22.1.2.3 String.prototype, https://tc39.es/ecma262/#sec-string.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().string_prototype(vm)),
            PropertyAttributes::new(0),
        );

        // NB: String.raw, String.fromCharCode and String.fromCodePoint come with the String builtins.

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 22.1.1.1 String ( value ), https://tc39.es/ecma262/#sec-string-constructor-string-value
    fn call(_: &NativeFunction, _: &Vm) -> ThrowCompletionOr<Value> {
        unimplemented_runtime_function("StringConstructor::call, the [[Call]] of %String%", 0)
    }

    // 22.1.1.1 String ( value ), https://tc39.es/ecma262/#sec-string-constructor-string-value
    fn construct(_: &NativeFunction, _: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        unimplemented_runtime_function("StringConstructor::construct, the [[Construct]] of %String%", 0)
    }
}

pub fn from_char_code_impl(vm: &Vm, code_unit: Value) -> ThrowCompletionOr<Value> {
    let value = code_unit.to_u16(vm)?;
    Ok(Value::from_string(PrimitiveString::create_from_utf16_view(
        vm,
        Utf16View::Utf16(&[value]),
    )))
}
