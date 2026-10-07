/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! %Temporal.ZonedDateTime%.

use ak::Utf16String;
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
use crate::runtime::intl::abstract_operations::get_available_named_time_zone_identifier;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::temporal::calendar::canonicalize_calendar;
use crate::runtime::temporal::instant::{compare_epoch_nanoseconds, is_valid_epoch_nanoseconds};
use crate::runtime::temporal::time_zone::{format_offset_time_zone_identifier, parse_time_zone_identifier_or_throw};
use crate::runtime::temporal::zoned_date_time::{create_temporal_zoned_date_time, to_temporal_zoned_date_time};
use crate::utf16::Utf16View;

// 6.1 The Temporal.ZonedDateTime Constructor, https://tc39.es/proposal-temporal/#sec-temporal-zoneddatetime-constructor
#[repr(C)]
#[derive(Trace)]
pub struct ZonedDateTimeConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    ZonedDateTimeConstructor,
    initialize: ZonedDateTimeConstructor::initialize,
    call: ZonedDateTimeConstructor::call,
    construct: ZonedDateTimeConstructor::construct
);

impl ZonedDateTimeConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ZonedDateTimeConstructor> {
        realm.create_object(
            vm,
            ZonedDateTimeConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.ZonedDateTime.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 6.2.1 Temporal.ZonedDateTime.prototype, https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().temporal_zoned_date_time_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };
        define_native_function(&names.from, raw_native!(ZonedDateTimeConstructor::from), 1);
        define_native_function(&names.compare, raw_native!(ZonedDateTimeConstructor::compare), 2);

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(2),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 6.1.1 Temporal.ZonedDateTime ( epochNanoseconds, timeZone [ , calendar ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Temporal.ZonedDateTime"],
        )
    }

    // 6.1.1 Temporal.ZonedDateTime ( epochNanoseconds, timeZone [ , calendar ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let epoch_nanoseconds_value = vm.argument(0);
        let time_zone_value = vm.argument(1);
        let mut calendar_value = vm.argument(2);

        // 2. Set epochNanoseconds to ? ToBigInt(epochNanoseconds).
        let epoch_nanoseconds = epoch_nanoseconds_value.to_bigint(vm)?;

        // 3. If IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
        if !is_valid_epoch_nanoseconds(epoch_nanoseconds.big_integer()) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidEpochNanoseconds, &[]);
        }

        // 4. If timeZone is not a String, throw a TypeError exception.
        if !time_zone_value.is_string() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAString, &[&time_zone_value]);
        }

        // 5. Let timeZoneParse be ? ParseTimeZoneIdentifier(timeZone).
        let time_zone_string = time_zone_value.as_string().utf16_string();
        let time_zone_parse = parse_time_zone_identifier_or_throw(vm, Utf16View::of_string(&time_zone_string))?;

        let time_zone = match time_zone_parse.offset_minutes {
            // 6. If timeZoneParse.[[OffsetMinutes]] is EMPTY, then
            None => {
                let name = time_zone_parse
                    .name
                    .expect("a time zone identifier has a name or an offset");

                // a. Let identifierRecord be GetAvailableNamedTimeZoneIdentifier(timeZoneParse.[[Name]]).
                let identifier_record = get_available_named_time_zone_identifier(Utf16View::of_string(&name));

                // b. If identifierRecord is EMPTY, throw a RangeError exception.
                let Some(identifier_record) = identifier_record else {
                    return vm.throw_completion(
                        ErrorKind::RangeError,
                        ErrorType::TemporalInvalidTimeZoneName,
                        &[&name],
                    );
                };

                // c. Set timeZone to identifierRecord.[[Identifier]].
                identifier_record.identifier.clone()
            }
            // 7. Else,
            Some(offset_minutes) => {
                // a. Set timeZone to FormatOffsetTimeZoneIdentifier(timeZoneParse.[[OffsetMinutes]]).
                Utf16String::from_utf8(&format_offset_time_zone_identifier(offset_minutes, None))
            }
        };

        // 8. If calendar is undefined, set calendar to "iso8601".
        if calendar_value.is_undefined() {
            calendar_value = Value::from_string(PrimitiveString::create_from_utf8(vm, "iso8601"));
        }

        // 9. If calendar is not a String, throw a TypeError exception.
        if !calendar_value.is_string() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAString, &[&calendar_value]);
        }

        // 10. Set calendar to ? CanonicalizeCalendar(calendar).
        let calendar_string = calendar_value.as_string().utf16_string();
        let calendar = canonicalize_calendar(vm, Utf16View::of_string(&calendar_string))?;

        // 11. Return ? CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar, NewTarget).
        Ok(create_temporal_zoned_date_time(vm, epoch_nanoseconds, time_zone, calendar, Some(new_target))?.upcast())
    }

    // 6.2.2 Temporal.ZonedDateTime.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.from
    fn from(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? ToTemporalZonedDateTime(item, options).
        Ok(Value::from_object(to_temporal_zoned_date_time(
            vm,
            vm.argument(0),
            vm.argument(1),
        )?))
    }

    // 6.2.3 Temporal.ZonedDateTime.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.compare
    fn compare(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Set one to ? ToTemporalZonedDateTime(one).
        let one = to_temporal_zoned_date_time(vm, vm.argument(0), Value::UNDEFINED)?;

        // 2. Set two to ? ToTemporalZonedDateTime(two).
        let two = to_temporal_zoned_date_time(vm, vm.argument(1), Value::UNDEFINED)?;

        // 3. Return 𝔽(CompareEpochNanoseconds(one.[[EpochNanoseconds]], two.[[EpochNanoseconds]])).
        Ok(Value::from_i32(i32::from(compare_epoch_nanoseconds(
            one.epoch_nanoseconds().big_integer(),
            two.epoch_nanoseconds().big_integer(),
        ))))
    }
}
