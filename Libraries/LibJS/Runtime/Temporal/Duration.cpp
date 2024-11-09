/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/StringBuilder.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/DurationConstructor.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(Duration);

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

    // NOTE: The spec stores these fields as mathematical values. VERIFY() that we have finite,
    // integral values in them, and normalize any negative zeros caused by floating point math.
    // This is usually done using ℝ(𝔽(value)) at the call site.
    for (auto const& field : fields) {
        auto& value = this->*field;
        VERIFY(isfinite(value));
        // FIXME: test-js contains a small number of cases where a Temporal.Duration is constructed
        //        with a non-integral double. Eliminate these and VERIFY(trunc(value) == value) instead.
        if (trunc(value) != value)
            value = trunc(value);
        else if (bit_cast<u64>(value) == NEGATIVE_ZERO_BITS)
            value = 0;
    }
}

// NOTE: All of these have two overloads: one that can throw, and one that can't.
// This is so that we don't have to needlessly pass a global object and then unwrap
// the ThrowCompletionOr when we already know that the duration is valid.

// 7.5.5 CreateDurationRecord ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-createdurationrecord
DurationRecord create_duration_record(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds)
{
    // 1. If ! IsValidDuration(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds) is false, throw a RangeError exception.
    VERIFY(is_valid_duration(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds));

    // 2. Return the Record { [[Years]]: ℝ(𝔽(years)), [[Months]]: ℝ(𝔽(months)), [[Weeks]]: ℝ(𝔽(weeks)), [[Days]]: ℝ(𝔽(days)), [[Hours]]: ℝ(𝔽(hours)), [[Minutes]]: ℝ(𝔽(minutes)), [[Seconds]]: ℝ(𝔽(seconds)), [[Milliseconds]]: ℝ(𝔽(milliseconds)), [[Microseconds]]: ℝ(𝔽(microseconds)), [[Nanoseconds]]: ℝ(𝔽(nanoseconds)) }.
    return DurationRecord { .years = years, .months = months, .weeks = weeks, .days = days, .hours = hours, .minutes = minutes, .seconds = seconds, .milliseconds = milliseconds, .microseconds = microseconds, .nanoseconds = nanoseconds };
}

// 7.5.5 CreateDurationRecord ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-createdurationrecord
ThrowCompletionOr<DurationRecord> create_duration_record(VM& vm, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds)
{
    // 1. If ! IsValidDuration(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds) is false, throw a RangeError exception.
    if (!is_valid_duration(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDuration);

    // 2. Return the Record { [[Years]]: ℝ(𝔽(years)), [[Months]]: ℝ(𝔽(months)), [[Weeks]]: ℝ(𝔽(weeks)), [[Days]]: ℝ(𝔽(days)), [[Hours]]: ℝ(𝔽(hours)), [[Minutes]]: ℝ(𝔽(minutes)), [[Seconds]]: ℝ(𝔽(seconds)), [[Milliseconds]]: ℝ(𝔽(milliseconds)), [[Microseconds]]: ℝ(𝔽(microseconds)), [[Nanoseconds]]: ℝ(𝔽(nanoseconds)) }.
    return DurationRecord { .years = years, .months = months, .weeks = weeks, .days = days, .hours = hours, .minutes = minutes, .seconds = seconds, .milliseconds = milliseconds, .microseconds = microseconds, .nanoseconds = nanoseconds };
}

// 7.5.6 CreateDateDurationRecord ( years, months, weeks, days ), https://tc39.es/proposal-temporal/#sec-temporal-createdatedurationrecord
DateDurationRecord create_date_duration_record(double years, double months, double weeks, double days)
{
    // 1. If ! IsValidDuration(years, months, weeks, days, 0, 0, 0, 0, 0, 0) is false, throw a RangeError exception.
    VERIFY(is_valid_duration(years, months, weeks, days, 0, 0, 0, 0, 0, 0));

    // 2. Return the Record { [[Years]]: ℝ(𝔽(years)), [[Months]]: ℝ(𝔽(months)), [[Weeks]]: ℝ(𝔽(weeks)), [[Days]]: ℝ(𝔽(days)) }.
    return DateDurationRecord { .years = years, .months = months, .weeks = weeks, .days = days };
}

// 7.5.6 CreateDateDurationRecord ( years, months, weeks, days ), https://tc39.es/proposal-temporal/#sec-temporal-createdatedurationrecord
ThrowCompletionOr<DateDurationRecord> create_date_duration_record(VM& vm, double years, double months, double weeks, double days)
{
    // 1. If ! IsValidDuration(years, months, weeks, days, 0, 0, 0, 0, 0, 0) is false, throw a RangeError exception.
    if (!is_valid_duration(years, months, weeks, days, 0, 0, 0, 0, 0, 0))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDuration);

    // 2. Return the Record { [[Years]]: ℝ(𝔽(years)), [[Months]]: ℝ(𝔽(months)), [[Weeks]]: ℝ(𝔽(weeks)), [[Days]]: ℝ(𝔽(days)) }.
    return DateDurationRecord { .years = years, .months = months, .weeks = weeks, .days = days };
}

// 7.5.7 CreateTimeDurationRecord ( days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-createtimedurationrecord
ThrowCompletionOr<TimeDurationRecord> create_time_duration_record(VM& vm, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds)
{
    // 1. If ! IsValidDuration(0, 0, 0, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds) is false, throw a RangeError exception.
    if (!is_valid_duration(0, 0, 0, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDuration);

    // 2. Return the Record { [[Days]]: ℝ(𝔽(days)), [[Hours]]: ℝ(𝔽(hours)), [[Minutes]]: ℝ(𝔽(minutes)), [[Seconds]]: ℝ(𝔽(seconds)), [[Milliseconds]]: ℝ(𝔽(milliseconds)), [[Microseconds]]: ℝ(𝔽(microseconds)), [[Nanoseconds]]: ℝ(𝔽(nanoseconds)) }.
    return TimeDurationRecord { .days = days, .hours = hours, .minutes = minutes, .seconds = seconds, .milliseconds = milliseconds, .microseconds = microseconds, .nanoseconds = nanoseconds };
}

// 7.5.8 ToTemporalDuration ( item ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalduration
ThrowCompletionOr<NonnullGCPtr<Duration>> to_temporal_duration(VM& vm, Value item)
{
    // 1. If Type(item) is Object and item has an [[InitializedTemporalDuration]] internal slot, then
    if (item.is_object() && is<Duration>(item.as_object())) {
        // a. Return item.
        return static_cast<Duration&>(item.as_object());
    }

    // 2. Let result be ? ToTemporalDurationRecord(item).
    auto result = TRY(to_temporal_duration_record(vm, item));

    // 3. Return ! CreateTemporalDuration(result.[[Years]], result.[[Months]], result.[[Weeks]], result.[[Days]], result.[[Hours]], result.[[Minutes]], result.[[Seconds]], result.[[Milliseconds]], result.[[Microseconds]], result.[[Nanoseconds]]).
    return MUST(create_temporal_duration(vm, result.years, result.months, result.weeks, result.days, result.hours, result.minutes, result.seconds, result.milliseconds, result.microseconds, result.nanoseconds));
}

// 7.5.9 ToTemporalDurationRecord ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaldurationrecord
ThrowCompletionOr<DurationRecord> to_temporal_duration_record(VM& vm, Value temporal_duration_like)
{
    // 1. If Type(temporalDurationLike) is not Object, then
    if (!temporal_duration_like.is_object()) {
        // a. Let string be ? ToString(temporalDurationLike).
        auto string = TRY(temporal_duration_like.to_string(vm));

        // b. Return ? ParseTemporalDurationString(string).
        return parse_temporal_duration_string(vm, string);
    }

    // 2. If temporalDurationLike has an [[InitializedTemporalDuration]] internal slot, then
    if (is<Duration>(temporal_duration_like.as_object())) {
        auto& duration = static_cast<Duration const&>(temporal_duration_like.as_object());

        // a. Return ! CreateDurationRecord(temporalDurationLike.[[Years]], temporalDurationLike.[[Months]], temporalDurationLike.[[Weeks]], temporalDurationLike.[[Days]], temporalDurationLike.[[Hours]], temporalDurationLike.[[Minutes]], temporalDurationLike.[[Seconds]], temporalDurationLike.[[Milliseconds]], temporalDurationLike.[[Microseconds]], temporalDurationLike.[[Nanoseconds]]).
        return create_duration_record(duration.years(), duration.months(), duration.weeks(), duration.days(), duration.hours(), duration.minutes(), duration.seconds(), duration.milliseconds(), duration.microseconds(), duration.nanoseconds());
    }

    // 3. Let result be a new Duration Record with each field set to 0.
    auto result = DurationRecord {};

    // 4. Let partial be ? ToTemporalPartialDurationRecord(temporalDurationLike).
    auto partial = TRY(to_temporal_partial_duration_record(vm, temporal_duration_like));

    // 5. If partial.[[Years]] is not undefined, set result.[[Years]] to partial.[[Years]].
    if (partial.years.has_value())
        result.years = partial.years.value();

    // 6. If partial.[[Months]] is not undefined, set result.[[Months]] to partial.[[Months]].
    if (partial.months.has_value())
        result.months = partial.months.value();

    // 7. If partial.[[Weeks]] is not undefined, set result.[[Weeks]] to partial.[[Weeks]].
    if (partial.weeks.has_value())
        result.weeks = partial.weeks.value();

    // 8. If partial.[[Days]] is not undefined, set result.[[Days]] to partial.[[Days]].
    if (partial.days.has_value())
        result.days = partial.days.value();

    // 9. If partial.[[Hours]] is not undefined, set result.[[Hours]] to partial.[[Hours]].
    if (partial.hours.has_value())
        result.hours = partial.hours.value();

    // 10. If partial.[[Minutes]] is not undefined, set result.[[Minutes]] to partial.[[Minutes]].
    if (partial.minutes.has_value())
        result.minutes = partial.minutes.value();

    // 11. If partial.[[Seconds]] is not undefined, set result.[[Seconds]] to partial.[[Seconds]].
    if (partial.seconds.has_value())
        result.seconds = partial.seconds.value();

    // 12. If partial.[[Milliseconds]] is not undefined, set result.[[Milliseconds]] to partial.[[Milliseconds]].
    if (partial.milliseconds.has_value())
        result.milliseconds = partial.milliseconds.value();

    // 13. If partial.[[Microseconds]] is not undefined, set result.[[Microseconds]] to partial.[[Microseconds]].
    if (partial.microseconds.has_value())
        result.microseconds = partial.microseconds.value();

    // 14. If partial.[[Nanoseconds]] is not undefined, set result.[[Nanoseconds]] to partial.[[Nanoseconds]].
    if (partial.nanoseconds.has_value())
        result.nanoseconds = partial.nanoseconds.value();

    // 15. If ! IsValidDuration(result.[[Years]], result.[[Months]], result.[[Weeks]], result.[[Days]], result.[[Hours]], result.[[Minutes]], result.[[Seconds]], result.[[Milliseconds]], result.[[Microseconds]], result.[[Nanoseconds]]) is false, then
    if (!is_valid_duration(result.years, result.months, result.weeks, result.days, result.hours, result.minutes, result.seconds, result.milliseconds, result.microseconds, result.nanoseconds)) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDuration);
    }

    // 16. Return result.
    return result;
}

// 7.5.10 DurationSign ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-durationsign
i8 duration_sign(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds)
{
    // 1. For each value v of « years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds », do
    for (auto& v : { years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds }) {
        // a. If v < 0, return -1.
        if (v < 0)
            return -1;

        // b. If v > 0, return 1.
        if (v > 0)
            return 1;
    }

    // 2. Return 0.
    return 0;
}

