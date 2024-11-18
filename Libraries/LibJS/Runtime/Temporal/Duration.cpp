/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/DurationConstructor.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <math.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(Duration);

// 7 Temporal.Duration Objects, https://tc39.es/proposal-temporal/#sec-temporal-duration-objects
Duration::Duration(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_years(years)
    , m_months(months)
    , m_weeks(weeks)
    , m_days(days)
    , m_hours(hours)
    , m_minutes(minutes)
    , m_seconds(seconds)
    , m_milliseconds(milliseconds)
    , m_microseconds(microseconds)
    , m_nanoseconds(nanoseconds)
{
    auto fields = AK::Array {
        &Duration::m_years,
        &Duration::m_months,
        &Duration::m_weeks,
        &Duration::m_days,
        &Duration::m_hours,
        &Duration::m_minutes,
        &Duration::m_seconds,
        &Duration::m_milliseconds,
        &Duration::m_microseconds,
        &Duration::m_nanoseconds,
    };

    // NOTE: The spec stores these fields as mathematical values. VERIFY() that we have finite, integral values in them,
    //       and normalize any negative zeros caused by floating point math. This is usually done using ℝ(𝔽(value)) at
    //       the call site.
    for (auto const& field : fields) {
        auto& value = this->*field;
        VERIFY(isfinite(value));
        // FIXME: test-js contains a small number of cases where a Temporal.Duration is constructed with a non-integral
        //        double. Eliminate these and VERIFY(trunc(value) == value) instead.
        if (trunc(value) != value)
            value = trunc(value);
        else if (bit_cast<u64>(value) == NEGATIVE_ZERO_BITS)
            value = 0;
    }
}

// maxTimeDuration = 2**53 × 10**9 - 1 = 9,007,199,254,740,991,999,999,999
TimeDuration const MAX_TIME_DURATION = "9007199254740991999999999"_sbigint;

// 7.5.4 ZeroDateDuration ( ), https://tc39.es/proposal-temporal/#sec-temporal-zerodateduration
DateDuration zero_date_duration(VM& vm)
{
    // 1. Return ! CreateDateDurationRecord(0, 0, 0, 0).
    return MUST(create_date_duration_record(vm, 0, 0, 0, 0));
}

// 7.5.5 ToInternalDurationRecord ( duration ), https://tc39.es/proposal-temporal/#sec-temporal-tointernaldurationrecord
InternalDuration to_internal_duration_record(VM& vm, Duration const& duration)
{
    // 1. Let dateDuration be ! CreateDateDurationRecord(duration.[[Years]], duration.[[Months]], duration.[[Weeks]], duration.[[Days]]).
    auto date_duration = MUST(create_date_duration_record(vm, duration.years(), duration.months(), duration.weeks(), duration.days()));

    // 2. Let timeDuration be TimeDurationFromComponents(duration.[[Hours]], duration.[[Minutes]], duration.[[Seconds]], duration.[[Milliseconds]], duration.[[Microseconds]], duration.[[Nanoseconds]]).
    auto time_duration = time_duration_from_components(duration.hours(), duration.minutes(), duration.seconds(), duration.milliseconds(), duration.microseconds(), duration.nanoseconds());

    // 3. Return ! CombineDateAndTimeDuration(dateDuration, timeDuration).
    return MUST(combine_date_and_time_duration(vm, date_duration, move(time_duration)));
}

// 7.5.6 ToInternalDurationRecordWith24HourDays ( duration ), https://tc39.es/proposal-temporal/#sec-temporal-tointernaldurationrecordwith24hourdays
InternalDuration to_internal_duration_record_with_24_hour_days(VM& vm, Duration const& duration)
{
    // 1. Let timeDuration be TimeDurationFromComponents(duration.[[Hours]], duration.[[Minutes]], duration.[[Seconds]], duration.[[Milliseconds]], duration.[[Microseconds]], duration.[[Nanoseconds]]).
    auto time_duration = time_duration_from_components(duration.hours(), duration.minutes(), duration.seconds(), duration.milliseconds(), duration.microseconds(), duration.nanoseconds());

    // 2. Set timeDuration to ! Add24HourDaysToTimeDuration(timeDuration, duration.[[Days]]).
    time_duration = MUST(add_24_hour_days_to_time_duration(vm, time_duration, duration.days()));

    // 3. Let dateDuration be ! CreateDateDurationRecord(duration.[[Years]], duration.[[Months]], duration.[[Weeks]], 0).
    auto date_duration = MUST(create_date_duration_record(vm, duration.years(), duration.months(), duration.weeks(), 0));

    // 4. Return ! CombineDateAndTimeDuration(dateDuration, timeDuration).
    return MUST(combine_date_and_time_duration(vm, date_duration, move(time_duration)));
}

// 7.5.8 TemporalDurationFromInternal ( internalDuration, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-temporaldurationfrominternal
ThrowCompletionOr<GC::Ref<Duration>> temporal_duration_from_internal(VM& vm, InternalDuration const& internal_duration, Unit largest_unit)
{
    // 1. Let days, hours, minutes, seconds, milliseconds, and microseconds be 0.
    double days = 0;
    double hours = 0;
    double minutes = 0;
    double seconds = 0;
    double milliseconds = 0;
    double microseconds = 0;

    // 2. Let sign be TimeDurationSign(internalDuration.[[Time]]).
    auto sign = time_duration_sign(internal_duration.time);

    // 3. Let nanoseconds be abs(internalDuration.[[Time]]).
    auto const& absolute_nanoseconds = internal_duration.time.unsigned_value();
    double nanoseconds = 0;

    // 4. If TemporalUnitCategory(largestUnit) is date, then
    if (temporal_unit_category(largest_unit) == UnitCategory::Date) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = absolute_nanoseconds.divided_by(NANOSECONDS_PER_MICROSECOND);

        // b. Set nanoseconds to nanoseconds modulo 1000.
        nanoseconds = nanoseconds_division_result.remainder.to_double();

        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(MICROSECONDS_PER_MILLISECOND);

        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();

        // e. Set seconds to floor(milliseconds / 1000).
        auto milliseconds_division_result = microseconds_division_result.quotient.divided_by(MILLISECONDS_PER_SECOND);

        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = milliseconds_division_result.remainder.to_double();

        // g. Set minutes to floor(seconds / 60).
        auto seconds_division_result = milliseconds_division_result.quotient.divided_by(SECONDS_PER_MINUTE);

        // h. Set seconds to seconds modulo 60.
        seconds = seconds_division_result.remainder.to_double();

        // i. Set hours to floor(minutes / 60).
        auto minutes_division_result = seconds_division_result.quotient.divided_by(MINUTES_PER_HOUR);

        // j. Set minutes to minutes modulo 60.
        minutes = minutes_division_result.remainder.to_double();

        // k. Set days to floor(hours / 24).
        auto hours_division_result = minutes_division_result.quotient.divided_by(HOURS_PER_DAY);
        days = hours_division_result.quotient.to_double();

        // l. Set hours to hours modulo 24.
        hours = hours_division_result.remainder.to_double();
    }
    // 5. Else if largestUnit is hour, then
    else if (largest_unit == Unit::Hour) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = absolute_nanoseconds.divided_by(NANOSECONDS_PER_MICROSECOND);

        // b. Set nanoseconds to nanoseconds modulo 1000.
        nanoseconds = nanoseconds_division_result.remainder.to_double();

        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(MICROSECONDS_PER_MILLISECOND);

        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();

        // e. Set seconds to floor(milliseconds / 1000).
        auto milliseconds_division_result = microseconds_division_result.quotient.divided_by(MILLISECONDS_PER_SECOND);

        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = milliseconds_division_result.remainder.to_double();

        // g. Set minutes to floor(seconds / 60).
        auto seconds_division_result = milliseconds_division_result.quotient.divided_by(SECONDS_PER_MINUTE);

        // h. Set seconds to seconds modulo 60.
        seconds = seconds_division_result.remainder.to_double();

        // i. Set hours to floor(minutes / 60).
        auto minutes_division_result = seconds_division_result.quotient.divided_by(MINUTES_PER_HOUR);
        hours = minutes_division_result.quotient.to_double();

        // j. Set minutes to minutes modulo 60.
        minutes = minutes_division_result.remainder.to_double();
    }
    // 6. Else if largestUnit is minute, then
    else if (largest_unit == Unit::Minute) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = absolute_nanoseconds.divided_by(Crypto::UnsignedBigInteger(NANOSECONDS_PER_MICROSECOND));

        // b. Set nanoseconds to nanoseconds modulo 1000.
        nanoseconds = nanoseconds_division_result.remainder.to_double();

        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(MICROSECONDS_PER_MILLISECOND);

        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();

        // e. Set seconds to floor(milliseconds / 1000).
        auto milliseconds_division_result = microseconds_division_result.quotient.divided_by(MILLISECONDS_PER_SECOND);

        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = milliseconds_division_result.remainder.to_double();

        // g. Set minutes to floor(seconds / 60).
        auto seconds_division_result = milliseconds_division_result.quotient.divided_by(SECONDS_PER_MINUTE);
        minutes = seconds_division_result.quotient.to_double();

        // h. Set seconds to seconds modulo 60.
        seconds = seconds_division_result.remainder.to_double();
    }
    // 7. Else if largestUnit is second, then
    else if (largest_unit == Unit::Second) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = absolute_nanoseconds.divided_by(NANOSECONDS_PER_MICROSECOND);

        // b. Set nanoseconds to nanoseconds modulo 1000.
        nanoseconds = nanoseconds_division_result.remainder.to_double();

        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(MICROSECONDS_PER_MILLISECOND);

        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();

        // e. Set seconds to floor(milliseconds / 1000).
        auto milliseconds_division_result = microseconds_division_result.quotient.divided_by(MILLISECONDS_PER_SECOND);
        seconds = milliseconds_division_result.quotient.to_double();

        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = milliseconds_division_result.remainder.to_double();
    }
    // 8. Else if largestUnit is millisecond, then
    else if (largest_unit == Unit::Millisecond) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = absolute_nanoseconds.divided_by(NANOSECONDS_PER_MICROSECOND);

        // b. Set nanoseconds to nanoseconds modulo 1000.
        nanoseconds = nanoseconds_division_result.remainder.to_double();

        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(MICROSECONDS_PER_MILLISECOND);
        milliseconds = microseconds_division_result.quotient.to_double();

        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();
    }
    // 9. Else if largestUnit is microsecond, then
    else if (largest_unit == Unit::Microsecond) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = absolute_nanoseconds.divided_by(NANOSECONDS_PER_MICROSECOND);
        microseconds = nanoseconds_division_result.quotient.to_double();

        // b. Set nanoseconds to nanoseconds modulo 1000.
        nanoseconds = nanoseconds_division_result.remainder.to_double();
    }
    // 10. Else,
    else {
        // a. Assert: largestUnit is nanosecond.
        VERIFY(largest_unit == Unit::Nanosecond);
        nanoseconds = absolute_nanoseconds.to_double();
    }

    // 11. NOTE: When largestUnit is millisecond, microsecond, or nanosecond, milliseconds, microseconds, or nanoseconds
    //     may be an unsafe integer. In this case, care must be taken when implementing the calculation using floating
    //     point arithmetic. It can be implemented in C++ using std::fma(). String manipulation will also give an exact
    //     result, since the multiplication is by a power of 10.

    // 12. Return ? CreateTemporalDuration(internalDuration.[[Date]].[[Years]], internalDuration.[[Date]].[[Months]], internalDuration.[[Date]].[[Weeks]], internalDuration.[[Date]].[[Days]] + days × sign, hours × sign, minutes × sign, seconds × sign, milliseconds × sign, microseconds × sign, nanoseconds × sign).
    return TRY(create_temporal_duration(vm, internal_duration.date.years, internal_duration.date.months, internal_duration.date.weeks, internal_duration.date.days + (days * sign), hours * sign, minutes * sign, seconds * sign, milliseconds * sign, microseconds * sign, nanoseconds * sign));
}

