/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Time zone identifiers, offsets, and the conversions between epoch nanoseconds and the wall-clock time of a time
//! zone.

use std::cell::RefCell;
use std::collections::HashMap;

use ak::Utf16String;
use num_traits::Zero;

use crate::interpreter::vm::Vm;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{RoundingMode, big_floor, big_modulo, modulo};
use crate::runtime::big_int::SignedBigInteger;
use crate::runtime::big_int_algorithms;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::date::{
    clip_bigint_to_sane_time, get_named_time_zone_epoch_nanoseconds, get_named_time_zone_offset_nanoseconds,
    get_utc_epoch_nanoseconds, hour_from_time, is_offset_time_zone_identifier, min_from_time, ms_from_time,
    parse_date_time_utc_offset_from_parse_result, sec_from_time,
};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::abstract_operations::get_available_named_time_zone_identifier;
use crate::runtime::temporal::abstract_operations::{
    Disambiguation, SecondsPrecision, TimeStyle, check_iso_days_range, format_time_string,
    parse_temporal_time_zone_string, round_number_to_increment,
};
use crate::runtime::temporal::date_equations::{
    epoch_time_to_date, epoch_time_to_epoch_year, epoch_time_to_month_in_year,
};
use crate::runtime::temporal::duration::time_duration_from_components;
use crate::runtime::temporal::instant::{
    NANOSECONDS_MAX_INSTANT, NANOSECONDS_MIN_INSTANT, NANOSECONDS_PER_DAY, NANOSECONDS_PER_MILLISECOND,
    is_valid_epoch_nanoseconds,
};
use crate::runtime::temporal::iso_records::{ISODate, ISODateTime, ParsedTimeZoneIdentifier};
use crate::runtime::temporal::iso8601::{ParseResult, Production, SubMinutePrecision, parse_iso8601, parse_utc_offset};
use crate::runtime::temporal::plain_date::{add_days_to_iso_date, create_iso_date_record};
use crate::runtime::temporal::plain_date_time::{balance_iso_date_time, combine_iso_date_and_time_record};
use crate::runtime::temporal::plain_time::{add_time, create_time_record, midnight_time_record};
use crate::runtime::temporal::zoned_date_time::ZonedDateTime;
use crate::unicode::time_zone::{
    IncludeGivenTime, TimeZoneTransitionOptions, TransitionDirection, TransitionRule, UnixDateTime,
    get_time_zone_transition,
};
use crate::utf16::Utf16View;

pub const UTC_TIME_ZONE: &str = "UTC";

// 11.1.2 GetISOPartsFromEpoch ( epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-getisopartsfromepoch
pub fn get_iso_parts_from_epoch(epoch_nanoseconds: &SignedBigInteger) -> ISODateTime {
    // 1. Assert: IsValidEpochNanoseconds(ℤ(epochNanoseconds)) is true.
    assert!(is_valid_epoch_nanoseconds(epoch_nanoseconds));

    // 2. Let remainderNs be epochNanoseconds modulo 10**6.
    let remainder_nanoseconds = big_modulo(epoch_nanoseconds, &NANOSECONDS_PER_MILLISECOND);
    let remainder_nanoseconds_value = big_int_algorithms::to_double(&remainder_nanoseconds);

    // 3. Let epochMilliseconds be 𝔽((epochNanoseconds - remainderNs) / 10**6).
    let epoch_milliseconds =
        big_int_algorithms::to_double(&((epoch_nanoseconds - &remainder_nanoseconds) / &*NANOSECONDS_PER_MILLISECOND));

    // 4. Let year be EpochTimeToEpochYear(epochMilliseconds).
    let year = epoch_time_to_epoch_year(epoch_milliseconds);

    // 5. Let month be EpochTimeToMonthInYear(epochMilliseconds) + 1.
    let month = epoch_time_to_month_in_year(epoch_milliseconds) + 1;

    // 6. Let day be EpochTimeToDate(epochMilliseconds).
    let day = epoch_time_to_date(epoch_milliseconds);

    // 7. Let hour be ℝ(HourFromTime(epochMilliseconds)).
    let hour = hour_from_time(epoch_milliseconds);

    // 8. Let minute be ℝ(MinFromTime(epochMilliseconds)).
    let minute = min_from_time(epoch_milliseconds);

    // 9. Let second be ℝ(SecFromTime(epochMilliseconds)).
    let second = sec_from_time(epoch_milliseconds);

    // 10. Let millisecond be ℝ(msFromTime(epochMilliseconds)).
    let millisecond = ms_from_time(epoch_milliseconds);

    // 11. Let microsecond be floor(remainderNs / 1000).
    let microsecond = (remainder_nanoseconds_value / 1000.0).floor();

    // 12. Assert: microsecond < 1000.
    assert!(microsecond < 1000.0);

    // 13. Let nanosecond be remainderNs modulo 1000.
    let nanosecond = modulo(remainder_nanoseconds_value, 1000.0);

    // 14. Let isoDate be CreateISODateRecord(year, month, day).
    let iso_date = create_iso_date_record(f64::from(year), f64::from(month), f64::from(day));

    // 15. Let time be CreateTimeRecord(hour, minute, second, millisecond, microsecond, nanosecond).
    let time = create_time_record(
        f64::from(hour),
        f64::from(minute),
        f64::from(second),
        f64::from(millisecond),
        microsecond,
        nanosecond,
        0.0,
    );

    // 16. Return CombineISODateAndTimeRecord(isoDate, time).
    combine_iso_date_and_time_record(iso_date, time)
}

