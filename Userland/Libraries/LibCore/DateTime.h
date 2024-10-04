/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <LibIPC/Forward.h>
#include <time.h>

namespace Core {

// Represents a time in local time.
class DateTime {
public:
    time_t timestamp() const { return m_timestamp; }

    unsigned year() const { return m_year; }
    unsigned month() const { return m_month; }
    unsigned day() const { return m_day; }

    unsigned hour() const { return m_hour; }
    unsigned minute() const { return m_minute; }
    unsigned second() const { return m_second; }
    unsigned weekday() const;
    unsigned days_in_month() const;
    unsigned day_of_year() const;
    bool is_leap_year() const;

    void set_time(int year, int month = 1, int day = 1, int hour = 0, int minute = 0, int second = 0);
    void set_time_only(int hour, int minute, Optional<int> second = {});
    void set_date(Core::DateTime const& other);

    enum class LocalTime {
        Yes,
        No,
    };

    ErrorOr<String> to_string(StringView format = "%Y-%m-%d %H:%M:%S"sv, LocalTime = LocalTime::Yes) const;
    ByteString to_byte_string(StringView format = "%Y-%m-%d %H:%M:%S"sv, LocalTime = LocalTime::Yes) const;

    static DateTime create(int year, int month = 1, int day = 1, int hour = 0, int minute = 0, int second = 0);
    static DateTime now();
    static DateTime from_timestamp(time_t);
    static Optional<DateTime> parse(StringView format, StringView string);

    bool operator<(DateTime const& other) const { return m_timestamp < other.m_timestamp; }
    bool operator>(DateTime const& other) const { return m_timestamp > other.m_timestamp; }
    bool operator<=(DateTime const& other) const { return m_timestamp <= other.m_timestamp; }
    bool operator>=(DateTime const& other) const { return m_timestamp >= other.m_timestamp; }
    bool operator==(DateTime const& other) const { return m_timestamp == other.m_timestamp; }

private:
    time_t m_timestamp { 0 };
    int m_year { 0 };
    int m_month { 0 };
    int m_day { 0 };
    int m_hour { 0 };
    int m_minute { 0 };
    int m_second { 0 };
};

}

namespace AK {
template<>
struct Formatter<Core::DateTime> : StandardFormatter {
    ErrorOr<void> format(FormatBuilder& builder, Core::DateTime const& value)
    {
        // Can't use DateTime::to_string() here: It doesn't propagate allocation failure.
        return builder.builder().try_appendff("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}"sv,
            value.year(), value.month(), value.day(),
            value.hour(), value.minute(), value.second());
    }
};
}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Core::DateTime const&);

template<>
ErrorOr<Core::DateTime> decode(Decoder&);

}