// 7.5.9 CreateDateDurationRecord ( years, months, weeks, days ), https://tc39.es/proposal-temporal/#sec-temporal-createdatedurationrecord
ThrowCompletionOr<DateDuration> create_date_duration_record(VM& vm, double years, double months, double weeks, double days)
{
    // 1. If IsValidDuration(years, months, weeks, days, 0, 0, 0, 0, 0, 0) is false, throw a RangeError exception.
    if (!is_valid_duration(years, months, weeks, days, 0, 0, 0, 0, 0, 0))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDuration);

    // 2. Return Date Duration Record { [[Years]]: ℝ(𝔽(years)), [[Months]]: ℝ(𝔽(months)), [[Weeks]]: ℝ(𝔽(weeks)), [[Days]]: ℝ(𝔽(days))  }.
    return DateDuration { years, months, weeks, days };
}

// 7.5.11 CombineDateAndTimeDuration ( dateDuration, timeDuration ), https://tc39.es/proposal-temporal/#sec-temporal-combinedateandtimeduration
ThrowCompletionOr<InternalDuration> combine_date_and_time_duration(VM& vm, DateDuration date_duration, TimeDuration time_duration)
{
    // 1. Let dateSign be DateDurationSign(dateDuration).
    auto date_sign = date_duration_sign(date_duration);

    // 2. Let timeSign be TimeDurationSign(timeDuration).
    auto time_sign = time_duration_sign(time_duration);

    // 3. If dateSign ≠ 0 and timeSign ≠ 0 and dateSign ≠ timeSign, throw a RangeError exception.
    if (date_sign != 0 && time_sign != 0 && date_sign != time_sign)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDuration);

    // 4. Return Internal Duration Record { [[Date]]: dateDuration, [[Time]]: timeDuration  }.
    return InternalDuration { date_duration, move(time_duration) };
}