// 11.1.3 GetNamedTimeZoneNextTransition ( timeZoneIdentifier, epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-getnamedtimezonenexttransition
pub fn get_named_time_zone_next_transition(
    time_zone: Utf16View<'_>,
    epoch_nanoseconds: &SignedBigInteger,
) -> Option<SignedBigInteger> {
    let epoch_milliseconds = big_floor(epoch_nanoseconds, &NANOSECONDS_PER_MILLISECOND);
    let time = UnixDateTime::from_milliseconds_since_epoch(clip_bigint_to_sane_time(&epoch_milliseconds));

    let options = TimeZoneTransitionOptions {
        direction: TransitionDirection::Next,
        include_given_time: IncludeGivenTime::No,
        transition_rule: TransitionRule::TransitionWhereUTCOffsetChanges,
    };
    let time_zone_transition = get_time_zone_transition(time_zone, time, options)?;

    let result_nanoseconds = SignedBigInteger::from(time_zone_transition) * &*NANOSECONDS_PER_MILLISECOND;
    if result_nanoseconds > *NANOSECONDS_MAX_INSTANT {
        return None;
    }

    Some(result_nanoseconds)
}

// 11.1.4 GetNamedTimeZonePreviousTransition ( timeZoneIdentifier, epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-getnamedtimezoneprevioustransition
pub fn get_named_time_zone_previous_transition(
    time_zone: Utf16View<'_>,
    epoch_nanoseconds: &SignedBigInteger,
) -> Option<SignedBigInteger> {
    let epoch_milliseconds = big_floor(epoch_nanoseconds, &NANOSECONDS_PER_MILLISECOND);
    let time = UnixDateTime::from_milliseconds_since_epoch(clip_bigint_to_sane_time(&epoch_milliseconds));

    // Assume there's a hypothetical time zone with 10000ms as a time zone transition and arbitrary transitions before that time.
    // If there's sub-millisecond precision, for example 10000.1ms, it will be floored to 10000ms.
    // If we then don't include the given time, we will go on to find a transition before 10000ms, which is incorrect because it should find
    // the 10000ms transition when going backwards from 10000.1ms.
    let remainder = big_modulo(epoch_nanoseconds, &NANOSECONDS_PER_MILLISECOND);
    let has_sub_millisecond_precision = !remainder.is_zero();

    let options = TimeZoneTransitionOptions {
        direction: TransitionDirection::Previous,
        include_given_time: if has_sub_millisecond_precision {
            IncludeGivenTime::Yes
        } else {
            IncludeGivenTime::No
        },
        transition_rule: TransitionRule::TransitionWhereUTCOffsetChanges,
    };
    let time_zone_transition = get_time_zone_transition(time_zone, time, options)?;

    let result_nanoseconds = SignedBigInteger::from(time_zone_transition) * &*NANOSECONDS_PER_MILLISECOND;
    if result_nanoseconds < *NANOSECONDS_MIN_INSTANT {
        return None;
    }

    Some(result_nanoseconds)
}

// 11.1.5 FormatOffsetTimeZoneIdentifier ( offsetMinutes [ , style ] ), https://tc39.es/proposal-temporal/#sec-temporal-formatoffsettimezoneidentifier
pub fn format_offset_time_zone_identifier(offset_minutes: i64, style: Option<TimeStyle>) -> String {
    // 1. If offsetMinutes ≥ 0, let sign be the code unit 0x002B (PLUS SIGN); else, let sign be the code unit 0x002D (HYPHEN-MINUS).
    let sign = if offset_minutes >= 0 { '+' } else { '-' };

    // 2. Let absoluteMinutes be abs(offsetMinutes).
    let absolute_minutes = offset_minutes.unsigned_abs();

    // 3. Let hour be floor(absoluteMinutes / 60).
    let hour = (absolute_minutes as f64 / 60.0).floor() as u8;

    // 4. Let minute be absoluteMinutes modulo 60.
    let minute = modulo(absolute_minutes as f64, 60.0) as u8;

    // 5. Let timeString be FormatTimeString(hour, minute, 0, 0, MINUTE, style).
    let time_string = format_time_string(hour, minute, 0, 0, SecondsPrecision::Minute, style);

    // 6. Return the string-concatenation of sign and timeString.
    format!("{sign}{time_string}")
}

