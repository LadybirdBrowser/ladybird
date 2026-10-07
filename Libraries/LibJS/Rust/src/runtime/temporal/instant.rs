/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Temporal.Instant objects and the epoch nanosecond operations.

use std::sync::LazyLock;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{RoundingMode, get_options_object, ordinary_create_from_constructor_of};
use crate::runtime::big_int::{BigInt, SignedBigInteger};
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::date::{get_utc_epoch_nanoseconds, parse_date_time_utc_offset};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::temporal::abstract_operations::{
    ArithmeticOperation, DurationOperation, SecondsPrecision, ShowCalendar, Unit, UnitCategory, UnitGroup, ascii_view,
    check_iso_days_range, get_difference_settings, parse_iso_date_time, round_number_to_increment_as_if_positive,
    temporal_unit_category, temporal_unit_length_in_nanoseconds, temporal_unit_to_string,
};
use crate::runtime::temporal::calendar::ISO8601_CALENDAR;
use crate::runtime::temporal::duration::{
    Duration, InternalDuration, add_time_duration_to_epoch_nanoseconds, combine_date_and_time_duration,
    create_negated_temporal_duration, default_temporal_largest_unit, round_time_duration,
    temporal_duration_from_internal, time_duration_from_epoch_nanoseconds_difference,
    to_internal_duration_record_with_24_hour_days, to_temporal_duration, zero_date_duration,
};
use crate::runtime::temporal::iso_records::{Time, TimeDuration, TimeOrStartOfDay};
use crate::runtime::temporal::iso8601::Production;
use crate::runtime::temporal::plain_date_time::{balance_iso_date_time, iso_date_time_to_string};
use crate::runtime::temporal::plain_time::midnight_time_record;
use crate::runtime::temporal::time_zone::{
    UTC_TIME_ZONE, format_date_time_utc_offset_rounded, get_iso_date_time_for, get_offset_nanoseconds_for,
};
use crate::runtime::temporal::zoned_date_time::ZonedDateTime;
use crate::runtime::value::PreferredType;
use crate::utf16::Utf16View;

// 8 Temporal.Instant Objects, https://tc39.es/proposal-temporal/#sec-temporal-instant-objects
#[repr(C)]
#[derive(Trace)]
pub struct Instant {
    base: Object,
    epoch_nanoseconds: Gc<BigInt>, // [[EpochNanoseconds]]
}

define_cell!(Instant, Object, extends: [Object]);

impl core::ops::Deref for Instant {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Instant {
    fn new(vm: &Vm, epoch_nanoseconds: Gc<BigInt>, prototype: Gc<Object>) -> Instant {
        Instant {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            epoch_nanoseconds,
        }
    }

