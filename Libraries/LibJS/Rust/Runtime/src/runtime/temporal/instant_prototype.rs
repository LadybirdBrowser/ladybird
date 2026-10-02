/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/InstantPrototype.cpp: %Temporal.Instant.prototype%.

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    RoundingMode, big_floor, get_options_object, get_rounding_increment_option, get_rounding_mode_option,
};
use crate::runtime::big_int::BigInt;
use crate::runtime::big_int_algorithms;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::date::{HOURS_PER_DAY, MINUTES_PER_HOUR, MS_PER_DAY, NS_PER_DAY, SECONDS_PER_MINUTE};
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
    get_temporal_fractional_second_digits_option, get_temporal_unit_valued_option, temporal_unit_to_string,
    to_seconds_string_precision_record, validate_temporal_rounding_increment, validate_temporal_unit_value,
};
use crate::runtime::temporal::calendar::ISO8601_CALENDAR;
use crate::runtime::temporal::instant::{
    Instant, NANOSECONDS_PER_MILLISECOND, add_duration_to_instant, create_temporal_instant,
    difference_temporal_instant, round_temporal_instant, temporal_instant_to_string, to_temporal_instant,
};
use crate::runtime::temporal::time_zone::to_temporal_time_zone_identifier;
use crate::runtime::temporal::zoned_date_time::create_temporal_zoned_date_time;
use crate::utf16::Utf16View;

// 8.3 Properties of the Temporal.Instant Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-instant-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct InstantPrototype {
    base: Object,
}

