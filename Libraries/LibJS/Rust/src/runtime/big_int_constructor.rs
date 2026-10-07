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
use crate::runtime::big_int::{BigInt, number_to_bigint};
use crate::runtime::big_int_algorithms;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::runtime::value::PreferredType;

#[repr(C)]
#[derive(Trace)]
pub struct BigIntConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    BigIntConstructor,
    initialize: BigIntConstructor::initialize,
    call: BigIntConstructor::call,
    construct: BigIntConstructor::construct
);

impl BigIntConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<BigIntConstructor> {
        realm.create_object(
            vm,
            BigIntConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.BigInt.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 21.2.2.3 BigInt.prototype, https://tc39.es/ecma262/#sec-bigint.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().bigint_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &vm.names.asIntN,
            raw_native!(BigIntConstructor::as_int_n),
            2,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &vm.names.asUintN,
            raw_native!(BigIntConstructor::as_uint_n),
            2,
            attr,
            None,
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 21.2.1.1 BigInt ( value ), https://tc39.es/ecma262/#sec-bigint-constructor-number-value
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 2. Let prim be ? ToPrimitive(value, number).
        let primitive = value.to_primitive(vm, PreferredType::Number)?;

        // 3. If Type(prim) is Number, return ? NumberToBigInt(prim).
        if primitive.is_number() {
            return Ok(Value::from_bigint(number_to_bigint(vm, primitive)?));
        }

        // 4. Otherwise, return ? ToBigInt(prim).
        Ok(Value::from_bigint(primitive.to_bigint(vm)?))
    }

    // 21.2.1.1 BigInt ( value ), https://tc39.es/ecma262/#sec-bigint-constructor-number-value
    fn construct(_: &NativeFunction, vm: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAConstructor, &[&"BigInt"])
    }

    // 21.2.2.1 BigInt.asIntN ( bits, bigint ), https://tc39.es/ecma262/#sec-bigint.asintn
    fn as_int_n(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Set bits to ? ToIndex(bits).
        let bits = vm.argument(0).to_index(vm)?;

        // 2. Set bigint to ? ToBigInt(bigint).
        let bigint = vm.argument(1).to_bigint(vm)?;

        // 3-5. NB: big_int_algorithms::as_int_n() performs these steps.
        match big_int_algorithms::as_int_n(bits, bigint.big_integer()) {
            Ok(result) => Ok(Value::from_bigint(BigInt::create(vm, result))),
            Err(error) => error.throw_completion(vm),
        }
    }

    // 21.2.2.2 BigInt.asUintN ( bits, bigint ), https://tc39.es/ecma262/#sec-bigint.asuintn
    fn as_uint_n(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Set bits to ? ToIndex(bits).
        let bits = vm.argument(0).to_index(vm)?;

        // 2. Set bigint to ? ToBigInt(bigint).
        let bigint = vm.argument(1).to_bigint(vm)?;

        // 3. Return the BigInt value that represents ℝ(bigint) modulo 2^bits.
        match big_int_algorithms::as_uint_n(bits, bigint.big_integer()) {
            Ok(result) => Ok(Value::from_bigint(BigInt::create(vm, result))),
            Err(error) => error.throw_completion(vm),
        }
    }
}
