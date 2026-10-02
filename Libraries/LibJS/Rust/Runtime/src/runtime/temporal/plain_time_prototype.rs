/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/PlainTimePrototype.cpp: %Temporal.PlainTime.prototype%.

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    RoundingMode, get_options_object, get_rounding_increment_option, get_rounding_mode_option,
};
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::runtime::temporal::abstract_operations::{
    ArithmeticOperation, DurationOperation, SecondsPrecision, Unit, UnitDefault, UnitGroup, UnitValue,
    get_temporal_fractional_second_digits_option, get_temporal_overflow_option, get_temporal_unit_valued_option,
    is_partial_temporal_object, maximum_temporal_duration_rounding_increment, temporal_unit_to_string,
    to_seconds_string_precision_record, validate_temporal_rounding_increment, validate_temporal_unit_value,
};
use crate::runtime::temporal::plain_time::{
    Completeness, PlainTime, add_duration_to_time, compare_time_record, create_temporal_time,
    difference_temporal_plain_time, regulate_time, round_time, time_record_to_string, to_temporal_time,
    to_temporal_time_record,
};

// 4.3 Properties of the Temporal.PlainTime Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plaintime-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct PlainTimePrototype {
    base: Object,
}

define_object_class!(PlainTimePrototype, extends: [Object], methods: {
    initialize: PlainTimePrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_plain_time(vm: &Vm) -> ThrowCompletionOr<Gc<PlainTime>> {
    typed_this_object::<PlainTime>(vm, "Temporal.PlainTime")
}

fn string_value(vm: &Vm, string: &str) -> Value {
    Value::from_string(PrimitiveString::create(vm, Utf16String::from_utf8(string)))
}

impl PlainTimePrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PlainTimePrototype> {
        realm.create_object(
            vm,
            PlainTimePrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 4.3.2 Temporal.PlainTime.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Temporal.PlainTime")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let define_getter = |name: &PropertyKey, getter| {
            object.define_native_accessor(
                vm,
                realm,
                name,
                getter,
                None,
                PropertyAttributes::new(Attribute::CONFIGURABLE),
            );
        };
        define_getter(&names.hour, raw_native!(PlainTimePrototype::hour_getter));
        define_getter(&names.minute, raw_native!(PlainTimePrototype::minute_getter));
        define_getter(&names.second, raw_native!(PlainTimePrototype::second_getter));
        define_getter(&names.millisecond, raw_native!(PlainTimePrototype::millisecond_getter));
        define_getter(&names.microsecond, raw_native!(PlainTimePrototype::microsecond_getter));
        define_getter(&names.nanosecond, raw_native!(PlainTimePrototype::nanosecond_getter));

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };
        define_native_function(&names.add, raw_native!(PlainTimePrototype::add), 1);
        define_native_function(&names.subtract, raw_native!(PlainTimePrototype::subtract), 1);
        define_native_function(&names.with, raw_native!(PlainTimePrototype::with), 1);
        define_native_function(&names.until, raw_native!(PlainTimePrototype::until), 1);
        define_native_function(&names.since, raw_native!(PlainTimePrototype::since), 1);
        define_native_function(&names.round, raw_native!(PlainTimePrototype::round), 1);
        define_native_function(&names.equals, raw_native!(PlainTimePrototype::equals), 1);
        define_native_function(&names.toString, raw_native!(PlainTimePrototype::to_string), 0);
        define_native_function(
            &names.toLocaleString,
            raw_native!(PlainTimePrototype::to_locale_string),
            0,
        );
        define_native_function(&names.toJSON, raw_native!(PlainTimePrototype::to_json), 0);
        define_native_function(&names.valueOf, raw_native!(PlainTimePrototype::value_of), 0);
    }

    // 4.3.3 get Temporal.PlainTime.prototype.hour, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.hour
    fn hour_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. Return 𝔽(plainTime.[[Time]].[[Hour]]).
        Ok(Value::from_i32(i32::from(plain_time.time().hour)))
    }

    // 4.3.4 get Temporal.PlainTime.prototype.minute, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.minute
    fn minute_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. Return 𝔽(plainTime.[[Time]].[[Minute]]).
        Ok(Value::from_i32(i32::from(plain_time.time().minute)))
    }

    // 4.3.5 get Temporal.PlainTime.prototype.second, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.second
    fn second_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. Return 𝔽(plainTime.[[Time]].[[Second]]).
        Ok(Value::from_i32(i32::from(plain_time.time().second)))
    }

    // 4.3.6 get Temporal.PlainTime.prototype.millisecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.millisecond
    fn millisecond_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. Return 𝔽(plainTime.[[Time]].[[Millisecond]]).
        Ok(Value::from_i32(i32::from(plain_time.time().millisecond)))
    }

    // 4.3.7 get Temporal.PlainTime.prototype.microsecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.microsecond
    fn microsecond_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. Return 𝔽(plainTime.[[Time]].[[Microsecond]]).
        Ok(Value::from_i32(i32::from(plain_time.time().microsecond)))
    }

    // 4.3.8 get Temporal.PlainTime.prototype.nanosecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.microsecond
    fn nanosecond_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. Return 𝔽(plainTime.[[Time]].[[Nanosecond]]).
        Ok(Value::from_i32(i32::from(plain_time.time().nanosecond)))
    }

    // 4.3.9 Temporal.PlainTime.prototype.add ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.add
    fn add(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_duration_like = vm.argument(0);

        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. Return ? AddDurationToTime(ADD, plainTime, temporalDurationLike).
        Ok(Value::from_object(add_duration_to_time(
            vm,
            ArithmeticOperation::Add,
            &plain_time,
            temporal_duration_like,
        )?))
    }

    // 4.3.10 Temporal.PlainTime.prototype.subtract ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.subtract
    fn subtract(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_duration_like = vm.argument(0);

        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. Return ? AddDurationToTime(SUBTRACT, plainTime, temporalDurationLike).
        Ok(Value::from_object(add_duration_to_time(
            vm,
            ArithmeticOperation::Subtract,
            &plain_time,
            temporal_duration_like,
        )?))
    }

    // 4.3.11 Temporal.PlainTime.prototype.with ( temporalTimeLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.with
    fn with(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_time_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. If ? IsPartialTemporalObject(temporalTimeLike) is false, throw a TypeError exception.
        if !is_partial_temporal_object(vm, temporal_time_like)? {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::TemporalObjectMustBePartialTemporalObject,
                &[],
            );
        }

        // 4. Let partialTime be ? ToTemporalTimeRecord(temporalTimeLike, PARTIAL).
        let partial_time = to_temporal_time_record(vm, &temporal_time_like.as_object(), Completeness::Partial)?;

        let time = plain_time.time();

        // 5. If partialTime.[[Hour]] is not undefined, then
        //     a. Let hour be partialTime.[[Hour]].
        // 6. Else,
        //     a. Let hour be plainTime.[[Time]].[[Hour]].
        let hour = partial_time.hour.unwrap_or(f64::from(time.hour));

        // 7. If partialTime.[[Minute]] is not undefined, then
        //     a. Let minute be partialTime.[[Minute]].
        // 8. Else,
        //     a. Let minute be plainTime.[[Time]].[[Minute]].
        let minute = partial_time.minute.unwrap_or(f64::from(time.minute));

        // 9. If partialTime.[[Second]] is not undefined, then
        //     a. Let second be partialTime.[[Second]].
        // 10. Else,
        //     a. Let second be plainTime.[[Time]].[[Second]].
        let second = partial_time.second.unwrap_or(f64::from(time.second));

        // 11. If partialTime.[[Millisecond]] is not undefined, then
        //     a. Let millisecond be partialTime.[[Millisecond]].
        // 12. Else,
        //     a. Let millisecond be plainTime.[[Time]].[[Millisecond]].
        let millisecond = partial_time.millisecond.unwrap_or(f64::from(time.millisecond));

        // 13. If partialTime.[[Microsecond]] is not undefined, then
        //     a. Let microsecond be partialTime.[[Microsecond]].
        // 14. Else,
        //     a. Let microsecond be plainTime.[[Time]].[[Microsecond]].
        let microsecond = partial_time.microsecond.unwrap_or(f64::from(time.microsecond));

        // 15. If partialTime.[[Nanosecond]] is not undefined, then
        //     a. Let nanosecond be partialTime.[[Nanosecond]].
        // 16. Else,
        //     a. Let nanosecond be plainTime.[[Time]].[[Nanosecond]].
        let nanosecond = partial_time.nanosecond.unwrap_or(f64::from(time.nanosecond));

        // 17. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, options)?;

        // 18. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

        // 19. Let result be ? RegulateTime(hour, minute, second, millisecond, microsecond, nanosecond, overflow).
        let result = regulate_time(vm, hour, minute, second, millisecond, microsecond, nanosecond, overflow)?;

        // 20. Return ! CreateTemporalTime(result).
        Ok(Value::from_object(create_temporal_time(vm, result, None).must()))
    }

    // 4.3.12 Temporal.PlainTime.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.until
    fn until(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. Return ? DifferenceTemporalPlainTime(UNTIL, plainTime, other, options).
        Ok(Value::from_object(difference_temporal_plain_time(
            vm,
            DurationOperation::Until,
            &plain_time,
            other,
            options,
        )?))
    }

    // 4.3.13 Temporal.PlainTime.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.since
    fn since(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. Return ? DifferenceTemporalPlainTime(SINCE, plainTime, other, options).
        Ok(Value::from_object(difference_temporal_plain_time(
            vm,
            DurationOperation::Since,
            &plain_time,
            other,
            options,
        )?))
    }

    // 4.3.14 Temporal.PlainTime.prototype.round ( roundTo ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.round
    fn round(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let round_to_value = vm.argument(0);

        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. If roundTo is undefined, throw a TypeError exception.
        if round_to_value.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::TemporalMissingOptionsObject, &[]);
        }

        // 4. If roundTo is a String, then
        let round_to = if round_to_value.is_string() {
            // a. Let paramString be roundTo.
            let param_string = round_to_value;

            // b. Set roundTo to OrdinaryObjectCreate(null).
            let round_to = Object::create(vm, realm, None);

            // c. Perform ! CreateDataPropertyOrThrow(roundTo, "smallestUnit", paramString).
            round_to
                .create_data_property_or_throw(vm, &vm.names.smallestUnit, param_string)
                .must();
            round_to
        }
        // 5. Else,
        else {
            // a. Set roundTo to ? GetOptionsObject(roundTo).
            get_options_object(vm, round_to_value)?
        };

        // 6. NOTE: The following steps read options and perform independent validation in alphabetical order
        //    (GetRoundingIncrementOption reads "roundingIncrement" and GetRoundingModeOption reads "roundingMode").

        // 7. Let roundingIncrement be ? GetRoundingIncrementOption(roundTo).
        let rounding_increment = get_rounding_increment_option(vm, &round_to)?;

        // 8. Let roundingMode be ? GetRoundingModeOption(roundTo, HALF-EXPAND).
        let rounding_mode = get_rounding_mode_option(vm, &round_to, RoundingMode::HalfExpand)?;

        // 9. Let smallestUnit be ? GetTemporalUnitValuedOption(roundTo, "smallestUnit", REQUIRED).
        let smallest_unit =
            get_temporal_unit_valued_option(vm, &round_to, &vm.names.smallestUnit, UnitDefault::Required)?;

        // 10. Perform ? ValidateTemporalUnitValue(smallestUnit, TIME).
        validate_temporal_unit_value(vm, &vm.names.smallestUnit, smallest_unit, UnitGroup::Time, &[])?;
        let UnitValue::Unit(smallest_unit_value) = smallest_unit else {
            unreachable!("the smallest unit was validated");
        };

        // 11. Let maximum be MaximumTemporalDurationRoundingIncrement(smallestUnit).
        // 12. Assert: maximum is not UNSET.
        let maximum = maximum_temporal_duration_rounding_increment(smallest_unit_value)
            .expect("a time unit has a maximum rounding increment");

        // 13. Perform ? ValidateTemporalRoundingIncrement(roundingIncrement, maximum, false).
        validate_temporal_rounding_increment(vm, rounding_increment, maximum, false)?;

        // 14. Let result be RoundTime(plainTime.[[Time]], roundingIncrement, smallestUnit, roundingMode).
        let result = round_time(
            &plain_time.time(),
            rounding_increment,
            smallest_unit_value,
            rounding_mode,
        );

        // 15. Return ! CreateTemporalTime(result).
        Ok(Value::from_object(create_temporal_time(vm, result, None).must()))
    }

    // 4.3.15 Temporal.PlainTime.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.equals
    fn equals(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. Set other to ? ToTemporalTime(other).
        let other = to_temporal_time(vm, vm.argument(0), Value::UNDEFINED)?;

        // 4. If CompareTimeRecord(plainTime.[[Time]], other.[[Time]]) = 0, return true.
        // 5. Return false.
        Ok(Value::from_bool(
            compare_time_record(&plain_time.time(), &other.time()) == 0,
        ))
    }

    // 4.3.16 Temporal.PlainTime.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, vm.argument(0))?;

        // 4. NOTE: The following steps read options and perform independent validation in alphabetical order
        //    (GetTemporalFractionalSecondDigitsOption reads "fractionalSecondDigits" and GetRoundingModeOption reads "roundingMode").

        // 5. Let digits be ? GetTemporalFractionalSecondDigitsOption(resolvedOptions).
        let digits = get_temporal_fractional_second_digits_option(vm, &resolved_options)?;

        // 6. Let roundingMode be ? GetRoundingModeOption(resolvedOptions, TRUNC).
        let rounding_mode = get_rounding_mode_option(vm, &resolved_options, RoundingMode::Trunc)?;

        // 7. Let smallestUnit be ? GetTemporalUnitValuedOption(resolvedOptions, "smallestUnit", UNSET).
        let smallest_unit =
            get_temporal_unit_valued_option(vm, &resolved_options, &vm.names.smallestUnit, UnitDefault::Unset)?;

        // 8. Perform ? ValidateTemporalUnitValue(smallestUnit, TIME).
        validate_temporal_unit_value(vm, &vm.names.smallestUnit, smallest_unit, UnitGroup::Time, &[])?;

        // 9. If smallestUnit is HOUR, throw a RangeError exception.
        if smallest_unit == UnitValue::Unit(Unit::Hour) {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::OptionIsNotValidValue,
                &[&temporal_unit_to_string(Unit::Hour), &vm.names.smallestUnit],
            );
        }

        // 10. Let precision be ToSecondsStringPrecisionRecord(smallestUnit, digits).
        let precision = to_seconds_string_precision_record(smallest_unit, digits);

        // 11. Let roundResult be RoundTime(plainTime.[[Time]], precision.[[Increment]], precision.[[Unit]], roundingMode).
        let round_result = round_time(
            &plain_time.time(),
            u64::from(precision.increment),
            precision.unit,
            rounding_mode,
        );

        // 12. Return TimeRecordToString(roundResult, precision.[[Precision]]).
        Ok(string_value(
            vm,
            &time_record_to_string(&round_result, precision.precision),
        ))
    }

    // 4.3.17 Temporal.PlainTime.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.tolocalestring
    // 15.11.6.1 Temporal.PlainTime.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sup-temporal.plaintime.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        typed_this_plain_time(vm)?;

        // 3. Let dateFormat be ? CreateDateTimeFormat(%Intl.DateTimeFormat%, locales, options, TIME, TIME).
        realm.intrinsics().intl_date_time_format_constructor(vm);

        // 4. Return ? FormatDateTime(dateFormat, plainTime).
        unimplemented_runtime_function(
            "Temporal.PlainTime.prototype.toLocaleString, which needs CreateDateTimeFormat and FormatDateTime",
            0,
        )
    }

    // 4.3.18 Temporal.PlainTime.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.tojson
    fn to_json(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainTime, [[InitializedTemporalTime]]).
        let plain_time = typed_this_plain_time(vm)?;

        // 3. Return TimeRecordToString(plainTime.[[Time]], AUTO).
        Ok(string_value(
            vm,
            &time_record_to_string(&plain_time.time(), SecondsPrecision::Auto),
        ))
    }

    // 4.3.19 Temporal.PlainTime.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::Convert,
            &[&"Temporal.PlainTime", &"a primitive value"],
        )
    }
}