// 7.5.11 IsValidDuration ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidduration
bool is_valid_duration(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds)
{
    // 1. Let sign be ! DurationSign(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
    auto sign = duration_sign(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds);

    // 2. For each value v of « years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds », do
    for (auto& v : { years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds }) {
        // a. If 𝔽(v) is not finite, return false.
        if (!isfinite(v))
            return false;

        // b. If v < 0 and sign > 0, return false.
        if (v < 0 && sign > 0)
            return false;

        // c. If v > 0 and sign < 0, return false.
        if (v > 0 && sign < 0)
            return false;
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
    // 7. NOTE: The above step cannot be implemented directly using floating-point arithmetic. Multiplying by 10**-3, 10**-6, and 10**-9 respectively may be imprecise when milliseconds, microseconds, or nanoseconds is an unsafe integer. This multiplication can be implemented in C++ with an implementation of std::remquo() with sufficient bits in the quotient. String manipulation will also give an exact result, since the multiplication is by a power of 10.
    static Crypto::SignedBigInteger days_to_nanoseconds { 8.64e13 };
    static Crypto::SignedBigInteger hours_to_nanoseconds { 3.6e12 };
    static Crypto::SignedBigInteger minutes_to_nanoseconds { 6e10 };
    static Crypto::SignedBigInteger seconds_to_nanoseconds { 1e9 };
    static Crypto::SignedBigInteger milliseconds_to_nanoseconds { 1e6 };
    static Crypto::SignedBigInteger microseconds_to_nanoseconds { 1e3 };

    auto normalized_nanoseconds = Crypto::SignedBigInteger { days }.multiplied_by(days_to_nanoseconds);
    normalized_nanoseconds = normalized_nanoseconds.plus(Crypto::SignedBigInteger { hours }.multiplied_by(hours_to_nanoseconds));
    normalized_nanoseconds = normalized_nanoseconds.plus(Crypto::SignedBigInteger { minutes }.multiplied_by(minutes_to_nanoseconds));
    normalized_nanoseconds = normalized_nanoseconds.plus(Crypto::SignedBigInteger { seconds }.multiplied_by(seconds_to_nanoseconds));
    normalized_nanoseconds = normalized_nanoseconds.plus(Crypto::SignedBigInteger { milliseconds }.multiplied_by(milliseconds_to_nanoseconds));
    normalized_nanoseconds = normalized_nanoseconds.plus(Crypto::SignedBigInteger { microseconds }.multiplied_by(microseconds_to_nanoseconds));
    normalized_nanoseconds = normalized_nanoseconds.plus(Crypto::SignedBigInteger { nanoseconds });

    // 8. If abs(normalizedSeconds) ≥ 2**53, return false.
    static auto maximum_time = Crypto::SignedBigInteger { MAX_ARRAY_LIKE_INDEX }.plus(Crypto::SignedBigInteger { 1 }).multiplied_by(seconds_to_nanoseconds);

    if (normalized_nanoseconds.is_negative())
        normalized_nanoseconds.negate();

    if (normalized_nanoseconds >= maximum_time)
        return false;

    // 3. Return true.
    return true;
}

// 7.5.12 DefaultTemporalLargestUnit ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds ), https://tc39.es/proposal-temporal/#sec-temporal-defaulttemporallargestunit
StringView default_temporal_largest_unit(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds)
{
    // 1. If years ≠ 0, return "year".
    if (years != 0)
        return "year"sv;

    // 2. If months ≠ 0, return "month".
    if (months != 0)
        return "month"sv;

    // 3. If weeks ≠ 0, return "week".
    if (weeks != 0)
        return "week"sv;

    // 4. If days ≠ 0, return "day".
    if (days != 0)
        return "day"sv;

    // 5. If hours ≠ 0, return "hour".
    if (hours != 0)
        return "hour"sv;

    // 6. If minutes ≠ 0, return "minute".
    if (minutes != 0)
        return "minute"sv;

    // 7. If seconds ≠ 0, return "second".
    if (seconds != 0)
        return "second"sv;

    // 8. If milliseconds ≠ 0, return "millisecond".
    if (milliseconds != 0)
        return "millisecond"sv;

    // 9. If microseconds ≠ 0, return "microsecond".
    if (microseconds != 0)
        return "microsecond"sv;

    // 10. Return "nanosecond".
    return "nanosecond"sv;
}

// 7.5.13 ToTemporalPartialDurationRecord ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalpartialdurationrecord
ThrowCompletionOr<PartialDurationRecord> to_temporal_partial_duration_record(VM& vm, Value temporal_duration_like)
{
    // 1. If Type(temporalDurationLike) is not Object, then
    if (!temporal_duration_like.is_object()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, temporal_duration_like.to_string_without_side_effects());
    }

    // 2. Let result be a new partial Duration Record with each field set to undefined.
    auto result = PartialDurationRecord {};

    // 3. Let days be ? Get(temporalDurationLike, "days").
    auto days = TRY(temporal_duration_like.as_object().get(vm.names.days));

    // 4. If days is not undefined, set result.[[Days]] to ? ToIntegerIfIntegral(days).
    if (!days.is_undefined())
        result.days = TRY(to_integer_if_integral(vm, days, ErrorType::TemporalInvalidDurationPropertyValueNonIntegral, "days"sv, days));

    // 5. Let hours be ? Get(temporalDurationLike, "hours").
    auto hours = TRY(temporal_duration_like.as_object().get(vm.names.hours));

    // 6. If hours is not undefined, set result.[[Hours]] to ? ToIntegerIfIntegral(hours).
    if (!hours.is_undefined())
        result.hours = TRY(to_integer_if_integral(vm, hours, ErrorType::TemporalInvalidDurationPropertyValueNonIntegral, "hours"sv, hours));

    // 7. Let microseconds be ? Get(temporalDurationLike, "microseconds").
    auto microseconds = TRY(temporal_duration_like.as_object().get(vm.names.microseconds));

    // 8. If microseconds is not undefined, set result.[[Microseconds]] to ? ToIntegerIfIntegral(microseconds).
    if (!microseconds.is_undefined())
        result.microseconds = TRY(to_integer_if_integral(vm, microseconds, ErrorType::TemporalInvalidDurationPropertyValueNonIntegral, "microseconds"sv, microseconds));

    // 9. Let milliseconds be ? Get(temporalDurationLike, "milliseconds").
    auto milliseconds = TRY(temporal_duration_like.as_object().get(vm.names.milliseconds));

    // 10. If milliseconds is not undefined, set result.[[Milliseconds]] to ? ToIntegerIfIntegral(milliseconds).
    if (!milliseconds.is_undefined())
        result.milliseconds = TRY(to_integer_if_integral(vm, milliseconds, ErrorType::TemporalInvalidDurationPropertyValueNonIntegral, "milliseconds"sv, milliseconds));

    // 11. Let minutes be ? Get(temporalDurationLike, "minutes").
    auto minutes = TRY(temporal_duration_like.as_object().get(vm.names.minutes));

    // 12. If minutes is not undefined, set result.[[Minutes]] to ? ToIntegerIfIntegral(minutes).
    if (!minutes.is_undefined())
        result.minutes = TRY(to_integer_if_integral(vm, minutes, ErrorType::TemporalInvalidDurationPropertyValueNonIntegral, "minutes"sv, minutes));

    // 13. Let months be ? Get(temporalDurationLike, "months").
    auto months = TRY(temporal_duration_like.as_object().get(vm.names.months));

    // 14. If months is not undefined, set result.[[Months]] to ? ToIntegerIfIntegral(months).
    if (!months.is_undefined())
        result.months = TRY(to_integer_if_integral(vm, months, ErrorType::TemporalInvalidDurationPropertyValueNonIntegral, "months"sv, months));

    // 15. Let nanoseconds be ? Get(temporalDurationLike, "nanoseconds").
    auto nanoseconds = TRY(temporal_duration_like.as_object().get(vm.names.nanoseconds));

    // 16. If nanoseconds is not undefined, set result.[[Nanoseconds]] to ? ToIntegerIfIntegral(nanoseconds).
    if (!nanoseconds.is_undefined())
        result.nanoseconds = TRY(to_integer_if_integral(vm, nanoseconds, ErrorType::TemporalInvalidDurationPropertyValueNonIntegral, "nanoseconds"sv, nanoseconds));

    // 17. Let seconds be ? Get(temporalDurationLike, "seconds").
    auto seconds = TRY(temporal_duration_like.as_object().get(vm.names.seconds));

    // 18. If seconds is not undefined, set result.[[Seconds]] to ? ToIntegerIfIntegral(seconds).
    if (!seconds.is_undefined())
        result.seconds = TRY(to_integer_if_integral(vm, seconds, ErrorType::TemporalInvalidDurationPropertyValueNonIntegral, "seconds"sv, seconds));

    // 19. Let weeks be ? Get(temporalDurationLike, "weeks").
    auto weeks = TRY(temporal_duration_like.as_object().get(vm.names.weeks));

    // 20. If weeks is not undefined, set result.[[Weeks]] to ? ToIntegerIfIntegral(weeks).
    if (!weeks.is_undefined())
        result.weeks = TRY(to_integer_if_integral(vm, weeks, ErrorType::TemporalInvalidDurationPropertyValueNonIntegral, "weeks"sv, weeks));

    // 21. Let years be ? Get(temporalDurationLike, "years").
    auto years = TRY(temporal_duration_like.as_object().get(vm.names.years));

    // 22. If years is not undefined, set result.[[Years]] to ? ToIntegerIfIntegral(years).
    if (!years.is_undefined())
        result.years = TRY(to_integer_if_integral(vm, years, ErrorType::TemporalInvalidDurationPropertyValueNonIntegral, "years"sv, years));

    // 23. If years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, and nanoseconds are all undefined, throw a TypeError exception.
    if (years.is_undefined() && months.is_undefined() && weeks.is_undefined() && days.is_undefined() && hours.is_undefined() && minutes.is_undefined() && seconds.is_undefined() && milliseconds.is_undefined() && microseconds.is_undefined() && nanoseconds.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidDurationLikeObject);

    // 24. Return result.
    return result;
}

// 7.5.14 CreateTemporalDuration ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporalduration
ThrowCompletionOr<NonnullGCPtr<Duration>> create_temporal_duration(VM& vm, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, FunctionObject const* new_target)
{
    auto& realm = *vm.current_realm();

    // 1. If ! IsValidDuration(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds) is false, throw a RangeError exception.
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

// 7.5.15 CreateNegatedTemporalDuration ( duration ), https://tc39.es/proposal-temporal/#sec-temporal-createnegatedtemporalduration
NonnullGCPtr<Duration> create_negated_temporal_duration(VM& vm, Duration const& duration)
{
    // 1. Return ! CreateTemporalDuration(-duration.[[Years]], -duration.[[Months]], -duration.[[Weeks]], -duration.[[Days]], -duration.[[Hours]], -duration.[[Minutes]], -duration.[[Seconds]], -duration.[[Milliseconds]], -duration.[[Microseconds]], -duration.[[Nanoseconds]]).
    return MUST(create_temporal_duration(vm, -duration.years(), -duration.months(), -duration.weeks(), -duration.days(), -duration.hours(), -duration.minutes(), -duration.seconds(), -duration.milliseconds(), -duration.microseconds(), -duration.nanoseconds()));
}

// 7.5.16 CalculateOffsetShift ( relativeTo, y, mon, w, d ), https://tc39.es/proposal-temporal/#sec-temporal-calculateoffsetshift
ThrowCompletionOr<double> calculate_offset_shift(VM& vm, Value relative_to_value, double years, double months, double weeks, double days)
{
    // 1. If Type(relativeTo) is not Object or relativeTo does not have an [[InitializedTemporalZonedDateTime]] internal slot, return 0.
    if (!relative_to_value.is_object() || !is<ZonedDateTime>(relative_to_value.as_object()))
        return 0.0;

    auto& relative_to = static_cast<ZonedDateTime&>(relative_to_value.as_object());
    auto time_zone_record = TRY(create_time_zone_methods_record(vm, NonnullGCPtr<Object> { relative_to.time_zone() }, { { TimeZoneMethod::GetOffsetNanosecondsFor } }));

    // 2. Let instant be ! CreateTemporalInstant(relativeTo.[[Nanoseconds]]).
    auto* instant = MUST(create_temporal_instant(vm, relative_to.nanoseconds()));

    // 3. Let offsetBefore be ? GetOffsetNanosecondsFor(relativeTo.[[TimeZone]], instant).
    auto offset_before = TRY(get_offset_nanoseconds_for(vm, time_zone_record, *instant));

    // 4. Let after be ? AddZonedDateTime(relativeTo.[[Nanoseconds]], relativeTo.[[TimeZone]], relativeTo.[[Calendar]], y, mon, w, d, 0, 0, 0, 0, 0, 0).
    auto* after = TRY(add_zoned_date_time(vm, relative_to.nanoseconds(), &relative_to.time_zone(), relative_to.calendar(), years, months, weeks, days, 0, 0, 0, 0, 0, 0));

    // 5. Let instantAfter be ! CreateTemporalInstant(after).
    auto* instant_after = MUST(create_temporal_instant(vm, *after));

    // 6. Let offsetAfter be ? GetOffsetNanosecondsFor(relativeTo.[[TimeZone]], instantAfter).
    auto offset_after = TRY(get_offset_nanoseconds_for(vm, time_zone_record, *instant_after));

    // 7. Return offsetAfter - offsetBefore.
    return offset_after - offset_before;
}

// 7.5.17 BalanceTimeDuration ( days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-balancetimeduration
ThrowCompletionOr<TimeDurationRecord> balance_time_duration(VM& vm, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, Crypto::SignedBigInteger const& nanoseconds, StringView largest_unit)
{
    // 1. Let balanceResult be BalancePossiblyInfiniteTimeDuration(days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, largestUnit).
    auto balance_result = balance_possibly_infinite_time_duration(vm, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, largest_unit);

    // 2. If balanceResult is positive overflow or negative overflow, then
    if (balance_result.has<Overflow>()) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDuration);
    }
    // 3. Else,
    else {
        // a. Return balanceResult.
        return balance_result.get<TimeDurationRecord>();
    }
}

// 7.5.17 TotalDurationNanoseconds ( days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, offsetShift ), https://tc39.es/proposal-temporal/#sec-temporal-totaldurationnanoseconds
Crypto::SignedBigInteger total_duration_nanoseconds(double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, Crypto::SignedBigInteger const& nanoseconds, double offset_shift)
{
    VERIFY(offset_shift == trunc(offset_shift));

    auto result_nanoseconds = nanoseconds;

    // 1. If days ≠ 0, then
    if (days != 0) {
        // a. Set nanoseconds to nanoseconds - offsetShift.
        result_nanoseconds = result_nanoseconds.minus(Crypto::SignedBigInteger { offset_shift });
    }
    // 2. Set hours to hours + days × 24.
    auto total_hours = Crypto::SignedBigInteger { hours }.plus(Crypto::SignedBigInteger { days }.multiplied_by(Crypto::UnsignedBigInteger(24)));
    // 3. Set minutes to minutes + hours × 60.
    auto total_minutes = Crypto::SignedBigInteger { minutes }.plus(total_hours.multiplied_by(Crypto::UnsignedBigInteger(60)));
    // 4. Set seconds to seconds + minutes × 60.
    auto total_seconds = Crypto::SignedBigInteger { seconds }.plus(total_minutes.multiplied_by(Crypto::UnsignedBigInteger(60)));
    // 5. Set milliseconds to milliseconds + seconds × 1000.
    auto total_milliseconds = Crypto::SignedBigInteger { milliseconds }.plus(total_seconds.multiplied_by(Crypto::UnsignedBigInteger(1000)));
    // 6. Set microseconds to microseconds + milliseconds × 1000.
    auto total_microseconds = Crypto::SignedBigInteger { microseconds }.plus(total_milliseconds.multiplied_by(Crypto::UnsignedBigInteger(1000)));
    // 7. Return nanoseconds + microseconds × 1000.
    return result_nanoseconds.plus(total_microseconds.multiplied_by(Crypto::UnsignedBigInteger(1000)));
}