// 11.1.6 FormatUTCOffsetNanoseconds ( offsetNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-formatutcoffsetnanoseconds
pub fn format_utc_offset_nanoseconds(offset_nanoseconds: i64) -> String {
    // 1. If offsetNanoseconds ≥ 0, let sign be the code unit 0x002B (PLUS SIGN); else, let sign be the code unit 0x002D (HYPHEN-MINUS).
    let sign = if offset_nanoseconds >= 0 { '+' } else { '-' };

    // 2. Let absoluteNanoseconds be abs(offsetNanoseconds).
    let absolute_nanoseconds = offset_nanoseconds.unsigned_abs() as f64;

    // 3. Let hour be floor(absoluteNanoseconds / (3600 × 10**9)).
    let hour = (absolute_nanoseconds / 3_600_000_000_000.0).floor();

    // 4. Let minute be floor(absoluteNanoseconds / (60 × 10**9)) modulo 60.
    let minute = modulo((absolute_nanoseconds / 60_000_000_000.0).floor(), 60.0);

    // 5. Let second be floor(absoluteNanoseconds / 10**9) modulo 60.
    let second = modulo((absolute_nanoseconds / 1_000_000_000.0).floor(), 60.0);

    // 6. Let subSecondNanoseconds be absoluteNanoseconds modulo 10**9.
    let sub_second_nanoseconds = modulo(absolute_nanoseconds, 1_000_000_000.0);

    // 7. If second = 0 and subSecondNanoseconds = 0, let precision be MINUTE; else, let precision be AUTO.
    let precision = if second == 0.0 && sub_second_nanoseconds == 0.0 {
        SecondsPrecision::Minute
    } else {
        SecondsPrecision::Auto
    };

    // 8. Let timeString be FormatTimeString(hour, minute, second, subSecondNanoseconds, precision).
    let time_string = format_time_string(
        hour as u8,
        minute as u8,
        second as u8,
        sub_second_nanoseconds as u64,
        precision,
        None,
    );

    // 9. Return the string-concatenation of sign and timeString.
    format!("{sign}{time_string}")
}

// 11.1.7 FormatDateTimeUTCOffsetRounded ( offsetNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-formatdatetimeutcoffsetrounded
pub fn format_date_time_utc_offset_rounded(offset_nanoseconds: i64) -> String {
    // 1. Set offsetNanoseconds to RoundNumberToIncrement(offsetNanoseconds, 60 × 10**9, HALF-EXPAND).
    let offset_nanoseconds_value =
        round_number_to_increment(offset_nanoseconds as f64, 60_000_000_000, RoundingMode::HalfExpand);

    // 2. Let offsetMinutes be offsetNanoseconds / (60 × 10**9).
    let offset_minutes = offset_nanoseconds_value / 60_000_000_000.0;

    // 3. Assert: offsetMinutes is an integer.
    assert!(offset_minutes.trunc() == offset_minutes);

    // 4. Return FormatOffsetTimeZoneIdentifier(offsetMinutes).
    format_utc_offset_nanoseconds((offset_minutes as i64).wrapping_mul(60_000_000_000))
}

// 11.1.8 ToTemporalTimeZoneIdentifier ( temporalTimeZoneLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaltimezoneidentifier
pub fn to_temporal_time_zone_identifier(vm: &Vm, temporal_time_zone_like: Value) -> ThrowCompletionOr<Utf16String> {
    // 1. If temporalTimeZoneLike is an Object and temporalTimeZoneLike has an [[InitializedTemporalZonedDateTime]]
    //    internal slot, return temporalTimeZoneLike.[[TimeZone]].
    if temporal_time_zone_like.is_object()
        && let Some(zoned_date_time) = temporal_time_zone_like.as_object().downcast::<ZonedDateTime>()
    {
        return Ok(zoned_date_time.time_zone());
    }

    // 2. If temporalTimeZoneLike is not a String, throw a TypeError exception.
    if !temporal_time_zone_like.is_string() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::TemporalInvalidTimeZoneName,
            &[&temporal_time_zone_like],
        );
    }

    let temporal_time_zone_like = temporal_time_zone_like.as_string().utf16_string();
    to_temporal_time_zone_identifier_from_string(vm, Utf16View::of_string(&temporal_time_zone_like))
}

