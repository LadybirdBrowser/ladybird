/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Petróczi Zoltán <petroczizoltan@tutanota.com>
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/DateConstants.h>
#include <AK/Function.h>
#include <AK/TypeCasts.h>
#include <LibCore/DateTime.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/DatePrototype.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/DateTimeFormat.h>
#include <LibJS/Runtime/Intl/DateTimeFormatConstructor.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibUnicode/DisplayNames.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/TimeZone.h>

namespace JS {

GC_DEFINE_ALLOCATOR(DatePrototype);

DatePrototype::DatePrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void DatePrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.getDate, get_date, 0, attr);
    define_native_function(realm, vm.names.getDay, get_day, 0, attr);
    define_native_function(realm, vm.names.getFullYear, get_full_year, 0, attr);
    define_native_function(realm, vm.names.getHours, get_hours, 0, attr);
    define_native_function(realm, vm.names.getMilliseconds, get_milliseconds, 0, attr);
    define_native_function(realm, vm.names.getMinutes, get_minutes, 0, attr);
    define_native_function(realm, vm.names.getMonth, get_month, 0, attr);
    define_native_function(realm, vm.names.getSeconds, get_seconds, 0, attr);
    define_native_function(realm, vm.names.getTime, get_time, 0, attr);
    define_native_function(realm, vm.names.getTimezoneOffset, get_timezone_offset, 0, attr);
    define_native_function(realm, vm.names.getUTCDate, get_utc_date, 0, attr);
    define_native_function(realm, vm.names.getUTCDay, get_utc_day, 0, attr);
    define_native_function(realm, vm.names.getUTCFullYear, get_utc_full_year, 0, attr);
    define_native_function(realm, vm.names.getUTCHours, get_utc_hours, 0, attr);
    define_native_function(realm, vm.names.getUTCMilliseconds, get_utc_milliseconds, 0, attr);
    define_native_function(realm, vm.names.getUTCMinutes, get_utc_minutes, 0, attr);
    define_native_function(realm, vm.names.getUTCMonth, get_utc_month, 0, attr);
    define_native_function(realm, vm.names.getUTCSeconds, get_utc_seconds, 0, attr);
    define_native_function(realm, vm.names.setDate, set_date, 1, attr);
    define_native_function(realm, vm.names.setFullYear, set_full_year, 3, attr);
    define_native_function(realm, vm.names.setHours, set_hours, 4, attr);
    define_native_function(realm, vm.names.setMilliseconds, set_milliseconds, 1, attr);
    define_native_function(realm, vm.names.setMinutes, set_minutes, 3, attr);
    define_native_function(realm, vm.names.setMonth, set_month, 2, attr);
    define_native_function(realm, vm.names.setSeconds, set_seconds, 2, attr);
    define_native_function(realm, vm.names.setTime, set_time, 1, attr);
    define_native_function(realm, vm.names.setUTCDate, set_utc_date, 1, attr);
    define_native_function(realm, vm.names.setUTCFullYear, set_utc_full_year, 3, attr);
    define_native_function(realm, vm.names.setUTCHours, set_utc_hours, 4, attr);
    define_native_function(realm, vm.names.setUTCMilliseconds, set_utc_milliseconds, 1, attr);
    define_native_function(realm, vm.names.setUTCMinutes, set_utc_minutes, 3, attr);
    define_native_function(realm, vm.names.setUTCMonth, set_utc_month, 2, attr);
    define_native_function(realm, vm.names.setUTCSeconds, set_utc_seconds, 2, attr);
    define_native_function(realm, vm.names.toDateString, to_date_string, 0, attr);
    define_native_function(realm, vm.names.toISOString, to_iso_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 1, attr);
    define_native_function(realm, vm.names.toLocaleDateString, to_locale_date_string, 0, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toLocaleTimeString, to_locale_time_string, 0, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toTimeString, to_time_string, 0, attr);
    define_native_function(realm, vm.names.toUTCString, to_utc_string, 0, attr);

    define_native_function(realm, vm.names.toTemporalInstant, to_temporal_instant, 0, attr);

    define_native_function(realm, vm.names.getYear, get_year, 0, attr);
    define_native_function(realm, vm.names.setYear, set_year, 1, attr);

    // 21.4.4.45 Date.prototype [ @@toPrimitive ] ( hint ), https://tc39.es/ecma262/#sec-date.prototype-@@toprimitive
    define_native_function(realm, vm.well_known_symbol_to_primitive(), symbol_to_primitive, 1, Attribute::Configurable);

    // Aliases.
    define_native_function(realm, vm.names.valueOf, get_time, 0, attr);

    // B.2.4.3 Date.prototype.toGMTString ( ), https://tc39.es/ecma262/#sec-date.prototype.togmtstring
    // The initial value of the "toGMTString" property is %Date.prototype.toUTCString%, defined in 21.4.4.43.
    define_direct_property(vm.names.toGMTString, get_without_side_effects(vm.names.toUTCString), attr);
}

// thisTimeValue ( value ), https://tc39.es/ecma262/#thistimevalue
ThrowCompletionOr<double> this_time_value(VM& vm, Value value)
{
    // 1. If Type(value) is Object and value has a [[DateValue]] internal slot, then
    if (value.is_object() && is<Date>(value.as_object())) {
        // a. Return value.[[DateValue]].
        return static_cast<Date&>(value.as_object()).date_value();
    }

    // 2. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "Date");
}