// FIXME: This function does not exist in newer versions of the spec. It does not properly support as an example a roundingMode of 'ceil'
//        Update all callers of this function to spec as required.
// 7.5.18 BalanceDuration ( days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, largestUnit [ , relativeTo ] ), https://tc39.es/proposal-temporal/#sec-temporal-balanceduration
ThrowCompletionOr<TimeDurationRecord> balance_duration(VM& vm, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, Crypto::SignedBigInteger const& nanoseconds, StringView largest_unit, Object* relative_to)
{
    // 1. If relativeTo is not present, set relativeTo to undefined.

    // NOTE: If any of the inputs is not finite this will mean that we have infinities,
    //       so the duration will never be valid. Also
    if (!isfinite(days) || !isfinite(hours) || !isfinite(minutes) || !isfinite(seconds) || !isfinite(milliseconds) || !isfinite(microseconds))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDuration);

    Crypto::SignedBigInteger total_nanoseconds;
    // 2. If Type(relativeTo) is Object and relativeTo has an [[InitializedTemporalZonedDateTime]] internal slot, then
    if (relative_to && is<ZonedDateTime>(*relative_to)) {
        auto& relative_to_zoned_date_time = static_cast<ZonedDateTime&>(*relative_to);

        // a. Let endNs be ? AddZonedDateTime(relativeTo.[[Nanoseconds]], relativeTo.[[TimeZone]], relativeTo.[[Calendar]], 0, 0, 0, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
        auto* end_ns = TRY(add_zoned_date_time(vm, relative_to_zoned_date_time.nanoseconds(), &relative_to_zoned_date_time.time_zone(), relative_to_zoned_date_time.calendar(), 0, 0, 0, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds.to_double()));

        // b. Set nanoseconds to ℝ(endNs - relativeTo.[[Nanoseconds]]).
        total_nanoseconds = end_ns->big_integer().minus(relative_to_zoned_date_time.nanoseconds().big_integer());
    }
    // 3. Else,
    else {
        // a. Set nanoseconds to ! TotalDurationNanoseconds(days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, 0).
        total_nanoseconds = total_duration_nanoseconds(days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, 0);
    }

    // 4. If largestUnit is one of "year", "month", "week", or "day", then
    if (largest_unit.is_one_of("year"sv, "month"sv, "week"sv, "day"sv)) {
        // a. Let result be ? NanosecondsToDays(nanoseconds, relativeTo).
        auto result = TRY(nanoseconds_to_days(vm, total_nanoseconds, relative_to ?: js_undefined()));

        // b. Set days to result.[[Days]].
        days = result.days;

        // c. Set nanoseconds to result.[[Nanoseconds]].
        total_nanoseconds = move(result.nanoseconds);
    }
    // 5. Else,
    else {
        // a. Set days to 0.
        days = 0;
    }
    // 6. Set hours, minutes, seconds, milliseconds, and microseconds to 0.
    hours = 0;
    minutes = 0;
    seconds = 0;
    milliseconds = 0;
    microseconds = 0;

    // 7. If nanoseconds < 0, let sign be -1; else, let sign be 1.
    i8 sign = total_nanoseconds.is_negative() ? -1 : 1;

    // 8. Set nanoseconds to abs(nanoseconds).
    total_nanoseconds = Crypto::SignedBigInteger(total_nanoseconds.unsigned_value());
    auto result_nanoseconds = total_nanoseconds.to_double();

    // 9. If largestUnit is "year", "month", "week", "day", or "hour", then
    if (largest_unit.is_one_of("year"sv, "month"sv, "week"sv, "day"sv, "hour"sv)) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = total_nanoseconds.divided_by(Crypto::UnsignedBigInteger(1000));
        // b. Set nanoseconds to nanoseconds modulo 1000.
        result_nanoseconds = nanoseconds_division_result.remainder.to_double();
        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();
        // e. Set seconds to floor(milliseconds / 1000).
        auto milliseconds_division_result = microseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = milliseconds_division_result.remainder.to_double();
        // g. Set minutes to floor(seconds / 60).
        auto seconds_division_result = milliseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(60));
        // h. Set seconds to seconds modulo 60.
        seconds = seconds_division_result.remainder.to_double();
        // i. Set hours to floor(minutes / 60).
        auto minutes_division_result = seconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(60));
        hours = minutes_division_result.quotient.to_double();
        // j. Set minutes to minutes modulo 60.
        minutes = minutes_division_result.remainder.to_double();
    }
    // 10. Else if largestUnit is "minute", then
    else if (largest_unit == "minute"sv) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = total_nanoseconds.divided_by(Crypto::UnsignedBigInteger(1000));
        // b. Set nanoseconds to nanoseconds modulo 1000.
        result_nanoseconds = nanoseconds_division_result.remainder.to_double();
        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();
        // e. Set seconds to floor(milliseconds / 1000).
        auto milliseconds_division_result = microseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = milliseconds_division_result.remainder.to_double();
        // g. Set minutes to floor(seconds / 60).
        auto seconds_division_result = milliseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(60));
        minutes = seconds_division_result.quotient.to_double();
        // h. Set seconds to seconds modulo 60.
        seconds = seconds_division_result.remainder.to_double();
    }
    // 11. Else if largestUnit is "second", then
    else if (largest_unit == "second"sv) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = total_nanoseconds.divided_by(Crypto::UnsignedBigInteger(1000));
        // b. Set nanoseconds to nanoseconds modulo 1000.
        result_nanoseconds = nanoseconds_division_result.remainder.to_double();
        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();
        // e. Set seconds to floor(milliseconds / 1000).
        auto milliseconds_division_result = microseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        seconds = milliseconds_division_result.quotient.to_double();
        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = milliseconds_division_result.remainder.to_double();
    }
    // 12. Else if largestUnit is "millisecond", then
    else if (largest_unit == "millisecond"sv) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = total_nanoseconds.divided_by(Crypto::UnsignedBigInteger(1000));
        // b. Set nanoseconds to nanoseconds modulo 1000.
        result_nanoseconds = nanoseconds_division_result.remainder.to_double();
        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        milliseconds = microseconds_division_result.quotient.to_double();
        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();
    }
    // 13. Else if largestUnit is "microsecond", then
    else if (largest_unit == "microsecond"sv) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = total_nanoseconds.divided_by(Crypto::UnsignedBigInteger(1000));
        microseconds = nanoseconds_division_result.quotient.to_double();
        // b. Set nanoseconds to nanoseconds modulo 1000.
        result_nanoseconds = nanoseconds_division_result.remainder.to_double();
    }
    // 14. Else,
    else {
        // a. Assert: largestUnit is "nanosecond".
        VERIFY(largest_unit == "nanosecond"sv);
    }
    // 15. Return ? CreateTimeDurationRecord(days, hours × sign, minutes × sign, seconds × sign, milliseconds × sign, microseconds × sign, nanoseconds × sign).
    return create_time_duration_record(vm, days, hours * sign, minutes * sign, seconds * sign, milliseconds * sign, microseconds * sign, result_nanoseconds * sign);
}

// 7.5.18 BalancePossiblyInfiniteTimeDuration ( days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-balancepossiblyinfinitetimeduration
Variant<TimeDurationRecord, Overflow> balance_possibly_infinite_time_duration(VM& vm, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, Crypto::SignedBigInteger const& nanoseconds, StringView largest_unit)
{
    // 1. Set hours to hours + days × 24.
    hours += days * 24.;

    // 2. Set nanoseconds to TotalDurationNanoseconds(hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
    // FIXME: We shouldn't be passing through 0 days and 0 offset digit
    auto nanoseconds_bigint = total_duration_nanoseconds(0, hours, minutes, seconds, milliseconds, microseconds, Crypto::SignedBigInteger { nanoseconds }, 0);
    double result_nanoseconds = 0;

    // 3. Set days, hours, minutes, seconds, milliseconds, and microseconds to 0.
    days = 0;
    hours = 0;
    minutes = 0;
    seconds = 0;
    milliseconds = 0;
    microseconds = 0;

    // 4. If nanoseconds < 0, let sign be -1; else, let sign be 1.
    auto sign = nanoseconds_bigint.is_negative() ? -1 : 1;

    // 5. Set nanoseconds to abs(nanoseconds).
    auto total_nanoseconds = Crypto::SignedBigInteger { nanoseconds_bigint.unsigned_value() };

    // 6. If largestUnit is "year", "month", "week", or "day", then
    if (largest_unit.is_one_of("year"sv, "month"sv, "week"sv, "day"sv)) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = total_nanoseconds.divided_by(Crypto::UnsignedBigInteger(1000));
        // b. Set nanoseconds to nanoseconds modulo 1000.
        result_nanoseconds = nanoseconds_division_result.remainder.to_double();
        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();
        // e. Set seconds to floor(milliseconds / 1000).
        auto milliseconds_division_result = microseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = milliseconds_division_result.remainder.to_double();
        // g. Set minutes to floor(seconds / 60).
        auto seconds_division_result = milliseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(60));
        // h. Set seconds to seconds modulo 60.
        seconds = seconds_division_result.remainder.to_double();
        // i. Set hours to floor(minutes / 60).
        auto minutes_division_result = seconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(60));
        // j. Set minutes to minutes modulo 60.
        minutes = minutes_division_result.remainder.to_double();
        // k. Set days to floor(hours / 24).
        auto hours_division_result = minutes_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(24));
        days = hours_division_result.quotient.to_double();
        // l. Set hours to hours modulo 24.
        hours = hours_division_result.remainder.to_double();
    }
    // 7. Else if largestUnit is "hour", then
    else if (largest_unit == "hour"sv) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = total_nanoseconds.divided_by(Crypto::UnsignedBigInteger(1000));
        // b. Set nanoseconds to nanoseconds modulo 1000.
        result_nanoseconds = nanoseconds_division_result.remainder.to_double();
        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();
        // e. Set seconds to floor(milliseconds / 1000).
        auto milliseconds_division_result = microseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = milliseconds_division_result.remainder.to_double();
        // g. Set minutes to floor(seconds / 60).
        auto seconds_division_result = milliseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(60));
        // h. Set seconds to seconds modulo 60.
        seconds = seconds_division_result.remainder.to_double();
        // i. Set hours to floor(minutes / 60).
        auto minutes_division_result = seconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(60));
        hours = minutes_division_result.quotient.to_double();
        // j. Set minutes to minutes modulo 60.
        minutes = minutes_division_result.remainder.to_double();
    }
    // 8. Else if largestUnit is "minute", then
    else if (largest_unit == "minute"sv) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = total_nanoseconds.divided_by(Crypto::UnsignedBigInteger(1000));
        // b. Set nanoseconds to nanoseconds modulo 1000.
        result_nanoseconds = nanoseconds_division_result.remainder.to_double();
        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();
        // e. Set seconds to floor(milliseconds / 1000).
        auto milliseconds_division_result = microseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = milliseconds_division_result.remainder.to_double();
        // g. Set minutes to floor(seconds / 60).
        auto seconds_division_result = milliseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(60));
        minutes = seconds_division_result.quotient.to_double();
        // h. Set seconds to seconds modulo 60.
        seconds = seconds_division_result.remainder.to_double();
    }
    // 9. Else if largestUnit is "second", then
    else if (largest_unit == "second"sv) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = total_nanoseconds.divided_by(Crypto::UnsignedBigInteger(1000));
        // b. Set nanoseconds to nanoseconds modulo 1000.
        result_nanoseconds = nanoseconds_division_result.remainder.to_double();
        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();
        // e. Set seconds to floor(milliseconds / 1000).
        auto milliseconds_division_result = microseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        seconds = milliseconds_division_result.quotient.to_double();
        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = milliseconds_division_result.remainder.to_double();
    }
    // 10. Else if largestUnit is "millisecond", then
    else if (largest_unit == "millisecond"sv) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = total_nanoseconds.divided_by(Crypto::UnsignedBigInteger(1000));
        // b. Set nanoseconds to nanoseconds modulo 1000.
        result_nanoseconds = nanoseconds_division_result.remainder.to_double();
        // c. Set milliseconds to floor(microseconds / 1000).
        auto microseconds_division_result = nanoseconds_division_result.quotient.divided_by(Crypto::UnsignedBigInteger(1000));
        milliseconds = microseconds_division_result.quotient.to_double();
        // d. Set microseconds to microseconds modulo 1000.
        microseconds = microseconds_division_result.remainder.to_double();
    }
    // 11. Else if largestUnit is "microsecond", then
    else if (largest_unit == "microsecond"sv) {
        // a. Set microseconds to floor(nanoseconds / 1000).
        auto nanoseconds_division_result = total_nanoseconds.divided_by(Crypto::UnsignedBigInteger(1000));
        microseconds = nanoseconds_division_result.quotient.to_double();
        // b. Set nanoseconds to nanoseconds modulo 1000.
        result_nanoseconds = nanoseconds_division_result.remainder.to_double();
    }
    // 12. Else,
    else {
        // a. Assert: largestUnit is "nanosecond".
        VERIFY(largest_unit == "nanosecond"sv);
        result_nanoseconds = total_nanoseconds.to_double();
    }

    // 13. For each value v of « days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds », do
    for (double v : { days, hours, minutes, seconds, milliseconds, microseconds, microseconds, result_nanoseconds }) {
        // a. If 𝔽(v) is not finite, then
        if (!isfinite(v)) {
            // i. If sign = 1, then
            if (sign == 1) {
                // 1. Return positive overflow.
                return Overflow::Positive;
            }
            // ii. Else if sign = -1, then
            else {
                // 1. Return negative overflow.
                return Overflow::Negative;
            }
        }
    }

    // 14. Return ! CreateTimeDurationRecord(days × sign, hours × sign, minutes × sign, seconds × sign, milliseconds × sign, microseconds × sign, nanoseconds × sign).
    return MUST(create_time_duration_record(vm, days * sign, hours * sign, minutes * sign, seconds * sign, milliseconds * sign, microseconds * sign, result_nanoseconds * sign));
}