define_object_class!(InstantPrototype, extends: [Object], methods: {
    initialize: InstantPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_instant(vm: &Vm) -> ThrowCompletionOr<Gc<Instant>> {
    typed_this_object::<Instant>(vm, "Temporal.Instant")
}

fn string_value(vm: &Vm, string: &str) -> Value {
    Value::from_string(PrimitiveString::create(vm, Utf16String::from_utf8(string)))
}

impl InstantPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<InstantPrototype> {
        realm.create_object(
            vm,
            InstantPrototype {
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

        // 8.3.2 Temporal.Instant.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-instant-prototype-object
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Temporal.Instant")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        object.define_native_accessor(
            vm,
            realm,
            &names.epochMilliseconds,
            raw_native!(InstantPrototype::epoch_milliseconds_getter),
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
        object.define_native_accessor(
            vm,
            realm,
            &names.epochNanoseconds,
            raw_native!(InstantPrototype::epoch_nanoseconds_getter),
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };
        define_native_function(&names.add, raw_native!(InstantPrototype::add), 1);
        define_native_function(&names.subtract, raw_native!(InstantPrototype::subtract), 1);
        define_native_function(&names.until, raw_native!(InstantPrototype::until), 1);
        define_native_function(&names.since, raw_native!(InstantPrototype::since), 1);
        define_native_function(&names.round, raw_native!(InstantPrototype::round), 1);
        define_native_function(&names.equals, raw_native!(InstantPrototype::equals), 1);
        define_native_function(&names.toString, raw_native!(InstantPrototype::to_string), 0);
        define_native_function(
            &names.toLocaleString,
            raw_native!(InstantPrototype::to_locale_string),
            0,
        );
        define_native_function(&names.toJSON, raw_native!(InstantPrototype::to_json), 0);
        define_native_function(&names.valueOf, raw_native!(InstantPrototype::value_of), 0);
        define_native_function(
            &names.toZonedDateTimeISO,
            raw_native!(InstantPrototype::to_zoned_date_time_iso),
            1,
        );
    }

    // 8.3.3 get Temporal.Instant.prototype.epochMilliseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.instant.prototype.epochmilliseconds
    fn epoch_milliseconds_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let instant be the this value.
        // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
        let instant = typed_this_instant(vm)?;

        // 3. Let ns be instant.[[EpochNanoseconds]].
        let nanoseconds = instant.epoch_nanoseconds();

        // 4. Let ms be floor(ℝ(ns) / 10**6).
        let milliseconds = big_floor(nanoseconds.big_integer(), &NANOSECONDS_PER_MILLISECOND);

        // 5. Return 𝔽(ms).
        Ok(Value::from_f64(big_int_algorithms::to_double(&milliseconds)))
    }

    // 8.3.4 get Temporal.Instant.prototype.epochNanoseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.instant.prototype.epochnanoseconds
    fn epoch_nanoseconds_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let instant be the this value.
        // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
        let instant = typed_this_instant(vm)?;

        // 3. Return instant.[[EpochNanoseconds]].
        Ok(Value::from_bigint(instant.epoch_nanoseconds()))
    }

    // 8.3.5 Temporal.Instant.prototype.add ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.add
    fn add(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_duration_like = vm.argument(0);

        // 1. Let instant be the this value.
        // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
        let instant = typed_this_instant(vm)?;

        // 3. Return ? AddDurationToInstant(ADD, instant, temporalDurationLike).
        Ok(Value::from_object(add_duration_to_instant(
            vm,
            ArithmeticOperation::Add,
            &instant,
            temporal_duration_like,
        )?))
    }

    // 8.3.6 Temporal.Instant.prototype.subtract ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.subtract
    fn subtract(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_duration_like = vm.argument(0);

        // 1. Let instant be the this value.
        // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
        let instant = typed_this_instant(vm)?;

        // 3. Return ? AddDurationToInstant(SUBTRACT, instant, temporalDurationLike).
        Ok(Value::from_object(add_duration_to_instant(
            vm,
            ArithmeticOperation::Subtract,
            &instant,
            temporal_duration_like,
        )?))
    }

    // 8.3.7 Temporal.Instant.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.until
    fn until(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let instant be the this value.
        // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
        let instant = typed_this_instant(vm)?;

        // 3. Return ? DifferenceTemporalInstant(UNTIL, instant, other, options).
        Ok(Value::from_object(difference_temporal_instant(
            vm,
            DurationOperation::Until,
            &instant,
            other,
            options,
        )?))
    }

    // 8.3.8 Temporal.Instant.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.since
    fn since(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let instant be the this value.
        // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
        let instant = typed_this_instant(vm)?;

        // 3. Return ? DifferenceTemporalInstant(SINCE, instant, other, options).
        Ok(Value::from_object(difference_temporal_instant(
            vm,
            DurationOperation::Since,
            &instant,
            other,
            options,
        )?))
    }

    // 8.3.9 Temporal.Instant.prototype.round ( roundTo ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.round
    fn round(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let round_to_value = vm.argument(0);

        // 1. Let instant be the this value.
        // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
        let instant = typed_this_instant(vm)?;

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

        let maximum = match smallest_unit_value {
            // 11. If smallestUnit is hour, then
            // a. Let maximum be HoursPerDay.
            Unit::Hour => HOURS_PER_DAY,
            // 12. Else if smallestUnit is minute, then
            // a. Let maximum be MinutesPerHour × HoursPerDay.
            Unit::Minute => MINUTES_PER_HOUR * HOURS_PER_DAY,
            // 13. Else if smallestUnit is second, then
            // a. Let maximum be SecondsPerMinute × MinutesPerHour × HoursPerDay.
            Unit::Second => SECONDS_PER_MINUTE * MINUTES_PER_HOUR * HOURS_PER_DAY,
            // 14. Else if smallestUnit is millisecond, then
            // a. Let maximum be ℝ(msPerDay).
            Unit::Millisecond => MS_PER_DAY,
            // 15. Else if smallestUnit is microsecond, then
            // a. Let maximum be 10**3 × ℝ(msPerDay).
            Unit::Microsecond => 1000.0 * MS_PER_DAY,
            // 16. Else,
            // a. Assert: smallestUnit is nanosecond.
            // b. Let maximum be nsPerDay.
            Unit::Nanosecond => NS_PER_DAY,
            _ => unreachable!("the smallest unit is a time unit"),
        };

        // 17. Perform ? ValidateTemporalRoundingIncrement(roundingIncrement, maximum, true).
        validate_temporal_rounding_increment(vm, rounding_increment, maximum as u64, true)?;

        // 18. Let roundedNs be RoundTemporalInstant(instant.[[EpochNanoseconds]], roundingIncrement, smallestUnit, roundingMode).
        let rounded_nanoseconds = round_temporal_instant(
            instant.epoch_nanoseconds().big_integer(),
            rounding_increment,
            smallest_unit_value,
            rounding_mode,
        );

        // 19. Return ! CreateTemporalInstant(roundedNs).
        Ok(Value::from_object(
            create_temporal_instant(vm, BigInt::create(vm, rounded_nanoseconds), None).must(),
        ))
    }

    // 8.3.10 Temporal.Instant.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.equals
    fn equals(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let instant be the this value.
        // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
        let instant = typed_this_instant(vm)?;

        // 3. Set other to ? ToTemporalInstant(other).
        let other = to_temporal_instant(vm, vm.argument(0))?;

        // 4. If instant.[[EpochNanoseconds]] ≠ other.[[EpochNanoseconds]], return false.
        // 5. Return true.
        Ok(Value::from_bool(
            instant.epoch_nanoseconds().big_integer() == other.epoch_nanoseconds().big_integer(),
        ))
    }

    // 8.3.11 Temporal.Instant.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let instant be the this value.
        // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
        let instant = typed_this_instant(vm)?;

        // 3. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, vm.argument(0))?;

        // 4. NOTE: The following steps read options and perform independent validation in alphabetical order
        //    (GetTemporalFractionalSecondDigitsOption reads "fractionalSecondDigits" and GetRoundingModeOption reads "roundingMode").

        // 5. Let digits be ? GetTemporalFractionalSecondDigitsOption(resolvedOptions).
        let digits = get_temporal_fractional_second_digits_option(vm, &resolved_options)?;

        // 6. Let roundingMode be ? GetRoundingModeOption(resolvedOptions, trunc).
        let rounding_mode = get_rounding_mode_option(vm, &resolved_options, RoundingMode::Trunc)?;

        // 7. Let smallestUnit be ? GetTemporalUnitValuedOption(resolvedOptions, "smallestUnit", UNSET).
        let smallest_unit =
            get_temporal_unit_valued_option(vm, &resolved_options, &vm.names.smallestUnit, UnitDefault::Unset)?;

        // 8. Let timeZone be ? Get(resolvedOptions, "timeZone").
        let time_zone_value = resolved_options.get(vm, &vm.names.timeZone)?;

        // 9. Perform ? ValidateTemporalUnitValue(smallestUnit, TIME).
        validate_temporal_unit_value(vm, &vm.names.smallestUnit, smallest_unit, UnitGroup::Time, &[])?;

        // 10. If smallestUnit is HOUR, throw a RangeError exception.
        if smallest_unit == UnitValue::Unit(Unit::Hour) {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::OptionIsNotValidValue,
                &[&temporal_unit_to_string(Unit::Hour), &vm.names.smallestUnit],
            );
        }

        // 11. If timeZone is not undefined, then
        let time_zone = if time_zone_value.is_undefined() {
            None
        } else {
            // a. Set timeZone to ? ToTemporalTimeZoneIdentifier(timeZone).
            Some(to_temporal_time_zone_identifier(vm, time_zone_value)?)
        };

        // 12. Let precision be ToSecondsStringPrecisionRecord(smallestUnit, digits).
        let precision = to_seconds_string_precision_record(smallest_unit, digits);

        // 13. Let roundedNs be RoundTemporalInstant(instant.[[EpochNanoseconds]], precision.[[Increment]], precision.[[Unit]], roundingMode).
        let rounded_nanoseconds = round_temporal_instant(
            instant.epoch_nanoseconds().big_integer(),
            u64::from(precision.increment),
            precision.unit,
            rounding_mode,
        );

        // 14. Let roundedInstant be ! CreateTemporalInstant(roundedNs).
        let rounded_instant = create_temporal_instant(vm, BigInt::create(vm, rounded_nanoseconds), None).must();

        // 15. Return TemporalInstantToString(roundedInstant, timeZone, precision.[[Precision]]).
        Ok(string_value(
            vm,
            &temporal_instant_to_string(
                &rounded_instant,
                time_zone.as_ref().map(Utf16View::of_string),
                precision.precision,
            ),
        ))
    }

    // 8.3.12 Temporal.Instant.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.tolocalestring
    // 15.11.2.1 Temporal.Instant.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sup-temporal.instant.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let instant be the this value.
        // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
        typed_this_instant(vm)?;

        // 3. Let dateFormat be ? CreateDateTimeFormat(%Intl.DateTimeFormat%, locales, options, ANY, ALL).
        realm.intrinsics().intl_date_time_format_constructor(vm);

        // 4. Return ? FormatDateTime(dateFormat, instant).
        unimplemented_runtime_function(
            "Temporal.Instant.prototype.toLocaleString, which needs CreateDateTimeFormat and FormatDateTime",
            0,
        )
    }

    // 8.3.13 Temporal.Instant.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.tojson
    fn to_json(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let instant be the this value.
        // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
        let instant = typed_this_instant(vm)?;

        // 3. Return TemporalInstantToString(instant, undefined, AUTO).
        Ok(string_value(
            vm,
            &temporal_instant_to_string(&instant, None, SecondsPrecision::Auto),
        ))
    }

    // 8.3.14 Temporal.Instant.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::Convert,
            &[&"Temporal.Instant", &"a primitive value"],
        )
    }

    // 8.3.15 Temporal.Instant.prototype.toZonedDateTimeISO ( timeZone ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.tozoneddatetimeiso
    fn to_zoned_date_time_iso(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let instant be the this value.
        // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
        let instant = typed_this_instant(vm)?;

        // 3. Set timeZone to ? ToTemporalTimeZoneIdentifier(timeZone).
        let time_zone = to_temporal_time_zone_identifier(vm, vm.argument(0))?;

        // 4. Return ! CreateTemporalZonedDateTime(instant.[[EpochNanoseconds]], timeZone, "iso8601").
        Ok(Value::from_object(
            create_temporal_zoned_date_time(
                vm,
                instant.epoch_nanoseconds(),
                time_zone,
                Utf16String::from_utf8(ISO8601_CALENDAR),
                None,
            )
            .must(),
        ))
    }
}