// 7.5.12 ToTemporalDuration ( item ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalduration
ThrowCompletionOr<GC::Ref<Duration>> to_temporal_duration(VM& vm, Value item)
{
    // 1. If item is an Object and item has an [[InitializedTemporalDuration]] internal slot, then
    if (item.is_object() && is<Duration>(item.as_object())) {
        auto const& duration = static_cast<Duration const&>(item.as_object());

        // a. Return ! CreateTemporalDuration(item.[[Years]], item.[[Months]], item.[[Weeks]], item.[[Days]], item.[[Hours]], item.[[Minutes]], item.[[Seconds]], item.[[Milliseconds]], item.[[Microseconds]], item.[[Nanoseconds]]).
        return MUST(create_temporal_duration(vm, duration.years(), duration.months(), duration.weeks(), duration.days(), duration.hours(), duration.minutes(), duration.seconds(), duration.milliseconds(), duration.microseconds(), duration.nanoseconds()));
    }

    // 2. If item is not an Object, then
    if (!item.is_object()) {
        // a. If item is not a String, throw a TypeError exception.
        if (!item.is_string())
            return vm.throw_completion<TypeError>(ErrorType::NotAString, item);

        // b. Return ? ParseTemporalDurationString(item).
        return TRY(parse_temporal_duration_string(vm, item.as_string().utf8_string_view()));
    }

    // 3. Let result be a new Partial Duration Record with each field set to 0.
    auto result = PartialDuration::zero();

    // 4. Let partial be ? ToTemporalPartialDurationRecord(item).
    auto partial = TRY(to_temporal_partial_duration_record(vm, item));

    // 5. If partial.[[Years]] is not undefined, set result.[[Years]] to partial.[[Years]].
    if (partial.years.has_value())
        result.years = *partial.years;

    // 6. If partial.[[Months]] is not undefined, set result.[[Months]] to partial.[[Months]].
    if (partial.months.has_value())
        result.months = *partial.months;

    // 7. If partial.[[Weeks]] is not undefined, set result.[[Weeks]] to partial.[[Weeks]].
    if (partial.weeks.has_value())
        result.weeks = *partial.weeks;

    // 8. If partial.[[Days]] is not undefined, set result.[[Days]] to partial.[[Days]].
    if (partial.days.has_value())
        result.days = *partial.days;

    // 9. If partial.[[Hours]] is not undefined, set result.[[Hours]] to partial.[[Hours]].
    if (partial.hours.has_value())
        result.hours = *partial.hours;

    // 10. If partial.[[Minutes]] is not undefined, set result.[[Minutes]] to partial.[[Minutes]].
    if (partial.minutes.has_value())
        result.minutes = *partial.minutes;

    // 11. If partial.[[Seconds]] is not undefined, set result.[[Seconds]] to partial.[[Seconds]].
    if (partial.seconds.has_value())
        result.seconds = *partial.seconds;

    // 12. If partial.[[Milliseconds]] is not undefined, set result.[[Milliseconds]] to partial.[[Milliseconds]].
    if (partial.milliseconds.has_value())
        result.milliseconds = *partial.milliseconds;

    // 13. If partial.[[Microseconds]] is not undefined, set result.[[Microseconds]] to partial.[[Microseconds]].
    if (partial.microseconds.has_value())
        result.microseconds = *partial.microseconds;

    // 14. If partial.[[Nanoseconds]] is not undefined, set result.[[Nanoseconds]] to partial.[[Nanoseconds]].
    if (partial.nanoseconds.has_value())
        result.nanoseconds = *partial.nanoseconds;

    // 15. Return ? CreateTemporalDuration(result.[[Years]], result.[[Months]], result.[[Weeks]], result.[[Days]], result.[[Hours]], result.[[Minutes]], result.[[Seconds]], result.[[Milliseconds]], result.[[Microseconds]], result.[[Nanoseconds]]).
    return TRY(create_temporal_duration(vm, *result.years, *result.months, *result.weeks, *result.days, *result.hours, *result.minutes, *result.seconds, *result.milliseconds, *result.microseconds, *result.nanoseconds));
}

