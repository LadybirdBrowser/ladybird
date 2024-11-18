/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Value.h>

namespace JS::Temporal {

#define JS_ENUMERATE_DURATION_UNITS \
    __JS_ENUMERATE(years)           \
    __JS_ENUMERATE(months)          \
    __JS_ENUMERATE(weeks)           \
    __JS_ENUMERATE(days)            \
    __JS_ENUMERATE(hours)           \
    __JS_ENUMERATE(minutes)         \
    __JS_ENUMERATE(seconds)         \
    __JS_ENUMERATE(milliseconds)    \
    __JS_ENUMERATE(microseconds)    \
    __JS_ENUMERATE(nanoseconds)

class Duration final : public Object {
    JS_OBJECT(Duration, Object);
    GC_DECLARE_ALLOCATOR(Duration);

public:
    virtual ~Duration() override = default;

#define __JS_ENUMERATE(unit) \
    [[nodiscard]] double unit() const { return m_##unit; }
    JS_ENUMERATE_DURATION_UNITS
#undef __JS_ENUMERATE

private:
    Duration(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, Object& prototype);

    double m_years { 0 };        // [[Years]]
    double m_months { 0 };       // [[Months]]
    double m_weeks { 0 };        // [[Weeks]]
    double m_days { 0 };         // [[Days]]
    double m_hours { 0 };        // [[Hours]]
    double m_minutes { 0 };      // [[Minutes]]
    double m_seconds { 0 };      // [[Seconds]]
    double m_milliseconds { 0 }; // [[Milliseconds]]
    double m_microseconds { 0 }; // [[Microseconds]]
    double m_nanoseconds { 0 };  // [[Nanoseconds]]
};

// 7.5.1 Date Duration Records, https://tc39.es/proposal-temporal/#sec-temporal-date-duration-records
struct DateDuration {
    double years { 0 };
    double months { 0 };
    double weeks { 0 };
    double days { 0 };
};

// 7.5.2 Partial Duration Records, https://tc39.es/proposal-temporal/#sec-temporal-partial-duration-records
struct PartialDuration {
    static PartialDuration zero()
    {
        return { .years = 0, .months = 0, .weeks = 0, .days = 0, .hours = 0, .minutes = 0, .seconds = 0, .milliseconds = 0, .microseconds = 0, .nanoseconds = 0 };
    }

    bool any_field_defined() const
    {
        return years.has_value() || months.has_value() || weeks.has_value() || days.has_value() || hours.has_value() || minutes.has_value() || seconds.has_value() || milliseconds.has_value() || microseconds.has_value() || nanoseconds.has_value();
    }

    Optional<double> years;
    Optional<double> months;
    Optional<double> weeks;
    Optional<double> days;
    Optional<double> hours;
    Optional<double> minutes;
    Optional<double> seconds;
    Optional<double> milliseconds;
    Optional<double> microseconds;
    Optional<double> nanoseconds;
};

// A time duration is an integer in the inclusive interval from -maxTimeDuration to maxTimeDuration, where
// maxTimeDuration = 2**53 × 10**9 - 1 = 9,007,199,254,740,991,999,999,999. It represents the portion of a
// Temporal.Duration object that deals with time units, but as a combined value of total nanoseconds.
using TimeDuration = Crypto::SignedBigInteger;
extern TimeDuration const MAX_TIME_DURATION;

// 7.5.3 Internal Duration Records, https://tc39.es/proposal-temporal/#sec-temporal-internal-duration-records
struct InternalDuration {
    DateDuration date;
    TimeDuration time;
};

DateDuration zero_date_duration(VM&);
InternalDuration to_internal_duration_record(VM&, Duration const&);
InternalDuration to_internal_duration_record_with_24_hour_days(VM&, Duration const&);
ThrowCompletionOr<GC::Ref<Duration>> temporal_duration_from_internal(VM&, InternalDuration const&, Unit largest_unit);
ThrowCompletionOr<DateDuration> create_date_duration_record(VM&, double years, double months, double weeks, double days);
ThrowCompletionOr<InternalDuration> combine_date_and_time_duration(VM&, DateDuration, TimeDuration);
ThrowCompletionOr<GC::Ref<Duration>> to_temporal_duration(VM&, Value);
i8 duration_sign(Duration const&);
i8 date_duration_sign(DateDuration const&);
bool is_valid_duration(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds);
Unit default_temporal_largest_unit(Duration const&);
ThrowCompletionOr<PartialDuration> to_temporal_partial_duration_record(VM&, Value temporal_duration_like);
ThrowCompletionOr<GC::Ref<Duration>> create_temporal_duration(VM&, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, GC::Ptr<FunctionObject> new_target = {});
GC::Ref<Duration> create_negated_temporal_duration(VM&, Duration const&);
TimeDuration time_duration_from_components(double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds);
ThrowCompletionOr<TimeDuration> add_time_duration(VM&, TimeDuration const&, TimeDuration const&);
ThrowCompletionOr<TimeDuration> add_24_hour_days_to_time_duration(VM&, TimeDuration const&, double days);
i8 compare_time_duration(TimeDuration const&, TimeDuration const&);
ThrowCompletionOr<TimeDuration> round_time_duration_to_increment(VM&, TimeDuration const&, Crypto::UnsignedBigInteger const& increment, RoundingMode);
i8 time_duration_sign(TimeDuration const&);
ThrowCompletionOr<TimeDuration> round_time_duration(VM&, TimeDuration const&, Crypto::UnsignedBigInteger const& increment, Unit, RoundingMode);
double total_time_duration(TimeDuration const&, Unit);
String temporal_duration_to_string(Duration const&, Precision);
ThrowCompletionOr<GC::Ref<Duration>> add_durations(VM&, ArithmeticOperation, Duration const&, Value);

}