// 7.5.19 UnbalanceDurationRelative ( years, months, weeks, days, largestUnit, relativeTo ), https://tc39.es/proposal-temporal/#sec-temporal-unbalancedurationrelative
ThrowCompletionOr<DateDurationRecord> unbalance_duration_relative(VM& vm, double years, double months, double weeks, double days, StringView largest_unit, Value relative_to)
{
    auto& realm = *vm.current_realm();

    // 1. If largestUnit is "year", or years, months, weeks, and days are all 0, then
    if (largest_unit == "year"sv || (years == 0 && months == 0 && weeks == 0 && days == 0)) {
        // a. Return ! CreateDateDurationRecord(years, months, weeks, days).
        return create_date_duration_record(years, months, weeks, days);
    }

    // 2. Let sign be ! DurationSign(years, months, weeks, days, 0, 0, 0, 0, 0, 0).
    auto sign = duration_sign(years, months, weeks, days, 0, 0, 0, 0, 0, 0);

    // 3. Assert: sign ≠ 0.
    VERIFY(sign != 0);

    // 4. Let oneYear be ! CreateTemporalDuration(sign, 0, 0, 0, 0, 0, 0, 0, 0, 0).
    auto one_year = MUST(create_temporal_duration(vm, sign, 0, 0, 0, 0, 0, 0, 0, 0, 0));

    // 5. Let oneMonth be ! CreateTemporalDuration(0, sign, 0, 0, 0, 0, 0, 0, 0, 0).
    auto one_month = MUST(create_temporal_duration(vm, 0, sign, 0, 0, 0, 0, 0, 0, 0, 0));

    // 6. Let oneWeek be ! CreateTemporalDuration(0, 0, sign, 0, 0, 0, 0, 0, 0, 0).
    auto one_week = MUST(create_temporal_duration(vm, 0, 0, sign, 0, 0, 0, 0, 0, 0, 0));

    Object* calendar;

    // 7. If relativeTo is not undefined, then
    if (!relative_to.is_undefined()) {
        // a. Set relativeTo to ? ToTemporalDate(relativeTo).
        auto* relative_to_plain_date = TRY(to_temporal_date(vm, relative_to));
        relative_to = relative_to_plain_date;

        // b. Let calendar be relativeTo.[[Calendar]].
        calendar = &relative_to_plain_date->calendar();
    }
    // 8. Else,
    else {
        // a. Let calendar be undefined.
        calendar = nullptr;
    }

    // 9. If largestUnit is "month", then
    if (largest_unit == "month"sv) {
        // a. If calendar is undefined, then
        if (!calendar) {
            // i. Throw a RangeError exception.
            return vm.throw_completion<RangeError>(ErrorType::TemporalMissingStartingPoint, "months");
        }

        // b. Let dateAdd be ? GetMethod(calendar, "dateAdd").
        auto date_add = TRY(Value(calendar).get_method(vm, vm.names.dateAdd));

        // FIXME: This AO is out of date, this is no longer needed.
        // c. Let dateUntil be ? GetMethod(calendar, "dateUntil").

        // d. Repeat, while years ≠ 0,
        while (years != 0) {
            // i. Let newRelativeTo be ? CalendarDateAdd(calendar, relativeTo, oneYear, undefined, dateAdd).
            auto* new_relative_to = TRY(calendar_date_add(vm, *calendar, relative_to, *one_year, nullptr, date_add));

            // ii. Let untilOptions be OrdinaryObjectCreate(null).
            auto until_options = Object::create(realm, nullptr);

            // iii. Perform ! CreateDataPropertyOrThrow(untilOptions, "largestUnit", "month").
            MUST(until_options->create_data_property_or_throw(vm.names.largestUnit, PrimitiveString::create(vm, "month"_string)));

            // FIXME: AD-HOC calendar records use as this AO is not up to date with latest spec
            // iv. Let untilResult be ? CalendarDateUntil(calendar, relativeTo, newRelativeTo, untilOptions, dateUntil).
            auto calendar_record = TRY(create_calendar_methods_record(vm, NonnullGCPtr<Object> { *calendar }, { { CalendarMethod::DateAdd, CalendarMethod::DateUntil } }));
            auto until_result = TRY(calendar_date_until(vm, calendar_record, relative_to, new_relative_to, *until_options));

            // v. Let oneYearMonths be untilResult.[[Months]].
            auto one_year_months = until_result->months();

            // vi. Set relativeTo to newRelativeTo.
            relative_to = new_relative_to;

            // vii. Set years to years - sign.
            years -= sign;

            // viii. Set months to months + oneYearMonths.
            months += one_year_months;
        }
    }
    // 10. Else if largestUnit is "week", then
    else if (largest_unit == "week"sv) {
        // a. If calendar is undefined, then
        if (!calendar) {
            // i. Throw a RangeError exception.
            return vm.throw_completion<RangeError>(ErrorType::TemporalMissingStartingPoint, "weeks");
        }

        // b. Let dateAdd be ? GetMethod(calendar, "dateAdd").
        auto date_add = TRY(Value(calendar).get_method(vm, vm.names.dateAdd));

        // c. Repeat, while years ≠ 0,
        while (years != 0) {
            // i. Let moveResult be ? MoveRelativeDate(calendar, relativeTo, oneYear, dateAdd).
            auto move_result = TRY(move_relative_date(vm, *calendar, verify_cast<PlainDate>(relative_to.as_object()), *one_year, date_add));

            // ii. Set relativeTo to moveResult.[[RelativeTo]].
            relative_to = move_result.relative_to.cell();

            // iii. Set days to days + moveResult.[[Days]].
            days += move_result.days;

            // iv. Set years to years - sign.
            years -= sign;
        }

        // d. Repeat, while months ≠ 0,
        while (months != 0) {
            // i. Let moveResult be ? MoveRelativeDate(calendar, relativeTo, oneMonth, dateAdd).
            auto move_result = TRY(move_relative_date(vm, *calendar, verify_cast<PlainDate>(relative_to.as_object()), *one_month, date_add));

            // ii. Set relativeTo to moveResult.[[RelativeTo]].
            relative_to = move_result.relative_to.cell();

            // iii. Set days to days + moveResult.[[Days]].
            days += move_result.days;

            // iv. Set months to months - sign.
            months -= sign;
        }
    }
    // 11. Else,
    else {
        // a. If any of years, months, and weeks are not zero, then
        if (years != 0 || months != 0 || weeks != 0) {
            // i. If calendar is undefined, then
            if (!calendar) {
                // i. Throw a RangeError exception.
                return vm.throw_completion<RangeError>(ErrorType::TemporalMissingStartingPoint, "calendar units");
            }

            // ii. Let dateAdd be ? GetMethod(calendar, "dateAdd").
            auto date_add = TRY(Value(calendar).get_method(vm, vm.names.dateAdd));

            // iii. Repeat, while years ≠ 0,
            while (years != 0) {
                // 1. Let moveResult be ? MoveRelativeDate(calendar, relativeTo, oneYear, dateAdd).
                auto move_result = TRY(move_relative_date(vm, *calendar, verify_cast<PlainDate>(relative_to.as_object()), *one_year, date_add));

                // 2. Set relativeTo to moveResult.[[RelativeTo]].
                relative_to = move_result.relative_to.cell();

                // 3. Set days to days + moveResult.[[Days]].
                days += move_result.days;

                // 4. Set years to years - sign.
                years -= sign;
            }

            // iv. Repeat, while months ≠ 0,
            while (months != 0) {
                // 1. Let moveResult be ? MoveRelativeDate(calendar, relativeTo, oneMonth, dateAdd).
                auto move_result = TRY(move_relative_date(vm, *calendar, verify_cast<PlainDate>(relative_to.as_object()), *one_month, date_add));

                // 2. Set relativeTo to moveResult.[[RelativeTo]].
                relative_to = move_result.relative_to.cell();

                // 3. Set days to days +moveResult.[[Days]].
                days += move_result.days;

                // 4. Set months to months - sign.
                months -= sign;
            }

            // v. Repeat, while weeks ≠ 0,
            while (weeks != 0) {
                // 1. Let moveResult be ? MoveRelativeDate(calendar, relativeTo, oneWeek, dateAdd).
                auto move_result = TRY(move_relative_date(vm, *calendar, verify_cast<PlainDate>(relative_to.as_object()), *one_week, date_add));

                // 2. Set relativeTo to moveResult.[[RelativeTo]].
                relative_to = move_result.relative_to.cell();

                // 3. Set days to days + moveResult.[[Days]].
                days += move_result.days;

                // 4. Set weeks to weeks - sign.
                weeks -= sign;
            }
        }
    }

    // 12. Return ? CreateDateDurationRecord(years, months, weeks, days).
    return create_date_duration_record(vm, years, months, weeks, days);
}

// 7.5.20 BalanceDurationRelative ( years, months, weeks, days, largestUnit, relativeTo ), https://tc39.es/proposal-temporal/#sec-temporal-balancedurationrelative
ThrowCompletionOr<DateDurationRecord> balance_duration_relative(VM& vm, double years, double months, double weeks, double days, StringView largest_unit, Value relative_to_value)
{
    auto& realm = *vm.current_realm();

    // 1. If largestUnit is not one of "year", "month", or "week", or years, months, weeks, and days are all 0, then
    if (!largest_unit.is_one_of("year"sv, "month"sv, "week"sv) || (years == 0 && months == 0 && weeks == 0 && days == 0)) {
        // a. Return ! CreateDateDurationRecord(years, months, weeks, days).
        return create_date_duration_record(years, months, weeks, days);
    }

    // 2. If relativeTo is undefined, then
    if (relative_to_value.is_undefined()) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalMissingStartingPoint, "calendar units");
    }

    // 3. Let sign be ! DurationSign(years, months, weeks, days, 0, 0, 0, 0, 0, 0).
    auto sign = duration_sign(years, months, weeks, days, 0, 0, 0, 0, 0, 0);

    // 4. Assert: sign ≠ 0.
    VERIFY(sign != 0);

    // 5. Let oneYear be ! CreateTemporalDuration(sign, 0, 0, 0, 0, 0, 0, 0, 0, 0).
    auto one_year = MUST(create_temporal_duration(vm, sign, 0, 0, 0, 0, 0, 0, 0, 0, 0));

    // 6. Let oneMonth be ! CreateTemporalDuration(0, sign, 0, 0, 0, 0, 0, 0, 0, 0).
    auto one_month = MUST(create_temporal_duration(vm, 0, sign, 0, 0, 0, 0, 0, 0, 0, 0));

    // 7. Let oneWeek be ! CreateTemporalDuration(0, 0, sign, 0, 0, 0, 0, 0, 0, 0).
    auto one_week = MUST(create_temporal_duration(vm, 0, 0, sign, 0, 0, 0, 0, 0, 0, 0));

    // 8. Set relativeTo to ? ToTemporalDate(relativeTo).
    auto* relative_to = TRY(to_temporal_date(vm, relative_to_value));

    // 9. Let calendar be relativeTo.[[Calendar]].
    auto& calendar = relative_to->calendar();

    // 10. If largestUnit is "year", then
    if (largest_unit == "year"sv) {
        // a. Let dateAdd be ? GetMethod(calendar, "dateAdd").
        auto date_add = TRY(Value(&calendar).get_method(vm, vm.names.dateAdd));

        // b. Let moveResult be ? MoveRelativeDate(calendar, relativeTo, oneYear, dateAdd).
        auto move_result = TRY(move_relative_date(vm, calendar, *relative_to, *one_year, date_add));

        // c. Let newRelativeTo be moveResult.[[RelativeTo]].
        auto* new_relative_to = move_result.relative_to.cell();

        // d. Let oneYearDays be moveResult.[[Days]].
        auto one_year_days = move_result.days;

        // e. Repeat, while abs(days) ≥ abs(oneYearDays),
        while (fabs(days) >= fabs(one_year_days)) {
            // i. Set days to days - oneYearDays.
            days -= one_year_days;

            // ii. Set years to years + sign.
            years += sign;

            // iii. Set relativeTo to newRelativeTo.
            relative_to = new_relative_to;

            // iv. Set moveResult to ? MoveRelativeDate(calendar, relativeTo, oneYear, dateAdd).
            move_result = TRY(move_relative_date(vm, calendar, *relative_to, *one_year, date_add));

            // v. Set newRelativeTo to moveResult.[[RelativeTo]].
            new_relative_to = move_result.relative_to.cell();

            // vi. Set oneYearDays to moveResult.[[Days]].
            one_year_days = move_result.days;
        }

        // f. Set moveResult to ? MoveRelativeDate(calendar, relativeTo, oneMonth, dateAdd).
        move_result = TRY(move_relative_date(vm, calendar, *relative_to, *one_month, date_add));

        // g. Set newRelativeTo to moveResult.[[RelativeTo]].
        new_relative_to = move_result.relative_to.cell();

        // h. Let oneMonthDays be moveResult.[[Days]].
        auto one_month_days = move_result.days;

        // i. Repeat, while abs(days) ≥ abs(oneMonthDays),
        while (fabs(days) >= fabs(one_month_days)) {
            // i. Set days to days - oneMonthDays.
            days -= one_month_days;

            // ii. Set months to months + sign.
            months += sign;

            // iii. Set relativeTo to newRelativeTo.
            relative_to = new_relative_to;

            // iv. Set moveResult to ? MoveRelativeDate(calendar, relativeTo, oneMonth, dateAdd).
            move_result = TRY(move_relative_date(vm, calendar, *relative_to, *one_month, date_add));

            // v. Set newRelativeTo to moveResult.[[RelativeTo]].
            new_relative_to = move_result.relative_to.cell();

            // vi. Set oneMonthDays to moveResult.[[Days]].
            one_month_days = move_result.days;
        }

        // j. Set newRelativeTo to ? CalendarDateAdd(calendar, relativeTo, oneYear, undefined, dateAdd).
        new_relative_to = TRY(calendar_date_add(vm, calendar, relative_to, *one_year, nullptr, date_add));

        // FIXME: This AO is out of date, this is no longer needed.
        // k. Let dateUntil be ? GetMethod(calendar, "dateUntil").

        // l. Let untilOptions be OrdinaryObjectCreate(null).
        auto until_options = Object::create(realm, nullptr);

        // m. Perform ! CreateDataPropertyOrThrow(untilOptions, "largestUnit", "month").
        MUST(until_options->create_data_property_or_throw(vm.names.largestUnit, PrimitiveString::create(vm, "month"_string)));

        // FIXME: AD-HOC calendar records use as this AO is not up to date with latest spec
        // n. Let untilResult be ? CalendarDateUntil(calendar, relativeTo, newRelativeTo, untilOptions, dateUntil).
        auto calendar_record = TRY(create_calendar_methods_record(vm, NonnullGCPtr<Object> { calendar }, { { CalendarMethod::DateAdd, CalendarMethod::DateUntil } }));
        auto until_result = TRY(calendar_date_until(vm, calendar_record, relative_to, new_relative_to, *until_options));

        // o. Let oneYearMonths be untilResult.[[Months]].
        auto one_year_months = until_result->months();

        // p. Repeat, while abs(months) ≥ abs(oneYearMonths),
        while (fabs(months) >= fabs(one_year_months)) {
            // i. Set months to months - oneYearMonths.
            months -= one_year_months;

            // ii. Set years to years + sign.
            years += sign;

            // iii. Set relativeTo to newRelativeTo.
            relative_to = new_relative_to;

            // iv. Set newRelativeTo to ? CalendarDateAdd(calendar, relativeTo, oneYear, undefined, dateAdd).
            new_relative_to = TRY(calendar_date_add(vm, calendar, relative_to, *one_year, nullptr, date_add));

            // v. Set untilOptions to OrdinaryObjectCreate(null).
            until_options = Object::create(realm, nullptr);

            // vi. Perform ! CreateDataPropertyOrThrow(untilOptions, "largestUnit", "month").
            MUST(until_options->create_data_property_or_throw(vm.names.largestUnit, PrimitiveString::create(vm, "month"_string)));

            // vii. Set untilResult to ? CalendarDateUntil(calendar, relativeTo, newRelativeTo, untilOptions, dateUntil).
            until_result = TRY(calendar_date_until(vm, calendar_record, relative_to, new_relative_to, *until_options));

            // viii. Set oneYearMonths to untilResult.[[Months]].
            one_year_months = until_result->months();
        }
    }
    // 11. Else if largestUnit is "month", then
    else if (largest_unit == "month"sv) {
        // a. Let dateAdd be ? GetMethod(calendar, "dateAdd").
        auto date_add = TRY(Value(&calendar).get_method(vm, vm.names.dateAdd));

        // b. Let moveResult be ? MoveRelativeDate(calendar, relativeTo, oneMonth, dateAdd).
        auto move_result = TRY(move_relative_date(vm, calendar, *relative_to, *one_month, date_add));

        // c. Let newRelativeTo be moveResult.[[RelativeTo]].
        auto* new_relative_to = move_result.relative_to.cell();

        // d. Let oneMonthDays be moveResult.[[Days]].
        auto one_month_days = move_result.days;

        // e. Repeat, while abs(days) ≥ abs(oneMonthDays),
        while (fabs(days) >= fabs(one_month_days)) {
            // i. Set days to days - oneMonthDays.
            days -= one_month_days;

            // ii. Set months to months + sign.
            months += sign;

            // iii. Set relativeTo to newRelativeTo.
            relative_to = new_relative_to;

            // iv. Set moveResult to ? MoveRelativeDate(calendar, relativeTo, oneMonth, dateAdd).
            move_result = TRY(move_relative_date(vm, calendar, *relative_to, *one_month, date_add));

            // v. Set newRelativeTo to moveResult.[[RelativeTo]].
            new_relative_to = move_result.relative_to.cell();

            // vi. Set oneMonthDays to moveResult.[[Days]].
            one_month_days = move_result.days;
        }
    }
    // 12. Else,
    else {
        // a. Assert: largestUnit is "week".
        VERIFY(largest_unit == "week"sv);

        // b. Let dateAdd be ? GetMethod(calendar, "dateAdd").
        auto date_add = TRY(Value(&calendar).get_method(vm, vm.names.dateAdd));

        // c. Let moveResult be ? MoveRelativeDate(calendar, relativeTo, oneWeek, dateAdd).
        auto move_result = TRY(move_relative_date(vm, calendar, *relative_to, *one_week, date_add));

        // d. Let newRelativeTo be moveResult.[[RelativeTo]].
        auto* new_relative_to = move_result.relative_to.cell();

        // e. Let oneWeekDays be moveResult.[[Days]].
        auto one_week_days = move_result.days;

        // f. Repeat, while abs(days) ≥ abs(oneWeekDays),
        while (fabs(days) >= fabs(one_week_days)) {
            // i. Set days to days - oneWeekDays.
            days -= one_week_days;

            // ii. Set weeks to weeks + sign.
            weeks += sign;

            // iii. Set relativeTo to newRelativeTo.
            relative_to = new_relative_to;

            // iv. Set moveResult to ? MoveRelativeDate(calendar, relativeTo, oneWeek, dateAdd).
            move_result = TRY(move_relative_date(vm, calendar, *relative_to, *one_week, date_add));

            // v. Set newRelativeTo to moveResult.[[RelativeTo]].
            new_relative_to = move_result.relative_to.cell();

            // vi. Set oneWeekDays to moveResult.[[Days]].
            one_week_days = move_result.days;
        }
    }

    // 13. Return ! CreateDateDurationRecord(years, months, weeks, days).
    return create_date_duration_record(years, months, weeks, days);
}