// 21.4.4.2 Date.prototype.getDate ( ), https://tc39.es/ecma262/#sec-date.prototype.getdate
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_date)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return DateFromTime(LocalTime(t)).
    return Value(date_from_time(local_time(time)));
}

// 21.4.4.3 Date.prototype.getDay ( ), https://tc39.es/ecma262/#sec-date.prototype.getday
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_day)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return WeekDay(LocalTime(t)).
    return Value(week_day(local_time(time)));
}

// 21.4.4.4 Date.prototype.getFullYear ( ), https://tc39.es/ecma262/#sec-date.prototype.getfullyear
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_full_year)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return YearFromTime(LocalTime(t)).
    return Value(year_from_time(local_time(time)));
}

// 21.4.4.5 Date.prototype.getHours ( ), https://tc39.es/ecma262/#sec-date.prototype.gethours
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_hours)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return HourFromTime(LocalTime(t)).
    return Value(hour_from_time(local_time(time)));
}

// 21.4.4.6 Date.prototype.getMilliseconds ( ), https://tc39.es/ecma262/#sec-date.prototype.getmilliseconds
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_milliseconds)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return msFromTime(LocalTime(t)).
    return Value(ms_from_time(local_time(time)));
}

// 21.4.4.7 Date.prototype.getMinutes ( ), https://tc39.es/ecma262/#sec-date.prototype.getminutes
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_minutes)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return MinFromTime(LocalTime(t)).
    return Value(min_from_time(local_time(time)));
}

// 21.4.4.8 Date.prototype.getMonth ( ), https://tc39.es/ecma262/#sec-date.prototype.getmonth
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_month)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return MonthFromTime(LocalTime(t)).
    return Value(month_from_time(local_time(time)));
}

// 21.4.4.9 Date.prototype.getSeconds ( ), https://tc39.es/ecma262/#sec-date.prototype.getseconds
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_seconds)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return SecFromTime(LocalTime(t)).
    return Value(sec_from_time(local_time(time)));
}

// 21.4.4.10 Date.prototype.getTime ( ), https://tc39.es/ecma262/#sec-date.prototype.gettime
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_time)
{
    // 1. Return ? thisTimeValue(this value).
    return Value(TRY(this_time_value(vm, vm.this_value())));
}

// 21.4.4.11 Date.prototype.getTimezoneOffset ( ), https://tc39.es/ecma262/#sec-date.prototype.gettimezoneoffset
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_timezone_offset)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return (t - LocalTime(t)) / msPerMinute.
    return Value((time - local_time(time)) / ms_per_minute);
}

// 21.4.4.12 Date.prototype.getUTCDate ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcdate
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_utc_date)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return DateFromTime(t).
    return Value(date_from_time(time));
}

// 21.4.4.13 Date.prototype.getUTCDay ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcday
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_utc_day)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return WeekDay(t).
    return Value(week_day(time));
}

// 21.4.4.14 Date.prototype.getUTCFullYear ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcfullyear
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_utc_full_year)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return YearFromTime(t).
    return Value(year_from_time(time));
}

// 21.4.4.15 Date.prototype.getUTCHours ( ), https://tc39.es/ecma262/#sec-date.prototype.getutchours
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_utc_hours)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return HourFromTime(t).
    return Value(hour_from_time(time));
}

// 21.4.4.16 Date.prototype.getUTCMilliseconds ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcmilliseconds
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_utc_milliseconds)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return msFromTime(t).
    return Value(ms_from_time(time));
}

// 21.4.4.17 Date.prototype.getUTCMinutes ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcminutes
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_utc_minutes)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return MinFromTime(t).
    return Value(min_from_time(time));
}

// 21.4.4.18 Date.prototype.getUTCMonth ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcmonth
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_utc_month)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return MonthFromTime(t).
    return month_from_time(time);
}

// 21.4.4.19 Date.prototype.getUTCSeconds ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcseconds
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_utc_seconds)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return SecFromTime(t).
    return Value(sec_from_time(time));
}

static ThrowCompletionOr<double> argument_or_number(VM& vm, size_t index, double fallback)
{
    if (vm.argument_count() > index)
        return TRY(vm.argument(index).to_number(vm)).as_double();

    return fallback;
}

static ThrowCompletionOr<Optional<double>> argument_or_empty(VM& vm, size_t index)
{
    if (vm.argument_count() > index)
        return TRY(vm.argument(index).to_number(vm)).as_double();

    return Optional<double> {};
}

// 21.4.4.20 Date.prototype.setDate ( date ), https://tc39.es/ecma262/#sec-date.prototype.setdate
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_date)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto this_time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Let dt be ? ToNumber(date).
    auto date = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. If t is NaN, return NaN.
    if (isnan(this_time))
        return js_nan();

    // 4. Set t to LocalTime(t).
    auto time = local_time(this_time);

    // 5. Let newDate be MakeDate(MakeDay(YearFromTime(t), MonthFromTime(t), dt), TimeWithinDay(t)).
    auto year = year_from_time(time);
    auto month = month_from_time(time);

    auto day = make_day(year, month, date);
    auto new_date = make_date(day, time_within_day(time));

    // 6. Let u be TimeClip(UTC(newDate)).
    new_date = time_clip(utc_time(new_date));

    // 7. Set the [[DateValue]] internal slot of this Date object to u.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(new_date);

    // 8. Return u.
    return Value(new_date);
}