    pub fn epoch_nanoseconds(&self) -> Gc<BigInt> {
        self.epoch_nanoseconds
    }
}

fn big_integer_constant(digits: &str) -> SignedBigInteger {
    digits.parse().expect("a valid integer")
}

// nsMaxInstant = 10**8 × nsPerDay = 8.64 × 10**21
// https://tc39.es/proposal-temporal/#eqn-nsMaxInstant
pub static NANOSECONDS_MAX_INSTANT: LazyLock<SignedBigInteger> =
    LazyLock::new(|| big_integer_constant("8640000000000000000000"));

// nsMinInstant = -nsMaxInstant = -8.64 × 10**21
// https://tc39.es/proposal-temporal/#eqn-nsMinInstant
pub static NANOSECONDS_MIN_INSTANT: LazyLock<SignedBigInteger> =
    LazyLock::new(|| big_integer_constant("-8640000000000000000000"));

// nsPerDay = 10**6 × ℝ(msPerDay) = 8.64 × 10**13
// https://tc39.es/proposal-temporal/#eqn-nsPerDay
pub static NANOSECONDS_PER_DAY: LazyLock<SignedBigInteger> =
    LazyLock::new(|| SignedBigInteger::from(86_400_000_000_000_i64));

// Non-standard:
pub static NANOSECONDS_PER_HOUR: LazyLock<SignedBigInteger> =
    LazyLock::new(|| SignedBigInteger::from(3_600_000_000_000_i64));
pub static NANOSECONDS_PER_MINUTE: LazyLock<SignedBigInteger> =
    LazyLock::new(|| SignedBigInteger::from(60_000_000_000_i64));
pub static NANOSECONDS_PER_SECOND: LazyLock<SignedBigInteger> =
    LazyLock::new(|| SignedBigInteger::from(1_000_000_000_i64));
pub static NANOSECONDS_PER_MILLISECOND: LazyLock<SignedBigInteger> =
    LazyLock::new(|| SignedBigInteger::from(1_000_000_i64));
pub static NANOSECONDS_PER_MICROSECOND: LazyLock<SignedBigInteger> =
    LazyLock::new(|| SignedBigInteger::from(1_000_i64));
pub static NANOSECONDS_PER_NANOSECOND: LazyLock<SignedBigInteger> = LazyLock::new(|| SignedBigInteger::from(1_i64));

pub static MICROSECONDS_PER_MILLISECOND: LazyLock<SignedBigInteger> =
    LazyLock::new(|| SignedBigInteger::from(1_000_i64));
pub static MILLISECONDS_PER_SECOND: LazyLock<SignedBigInteger> = LazyLock::new(|| SignedBigInteger::from(1_000_i64));
pub static SECONDS_PER_MINUTE: LazyLock<SignedBigInteger> = LazyLock::new(|| SignedBigInteger::from(60_i64));
pub static MINUTES_PER_HOUR: LazyLock<SignedBigInteger> = LazyLock::new(|| SignedBigInteger::from(60_i64));
pub static HOURS_PER_DAY: LazyLock<SignedBigInteger> = LazyLock::new(|| SignedBigInteger::from(24_i64));

// 8.5.1 IsValidEpochNanoseconds ( epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidepochnanoseconds
pub fn is_valid_epoch_nanoseconds(epoch_nanoseconds: &SignedBigInteger) -> bool {
    // 1. If ℝ(epochNanoseconds) < nsMinInstant or ℝ(epochNanoseconds) > nsMaxInstant, return false; else return true.
    *epoch_nanoseconds >= *NANOSECONDS_MIN_INSTANT && *epoch_nanoseconds <= *NANOSECONDS_MAX_INSTANT
}

// 8.5.2 CreateTemporalInstant ( epochNanoseconds [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidepochnanoseconds
pub fn create_temporal_instant(
    vm: &Vm,
    epoch_nanoseconds: Gc<BigInt>,
    new_target: Option<Gc<FunctionObject>>,
) -> ThrowCompletionOr<Gc<Instant>> {
    let realm = vm.current_realm().expect("CreateTemporalInstant runs in a realm");

    // 1.  Assert: IsValidEpochNanoseconds(epochNanoseconds) is true.
    assert!(is_valid_epoch_nanoseconds(epoch_nanoseconds.big_integer()));

    // 2. If newTarget is not present, set newTarget to %Temporal.Instant%.
    let new_target = new_target.unwrap_or_else(|| realm.intrinsics().temporal_instant_constructor(vm));

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.Instant.prototype%", « [[InitializedTemporalInstant]], [[EpochNanoseconds]] »).
    // 4. Set object.[[EpochNanoseconds]] to epochNanoseconds.
    let object = ordinary_create_from_constructor_of(
        vm,
        realm,
        new_target,
        Intrinsics::temporal_instant_prototype,
        |prototype| Instant::new(vm, epoch_nanoseconds, prototype),
    )?;

    // 5. Return object.
    Ok(object)
}

// 8.5.3 ToTemporalInstant ( item ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalinstant
pub fn to_temporal_instant(vm: &Vm, mut item: Value) -> ThrowCompletionOr<Gc<Instant>> {
    // 1. If item is an Object, then
    if item.is_object() {
        let object = item.as_object();

        // a. If item has an [[InitializedTemporalInstant]] or [[InitializedTemporalZonedDateTime]] internal slot, then
        //     i. Return ! CreateTemporalInstant(item.[[EpochNanoseconds]]).
        if let Some(instant) = object.downcast::<Instant>() {
            return Ok(create_temporal_instant(vm, instant.epoch_nanoseconds(), None).must());
        }
        if let Some(zoned_date_time) = object.downcast::<ZonedDateTime>() {
            return Ok(create_temporal_instant(vm, zoned_date_time.epoch_nanoseconds(), None).must());
        }

        // b. NOTE: This use of ToPrimitive allows Instant-like objects to be converted.
        // c. Set item to ? ToPrimitive(item, STRING).
        item = item.to_primitive(vm, PreferredType::String)?;
    }

    // 2. If item is not a String, throw a TypeError exception.
    if !item.is_string() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::TemporalInvalidInstantString, &[&item]);
    }

    // 3. Let parsed be ? ParseISODateTime(item, « TemporalInstantString »).
    let item_string = item.as_string().utf16_string();
    let parsed = parse_iso_date_time(
        vm,
        Utf16View::of_string(&item_string),
        &[Production::TemporalInstantString],
    )?;

    // 4. Assert: Either parsed.[[TimeZone]].[[OffsetString]] is not empty or parsed.[[TimeZone]].[[Z]] is true, but not both.
    let offset_string = &parsed.time_zone.offset_string;
    let z_designator = parsed.time_zone.z_designator;

    assert!(offset_string.is_some() || z_designator);
    assert!(offset_string.is_none() || !z_designator);

    // 5. If parsed.[[TimeZone]].[[Z]] is true, let offsetNanoseconds be 0; else, let offsetNanoseconds be
    //    ! ParseDateTimeUTCOffset(parsed.[[TimeZone]].[[OffsetString]]).
    let offset_nanoseconds = match offset_string {
        Some(offset_string) if !z_designator => parse_date_time_utc_offset(Utf16View::of_string(offset_string)),
        _ => 0.0,
    };

    // 6. If parsed.[[Time]] is START-OF-DAY, let time be MidnightTimeRecord(); else let time be parsed.[[Time]].
    let time = match parsed.time {
        TimeOrStartOfDay::StartOfDay => midnight_time_record(),
        TimeOrStartOfDay::Time(time) => time,
    };

    // 7. Let balanced be BalanceISODateTime(parsed.[[Year]], parsed.[[Month]], parsed.[[Day]], time.[[Hour]], time.[[Minute]], time.[[Second]], time.[[Millisecond]], time.[[Microsecond]], time.[[Nanosecond]] - offsetNanoseconds).
    let balanced = balance_iso_date_time(
        f64::from(parsed.year.expect("an instant string has a year")),
        f64::from(parsed.month),
        f64::from(parsed.day),
        f64::from(time.hour),
        f64::from(time.minute),
        f64::from(time.second),
        f64::from(time.millisecond),
        f64::from(time.microsecond),
        f64::from(time.nanosecond) - offset_nanoseconds,
    );

    // 8. Perform ? CheckISODaysRange(balanced.[[ISODate]]).
    check_iso_days_range(vm, balanced.iso_date)?;

    // 9. Let epochNanoseconds be GetUTCEpochNanoseconds(balanced).
    let epoch_nanoseconds = get_utc_epoch_nanoseconds(&balanced);

    // 10. If IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
    if !is_valid_epoch_nanoseconds(&epoch_nanoseconds) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidEpochNanoseconds, &[]);
    }

    // 11. Return ! CreateTemporalInstant(epochNanoseconds).
    Ok(create_temporal_instant(vm, BigInt::create(vm, epoch_nanoseconds), None).must())
}

