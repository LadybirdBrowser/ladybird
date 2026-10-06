/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<Date> Date::create(Realm& realm, double date_value)
{
    return static_cast<Date&>(object_from_abi(js_date_create(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), date_value)));
}

double Date::date_value() const
{
    return js_date_date_value(object_to_abi(*this));
}

i32 year_from_time(double time)
{
    return js_date_year_from_time(time);
}

u8 month_from_time(double time)
{
    return js_date_month_from_time(time);
}

u8 date_from_time(double time)
{
    return js_date_date_from_time(time);
}

u8 hour_from_time(double time)
{
    return js_date_hour_from_time(time);
}

u8 min_from_time(double time)
{
    return js_date_min_from_time(time);
}

u8 sec_from_time(double time)
{
    return js_date_sec_from_time(time);
}

u16 ms_from_time(double time)
{
    return js_date_ms_from_time(time);
}

void clear_system_time_zone_cache()
{
    js_vm_clear_system_time_zone_cache();
}

double make_time(double hour, double min, double sec, double ms)
{
    return js_date_make_time(hour, min, sec, ms);
}

double make_day(double year, double month, double date)
{
    return js_date_make_day(year, month, date);
}

double make_date(double day, double time)
{
    return js_date_make_date(day, time);
}

}