// 21.4.4.21 Date.prototype.setFullYear ( year [ , month [ , date ] ] ), https://tc39.es/ecma262/#sec-date.prototype.setfullyear
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_full_year)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto this_time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Let y be ? ToNumber(year).
    auto year = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. If t is NaN, set t to +0𝔽; otherwise, set t to LocalTime(t).
    double time = 0;
    if (!isnan(this_time))
        time = local_time(this_time);

    // 4. If month is not present, let m be MonthFromTime(t); otherwise, let m be ? ToNumber(month).
    auto month = TRY(argument_or_number(vm, 1, month_from_time(time)));

    // 5. If date is not present, let dt be DateFromTime(t); otherwise, let dt be ? ToNumber(date).
    auto date = TRY(argument_or_number(vm, 2, date_from_time(time)));

    // 6. Let newDate be MakeDate(MakeDay(y, m, dt), TimeWithinDay(t)).
    auto day = make_day(year, month, date);
    auto new_date = make_date(day, time_within_day(time));

    // 7. Let u be TimeClip(UTC(newDate)).
    new_date = time_clip(utc_time(new_date));

    // 8. Set the [[DateValue]] internal slot of this Date object to u.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(new_date);

    // 9. Return u.
    return Value(new_date);
}

// 21.4.4.22 Date.prototype.setHours ( hour [ , min [ , sec [ , ms ] ] ] ), https://tc39.es/ecma262/#sec-date.prototype.sethours
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_hours)
{
    // 1. Let t be LocalTime(? thisTimeValue(this value)).
    auto this_time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Let h be ? ToNumber(hour).
    auto hour = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. If min is present, let m be ? ToNumber(min).
    auto minute = TRY(argument_or_empty(vm, 1));

    // 4. If sec is present, let s be ? ToNumber(sec).
    auto second = TRY(argument_or_empty(vm, 2));

    // 5. If ms is present, let milli be ? ToNumber(ms).
    auto millisecond = TRY(argument_or_empty(vm, 3));

    // 6. If t is NaN, return NaN.
    if (isnan(this_time))
        return js_nan();

    // 7. Set t to LocalTime(t).
    auto time = local_time(this_time);

    // 8. If min is not present, let m be MinFromTime(t).
    if (!minute.has_value())
        minute = min_from_time(time);

    // 9. If sec is not present, let s be SecFromTime(t).
    if (!second.has_value())
        second = sec_from_time(time);

    // 10. If ms is not present, let milli be msFromTime(t).
    if (!millisecond.has_value())
        millisecond = ms_from_time(time);

    // 11. Let date be MakeDate(Day(t), MakeTime(h, m, s, milli)).
    auto new_time = make_time(hour, *minute, *second, *millisecond);
    auto date = make_date(day(time), new_time);

    // 12. Let u be TimeClip(UTC(date)).
    date = time_clip(utc_time(date));

    // 13. Set the [[DateValue]] internal slot of this Date object to u.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(date);

    // 14. Return u.
    return Value(date);
}

// 21.4.4.23 Date.prototype.setMilliseconds ( ms ), https://tc39.es/ecma262/#sec-date.prototype.setmilliseconds
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_milliseconds)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto this_time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Set ms to ? ToNumber(ms).
    auto millisecond = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. If t is NaN, return NaN.
    if (isnan(this_time))
        return js_nan();

    // 4. Set t to LocalTime(t).
    auto time = local_time(this_time);

    // 5. Let time be MakeTime(HourFromTime(t), MinFromTime(t), SecFromTime(t), ms).
    auto hour = hour_from_time(time);
    auto minute = min_from_time(time);
    auto second = sec_from_time(time);

    auto new_time = make_time(hour, minute, second, millisecond);

    // 6. Let u be TimeClip(UTC(MakeDate(Day(t), time))).
    auto date = make_date(day(time), new_time);
    date = time_clip(utc_time(date));

    // 7. Set the [[DateValue]] internal slot of this Date object to u.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(date);

    // 8. Return u.
    return Value(date);
}

// 21.4.4.24 Date.prototype.setMinutes ( min [ , sec [ , ms ] ] ), https://tc39.es/ecma262/#sec-date.prototype.setminutes
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_minutes)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto this_time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Let m be ? ToNumber(min).
    auto minute = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. If sec is present, let s be ? ToNumber(sec).
    auto second = TRY(argument_or_empty(vm, 1));

    // 4. If ms is present, let milli be ? ToNumber(ms).
    auto millisecond = TRY(argument_or_empty(vm, 2));

    // 5. If t is NaN, return NaN.
    if (isnan(this_time))
        return js_nan();

    // 6. Set t to LocalTime(t).
    auto time = local_time(this_time);

    // 7. If sec is not present, let s be SecFromTime(t).
    if (!second.has_value())
        second = sec_from_time(time);

    // 8. If ms is not present, let milli be msFromTime(t).
    if (!millisecond.has_value())
        millisecond = ms_from_time(time);

    // 9. Let date be MakeDate(Day(t), MakeTime(HourFromTime(t), m, s, milli)).
    auto hour = hour_from_time(time);

    auto new_time = make_time(hour, minute, *second, *millisecond);
    auto date = make_date(day(time), new_time);

    // 10. Let u be TimeClip(UTC(date)).
    date = time_clip(utc_time(date));

    // 11. Set the [[DateValue]] internal slot of this Date object to u.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(date);

    // 12. Return u.
    return Value(date);
}

