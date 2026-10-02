/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/DurationPrototype.cpp: %Temporal.Duration.prototype%.

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    RoundingMode, construct, get_options_object, get_rounding_increment_option, get_rounding_mode_option,
};
use crate::runtime::big_int::SignedBigInteger;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::duration_format::{DurationFormat, partition_duration_format_pattern};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::runtime::temporal::abstract_operations::{
    ArithmeticOperation, Overflow, Precision, Unit, UnitCategory, UnitDefault, UnitGroup, UnitValue,
    get_temporal_fractional_second_digits_option, get_temporal_relative_to_option, get_temporal_unit_valued_option,
    is_calendar_unit, larger_of_two_temporal_units, maximum_temporal_duration_rounding_increment,
    round_number_to_increment, temporal_unit_category, temporal_unit_to_string, to_seconds_string_precision_record,
    validate_temporal_rounding_increment, validate_temporal_unit_value,
};
use crate::runtime::temporal::calendar::calendar_date_add;
use crate::runtime::temporal::duration::{
    Duration, DurationFields, add_durations, adjust_date_duration_record, combine_date_and_time_duration,
    create_date_duration_record, create_negated_temporal_duration, create_temporal_duration,
    default_temporal_largest_unit, duration_sign, round_time_duration, temporal_duration_from_internal,
    temporal_duration_to_string, to_internal_duration_record, to_internal_duration_record_with_24_hour_days,
    to_temporal_partial_duration_record, total_time_duration, zero_date_duration,
};
use crate::runtime::temporal::iso_records::TimeDuration;
use crate::runtime::temporal::plain_date_time::{
    combine_iso_date_and_time_record, difference_plain_date_time_with_rounding, difference_plain_date_time_with_total,
};
use crate::runtime::temporal::plain_time::{add_time, midnight_time_record};
use crate::runtime::temporal::zoned_date_time::{
    add_zoned_date_time, difference_zoned_date_time_with_rounding, difference_zoned_date_time_with_total,
};
use crate::utf16::{Utf16StringBuilder, Utf16View};

// 7.3 Properties of the Temporal.Duration Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-duration-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct DurationPrototype {
    base: Object,
}