// 11.1.8 ToTemporalTimeZoneIdentifier ( temporalTimeZoneLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaltimezoneidentifier
pub fn to_temporal_time_zone_identifier_from_string(
    vm: &Vm,
    temporal_time_zone_like: Utf16View<'_>,
) -> ThrowCompletionOr<Utf16String> {
    // 3. Let parseResult be ? ParseTemporalTimeZoneString(temporalTimeZoneLike).
    let parse_result = parse_temporal_time_zone_string(vm, temporal_time_zone_like)?;

    // 4. Let offsetMinutes be parseResult.[[OffsetMinutes]].
    // 5. If offsetMinutes is not empty, return FormatOffsetTimeZoneIdentifier(offsetMinutes).
    if let Some(offset_minutes) = parse_result.offset_minutes {
        return Ok(Utf16String::from_utf8(&format_offset_time_zone_identifier(
            offset_minutes,
            None,
        )));
    }

    // 6. Let name be parseResult.[[Name]].
    let name = parse_result
        .name
        .expect("a time zone identifier has a name or an offset");

    // 7. Let timeZoneIdentifierRecord be GetAvailableNamedTimeZoneIdentifier(name).
    let time_zone_identifier_record = get_available_named_time_zone_identifier(Utf16View::of_string(&name));

    // 8. If timeZoneIdentifierRecord is empty, throw a RangeError exception.
    let Some(time_zone_identifier_record) = time_zone_identifier_record else {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TemporalInvalidTimeZoneName,
            &[&temporal_time_zone_like],
        );
    };

    // 9. Return timeZoneIdentifierRecord.[[Identifier]].
    Ok(time_zone_identifier_record.identifier.clone())
}

// 11.1.9 GetOffsetNanosecondsFor ( timeZone, epochNs ), https://tc39.es/proposal-temporal/#sec-temporal-getoffsetnanosecondsfor
pub fn get_offset_nanoseconds_for(time_zone: Utf16View<'_>, epoch_nanoseconds: &SignedBigInteger) -> i64 {
    // 1. Let parseResult be ! ParseTimeZoneIdentifier(timeZone).
    let parse_result = parse_time_zone_identifier(time_zone);

    // 2. If parseResult.[[OffsetMinutes]] is not empty, return parseResult.[[OffsetMinutes]] × (60 × 10**9).
    if let Some(offset_minutes) = parse_result.offset_minutes {
        return offset_minutes * 60_000_000_000;
    }

    // 3. Return GetNamedTimeZoneOffsetNanoseconds(parseResult.[[Name]], epochNs).
    let name = parse_result
        .name
        .expect("a time zone identifier has a name or an offset");
    get_named_time_zone_offset_nanoseconds(Utf16View::of_string(&name), epoch_nanoseconds).offset_nanoseconds
}

// 11.1.10 GetISODateTimeFor ( timeZone, epochNs ), https://tc39.es/proposal-temporal/#sec-temporal-getisodatetimefor
pub fn get_iso_date_time_for(time_zone: Utf16View<'_>, epoch_nanoseconds: &SignedBigInteger) -> ISODateTime {
    // 1. Let offsetNanoseconds be GetOffsetNanosecondsFor(timeZone, epochNs).
    let offset_nanoseconds = get_offset_nanoseconds_for(time_zone, epoch_nanoseconds);

    // 2. Let result be GetISOPartsFromEpoch(ℝ(epochNs)).
    let result = get_iso_parts_from_epoch(epoch_nanoseconds);

    // 3. Return BalanceISODateTime(result.[[ISODate]].[[Year]], result.[[ISODate]].[[Month]], result.[[ISODate]].[[Day]], result.[[Time]].[[Hour]], result.[[Time]].[[Minute]], result.[[Time]].[[Second]], result.[[Time]].[[Millisecond]], result.[[Time]].[[Microsecond]], result.[[Time]].[[Nanosecond]] + offsetNanoseconds).
    balance_iso_date_time(
        f64::from(result.iso_date.year),
        f64::from(result.iso_date.month),
        f64::from(result.iso_date.day),
        f64::from(result.time.hour),
        f64::from(result.time.minute),
        f64::from(result.time.second),
        f64::from(result.time.millisecond),
        f64::from(result.time.microsecond),
        f64::from(result.time.nanosecond) + offset_nanoseconds as f64,
    )
}

// 11.1.11 GetEpochNanosecondsFor ( timeZone, isoDateTime, disambiguation ), https://tc39.es/proposal-temporal/#sec-temporal-getepochnanosecondsfor
pub fn get_epoch_nanoseconds_for(
    vm: &Vm,
    time_zone: Utf16View<'_>,
    iso_date_time: &ISODateTime,
    disambiguation: Disambiguation,
) -> ThrowCompletionOr<SignedBigInteger> {
    // 1. Let possibleEpochNs be ? GetPossibleEpochNanoseconds(timeZone, isoDateTime).
    let possible_epoch_ns = get_possible_epoch_nanoseconds(vm, time_zone, iso_date_time)?;

    // 2. Return ? DisambiguatePossibleEpochNanoseconds(possibleEpochNs, timeZone, isoDateTime, disambiguation).
    disambiguate_possible_epoch_nanoseconds(vm, possible_epoch_ns, time_zone, iso_date_time, disambiguation)
}