// 7.5.21 AddDuration ( y1, mon1, w1, d1, h1, min1, s1, ms1, mus1, ns1, y2, mon2, w2, d2, h2, min2, s2, ms2, mus2, ns2, relativeTo ), https://tc39.es/proposal-temporal/#sec-temporal-addduration
ThrowCompletionOr<DurationRecord> add_duration(VM& vm, double years1, double months1, double weeks1, double days1, double hours1, double minutes1, double seconds1, double milliseconds1, double microseconds1, double nanoseconds1, double years2, double months2, double weeks2, double days2, double hours2, double minutes2, double seconds2, double milliseconds2, double microseconds2, double nanoseconds2, Value relative_to_value)
{
    auto& realm = *vm.current_realm();

    VERIFY(all_of(AK::Array { years1, months1, weeks1, days1, hours1, minutes1, seconds1, milliseconds1, microseconds1, nanoseconds1, years2, months2, weeks2, days2, hours2, minutes2, seconds2, milliseconds2, microseconds2, nanoseconds2 }, [](auto value) { return value == trunc(value); }));

    // 1. Let largestUnit1 be ! DefaultTemporalLargestUnit(y1, mon1, w1, d1, h1, min1, s1, ms1, mus1).
    auto largest_unit1 = default_temporal_largest_unit(years1, months1, weeks1, days1, hours1, minutes1, seconds1, milliseconds1, microseconds1);

    // 2. Let largestUnit2 be ! DefaultTemporalLargestUnit(y2, mon2, w2, d2, h2, min2, s2, ms2, mus2).
    auto largest_unit2 = default_temporal_largest_unit(years2, months2, weeks2, days2, hours2, minutes2, seconds2, milliseconds2, microseconds2);

    // 3. Let largestUnit be ! LargerOfTwoTemporalUnits(largestUnit1, largestUnit2).
    auto largest_unit = larger_of_two_temporal_units(largest_unit1, largest_unit2);

    // 4. If relativeTo is undefined, then
    if (relative_to_value.is_undefined()) {
        // a. If largestUnit is one of "year", "month", or "week", then
        if (largest_unit.is_one_of("year"sv, "month"sv, "week"sv)) {
            // i. Throw a RangeError exception.
            return vm.throw_completion<RangeError>(ErrorType::TemporalMissingStartingPoint, "year, month or week");
        }

        // b. Let result be ? BalanceDuration(d1 + d2, h1 + h2, min1 + min2, s1 + s2, ms1 + ms2, mus1 + mus2, ns1 + ns2, largestUnit).
        // NOTE: Nanoseconds is the only one that can overflow the safe integer range of a double
        //       so we have to check for that case.
        Crypto::SignedBigInteger sum_of_nano_seconds;
        if (fabs(nanoseconds1 + nanoseconds2) >= MAX_ARRAY_LIKE_INDEX)
            sum_of_nano_seconds = Crypto::SignedBigInteger { nanoseconds1 }.plus(Crypto::SignedBigInteger { nanoseconds2 });
        else
            sum_of_nano_seconds = Crypto::SignedBigInteger { nanoseconds1 + nanoseconds2 };

        auto result = TRY(balance_duration(vm, days1 + days2, hours1 + hours2, minutes1 + minutes2, seconds1 + seconds2, milliseconds1 + milliseconds2, microseconds1 + microseconds2, sum_of_nano_seconds, largest_unit));

        // c. Return ! CreateDurationRecord(0, 0, 0, result.[[Days]], result.[[Hours]], result.[[Minutes]], result.[[Seconds]], result.[[Milliseconds]], result.[[Microseconds]], result.[[Nanoseconds]]).
        return MUST(create_duration_record(vm, 0, 0, 0, result.days, result.hours, result.minutes, result.seconds, result.milliseconds, result.microseconds, result.nanoseconds));
    }

    // 5. If relativeTo has an [[InitializedTemporalDate]] internal slot, then
    if (is<PlainDate>(relative_to_value.as_object())) {
        auto& relative_to = static_cast<PlainDate&>(relative_to_value.as_object());

        // a. Let calendar be relativeTo.[[Calendar]].
        auto& calendar = relative_to.calendar();

        // b. Let dateDuration1 be ! CreateTemporalDuration(y1, mon1, w1, d1, 0, 0, 0, 0, 0, 0).
        auto date_duration1 = MUST(create_temporal_duration(vm, years1, months1, weeks1, days1, 0, 0, 0, 0, 0, 0));

        // c. Let dateDuration2 be ! CreateTemporalDuration(y2, mon2, w2, d2, 0, 0, 0, 0, 0, 0).
        auto date_duration2 = MUST(create_temporal_duration(vm, years2, months2, weeks2, days2, 0, 0, 0, 0, 0, 0));

        // d. Let dateAdd be ? GetMethod(calendar, "dateAdd").
        auto date_add = TRY(Value(&calendar).get_method(vm, vm.names.dateAdd));

        // e. Let intermediate be ? CalendarDateAdd(calendar, relativeTo, dateDuration1, undefined, dateAdd).
        auto* intermediate = TRY(calendar_date_add(vm, calendar, &relative_to, *date_duration1, nullptr, date_add));

        // f. Let end be ? CalendarDateAdd(calendar, intermediate, dateDuration2, undefined, dateAdd).
        auto* end = TRY(calendar_date_add(vm, calendar, intermediate, *date_duration2, nullptr, date_add));

        // g. Let dateLargestUnit be ! LargerOfTwoTemporalUnits("day", largestUnit).
        auto date_largest_unit = larger_of_two_temporal_units("day"sv, largest_unit);

        // h. Let differenceOptions be OrdinaryObjectCreate(null).
        auto difference_options = Object::create(realm, nullptr);

        // i. Perform ! CreateDataPropertyOrThrow(differenceOptions, "largestUnit", dateLargestUnit).
        MUST(difference_options->create_data_property_or_throw(vm.names.largestUnit, PrimitiveString::create(vm, date_largest_unit)));

        // j. Let dateDifference be ? CalendarDateUntil(calendar, relativeTo, end, differenceOptions).
        // FIXME: AD-HOC calendar records use as this AO is not up to date with latest spec
        auto calendar_record = TRY(create_calendar_methods_record(vm, NonnullGCPtr<Object> { calendar }, { { CalendarMethod::DateAdd, CalendarMethod::DateUntil } }));
        auto date_difference = TRY(calendar_date_until(vm, calendar_record, &relative_to, end, *difference_options));

        // k. Let result be ? BalanceDuration(dateDifference.[[Days]], h1 + h2, min1 + min2, s1 + s2, ms1 + ms2, mus1 + mus2, ns1 + ns2, largestUnit).
        // NOTE: Nanoseconds is the only one that can overflow the safe integer range of a double
        //       so we have to check for that case.
        Crypto::SignedBigInteger sum_of_nano_seconds;
        if (fabs(nanoseconds1 + nanoseconds2) >= MAX_ARRAY_LIKE_INDEX)
            sum_of_nano_seconds = Crypto::SignedBigInteger { nanoseconds1 }.plus(Crypto::SignedBigInteger { nanoseconds2 });
        else
            sum_of_nano_seconds = Crypto::SignedBigInteger { nanoseconds1 + nanoseconds2 };

        auto result = TRY(balance_duration(vm, date_difference->days(), hours1 + hours2, minutes1 + minutes2, seconds1 + seconds2, milliseconds1 + milliseconds2, microseconds1 + microseconds2, sum_of_nano_seconds, largest_unit));

        // l. Return ? CreateDurationRecord(dateDifference.[[Years]], dateDifference.[[Months]], dateDifference.[[Weeks]], result.[[Days]], result.[[Hours]], result.[[Minutes]], result.[[Seconds]], result.[[Milliseconds]], result.[[Microseconds]], result.[[Nanoseconds]]).
        return MUST(create_duration_record(vm, date_difference->years(), date_difference->months(), date_difference->weeks(), result.days, result.hours, result.minutes, result.seconds, result.milliseconds, result.microseconds, result.nanoseconds));
    }

    // 6. Assert: relativeTo has an [[InitializedTemporalZonedDateTime]] internal slot.
    auto& relative_to = verify_cast<ZonedDateTime>(relative_to_value.as_object());

    // 7. Let timeZone be relativeTo.[[TimeZone]].
    auto& time_zone = relative_to.time_zone();

    // 8. Let calendar be relativeTo.[[Calendar]].
    auto& calendar = relative_to.calendar();

    // 9. Let intermediateNs be ? AddZonedDateTime(relativeTo.[[Nanoseconds]], timeZone, calendar, y1, mon1, w1, d1, h1, min1, s1, ms1, mus1, ns1).
    auto* intermediate_ns = TRY(add_zoned_date_time(vm, relative_to.nanoseconds(), &time_zone, calendar, years1, months1, weeks1, days1, hours1, minutes1, seconds1, milliseconds1, microseconds1, nanoseconds1));

    // 10. Let endNs be ? AddZonedDateTime(intermediateNs, timeZone, calendar, y2, mon2, w2, d2, h2, min2, s2, ms2, mus2, ns2).
    auto* end_ns = TRY(add_zoned_date_time(vm, *intermediate_ns, &time_zone, calendar, years2, months2, weeks2, days2, hours2, minutes2, seconds2, milliseconds2, microseconds2, nanoseconds2));

    // 11. If largestUnit is not one of "year", "month", "week", or "day", then
    if (!largest_unit.is_one_of("year"sv, "month"sv, "week"sv, "day"sv)) {
        // a. Let result be DifferenceInstant(zonedRelativeTo.[[Nanoseconds]], endNs, 1, "nanosecond", largestUnit, "halfExpand").
        auto result = difference_instant(vm, relative_to.nanoseconds(), *end_ns, 1, "nanosecond"sv, largest_unit, "halfExpand"sv);

        // b. Return ! CreateDurationRecord(0, 0, 0, 0, result.[[Hours]], result.[[Minutes]], result.[[Seconds]], result.[[Milliseconds]], result.[[Microseconds]], result.[[Nanoseconds]]).
        return create_duration_record(0, 0, 0, 0, result.hours, result.minutes, result.seconds, result.milliseconds, result.microseconds, result.nanoseconds);
    }

    // 12. Return ? DifferenceZonedDateTime(relativeTo.[[Nanoseconds]], endNs, timeZone, calendar, largestUnit, OrdinaryObjectCreate(null)).
    return difference_zoned_date_time(vm, relative_to.nanoseconds(), *end_ns, time_zone, calendar, largest_unit, *Object::create(realm, nullptr));
}

