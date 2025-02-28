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
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/DurationConstructor.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
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

    // 3. Return CombineDateAndTimeDuration(dateDuration, timeDuration).
    return combine_date_and_time_duration(date_duration, move(time_duration));
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

    // 4. Return CombineDateAndTimeDuration(dateDuration, timeDuration).
    return combine_date_and_time_duration(date_duration, move(time_duration));
}

// 7.5.7 ToDateDurationRecordWithoutTime ( duration ), https://tc39.es/proposal-temporal/#sec-temporal-todatedurationrecordwithouttime
DateDuration to_date_duration_record_without_time(VM& vm, Duration const& duration)
{
    // 1. Let internalDuration be ToInternalDurationRecordWith24HourDays(duration).
    auto internal_duration = to_internal_duration_record_with_24_hour_days(vm, duration);

    // 2. Let days be truncate(internalDuration.[[Time]] / nsPerDay).
    auto days = internal_duration.time.divided_by(NANOSECONDS_PER_DAY).quotient;

    // 3. Return ! CreateDateDurationRecord(internalDuration.[[Date]].[[Years]], internalDuration.[[Date]].[[Months]], internalDuration.[[Date]].[[Weeks]], days).
    return MUST(create_date_duration_record(vm, duration.years(), duration.months(), duration.weeks(), days.to_double()));
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

// 7.5.10 AdjustDateDurationRecord ( dateDuration, days [ , weeks [ , months ] ] ), https://tc39.es/proposal-temporal/#sec-temporal-adjustdatedurationrecord
ThrowCompletionOr<DateDuration> adjust_date_duration_record(VM& vm, DateDuration const& date_duration, double days, Optional<double> weeks, Optional<double> months)
{
    // 1. If weeks is not present, set weeks to dateDuration.[[Weeks]].
    if (!weeks.has_value())
        weeks = date_duration.weeks;

    // 2. If months is not present, set months to dateDuration.[[Months]].
    if (!months.has_value())
        months = date_duration.months;

    // 3. Return ? CreateDateDurationRecord(dateDuration.[[Years]], months, weeks, days).
    return TRY(create_date_duration_record(vm, date_duration.years, *months, *weeks, days));
}

// 7.5.11 CombineDateAndTimeDuration ( dateDuration, timeDuration ), https://tc39.es/proposal-temporal/#sec-temporal-combinedateandtimeduration
InternalDuration combine_date_and_time_duration(DateDuration date_duration, TimeDuration time_duration)
{
    // 1. Let dateSign be DateDurationSign(dateDuration).
    auto date_sign = date_duration_sign(date_duration);

    // 2. Let timeSign be TimeDurationSign(timeDuration).
    auto time_sign = time_duration_sign(time_duration);

    // 3. Assert: If dateSign ≠ 0 and timeSign ≠ 0, dateSign = timeSign.
    if (date_sign != 0 && time_sign != 0)
        VERIFY(date_sign == time_sign);

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

// 7.5.15 InternalDurationSign ( internalDuration ), https://tc39.es/proposal-temporal/#sec-temporal-internaldurationsign
i8 internal_duration_sign(InternalDuration const& internal_duration)
{
    // 1. Let dateSign be DateDurationSign(internalDuration.[[Date]]).
    auto date_sign = date_duration_sign(internal_duration.date);

    // 2. If dateSign ≠ 0, return dateSign.
    if (date_sign != 0)
        return date_sign;

    // 3. Return TimeDurationSign(internalDuration.[[Time]]).
    return time_duration_sign(internal_duration.time);
}

// 7.5.16 IsValidDuration ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds ), https://tc39.es/proposal-temporal/#sec-isvalidduration
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

    // 6. Let normalizedSeconds be days × 86,400 + hours × 3600 + minutes × 60 + seconds + ℝ(𝔽(milliseconds)) × 10**-3 + ℝ(𝔽(microseconds)) × 10**-6 + ℝ(𝔽(nanoseconds)) × 10**-9.
    // 7. NOTE: The above step cannot be implemented directly using floating-point arithmetic. Multiplying by 10**-3,
    //          10**-6, and 10**-9 respectively may be imprecise when milliseconds, microseconds, or nanoseconds is an
    //          unsafe integer. This multiplication can be implemented in C++ with an implementation of std::remquo()
    //          with sufficient bits in the quotient. String manipulation will also give an exact result, since the
    //          multiplication is by a power of 10.
    auto normalized_seconds = TimeDuration { days }.multiplied_by(NANOSECONDS_PER_DAY);
    normalized_seconds = normalized_seconds.plus(TimeDuration { hours }.multiplied_by(NANOSECONDS_PER_HOUR));
    normalized_seconds = normalized_seconds.plus(TimeDuration { minutes }.multiplied_by(NANOSECONDS_PER_MINUTE));
    normalized_seconds = normalized_seconds.plus(TimeDuration { seconds }.multiplied_by(NANOSECONDS_PER_SECOND));
    normalized_seconds = normalized_seconds.plus(TimeDuration { milliseconds }.multiplied_by(NANOSECONDS_PER_MILLISECOND));
    normalized_seconds = normalized_seconds.plus(TimeDuration { microseconds }.multiplied_by(NANOSECONDS_PER_MICROSECOND));
    normalized_seconds = normalized_seconds.plus(TimeDuration { nanoseconds });

    // 8. If abs(normalizedSeconds) ≥ 2**53, return false.
    if (normalized_seconds.unsigned_value() > MAX_TIME_DURATION.unsigned_value())
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

// 7.5.24 AddTimeDurationToEpochNanoseconds ( d, epochNs ), https://tc39.es/proposal-temporal/#sec-temporal-addtimedurationtoepochnanoseconds
Crypto::SignedBigInteger add_time_duration_to_epoch_nanoseconds(TimeDuration const& duration, Crypto::SignedBigInteger const& epoch_nanoseconds)
{
    // 1. Return epochNs + ℤ(d).
    return epoch_nanoseconds.plus(duration);
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

// 7.5.26 TimeDurationFromEpochNanosecondsDifference ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-timedurationfromepochnanosecondsdifference
TimeDuration time_duration_from_epoch_nanoseconds_difference(Crypto::SignedBigInteger const& one, Crypto::SignedBigInteger const& two)
{
    // 1. Let result be ℝ(one) - ℝ(two).
    auto result = one.minus(two);

    // 2. Assert: abs(result) ≤ maxTimeDuration.
    VERIFY(result.unsigned_value() <= MAX_TIME_DURATION.unsigned_value());

    // 3. Return result.
    return result;
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

// 7.5.29 DateDurationDays ( dateDuration, plainRelativeTo ), https://tc39.es/proposal-temporal/#sec-temporal-datedurationdays
ThrowCompletionOr<double> date_duration_days(VM& vm, DateDuration const& date_duration, PlainDate const& plain_relative_to)
{
    // 1. Let yearsMonthsWeeksDuration be ! AdjustDateDurationRecord(dateDuration, 0).
    auto years_months_weeks_duration = MUST(adjust_date_duration_record(vm, date_duration, 0));

    // 2. If DateDurationSign(yearsMonthsWeeksDuration) = 0, return dateDuration.[[Days]].
    if (date_duration_sign(years_months_weeks_duration) == 0)
        return date_duration.days;

    // 3. Let later be ? CalendarDateAdd(plainRelativeTo.[[Calendar]], plainRelativeTo.[[ISODate]], yearsMonthsWeeksDuration, CONSTRAIN).
    auto later = TRY(calendar_date_add(vm, plain_relative_to.calendar(), plain_relative_to.iso_date(), years_months_weeks_duration, Overflow::Constrain));

    // 4. Let epochDays1 be ISODateToEpochDays(plainRelativeTo.[[ISODate]].[[Year]], plainRelativeTo.[[ISODate]].[[Month]] - 1, plainRelativeTo.[[ISODate]].[[Day]]).
    auto epoch_days1 = iso_date_to_epoch_days(plain_relative_to.iso_date().year, plain_relative_to.iso_date().month - 1, plain_relative_to.iso_date().day);

    // 5. Let epochDays2 be ISODateToEpochDays(later.[[Year]], later.[[Month]] - 1, later.[[Day]]).
    auto epoch_days2 = iso_date_to_epoch_days(later.year, later.month - 1, later.day);

    // 6. Let yearsMonthsWeeksInDays be epochDays2 - epochDays1.
    auto years_months_weeks_in_days = epoch_days2 - epoch_days1;

    // 7. Return dateDuration.[[Days]] + yearsMonthsWeeksInDays.
    return date_duration.days + years_months_weeks_in_days;
}

// 7.5.30 RoundTimeDuration ( timeDuration, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundtimeduration
ThrowCompletionOr<TimeDuration> round_time_duration(VM& vm, TimeDuration const& time_duration, Crypto::UnsignedBigInteger const& increment, Unit unit, RoundingMode rounding_mode)
{
    // 1. Let divisor be the value in the "Length in Nanoseconds" column of the row of Table 21 whose "Value" column contains unit.
    auto const& divisor = temporal_unit_length_in_nanoseconds(unit);

    // 2. Return ? RoundTimeDurationToIncrement(timeDuration, divisor × increment, roundingMode).
    return TRY(round_time_duration_to_increment(vm, time_duration, divisor.multiplied_by(increment), rounding_mode));
}

// 7.5.31 TotalTimeDuration ( timeDuration, unit ), https://tc39.es/proposal-temporal/#sec-temporal-totaltimeduration
Crypto::BigFraction total_time_duration(TimeDuration const& time_duration, Unit unit)
{
    // 1. Let divisor be the value in the "Length in Nanoseconds" column of the row of Table 21 whose "Value" column contains unit.
    auto const& divisor = temporal_unit_length_in_nanoseconds(unit);

    // 2. NOTE: The following step cannot be implemented directly using floating-point arithmetic when 𝔽(timeDuration) is
    //    not a safe integer. The division can be implemented in C++ with the __float128 type if the compiler supports it,
    //    or with software emulation such as in the SoftFP library.

    // 3. Return timeDuration / divisor.
    return Crypto::BigFraction { time_duration } / Crypto::BigFraction { Crypto::SignedBigInteger { divisor } };
}

// 7.5.33 NudgeToCalendarUnit ( sign, duration, destEpochNs, isoDateTime, timeZone, calendar, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-nudgetocalendarunit
ThrowCompletionOr<CalendarNudgeResult> nudge_to_calendar_unit(VM& vm, i8 sign, InternalDuration const& duration, Crypto::SignedBigInteger const& dest_epoch_ns, ISODateTime const& iso_date_time, Optional<StringView> time_zone, StringView calendar, u64 increment, Unit unit, RoundingMode rounding_mode)
{
    DateDuration start_duration;
    DateDuration end_duration;

    double r1 = 0;
    double r2 = 0;

    // 1. If unit is YEAR, then
    if (unit == Unit::Year) {
        // a. Let years be RoundNumberToIncrement(duration.[[Date]].[[Years]], increment, TRUNC).
        auto years = round_number_to_increment(duration.date.years, increment, RoundingMode::Trunc);

        // b. Let r1 be years.
        r1 = years;

        // c. Let r2 be years + increment × sign.
        r2 = years + static_cast<double>(increment) * sign;

        // d. Let startDuration be ? CreateDateDurationRecord(r1, 0, 0, 0).
        start_duration = TRY(create_date_duration_record(vm, r1, 0, 0, 0));

        // e. Let endDuration be ? CreateDateDurationRecord(r2, 0, 0, 0).
        end_duration = TRY(create_date_duration_record(vm, r2, 0, 0, 0));
    }
    // 2. Else if unit is MONTH, then
    else if (unit == Unit::Month) {
        // a. Let months be RoundNumberToIncrement(duration.[[Date]].[[Months]], increment, TRUNC).
        auto months = round_number_to_increment(duration.date.months, increment, RoundingMode::Trunc);

        // b. Let r1 be months.
        r1 = months;

        // c. Let r2 be months + increment × sign.
        r2 = months + static_cast<double>(increment) * sign;

        // d. Let startDuration be ? AdjustDateDurationRecord(duration.[[Date]], 0, 0, r1).
        start_duration = TRY(adjust_date_duration_record(vm, duration.date, 0, 0, r1));

        // e. Let endDuration be ? AdjustDateDurationRecord(duration.[[Date]], 0, 0, r2).
        end_duration = TRY(adjust_date_duration_record(vm, duration.date, 0, 0, r2));
    }
    // 3. Else if unit is WEEK, then
    else if (unit == Unit::Week) {
        // a. Let yearsMonths be ! AdjustDateDurationRecord(duration.[[Date]], 0, 0).
        auto years_months = MUST(adjust_date_duration_record(vm, duration.date, 0, 0));

        // b. Let weeksStart be ? CalendarDateAdd(calendar, isoDateTime.[[ISODate]], yearsMonths, CONSTRAIN).
        auto weeks_start = TRY(calendar_date_add(vm, calendar, iso_date_time.iso_date, years_months, Overflow::Constrain));

        // c. Let weeksEnd be BalanceISODate(weeksStart.[[Year]], weeksStart.[[Month]], weeksStart.[[Day]] + duration.[[Date]].[[Days]]).
        auto weeks_end = balance_iso_date(weeks_start.year, weeks_start.month, static_cast<double>(weeks_start.day) + duration.date.days);

        // d. Let untilResult be CalendarDateUntil(calendar, weeksStart, weeksEnd, WEEK).
        auto until_result = calendar_date_until(vm, calendar, weeks_start, weeks_end, Unit::Week);

        // e. Let weeks be RoundNumberToIncrement(duration.[[Date]].[[Weeks]] + untilResult.[[Weeks]], increment, TRUNC).
        auto weeks = round_number_to_increment(duration.date.weeks + until_result.weeks, increment, RoundingMode::Trunc);

        // f. Let r1 be weeks.
        r1 = weeks;

        // g. Let r2 be weeks + increment × sign.
        r2 = weeks + static_cast<double>(increment) * sign;

        // h. Let startDuration be ? AdjustDateDurationRecord(duration.[[Date]], 0, r1).
        start_duration = TRY(adjust_date_duration_record(vm, duration.date, 0, r1));

        // i. Let endDuration be ? AdjustDateDurationRecord(duration.[[Date]], 0, r2).
        end_duration = TRY(adjust_date_duration_record(vm, duration.date, 0, r2));
    }
    // 4. Else,
    else {
        // a. Assert: unit is DAY.
        VERIFY(unit == Unit::Day);

        // b. Let days be RoundNumberToIncrement(duration.[[Date]].[[Days]], increment, TRUNC).
        auto days = round_number_to_increment(duration.date.days, increment, RoundingMode::Trunc);

        // c. Let r1 be days.
        r1 = days;

        // d. Let r2 be days + increment × sign.
        r2 = days + static_cast<double>(increment) * sign;

        // e. Let startDuration be ? AdjustDateDurationRecord(duration.[[Date]], r1).
        start_duration = TRY(adjust_date_duration_record(vm, duration.date, r1));

        // f. Let endDuration be ? AdjustDateDurationRecord(duration.[[Date]], r2).
        end_duration = TRY(adjust_date_duration_record(vm, duration.date, r2));
    }

    // 5. Assert: If sign is 1, r1 ≥ 0 and r1 < r2.
    if (sign == 1)
        VERIFY(r1 >= 0 && r1 < r2);
    // 6. Assert: If sign is -1, r1 ≤ 0 and r1 > r2.
    else if (sign == -1)
        VERIFY(r1 <= 0 && r1 > r2);

    // 7. Let start be ? CalendarDateAdd(calendar, isoDateTime.[[ISODate]], startDuration, CONSTRAIN).
    auto start = TRY(calendar_date_add(vm, calendar, iso_date_time.iso_date, start_duration, Overflow::Constrain));

    // 8. Let end be ? CalendarDateAdd(calendar, isoDateTime.[[ISODate]], endDuration, CONSTRAIN).
    auto end = TRY(calendar_date_add(vm, calendar, iso_date_time.iso_date, end_duration, Overflow::Constrain));

    // 9. Let startDateTime be CombineISODateAndTimeRecord(start, isoDateTime.[[Time]]).
    auto start_date_time = combine_iso_date_and_time_record(start, iso_date_time.time);

    // 10. Let endDateTime be CombineISODateAndTimeRecord(end, isoDateTime.[[Time]]).
    auto end_date_time = combine_iso_date_and_time_record(end, iso_date_time.time);

    Crypto::SignedBigInteger start_epoch_ns;
    Crypto::SignedBigInteger end_epoch_ns;

    // 11. If timeZone is UNSET, then
    if (!time_zone.has_value()) {
        // a. Let startEpochNs be GetUTCEpochNanoseconds(startDateTime).
        start_epoch_ns = get_utc_epoch_nanoseconds(start_date_time);

        // b. Let endEpochNs be GetUTCEpochNanoseconds(endDateTime).
        end_epoch_ns = get_utc_epoch_nanoseconds(end_date_time);
    }
    // 12. Else,
    else {
        // a. Let startEpochNs be ? GetEpochNanosecondsFor(timeZone, startDateTime, COMPATIBLE).
        start_epoch_ns = TRY(get_epoch_nanoseconds_for(vm, *time_zone, start_date_time, Disambiguation::Compatible));

        // b. Let endEpochNs be ? GetEpochNanosecondsFor(timeZone, endDateTime, COMPATIBLE).
        end_epoch_ns = TRY(get_epoch_nanoseconds_for(vm, *time_zone, end_date_time, Disambiguation::Compatible));
    }

    // 13. If sign is 1, then
    if (sign == 1) {
        // a. Assert: startEpochNs ≤ destEpochNs ≤ endEpochNs.
        VERIFY(start_epoch_ns <= dest_epoch_ns);
        VERIFY(dest_epoch_ns <= end_epoch_ns);
    }
    // 14. Else,
    else {
        // a. Assert: endEpochNs ≤ destEpochNs ≤ startEpochNs.
        VERIFY(end_epoch_ns <= dest_epoch_ns);
        VERIFY(dest_epoch_ns <= start_epoch_ns);
    }

    // 15. Assert: startEpochNs ≠ endEpochNs.
    VERIFY(start_epoch_ns != end_epoch_ns);

    // 16. Let progress be (destEpochNs - startEpochNs) / (endEpochNs - startEpochNs).
    auto progress_numerator = dest_epoch_ns.minus(start_epoch_ns);
    auto progress_denominator = end_epoch_ns.minus(start_epoch_ns);
    auto progress_equals_one = progress_numerator == progress_denominator;

    // 17. Let total be r1 + progress × increment × sign.
    auto total_numerator = progress_numerator.multiplied_by(Crypto::UnsignedBigInteger { increment });

    if (sign == -1)
        total_numerator.negate();
    if (progress_denominator.is_negative())
        total_numerator.negate();

    auto total_mv = Crypto::BigFraction { Crypto::SignedBigInteger { r1 } } + Crypto::BigFraction { move(total_numerator), progress_denominator.unsigned_value() };
    auto total = total_mv.to_double();

    // 18. NOTE: The above two steps cannot be implemented directly using floating-point arithmetic. This division can be
    //     implemented as if expressing the denominator and numerator of total as two time durations, and performing one
    //     division operation with a floating-point result.

    // 19. Assert: 0 ≤ progress ≤ 1.

    // 20. If sign < 0, let isNegative be NEGATIVE; else let isNegative be POSITIVE.
    auto is_negative = sign < 0 ? Sign::Negative : Sign::Positive;

    // 21. Let unsignedRoundingMode be GetUnsignedRoundingMode(roundingMode, isNegative).
    auto unsigned_rounding_mode = get_unsigned_rounding_mode(rounding_mode, is_negative);

    double rounded_unit = 0;

    // 22. If progress = 1, then
    if (progress_equals_one) {
        // a. Let roundedUnit be abs(r2).
        rounded_unit = fabs(r2);
    }
    // 23. Else,
    else {
        // a. Assert: abs(r1) ≤ abs(total) < abs(r2).
        VERIFY(fabs(r1) <= fabs(total));
        VERIFY(fabs(total) <= fabs(r2));

        // b. Let roundedUnit be ApplyUnsignedRoundingMode(abs(total), abs(r1), abs(r2), unsignedRoundingMode).
        rounded_unit = apply_unsigned_rounding_mode(fabs(total), fabs(r1), fabs(r2), unsigned_rounding_mode);
    }

    auto did_expand_calendar_unit = false;
    DateDuration result_duration;
    Crypto::SignedBigInteger nudged_epoch_ns;

    // 24. If roundedUnit is abs(r2), then
    if (rounded_unit == fabs(r2)) {
        // a. Let didExpandCalendarUnit be true.
        did_expand_calendar_unit = true;

        // b. Let resultDuration be endDuration.
        result_duration = end_duration;

        // c. Let nudgedEpochNs be endEpochNs.
        nudged_epoch_ns = move(end_epoch_ns);
    }
    // 25. Else,
    else {
        // a. Let didExpandCalendarUnit be false.
        did_expand_calendar_unit = false;

        // b. Let resultDuration be startDuration.
        result_duration = start_duration;

        // c. Let nudgedEpochNs be startEpochNs.
        nudged_epoch_ns = move(start_epoch_ns);
    }

    // 26. Set resultDuration to CombineDateAndTimeDuration(resultDuration, 0).
    auto result_date_and_time_duration = combine_date_and_time_duration(result_duration, TimeDuration { 0 });

    // 27. Let nudgeResult be Duration Nudge Result Record { [[Duration]]: resultDuration, [[NudgedEpochNs]]: nudgedEpochNs, [[DidExpandCalendarUnit]]: didExpandCalendarUnit }.
    DurationNudgeResult nudge_result { .duration = move(result_date_and_time_duration), .nudged_epoch_ns = move(nudged_epoch_ns), .did_expand_calendar_unit = did_expand_calendar_unit };

    // 28. Return the Record { [[NudgeResult]]: nudgeResult, [[Total]]: total }.
    return CalendarNudgeResult { .nudge_result = move(nudge_result), .total = move(total_mv) };
}

// 7.5.34 NudgeToZonedTime ( sign, duration, isoDateTime, timeZone, calendar, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-nudgetozonedtime
ThrowCompletionOr<DurationNudgeResult> nudge_to_zoned_time(VM& vm, i8 sign, InternalDuration const& duration, ISODateTime const& iso_date_time, StringView time_zone, StringView calendar, u64 increment, Unit unit, RoundingMode rounding_mode)
{
    // 1. Let start be ? CalendarDateAdd(calendar, isoDateTime.[[ISODate]], duration.[[Date]], CONSTRAIN).
    auto start = TRY(calendar_date_add(vm, calendar, iso_date_time.iso_date, duration.date, Overflow::Constrain));

    // 2. Let startDateTime be CombineISODateAndTimeRecord(start, isoDateTime.[[Time]]).
    auto start_date_time = combine_iso_date_and_time_record(start, iso_date_time.time);

    // 3. Let endDate be BalanceISODate(start.[[Year]], start.[[Month]], start.[[Day]] + sign).
    auto end_date = balance_iso_date(start.year, start.month, static_cast<double>(start.day) + sign);

    // 4. Let endDateTime be CombineISODateAndTimeRecord(endDate, isoDateTime.[[Time]]).
    auto end_date_time = combine_iso_date_and_time_record(end_date, iso_date_time.time);

    // 5. Let startEpochNs be ? GetEpochNanosecondsFor(timeZone, startDateTime, COMPATIBLE).
    auto start_epoch_ns = TRY(get_epoch_nanoseconds_for(vm, time_zone, start_date_time, Disambiguation::Compatible));

    // 6. Let endEpochNs be ? GetEpochNanosecondsFor(timeZone, endDateTime, COMPATIBLE).
    auto end_epoch_ns = TRY(get_epoch_nanoseconds_for(vm, time_zone, end_date_time, Disambiguation::Compatible));

    // 7. Let daySpan be TimeDurationFromEpochNanosecondsDifference(endEpochNs, startEpochNs).
    auto day_span = time_duration_from_epoch_nanoseconds_difference(end_epoch_ns, start_epoch_ns);

    // 8. Assert: TimeDurationSign(daySpan) = sign.
    VERIFY(time_duration_sign(day_span) == sign);

    // 9. Let unitLength be the value in the "Length in Nanoseconds" column of the row of Table 21 whose "Value" column contains unit.
    auto const& unit_length = temporal_unit_length_in_nanoseconds(unit);

    // 10. Let roundedTimeDuration be ? RoundTimeDurationToIncrement(duration.[[Time]], increment × unitLength, roundingMode).
    auto unit_length_multiplied_by_increment = unit_length.multiplied_by(Crypto::UnsignedBigInteger { increment });
    auto rounded_time_duration = TRY(round_time_duration_to_increment(vm, duration.time, unit_length_multiplied_by_increment, rounding_mode));

    // 11. Let beyondDaySpan be ! AddTimeDuration(roundedTimeDuration, -daySpan).
    day_span.negate();
    auto beyond_day_span = MUST(add_time_duration(vm, rounded_time_duration, day_span));

    auto did_round_beyond_day = false;
    Crypto::SignedBigInteger nudged_epoch_ns;
    i8 day_delta = 0;

    // 12. If TimeDurationSign(beyondDaySpan) ≠ -sign, then
    if (time_duration_sign(beyond_day_span) != -sign) {
        // a. Let didRoundBeyondDay be true.
        did_round_beyond_day = true;

        // b. Let dayDelta be sign.
        day_delta = sign;

        // c. Set roundedTimeDuration to ? RoundTimeDurationToIncrement(beyondDaySpan, increment × unitLength, roundingMode).
        rounded_time_duration = TRY(round_time_duration_to_increment(vm, beyond_day_span, unit_length_multiplied_by_increment, rounding_mode));

        // d. Let nudgedEpochNs be AddTimeDurationToEpochNanoseconds(roundedTimeDuration, endEpochNs).
        nudged_epoch_ns = add_time_duration_to_epoch_nanoseconds(rounded_time_duration, end_epoch_ns);
    }
    // 13. Else,
    else {
        // a. Let didRoundBeyondDay be false.
        did_round_beyond_day = false;

        // b. Let dayDelta be 0.
        day_delta = 0;

        // c. Let nudgedEpochNs be AddTimeDurationToEpochNanoseconds(roundedTimeDuration, startEpochNs).
        nudged_epoch_ns = add_time_duration_to_epoch_nanoseconds(rounded_time_duration, start_epoch_ns);
    }

    // 14. Let dateDuration be ! AdjustDateDurationRecord(duration.[[Date]], duration.[[Date]].[[Days]] + dayDelta).
    auto date_duration = MUST(adjust_date_duration_record(vm, duration.date, duration.date.days + day_delta));

    // 15. Let resultDuration be CombineDateAndTimeDuration(dateDuration, roundedTimeDuration).
    auto result_duration = combine_date_and_time_duration(date_duration, move(rounded_time_duration));

    // 16. Return Duration Nudge Result Record { [[Duration]]: resultDuration, [[NudgedEpochNs]]: nudgedEpochNs, [[DidExpandCalendarUnit]]: didRoundBeyondDay }.
    return DurationNudgeResult { .duration = move(result_duration), .nudged_epoch_ns = move(nudged_epoch_ns), .did_expand_calendar_unit = did_round_beyond_day };
}

// 7.5.35 NudgeToDayOrTime ( duration, destEpochNs, largestUnit, increment, smallestUnit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-nudgetodayortime
ThrowCompletionOr<DurationNudgeResult> nudge_to_day_or_time(VM& vm, InternalDuration const& duration, Crypto::SignedBigInteger const& dest_epoch_ns, Unit largest_unit, u64 increment, Unit smallest_unit, RoundingMode rounding_mode)
{
    // 1. Let timeDuration be ! Add24HourDaysToTimeDuration(duration.[[Time]], duration.[[Date]].[[Days]]).
    auto time_duration = MUST(add_24_hour_days_to_time_duration(vm, duration.time, duration.date.days));

    // 2. Let unitLength be the value in the "Length in Nanoseconds" column of the row of Table 21 whose "Value" column contains smallestUnit.
    auto const& unit_length = temporal_unit_length_in_nanoseconds(smallest_unit);

    // 3. Let roundedTime be ? RoundTimeDurationToIncrement(timeDuration, unitLength × increment, roundingMode).
    auto unit_length_multiplied_by_increment = unit_length.multiplied_by(Crypto::UnsignedBigInteger { increment });
    auto rounded_time = TRY(round_time_duration_to_increment(vm, time_duration, unit_length_multiplied_by_increment, rounding_mode));

    // 4. Let diffTime be ! AddTimeDuration(roundedTime, -timeDuration).
    time_duration.negate();
    auto diff_time = MUST(add_time_duration(vm, rounded_time, time_duration));
    time_duration.negate();

    // 5. Let wholeDays be truncate(TotalTimeDuration(timeDuration, DAY)).
    auto whole_days = trunc(total_time_duration(time_duration, Unit::Day).to_double());

    // 6. Let roundedWholeDays be truncate(TotalTimeDuration(roundedTime, DAY)).
    auto rounded_whole_days = trunc(total_time_duration(rounded_time, Unit::Day).to_double());

    // 7. Let dayDelta be roundedWholeDays - wholeDays.
    auto day_delta = rounded_whole_days - whole_days;

    // 8. If dayDelta < 0, let dayDeltaSign be -1; else if dayDelta > 0, let dayDeltaSign be 1; else let dayDeltaSign be 0.
    auto day_delta_sign = day_delta < 0 ? -1 : (day_delta > 0 ? 1 : 0);

    // 9. If dayDeltaSign = TimeDurationSign(timeDuration), let didExpandDays be true; else let didExpandDays be false.
    auto did_expand_days = day_delta_sign == time_duration_sign(time_duration);

    // 10. Let nudgedEpochNs be AddTimeDurationToEpochNanoseconds(diffTime, destEpochNs).
    auto nudged_epoch_ns = add_time_duration_to_epoch_nanoseconds(diff_time, dest_epoch_ns);

    // 11. Let days be 0.
    double days = 0;

    // 12. Let remainder be roundedTime.
    TimeDuration remainder;

    // 13. If TemporalUnitCategory(largestUnit) is DATE, then
    if (temporal_unit_category(largest_unit) == UnitCategory::Date) {
        // a. Set days to roundedWholeDays.
        days = rounded_whole_days;

        // b. Set remainder to ! AddTimeDuration(roundedTime, TimeDurationFromComponents(-roundedWholeDays * HoursPerDay, 0, 0, 0, 0, 0)).
        remainder = MUST(add_time_duration(vm, rounded_time, time_duration_from_components(-rounded_whole_days * JS::hours_per_day, 0, 0, 0, 0, 0)));
    } else {
        remainder = move(rounded_time);
    }

    // 14. Let dateDuration be ! AdjustDateDurationRecord(duration.[[Date]], days).
    auto date_duration = MUST(adjust_date_duration_record(vm, duration.date, days));

    // 15. Let resultDuration be CombineDateAndTimeDuration(dateDuration, remainder).
    auto result_duration = combine_date_and_time_duration(date_duration, move(remainder));

    // 16. Return Duration Nudge Result Record { [[Duration]]: resultDuration, [[NudgedEpochNs]]: nudgedEpochNs, [[DidExpandCalendarUnit]]: didExpandDays }.
    return DurationNudgeResult { .duration = move(result_duration), .nudged_epoch_ns = move(nudged_epoch_ns), .did_expand_calendar_unit = did_expand_days };
}

// 7.5.36 BubbleRelativeDuration ( sign, duration, nudgedEpochNs, isoDateTime, timeZone, calendar, largestUnit, smallestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-bubblerelativeduration
ThrowCompletionOr<InternalDuration> bubble_relative_duration(VM& vm, i8 sign, InternalDuration duration, Crypto::SignedBigInteger const& nudged_epoch_ns, ISODateTime const& iso_date_time, Optional<StringView> time_zone, StringView calendar, Unit largest_unit, Unit smallest_unit)
{
    // 1. If smallestUnit is largestUnit, return duration.
    if (smallest_unit == largest_unit)
        return duration;

    // 2. Let largestUnitIndex be the ordinal index of the row of Table 21 whose "Value" column contains largestUnit.
    auto largest_unit_index = to_underlying(largest_unit);

    // 3. Let smallestUnitIndex be the ordinal index of the row of Table 21 whose "Value" column contains smallestUnit.
    auto smallest_unit_index = to_underlying(smallest_unit);

    // 4. Let unitIndex be smallestUnitIndex - 1.
    auto unit_index = smallest_unit_index - 1;

    // 5. Let done be false.
    auto done = false;

    // 6. Repeat, while unitIndex ≥ largestUnitIndex and done is false,
    while (unit_index >= largest_unit_index && !done) {
        // a. Let unit be the value in the "Value" column of Table 21 in the row whose ordinal index is unitIndex.
        auto unit = static_cast<Unit>(unit_index);

        // b. If unit is not WEEK, or largestUnit is WEEK, then
        if (unit != Unit::Week || largest_unit == Unit::Week) {
            DateDuration end_duration;

            // i. If unit is YEAR, then
            if (unit == Unit::Year) {
                // 1. Let years be duration.[[Date]].[[Years]] + sign.
                auto years = duration.date.years + sign;

                // 2. Let endDuration be ? CreateDateDurationRecord(years, 0, 0, 0).
                end_duration = TRY(create_date_duration_record(vm, years, 0, 0, 0));
            }
            // ii. Else if unit is MONTH, then
            else if (unit == Unit::Month) {
                // 1. Let months be duration.[[Date]].[[Months]] + sign.
                auto months = duration.date.months + sign;

                // 2. Let endDuration be ? AdjustDateDurationRecord(duration.[[Date]], 0, 0, months).
                end_duration = TRY(adjust_date_duration_record(vm, duration.date, 0, 0, months));
            }
            // iii. Else,
            else {
                // 1. Assert: unit is WEEK.
                VERIFY(unit == Unit::Week);

                // 2. Let weeks be duration.[[Date]].[[Weeks]] + sign.
                auto weeks = duration.date.weeks + sign;

                // 3. Let endDuration be ? AdjustDateDurationRecord(duration.[[Date]], 0, weeks).
                end_duration = TRY(adjust_date_duration_record(vm, duration.date, 0, weeks));
            }

            // iv. Let end be ? CalendarDateAdd(calendar, isoDateTime.[[ISODate]], endDuration, CONSTRAIN).
            auto end = TRY(calendar_date_add(vm, calendar, iso_date_time.iso_date, end_duration, Overflow::Constrain));

            // v. Let endDateTime be CombineISODateAndTimeRecord(end, isoDateTime.[[Time]]).
            auto end_date_time = combine_iso_date_and_time_record(end, iso_date_time.time);

            Crypto::SignedBigInteger end_epoch_ns;

            // vi. If timeZone is UNSET, then
            if (!time_zone.has_value()) {
                // 1. Let endEpochNs be GetUTCEpochNanoseconds(endDateTime).
                end_epoch_ns = get_utc_epoch_nanoseconds(end_date_time);
            }
            // vii. Else,
            else {
                // 1. Let endEpochNs be ? GetEpochNanosecondsFor(timeZone, endDateTime, COMPATIBLE).
                end_epoch_ns = TRY(get_epoch_nanoseconds_for(vm, *time_zone, end_date_time, Disambiguation::Compatible));
            }

            // viii. Let beyondEnd be nudgedEpochNs - endEpochNs.
            auto beyond_end = nudged_epoch_ns.minus(end_epoch_ns);

            // ix. If beyondEnd < 0, let beyondEndSign be -1; else if beyondEnd > 0, let beyondEndSign be 1; else let beyondEndSign be 0.
            auto beyond_end_sign = beyond_end.is_negative() ? -1 : (beyond_end.is_positive() ? 1 : 0);

            // x. If beyondEndSign ≠ -sign, then
            if (beyond_end_sign != -sign) {
                // 1. Set duration to CombineDateAndTimeDuration(endDuration, 0).
                duration = combine_date_and_time_duration(end_duration, TimeDuration { 0 });
            }
            // xi. Else,
            else {
                // 1. Set done to true.
                done = true;
            }
        }

        // c. Set unitIndex to unitIndex - 1.
        --unit_index;
    }

    // 7. Return duration.
    return duration;
}

// 7.5.37 RoundRelativeDuration ( duration, destEpochNs, isoDateTime, timeZone, calendar, largestUnit, increment, smallestUnit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundrelativeduration
ThrowCompletionOr<InternalDuration> round_relative_duration(VM& vm, InternalDuration duration, Crypto::SignedBigInteger const& dest_epoch_ns, ISODateTime const& iso_date_time, Optional<StringView> time_zone, StringView calendar, Unit largest_unit, u64 increment, Unit smallest_unit, RoundingMode rounding_mode)
{
    // 1. Let irregularLengthUnit be false.
    auto irregular_length_unit = false;

    // 2. If IsCalendarUnit(smallestUnit) is true, set irregularLengthUnit to true.
    if (is_calendar_unit(smallest_unit))
        irregular_length_unit = true;

    // 3. If timeZone is not UNSET and smallestUnit is DAY, set irregularLengthUnit to true.
    if (time_zone.has_value() && smallest_unit == Unit::Day)
        irregular_length_unit = true;

    // 4. If InternalDurationSign(duration) < 0, let sign be -1; else let sign be 1.
    i8 sign = internal_duration_sign(duration) < 0 ? -1 : 1;

    DurationNudgeResult nudge_result;

    // 5. If irregularLengthUnit is true, then
    if (irregular_length_unit) {
        // a. Let record be ? NudgeToCalendarUnit(sign, duration, destEpochNs, isoDateTime, timeZone, calendar, increment, smallestUnit, roundingMode).
        auto record = TRY(nudge_to_calendar_unit(vm, sign, duration, dest_epoch_ns, iso_date_time, time_zone, calendar, increment, smallest_unit, rounding_mode));

        // b. Let nudgeResult be record.[[NudgeResult]].
        nudge_result = move(record.nudge_result);
    }
    // 6. Else if timeZone is not UNSET, then
    else if (time_zone.has_value()) {
        // a. Let nudgeResult be ? NudgeToZonedTime(sign, duration, isoDateTime, timeZone, calendar, increment, smallestUnit, roundingMode).
        nudge_result = TRY(nudge_to_zoned_time(vm, sign, duration, iso_date_time, *time_zone, calendar, increment, smallest_unit, rounding_mode));
    }
    // 7. Else,
    else {
        // a. Let nudgeResult be ? NudgeToDayOrTime(duration, destEpochNs, largestUnit, increment, smallestUnit, roundingMode).
        nudge_result = TRY(nudge_to_day_or_time(vm, duration, dest_epoch_ns, largest_unit, increment, smallest_unit, rounding_mode));
    }

    // 8. Set duration to nudgeResult.[[Duration]].
    duration = move(nudge_result.duration);

    // 9. If nudgeResult.[[DidExpandCalendarUnit]] is true and smallestUnit is not WEEK, then
    if (nudge_result.did_expand_calendar_unit && smallest_unit != Unit::Week) {
        // a. Let startUnit be LargerOfTwoTemporalUnits(smallestUnit, DAY).
        auto start_unit = larger_of_two_temporal_units(smallest_unit, Unit::Day);

        // b. Set duration to ? BubbleRelativeDuration(sign, duration, nudgeResult.[[NudgedEpochNs]], isoDateTime, timeZone, calendar, largestUnit, startUnit).
        duration = TRY(bubble_relative_duration(vm, sign, move(duration), nudge_result.nudged_epoch_ns, iso_date_time, time_zone, calendar, largest_unit, start_unit));
    }

    // 10. Return duration.
    return duration;
}

// 7.5.38 TotalRelativeDuration ( duration, destEpochNs, isoDateTime, timeZone, calendar, unit ), https://tc39.es/proposal-temporal/#sec-temporal-totalrelativeduration
ThrowCompletionOr<Crypto::BigFraction> total_relative_duration(VM& vm, InternalDuration const& duration, Crypto::SignedBigInteger const& dest_epoch_ns, ISODateTime const& iso_date_time, Optional<StringView> time_zone, StringView calendar, Unit unit)
{
    // 1. If IsCalendarUnit(unit) is true, or timeZone is not UNSET and unit is DAY, then
    if (is_calendar_unit(unit) || (time_zone.has_value() && unit == Unit::Day)) {
        // a. Let sign be InternalDurationSign(duration).
        auto sign = internal_duration_sign(duration);

        // b. Let record be ? NudgeToCalendarUnit(sign, duration, destEpochNs, isoDateTime, timeZone, calendar, 1, unit, TRUNC).
        auto record = TRY(nudge_to_calendar_unit(vm, sign, duration, dest_epoch_ns, iso_date_time, time_zone, calendar, 1, unit, RoundingMode::Trunc));

        // c. Return record.[[Total]].
        return record.total;
    }

    // 2. Let timeDuration be ! Add24HourDaysToTimeDuration(duration.[[Time]], duration.[[Date]].[[Days]]).
    auto time_duration = MUST(add_24_hour_days_to_time_duration(vm, duration.time, duration.date.days));

    // 3. Return TotalTimeDuration(timeDuration, unit).
    return total_time_duration(time_duration, unit);
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

    // 10. Let result be CombineDateAndTimeDuration(ZeroDateDuration(), timeResult).
    auto result = combine_date_and_time_duration(zero_date_duration(vm), move(time_result));

    // 11. Return ? TemporalDurationFromInternal(result, largestUnit).
    return TRY(temporal_duration_from_internal(vm, result, largest_unit));
}

}