// 21.4.4.25 Date.prototype.setMonth ( month [ , date ] ), https://tc39.es/ecma262/#sec-date.prototype.setmonth
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_month)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto this_time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Let m be ? ToNumber(month).
    auto month = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. If date is present, let dt be ? ToNumber(date).
    auto date = TRY(argument_or_empty(vm, 1));

    // 4. If t is NaN, return NaN.
    if (isnan(this_time))
        return js_nan();

    // 5. Set t to LocalTime(t).
    auto time = local_time(this_time);

    // 6. If date is not present, let dt be DateFromTime(t).
    if (!date.has_value())
        date = date_from_time(time);

    // 7. Let newDate be MakeDate(MakeDay(YearFromTime(t), m, dt), TimeWithinDay(t)).
    auto year = year_from_time(time);

    auto day = make_day(year, month, *date);
    auto new_date = make_date(day, time_within_day(time));

    // 8. Let u be TimeClip(UTC(newDate)).
    new_date = time_clip(utc_time(new_date));

    // 9. Set the [[DateValue]] internal slot of this Date object to u.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(new_date);

    // 10. Return u.
    return Value(new_date);
}

// 21.4.4.26 Date.prototype.setSeconds ( sec [ , ms ] ), https://tc39.es/ecma262/#sec-date.prototype.setseconds
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_seconds)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto this_time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Let s be ? ToNumber(sec).
    auto second = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. If ms is present, let milli be ? ToNumber(ms).
    auto millisecond = TRY(argument_or_empty(vm, 1));

    // 4. If t is NaN, return NaN.
    if (isnan(this_time))
        return js_nan();

    // 5. Set t to LocalTime(t).
    auto time = local_time(this_time);

    // 6. If ms is not present, let milli be msFromTime(t).
    if (!millisecond.has_value())
        millisecond = ms_from_time(time);

    // 7. Let date be MakeDate(Day(t), MakeTime(HourFromTime(t), MinFromTime(t), s, milli)).
    auto hour = hour_from_time(time);
    auto minute = min_from_time(time);

    auto new_time = make_time(hour, minute, second, *millisecond);
    auto new_date = make_date(day(time), new_time);

    // 8. Let u be TimeClip(UTC(date)).
    new_date = time_clip(utc_time(new_date));

    // 9. Set the [[DateValue]] internal slot of this Date object to u.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(new_date);

    // 10. Return u.
    return Value(new_date);
}

// 21.4.4.27 Date.prototype.setTime ( time ), https://tc39.es/ecma262/#sec-date.prototype.settime
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_time)
{
    // 1. Perform ? thisTimeValue(this value).
    TRY(this_time_value(vm, vm.this_value()));

    // 2. Let t be ? ToNumber(time).
    auto time = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. Let v be TimeClip(t).
    time = time_clip(time);

    // 4. Set the [[DateValue]] internal slot of this Date object to v.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(time);

    // 5. Return v.
    return Value(time);
}

// 21.4.4.28 Date.prototype.setUTCDate ( date ), https://tc39.es/ecma262/#sec-date.prototype.setutcdate
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_utc_date)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Let dt be ? ToNumber(date).
    auto date = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 4. Let newDate be MakeDate(MakeDay(YearFromTime(t), MonthFromTime(t), dt), TimeWithinDay(t)).
    auto year = year_from_time(time);
    auto month = month_from_time(time);

    auto day = make_day(year, month, date);
    auto new_date = make_date(day, time_within_day(time));

    // 5. Let v be TimeClip(newDate).
    new_date = time_clip(new_date);

    // 6. Set the [[DateValue]] internal slot of this Date object to v.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(new_date);

    // 7. Return v.
    return Value(new_date);
}

// 21.4.4.29 Date.prototype.setUTCFullYear ( year [ , month [ , date ] ] ), https://tc39.es/ecma262/#sec-date.prototype.setutcfullyear
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_utc_full_year)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto this_time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, set t to +0𝔽.
    double time = 0;
    if (!isnan(this_time))
        time = this_time;

    // 3. Let y be ? ToNumber(year).
    auto year = TRY(vm.argument(0).to_number(vm)).as_double();

    // 4. If month is not present, let m be MonthFromTime(t); otherwise, let m be ? ToNumber(month).
    auto month = TRY(argument_or_number(vm, 1, month_from_time(time)));

    // 5. If date is not present, let dt be DateFromTime(t); otherwise, let dt be ? ToNumber(date).
    auto date = TRY(argument_or_number(vm, 2, date_from_time(time)));

    // 6. Let newDate be MakeDate(MakeDay(y, m, dt), TimeWithinDay(t)).
    auto day = make_day(year, month, date);
    auto new_date = make_date(day, time_within_day(time));

    // 7. Let v be TimeClip(newDate).
    new_date = time_clip(new_date);

    // 8. Set the [[DateValue]] internal slot of this Date object to v.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(new_date);

    // 9. Return v.
    return Value(new_date);
}