// 7.5.23 MoveRelativeDate ( calendar, relativeTo, duration, dateAdd ), https://tc39.es/proposal-temporal/#sec-temporal-moverelativedate
ThrowCompletionOr<MoveRelativeDateResult> move_relative_date(VM& vm, Object& calendar, PlainDate& relative_to, Duration& duration, FunctionObject* date_add)
{
    // 1. Let newDate be ? CalendarDateAdd(calendar, relativeTo, duration, undefined, dateAdd)
    auto* new_date = TRY(calendar_date_add(vm, calendar, &relative_to, duration, nullptr, date_add));

    // 2. Let days be DaysUntil(relativeTo, newDate).
    auto days = days_until(relative_to, *new_date);

    // 3. Return the Record { [[RelativeTo]]: newDate, [[Days]]: days }.
    return MoveRelativeDateResult { .relative_to = make_handle(new_date), .days = days };
}

// 7.5.24 MoveRelativeZonedDateTime ( zonedDateTime, years, months, weeks, days ), https://tc39.es/proposal-temporal/#sec-temporal-moverelativezoneddatetime
ThrowCompletionOr<ZonedDateTime*> move_relative_zoned_date_time(VM& vm, ZonedDateTime& zoned_date_time, double years, double months, double weeks, double days)
{
    // 1. Let intermediateNs be ? AddZonedDateTime(zonedDateTime.[[Nanoseconds]], zonedDateTime.[[TimeZone]], zonedDateTime.[[Calendar]], years, months, weeks, days, 0, 0, 0, 0, 0, 0).
    auto* intermediate_ns = TRY(add_zoned_date_time(vm, zoned_date_time.nanoseconds(), &zoned_date_time.time_zone(), zoned_date_time.calendar(), years, months, weeks, days, 0, 0, 0, 0, 0, 0));

    // 2. Return ! CreateTemporalZonedDateTime(intermediateNs, zonedDateTime.[[TimeZone]], zonedDateTime.[[Calendar]]).
    return MUST(create_temporal_zoned_date_time(vm, *intermediate_ns, zoned_date_time.time_zone(), zoned_date_time.calendar()));
}

