/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

namespace JS::Temporal {

class ZonedDateTimePrototype final : public PrototypeObject<ZonedDateTimePrototype, ZonedDateTime> {
    JS_PROTOTYPE_OBJECT(ZonedDateTimePrototype, ZonedDateTime, Temporal.ZonedDateTime);
    GC_DECLARE_ALLOCATOR(ZonedDateTimePrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~ZonedDateTimePrototype() override = default;

private:
    explicit ZonedDateTimePrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(calendar_id_getter);
    JS_DECLARE_NATIVE_FUNCTION(time_zone_id_getter);
    JS_DECLARE_NATIVE_FUNCTION(era_getter);
    JS_DECLARE_NATIVE_FUNCTION(era_year_getter);
    JS_DECLARE_NATIVE_FUNCTION(year_getter);
    JS_DECLARE_NATIVE_FUNCTION(month_getter);
    JS_DECLARE_NATIVE_FUNCTION(month_code_getter);
    JS_DECLARE_NATIVE_FUNCTION(day_getter);
    JS_DECLARE_NATIVE_FUNCTION(hour_getter);
    JS_DECLARE_NATIVE_FUNCTION(minute_getter);
    JS_DECLARE_NATIVE_FUNCTION(second_getter);
    JS_DECLARE_NATIVE_FUNCTION(millisecond_getter);
    JS_DECLARE_NATIVE_FUNCTION(microsecond_getter);
    JS_DECLARE_NATIVE_FUNCTION(nanosecond_getter);
    JS_DECLARE_NATIVE_FUNCTION(epoch_milliseconds_getter);
    JS_DECLARE_NATIVE_FUNCTION(epoch_nanoseconds_getter);
    JS_DECLARE_NATIVE_FUNCTION(day_of_week_getter);
    JS_DECLARE_NATIVE_FUNCTION(day_of_year_getter);
    JS_DECLARE_NATIVE_FUNCTION(week_of_year_getter);
    JS_DECLARE_NATIVE_FUNCTION(year_of_week_getter);
    JS_DECLARE_NATIVE_FUNCTION(hours_in_day_getter);
    JS_DECLARE_NATIVE_FUNCTION(days_in_week_getter);
    JS_DECLARE_NATIVE_FUNCTION(days_in_month_getter);
    JS_DECLARE_NATIVE_FUNCTION(days_in_year_getter);
    JS_DECLARE_NATIVE_FUNCTION(months_in_year_getter);
    JS_DECLARE_NATIVE_FUNCTION(in_leap_year_getter);
    JS_DECLARE_NATIVE_FUNCTION(offset_nanoseconds_getter);
    JS_DECLARE_NATIVE_FUNCTION(offset_getter);
    JS_DECLARE_NATIVE_FUNCTION(value_of);
};

}