// 21.4.4.30 Date.prototype.setUTCHours ( hour [ , min [ , sec [ , ms ] ] ] ), https://tc39.es/ecma262/#sec-date.prototype.setutchours
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_utc_hours)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Let h be ? ToNumber(hour).
    auto hour = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. If min is present, let m be ? ToNumber(min).
    auto minute = TRY(argument_or_empty(vm, 1));

    // 4. If sec is present, let s be ? ToNumber(sec).
    auto second = TRY(argument_or_empty(vm, 2));

    // 5. If ms is present, let milli be ? ToNumber(ms).
    auto millisecond = TRY(argument_or_empty(vm, 3));

    // 6. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 7. If min is not present, let m be MinFromTime(t).
    if (!minute.has_value())
        minute = min_from_time(time);

    // 8. If sec is not present, let s be SecFromTime(t).
    if (!second.has_value())
        second = sec_from_time(time);

    // 9. If ms is not present, let milli be msFromTime(t).
    if (!millisecond.has_value())
        millisecond = ms_from_time(time);

    // 10. Let date be MakeDate(Day(t), MakeTime(h, m, s, milli)).
    auto new_time = make_time(hour, *minute, *second, *millisecond);
    auto date = make_date(day(time), new_time);

    // 11. Let v be TimeClip(date).
    date = time_clip(date);

    // 12. Set the [[DateValue]] internal slot of this Date object to v.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(date);

    // 13. Return v.
    return Value(date);
}

// 21.4.4.31 Date.prototype.setUTCMilliseconds ( ms ), https://tc39.es/ecma262/#sec-date.prototype.setutcmilliseconds
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_utc_milliseconds)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Set ms to ? ToNumber(ms).
    auto millisecond = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 4. Let time be MakeTime(HourFromTime(t), MinFromTime(t), SecFromTime(t), ms).
    auto hour = hour_from_time(time);
    auto minute = min_from_time(time);
    auto second = sec_from_time(time);

    auto new_time = make_time(hour, minute, second, millisecond);

    // 5. Let v be TimeClip(MakeDate(Day(t), time)).
    auto date = make_date(day(time), new_time);
    date = time_clip(date);

    // 6. Set the [[DateValue]] internal slot of this Date object to v.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(date);

    // 7. Return v.
    return Value(date);
}

// 21.4.4.32 Date.prototype.setUTCMinutes ( min [ , sec [ , ms ] ] ), https://tc39.es/ecma262/#sec-date.prototype.setutcminutes
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_utc_minutes)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Let m be ? ToNumber(min).
    auto minute = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. If sec is present, let s be ? ToNumber(sec).
    auto second = TRY(argument_or_empty(vm, 1));

    // 4. If ms is present, let milli be ? ToNumber(ms).
    auto millisecond = TRY(argument_or_empty(vm, 2));

    // 5. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 6. If sec is not present, let s be SecFromTime(t).
    if (!second.has_value())
        second = sec_from_time(time);

    // 7. If ms is not present, let milli be msFromTime(t).
    if (!millisecond.has_value())
        millisecond = ms_from_time(time);

    // 8. Let date be MakeDate(Day(t), MakeTime(HourFromTime(t), m, s, milli)).
    auto hour = hour_from_time(time);

    auto new_time = make_time(hour, minute, *second, *millisecond);
    auto date = make_date(day(time), new_time);

    // 9. Let v be TimeClip(date).
    date = time_clip(date);

    // 10. Set the [[DateValue]] internal slot of this Date object to v.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(date);

    // 11. Return v.
    return Value(date);
}

// 21.4.4.33 Date.prototype.setUTCMonth ( month [ , date ] ), https://tc39.es/ecma262/#sec-date.prototype.setutcmonth
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_utc_month)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Let m be ? ToNumber(month).
    auto month = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. If date is present, let dt be ? ToNumber(date).
    auto date = TRY(argument_or_empty(vm, 1));

    // 4. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 5. If date is not present, let dt be DateFromTime(t).
    if (!date.has_value())
        date = date_from_time(time);

    // 6. Let newDate be MakeDate(MakeDay(YearFromTime(t), m, dt), TimeWithinDay(t)).
    auto year = year_from_time(time);

    auto day = make_day(year, month, *date);
    auto new_date = make_date(day, time_within_day(time));

    // 7. Let v be TimeClip(newDate).
    new_date = time_clip(new_date);

    // 8. Set the [[DateValue]] internal slot of this Date object to v.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(new_date);

    // 9. Return v.
    return Value(new_date);
}

// 21.4.4.34 Date.prototype.setUTCSeconds ( sec [ , ms ] ), https://tc39.es/ecma262/#sec-date.prototype.setutcseconds
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_utc_seconds)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Let s be ? ToNumber(sec).
    auto second = TRY(vm.argument(0).to_number(vm)).as_double();

    // 3. If ms is present, let milli be ? ToNumber(ms).
    auto millisecond = TRY(argument_or_empty(vm, 1));

    // 4. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 5. If ms is not present, let milli be msFromTime(t).
    if (!millisecond.has_value())
        millisecond = ms_from_time(time);

    // 6. Let date be MakeDate(Day(t), MakeTime(HourFromTime(t), MinFromTime(t), s, milli)).
    auto hour = hour_from_time(time);
    auto minute = min_from_time(time);

    auto new_time = make_time(hour, minute, second, *millisecond);
    auto new_date = make_date(day(time), new_time);

    // 7. Let v be TimeClip(date).
    new_date = time_clip(new_date);

    // 8. Set the [[DateValue]] internal slot of this Date object to v.
    auto this_object = MUST(typed_this_object(vm));
    this_object->set_date_value(new_date);

    // 9. Return v.
    return Value(new_date);
}

// 21.4.4.35 Date.prototype.toDateString ( ), https://tc39.es/ecma262/#sec-date.prototype.todatestring
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::to_date_string)
{
    // 1. Let O be this Date object.
    // 2. Let tv be ? thisTimeValue(O).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 3. If tv is NaN, return "Invalid Date".
    if (isnan(time))
        return PrimitiveString::create(vm, "Invalid Date"_string);

    // 4. Let t be LocalTime(tv).
    // 5. Return DateString(t).
    return PrimitiveString::create(vm, date_string(local_time(time)));
}

