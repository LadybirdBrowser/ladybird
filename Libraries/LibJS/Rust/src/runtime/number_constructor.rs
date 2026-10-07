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
use crate::runtime::abstract_operations::get_prototype_from_constructor;
use crate::runtime::big_int_algorithms;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::number_object::NumberObject;
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

// Most of 21.1.1.1 Number ( value ) factored into a separate function for sharing between call() and construct().
fn get_value_from_constructor_argument(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. If value is present, then
    if vm.argument_count() > 0 {
        // a. Let prim be ? ToNumeric(value).
        let primitive = vm.argument(0).to_numeric(vm)?;

        // b. If Type(prim) is BigInt, let n be 𝔽(ℝ(prim)).
        if primitive.is_bigint() {
            return Ok(Value::from_f64(big_int_algorithms::to_double(
                primitive.as_bigint().big_integer(),
            )));
        }

        // c. Otherwise, let n be prim.
        return Ok(primitive);
    }

    // 2. Else,
    //     a. Let n be +0𝔽.
    Ok(Value::from_i32(0))
}

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
        let names = &vm.names;

        // 21.1.2.15 Number.prototype, https://tc39.es/ecma262/#sec-number.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().number_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.isFinite,
            raw_native!(NumberConstructor::is_finite),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.isInteger,
            raw_native!(NumberConstructor::is_integer),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.isNaN,
            raw_native!(NumberConstructor::is_nan),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.isSafeInteger,
            raw_native!(NumberConstructor::is_safe_integer),
            1,
            attr,
            None,
        );
        object.define_direct_property(
            vm,
            &names.parseInt,
            Value::from_object(realm.intrinsics().parse_int_function()),
            attr,
        );
        object.define_direct_property(
            vm,
            &names.parseFloat,
            Value::from_object(realm.intrinsics().parse_float_function()),
            attr,
        );

        let constant = |name: &PropertyKey, value: f64| {
            object.define_direct_property(vm, name, Value::from_f64(value), PropertyAttributes::new(0));
        };
        constant(&names.EPSILON, EPSILON_VALUE);
        constant(&names.MAX_VALUE, f64::MAX);
        constant(&names.MIN_VALUE, f64::from_bits(1));
        constant(&names.MAX_SAFE_INTEGER, MAX_SAFE_INTEGER_VALUE);
        constant(&names.MIN_SAFE_INTEGER, MIN_SAFE_INTEGER_VALUE);
        constant(&names.NEGATIVE_INFINITY, f64::NEG_INFINITY);
        constant(&names.POSITIVE_INFINITY, f64::INFINITY);
        constant(&names.NaN, f64::NAN);

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 21.1.1.1 Number ( value ), https://tc39.es/ecma262/#sec-number-constructor-number-value
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // NOTE: get_value_from_constructor_argument performs steps 1 and 2 and returns n.
        // 3. If NewTarget is undefined, return n.
        get_value_from_constructor_argument(vm)
    }

    // 21.1.1.1 Number ( value ), https://tc39.es/ecma262/#sec-number-constructor-number-value
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");

        // NOTE: get_value_from_constructor_argument performs steps 1 and 2 and returns n.
        let number = get_value_from_constructor_argument(vm)?;

        // 4. Let O be ? OrdinaryCreateFromConstructor(NewTarget, "%Number.prototype%", « [[NumberData]] »).
        // 5. Set O.[[NumberData]] to n.
        // 6. Return O.
        let prototype = get_prototype_from_constructor(vm, new_target, Intrinsics::number_prototype)?;
        Ok(realm
            .create_object(
                vm,
                NumberObject::new(vm, NumberObject::CLASS, number.as_f64(), prototype),
            )
            .upcast())
    }

    // 21.1.2.2 Number.isFinite ( number ), https://tc39.es/ecma262/#sec-number.isfinite
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn is_finite(vm: &Vm) -> ThrowCompletionOr<Value> {
        let number = vm.argument(0);

        // 1. If number is not a Number, return false.
        // 2. If number is not finite, return false.
        // 3. Otherwise, return true.
        Ok(Value::from_bool(number.is_finite_number()))
    }

    // 21.1.2.3 Number.isInteger ( number ), https://tc39.es/ecma262/#sec-number.isinteger
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn is_integer(vm: &Vm) -> ThrowCompletionOr<Value> {
        let number = vm.argument(0);

        // 1. Return IsIntegralNumber(number).
        Ok(Value::from_bool(number.is_integral_number()))
    }

    // 21.1.2.4 Number.isNaN ( number ), https://tc39.es/ecma262/#sec-number.isnan
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn is_nan(vm: &Vm) -> ThrowCompletionOr<Value> {
        let number = vm.argument(0);

        // 1. If number is not a Number, return false.
        // 2. If number is NaN, return true.
        // 3. Otherwise, return false.
        Ok(Value::from_bool(number.is_nan()))
    }

    // 21.1.2.5 Number.isSafeInteger ( number ), https://tc39.es/ecma262/#sec-number.issafeinteger
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn is_safe_integer(vm: &Vm) -> ThrowCompletionOr<Value> {
        let number = vm.argument(0);

        // 1. If IsIntegralNumber(number) is true, then
        if number.is_integral_number() {
            // a. If abs(ℝ(number)) ≤ 2^53 - 1, return true.
            if number.as_f64().abs() <= MAX_SAFE_INTEGER_VALUE {
                return Ok(Value::TRUE);
            }
        }

        // 2. Return false.
        Ok(Value::FALSE)
    }
}
