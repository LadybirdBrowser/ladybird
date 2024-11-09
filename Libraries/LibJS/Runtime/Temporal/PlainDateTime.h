/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Duration.h>

namespace JS::Temporal {

class PlainDateTime final : public Object {
    JS_OBJECT(PlainDateTime, Object);
    JS_DECLARE_ALLOCATOR(PlainDateTime);

public:
    virtual ~PlainDateTime() override = default;

    [[nodiscard]] i32 iso_year() const { return m_iso_year; }
    [[nodiscard]] u8 iso_month() const { return m_iso_month; }
    [[nodiscard]] u8 iso_day() const { return m_iso_day; }
    [[nodiscard]] u8 iso_hour() const { return m_iso_hour; }
    [[nodiscard]] u8 iso_minute() const { return m_iso_minute; }
    [[nodiscard]] u8 iso_second() const { return m_iso_second; }
    [[nodiscard]] u16 iso_millisecond() const { return m_iso_millisecond; }
    [[nodiscard]] u16 iso_microsecond() const { return m_iso_microsecond; }
    [[nodiscard]] u16 iso_nanosecond() const { return m_iso_nanosecond; }
    [[nodiscard]] Object const& calendar() const { return m_calendar; }
    [[nodiscard]] Object& calendar() { return m_calendar; }

private:
    PlainDateTime(i32 iso_year, u8 iso_month, u8 iso_day, u8 iso_hour, u8 iso_minute, u8 iso_second, u16 iso_millisecond, u16 iso_microsecond, u16 iso_nanosecond, Object& calendar, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    // 5.4 Properties of Temporal.PlainDateTime Instances, https://tc39.es/proposal-temporal/#sec-properties-of-temporal-plaindatetime-instances
    i32 m_iso_year { 0 };            // [[ISOYear]]
    u8 m_iso_month { 0 };            // [[ISOMonth]]
    u8 m_iso_day { 0 };              // [[ISODay]]
    u8 m_iso_hour { 0 };             // [[ISOHour]]
    u8 m_iso_minute { 0 };           // [[ISOMinute]]
    u8 m_iso_second { 0 };           // [[ISOSecond]]
    u16 m_iso_millisecond { 0 };     // [[ISOMillisecond]]
    u16 m_iso_microsecond { 0 };     // [[ISOMicrosecond]]
    u16 m_iso_nanosecond { 0 };      // [[ISONanosecond]]
    NonnullGCPtr<Object> m_calendar; // [[Calendar]]
};

// Used by AddDateTime to temporarily hold values
struct TemporalPlainDateTime {
    i32 year;
    u8 month;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
    u16 millisecond;
    u16 microsecond;
    u16 nanosecond;
};

bool iso_date_time_within_limits(i32 year, u8 month, u8 day, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond);
ThrowCompletionOr<ISODateTime> interpret_temporal_date_time_fields(VM&, Object& calendar, Object& fields, Object const* options);
ThrowCompletionOr<PlainDateTime*> to_temporal_date_time(VM&, Value item, Object const* options = nullptr);
ISODateTime balance_iso_date_time(i32 year, u8 month, u8 day, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, i64 nanosecond);
ThrowCompletionOr<PlainDateTime*> create_temporal_date_time(VM&, i32 iso_year, u8 iso_month, u8 iso_day, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, Object& calendar, FunctionObject const* new_target = nullptr);
ThrowCompletionOr<String> temporal_date_time_to_string(VM&, i32 iso_year, u8 iso_month, u8 iso_day, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, Object const* calendar, Variant<StringView, u8> const& precision, StringView show_calendar);
i8 compare_iso_date_time(i32 year1, u8 month1, u8 day1, u8 hour1, u8 minute1, u8 second1, u16 millisecond1, u16 microsecond1, u16 nanosecond1, i32 year2, u8 month2, u8 day2, u8 hour2, u8 minute2, u8 second2, u16 millisecond2, u16 microsecond2, u16 nanosecond2);
ThrowCompletionOr<TemporalPlainDateTime> add_date_time(VM&, i32 year, u8 month, u8 day, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, Object& calendar, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, Object* options);
ISODateTime round_iso_date_time(i32 year, u8 month, u8 day, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, u64 increment, StringView unit, StringView rounding_mode, Optional<double> day_length = {});
ThrowCompletionOr<DurationRecord> difference_iso_date_time(VM&, i32 year1, u8 month1, u8 day1, u8 hour1, u8 minute1, u8 second1, u16 millisecond1, u16 microsecond1, u16 nanosecond1, i32 year2, u8 month2, u8 day2, u8 hour2, u8 minute2, u8 second2, u16 millisecond2, u16 microsecond2, u16 nanosecond2, Object& calendar, StringView largest_unit, Object const& options);
ThrowCompletionOr<NonnullGCPtr<Duration>> difference_temporal_plain_date_time(VM&, DifferenceOperation, PlainDateTime&, Value other, Value options);
ThrowCompletionOr<PlainDateTime*> add_duration_to_or_subtract_duration_from_plain_date_time(VM&, ArithmeticOperation, PlainDateTime&, Value temporal_duration_like, Value options_value);

}
