/*
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibJS/Runtime/Object.h>
#include <LibUnicode/TimeZone.h>

namespace JS {

class Date final : public Object {
    JS_OBJECT(Date, Object);
    GC_DECLARE_ALLOCATOR(Date);

public:
    static GC::Ref<Date> create(Realm&, double date_value);

    // Out of line to ensure we have a key function
    virtual ~Date() override;

    double date_value() const { return m_date_value; }
    void set_date_value(double value) { m_date_value = value; }

    ErrorOr<String> iso_date_string() const;

private:
    Date(double date_value, Object& prototype);

    virtual bool is_date() const final { return true; }

    double m_date_value { 0 }; // [[DateValue]]
};

template<>
inline bool Object::fast_is<Date>() const { return is_date(); }

// 21.4.1.22 Time Zone Identifier Record, https://tc39.es/ecma262/#sec-time-zone-identifier-record
struct TimeZoneIdentifier {
    String identifier;         // [[Identifier]]
    String primary_identifier; // [[PrimaryIdentifier]]
};

// https://tc39.es/ecma262/#eqn-HoursPerDay
constexpr inline double hours_per_day = 24;
// https://tc39.es/ecma262/#eqn-MinutesPerHour
constexpr inline double minutes_per_hour = 60;
// https://tc39.es/ecma262/#eqn-SecondsPerMinute
constexpr inline double seconds_per_minute = 60;
// https://tc39.es/ecma262/#eqn-msPerSecond
constexpr inline double ms_per_second = 1'000;
// https://tc39.es/ecma262/#eqn-msPerMinute
constexpr inline double ms_per_minute = 60'000;
// https://tc39.es/ecma262/#eqn-msPerHour
constexpr inline double ms_per_hour = 3'600'000;
// https://tc39.es/ecma262/#eqn-msPerDay
constexpr inline double ms_per_day = 86'400'000;
// https://tc39.es/proposal-temporal/#eqn-nsPerDay
constexpr inline double ns_per_day = 86'400'000'000'000;
extern Crypto::SignedBigInteger const ns_per_day_bigint;

double day(double);
double time_within_day(double);
u16 days_in_year(i32);
double day_from_year(i32);
double time_from_year(i32);
i32 year_from_time(double);
u16 day_within_year(double);
bool in_leap_year(double);
u8 month_from_time(double);
u8 date_from_time(double);
u8 week_day(double);
u8 hour_from_time(double);
u8 min_from_time(double);
u8 sec_from_time(double);
u16 ms_from_time(double);
Crypto::SignedBigInteger get_utc_epoch_nanoseconds(Temporal::ISODateTime const&);
Vector<Crypto::SignedBigInteger> get_named_time_zone_epoch_nanoseconds(StringView time_zone_identifier, Temporal::ISODateTime const&);
Unicode::TimeZoneOffset get_named_time_zone_offset_nanoseconds(StringView time_zone_identifier, Crypto::SignedBigInteger const& epoch_nanoseconds);
Unicode::TimeZoneOffset get_named_time_zone_offset_milliseconds(StringView time_zone_identifier, double epoch_milliseconds);
String system_time_zone_identifier();
void clear_system_time_zone_cache();
double local_time(double time);
double utc_time(double time);
double make_time(double hour, double min, double sec, double ms);
double make_day(double year, double month, double date);
double make_date(double day, double time);
double time_clip(double time);
bool is_offset_time_zone_identifier(StringView offset_string);
ThrowCompletionOr<double> parse_date_time_utc_offset(VM&, StringView offset_string);
double parse_date_time_utc_offset(StringView offset_string);
double parse_date_time_utc_offset(Temporal::TimeZoneOffset const&);

}