define_object_class!(DurationPrototype, extends: [Object], methods: {
    initialize: DurationPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_duration(vm: &Vm) -> ThrowCompletionOr<Gc<Duration>> {
    typed_this_object::<Duration>(vm, "Temporal.Duration")
}

fn string_value(vm: &Vm, string: &str) -> Value {
    Value::from_string(PrimitiveString::create(vm, Utf16String::from_utf8(string)))
}

/// Duration.prototype.round ( roundTo ) and Duration.prototype.total ( totalOf ) take either an options object, or a
/// string that becomes the value of `key` in a new options object.
fn get_options_or_string_option(vm: &Vm, value: Value, key: &PropertyKey) -> ThrowCompletionOr<Gc<Object>> {
    let realm = vm.current_realm().expect("a builtin runs in a realm");

    // 4. If roundTo is a String, then
    if value.is_string() {
        // a. Let paramString be roundTo.
        let param_string = value;

        // b. Set roundTo to OrdinaryObjectCreate(null).
        let options = Object::create(vm, realm, None);

        // c. Perform ! CreateDataPropertyOrThrow(roundTo, "smallestUnit", paramString).
        options.create_data_property_or_throw(vm, key, param_string).must();
        return Ok(options);
    }

    // 5. Else,
    // a. Set roundTo to ? GetOptionsObject(roundTo).
    get_options_object(vm, value)
}

impl DurationPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<DurationPrototype> {
        realm.create_object(
            vm,
            DurationPrototype {
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

        // 7.3.2 Temporal.Duration.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Temporal.Duration")),
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
        define_getter(&names.years, raw_native!(DurationPrototype::years_getter));
        define_getter(&names.months, raw_native!(DurationPrototype::months_getter));
        define_getter(&names.weeks, raw_native!(DurationPrototype::weeks_getter));
        define_getter(&names.days, raw_native!(DurationPrototype::days_getter));
        define_getter(&names.hours, raw_native!(DurationPrototype::hours_getter));
        define_getter(&names.minutes, raw_native!(DurationPrototype::minutes_getter));
        define_getter(&names.seconds, raw_native!(DurationPrototype::seconds_getter));
        define_getter(&names.milliseconds, raw_native!(DurationPrototype::milliseconds_getter));
        define_getter(&names.microseconds, raw_native!(DurationPrototype::microseconds_getter));
        define_getter(&names.nanoseconds, raw_native!(DurationPrototype::nanoseconds_getter));
        define_getter(&names.sign, raw_native!(DurationPrototype::sign_getter));
        define_getter(&names.blank, raw_native!(DurationPrototype::blank_getter));

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };
        define_native_function(&names.with, raw_native!(DurationPrototype::with), 1);
        define_native_function(&names.negated, raw_native!(DurationPrototype::negated), 0);
        define_native_function(&names.abs, raw_native!(DurationPrototype::abs), 0);
        define_native_function(&names.add, raw_native!(DurationPrototype::add), 1);
        define_native_function(&names.subtract, raw_native!(DurationPrototype::subtract), 1);
        define_native_function(&names.round, raw_native!(DurationPrototype::round), 1);
        define_native_function(&names.total, raw_native!(DurationPrototype::total), 1);
        define_native_function(&names.toString, raw_native!(DurationPrototype::to_string), 0);
        define_native_function(&names.toJSON, raw_native!(DurationPrototype::to_json), 0);
        define_native_function(
            &names.toLocaleString,
            raw_native!(DurationPrototype::to_locale_string),
            0,
        );
        define_native_function(&names.valueOf, raw_native!(DurationPrototype::value_of), 0);
    }

    // 7.3.3 get Temporal.Duration.prototype.years, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.years
    fn years_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return 𝔽(duration.[[Years]]).
        Ok(Value::from_f64(duration.years()))
    }

    // 7.3.4 get Temporal.Duration.prototype.months, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.months
    fn months_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return 𝔽(duration.[[Months]]).
        Ok(Value::from_f64(duration.months()))
    }

    // 7.3.5 get Temporal.Duration.prototype.weeks, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.weeks
    fn weeks_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return 𝔽(duration.[[Weeks]]).
        Ok(Value::from_f64(duration.weeks()))
    }

    // 7.3.6 get Temporal.Duration.prototype.days, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.days
    fn days_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return 𝔽(duration.[[Days]]).
        Ok(Value::from_f64(duration.days()))
    }

    // 7.3.7 get Temporal.Duration.prototype.hours, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.hours
    fn hours_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return 𝔽(duration.[[Hours]]).
        Ok(Value::from_f64(duration.hours()))
    }

    // 7.3.8 get Temporal.Duration.prototype.minutes, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.minutes
    fn minutes_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return 𝔽(duration.[[Minutes]]).
        Ok(Value::from_f64(duration.minutes()))
    }

    // 7.3.9 get Temporal.Duration.prototype.seconds, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.seconds
    fn seconds_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return 𝔽(duration.[[Seconds]]).
        Ok(Value::from_f64(duration.seconds()))
    }

    // 7.3.10 get Temporal.Duration.prototype.milliseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.milliseconds
    fn milliseconds_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return 𝔽(duration.[[Milliseconds]]).
        Ok(Value::from_f64(duration.milliseconds()))
    }

    // 7.3.11 get Temporal.Duration.prototype.microseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.microseconds
    fn microseconds_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return 𝔽(duration.[[Microseconds]]).
        Ok(Value::from_f64(duration.microseconds()))
    }

    // 7.3.12 get Temporal.Duration.prototype.nanoseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.nanoseconds
    fn nanoseconds_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return 𝔽(duration.[[Nanoseconds]]).
        Ok(Value::from_f64(duration.nanoseconds()))
    }

    // 7.3.13 get Temporal.Duration.prototype.sign, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.sign
    fn sign_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return 𝔽(DurationSign(duration)).
        Ok(Value::from_i32(i32::from(duration_sign(&duration))))
    }

    // 7.3.14 get Temporal.Duration.prototype.blank, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.blank
    fn blank_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. If DurationSign(duration) = 0, return true.
        // 4. Return false.
        Ok(Value::from_bool(duration_sign(&duration) == 0))
    }

    // 7.3.15 Temporal.Duration.prototype.with ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.with
    fn with(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Let temporalDurationLike be ? ToTemporalPartialDurationRecord(temporalDurationLike).
        let temporal_duration_like = to_temporal_partial_duration_record(vm, vm.argument(0))?;

        // 4. If temporalDurationLike.[[Years]] is not undefined, then
        //     a. Let years be temporalDurationLike.[[Years]].
        // 5. Else,
        //     a. Let years be duration.[[Years]].
        let years = temporal_duration_like.years.unwrap_or(duration.years());

        // 6. If temporalDurationLike.[[Months]] is not undefined, then
        //     a. Let months be temporalDurationLike.[[Months]].
        // 7. Else,
        //     a. Let months be duration.[[Months]].
        let months = temporal_duration_like.months.unwrap_or(duration.months());

        // 8. If temporalDurationLike.[[Weeks]] is not undefined, then
        //     a. Let weeks be temporalDurationLike.[[Weeks]].
        // 9. Else,
        //     a. Let weeks be duration.[[Weeks]].
        let weeks = temporal_duration_like.weeks.unwrap_or(duration.weeks());

        // 10. If temporalDurationLike.[[Days]] is not undefined, then
        //     a. Let days be temporalDurationLike.[[Days]].
        // 11. Else,
        //     a. Let days be duration.[[Days]].
        let days = temporal_duration_like.days.unwrap_or(duration.days());

        // 12. If temporalDurationLike.[[Hours]] is not undefined, then
        //     a. Let hours be temporalDurationLike.[[Hours]].
        // 13. Else,
        //     a. Let hours be duration.[[Hours]].
        let hours = temporal_duration_like.hours.unwrap_or(duration.hours());

        // 14. If temporalDurationLike.[[Minutes]] is not undefined, then
        //     a. Let minutes be temporalDurationLike.[[Minutes]].
        // 15. Else,
        //     a. Let minutes be duration.[[Minutes]].
        let minutes = temporal_duration_like.minutes.unwrap_or(duration.minutes());

        // 16. If temporalDurationLike.[[Seconds]] is not undefined, then
        //     a. Let seconds be temporalDurationLike.[[Seconds]].
        // 17. Else,
        //     a. Let seconds be duration.[[Seconds]].
        let seconds = temporal_duration_like.seconds.unwrap_or(duration.seconds());

        // 18. If temporalDurationLike.[[Milliseconds]] is not undefined, then
        //     a. Let milliseconds be temporalDurationLike.[[Milliseconds]].
        // 19. Else,
        //     a. Let milliseconds be duration.[[Milliseconds]].
        let milliseconds = temporal_duration_like.milliseconds.unwrap_or(duration.milliseconds());

        // 20. If temporalDurationLike.[[Microseconds]] is not undefined, then
        //     a. Let microseconds be temporalDurationLike.[[Microseconds]].
        // 21. Else,
        //     a. Let microseconds be duration.[[Microseconds]].
        let microseconds = temporal_duration_like.microseconds.unwrap_or(duration.microseconds());

        // 22. If temporalDurationLike.[[Nanoseconds]] is not undefined, then
        //     a. Let nanoseconds be temporalDurationLike.[[Nanoseconds]].
        // 23. Else,
        //     a. Let nanoseconds be duration.[[Nanoseconds]].
        let nanoseconds = temporal_duration_like.nanoseconds.unwrap_or(duration.nanoseconds());

        // 24. Return ? CreateTemporalDuration(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
        let result = create_temporal_duration(
            vm,
            DurationFields {
                years,
                months,
                weeks,
                days,
                hours,
                minutes,
                seconds,
                milliseconds,
                microseconds,
                nanoseconds,
            },
            None,
        )?;
        Ok(Value::from_object(result))
    }

    // 7.3.16 Temporal.Duration.prototype.negated ( ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.negated
    fn negated(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return CreateNegatedTemporalDuration(duration).
        Ok(Value::from_object(create_negated_temporal_duration(vm, &duration)))
    }

    // 7.3.17 Temporal.Duration.prototype.abs ( ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.abs
    fn abs(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return ! CreateTemporalDuration(abs(duration.[[Years]]), abs(duration.[[Months]]), abs(duration.[[Weeks]]), abs(duration.[[Days]]), abs(duration.[[Hours]]), abs(duration.[[Minutes]]), abs(duration.[[Seconds]]), abs(duration.[[Milliseconds]]), abs(duration.[[Microseconds]]), abs(duration.[[Nanoseconds]])).
        let fields = duration.fields();
        let result = create_temporal_duration(
            vm,
            DurationFields {
                years: fields.years.abs(),
                months: fields.months.abs(),
                weeks: fields.weeks.abs(),
                days: fields.days.abs(),
                hours: fields.hours.abs(),
                minutes: fields.minutes.abs(),
                seconds: fields.seconds.abs(),
                milliseconds: fields.milliseconds.abs(),
                microseconds: fields.microseconds.abs(),
                nanoseconds: fields.nanoseconds.abs(),
            },
            None,
        )
        .must();
        Ok(Value::from_object(result))
    }

    // 7.3.18 Temporal.Duration.prototype.add ( other ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.add
    fn add(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);

        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return ? AddDurations(ADD, duration, other).
        Ok(Value::from_object(add_durations(
            vm,
            ArithmeticOperation::Add,
            &duration,
            other,
        )?))
    }

    // 7.3.19 Temporal.Duration.prototype.subtract ( other ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.subtract
    fn subtract(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);

        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return ? AddDurations(SUBTRACT, duration, other).
        Ok(Value::from_object(add_durations(
            vm,
            ArithmeticOperation::Subtract,
            &duration,
            other,
        )?))
    }

    // 7.3.20 Temporal.Duration.prototype.round ( roundTo ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.round
    fn round(vm: &Vm) -> ThrowCompletionOr<Value> {
        let round_to_value = vm.argument(0);

        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. If roundTo is undefined, throw a TypeError exception.
        if round_to_value.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::TemporalMissingOptionsObject, &[]);
        }

        // 4. If roundTo is a String, then
        //     a. Let paramString be roundTo.
        //     b. Set roundTo to OrdinaryObjectCreate(null).
        //     c. Perform ! CreateDataPropertyOrThrow(roundTo, "smallestUnit", paramString).
        // 5. Else,
        //     a. Set roundTo to ? GetOptionsObject(roundTo).
        let round_to = get_options_or_string_option(vm, round_to_value, &vm.names.smallestUnit)?;

        // 6. Let smallestUnitPresent be true.
        let mut smallest_unit_present = true;

        // 7. Let largestUnitPresent be true.
        let mut largest_unit_present = true;

        // 8. NOTE: The following steps read options and perform independent validation in alphabetical order
        //    (GetTemporalRelativeToOption reads "relativeTo", GetRoundingIncrementOption reads "roundingIncrement" and
        //    GetRoundingModeOption reads "roundingMode").

        // 9. Let largestUnit be ? GetTemporalUnitValuedOption(roundTo, "largestUnit", UNSET).
        let mut largest_unit =
            get_temporal_unit_valued_option(vm, &round_to, &vm.names.largestUnit, UnitDefault::Unset)?;

        // 10. Let relativeToRecord be ? GetTemporalRelativeToOption(roundTo).
        // 11. Let zonedRelativeTo be relativeToRecord.[[ZonedRelativeTo]].
        // 12. Let plainRelativeTo be relativeToRecord.[[PlainRelativeTo]].
        let relative_to_record = get_temporal_relative_to_option(vm, &round_to)?;
        let plain_relative_to = relative_to_record.plain_relative_to;
        let zoned_relative_to = relative_to_record.zoned_relative_to;

        // 13. Let roundingIncrement be ? GetRoundingIncrementOption(roundTo).
        let rounding_increment = get_rounding_increment_option(vm, &round_to)?;

        // 14. Let roundingMode be ? GetRoundingModeOption(roundTo, HALF-EXPAND).
        let rounding_mode = get_rounding_mode_option(vm, &round_to, RoundingMode::HalfExpand)?;

        // 15. Let smallestUnit be ? GetTemporalUnitValuedOption(roundTo, "smallestUnit", UNSET).
        let mut smallest_unit =
            get_temporal_unit_valued_option(vm, &round_to, &vm.names.smallestUnit, UnitDefault::Unset)?;

        // 16. Perform ? ValidateTemporalUnitValue(smallestUnit, DATETIME).
        validate_temporal_unit_value(vm, &vm.names.smallestUnit, smallest_unit, UnitGroup::DateTime, &[])?;

        // 17. If smallestUnit is UNSET, then
        if smallest_unit == UnitValue::Unset {
            // a. Set smallestUnitPresent to false.
            smallest_unit_present = false;

            // b. Set smallestUnit to NANOSECOND.
            smallest_unit = UnitValue::Unit(Unit::Nanosecond);
        }

        let UnitValue::Unit(smallest_unit_value) = smallest_unit else {
            unreachable!("the smallest unit was validated");
        };

        // 18. Let existingLargestUnit be DefaultTemporalLargestUnit(duration).
        let existing_largest_unit = default_temporal_largest_unit(&duration);

        // 19. Let defaultLargestUnit be LargerOfTwoTemporalUnits(existingLargestUnit, smallestUnit).
        let default_largest_unit = larger_of_two_temporal_units(existing_largest_unit, smallest_unit_value);

        // 20. If largestUnit is UNSET, then
        if largest_unit == UnitValue::Unset {
            // a. Set largestUnitPresent to false.
            largest_unit_present = false;

            // b. Set largestUnit to defaultLargestUnit.
            largest_unit = UnitValue::Unit(default_largest_unit);
        }
        // 21. Else if largestUnit is AUTO, then
        else if largest_unit == UnitValue::Auto {
            // a. Set largestUnit to defaultLargestUnit.
            largest_unit = UnitValue::Unit(default_largest_unit);
        }

        // 22. If smallestUnitPresent is false and largestUnitPresent is false, throw a RangeError exception.
        if !smallest_unit_present && !largest_unit_present {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalMissingUnits, &[]);
        }

        let UnitValue::Unit(mut largest_unit_value) = largest_unit else {
            unreachable!("the largest unit was resolved");
        };

        // 23. If LargerOfTwoTemporalUnits(largestUnit, smallestUnit) is not largestUnit, throw a RangeError exception.
        if larger_of_two_temporal_units(largest_unit_value, smallest_unit_value) != largest_unit_value {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::TemporalInvalidUnitRange,
                &[
                    &temporal_unit_to_string(smallest_unit_value),
                    &temporal_unit_to_string(largest_unit_value),
                ],
            );
        }

        // 24. Let maximum be MaximumTemporalDurationRoundingIncrement(smallestUnit).
        let maximum = maximum_temporal_duration_rounding_increment(smallest_unit_value);

        // 25. If maximum is not UNSET, perform ? ValidateTemporalRoundingIncrement(roundingIncrement, maximum, false).
        if let Some(maximum) = maximum {
            validate_temporal_rounding_increment(vm, rounding_increment, maximum, false)?;
        }

        // 26. If roundingIncrement > 1, and largestUnit is not smallestUnit, and TemporalUnitCategory(smallestUnit) is DATE,
        //     throw a RangeError exception.
        if rounding_increment > 1
            && largest_unit_value != smallest_unit_value
            && temporal_unit_category(smallest_unit_value) == UnitCategory::Date
        {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::OptionIsNotValidValue,
                &[&rounding_increment, &"roundingIncrement"],
            );
        }

        // 27. If zonedRelativeTo is not undefined, then
        if let Some(zoned_relative_to) = zoned_relative_to {
            // a. Let internalDuration be ToInternalDurationRecord(duration).
            let internal_duration = to_internal_duration_record(vm, &duration);

            // b. Let timeZone be zonedRelativeTo.[[TimeZone]].
            let time_zone = zoned_relative_to.time_zone();

            // c. Let calendar be zonedRelativeTo.[[Calendar]].
            let calendar = zoned_relative_to.calendar();

            // d. Let relativeEpochNs be zonedRelativeTo.[[EpochNanoseconds]].
            let relative_epoch_nanoseconds = zoned_relative_to.epoch_nanoseconds().big_integer().clone();

            // e. Let targetEpochNs be ? AddZonedDateTime(relativeEpochNs, timeZone, calendar, internalDuration, CONSTRAIN).
            let target_epoch_nanoseconds = add_zoned_date_time(
                vm,
                &relative_epoch_nanoseconds,
                Utf16View::of_string(&time_zone),
                Utf16View::of_string(&calendar),
                &internal_duration,
                Overflow::Constrain,
            )?;

            // f. Set internalDuration to ? DifferenceZonedDateTimeWithRounding(relativeEpochNs, targetEpochNs, timeZone, calendar, largestUnit, roundingIncrement, smallestUnit, roundingMode).
            let internal_duration = difference_zoned_date_time_with_rounding(
                vm,
                &relative_epoch_nanoseconds,
                &target_epoch_nanoseconds,
                Utf16View::of_string(&time_zone),
                Utf16View::of_string(&calendar),
                largest_unit_value,
                rounding_increment,
                smallest_unit_value,
                rounding_mode,
            )?;

            // g. If TemporalUnitCategory(largestUnit) is DATE, set largestUnit to HOUR.
            if temporal_unit_category(largest_unit_value) == UnitCategory::Date {
                largest_unit_value = Unit::Hour;
            }

            // h. Return ? TemporalDurationFromInternal(internalDuration, largestUnit).
            return Ok(Value::from_object(temporal_duration_from_internal(
                vm,
                &internal_duration,
                largest_unit_value,
            )?));
        }

        // 28. If plainRelativeTo is not undefined, then
        if let Some(plain_relative_to) = plain_relative_to {
            // a. Let internalDuration be ToInternalDurationRecordWith24HourDays(duration).
            let internal_duration = to_internal_duration_record_with_24_hour_days(vm, &duration);

            // b. Let targetTime be AddTime(MidnightTimeRecord(), internalDuration.[[Time]]).
            let target_time = add_time(&midnight_time_record(), &internal_duration.time);

            // c. Let calendar be plainRelativeTo.[[Calendar]].
            let calendar = plain_relative_to.calendar();

            // d. Let dateDuration be ? AdjustDateDurationRecord(internalDuration.[[Date]], targetTime.[[Days]]).
            let date_duration = adjust_date_duration_record(vm, &internal_duration.date, target_time.days, None, None)?;

            // e. Let targetDate be ? CalendarDateAdd(calendar, plainRelativeTo.[[ISODate]], dateDuration, CONSTRAIN).
            let target_date = calendar_date_add(
                vm,
                Utf16View::of_string(&calendar),
                plain_relative_to.iso_date(),
                &date_duration,
                Overflow::Constrain,
            )?;

            // f. Let isoDateTime be CombineISODateAndTimeRecord(plainRelativeTo.[[ISODate]], MidnightTimeRecord()).
            let iso_date_time = combine_iso_date_and_time_record(plain_relative_to.iso_date(), midnight_time_record());

            // g. Let targetDateTime be CombineISODateAndTimeRecord(targetDate, targetTime).
            let target_date_time = combine_iso_date_and_time_record(target_date, target_time);

            // h. Set internalDuration to ? DifferencePlainDateTimeWithRounding(isoDateTime, targetDateTime, calendar, largestUnit, roundingIncrement, smallestUnit, roundingMode).
            let internal_duration = difference_plain_date_time_with_rounding(
                vm,
                &iso_date_time,
                &target_date_time,
                Utf16View::of_string(&calendar),
                largest_unit_value,
                rounding_increment,
                smallest_unit_value,
                rounding_mode,
            )?;

            // i. Return ? TemporalDurationFromInternal(internalDuration, largestUnit).
            return Ok(Value::from_object(temporal_duration_from_internal(
                vm,
                &internal_duration,
                largest_unit_value,
            )?));
        }

        // 29. If IsCalendarUnit(existingLargestUnit) is true or IsCalendarUnit(largestUnit) is true, throw a RangeError exception.
        if is_calendar_unit(existing_largest_unit) {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::TemporalInvalidLargestUnit,
                &[&temporal_unit_to_string(existing_largest_unit)],
            );
        }
        if is_calendar_unit(largest_unit_value) {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::TemporalInvalidLargestUnit,
                &[&temporal_unit_to_string(largest_unit_value)],
            );
        }

        // 30. Assert: IsCalendarUnit(smallestUnit) is false.
        assert!(!is_calendar_unit(smallest_unit_value));

        // 31. Let internalDuration be ToInternalDurationRecordWith24HourDays(duration).
        let internal_duration = to_internal_duration_record_with_24_hour_days(vm, &duration);

        // 32. If smallestUnit is DAY, then
        let internal_duration = if smallest_unit_value == Unit::Day {
            // a. Let fractionalDays be TotalTimeDuration(internalDuration.[[Time]], DAY).
            let fractional_days = total_time_duration(&internal_duration.time, Unit::Day);

            // b. Let days be RoundNumberToIncrement(fractionalDays, roundingIncrement, roundingMode).
            let days = round_number_to_increment(fractional_days.to_double(), rounding_increment, rounding_mode);

            // c. Let dateDuration be ? CreateDateDurationRecord(0, 0, 0, days).
            let date_duration = create_date_duration_record(vm, 0.0, 0.0, 0.0, days)?;

            // d. Set internalDuration to CombineDateAndTimeDuration(dateDuration, 0).
            combine_date_and_time_duration(date_duration, TimeDuration::default())
        }
        // 33. Else,
        else {
            // a. Let timeDuration be ? RoundTimeDuration(internalDuration.[[Time]], roundingIncrement, smallestUnit, roundingMode).
            let time_duration = round_time_duration(
                vm,
                &internal_duration.time,
                &SignedBigInteger::from(rounding_increment),
                smallest_unit_value,
                rounding_mode,
            )?;

            // b. Set internalDuration to CombineDateAndTimeDuration(ZeroDateDuration(), timeDuration).
            combine_date_and_time_duration(zero_date_duration(vm), time_duration)
        };

        // 34. Return ? TemporalDurationFromInternal(internalDuration, largestUnit).
        Ok(Value::from_object(temporal_duration_from_internal(
            vm,
            &internal_duration,
            largest_unit_value,
        )?))
    }

    // 7.3.21 Temporal.Duration.prototype.total ( totalOf ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.total
    fn total(vm: &Vm) -> ThrowCompletionOr<Value> {
        let total_of_value = vm.argument(0);

        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. If totalOf is undefined, throw a TypeError exception.
        if total_of_value.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::IsUndefined, &[&"totalOf"]);
        }

        // 4. If totalOf is a String, then
        //     a. Let paramString be totalOf.
        //     b. Set totalOf to OrdinaryObjectCreate(null).
        //     c. Perform ! CreateDataPropertyOrThrow(totalOf, "unit", paramString).
        // 5. Else,
        //     a. Set totalOf to ? GetOptionsObject(totalOf).
        let total_of = get_options_or_string_option(vm, total_of_value, &vm.names.unit)?;

        // 6. NOTE: The following steps read options and perform independent validation in alphabetical order
        //    (GetTemporalRelativeToOption reads "relativeTo").

        // 7. Let relativeToRecord be ? GetTemporalRelativeToOption(totalOf).
        // 8. Let zonedRelativeTo be relativeToRecord.[[ZonedRelativeTo]].
        // 9. Let plainRelativeTo be relativeToRecord.[[PlainRelativeTo]].
        let relative_to_record = get_temporal_relative_to_option(vm, &total_of)?;

        // 10. Let unit be ? GetTemporalUnitValuedOption(totalOf, "unit", REQUIRED).
        let unit = get_temporal_unit_valued_option(vm, &total_of, &vm.names.unit, UnitDefault::Required)?;

        // 11. Perform ? ValidateTemporalUnitValue(unit, DATETIME).
        validate_temporal_unit_value(vm, &vm.names.smallestUnit, unit, UnitGroup::DateTime, &[])?;
        let UnitValue::Unit(unit_value) = unit else {
            unreachable!("the unit was validated");
        };

        // 12. If zonedRelativeTo is not undefined, then
        let total = if let Some(zoned_relative_to) = relative_to_record.zoned_relative_to {
            // a. Let internalDuration be ToInternalDurationRecord(duration).
            let internal_duration = to_internal_duration_record(vm, &duration);

            // b. Let timeZone be zonedRelativeTo.[[TimeZone]].
            let time_zone = zoned_relative_to.time_zone();

            // c. Let calendar be zonedRelativeTo.[[Calendar]].
            let calendar = zoned_relative_to.calendar();

            // d. Let relativeEpochNs be zonedRelativeTo.[[EpochNanoseconds]].
            let relative_epoch_nanoseconds = zoned_relative_to.epoch_nanoseconds().big_integer().clone();

            // e. Let targetEpochNs be ? AddZonedDateTime(relativeEpochNs, timeZone, calendar, internalDuration, CONSTRAIN).
            let target_epoch_nanoseconds = add_zoned_date_time(
                vm,
                &relative_epoch_nanoseconds,
                Utf16View::of_string(&time_zone),
                Utf16View::of_string(&calendar),
                &internal_duration,
                Overflow::Constrain,
            )?;

            // f. Let total be ? DifferenceZonedDateTimeWithTotal(relativeEpochNs, targetEpochNs, timeZone, calendar, unit).
            difference_zoned_date_time_with_total(
                vm,
                &relative_epoch_nanoseconds,
                &target_epoch_nanoseconds,
                Utf16View::of_string(&time_zone),
                Utf16View::of_string(&calendar),
                unit_value,
            )?
        }
        // 13. Else if plainRelativeTo is not undefined, then
        else if let Some(plain_relative_to) = relative_to_record.plain_relative_to {
            // a. Let internalDuration be ToInternalDurationRecordWith24HourDays(duration).
            let internal_duration = to_internal_duration_record_with_24_hour_days(vm, &duration);

            // b. Let targetTime be AddTime(MidnightTimeRecord(), internalDuration.[[Time]]).
            let target_time = add_time(&midnight_time_record(), &internal_duration.time);

            // c. Let calendar be plainRelativeTo.[[Calendar]].
            let calendar = plain_relative_to.calendar();

            // d. Let dateDuration be ? AdjustDateDurationRecord(internalDuration.[[Date]], targetTime.[[Days]]).
            let date_duration = adjust_date_duration_record(vm, &internal_duration.date, target_time.days, None, None)?;

            // e. Let targetDate be ? CalendarDateAdd(calendar, plainRelativeTo.[[ISODate]], dateDuration, CONSTRAIN).
            let target_date = calendar_date_add(
                vm,
                Utf16View::of_string(&calendar),
                plain_relative_to.iso_date(),
                &date_duration,
                Overflow::Constrain,
            )?;

            // f. Let isoDateTime be CombineISODateAndTimeRecord(plainRelativeTo.[[ISODate]], MidnightTimeRecord()).
            let iso_date_time = combine_iso_date_and_time_record(plain_relative_to.iso_date(), midnight_time_record());

            // g. Let targetDateTime be CombineISODateAndTimeRecord(targetDate, targetTime).
            let target_date_time = combine_iso_date_and_time_record(target_date, target_time);

            // h. Let total be ? DifferencePlainDateTimeWithTotal(isoDateTime, targetDateTime, calendar, unit).
            difference_plain_date_time_with_total(
                vm,
                &iso_date_time,
                &target_date_time,
                Utf16View::of_string(&calendar),
                unit_value,
            )?
        }
        // 14. Else,
        else {
            // a. Let largestUnit be DefaultTemporalLargestUnit(duration).
            let largest_unit = default_temporal_largest_unit(&duration);

            // b. If IsCalendarUnit(largestUnit) is true or IsCalendarUnit(unit) is true, throw a RangeError exception.
            if is_calendar_unit(largest_unit) {
                return vm.throw_completion(
                    ErrorKind::RangeError,
                    ErrorType::TemporalInvalidLargestUnit,
                    &[&temporal_unit_to_string(largest_unit)],
                );
            }
            if is_calendar_unit(unit_value) {
                return vm.throw_completion(
                    ErrorKind::RangeError,
                    ErrorType::TemporalInvalidLargestUnit,
                    &[&temporal_unit_to_string(unit_value)],
                );
            }

            // c. Let internalDuration be ToInternalDurationRecordWith24HourDays(duration).
            let internal_duration = to_internal_duration_record_with_24_hour_days(vm, &duration);

            // d. Let total be TotalTimeDuration(internalDuration.[[Time]], unit).
            total_time_duration(&internal_duration.time, unit_value)
        };

        // 15. Return 𝔽(total).
        Ok(Value::from_f64(total.to_double()))
    }

    // 7.3.22 Temporal.Duration.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, vm.argument(0))?;

        // 4. NOTE: The following steps read options and perform independent validation in alphabetical order
        //    (GetTemporalFractionalSecondDigitsOption reads "fractionalSecondDigits" and GetRoundingModeOption reads
        //    "roundingMode").

        // 5. Let digits be ? GetTemporalFractionalSecondDigitsOption(resolvedOptions).
        let digits = get_temporal_fractional_second_digits_option(vm, &resolved_options)?;

        // 6. Let roundingMode be ? GetRoundingModeOption(resolvedOptions, TRUNC).
        let rounding_mode = get_rounding_mode_option(vm, &resolved_options, RoundingMode::Trunc)?;

        // 7. Let smallestUnit be ? GetTemporalUnitValuedOption(resolvedOptions, "smallestUnit", UNSET).
        let smallest_unit =
            get_temporal_unit_valued_option(vm, &resolved_options, &vm.names.smallestUnit, UnitDefault::Unset)?;

        // 8. Perform ? ValidateTemporalUnitValue(smallestUnit, TIME).
        validate_temporal_unit_value(vm, &vm.names.smallestUnit, smallest_unit, UnitGroup::Time, &[])?;

        // 9. If smallestUnit is either HOUR or MINUTE, throw a RangeError exception.
        if let UnitValue::Unit(unit) = smallest_unit
            && (unit == Unit::Hour || unit == Unit::Minute)
        {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::OptionIsNotValidValue,
                &[&temporal_unit_to_string(unit), &vm.names.smallestUnit],
            );
        }

        // 10. Let precision be ToSecondsStringPrecisionRecord(smallestUnit, digits).
        let precision = to_seconds_string_precision_record(smallest_unit, digits);

        // 11. If precision.[[Unit]] is NANOSECOND and precision.[[Increment]] = 1, then
        if precision.unit == Unit::Nanosecond && precision.increment == 1 {
            // a. Return TemporalDurationToString(duration, precision.[[Precision]]).
            return Ok(string_value(
                vm,
                &temporal_duration_to_string(&duration, precision.precision.to_precision()),
            ));
        }

        // 12. Let largestUnit be DefaultTemporalLargestUnit(duration).
        let largest_unit = default_temporal_largest_unit(&duration);

        // 13. Let internalDuration be ToInternalDurationRecord(duration).
        let internal_duration = to_internal_duration_record(vm, &duration);

        // 14. Let timeDuration be ? RoundTimeDuration(internalDuration.[[Time]], precision.[[Increment]], precision.[[Unit]], roundingMode).
        let time_duration = round_time_duration(
            vm,
            &internal_duration.time,
            &SignedBigInteger::from(precision.increment),
            precision.unit,
            rounding_mode,
        )?;

        // 15. Set internalDuration to CombineDateAndTimeDuration(internalDuration.[[Date]], timeDuration).
        let internal_duration = combine_date_and_time_duration(internal_duration.date, time_duration);

        // 16. Let roundedLargestUnit be LargerOfTwoTemporalUnits(largestUnit, SECOND).
        let rounded_largest_unit = larger_of_two_temporal_units(largest_unit, Unit::Second);

        // 17. Let roundedDuration be ? TemporalDurationFromInternal(internalDuration, roundedLargestUnit).
        let rounded_duration = temporal_duration_from_internal(vm, &internal_duration, rounded_largest_unit)?;

        // 18. Return TemporalDurationToString(roundedDuration, precision.[[Precision]]).
        Ok(string_value(
            vm,
            &temporal_duration_to_string(&rounded_duration, precision.precision.to_precision()),
        ))
    }

    // 7.3.23 Temporal.Duration.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.tojson
    fn to_json(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Return TemporalDurationToString(duration, AUTO).
        Ok(string_value(
            vm,
            &temporal_duration_to_string(&duration, Precision::Auto),
        ))
    }

    // 7.3.24 Temporal.Duration.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.tolocalestring
    // 15.11.1.1 Temporal.Duration.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sup-temporal.duration.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let duration be the this value.
        // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
        let duration = typed_this_duration(vm)?;

        // 3. Let formatter be ? Construct(%Intl.DurationFormat%, « locales, options »).
        let formatter = construct(
            vm,
            realm.intrinsics().intl_duration_format_constructor(vm),
            &[locales, options],
            None,
        )?
        .downcast::<DurationFormat>()
        .expect("the Intl.DurationFormat constructor creates an Intl.DurationFormat");

        // 4. Let parts be PartitionDurationFormatPattern(formatter, duration).
        let parts = partition_duration_format_pattern(vm, formatter, duration);

        // 5. Let result be the empty String.
        let mut result = Utf16StringBuilder::new();

        // 6. For each Record { [[Type]], [[Value]], [[Unit]] } part in parts, do
        for part in &parts {
            // a. Set result to the string-concatenation of result and part.[[Value]].
            result.append(Utf16View::of_string(&part.value));
        }

        // 7. Return result.
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            result.to_utf16_string(),
        )))
    }

    // 7.3.25 Temporal.Duration.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::Convert,
            &[&"Temporal.Duration", &"a primitive value"],
        )
    }
}