// 7.5.27 RoundDuration ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, increment, unit, roundingMode [ , plainRelativeTo [ , calendarRec [ , zonedRelativeTo [ , timeZoneRec [ , precalculatedPlainDateTime ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal-roundduration
// FIXME: Support calendarRec, zonedRelativeTo, timeZoneRec, precalculatedPlainDateTime
ThrowCompletionOr<RoundedDuration> round_duration(VM& vm, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, u32 increment, StringView unit, StringView rounding_mode, Object* plain_relative_to_object, Optional<CalendarMethods> const& calendar_record)
{
    auto& realm = *vm.current_realm();

    Object* calendar = nullptr; // FIXME: Should come from calendarRec

    double fractional_seconds = 0;
    double fractional_days = 0;

    // FIXME: 1. Assert: If either of plainRelativeTo or zonedRelativeTo are present and not undefined, calendarRec is not undefined.
    if (plain_relative_to_object)
        VERIFY(calendar_record.has_value());

    // FIXME: 2. Assert: If zonedRelativeTo is present and not undefined, timeZoneRec is not undefined.

    // 3. If plainRelativeTo is not present, set plainRelativeTo to undefined.
    // NOTE: `plain_relative_to_object` and `plain_relative_to` in the various code paths below are all the same as far as the
    //        spec is concerned, but the latter is more strictly typed for convenience.
    PlainDate* plain_relative_to = nullptr;

    // FIXME: 4. If zonedRelativeTo is not present, set zonedRelativeTo to undefined.
    ZonedDateTime* zoned_relative_to = nullptr;

    // FIXME: 5. If precalculatedPlainDateTime is not present, set precalculatedPlainDateTime to undefined.

    // FIXME: assuming "smallestUnit" as the option name here leads to confusing error messages in some cases:
    //        > new Temporal.Duration().total({ unit: "month" })
    //        Uncaught exception: [RangeError] month is not a valid value for option smallestUnit
    // 6. If unit is "year", "month", or "week", then
    if (unit.is_one_of("year"sv, "month"sv, "week"sv)) {
        // a. If plainRelativeTo is undefined, throw a RangeError exception.
        if (!plain_relative_to_object)
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, unit, "smallestUnit"sv);

        // b. Assert: CalendarMethodsRecordHasLookedUp(calendarRec, dateAdd) is true.
        VERIFY(calendar_methods_record_has_looked_up(calendar_record.value(), CalendarMethod::DateAdd));

        // c. Assert: CalendarMethodsRecordHasLookedUp(calendarRec, dateUntil) is true.
        VERIFY(calendar_methods_record_has_looked_up(calendar_record.value(), CalendarMethod::DateAdd));
    }

    // FIXME: AD-HOC, should be coming from arguments given to this function.
    if (plain_relative_to_object) {
        if (is<ZonedDateTime>(plain_relative_to_object)) {
            auto* relative_to_zoned_date_time = static_cast<ZonedDateTime*>(plain_relative_to_object);
            zoned_relative_to = relative_to_zoned_date_time;
            plain_relative_to = TRY(to_temporal_date(vm, plain_relative_to_object));
        } else {
            VERIFY(is<PlainDate>(plain_relative_to_object));
            plain_relative_to = verify_cast<PlainDate>(plain_relative_to_object);
        }
        calendar = &plain_relative_to->calendar();
    }

    // 7. If unit is one of "year", "month", "week", or "day", then
    if (unit.is_one_of("year"sv, "month"sv, "week"sv, "day"sv)) {

        // a. Let nanoseconds be TotalDurationNanoseconds(hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
        // FIXME: We shouldn't be passing through 0 days and 0 offset digit
        auto nanoseconds_bigint = total_duration_nanoseconds(0, hours, minutes, seconds, milliseconds, microseconds, Crypto::SignedBigInteger { nanoseconds }, 0);

        // b. If zonedRelativeTo is not undefined, then
        if (zoned_relative_to) {
            // i. Let intermediate be ? MoveRelativeZonedDateTime(zonedRelativeTo, calendarRec, timeZoneRec, years, months, weeks, days, precalculatedPlainDateTime).
            // FIXME: Pass through calendarRecord, timeZoneRec and precalculatedPlainDateTime
            auto* intermediate = TRY(move_relative_zoned_date_time(vm, *zoned_relative_to, years, months, weeks, days));

            // ii. Let result be ? NanosecondsToDays(nanoseconds, intermediate, timeZoneRec).
            // FIXME: Pass through timeZoneRec
            auto result = TRY(nanoseconds_to_days(vm, nanoseconds_bigint, intermediate));

            // iii. Let fractionalDays be days + result.[[Days]] + result.[[Nanoseconds]] / result.[[DayLength]].
            auto nanoseconds_division_result = result.nanoseconds.divided_by(Crypto::UnsignedBigInteger { result.day_length });
            fractional_days = days + result.days + nanoseconds_division_result.quotient.to_double() + nanoseconds_division_result.remainder.to_double() / result.day_length;
        }
        // c. Else,
        else {
            // i. Let fractionalDays be days + nanoseconds / nsPerDay.
            auto nanoseconds_division_result = nanoseconds_bigint.divided_by(ns_per_day_bigint);
            fractional_days = days + nanoseconds_division_result.quotient.to_double() + (nanoseconds_division_result.remainder.to_double() / ns_per_day);
        }

        // d. Set days, hours, minutes, seconds, milliseconds, microseconds, and nanoseconds to 0.
        days = 0;
        hours = 0;
        minutes = 0;
        seconds = 0;
        milliseconds = 0;
        microseconds = 0;
        nanoseconds = 0;

        // e. Assert: fractionalSeconds is not used below.
    }
    // 8. Else,
    else {
        // a. Let fractionalSeconds be nanoseconds × 10^-9 + microseconds × 10^-6 + milliseconds × 10^-3 + seconds.
        fractional_seconds = nanoseconds * 0.000000001 + microseconds * 0.000001 + milliseconds * 0.001 + seconds;

        // b. Assert: fractionalDays is not used below.
    }

    // 9. Let total be unset.
    double total = 0;

    // 10. If unit is "year", then
    if (unit == "year"sv) {
        VERIFY(plain_relative_to);

        // a. Let yearsDuration be ! CreateTemporalDuration(years, 0, 0, 0, 0, 0, 0, 0, 0, 0).
        auto years_duration = MUST(create_temporal_duration(vm, years, 0, 0, 0, 0, 0, 0, 0, 0, 0));

        // FIXME: b. Let yearsLater be ? AddDate(calendarRec, plainRelativeTo, yearsDuration).
        auto date_add = TRY(Value(calendar).get_method(vm, vm.names.dateAdd));
        auto* years_later = TRY(calendar_date_add(vm, *calendar, plain_relative_to, *years_duration, nullptr, date_add));

        // c. Let yearsMonthsWeeks be ! CreateTemporalDuration(years, months, weeks, 0, 0, 0, 0, 0, 0, 0).
        auto years_months_weeks = MUST(create_temporal_duration(vm, years, months, weeks, 0, 0, 0, 0, 0, 0, 0));

        // FIXME: d. Let yearsMonthsWeeksLater be ? AddDate(calendarRec, plainRelativeTo, yearsMonthsWeeks).
        auto* years_months_weeks_later = TRY(calendar_date_add(vm, *calendar, plain_relative_to, *years_months_weeks, nullptr, date_add));

        // e. Let monthsWeeksInDays be DaysUntil(yearsLater, yearsMonthsWeeksLater).
        auto months_weeks_in_days = days_until(*years_later, *years_months_weeks_later);

        // f. Set plainRelativeTo to yearsLater.
        plain_relative_to = years_later;

        // g. Set fractionalDays to fractionalDays + monthsWeeksInDays.
        fractional_days = fractional_days + months_weeks_in_days;

        // h. Let isoResult be ! AddISODate(plainRelativeTo.[[ISOYear]]. plainRelativeTo.[[ISOMonth]], plainRelativeTo.[[ISODay]], 0, 0, 0, truncate(fractionalDays), "constrain").
        auto iso_result = MUST(add_iso_date(vm, plain_relative_to->iso_year(), plain_relative_to->iso_month(), plain_relative_to->iso_day(), 0, 0, 0, trunc(fractional_days), "constrain"sv));

        // i. Let wholeDaysLater be ? CreateTemporalDate(isoResult.[[Year]], isoResult.[[Month]], isoResult.[[Day]], calendarRec.[[Receiver]]).
        // FIXME: Pass through receiver from calendarRec
        auto* whole_days_later = TRY(create_temporal_date(vm, iso_result.year, iso_result.month, iso_result.day, *calendar));

        // j. Let untilOptions be OrdinaryObjectCreate(null).
        auto until_options = Object::create(realm, nullptr);

        // k. Perform ! CreateDataPropertyOrThrow(untilOptions, "largestUnit", "year").
        MUST(until_options->create_data_property_or_throw(vm.names.largestUnit, PrimitiveString::create(vm, "year"_string)));

        // l. Let timePassed be ? DifferenceDate(calendarRec, plainRelativeTo, wholeDaysLater, untilOptions).
        auto time_passed = TRY(difference_date(vm, calendar_record.value(), *plain_relative_to, *whole_days_later, *until_options));

        // m. Let yearsPassed be timePassed.[[Years]].
        auto years_passed = time_passed->years();

        // n. Set years to years + yearsPassed.
        years += years_passed;

        // o. Let yearsDuration be ! CreateTemporalDuration(yearsPassed, 0, 0, 0, 0, 0, 0, 0, 0, 0).
        years_duration = MUST(create_temporal_duration(vm, years_passed, 0, 0, 0, 0, 0, 0, 0, 0, 0));

        // p. Let moveResult be ? MoveRelativeDate(calendarRec, plainRelativeTo, yearsDuration).
        // FIXME: Pass through calendarRec instead of date_add
        auto move_result = TRY(move_relative_date(vm, *calendar, *plain_relative_to, *years_duration, date_add));

        // q. Set plainRelativeTo to moveResult.[[RelativeTo]].
        auto plain_relative_to = move_result.relative_to;

        // r. Let daysPassed be moveResult.[[Days]].
        auto days_passed = move_result.days;

        // s. Set fractionalDays to fractionalDays - daysPassed.
        fractional_days -= days_passed;

        // t. If fractionalDays < 0, let sign be -1; else, let sign be 1.
        auto sign = fractional_days < 0 ? -1 : 1;

        // u. Let oneYear be ! CreateTemporalDuration(sign, 0, 0, 0, 0, 0, 0, 0, 0, 0).
        auto one_year = MUST(create_temporal_duration(vm, sign, 0, 0, 0, 0, 0, 0, 0, 0, 0));

        // v. Set moveResult to ? MoveRelativeDate(calendarRec, plainRelativeTo, oneYear).
        // FIXME:: pass through calendarRec
        move_result = TRY(move_relative_date(vm, *calendar, *plain_relative_to, *one_year, date_add));

        // w. Let oneYearDays be moveResult.[[Days]].
        auto one_year_days = move_result.days;

        // x. If oneYearDays = 0, throw a RangeError exception.
        if (one_year_days == 0)
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, "dateAdd", "result implying a year is zero days long");

        // y. Let fractionalYears be years + fractionalDays / abs(oneYearDays).
        auto fractional_years = years + fractional_days / fabs(one_year_days);

        // z. Set years to RoundNumberToIncrement(fractionalYears, increment, roundingMode).
        years = round_number_to_increment(fractional_years, increment, rounding_mode);

        // aa. Set total to fractionalYears.
        total = fractional_years;

        // ab. Set months and weeks to 0.
        months = 0;
        weeks = 0;
    }
    // 10. Else if unit is "month", then
    else if (unit == "month"sv) {
        VERIFY(plain_relative_to);

        // a. Let yearsMonths be ! CreateTemporalDuration(years, months, 0, 0, 0, 0, 0, 0, 0, 0).
        auto years_months = MUST(create_temporal_duration(vm, years, months, 0, 0, 0, 0, 0, 0, 0, 0));

        // FIXME: b. Let yearsMonthsLater be ? AddDate(calendarRec, plainRelativeTo, yearsMonths).
        auto date_add = TRY(Value(calendar).get_method(vm, vm.names.dateAdd));
        auto* years_months_later = TRY(calendar_date_add(vm, *calendar, plain_relative_to, *years_months, nullptr, date_add));

        // c. Let yearsMonthsWeeks be ! CreateTemporalDuration(years, months, weeks, 0, 0, 0, 0, 0, 0, 0).
        auto years_months_weeks = MUST(create_temporal_duration(vm, years, months, weeks, 0, 0, 0, 0, 0, 0, 0));

        // FIXME: d. Let yearsMonthsWeeksLater be ? AddDate(calendarRec, plainRelativeTo, yearsMonthsWeeks).
        auto* years_months_weeks_later = TRY(calendar_date_add(vm, *calendar, plain_relative_to, *years_months_weeks, nullptr, date_add));

        // e. Let weeksInDays be DaysUntil(yearsMonthsLater, yearsMonthsWeeksLater).
        auto weeks_in_days = days_until(*years_months_later, *years_months_weeks_later);

        // f. Set plainRelativeTo to yearsMonthsLater.
        plain_relative_to = years_months_later;

        // g. Set fractionalDays to fractionalDays + weeksInDays.
        fractional_days += weeks_in_days;

        // h. Let isoResult be ! AddISODate(plainRelativeTo.[[ISOYear]], plainRelativeTo.[[ISOMonth]], plainRelativeTo.[[ISODay]], 0, 0, 0, truncate(fractionalDays), "constrain").
        auto iso_result = MUST(add_iso_date(vm, plain_relative_to->iso_year(), plain_relative_to->iso_month(), plain_relative_to->iso_day(), 0, 0, 0, trunc(fractional_days), "constrain"sv));

        // i. Let wholeDaysLater be ? CreateTemporalDate(isoResult.[[Year]], isoResult.[[Month]], isoResult.[[Day]], calendarRec.[[Receiver]]).
        // FIXME: Pass through calendarRec
        auto* whole_days_later = TRY(create_temporal_date(vm, iso_result.year, iso_result.month, iso_result.day, *calendar)); // FIXME: receiver

        // j. Let untilOptions be OrdinaryObjectCreate(null).
        auto until_options = Object::create(realm, nullptr);

        // k. Perform ! CreateDataPropertyOrThrow(untilOptions, "largestUnit", "month").
        MUST(until_options->create_data_property_or_throw(vm.names.largestUnit, PrimitiveString::create(vm, "month"_string)));

        // l. Let timePassed be ? DifferenceDate(calendarRec, plainRelativeTo, wholeDaysLater, untilOptions).
        auto time_passed = TRY(difference_date(vm, calendar_record.value(), *plain_relative_to, *whole_days_later, *until_options));

        // m. Let monthsPassed be timePassed.[[Months]].
        auto months_passed = time_passed->months();

        // n. Set months to months + monthsPassed.
        months += months_passed;

        // o. Let monthsPassedDuration be ! CreateTemporalDuration(0, monthsPassed, 0, 0, 0, 0, 0, 0, 0, 0).
        auto months_passed_duration = MUST(create_temporal_duration(vm, 0, months_passed, 0, 0, 0, 0, 0, 0, 0, 0));

        // p. Let moveResult be ? MoveRelativeDate(calendarRec, plainRelativeTo, monthsPassedDuration).
        // FIXME: Pass through calendarRec
        auto move_result = TRY(move_relative_date(vm, *calendar, *plain_relative_to, *months_passed_duration, date_add));

        // q. Set plainRelativeTo to moveResult.[[RelativeTo]].
        plain_relative_to = move_result.relative_to;

        // r. Let daysPassed be moveResult.[[Days]].
        auto days_passed = move_result.days;

        // s. Set fractionalDays to fractionalDays - daysPassed.
        fractional_days -= days_passed;

        // t. If fractionalDays < 0, let sign be -1; else, let sign be 1.
        auto sign = fractional_days < 0 ? -1 : 1;

        // u. Let oneMonth be ! CreateTemporalDuration(0, sign, 0, 0, 0, 0, 0, 0, 0, 0).
        auto one_month = MUST(create_temporal_duration(vm, 0, sign, 0, 0, 0, 0, 0, 0, 0, 0));

        // v. Let moveResult be ? MoveRelativeDate(calendarRec, plainRelativeTo, oneMonth).
        // FIXME: spec bug, this should be set.
        move_result = TRY(move_relative_date(vm, *calendar, *plain_relative_to, *one_month, date_add));

        // w. Let oneMonthDays be moveResult.[[Days]].
        auto one_month_days = move_result.days;

        // x. If oneMonthDays = 0, throw a RangeError exception.
        if (one_month_days == 0)
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, "dateAdd", "result implying a month is zero days long");

        // y. Let fractionalMonths be months + fractionalDays / abs(oneMonthDays).
        auto fractional_months = months + fractional_days / fabs(one_month_days);

        // z. Set months to RoundNumberToIncrement(fractionalMonths, increment, roundingMode).
        months = round_number_to_increment(fractional_months, increment, rounding_mode);

        // aa. Set total to fractionalMonths.
        total = fractional_months;

        // ab. Set weeks to 0.
        weeks = 0;
    }
    // 11. Else if unit is "week", then
    else if (unit == "week"sv) {
        VERIFY(plain_relative_to);

        // a. Let isoResult be ! AddISODate(plainRelativeTo.[[ISOYear]], plainRelativeTo.[[ISOMonth]], plainRelativeTo.[[ISODay]], 0, 0, 0, truncate(fractionalDays), "constrain").
        auto iso_result = MUST(add_iso_date(vm, plain_relative_to->iso_year(), plain_relative_to->iso_month(), plain_relative_to->iso_day(), 0, 0, 0, trunc(fractional_days), "constrain"sv));

        // b. Let wholeDaysLater be ? CreateTemporalDate(isoResult.[[Year]], isoResult.[[Month]], isoResult.[[Day]], calendarRec.[[Receiver]]).
        // FIXME: Pass through receiver from calendarRec
        auto* whole_days_later = TRY(create_temporal_date(vm, iso_result.year, iso_result.month, iso_result.day, *calendar));

        // c. Let untilOptions be OrdinaryObjectCreate(null).
        auto until_options = Object::create(realm, nullptr);

        // d. Perform ! CreateDataPropertyOrThrow(untilOptions, "largestUnit", "week").
        MUST(until_options->create_data_property_or_throw(vm.names.largestUnit, PrimitiveString::create(vm, "month"_string)));

        // e. Let timePassed be ? DifferenceDate(calendarRec, plainRelativeTo, wholeDaysLater, untilOptions).
        auto time_passed = TRY(difference_date(vm, calendar_record.value(), *plain_relative_to, *whole_days_later, *until_options));

        // f. Let weeksPassed be timePassed.[[Weeks]].
        auto weeks_passed = time_passed->weeks();

        // g. Set weeks to weeks + weeksPassed.
        weeks += weeks_passed;

        // h. Let weeksPassedDuration be ! CreateTemporalDuration(0, 0, weeksPassed, 0, 0, 0, 0, 0, 0, 0).
        auto weeks_passed_duration = MUST(create_temporal_duration(vm, 0, 0, weeks_passed, 0, 0, 0, 0, 0, 0, 0));

        // FIXME: i. Let moveResult be ? MoveRelativeDate(calendarRec, plainRelativeTo, weeksPassedDuration).
        auto date_add = TRY(Value(calendar).get_method(vm, vm.names.dateAdd));
        auto move_result = TRY(move_relative_date(vm, *calendar, *plain_relative_to, *weeks_passed_duration, date_add));

        // j. Set plainRelativeTo to moveResult.[[RelativeTo]].
        plain_relative_to = move_result.relative_to;

        // k. Let daysPassed be moveResult.[[Days]].
        auto days_passed = move_result.days;

        // l. Set fractionalDays to fractionalDays - daysPassed.
        fractional_days -= days_passed;

        // m. If fractionalDays < 0, let sign be -1; else, let sign be 1.
        auto sign = fractional_days < 0 ? -1 : 1;

        // n. Let oneWeek be ! CreateTemporalDuration(0, 0, sign, 0, 0, 0, 0, 0, 0, 0).
        auto one_week = MUST(create_temporal_duration(vm, 0, 0, sign, 0, 0, 0, 0, 0, 0, 0));

        // o. Let moveResult be ? MoveRelativeDate(calendarRec, plainRelativeTo, oneWeek).
        // FIXME: spec bug, should be set
        move_result = TRY(move_relative_date(vm, *calendar, *plain_relative_to, *one_week, date_add));

        // p. Let oneWeekDays be moveResult.[[Days]].
        auto one_week_days = move_result.days;

        // q. If oneWeekDays = 0, throw a RangeError exception.
        if (one_week_days == 0)
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, "dateAdd", "result implying a month is zero days long");

        // r. Let fractionalWeeks be weeks + fractionalDays / abs(oneWeekDays).
        auto fractional_weeks = weeks + fractional_days / fabs(one_week_days);

        // s. Set weeks to RoundNumberToIncrement(fractionalWeeks, increment, roundingMode).
        weeks = round_number_to_increment(fractional_weeks, increment, rounding_mode);

        // t. Set total to fractionalWeeks.
        total = fractional_weeks;
    }
    // 12. Else if unit is "day", then
    else if (unit == "day"sv) {
        // a. Set days to RoundNumberToIncrement(fractionalDays, increment, roundingMode).
        days = round_number_to_increment(fractional_days, increment, rounding_mode);

        // b. Set total to fractionalDays.
        total = fractional_days;
    }
    // 13. Else if unit is "hour", then
    else if (unit == "hour"sv) {
        // a. Let fractionalHours be (fractionalSeconds / 60 + minutes) / 60 + hours.
        auto fractional_hours = (fractional_seconds / 60 + minutes) / 60 + hours;

        // b. Set hours to RoundNumberToIncrement(fractionalHours, increment, roundingMode).
        hours = round_number_to_increment(fractional_hours, increment, rounding_mode);

        // c. Set total to fractionalHours.
        total = fractional_hours;

        // d. Set minutes, seconds, milliseconds, microseconds, and nanoseconds to 0.
        minutes = 0;
        seconds = 0;
        milliseconds = 0;
        microseconds = 0;
        nanoseconds = 0;
    }
    // 14. Else if unit is "minute", then
    else if (unit == "minute"sv) {
        // a. Let fractionalMinutes be fractionalSeconds / 60 + minutes.
        auto fractional_minutes = fractional_seconds / 60 + minutes;

        // b. Set minutes to RoundNumberToIncrement(fractionalMinutes, increment, roundingMode).
        minutes = round_number_to_increment(fractional_minutes, increment, rounding_mode);

        // c. Set total to fractionalMinutes.
        total = fractional_minutes;

        // d. Set seconds, milliseconds, microseconds, and nanoseconds to 0.
        seconds = 0;
        milliseconds = 0;
        microseconds = 0;
        nanoseconds = 0;
    }
    // 15. Else if unit is "second", then
    else if (unit == "second"sv) {
        // a. Set seconds to RoundNumberToIncrement(fractionalSeconds, increment, roundingMode).
        seconds = round_number_to_increment(fractional_seconds, increment, rounding_mode);

        // b. Set total to fractionalSeconds.
        total = fractional_seconds;

        // c. Set milliseconds, microseconds, and nanoseconds to 0.
        milliseconds = 0;
        microseconds = 0;
        nanoseconds = 0;
    }
    // 16. Else if unit is "millisecond", then
    else if (unit == "millisecond"sv) {
        // a. Let fractionalMilliseconds be nanoseconds × 10^-6 + microseconds × 10^-3 + milliseconds.
        auto fractional_milliseconds = nanoseconds * 0.000001 + microseconds * 0.001 + milliseconds;

        // b. Set milliseconds to RoundNumberToIncrement(fractionalMilliseconds, increment, roundingMode).
        milliseconds = round_number_to_increment(fractional_milliseconds, increment, rounding_mode);

        // c. Set total to fractionalMilliseconds.
        total = fractional_milliseconds;

        // d. Set microseconds and nanoseconds to 0.
        microseconds = 0;
        nanoseconds = 0;
    }
    // 17. Else if unit is "microsecond", then
    else if (unit == "microsecond"sv) {
        // a. Let fractionalMicroseconds be nanoseconds × 10^-3 + microseconds.
        auto fractional_microseconds = nanoseconds * 0.001 + microseconds;

        // b. Set microseconds to RoundNumberToIncrement(fractionalMicroseconds, increment, roundingMode).
        microseconds = round_number_to_increment(fractional_microseconds, increment, rounding_mode);

        // c. Set total to fractionalMicroseconds.
        total = fractional_microseconds;

        // d. Set nanoseconds to 0.
        nanoseconds = 0;
    }
    // 18. Else,
    else {
        // a. Assert: unit is "nanosecond".
        VERIFY(unit == "nanosecond"sv);

        // b. Set total to nanoseconds.
        total = nanoseconds;

        // c. Set nanoseconds to RoundNumberToIncrement(nanoseconds, increment, roundingMode).
        nanoseconds = round_number_to_increment(nanoseconds, increment, rounding_mode);
    }

    // 20. Let duration be ? CreateDurationRecord(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
    auto duration = TRY(create_duration_record(vm, years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds));

    // 21. Return the Record { [[DurationRecord]]: duration, [[Total]]: total }.
    return RoundedDuration { .duration_record = duration, .total = total };
}