// 7.5.13 DurationSign ( duration ), https://tc39.es/proposal-temporal/#sec-temporal-durationsign
i8 duration_sign(Duration const& duration)
{
    // 1. For each value v of « duration.[[Years]], duration.[[Months]], duration.[[Weeks]], duration.[[Days]], duration.[[Hours]], duration.[[Minutes]], duration.[[Seconds]], duration.[[Milliseconds]], duration.[[Microseconds]], duration.[[Nanoseconds]] », do
    for (auto value : { duration.years(), duration.months(), duration.weeks(), duration.days(), duration.hours(), duration.minutes(), duration.seconds(), duration.milliseconds(), duration.microseconds(), duration.nanoseconds() }) {
        // a. If v < 0, return -1.
        if (value < 0)
            return -1;

        // b. If v > 0, return 1.
        if (value > 0)
            return 1;
    }

    // 2. Return 0.
    return 0;
}

// 7.5.14 DateDurationSign ( dateDuration ), https://tc39.es/proposal-temporal/#sec-temporal-datedurationsign
i8 date_duration_sign(DateDuration const& date_duration)
{
    // 1. For each value v of « dateDuration.[[Years]], dateDuration.[[Months]], dateDuration.[[Weeks]], dateDuration.[[Days]] », do
    for (auto value : { date_duration.years, date_duration.months, date_duration.weeks, date_duration.days }) {
        // a. If v < 0, return -1.
        if (value < 0)
            return -1;

        // b. If v > 0, return 1.
        if (value > 0)
            return 1;
    }

    // 2. Return 0.
    return 0;
}

// 7.5.16 IsValidDuration ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidduration
bool is_valid_duration(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds)
{
    // 1. Let sign be 0.
    auto sign = 0;

    // 2. For each value v of « years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds », do
    for (auto value : { years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds }) {
        // a. If 𝔽(v) is not finite, return false.
        if (!isfinite(value))
            return false;

        // b. If v < 0, then
        if (value < 0) {
            // i. If sign > 0, return false.
            if (sign > 0)
                return false;

            // ii. Set sign to -1.
            sign = -1;
        }
        // c. Else if v > 0, then
        else if (value > 0) {
            // i. If sign < 0, return false.
            if (sign < 0)
                return false;

            // ii. Set sign to 1.
            sign = 1;
        }
    }

    // 3. If abs(years) ≥ 2**32, return false.
    if (AK::fabs(years) > NumericLimits<u32>::max())
        return false;

    // 4. If abs(months) ≥ 2**32, return false.
    if (AK::fabs(months) > NumericLimits<u32>::max())
        return false;

    // 5. If abs(weeks) ≥ 2**32, return false.
    if (AK::fabs(weeks) > NumericLimits<u32>::max())
        return false;

    // 6. Let totalFractionalSeconds be days × 86,400 + hours × 3600 + minutes × 60 + seconds + ℝ(𝔽(milliseconds)) × 10**-3 + ℝ(𝔽(microseconds)) × 10**-6 + ℝ(𝔽(nanoseconds)) × 10**-9.
    // 7. NOTE: The above step cannot be implemented directly using floating-point arithmetic. Multiplying by 10**-3,
    //          10**-6, and 10**-9 respectively may be imprecise when milliseconds, microseconds, or nanoseconds is an
    //          unsafe integer. This multiplication can be implemented in C++ with an implementation of std::remquo()
    //          with sufficient bits in the quotient. String manipulation will also give an exact result, since the
    //          multiplication is by a power of 10.
    auto total_fractional_seconds = TimeDuration { days }.multiplied_by(NANOSECONDS_PER_DAY);
    total_fractional_seconds = total_fractional_seconds.plus(TimeDuration { hours }.multiplied_by(NANOSECONDS_PER_HOUR));
    total_fractional_seconds = total_fractional_seconds.plus(TimeDuration { minutes }.multiplied_by(NANOSECONDS_PER_MINUTE));
    total_fractional_seconds = total_fractional_seconds.plus(TimeDuration { seconds }.multiplied_by(NANOSECONDS_PER_SECOND));
    total_fractional_seconds = total_fractional_seconds.plus(TimeDuration { milliseconds }.multiplied_by(NANOSECONDS_PER_MILLISECOND));
    total_fractional_seconds = total_fractional_seconds.plus(TimeDuration { microseconds }.multiplied_by(NANOSECONDS_PER_MICROSECOND));
    total_fractional_seconds = total_fractional_seconds.plus(TimeDuration { nanoseconds });

    // 8. If abs(totalFractionalSeconds) ≥ 2**53, return false.
    if (total_fractional_seconds.unsigned_value() > MAX_TIME_DURATION.unsigned_value())
        return false;

    // 9. Return true.
    return true;
}

