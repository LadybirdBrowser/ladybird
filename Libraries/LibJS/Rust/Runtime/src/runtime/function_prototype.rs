/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_abi::Builtin;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::{FUNCTION_OBJECT_METHODS, FunctionObject};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::value::ordinary_has_instance;

/// %Function.prototype%, a function object that accepts any arguments and returns undefined.
#[repr(C)]
#[derive(Trace)]
pub struct FunctionPrototype {
    base: FunctionObject,
}

define_object_class!(FunctionPrototype, extends: [FunctionObject, Object], methods: {
    initialize: FunctionPrototype::initialize,
    internal_call: Some(FunctionPrototype::internal_call),
    name_for_call_stack: |_| Utf16String::from_utf8("(Function.prototype)"),
    ..FUNCTION_OBJECT_METHODS
});

impl FunctionPrototype {
    pub fn new(vm: &Vm, realm: Gc<Realm>) -> FunctionPrototype {
        FunctionPrototype {
            base: FunctionObject::new_with_prototype(
                vm,
                Self::CLASS,
                realm.object_prototype(),
                MayInterfereWithIndexedPropertyAccess::No,
            ),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<FunctionPrototype> {
        realm.create_object(vm, Self::new(vm, realm))
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // NB: apply, bind, call and toString come with the Function.prototype builtins, before @@hasInstance.
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().has_instance),
            raw_native!(FunctionPrototype::symbol_has_instance),
            1,
            PropertyAttributes::new(0),
            Some(Builtin::OrdinaryHasInstance),
        );
        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
        object.define_direct_property(
            vm,
            &vm.names.name,
            Value::from_string(PrimitiveString::create(vm, Utf16String::default())),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 20.2.3.6 Function.prototype [ @@hasInstance ] ( V ), https://tc39.es/ecma262/#sec-function.prototype-@@hasinstance
    fn symbol_has_instance(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let F be the this value.
        // 2. Return ? OrdinaryHasInstance(F, V).
        ordinary_has_instance(vm, vm.argument(0), vm.this_value())
    }

    #[allow(clippy::unnecessary_wraps, reason = "[[Call]] can throw for other functions")]
    fn internal_call(_: &Object, _: &Vm, _: &ExecutionContext, _: Value) -> ThrowCompletionOr<Value> {
        // The Function prototype object:
        // - accepts any arguments and returns undefined when invoked.
        Ok(Value::UNDEFINED)
    }
}