// 21.4.4.36 Date.prototype.toISOString ( ), https://tc39.es/ecma262/#sec-date.prototype.toisostring
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::to_iso_string)
{
    auto this_object = TRY(typed_this_object(vm));

    if (!Value(this_object->date_value()).is_finite_number())
        return vm.throw_completion<RangeError>(ErrorType::InvalidTimeValue);

    auto string = TRY_OR_THROW_OOM(vm, this_object->iso_date_string());
    return PrimitiveString::create(vm, move(string));
}

// 21.4.4.37 Date.prototype.toJSON ( key ), https://tc39.es/ecma262/#sec-date.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::to_json)
{
    auto this_value = vm.this_value();

    auto time_value = TRY(this_value.to_primitive(vm, Value::PreferredType::Number));

    if (time_value.is_number() && !time_value.is_finite_number())
        return js_null();

    return TRY(this_value.invoke(vm, vm.names.toISOString));
}

// 21.4.4.38 Date.prototype.toLocaleDateString ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-date.prototype.tolocaledatestring
// 20.4.2 Date.prototype.toLocaleDateString ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sup-date.prototype.tolocaledatestring
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::to_locale_date_string)
{
    auto& realm = *vm.current_realm();

    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let x be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If x is NaN, return "Invalid Date".
    if (isnan(time))
        return PrimitiveString::create(vm, "Invalid Date"_string);

    // 3. Let dateFormat be ? CreateDateTimeFormat(%DateTimeFormat%, locales, options, "date", "date").
    auto date_format = TRY(Intl::create_date_time_format(vm, realm.intrinsics().intl_date_time_format_constructor(), locales, options, Intl::OptionRequired::Date, Intl::OptionDefaults::Date));

    // 4. Return ? FormatDateTime(dateFormat, x).
    auto formatted = TRY(Intl::format_date_time(vm, date_format, time));
    return PrimitiveString::create(vm, move(formatted));
}

// 21.4.4.39 Date.prototype.toLocaleString ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-date.prototype.tolocalestring
// 20.4.1 Date.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sup-date.prototype.tolocalestring
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::to_locale_string)
{
    auto& realm = *vm.current_realm();

    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let x be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If x is NaN, return "Invalid Date".
    if (isnan(time))
        return PrimitiveString::create(vm, "Invalid Date"_string);

    // 3. Let dateFormat be ? CreateDateTimeFormat(%DateTimeFormat%, locales, options, "any", "all").
    auto date_format = TRY(Intl::create_date_time_format(vm, realm.intrinsics().intl_date_time_format_constructor(), locales, options, Intl::OptionRequired::Any, Intl::OptionDefaults::All));

    // 4. Return ? FormatDateTime(dateFormat, x).
    auto formatted = TRY(Intl::format_date_time(vm, date_format, time));
    return PrimitiveString::create(vm, move(formatted));
}

// 21.4.4.40 Date.prototype.toLocaleTimeString ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-date.prototype.tolocaletimestring
// 20.4.3 Date.prototype.toLocaleTimeString ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sup-date.prototype.tolocaletimestring
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::to_locale_time_string)
{
    auto& realm = *vm.current_realm();

    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let x be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If x is NaN, return "Invalid Date".
    if (isnan(time))
        return PrimitiveString::create(vm, "Invalid Date"_string);

    // 3. Let timeFormat be ? CreateDateTimeFormat(%DateTimeFormat%, locales, options, "time", "time").
    auto time_format = TRY(Intl::create_date_time_format(vm, realm.intrinsics().intl_date_time_format_constructor(), locales, options, Intl::OptionRequired::Time, Intl::OptionDefaults::Time));

    // 4. Return ? FormatDateTime(timeFormat, x).
    auto formatted = TRY(Intl::format_date_time(vm, time_format, time));
    return PrimitiveString::create(vm, move(formatted));
}

// 21.4.4.41 Date.prototype.toString ( ), https://tc39.es/ecma262/#sec-date.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::to_string)
{
    // 1. Let tv be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. Return ToDateString(tv).
    return PrimitiveString::create(vm, JS::to_date_string(time));
}

// 21.4.4.41.1 TimeString ( tv ), https://tc39.es/ecma262/#sec-timestring
// 14.5.8 TimeString ( tv ), https://tc39.es/proposal-temporal/#sec-timestring
ByteString time_string(double time)
{
    // 1. Let timeString be FormatTimeString(ℝ(HourFromTime(tv)), ℝ(MinFromTime(tv)), ℝ(SecFromTime(tv)), 0, 0).
    auto time_string = Temporal::format_time_string(hour_from_time(time), min_from_time(time), sec_from_time(time), 0, 0);

    // 4. Return the string-concatenation of timeString, the code unit 0x0020 (SPACE), and "GMT".
    return ByteString::formatted("{} GMT", time_string);
}