// 11.1.12 DisambiguatePossibleEpochNanoseconds ( possibleEpochNs, timeZone, isoDateTime, disambiguation ), https://tc39.es/proposal-temporal/#sec-temporal-disambiguatepossibleepochnanoseconds
pub fn disambiguate_possible_epoch_nanoseconds(
    vm: &Vm,
    mut possible_epoch_ns: Vec<SignedBigInteger>,
    time_zone: Utf16View<'_>,
    iso_date_time: &ISODateTime,
    disambiguation: Disambiguation,
) -> ThrowCompletionOr<SignedBigInteger> {
    // 1. Let n be the number of elements in possibleEpochNs.
    let n = possible_epoch_ns.len();

    // 2. If n = 1, return the sole element of possibleEpochNs.
    if n == 1 {
        return Ok(possible_epoch_ns.swap_remove(0));
    }

    // 3. If n ≠ 0, then
    if n != 0 {
        // a. If disambiguation is either EARLIER or COMPATIBLE, return possibleEpochNs[0].
        if disambiguation == Disambiguation::Earlier || disambiguation == Disambiguation::Compatible {
            return Ok(possible_epoch_ns.swap_remove(0));
        }

        // b. If disambiguation is LATER, return possibleEpochNs[n - 1].
        if disambiguation == Disambiguation::Later {
            return Ok(possible_epoch_ns.swap_remove(n - 1));
        }

        // c. Assert: disambiguation is REJECT.
        assert!(disambiguation == Disambiguation::Reject);

        // d. Throw a RangeError exception.
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TemporalDisambiguatePossibleEpochNSRejectMoreThanOne,
            &[],
        );
    }

    // 4. Assert: n = 0.

    // 5. If disambiguation is REJECT, throw a RangeError exception.
    if disambiguation == Disambiguation::Reject {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TemporalDisambiguatePossibleEpochNSRejectZero,
            &[],
        );
    }

    // 6. Let before be the latest possible ISO Date-Time Record for which CompareISODateTime(before, isoDateTime) = -1
    //    and ! GetPossibleEpochNanoseconds(timeZone, before) is not empty.
    // 7. Let after be the earliest possible ISO Date-Time Record for which CompareISODateTime(after, isoDateTime) = 1
    //    and ! GetPossibleEpochNanoseconds(timeZone, after) is not empty.
    // 8. Let beforePossible be ! GetPossibleEpochNanoseconds(timeZone, before).
    // 9. Assert: The number of elements in beforePossible = 1.
    // 10. Let afterPossible be ! GetPossibleEpochNanoseconds(timeZone, after).
    // 11. Assert: The number of elements in afterPossible = 1.
    // NB: We implement this by finding the UTC offsets one day before and after the gap, which is guaranteed to be
    //     outside the transition period. We then use those offsets to determine the before/after epoch nanoseconds.
    let epoch_nanoseconds = get_utc_epoch_nanoseconds(iso_date_time);
    let before_possible = &epoch_nanoseconds - &*NANOSECONDS_PER_DAY;
    let after_possible = &epoch_nanoseconds + &*NANOSECONDS_PER_DAY;

    // 12. Let offsetBefore be GetOffsetNanosecondsFor(timeZone, the sole element of beforePossible).
    let offset_before = get_offset_nanoseconds_for(time_zone, &before_possible);

    // 13. Let offsetAfter be GetOffsetNanosecondsFor(timeZone, the sole element of afterPossible).
    let offset_after = get_offset_nanoseconds_for(time_zone, &after_possible);

    // 14. Let nanoseconds be offsetAfter - offsetBefore.
    let nanoseconds = offset_after - offset_before;

    // 15. Assert: abs(nanoseconds) ≤ nsPerDay.

    // 16. If disambiguation is EARLIER, then
    if disambiguation == Disambiguation::Earlier {
        // a. Let timeDuration be TimeDurationFromComponents(0, 0, 0, 0, 0, -nanoseconds).
        let time_duration = time_duration_from_components(0.0, 0.0, 0.0, 0.0, 0.0, -(nanoseconds as f64));

        // b. Let earlierTime be AddTime(isoDateTime.[[Time]], timeDuration).
        let earlier_time = add_time(&iso_date_time.time, &time_duration);

        // c. Let earlierDate be AddDaysToISODate(isoDateTime.[[ISODate]], earlierTime.[[Days]]).
        let earlier_date = add_days_to_iso_date(iso_date_time.iso_date, earlier_time.days);

        // d. Let earlierDateTime be CombineISODateAndTimeRecord(earlierDate, earlierTime).
        let earlier_date_time = combine_iso_date_and_time_record(earlier_date, earlier_time);

        // e. Set possibleEpochNs to ? GetPossibleEpochNanoseconds(timeZone, earlierDateTime).
        let mut possible_epoch_ns = get_possible_epoch_nanoseconds(vm, time_zone, &earlier_date_time)?;

        // f. Assert: possibleEpochNs is not empty.
        assert!(!possible_epoch_ns.is_empty());

        // g. Return possibleEpochNs[0].
        return Ok(possible_epoch_ns.swap_remove(0));
    }

    // 17. Assert: disambiguation is COMPATIBLE or LATER.
    assert!(disambiguation == Disambiguation::Compatible || disambiguation == Disambiguation::Later);

    // 18. Let timeDuration be TimeDurationFromComponents(0, 0, 0, 0, 0, nanoseconds).
    let time_duration = time_duration_from_components(0.0, 0.0, 0.0, 0.0, 0.0, nanoseconds as f64);

    // 19. Let laterTime be AddTime(isoDateTime.[[Time]], timeDuration).
    let later_time = add_time(&iso_date_time.time, &time_duration);

    // 20. Let laterDate be AddDaysToISODate(isoDateTime.[[ISODate]], laterTime.[[Days]]).
    let later_date = add_days_to_iso_date(iso_date_time.iso_date, later_time.days);

    // 21. Let laterDateTime be CombineISODateAndTimeRecord(laterDate, laterTime).
    let later_date_time = combine_iso_date_and_time_record(later_date, later_time);

    // 22. Set possibleEpochNs to ? GetPossibleEpochNanoseconds(timeZone, laterDateTime).
    let mut possible_epoch_ns = get_possible_epoch_nanoseconds(vm, time_zone, &later_date_time)?;

    // 23. Set n to the number of elements in possibleEpochNs.
    let n = possible_epoch_ns.len();

    // 24. Assert: n ≠ 0.
    assert!(n != 0);

    // 25. Return possibleEpochNs[n - 1].
    Ok(possible_epoch_ns.swap_remove(n - 1))
}

