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
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

const EPSILON_VALUE: f64 = f64::EPSILON;
const MAX_SAFE_INTEGER_VALUE: f64 = 9_007_199_254_740_991.0;
const MIN_SAFE_INTEGER_VALUE: f64 = -9_007_199_254_740_991.0;

#[repr(C)]
#[derive(Trace)]
pub struct NumberConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    NumberConstructor,
    initialize: NumberConstructor::initialize,
    call: NumberConstructor::call,
    construct: NumberConstructor::construct
);

impl NumberConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<NumberConstructor> {
        realm.create_object(
            vm,
            NumberConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Number.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 21.1.2.15 Number.prototype, https://tc39.es/ecma262/#sec-number.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().number_prototype(vm)),
            PropertyAttributes::new(0),
        );

        // NB: Number.isFinite, Number.isInteger, Number.isNaN and Number.isSafeInteger come with the Number
        //     builtins, and Number.parseInt and Number.parseFloat with the global object's functions.

        let constant = |name: &PropertyKey, value: Value| {
            object.define_direct_property(vm, name, value, PropertyAttributes::new(0));
        };
        let names = &vm.names;
        constant(&names.EPSILON, Value::from_f64(EPSILON_VALUE));
        constant(&names.MAX_VALUE, Value::from_f64(f64::MAX));
        constant(&names.MIN_VALUE, Value::from_f64(f64::from_bits(1)));
        constant(&names.MAX_SAFE_INTEGER, Value::from_f64(MAX_SAFE_INTEGER_VALUE));
        constant(&names.MIN_SAFE_INTEGER, Value::from_f64(MIN_SAFE_INTEGER_VALUE));
        constant(&names.NEGATIVE_INFINITY, Value::from_f64(f64::NEG_INFINITY));
        constant(&names.POSITIVE_INFINITY, Value::from_f64(f64::INFINITY));
        constant(&names.NaN, Value::from_f64(f64::NAN));

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 21.1.1.1 Number ( value ), https://tc39.es/ecma262/#sec-number-constructor-number-value
    fn call(_: &NativeFunction, _: &Vm) -> ThrowCompletionOr<Value> {
        unimplemented_runtime_function("NumberConstructor::call, the [[Call]] of %Number%", 0)
    }

    // 21.1.1.1 Number ( value ), https://tc39.es/ecma262/#sec-number-constructor-number-value
    fn construct(_: &NativeFunction, _: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        unimplemented_runtime_function("NumberConstructor::construct, the [[Construct]] of %Number%", 0)
    }
}
