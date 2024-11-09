/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/VM.h>

namespace JS::Temporal {

class PlainTime final : public Object {
    JS_OBJECT(PlainTime, Object);
    JS_DECLARE_ALLOCATOR(PlainTime);

public:
    virtual ~PlainTime() override = default;

    [[nodiscard]] u8 iso_hour() const { return m_iso_hour; }
    [[nodiscard]] u8 iso_minute() const { return m_iso_minute; }
    [[nodiscard]] u8 iso_second() const { return m_iso_second; }
    [[nodiscard]] u16 iso_millisecond() const { return m_iso_millisecond; }
    [[nodiscard]] u16 iso_microsecond() const { return m_iso_microsecond; }
    [[nodiscard]] u16 iso_nanosecond() const { return m_iso_nanosecond; }
    [[nodiscard]] Calendar const& calendar() const { return m_calendar; }
    [[nodiscard]] Calendar& calendar() { return m_calendar; }

private:
    PlainTime(u8 iso_hour, u8 iso_minute, u8 iso_second, u16 iso_millisecond, u16 iso_microsecond, u16 iso_nanosecond, Calendar& calendar, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    // 4.4 Properties of Temporal.PlainTime Instances, https://tc39.es/proposal-temporal/#sec-properties-of-temporal-plaintime-instances
    u8 m_iso_hour { 0 };               // [[ISOHour]]
    u8 m_iso_minute { 0 };             // [[ISOMinute]]
    u8 m_iso_second { 0 };             // [[ISOSecond]]
    u16 m_iso_millisecond { 0 };       // [[ISOMillisecond]]
    u16 m_iso_microsecond { 0 };       // [[ISOMicrosecond]]
    u16 m_iso_nanosecond { 0 };        // [[ISONanosecond]]
    NonnullGCPtr<Calendar> m_calendar; // [[Calendar]] (always the built-in ISO 8601 calendar)
};

struct DaysAndTime {
    i32 days;
    u8 hour;
    u8 minute;
    u8 second;
    u16 millisecond;
    u16 microsecond;
    u16 nanosecond;
};

struct TemporalTimeLikeRecord {
    Optional<double> hour;
    Optional<double> minute;
    Optional<double> second;
    Optional<double> millisecond;
    Optional<double> microsecond;
    Optional<double> nanosecond;
};

// Table 4: TemporalTimeLike Record Fields, https://tc39.es/proposal-temporal/#table-temporal-temporaltimelike-properties

template<typename StructT, typename ValueT>
struct TemporalTimeLikeRecordField {
    ValueT StructT::*field_name { nullptr };
    PropertyKey property_name;
};

enum class ToTemporalTimeRecordCompleteness {
    Partial,
    Complete,
};

TimeDurationRecord difference_time(VM&, u8 hour1, u8 minute1, u8 second1, u16 millisecond1, u16 microsecond1, u16 nanosecond1, u8 hour2, u8 minute2, u8 second2, u16 millisecond2, u16 microsecond2, u16 nanosecond2);
ThrowCompletionOr<PlainTime*> to_temporal_time(VM&, Value item, Optional<StringView> overflow = {});
ThrowCompletionOr<TemporalTime> regulate_time(VM&, double hour, double minute, double second, double millisecond, double microsecond, double nanosecond, StringView overflow);
bool is_valid_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond);
DaysAndTime balance_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond);
TemporalTime constrain_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond);
ThrowCompletionOr<PlainTime*> create_temporal_time(VM&, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, FunctionObject const* new_target = nullptr);
ThrowCompletionOr<TemporalTimeLikeRecord> to_temporal_time_record(VM&, Object const& temporal_time_like, ToTemporalTimeRecordCompleteness = ToTemporalTimeRecordCompleteness::Complete);
ThrowCompletionOr<String> temporal_time_to_string(VM&, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, Variant<StringView, u8> const& precision);
i8 compare_temporal_time(u8 hour1, u8 minute1, u8 second1, u16 millisecond1, u16 microsecond1, u16 nanosecond1, u8 hour2, u8 minute2, u8 second2, u16 millisecond2, u16 microsecond2, u16 nanosecond2);
DaysAndTime add_time(u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds);
DaysAndTime round_time(u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, u64 increment, StringView unit, StringView rounding_mode, Optional<double> day_length_ns = {});
ThrowCompletionOr<NonnullGCPtr<Duration>> difference_temporal_plain_time(VM&, DifferenceOperation, PlainTime const&, Value other, Value options);
ThrowCompletionOr<PlainTime*> add_duration_to_or_subtract_duration_from_plain_time(VM&, ArithmeticOperation, PlainTime const&, Value temporal_duration_like);

}