// 7.5.17 DefaultTemporalLargestUnit ( duration ), https://tc39.es/proposal-temporal/#sec-temporal-defaulttemporallargestunit
Unit default_temporal_largest_unit(Duration const& duration)
{
    // 1. If duration.[[Years]] ≠ 0, return YEAR.
    if (duration.years() != 0)
        return Unit::Year;

    // 2. If duration.[[Months]] ≠ 0, return MONTH.
    if (duration.months() != 0)
        return Unit::Month;

    // 3. If duration.[[Weeks]] ≠ 0, return WEEK.
    if (duration.weeks() != 0)
        return Unit::Week;

    // 4. If duration.[[Days]] ≠ 0, return DAY.
    if (duration.days() != 0)
        return Unit::Day;

    // 5. If duration.[[Hours]] ≠ 0, return HOUR.
    if (duration.hours() != 0)
        return Unit::Hour;

    // 6. If duration.[[Minutes]] ≠ 0, return MINUTE.
    if (duration.minutes() != 0)
        return Unit::Minute;

    // 7. If duration.[[Seconds]] ≠ 0, return SECOND.
    if (duration.seconds() != 0)
        return Unit::Second;

    // 8. If duration.[[Milliseconds]] ≠ 0, return MILLISECOND.
    if (duration.milliseconds() != 0)
        return Unit::Millisecond;

    // 9. If duration.[[Microseconds]] ≠ 0, return MICROSECOND.
    if (duration.microseconds() != 0)
        return Unit::Microsecond;

    // 10. Return NANOSECOND.
    return Unit::Nanosecond;
}

// 7.5.18 ToTemporalPartialDurationRecord ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalpartialdurationrecord
ThrowCompletionOr<PartialDuration> to_temporal_partial_duration_record(VM& vm, Value temporal_duration_like)
{
    // 1. If temporalDurationLike is not an Object, then
    if (!temporal_duration_like.is_object()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, temporal_duration_like);
    }

    // 2. Let result be a new partial Duration Record with each field set to undefined.
    PartialDuration result {};

    // 3. NOTE: The following steps read properties and perform independent validation in alphabetical order.

    auto to_integral_if_defined = [&vm, &temporal_duration = temporal_duration_like.as_object()](auto const& property, auto& field) -> ThrowCompletionOr<void> {
        if (auto value = TRY(temporal_duration.get(property)); !value.is_undefined())
            field = TRY(to_integer_if_integral(vm, value, ErrorType::TemporalInvalidDurationPropertyValueNonIntegral, property, value));
        return {};
    };

    // 4. Let days be ? Get(temporalDurationLike, "days").
    // 5. If days is not undefined, set result.[[Days]] to ? ToIntegerIfIntegral(days).
    TRY(to_integral_if_defined(vm.names.days, result.days));

    // 6. Let hours be ? Get(temporalDurationLike, "hours").
    // 7. If hours is not undefined, set result.[[Hours]] to ? ToIntegerIfIntegral(hours).
    TRY(to_integral_if_defined(vm.names.hours, result.hours));

    // 8. Let microseconds be ? Get(temporalDurationLike, "microseconds").
    // 9. If microseconds is not undefined, set result.[[Microseconds]] to ? ToIntegerIfIntegral(microseconds).
    TRY(to_integral_if_defined(vm.names.microseconds, result.microseconds));

    // 10. Let milliseconds be ? Get(temporalDurationLike, "milliseconds").
    // 11. If milliseconds is not undefined, set result.[[Milliseconds]] to ? ToIntegerIfIntegral(milliseconds).
    TRY(to_integral_if_defined(vm.names.milliseconds, result.milliseconds));

    // 12. Let minutes be ? Get(temporalDurationLike, "minutes").
    // 13. If minutes is not undefined, set result.[[Minutes]] to ? ToIntegerIfIntegral(minutes).
    TRY(to_integral_if_defined(vm.names.minutes, result.minutes));

    // 14. Let months be ? Get(temporalDurationLike, "months").
    // 15. If months is not undefined, set result.[[Months]] to ? ToIntegerIfIntegral(months).
    TRY(to_integral_if_defined(vm.names.months, result.months));

    // 16. Let nanoseconds be ? Get(temporalDurationLike, "nanoseconds").
    // 17. If nanoseconds is not undefined, set result.[[Nanoseconds]] to ? ToIntegerIfIntegral(nanoseconds).
    TRY(to_integral_if_defined(vm.names.nanoseconds, result.nanoseconds));

    // 18. Let seconds be ? Get(temporalDurationLike, "seconds").
    // 19. If seconds is not undefined, set result.[[Seconds]] to ? ToIntegerIfIntegral(seconds).
    TRY(to_integral_if_defined(vm.names.seconds, result.seconds));

    // 20. Let weeks be ? Get(temporalDurationLike, "weeks").
    // 21. If weeks is not undefined, set result.[[Weeks]] to ? ToIntegerIfIntegral(weeks).
    TRY(to_integral_if_defined(vm.names.weeks, result.weeks));

    // 22. Let years be ? Get(temporalDurationLike, "years").
    // 23. If years is not undefined, set result.[[Years]] to ? ToIntegerIfIntegral(years).
    TRY(to_integral_if_defined(vm.names.years, result.years));

    // 24. If years is undefined, and months is undefined, and weeks is undefined, and days is undefined, and hours is
    //     undefined, and minutes is undefined, and seconds is undefined, and milliseconds is undefined, and microseconds
    //     is undefined, and nanoseconds is undefined, throw a TypeError exception.
    if (!result.any_field_defined())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidDurationLikeObject);

    // 25. Return result.
    return result;
}