// 21.4.4.41.2 DateString ( tv ), https://tc39.es/ecma262/#sec-datestring
ByteString date_string(double time)
{
    // 1. Let weekday be the Name of the entry in Table 62 with the Number WeekDay(tv).
    auto weekday = short_day_names[week_day(time)];

    // 2. Let month be the Name of the entry in Table 63 with the Number MonthFromTime(tv).
    auto month = short_month_names[month_from_time(time)];

    // 3. Let day be ToZeroPaddedDecimalString(ℝ(DateFromTime(tv)), 2).
    auto day = date_from_time(time);

    // 4. Let yv be YearFromTime(tv).
    auto year = year_from_time(time);

    // 5. If yv is +0𝔽 or yv > +0𝔽, let yearSign be the empty String; otherwise, let yearSign be "-".
    auto year_sign = year >= 0 ? ""sv : "-"sv;

    // 6. Let paddedYear be ToZeroPaddedDecimalString(abs(ℝ(yv)), 4).
    // 7. Return the string-concatenation of weekday, the code unit 0x0020 (SPACE), month, the code unit 0x0020 (SPACE), day, the code unit 0x0020 (SPACE), yearSign, and paddedYear.
    return ByteString::formatted("{} {} {:02} {}{:04}", weekday, month, day, year_sign, abs(year));
}

// 21.4.4.41.3 TimeZoneString ( tv ), https://tc39.es/ecma262/#sec-timezoneestring
// 14.5.9 TimeZoneString ( tv ), https://tc39.es/proposal-temporal/#sec-timezoneestring
ByteString time_zone_string(double time)
{
    // 1. Let systemTimeZoneIdentifier be SystemTimeZoneIdentifier().
    auto system_time_zone_identifier = JS::system_time_zone_identifier();

    // 2. Let offsetMinutes be ! ParseTimeZoneIdentifier(systemTimeZoneIdentifier).[[OffsetMinutes]].
    auto offset_minutes = Temporal::parse_time_zone_identifier(system_time_zone_identifier).offset_minutes;
    auto in_dst = Unicode::TimeZoneOffset::InDST::No;

    // 2. If offsetMinutes is EMPTY, then
    if (!offset_minutes.has_value()) {
        // a. Let offsetNs be GetNamedTimeZoneOffsetNanoseconds(systemTimeZoneIdentifier, ℤ(ℝ(tv) × 10^6)).
        auto offset = get_named_time_zone_offset_milliseconds(system_time_zone_identifier, time);
        in_dst = offset.in_dst;

        // b. Set offsetMinutes to truncate(offsetNs / (60 × 10**9)).
        offset_minutes = trunc(static_cast<double>(offset.offset.to_nanoseconds()) / 60'000'000'000.0);
    }

    // 3. Let offsetString be FormatOffsetTimeZoneIdentifier(offsetMinutes, UNSEPARATED).
    auto offset_string = Temporal::format_offset_time_zone_identifier(*offset_minutes, Temporal::TimeStyle::Unseparated);

    // 5. Let tzName be an implementation-defined string that is either the empty String or the string-concatenation of
    //    the code unit 0x0020 (SPACE), the code unit 0x0028 (LEFT PARENTHESIS), an implementation-defined timezone name,
    //    and the code unit 0x0029 (RIGHT PARENTHESIS).
    auto tz_name = Unicode::current_time_zone();

    // Most implementations seem to prefer the long-form display name of the time zone. Not super important, but we may as well match that behavior.
    if (auto name = Unicode::time_zone_display_name(Unicode::default_locale(), tz_name, in_dst, time); name.has_value())
        tz_name = name.release_value();

    // 10. Return the string-concatenation of offsetString and tzName.
    return ByteString::formatted("{} ({})", offset_string, tz_name);
}

// 21.4.4.41.4 ToDateString ( tv ), https://tc39.es/ecma262/#sec-todatestring
ByteString to_date_string(double time)
{
    // 1. If tv is NaN, return "Invalid Date".
    if (Value(time).is_nan())
        return "Invalid Date"sv;

    // 2. Let t be LocalTime(tv).
    time = local_time(time);

    // 3. Return the string-concatenation of DateString(t), the code unit 0x0020 (SPACE), TimeString(t), and TimeZoneString(tv).
    return ByteString::formatted("{} {}{}", date_string(time), time_string(time), time_zone_string(time));
}

// 21.4.4.42 Date.prototype.toTimeString ( ), https://tc39.es/ecma262/#sec-date.prototype.totimestring
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::to_time_string)
{
    // 1. Let O be this Date object.
    // 2. Let tv be ? thisTimeValue(O).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 3. If tv is NaN, return "Invalid Date".
    if (isnan(time))
        return PrimitiveString::create(vm, "Invalid Date"_string);

    // 4. Let t be LocalTime(tv).
    // 5. Return the string-concatenation of TimeString(t) and TimeZoneString(tv).
    auto string = ByteString::formatted("{}{}", time_string(local_time(time)), time_zone_string(time));
    return PrimitiveString::create(vm, move(string));
}