// 8.5.4 CompareEpochNanoseconds ( epochNanosecondsOne, epochNanosecondsTwo ), https://tc39.es/proposal-temporal/#sec-temporal-compareepochnanoseconds
pub fn compare_epoch_nanoseconds(
    epoch_nanoseconds_one: &SignedBigInteger,
    epoch_nanoseconds_two: &SignedBigInteger,
) -> i8 {
    // 1. If epochNanosecondsOne > epochNanosecondsTwo, return 1.
    if epoch_nanoseconds_one > epoch_nanoseconds_two {
        return 1;
    }

    // 2. If epochNanosecondsOne < epochNanosecondsTwo, return -1.
    if epoch_nanoseconds_one < epoch_nanoseconds_two {
        return -1;
    }

    // 3. Return 0.
    0
}

// 8.5.5 AddInstant ( epochNanoseconds, timeDuration ), https://tc39.es/proposal-temporal/#sec-temporal-addinstant
pub fn add_instant(
    vm: &Vm,
    epoch_nanoseconds: &SignedBigInteger,
    time_duration: &TimeDuration,
) -> ThrowCompletionOr<SignedBigInteger> {
    // 1. Let result be AddTimeDurationToEpochNanoseconds(timeDuration, epochNanoseconds).
    let result = add_time_duration_to_epoch_nanoseconds(time_duration, epoch_nanoseconds);

    // 2. If IsValidEpochNanoseconds(result) is false, throw a RangeError exception.
    if !is_valid_epoch_nanoseconds(&result) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidEpochNanoseconds, &[]);
    }

    // 3. Return result.
    Ok(result)
}