// 7.5.19 CreateTemporalDuration ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporalduration
ThrowCompletionOr<GC::Ref<Duration>> create_temporal_duration(VM& vm, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, GC::Ptr<FunctionObject> new_target)
{
    auto& realm = *vm.current_realm();

    // 1. If IsValidDuration(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds) is false, throw a RangeError exception.
    if (!is_valid_duration(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDuration);

    // 2. If newTarget is not present, set newTarget to %Temporal.Duration%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_duration_constructor();

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.Duration.prototype%", « [[InitializedTemporalDuration]], [[Years]], [[Months]], [[Weeks]], [[Days]], [[Hours]], [[Minutes]], [[Seconds]], [[Milliseconds]], [[Microseconds]], [[Nanoseconds]] »).
    // 4. Set object.[[Years]] to ℝ(𝔽(years)).
    // 5. Set object.[[Months]] to ℝ(𝔽(months)).
    // 6. Set object.[[Weeks]] to ℝ(𝔽(weeks)).
    // 7. Set object.[[Days]] to ℝ(𝔽(days)).
    // 8. Set object.[[Hours]] to ℝ(𝔽(hours)).
    // 9. Set object.[[Minutes]] to ℝ(𝔽(minutes)).
    // 10. Set object.[[Seconds]] to ℝ(𝔽(seconds)).
    // 11. Set object.[[Milliseconds]] to ℝ(𝔽(milliseconds)).
    // 12. Set object.[[Microseconds]] to ℝ(𝔽(microseconds)).
    // 13. Set object.[[Nanoseconds]] to ℝ(𝔽(nanoseconds)).
    auto object = TRY(ordinary_create_from_constructor<Duration>(vm, *new_target, &Intrinsics::temporal_duration_prototype, years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds));

    // 14. Return object.
    return object;
}

// 7.5.20 CreateNegatedTemporalDuration ( duration ), https://tc39.es/proposal-temporal/#sec-temporal-createnegatedtemporalduration
GC::Ref<Duration> create_negated_temporal_duration(VM& vm, Duration const& duration)
{
    // 1. Return ! CreateTemporalDuration(-duration.[[Years]], -duration.[[Months]], -duration.[[Weeks]], -duration.[[Days]], -duration.[[Hours]], -duration.[[Minutes]], -duration.[[Seconds]], -duration.[[Milliseconds]], -duration.[[Microseconds]], -duration.[[Nanoseconds]]).
    return MUST(create_temporal_duration(vm, -duration.years(), -duration.months(), -duration.weeks(), -duration.days(), -duration.hours(), -duration.minutes(), -duration.seconds(), -duration.milliseconds(), -duration.microseconds(), -duration.nanoseconds()));
}

// 7.5.21 TimeDurationFromComponents ( hours, minutes, seconds, milliseconds, microseconds, nanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-timedurationfromcomponents
TimeDuration time_duration_from_components(double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds)
{
    // 1. Set minutes to minutes + hours × 60.
    auto total_minutes = TimeDuration { minutes }.plus(TimeDuration { hours }.multiplied_by(60_bigint));

    // 2. Set seconds to seconds + minutes × 60.
    auto total_seconds = TimeDuration { seconds }.plus(total_minutes.multiplied_by(60_bigint));

    // 3. Set milliseconds to milliseconds + seconds × 1000.
    auto total_milliseconds = TimeDuration { milliseconds }.plus(total_seconds.multiplied_by(1000_bigint));

    // 4. Set microseconds to microseconds + milliseconds × 1000.
    auto total_microseconds = TimeDuration { microseconds }.plus(total_milliseconds.multiplied_by(1000_bigint));

    // 5. Set nanoseconds to nanoseconds + microseconds × 1000.
    auto total_nanoseconds = TimeDuration { nanoseconds }.plus(total_microseconds.multiplied_by(1000_bigint));

    // 6. Assert: abs(nanoseconds) ≤ maxTimeDuration.
    VERIFY(total_nanoseconds.unsigned_value() <= MAX_TIME_DURATION.unsigned_value());

    // 7. Return nanoseconds.
    return total_nanoseconds;
}

// 7.5.22 AddTimeDuration ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-addtimeduration
ThrowCompletionOr<TimeDuration> add_time_duration(VM& vm, TimeDuration const& one, TimeDuration const& two)
{
    // 1. Let result be one + two.
    auto result = one.plus(two);

    // 2. If abs(result) > maxTimeDuration, throw a RangeError exception.
    if (result.unsigned_value() > MAX_TIME_DURATION.unsigned_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDuration);

    // 3. Return result.
    return result;
}

// 7.5.23 Add24HourDaysToTimeDuration ( d, days ), https://tc39.es/proposal-temporal/#sec-temporal-add24hourdaystonormalizedtimeduration
ThrowCompletionOr<TimeDuration> add_24_hour_days_to_time_duration(VM& vm, TimeDuration const& time_duration, double days)
{
    // 1. Let result be d + days × nsPerDay.
    auto result = time_duration.plus(TimeDuration { days }.multiplied_by(NANOSECONDS_PER_DAY));

    // 2. If abs(result) > maxTimeDuration, throw a RangeError exception.
    if (result.unsigned_value() > MAX_TIME_DURATION.unsigned_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDuration);

    // 3. Return result.
    return result;
}

// 7.5.25 CompareTimeDuration ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-comparetimeduration
i8 compare_time_duration(TimeDuration const& one, TimeDuration const& two)
{
    // 1. If one > two, return 1.
    if (one > two)
        return 1;

    // 2. If one < two, return -1.
    if (one < two)
        return -1;

    // 3. Return 0.
    return 0;
}

// 7.5.27 RoundTimeDurationToIncrement ( d, increment, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundtimedurationtoincrement
ThrowCompletionOr<TimeDuration> round_time_duration_to_increment(VM& vm, TimeDuration const& duration, Crypto::UnsignedBigInteger const& increment, RoundingMode rounding_mode)
{
    // 1. Let rounded be RoundNumberToIncrement(d, increment, roundingMode).
    auto rounded = round_number_to_increment(duration, increment, rounding_mode);

    // 2. If abs(rounded) > maxTimeDuration, throw a RangeError exception.
    if (rounded.unsigned_value() > MAX_TIME_DURATION.unsigned_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDuration);

    // 3. Return rounded.
    return rounded;
}

// 7.5.28 TimeDurationSign ( d ), https://tc39.es/proposal-temporal/#sec-temporal-timedurationsign
i8 time_duration_sign(TimeDuration const& time_duration)
{
    // 1. If d < 0, return -1.
    if (time_duration.is_negative())
        return -1;

    // 2. If d > 0, return 1.
    if (time_duration.is_positive())
        return 1;

    // 3. Return 0.
    return 0;
}

// 7.5.30 RoundTimeDuration ( timeDuration, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundtimeduration
ThrowCompletionOr<TimeDuration> round_time_duration(VM& vm, TimeDuration const& time_duration, Crypto::UnsignedBigInteger const& increment, Unit unit, RoundingMode rounding_mode)
{
    // 1. Let divisor be the value in the "Length in Nanoseconds" column of the row of Table 21 whose "Value" column contains unit.
    auto const& divisor = temporal_unit_length_in_nanoseconds(unit);

    // 2. Return ? RoundTimeDurationToIncrement(timeDuration, divisor × increment, roundingMode).
    return TRY(round_time_duration_to_increment(vm, time_duration, divisor.multiplied_by(increment), rounding_mode));
}

// 7.5.39 TemporalDurationToString ( duration, precision ), https://tc39.es/proposal-temporal/#sec-temporal-temporaldurationtostring
String temporal_duration_to_string(Duration const& duration, Precision precision)
{
    // 1. Let sign be DurationSign(duration).
    auto sign = duration_sign(duration);

    // 2. Let datePart be the empty String.
    StringBuilder date_part;

    // 3. If duration.[[Years]] ≠ 0, then
    if (duration.years() != 0) {
        // a. Set datePart to the string concatenation of abs(duration.[[Years]]) formatted as a decimal number and the
        //    code unit 0x0059 (LATIN CAPITAL LETTER Y).
        date_part.appendff("{}Y", AK::fabs(duration.years()));
    }
    // 4. If duration.[[Months]] ≠ 0, then
    if (duration.months() != 0) {
        // a. Set datePart to the string concatenation of datePart, abs(duration.[[Months]]) formatted as a decimal number,
        //    and the code unit 0x004D (LATIN CAPITAL LETTER M).
        date_part.appendff("{}M", AK::fabs(duration.months()));
    }
    // 5. If duration.[[Weeks]] ≠ 0, then
    if (duration.weeks() != 0) {
        // a. Set datePart to the string concatenation of datePart, abs(duration.[[Weeks]]) formatted as a decimal number,
        //    and the code unit 0x0057 (LATIN CAPITAL LETTER W).
        date_part.appendff("{}W", AK::fabs(duration.weeks()));
    }
    // 6. If duration.[[Days]] ≠ 0, then
    if (duration.days() != 0) {
        // a. Set datePart to the string concatenation of datePart, abs(duration.[[Days]]) formatted as a decimal number,
        //    and the code unit 0x0044 (LATIN CAPITAL LETTER D).
        date_part.appendff("{}D", AK::fabs(duration.days()));
    }

    // 7. Let timePart be the empty String.
    StringBuilder time_part;

    // 8. If duration.[[Hours]] ≠ 0, then
    if (duration.hours() != 0) {
        // a. Set timePart to the string concatenation of abs(duration.[[Hours]]) formatted as a decimal number and the
        //    code unit 0x0048 (LATIN CAPITAL LETTER H).
        time_part.appendff("{}H", AK::fabs(duration.hours()));
    }
    // 9. If duration.[[Minutes]] ≠ 0, then
    if (duration.minutes() != 0) {
        // a. Set timePart to the string concatenation of timePart, abs(duration.[[Minutes]]) formatted as a decimal number,
        //    and the code unit 0x004D (LATIN CAPITAL LETTER M).
        time_part.appendff("{}M", AK::fabs(duration.minutes()));
    }

    // 10. Let zeroMinutesAndHigher be false.
    auto zero_minutes_and_higher = false;

    // 11. If DefaultTemporalLargestUnit(duration) is SECOND, MILLISECOND, MICROSECOND, or NANOSECOND, set zeroMinutesAndHigher to true.
    if (auto unit = default_temporal_largest_unit(duration); unit == Unit::Second || unit == Unit::Millisecond || unit == Unit::Microsecond || unit == Unit::Nanosecond)
        zero_minutes_and_higher = true;

    // 12. Let secondsDuration be TimeDurationFromComponents(0, 0, duration.[[Seconds]], duration.[[Milliseconds]], duration.[[Microseconds]], duration.[[Nanoseconds]]).
    auto seconds_duration = time_duration_from_components(0, 0, duration.seconds(), duration.milliseconds(), duration.microseconds(), duration.nanoseconds());

    // 13. If secondsDuration ≠ 0, or zeroMinutesAndHigher is true, or precision is not auto, then
    if (!seconds_duration.is_zero() || zero_minutes_and_higher || !precision.has<Auto>()) {
        auto division_result = seconds_duration.divided_by(NANOSECONDS_PER_SECOND);

        // a. Let secondsPart be abs(truncate(secondsDuration / 10**9)) formatted as a decimal number.
        auto seconds_part = MUST(division_result.quotient.unsigned_value().to_base(10));

        // b. Let subSecondsPart be FormatFractionalSeconds(abs(remainder(secondsDuration, 10**9)), precision).
        auto sub_seconds_part = format_fractional_seconds(division_result.remainder.unsigned_value().to_u64(), precision);

        // c. Set timePart to the string concatenation of timePart, secondsPart, subSecondsPart, and the code unit
        //    0x0053 (LATIN CAPITAL LETTER S).
        time_part.appendff("{}{}S", seconds_part, sub_seconds_part);
    }

    // 14. Let signPart be the code unit 0x002D (HYPHEN-MINUS) if sign < 0, and otherwise the empty String.
    auto sign_part = sign < 0 ? "-"sv : ""sv;

    // 15. Let result be the string concatenation of signPart, the code unit 0x0050 (LATIN CAPITAL LETTER P) and datePart.
    StringBuilder result;
    result.appendff("{}P{}", sign_part, date_part.string_view());

    // 16. If timePart is not the empty String, then
    if (!time_part.is_empty()) {
        // a. Set result to the string concatenation of result, the code unit 0x0054 (LATIN CAPITAL LETTER T), and timePart.
        result.appendff("T{}", time_part.string_view());
    }

    // 17. Return result.
    return MUST(result.to_string());
}

// 7.5.40 AddDurations ( operation, duration, other ), https://tc39.es/proposal-temporal/#sec-temporal-adddurations
ThrowCompletionOr<GC::Ref<Duration>> add_durations(VM& vm, ArithmeticOperation operation, Duration const& duration, Value other_value)
{
    // 1. Set other to ? ToTemporalDuration(other).
    auto other = TRY(to_temporal_duration(vm, other_value));

    // 2. If operation is subtract, set other to CreateNegatedTemporalDuration(other).
    if (operation == ArithmeticOperation::Subtract)
        other = create_negated_temporal_duration(vm, other);

    // 3. Let largestUnit1 be DefaultTemporalLargestUnit(duration).
    auto largest_unit1 = default_temporal_largest_unit(duration);

    // 4. Let largestUnit2 be DefaultTemporalLargestUnit(other).
    auto largest_unit2 = default_temporal_largest_unit(other);

    // 5. Let largestUnit be LargerOfTwoTemporalUnits(largestUnit1, largestUnit2).
    auto largest_unit = larger_of_two_temporal_units(largest_unit1, largest_unit2);

    // 6. If IsCalendarUnit(largestUnit) is true, throw a RangeError exception.
    if (is_calendar_unit(largest_unit))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidLargestUnit, "a calendar unit"sv);

    // 7. Let d1 be ToInternalDurationRecordWith24HourDays(duration).
    auto duration1 = to_internal_duration_record_with_24_hour_days(vm, duration);

    // 8. Let d2 be ToInternalDurationRecordWith24HourDays(other).
    auto duration2 = to_internal_duration_record_with_24_hour_days(vm, other);

    // 9. Let timeResult be ? AddTimeDuration(d1.[[Time]], d2.[[Time]]).
    auto time_result = TRY(add_time_duration(vm, duration1.time, duration2.time));

    // 10. Let result be ! CombineDateAndTimeDuration(ZeroDateDuration(), timeResult).
    auto result = MUST(combine_date_and_time_duration(vm, zero_date_duration(vm), move(time_result)));

    // 11. Return ? TemporalDurationFromInternal(result, largestUnit).
    return TRY(temporal_duration_from_internal(vm, result, largest_unit));
}

}