// 11.1.13 GetPossibleEpochNanoseconds ( timeZone, isoDateTime ), https://tc39.es/proposal-temporal/#sec-temporal-getpossibleepochnanoseconds
pub fn get_possible_epoch_nanoseconds(
    vm: &Vm,
    time_zone: Utf16View<'_>,
    iso_date_time: &ISODateTime,
) -> ThrowCompletionOr<Vec<SignedBigInteger>> {
    // 1. Let parseResult be ! ParseTimeZoneIdentifier(timeZone).
    let parse_result = parse_time_zone_identifier(time_zone);

    // 2. If parseResult.[[OffsetMinutes]] is not empty, then
    let possible_epoch_nanoseconds = if let Some(offset_minutes) = parse_result.offset_minutes {
        // a. Let balanced be BalanceISODateTime(isoDateTime.[[ISODate]].[[Year]], isoDateTime.[[ISODate]].[[Month]], isoDateTime.[[ISODate]].[[Day]], isoDateTime.[[Time]].[[Hour]], isoDateTime.[[Time]].[[Minute]] - parseResult.[[OffsetMinutes]], isoDateTime.[[Time]].[[Second]], isoDateTime.[[Time]].[[Millisecond]], isoDateTime.[[Time]].[[Microsecond]], isoDateTime.[[Time]].[[Nanosecond]]).
        let balanced = balance_iso_date_time(
            f64::from(iso_date_time.iso_date.year),
            f64::from(iso_date_time.iso_date.month),
            f64::from(iso_date_time.iso_date.day),
            f64::from(iso_date_time.time.hour),
            f64::from(iso_date_time.time.minute) - offset_minutes as f64,
            f64::from(iso_date_time.time.second),
            f64::from(iso_date_time.time.millisecond),
            f64::from(iso_date_time.time.microsecond),
            f64::from(iso_date_time.time.nanosecond),
        );

        // b. Perform ? CheckISODaysRange(balanced.[[ISODate]]).
        check_iso_days_range(vm, balanced.iso_date)?;

        // c. Let epochNanoseconds be GetUTCEpochNanoseconds(balanced).
        let epoch_nanoseconds = get_utc_epoch_nanoseconds(&balanced);

        // d. Let possibleEpochNanoseconds be « epochNanoseconds ».
        vec![epoch_nanoseconds]
    }
    // 3. Else,
    else {
        // a. Let possibleEpochNanoseconds be GetNamedTimeZoneEpochNanoseconds(parseResult.[[Name]], isoDateTime).
        let name = parse_result
            .name
            .expect("a time zone identifier has a name or an offset");
        get_named_time_zone_epoch_nanoseconds(Utf16View::of_string(&name), iso_date_time)
    };

    // 4. For each value epochNanoseconds in possibleEpochNanoseconds, do
    for epoch_nanoseconds in &possible_epoch_nanoseconds {
        // a. If IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
        if !is_valid_epoch_nanoseconds(epoch_nanoseconds) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidEpochNanoseconds, &[]);
        }
    }

    // 5. Return possibleEpochNanoseconds.
    Ok(possible_epoch_nanoseconds)
}

