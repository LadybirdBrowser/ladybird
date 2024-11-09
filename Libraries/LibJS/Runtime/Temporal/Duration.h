/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Heap/Handle.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibJS/Runtime/VM.h>

namespace JS::Temporal {

class Duration final : public Object {
    JS_OBJECT(Duration, Object);
    JS_DECLARE_ALLOCATOR(Duration);

public:
    virtual ~Duration() override = default;

    [[nodiscard]] double years() const { return m_years; }
    [[nodiscard]] double months() const { return m_months; }
    [[nodiscard]] double weeks() const { return m_weeks; }
    [[nodiscard]] double days() const { return m_days; }
    [[nodiscard]] double hours() const { return m_hours; }
    [[nodiscard]] double minutes() const { return m_minutes; }
    [[nodiscard]] double seconds() const { return m_seconds; }
    [[nodiscard]] double milliseconds() const { return m_milliseconds; }
    [[nodiscard]] double microseconds() const { return m_microseconds; }
    [[nodiscard]] double nanoseconds() const { return m_nanoseconds; }

private:
    Duration(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, Object& prototype);

    // 7.4 Properties of Temporal.Duration Instances, https://tc39.es/proposal-temporal/#sec-properties-of-temporal-duration-instances
    double m_years;        // [[Years]]
    double m_months;       // [[Months]]
    double m_weeks;        // [[Weeks]]
    double m_days;         // [[Days]]
    double m_hours;        // [[Hours]]
    double m_minutes;      // [[Minutes]]
    double m_seconds;      // [[Seconds]]
    double m_milliseconds; // [[Milliseconds]]
    double m_microseconds; // [[Microseconds]]
    double m_nanoseconds;  // [[Nanoseconds]]
};

// 7.5.1 Duration Records, https://tc39.es/proposal-temporal/#sec-temporal-duration-records
struct DurationRecord {
    double years;
    double months;
    double weeks;
    double days;
    double hours;
    double minutes;
    double seconds;
    double milliseconds;
    double microseconds;
    double nanoseconds;
};

// 7.5.2 Date Duration Records, https://tc39.es/proposal-temporal/#sec-temporal-date-duration-records
struct DateDurationRecord {
    double years;
    double months;
    double weeks;
    double days;
};

// 7.5.3 Time Duration Records, https://tc39.es/proposal-temporal/#sec-temporal-time-duration-records
struct TimeDurationRecord {
    double days;
    double hours;
    double minutes;
    double seconds;
    double milliseconds;
    double microseconds;
    double nanoseconds;
};

// 7.5.4 Partial Duration Records, https://tc39.es/proposal-temporal/#sec-temporal-partial-duration-records
struct PartialDurationRecord {
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

// Used by MoveRelativeDate to temporarily hold values
struct MoveRelativeDateResult {
    Handle<PlainDate> relative_to;
    double days;
};

// Used by RoundDuration to temporarily hold values
struct RoundedDuration {
    DurationRecord duration_record;
    double total;
};

// Table 8: Duration Record Fields, https://tc39.es/proposal-temporal/#table-temporal-duration-record-fields

template<typename StructT, typename ValueT>
struct TemporalDurationRecordField {
    ValueT StructT::*field_name { nullptr };
    PropertyKey property_name;
};

DurationRecord create_duration_record(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds);
ThrowCompletionOr<DurationRecord> create_duration_record(VM&, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds);
DateDurationRecord create_date_duration_record(double years, double months, double weeks, double days);
ThrowCompletionOr<DateDurationRecord> create_date_duration_record(VM&, double years, double months, double weeks, double days);
ThrowCompletionOr<TimeDurationRecord> create_time_duration_record(VM&, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds);
ThrowCompletionOr<NonnullGCPtr<Duration>> to_temporal_duration(VM&, Value item);
ThrowCompletionOr<DurationRecord> to_temporal_duration_record(VM&, Value temporal_duration_like);
i8 duration_sign(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds);
bool is_valid_duration(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds);
StringView default_temporal_largest_unit(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds);
ThrowCompletionOr<PartialDurationRecord> to_temporal_partial_duration_record(VM&, Value temporal_duration_like);
ThrowCompletionOr<NonnullGCPtr<Duration>> create_temporal_duration(VM&, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, FunctionObject const* new_target = nullptr);
NonnullGCPtr<Duration> create_negated_temporal_duration(VM&, Duration const& duration);
ThrowCompletionOr<double> calculate_offset_shift(VM&, Value relative_to_value, double years, double months, double weeks, double days);
Crypto::SignedBigInteger total_duration_nanoseconds(double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, Crypto::SignedBigInteger const& nanoseconds, double offset_shift);
ThrowCompletionOr<TimeDurationRecord> balance_time_duration(VM& vm, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, Crypto::SignedBigInteger const& nanoseconds, StringView largest_unit);
ThrowCompletionOr<TimeDurationRecord> balance_duration(VM&, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, Crypto::SignedBigInteger const& nanoseconds, StringView largest_unit, Object* relative_to = nullptr);

enum class Overflow {
    Positive,
    Negative,
};

Variant<TimeDurationRecord, Overflow> balance_possibly_infinite_time_duration(VM& vm, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, Crypto::SignedBigInteger const& nanoseconds, StringView largest_unit);

ThrowCompletionOr<DateDurationRecord> unbalance_duration_relative(VM&, double years, double months, double weeks, double days, StringView largest_unit, Value relative_to);
ThrowCompletionOr<DateDurationRecord> balance_duration_relative(VM&, double years, double months, double weeks, double days, StringView largest_unit, Value relative_to);
ThrowCompletionOr<DurationRecord> add_duration(VM&, double years1, double months1, double weeks1, double days1, double hours1, double minutes1, double seconds1, double milliseconds1, double microseconds1, double nanoseconds1, double years2, double months2, double weeks2, double days2, double hours2, double minutes2, double seconds2, double milliseconds2, double microseconds2, double nanoseconds2, Value relative_to_value);
ThrowCompletionOr<MoveRelativeDateResult> move_relative_date(VM&, Object& calendar, PlainDate& relative_to, Duration& duration, FunctionObject* date_add);
ThrowCompletionOr<ZonedDateTime*> move_relative_zoned_date_time(VM&, ZonedDateTime&, double years, double months, double weeks, double days);
ThrowCompletionOr<RoundedDuration> round_duration(VM&, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, u32 increment, StringView unit, StringView rounding_mode, Object* relative_to_object = nullptr, Optional<CalendarMethods> const& = {});
ThrowCompletionOr<DurationRecord> adjust_rounded_duration_days(VM&, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, u32 increment, StringView unit, StringView rounding_mode, Object* relative_to_object);
ThrowCompletionOr<String> temporal_duration_to_string(VM&, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, Variant<StringView, u8> const& precision);
ThrowCompletionOr<NonnullGCPtr<Duration>> add_duration_to_or_subtract_duration_from_duration(VM&, ArithmeticOperation, Duration const&, Value other_value, Value options_value);

// 7.5.22 DaysUntil ( earlier, later ), https://tc39.es/proposal-temporal/#sec-temporal-daysuntil
template<typename EarlierObjectType, typename LaterObjectType>
double days_until(EarlierObjectType& earlier, LaterObjectType& later)
{
    // 1. Let epochDays1 be MakeDay(𝔽(earlier.[[ISOYear]]), 𝔽(earlier.[[ISOMonth]] - 1), 𝔽(earlier.[[ISODay]])).
    auto epoch_days_1 = make_day(earlier.iso_year(), earlier.iso_month() - 1, earlier.iso_day());

    // 2. Assert: epochDays1 is finite.
    VERIFY(isfinite(epoch_days_1));

    // 3. Let epochDays2 be MakeDay(𝔽(later.[[ISOYear]]), 𝔽(later.[[ISOMonth]] - 1), 𝔽(later.[[ISODay]])).
    auto epoch_days_2 = make_day(later.iso_year(), later.iso_month() - 1, later.iso_day());

    // 4. Assert: epochDays2 is finite.
    VERIFY(isfinite(epoch_days_2));

    // 5. Return ℝ(epochDays2) - ℝ(epochDays1).
    return epoch_days_2 - epoch_days_1;
}

}
