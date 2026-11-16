/*
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Object.h>
#include <LibUnicode/TimeZone.h>

namespace JS {

class JS_API Date final : public Object {
public:
    static GC::Ref<Date> create(Realm&, double date_value);

    double date_value() const;

    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_DATE; }
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

// https://tc39.es/ecma262/#sec-time-values-and-time-range
// A time value supports a [...] range of -8,640,000,000,000,000 to 8,640,000,000,000,000 milliseconds
constexpr inline double max_time_value = 8.64E15;

JS_API i32 year_from_time(double);
JS_API u8 month_from_time(double);
JS_API u8 date_from_time(double);
JS_API u8 hour_from_time(double);
JS_API u8 min_from_time(double);
JS_API u8 sec_from_time(double);
JS_API u16 ms_from_time(double);
JS_API void clear_system_time_zone_cache();
JS_API double make_time(double hour, double min, double sec, double ms);
JS_API double make_day(double year, double month, double date);
JS_API double make_date(double day, double time);

}