// 11.1.14 GetStartOfDay ( timeZone, isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-getstartofday
pub fn get_start_of_day(vm: &Vm, time_zone: Utf16View<'_>, iso_date: ISODate) -> ThrowCompletionOr<SignedBigInteger> {
    // 1. Let isoDateTime be CombineISODateAndTimeRecord(isoDate, MidnightTimeRecord()).
    let iso_date_time = combine_iso_date_and_time_record(iso_date, midnight_time_record());

    // 2. Let possibleEpochNs be ? GetPossibleEpochNanoseconds(timeZone, isoDateTime).
    let mut possible_epoch_nanoseconds = get_possible_epoch_nanoseconds(vm, time_zone, &iso_date_time)?;

    // 3. If possibleEpochNs is not empty, return possibleEpochNs[0].
    if !possible_epoch_nanoseconds.is_empty() {
        return Ok(possible_epoch_nanoseconds.swap_remove(0));
    }

    // 4. Assert: IsOffsetTimeZoneIdentifier(timeZone) is false.
    assert!(!is_offset_time_zone_identifier(time_zone));

    // 5. Let possibleEpochNsAfter be GetNamedTimeZoneEpochNanoseconds(timeZone, isoDateTimeAfter), where isoDateTimeAfter
    //    is the ISO Date-Time Record for which DifferenceISODateTime(isoDateTime, isoDateTimeAfter, "iso8601", hour).[[Time]]
    //    is the smallest possible value > 0 for which possibleEpochNsAfter is not empty (i.e., isoDateTimeAfter represents
    //    the first local time after the transition).
    // NB: We implement this by finding the next UTC offset transition after one day before midnight, which is guaranteed
    //     to be before the gap. The transition instant is the first valid epoch nanoseconds of the day.
    let epoch_nanoseconds = get_utc_epoch_nanoseconds(&iso_date_time);
    let day_before = &epoch_nanoseconds - &*NANOSECONDS_PER_DAY;
    let possible_epoch_nanoseconds_after = get_named_time_zone_next_transition(time_zone, &day_before);

    // 6. Assert: The number of elements in possibleEpochNsAfter = 1.
    // 7. Return the sole element of possibleEpochNsAfter.
    Ok(possible_epoch_nanoseconds_after.expect("a skipped midnight has a transition after the day before"))
}

// 11.1.15 TimeZoneEquals ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-timezoneequals
pub fn time_zone_equals(one: Utf16View<'_>, two: Utf16View<'_>) -> bool {
    // 1. If one is two, return true.
    if one == two {
        return true;
    }

    // NB: IsOffsetTimeZoneIdentifier simply invokes parse_utc_offset and returns whether it has a value. We do this
    //     manually here so that we can handle the offset minutes assertion below without any extra performance penalty.
    let time_zone_offset_one = parse_utc_offset(one, SubMinutePrecision::No);
    let time_zone_offset_two = parse_utc_offset(two, SubMinutePrecision::No);

    // 2. If IsOffsetTimeZoneIdentifier(one) is false and IsOffsetTimeZoneIdentifier(two) is false, then
    if time_zone_offset_one.is_none() && time_zone_offset_two.is_none() {
        // a. Let recordOne be GetAvailableNamedTimeZoneIdentifier(one).
        let record_one = get_available_named_time_zone_identifier(one);

        // b. Let recordTwo be GetAvailableNamedTimeZoneIdentifier(two).
        let record_two = get_available_named_time_zone_identifier(two);

        // c. Assert: recordOne is not EMPTY.
        let record_one = record_one.expect("a named time zone of a ZonedDateTime is available");

        // d. Assert: recordTwo is not EMPTY.
        let record_two = record_two.expect("a named time zone of a ZonedDateTime is available");

        // e. If recordOne.[[PrimaryIdentifier]] is recordTwo.[[PrimaryIdentifier]], return true.
        if record_one.primary_identifier == record_two.primary_identifier {
            return true;
        }
    }

    // 3. Assert: If one and two are both offset time zone identifiers, they do not represent the same number of offset minutes.
    if let (Some(offset_one), Some(offset_two)) = (&time_zone_offset_one, &time_zone_offset_two) {
        assert!(offset_one.minutes != offset_two.minutes);
    }

    // 4. Return false.
    false
}

