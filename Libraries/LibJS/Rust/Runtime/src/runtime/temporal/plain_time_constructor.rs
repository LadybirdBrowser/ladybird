/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! %Temporal.PlainTime%.

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::runtime::temporal::abstract_operations::to_integer_with_truncation;
use crate::runtime::temporal::plain_time::{
    compare_time_record, create_temporal_time, create_time_record, is_valid_time, to_temporal_time,
};

// 4.1 The Temporal.PlainTime Constructor, https://tc39.es/proposal-temporal/#sec-temporal-plaintime-constructor
#[repr(C)]
#[derive(Trace)]
pub struct PlainTimeConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    PlainTimeConstructor,
    initialize: PlainTimeConstructor::initialize,
    call: PlainTimeConstructor::call,
    construct: PlainTimeConstructor::construct
);

impl PlainTimeConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PlainTimeConstructor> {
        realm.create_object(
            vm,
            PlainTimeConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.PlainTime.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 4.2.1 Temporal.PlainTime.prototype, https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().temporal_plain_time_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.from,
            raw_native!(PlainTimeConstructor::from),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.compare,
            raw_native!(PlainTimeConstructor::compare),
            2,
            attr,
            None,
        );

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 4.1.1 Temporal.PlainTime ( [ hour [ , minute [ , second [ , millisecond [ , microsecond [ , nanosecond ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Temporal.PlainTime"],
        )
    }

    // 4.1.1 Temporal.PlainTime ( [ hour [ , minute [ , second [ , millisecond [ , microsecond [ , nanosecond ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let mut index = 0;
        let mut next_integer_argument = || -> ThrowCompletionOr<f64> {
            let value = vm.argument(index);
            index += 1;
            if !value.is_undefined() {
                return to_integer_with_truncation(vm, value, ErrorType::TemporalInvalidPlainTime, &[]);
            }
            Ok(0.0)
        };

        // 2. If hour is undefined, set hour to 0; else set hour to ? ToIntegerWithTruncation(hour).
        let hour = next_integer_argument()?;

        // 3. If minute is undefined, set minute to 0; else set minute to ? ToIntegerWithTruncation(minute).
        let minute = next_integer_argument()?;

        // 4. If second is undefined, set second to 0; else set second to ? ToIntegerWithTruncation(second).
        let second = next_integer_argument()?;

        // 5. If millisecond is undefined, set millisecond to 0; else set millisecond to ? ToIntegerWithTruncation(millisecond).
        let millisecond = next_integer_argument()?;

        // 6. If microsecond is undefined, set microsecond to 0; else set microsecond to ? ToIntegerWithTruncation(microsecond).
        let microsecond = next_integer_argument()?;

        // 7. If nanosecond is undefined, set nanosecond to 0; else set nanosecond to ? ToIntegerWithTruncation(nanosecond).
        let nanosecond = next_integer_argument()?;

        // 8. If IsValidTime(hour, minute, second, millisecond, microsecond, nanosecond) is false, throw a RangeError exception.
        if !is_valid_time(hour, minute, second, millisecond, microsecond, nanosecond) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainTime, &[]);
        }

        // 9. Let time be CreateTimeRecord(hour, minute, second, millisecond, microsecond, nanosecond).
        let time = create_time_record(hour, minute, second, millisecond, microsecond, nanosecond, 0.0);

        // 10. Return ? CreateTemporalTime(time, NewTarget).
        Ok(create_temporal_time(vm, time, Some(new_target))?.upcast())
    }

    // 4.2.2 Temporal.PlainTime.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.from
    fn from(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 4. Return ? ToTemporalTime(item, overflow).
        Ok(Value::from_object(to_temporal_time(
            vm,
            vm.argument(0),
            vm.argument(1),
        )?))
    }

    // 4.2.3 Temporal.PlainTime.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.compare
    fn compare(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Set one to ? ToTemporalTime(one).
        let one = to_temporal_time(vm, vm.argument(0), Value::UNDEFINED)?;

        // 2. Set two to ? ToTemporalTime(two).
        let two = to_temporal_time(vm, vm.argument(1), Value::UNDEFINED)?;

        // 3. Return 𝔽(CompareTimeRecord(one.[[Time]], two.[[Time]])).
        Ok(Value::from_i32(i32::from(compare_time_record(
            &one.time(),
            &two.time(),
        ))))
    }
}