// 21.4.4.43 Date.prototype.toUTCString ( ), https://tc39.es/ecma262/#sec-date.prototype.toutcstring
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::to_utc_string)
{
    // 1. Let O be this Date object.
    // 2. Let tv be ? thisTimeValue(O).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 3. If tv is NaN, return "Invalid Date".
    if (isnan(time))
        return PrimitiveString::create(vm, "Invalid Date"_string);

    // 4. Let weekday be the Name of the entry in Table 62 with the Number WeekDay(tv).
    auto weekday = short_day_names[week_day(time)];

    // 5. Let month be the Name of the entry in Table 63 with the Number MonthFromTime(tv).
    auto month = short_month_names[month_from_time(time)];

    // 6. Let day be ToZeroPaddedDecimalString(ℝ(DateFromTime(tv)), 2).
    auto day = date_from_time(time);

    // 7. Let yv be YearFromTime(tv).
    auto year = year_from_time(time);

    // 8. If yv is +0𝔽 or yv > +0𝔽, let yearSign be the empty String; otherwise, let yearSign be "-".
    auto year_sign = year >= 0 ? ""sv : "-"sv;

    // 9. Let paddedYear be ToZeroPaddedDecimalString(abs(ℝ(yv)), 4).
    // 10. Return the string-concatenation of weekday, ",", the code unit 0x0020 (SPACE), day, the code unit 0x0020 (SPACE), month, the code unit 0x0020 (SPACE), yearSign, paddedYear, the code unit 0x0020 (SPACE), and TimeString(tv).
    auto string = ByteString::formatted("{}, {:02} {} {}{:04} {}", weekday, day, month, year_sign, abs(year), time_string(time));
    return PrimitiveString::create(vm, move(string));
}

// 21.4.4.45 Date.prototype [ @@toPrimitive ] ( hint ), https://tc39.es/ecma262/#sec-date.prototype-@@toprimitive
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::symbol_to_primitive)
{
    auto this_value = vm.this_value();
    if (!this_value.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, this_value.to_string_without_side_effects());
    auto hint_value = vm.argument(0);
    if (!hint_value.is_string())
        return vm.throw_completion<TypeError>(ErrorType::InvalidHint, hint_value.to_string_without_side_effects());
    auto hint = hint_value.as_string().utf8_string_view();
    Value::PreferredType try_first;
    if (hint == "string"sv || hint == "default"sv)
        try_first = Value::PreferredType::String;
    else if (hint == "number"sv)
        try_first = Value::PreferredType::Number;
    else
        return vm.throw_completion<TypeError>(ErrorType::InvalidHint, hint);
    return TRY(this_value.as_object().ordinary_to_primitive(try_first));
}

// 14.8.1 Date.prototype.toTemporalInstant ( ), https://tc39.es/proposal-temporal/#sec-date.prototype.totemporalinstant
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::to_temporal_instant)
{
    // 1. Let dateObject be the this value.
    // 2. Perform ? RequireInternalSlot(dateObject, [[DateValue]]).
    auto date_object = TRY(typed_this_value(vm));

    // 3. Let t be dateObject.[[DateValue]].
    auto time = date_object->date_value();

    // 4. Let ns be ? NumberToBigInt(t) × ℤ(10**6).
    auto nanoseconds = TRY(number_to_bigint(vm, Value { time }));
    nanoseconds = BigInt::create(vm, nanoseconds->big_integer().multiplied_by(Temporal::NANOSECONDS_PER_MILLISECOND));

    // 5. Return ! CreateTemporalInstant(ns).
    return MUST(Temporal::create_temporal_instant(vm, nanoseconds));
}

// B.2.4.1 Date.prototype.getYear ( ), https://tc39.es/ecma262/#sec-date.prototype.getyear
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::get_year)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, return NaN.
    if (isnan(time))
        return js_nan();

    // 3. Return YearFromTime(LocalTime(t)) - 1900𝔽.
    return Value(year_from_time(local_time(time)) - 1900);
}

// B.2.4.2 Date.prototype.setYear ( year ), https://tc39.es/ecma262/#sec-date.prototype.setyear
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::set_year)
{
    // 1. Let t be ? thisTimeValue(this value).
    auto this_time = TRY(this_time_value(vm, vm.this_value()));

    // 2. If t is NaN, set t to +0𝔽; otherwise, set t to LocalTime(t).
    double time = 0;
    if (!isnan(this_time))
        time = local_time(this_time);

    // 3. Let y be ? ToNumber(year).
    auto year = TRY(vm.argument(0).to_number(vm)).as_double();

    auto this_object = MUST(typed_this_object(vm));

    // 4. If y is NaN, then
    if (isnan(year)) {
        // a. Set the [[DateValue]] internal slot of this Date object to NaN.
        this_object->set_date_value(NAN);

        // b. Return NaN.
        return js_nan();
    }

    // 5. Let yi be ! ToIntegerOrInfinity(y).
    auto year_integer = to_integer_or_infinity(year);

    // 6. If 0 ≤ yi ≤ 99, let yyyy be 1900𝔽 + 𝔽(yi).
    if (0 <= year_integer && year_integer <= 99)
        year = 1900 + year_integer;
    // 7. Else, let yyyy be y.

    // 8. Let d be MakeDay(yyyy, MonthFromTime(t), DateFromTime(t)).
    auto day = make_day(year, month_from_time(time), date_from_time(time));

    // 9. Let date be UTC(MakeDate(d, TimeWithinDay(t))).
    auto date = utc_time(make_date(day, time_within_day(time)));

    // 10. Set the [[DateValue]] internal slot of this Date object to TimeClip(date).
    auto new_date = time_clip(date);
    this_object->set_date_value(new_date);

    // 11. Return the value of the [[DateValue]] internal slot of this Date object.
    return Value(new_date);
}

// B.2.4.3 Date.prototype.toGMTString ( ), https://tc39.es/ecma262/#sec-date.prototype.togmtstring
JS_DEFINE_NATIVE_FUNCTION(DatePrototype::to_gmt_string)
{
    // NOTE: The toUTCString method is preferred. The toGMTString method is provided principally for compatibility with old code.
    return to_utc_string(vm);
}

}