std::thread_local! {
    // OPTIMIZATION: The result of parsing a time zone identifier will not change, so we can cache the result.
    static TIME_ZONE_ID_CACHE: RefCell<HashMap<Vec<u16>, ParsedTimeZoneIdentifier>> = RefCell::new(HashMap::new());
}

fn cached_time_zone_identifier(cache_key: &[u16]) -> Option<ParsedTimeZoneIdentifier> {
    TIME_ZONE_ID_CACHE.with_borrow(|cache| cache.get(cache_key).cloned())
}

fn cache_time_zone_identifier(cache_key: Vec<u16>, result: &ParsedTimeZoneIdentifier) {
    TIME_ZONE_ID_CACHE.with_borrow_mut(|cache| {
        cache.insert(cache_key, result.clone());
    });
}

// 11.1.16 ParseTimeZoneIdentifier ( identifier ), https://tc39.es/proposal-temporal/#sec-parsetimezoneidentifier
pub fn parse_time_zone_identifier_or_throw(
    vm: &Vm,
    identifier: Utf16View<'_>,
) -> ThrowCompletionOr<ParsedTimeZoneIdentifier> {
    let cache_key: Vec<u16> = identifier.code_units().collect();
    if let Some(result) = cached_time_zone_identifier(&cache_key) {
        return Ok(result);
    }

    // 1. Let parseResult be ParseText(StringToCodePoints(identifier), TimeZoneIdentifier).
    let parse_result = parse_iso8601(Production::TimeZoneIdentifier, identifier);

    // 2. If parseResult is a List of errors, throw a RangeError exception.
    let Some(parse_result) = parse_result else {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TemporalInvalidTimeZoneString,
            &[&identifier],
        );
    };

    let result = parse_time_zone_identifier_from_parse_result(&parse_result);
    cache_time_zone_identifier(cache_key, &result);

    Ok(result)
}

// 11.1.16 ParseTimeZoneIdentifier ( identifier ), https://tc39.es/proposal-temporal/#sec-parsetimezoneidentifier
pub fn parse_time_zone_identifier(identifier: Utf16View<'_>) -> ParsedTimeZoneIdentifier {
    let cache_key: Vec<u16> = identifier.code_units().collect();

    // OPTIMIZATION: Some callers can assume that parsing will succeed.
    if let Some(result) = cached_time_zone_identifier(&cache_key) {
        return result;
    }

    // 1. Let parseResult be ParseText(StringToCodePoints(identifier), TimeZoneIdentifier).
    let parse_result =
        parse_iso8601(Production::TimeZoneIdentifier, identifier).expect("the time zone identifier is valid");

    let result = parse_time_zone_identifier_from_parse_result(&parse_result);
    cache_time_zone_identifier(cache_key, &result);
    result
}

// 11.1.16 ParseTimeZoneIdentifier ( identifier ), https://tc39.es/proposal-temporal/#sec-parsetimezoneidentifier
pub fn parse_time_zone_identifier_from_parse_result(parse_result: &ParseResult<'_>) -> ParsedTimeZoneIdentifier {
    // OPTIMIZATION: Some callers will have already parsed and validated the time zone identifier.

    // 3. If parseResult contains a TimeZoneIANAName Parse Node, then
    if let Some(time_zone_iana_name) = parse_result.time_zone_iana_name {
        // a. Let name be the source text matched by the TimeZoneIANAName Parse Node contained within parseResult.
        // b. NOTE: name is syntactically valid, but does not necessarily conform to IANA Time Zone Database naming
        //    guidelines or correspond with an available named time zone identifier.
        // c. Return Time Zone Identifier Parse Record { [[Name]]: CodePointsToString(name), [[OffsetMinutes]]: EMPTY }.
        return ParsedTimeZoneIdentifier {
            name: Some(time_zone_iana_name.to_utf16_string()),
            offset_minutes: None,
        };
    }

    // 4. Assert: parseResult contains a UTCOffset[~SubMinutePrecision] Parse Node.
    let time_zone_offset = parse_result
        .time_zone_offset
        .as_ref()
        .expect("a time zone identifier is a name or an offset");

    // 5. Let offset be the source text matched by the UTCOffset[~SubMinutePrecision] Parse Node contained within parseResult.
    // 6. Let offsetNanoseconds be ! ParseDateTimeUTCOffset(CodePointsToString(offset)).
    let offset_nanoseconds = parse_date_time_utc_offset_from_parse_result(time_zone_offset);

    // 7. Let offsetMinutes be offsetNanoseconds / (60 × 10**9).
    let offset_minutes = offset_nanoseconds / 60_000_000_000.0;

    // 8. Return Time Zone Identifier Parse Record { [[Name]]: empty, [[OffsetMinutes]]: offsetMinutes }.
    ParsedTimeZoneIdentifier {
        name: None,
        offset_minutes: Some(offset_minutes as i64),
    }
}