// 8.5.6 DifferenceInstant ( ns1, ns2, roundingIncrement, smallestUnit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-differenceinstant
pub fn difference_instant(
    vm: &Vm,
    nanoseconds1: &SignedBigInteger,
    nanoseconds2: &SignedBigInteger,
    rounding_increment: u64,
    smallest_unit: Unit,
    rounding_mode: RoundingMode,
) -> InternalDuration {
    // 1. Let timeDuration be TimeDurationFromEpochNanosecondsDifference(ns2, ns1).
    let time_duration = time_duration_from_epoch_nanoseconds_difference(nanoseconds2, nanoseconds1);

    // 2. Set timeDuration to ! RoundTimeDuration(timeDuration, roundingIncrement, smallestUnit, roundingMode).
    let time_duration = round_time_duration(
        vm,
        &time_duration,
        &SignedBigInteger::from(rounding_increment),
        smallest_unit,
        rounding_mode,
    )
    .must();

    // 3. Return CombineDateAndTimeDuration(ZeroDateDuration(), timeDuration).
    combine_date_and_time_duration(zero_date_duration(vm), time_duration)
}

// 8.5.7 RoundTemporalInstant ( ns, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundtemporalinstant
pub fn round_temporal_instant(
    nanoseconds: &SignedBigInteger,
    increment: u64,
    unit: Unit,
    rounding_mode: RoundingMode,
) -> SignedBigInteger {
    // 1. Let unitLength be the value in the "Length in Nanoseconds" column of the row of Table 21 whose "Value" column contains unit.
    let unit_length = temporal_unit_length_in_nanoseconds(unit);

    // 2. Let incrementNs be increment × unitLength.
    let increment_nanoseconds = SignedBigInteger::from(increment) * unit_length;

    // 3. Return ℤ(RoundNumberToIncrementAsIfPositive(ℝ(ns), incrementNs, roundingMode)).
    round_number_to_increment_as_if_positive(nanoseconds, &increment_nanoseconds, rounding_mode)
}

// 8.5.8 TemporalInstantToString ( instant, timeZone, precision ), https://tc39.es/proposal-temporal/#sec-temporal-temporalinstanttostring
pub fn temporal_instant_to_string(
    instant: &Instant,
    time_zone: Option<Utf16View<'_>>,
    precision: SecondsPrecision,
) -> String {
    // 1. Let outputTimeZone be timeZone.
    // 2. If outputTimeZone is undefined, set outputTimeZone to "UTC".
    let output_time_zone = time_zone.unwrap_or(ascii_view(UTC_TIME_ZONE));

    // 3. Let epochNs be instant.[[EpochNanoseconds]].
    let epoch_nanoseconds = instant.epoch_nanoseconds();
    let epoch_nanoseconds = epoch_nanoseconds.big_integer();

    // 4. Let isoDateTime be GetISODateTimeFor(outputTimeZone, epochNs).
    let iso_date_time = get_iso_date_time_for(output_time_zone, epoch_nanoseconds);

    // 5. Let dateTimeString be ISODateTimeToString(isoDateTime, "iso8601", precision, NEVER).
    let date_time_string = iso_date_time_to_string(
        &iso_date_time,
        ascii_view(ISO8601_CALENDAR),
        precision,
        ShowCalendar::Never,
    );

    // 6. If timeZone is undefined, then
    let time_zone_string = if time_zone.is_none() {
        // a. Let timeZoneString be "Z".
        "Z".to_string()
    }
    // 7. Else,
    else {
        // a. Let offsetNanoseconds be GetOffsetNanosecondsFor(outputTimeZone, epochNs).
        let offset_nanoseconds = get_offset_nanoseconds_for(output_time_zone, epoch_nanoseconds);

        // b. Let timeZoneString be FormatDateTimeUTCOffsetRounded(offsetNanoseconds).
        format_date_time_utc_offset_rounded(offset_nanoseconds)
    };

    // 8. Return the string-concatenation of dateTimeString and timeZoneString.
    format!("{date_time_string}{time_zone_string}")
}