// 7.5.26 AdjustRoundedDurationDays ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, increment, unit, roundingMode, relativeTo ), https://tc39.es/proposal-temporal/#sec-temporal-adjustroundeddurationdays
ThrowCompletionOr<DurationRecord> adjust_rounded_duration_days(VM& vm, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, u32 increment, StringView unit, StringView rounding_mode, Object* relative_to_object)
{
    // 1. If Type(relativeTo) is not Object; or relativeTo does not have an [[InitializedTemporalZonedDateTime]] internal slot; or unit is one of "year", "month", "week", or "day"; or unit is "nanosecond" and increment is 1, then
    if (relative_to_object == nullptr || !is<ZonedDateTime>(relative_to_object) || unit.is_one_of("year"sv, "month"sv, "week"sv, "day"sv) || (unit == "nanosecond"sv && increment == 1)) {
        // a. Return ! CreateDurationRecord(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
        return create_duration_record(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds);
    }

    auto& relative_to = static_cast<ZonedDateTime&>(*relative_to_object);

    // 2. Let timeRemainderNs be ! TotalDurationNanoseconds(0, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, 0).
    auto time_remainder_ns = total_duration_nanoseconds(0, hours, minutes, seconds, milliseconds, microseconds, Crypto::SignedBigInteger { nanoseconds }, 0);

    i32 direction;

    // 3. If timeRemainderNs = 0, let direction be 0.
    if (time_remainder_ns.is_zero())
        direction = 0;
    // 4. Else if timeRemainderNs < 0, let direction be -1.
    else if (time_remainder_ns.is_negative())
        direction = -1;
    // 5. Else, let direction be 1.
    else
        direction = 1;

    // 6. Let dayStart be ? AddZonedDateTime(relativeTo.[[Nanoseconds]], relativeTo.[[TimeZone]], relativeTo.[[Calendar]], years, months, weeks, days, 0, 0, 0, 0, 0, 0).
    auto* day_start = TRY(add_zoned_date_time(vm, relative_to.nanoseconds(), &relative_to.time_zone(), relative_to.calendar(), years, months, weeks, days, 0, 0, 0, 0, 0, 0));

    // 7. Let dayEnd be ? AddZonedDateTime(dayStart, relativeTo.[[TimeZone]], relativeTo.[[Calendar]], 0, 0, 0, direction, 0, 0, 0, 0, 0, 0).
    auto* day_end = TRY(add_zoned_date_time(vm, *day_start, &relative_to.time_zone(), relative_to.calendar(), 0, 0, 0, direction, 0, 0, 0, 0, 0, 0));

    // 8. Let dayLengthNs be ℝ(dayEnd - dayStart).
    auto day_length_ns = day_end->big_integer().minus(day_start->big_integer());

    // 9. If (timeRemainderNs - dayLengthNs) × direction < 0, then
    if (time_remainder_ns.minus(day_length_ns).multiplied_by(Crypto::SignedBigInteger { direction }).is_negative()) {
        // a. Return ! CreateDurationRecord(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
        return create_duration_record(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds);
    }

    // 10. Set timeRemainderNs to ! RoundTemporalInstant(ℤ(timeRemainderNs - dayLengthNs), increment, unit, roundingMode).
    time_remainder_ns = round_temporal_instant(vm, BigInt::create(vm, time_remainder_ns.minus(day_length_ns)), increment, unit, rounding_mode)->big_integer();

    // 11. Let adjustedDateDuration be ? AddDuration(years, months, weeks, days, 0, 0, 0, 0, 0, 0, 0, 0, 0, direction, 0, 0, 0, 0, 0, 0, relativeTo).
    auto adjusted_date_duration = TRY(add_duration(vm, years, months, weeks, days, 0, 0, 0, 0, 0, 0, 0, 0, 0, direction, 0, 0, 0, 0, 0, 0, &relative_to));

    // 12. Let adjustedTimeDuration be ? BalanceDuration(0, 0, 0, 0, 0, 0, timeRemainderNs, "hour").
    auto adjusted_time_duration = TRY(balance_duration(vm, 0, 0, 0, 0, 0, 0, time_remainder_ns, "hour"sv));

    // 13. Return ! CreateDurationRecord(adjustedDateDuration.[[Years]], adjustedDateDuration.[[Months]], adjustedDateDuration.[[Weeks]], adjustedDateDuration.[[Days]], adjustedTimeDuration.[[Hours]], adjustedTimeDuration.[[Minutes]], adjustedTimeDuration.[[Seconds]], adjustedTimeDuration.[[Milliseconds]], adjustedTimeDuration.[[Microseconds]], adjustedTimeDuration.[[Nanoseconds]]).
    return create_duration_record(adjusted_date_duration.years, adjusted_date_duration.months, adjusted_date_duration.weeks, adjusted_date_duration.days, adjusted_time_duration.hours, adjusted_time_duration.minutes, adjusted_time_duration.seconds, adjusted_time_duration.milliseconds, adjusted_time_duration.microseconds, adjusted_time_duration.nanoseconds);
}

// 7.5.27 TemporalDurationToString ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds, precision ), https://tc39.es/proposal-temporal/#sec-temporal-temporaldurationtostring
ThrowCompletionOr<String> temporal_duration_to_string(VM& vm, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, Variant<StringView, u8> const& precision)
{
    if (precision.has<StringView>())
        VERIFY(precision.get<StringView>() == "auto"sv);

    // 1. Let sign be ! DurationSign(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
    auto sign = duration_sign(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds);

    // 2. Set microseconds to microseconds + truncate(nanoseconds / 1000).
    microseconds += trunc(nanoseconds / 1000);

    // 3. Set nanoseconds to remainder(nanoseconds, 1000).
    nanoseconds = fmod(nanoseconds, 1000);

    // 4. Set milliseconds to milliseconds + truncate(microseconds / 1000).
    milliseconds += trunc(microseconds / 1000);

    // 5. Set microseconds to remainder(microseconds, 1000).
    microseconds = fmod(microseconds, 1000);

    // 6. Set seconds to seconds + truncate(milliseconds / 1000).
    seconds += trunc(milliseconds / 1000);

    // 7. Set milliseconds to remainder(milliseconds, 1000).
    milliseconds = fmod(milliseconds, 1000);

    // 8. Let datePart be "".
    StringBuilder date_part;

    // 9. If years is not 0, then
    if (years != 0) {
        // a. Set datePart to the string concatenation of abs(years) formatted as a decimal number and the code unit 0x0059 (LATIN CAPITAL LETTER Y).
        date_part.appendff("{}", fabs(years));
        date_part.append('Y');
    }

    // 10. If months is not 0, then
    if (months != 0) {
        // a. Set datePart to the string concatenation of datePart, abs(months) formatted as a decimal number, and the code unit 0x004D (LATIN CAPITAL LETTER M).
        date_part.appendff("{}", fabs(months));
        date_part.append('M');
    }

    // 11. If weeks is not 0, then
    if (weeks != 0) {
        // a. Set datePart to the string concatenation of datePart, abs(weeks) formatted as a decimal number, and the code unit 0x0057 (LATIN CAPITAL LETTER W).
        date_part.appendff("{}", fabs(weeks));
        date_part.append('W');
    }

    // 12. If days is not 0, then
    if (days != 0) {
        // a. Set datePart to the string concatenation of datePart, abs(days) formatted as a decimal number, and the code unit 0x0044 (LATIN CAPITAL LETTER D).
        date_part.appendff("{}", fabs(days));
        date_part.append('D');
    }

    // 13. Let timePart be "".
    StringBuilder time_part;

    // 14. If hours is not 0, then
    if (hours != 0) {
        // a. Set timePart to the string concatenation of abs(hours) formatted as a decimal number and the code unit 0x0048 (LATIN CAPITAL LETTER H).
        time_part.appendff("{}", fabs(hours));
        time_part.append('H');
    }

    // 15. If minutes is not 0, then
    if (minutes != 0) {
        // a. Set timePart to the string concatenation of timePart, abs(minutes) formatted as a decimal number, and the code unit 0x004D (LATIN CAPITAL LETTER M).
        time_part.appendff("{}", fabs(minutes));
        time_part.append('M');
    }

    // 16. If any of seconds, milliseconds, microseconds, and nanoseconds are not 0; or years, months, weeks, days, hours, and minutes are all 0; or precision is not "auto"; then
    if ((seconds != 0 || milliseconds != 0 || microseconds != 0 || nanoseconds != 0) || (years == 0 && months == 0 && weeks == 0 && days == 0 && hours == 0 && minutes == 0) || (!precision.has<StringView>() || precision.get<StringView>() != "auto"sv)) {
        // a. Let fraction be abs(milliseconds) × 10^6 + abs(microseconds) × 10^3 + abs(nanoseconds).
        auto fraction = fabs(milliseconds) * 1'000'000 + fabs(microseconds) * 1'000 + fabs(nanoseconds);

        // b. Let decimalPart be ToZeroPaddedDecimalString(fraction, 9).
        // NOTE: padding with zeros leads to weird results when applied to a double. Not sure if that's a bug in AK/Format.h or if I'm doing this wrong.
        auto decimal_part_string = TRY_OR_THROW_OOM(vm, String::formatted("{:09}", (u64)fraction));
        StringView decimal_part;

        // c. If precision is "auto", then
        if (precision.has<StringView>() && precision.get<StringView>() == "auto"sv) {
            // i. Set decimalPart to the longest possible substring of decimalPart starting at position 0 and not ending with the code unit 0x0030 (DIGIT ZERO).
            decimal_part = decimal_part_string.bytes_as_string_view().trim("0"sv, TrimMode::Right);
        }
        // d. Else if precision = 0, then
        else if (precision.get<u8>() == 0) {
            // i. Set decimalPart to "".
            decimal_part = ""sv;
        }
        // e. Else,
        else {
            // i. Set decimalPart to the substring of decimalPart from 0 to precision.
            decimal_part = decimal_part_string.bytes_as_string_view().substring_view(0, precision.get<u8>());
        }

        // f. Let secondsPart be abs(seconds) formatted as a decimal number.
        StringBuilder seconds_part;
        seconds_part.appendff("{}", fabs(seconds));

        // g. If decimalPart is not "", then
        if (!decimal_part.is_empty()) {
            // i. Set secondsPart to the string-concatenation of secondsPart, the code unit 0x002E (FULL STOP), and decimalPart.
            seconds_part.append('.');
            seconds_part.append(decimal_part);
        }

        // h. Set timePart to the string concatenation of timePart, secondsPart, and the code unit 0x0053 (LATIN CAPITAL LETTER S).
        time_part.append(seconds_part.string_view());
        time_part.append('S');
    }

    // 17. Let signPart be the code unit 0x002D (HYPHEN-MINUS) if sign < 0, and otherwise the empty String.
    auto sign_part = sign < 0 ? "-"sv : ""sv;

    // 18. Let result be the string concatenation of signPart, the code unit 0x0050 (LATIN CAPITAL LETTER P) and datePart.
    StringBuilder result;
    result.append(sign_part);
    result.append('P');
    result.append(date_part.string_view());

    // 19. If timePart is not "", then
    if (!time_part.is_empty()) {
        // a. Set result to the string concatenation of result, the code unit 0x0054 (LATIN CAPITAL LETTER T), and timePart.
        result.append('T');
        result.append(time_part.string_view());
    }

    // 20. Return result.
    return TRY_OR_THROW_OOM(vm, result.to_string());
}

// 7.5.28 AddDurationToOrSubtractDurationFromDuration ( operation, duration, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtoorsubtractdurationfromduration
ThrowCompletionOr<NonnullGCPtr<Duration>> add_duration_to_or_subtract_duration_from_duration(VM& vm, ArithmeticOperation operation, Duration const& duration, Value other_value, Value options_value)
{
    // 1. If operation is subtract, let sign be -1. Otherwise, let sign be 1.
    i8 sign = operation == ArithmeticOperation::Subtract ? -1 : 1;

    // 2. Set other to ? ToTemporalDurationRecord(other).
    auto other = TRY(to_temporal_duration_record(vm, other_value));

    // 3. Set options to ? GetOptionsObject(options).
    auto const* options = TRY(get_options_object(vm, options_value));

    // 4. Let relativeToRecord be ? ToRelativeTemporalObject(options).
    auto relative_to_record = TRY(to_relative_temporal_object(vm, *options));

    // 5. Let result be ? AddDuration(duration.[[Years]], duration.[[Months]], duration.[[Weeks]], duration.[[Days]], duration.[[Hours]], duration.[[Minutes]], duration.[[Seconds]], duration.[[Milliseconds]], duration.[[Microseconds]], duration.[[Nanoseconds]], sign × other.[[Years]], sign × other.[[Months]], sign × other.[[Weeks]], sign × other.[[Days]], sign × other.[[Hours]], sign × other.[[Minutes]], sign × other.[[Seconds]], sign × other.[[Milliseconds]], sign × other.[[Microseconds]], sign × other.[[Nanoseconds]], relativeTo).
    auto result = TRY(add_duration(vm, duration.years(), duration.months(), duration.weeks(), duration.days(), duration.hours(), duration.minutes(), duration.seconds(), duration.milliseconds(), duration.microseconds(), duration.nanoseconds(), sign * other.years, sign * other.months, sign * other.weeks, sign * other.days, sign * other.hours, sign * other.minutes, sign * other.seconds, sign * other.milliseconds, sign * other.microseconds, sign * other.nanoseconds, relative_to_converted_to_value(relative_to_record)));

    // 6. Return ! CreateTemporalDuration(result.[[Years]], result.[[Months]], result.[[Weeks]], result.[[Days]], result.[[Hours]], result.[[Minutes]], result.[[Seconds]], result.[[Milliseconds]], result.[[Microseconds]], result.[[Nanoseconds]]).
    return MUST(create_temporal_duration(vm, result.years, result.months, result.weeks, result.days, result.hours, result.minutes, result.seconds, result.milliseconds, result.microseconds, result.nanoseconds));
}

}
