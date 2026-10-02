/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/InstantConstructor.cpp: %Temporal.Instant%.

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::big_int::{BigInt, number_to_bigint};
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::runtime::temporal::instant::{
    NANOSECONDS_PER_MILLISECOND, compare_epoch_nanoseconds, create_temporal_instant, is_valid_epoch_nanoseconds,
    to_temporal_instant,
};

// 8.1 The Temporal.Instant Constructor, https://tc39.es/proposal-temporal/#sec-temporal-instant-constructor
#[repr(C)]
#[derive(Trace)]
pub struct InstantConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    InstantConstructor,
    initialize: InstantConstructor::initialize,
    call: InstantConstructor::call,
    construct: InstantConstructor::construct
);

impl InstantConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<InstantConstructor> {
        realm.create_object(
            vm,
            InstantConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Instant.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 8.2.1 Temporal.Instant.prototype, https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().temporal_instant_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };
        define_native_function(&names.from, raw_native!(InstantConstructor::from), 1);
        define_native_function(
            &names.fromEpochMilliseconds,
            raw_native!(InstantConstructor::from_epoch_milliseconds),
            1,
        );
        define_native_function(
            &names.fromEpochNanoseconds,
            raw_native!(InstantConstructor::from_epoch_nanoseconds),
            1,
        );
        define_native_function(&names.compare, raw_native!(InstantConstructor::compare), 2);

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 8.1.1 Temporal.Instant ( epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal.instant
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Temporal.Instant"],
        )
    }

    // 8.1.1 Temporal.Instant ( epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal.instant
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        // 2. Let epochNanoseconds be ? ToBigInt(epochNanoseconds).
        let epoch_nanoseconds = vm.argument(0).to_bigint(vm)?;

        // 3. If ! IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
        if !is_valid_epoch_nanoseconds(epoch_nanoseconds.big_integer()) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidEpochNanoseconds, &[]);
        }

        // 4. Return ? CreateTemporalInstant(epochNanoseconds, NewTarget).
        Ok(create_temporal_instant(vm, epoch_nanoseconds, Some(new_target))?.upcast())
    }

    // 8.2.2 Temporal.Instant.from ( item ), https://tc39.es/proposal-temporal/#sec-temporal.instant.from
    fn from(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? ToTemporalInstant(item).
        Ok(Value::from_object(to_temporal_instant(vm, vm.argument(0))?))
    }

    // 8.2.4 Temporal.Instant.fromEpochMilliseconds ( epochMilliseconds ), https://tc39.es/proposal-temporal/#sec-temporal.instant.fromepochmilliseconds
    fn from_epoch_milliseconds(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Set epochMilliseconds to ? ToNumber(epochMilliseconds).
        let epoch_milliseconds_value = vm.argument(0).to_number(vm)?;

        // 2. Set epochMilliseconds to ? NumberToBigInt(epochMilliseconds).
        let epoch_milliseconds = number_to_bigint(vm, epoch_milliseconds_value)?;

        // 3. Let epochNanoseconds be epochMilliseconds × ℤ(10**6).
        let epoch_nanoseconds = epoch_milliseconds.big_integer() * &*NANOSECONDS_PER_MILLISECOND;

        // 4. If IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
        if !is_valid_epoch_nanoseconds(&epoch_nanoseconds) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidEpochNanoseconds, &[]);
        }

        // 5. Return ! CreateTemporalInstant(epochNanoseconds).
        Ok(Value::from_object(
            create_temporal_instant(vm, BigInt::create(vm, epoch_nanoseconds), None).must(),
        ))
    }

    // 8.2.6 Temporal.Instant.fromEpochNanoseconds ( epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal.instant.fromepochnanoseconds
    fn from_epoch_nanoseconds(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Set epochNanoseconds to ? ToBigInt(epochNanoseconds).
        let epoch_nanoseconds = vm.argument(0).to_bigint(vm)?;

        // 2. If IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
        if !is_valid_epoch_nanoseconds(epoch_nanoseconds.big_integer()) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidEpochNanoseconds, &[]);
        }

        // 3. Return ! CreateTemporalInstant(epochNanoseconds).
        Ok(Value::from_object(
            create_temporal_instant(vm, epoch_nanoseconds, None).must(),
        ))
    }

    // 8.2.7 Temporal.Instant.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.instant.compare
    fn compare(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Set one to ? ToTemporalInstant(one).
        let one = to_temporal_instant(vm, vm.argument(0))?;

        // 2. Set two to ? ToTemporalInstant(two).
        let two = to_temporal_instant(vm, vm.argument(1))?;

        // 3. Return 𝔽(CompareEpochNanoseconds(one.[[EpochNanoseconds]], two.[[EpochNanoseconds]])).
        Ok(Value::from_i32(i32::from(compare_epoch_nanoseconds(
            one.epoch_nanoseconds().big_integer(),
            two.epoch_nanoseconds().big_integer(),
        ))))
    }
}