// 8.5.9 DifferenceTemporalInstant ( operation, instant, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalinstant
pub fn difference_temporal_instant(
    vm: &Vm,
    operation: DurationOperation,
    instant: &Instant,
    other_value: Value,
    options: Value,
) -> ThrowCompletionOr<Gc<Duration>> {
    // 1. Set other to ? ToTemporalInstant(other).
    let other = to_temporal_instant(vm, other_value)?;

    // 2. Let resolvedOptions be ? GetOptionsObject(options).
    let resolved_options = get_options_object(vm, options)?;

    // 3. Let settings be ? GetDifferenceSettings(operation, resolvedOptions, TIME, « », NANOSECOND, SECOND).
    let settings = get_difference_settings(
        vm,
        operation,
        &resolved_options,
        UnitGroup::Time,
        &[],
        Unit::Nanosecond,
        Unit::Second,
    )?;

    // 4. Let internalDuration be DifferenceInstant(instant.[[EpochNanoseconds]], other.[[EpochNanoseconds]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]]).
    let internal_duration = difference_instant(
        vm,
        &instant.epoch_nanoseconds().big_integer().clone(),
        &other.epoch_nanoseconds().big_integer().clone(),
        settings.rounding_increment,
        settings.smallest_unit,
        settings.rounding_mode,
    );

    // 5. Let result be ! TemporalDurationFromInternal(internalDuration, settings.[[LargestUnit]]).
    let mut result = temporal_duration_from_internal(vm, &internal_duration, settings.largest_unit).must();

    // 6. If operation is SINCE, set result to CreateNegatedTemporalDuration(result).
    if operation == DurationOperation::Since {
        result = create_negated_temporal_duration(vm, &result);
    }

    // 7. Return result.
    Ok(result)
}

// 8.5.10 AddDurationToInstant ( operation, instant, temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtoinstant
pub fn add_duration_to_instant(
    vm: &Vm,
    operation: ArithmeticOperation,
    instant: &Instant,
    temporal_duration_like: Value,
) -> ThrowCompletionOr<Gc<Instant>> {
    // 1. Let duration be ? ToTemporalDuration(temporalDurationLike).
    let mut duration = to_temporal_duration(vm, temporal_duration_like)?;

    // 2. If operation is SUBTRACT, set duration to CreateNegatedTemporalDuration(duration).
    if operation == ArithmeticOperation::Subtract {
        duration = create_negated_temporal_duration(vm, &duration);
    }

    // 3. Let largestUnit be DefaultTemporalLargestUnit(duration).
    let largest_unit = default_temporal_largest_unit(&duration);

    // 4. If TemporalUnitCategory(largestUnit) is DATE, throw a RangeError exception.
    if temporal_unit_category(largest_unit) == UnitCategory::Date {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TemporalInvalidLargestUnit,
            &[&temporal_unit_to_string(largest_unit)],
        );
    }

    // 5. Let internalDuration be ToInternalDurationRecordWith24HourDays(duration).
    let internal_duration = to_internal_duration_record_with_24_hour_days(vm, &duration);

    // 6. Let ns be ? AddInstant(instant.[[EpochNanoseconds]], internalDuration.[[Time]]).
    let epoch_nanoseconds = instant.epoch_nanoseconds().big_integer().clone();
    let nanoseconds = add_instant(vm, &epoch_nanoseconds, &internal_duration.time)?;

    // 7. Return ! CreateTemporalInstant(ns).
    Ok(create_temporal_instant(vm, BigInt::create(vm, nanoseconds), None).must())
}

/// The Time Record of a parse result that is not START-OF-DAY.
pub fn parsed_time(time: TimeOrStartOfDay) -> Time {
    Time::from_parsed(time)
}
