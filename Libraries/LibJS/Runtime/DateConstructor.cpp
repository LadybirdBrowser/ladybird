/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2020, Nico Weber <thakis@chromium.org>
 * Copyright (c) 2021, Petróczi Zoltán <petroczizoltan@tutanota.com>
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <AK/Time.h>
#include <LibCore/DateTime.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/DateConstructor.h>
#include <LibJS/Runtime/DatePrototype.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <sys/time.h>
#include <time.h>

namespace JS {

GC_DEFINE_ALLOCATOR(DateConstructor);

// 21.4.3.2 Date.parse ( string ), https://tc39.es/ecma262/#sec-date.parse
static double parse_simplified_iso8601(ByteString const& iso_8601)
{
    // 21.4.1.15 Date Time String Format, https://tc39.es/ecma262/#sec-date-time-string-format
    GenericLexer lexer(iso_8601);
    auto lex_n_digits = [&](size_t n, Optional<int>& out) {
        if (lexer.tell_remaining() < n)
            return false;
        int r = 0;
        for (size_t i = 0; i < n; ++i) {
            char ch = lexer.consume();
            if (!is_ascii_digit(ch))
                return false;
            r = 10 * r + ch - '0';
        }
        out = r;
        return true;
    };

    Optional<int> year;
    Optional<int> month;
    Optional<int> day;
    Optional<int> hours;
    Optional<int> minutes;
    Optional<int> seconds;
    Optional<int> milliseconds;
    Optional<char> timezone;
    Optional<int> timezone_hours;
    Optional<int> timezone_minutes;

    auto lex_year = [&]() {
        if (lexer.consume_specific('+'))
            return lex_n_digits(6, year);
        if (lexer.consume_specific('-')) {
            Optional<int> absolute_year;
            if (!lex_n_digits(6, absolute_year))
                return false;
            // The representation of the year 0 as -000000 is invalid.
            if (absolute_year.value() == 0)
                return false;
            year = -absolute_year.value();
            return true;
        }
        return lex_n_digits(4, year);
    };
    auto lex_month = [&]() { return lex_n_digits(2, month) && *month >= 1 && *month <= 12; };
    auto lex_day = [&]() { return lex_n_digits(2, day) && *day >= 1 && *day <= 31; };
    auto lex_date = [&]() { return lex_year() && (!lexer.consume_specific('-') || (lex_month() && (!lexer.consume_specific('-') || lex_day()))); };

    auto lex_hours_minutes = [&](Optional<int>& out_h, Optional<int>& out_m) {
        Optional<int> h;
        Optional<int> m;
        if (lex_n_digits(2, h) && *h >= 0 && *h <= 24 && lexer.consume_specific(':') && lex_n_digits(2, m) && *m >= 0 && *m <= 59) {
            out_h = move(h);
            out_m = move(m);
            return true;
        }
        return false;
    };
    auto lex_seconds = [&]() { return lex_n_digits(2, seconds) && *seconds >= 0 && *seconds <= 59; };
    auto lex_milliseconds = [&]() {
        // Date.parse() is allowed to accept an arbitrary number of implementation-defined formats.
        // Milliseconds are parsed slightly different as other engines allow effectively any number of digits here.
        // We require at least one digit and only use the first three.

        auto digits_read = 0;
        int result = 0;
        while (!lexer.is_eof() && is_ascii_digit(lexer.peek())) {
            char ch = lexer.consume();
            if (digits_read < 3)
                result = 10 * result + ch - '0';

            ++digits_read;
        }

        if (digits_read == 0)
            return false;

        // If we got less than three digits pretend we have trailing zeros.
        while (digits_read < 3) {
            result *= 10;
            ++digits_read;
        }

        milliseconds = result;
        return true;
    };
    auto lex_seconds_milliseconds = [&]() { return lex_seconds() && (!lexer.consume_specific('.') || lex_milliseconds()); };
    auto lex_timezone = [&]() {
        if (lexer.consume_specific('+')) {
            timezone = '+';
            return lex_hours_minutes(timezone_hours, timezone_minutes);
        }
        if (lexer.consume_specific('-')) {
            timezone = '-';
            return lex_hours_minutes(timezone_hours, timezone_minutes);
        }
        if (lexer.consume_specific('Z'))
            timezone = 'Z';
        return true;
    };
    auto lex_time = [&]() { return lex_hours_minutes(hours, minutes) && (!lexer.consume_specific(':') || lex_seconds_milliseconds()) && lex_timezone(); };

    if (!lex_date() || (lexer.consume_specific('T') && !lex_time()) || !lexer.is_eof()) {
        return NAN;
    }

    // We parsed a valid date simplified ISO 8601 string.
    VERIFY(year.has_value()); // A valid date string always has at least a year.
    auto time = AK::UnixDateTime::from_unix_time_parts(*year, month.value_or(1), day.value_or(1), hours.value_or(0), minutes.value_or(0), seconds.value_or(0), milliseconds.value_or(0));
    auto time_ms = static_cast<double>(time.milliseconds_since_epoch());

    // https://tc39.es/ecma262/#sec-date.parse:
    // "When the UTC offset representation is absent, date-only forms are interpreted as a UTC time and date-time forms are interpreted as a local time."
    if (!timezone.has_value() && hours.has_value())
        time_ms = utc_time(time_ms);

    if (timezone == '-')
        time_ms += *timezone_hours * 3'600'000 + *timezone_minutes * 60'000;
    else if (timezone == '+')
        time_ms -= *timezone_hours * 3'600'000 + *timezone_minutes * 60'000;

    return time_clip(time_ms);
}

static double parse_date_string(VM& vm, ByteString const& date_string)
{
    if (date_string.is_empty())
        return NAN;

    auto value = parse_simplified_iso8601(date_string);
    if (isfinite(value))
        return value;

    // Date.parse() is allowed to accept an arbitrary number of implementation-defined formats.
    // FIXME: Exactly what timezone and which additional formats we should support is unclear.
    //        Both Chrome and Firefox seem to support "4/17/2019 11:08 PM +0000" with most parts
    //        being optional, however this is not clearly documented anywhere.
    static constexpr auto extra_formats = AK::Array {
        "%a%t%b%t%d%t%Y%t%T%tGMT%z%t(%+)"sv,   // "Tue Nov 07 2023 10:05:55 GMT-0500 (Eastern Standard Time)"
        "%a,%t%d%t%b%t%Y%t%T%t%Z"sv,           // "Tue, 07 Nov 2023 15:05:55 GMT"
        "%a%t%b%t%e%t%T%t%z%t%Y"sv,            // "Wed Apr 17 23:08:53 +0000 2019"
        "%m/%e/%Y"sv,                          // "4/17/2019"
        "%m/%e/%Y%t%R%t%z"sv,                  // "12/05/2022 10:00 -0800"
        "%Y/%m/%e%t%R"sv,                      // "2014/11/14 13:05"
        "%Y-%m-%e%t%R"sv,                      // "2014-11-14 13:05"
        "%B%t%e,%t%Y"sv,                       // "June 5, 2023"
        "%B%t%e,%t%Y%t%T"sv,                   // "June 5, 2023 17:00:00"
        "%b%t%d%t%Y%t%Z"sv,                    // "Jan 01 1970 GMT"
        "%a%t%b%t%e%t%T%t%Y%t%z"sv,            // "Wed Apr 17 23:08:53 2019 +0000"
        "%Y-%m-%e%t%R%z"sv,                    // "2021-07-01 03:00Z"
        "%a,%t%e%t%b%t%Y%t%T%t%z"sv,           // "Wed, 17 Jan 2024 11:36:34 +0000"
        "%a%t%b%t%e%t%Y%t%T%tGMT%t%x%t(%+)"sv, // "Sun Jan 21 2024 21:11:31 GMT 0100 (Central European Standard Time)"
        "%Y-%m-%e%t%T"sv,                      // "2024-01-15 00:00:01"
        "%a%t%b%t%e%t%Y%t%T%t%Z"sv,            // "Tue Nov 07 2023 10:05:55  UTC"
        "%a%t%b%t%e%t%T%t%Y"sv,                // "Wed Apr 17 23:08:53 2019"
        "%a%t%b%t%e%t%Y%t%T"sv,                // "Wed Apr 17 2019 23:08:53"
        "%Y-%m-%eT%T%X%z"sv,                   // "2024-01-26T22:10:11.306+0000"
        "%m/%e/%Y,%t%T%t%p"sv,                 // "1/27/2024, 9:28:30 AM"
        "%Y-%m-%e"sv,                          // "2024-1-15"
        "%Y-%m-%e%t%T%tGMT%z"sv,               // "2024-07-05 00:00:00 GMT-0800"
        "%d%t%B%t%Y"sv,                        // "01 February 2013"
        "%d%t%B%t%Y%t%R"sv,                    // "01 February 2013 08:00"
        "%d%t%b%t%Y"sv,                        // "01 Jan 2000"
        "%d%t%b%t%Y%t%R"sv,                    // "01 Jan 2000 08:00"
        "%A,%t%B%t%e,%t%Y,%t%R%t%Z"sv,         // "Tuesday, October 29, 2024, 18:00 UTC"
        "%B%t%d%t%Y%t%T%t%z"sv,                // "November 19 2024 00:00:00 +0900"
        "%a%t%b%t%e%t%Y"sv                     // "Wed Nov 20 2024"
    };

    for (auto const& format : extra_formats) {
        auto maybe_datetime = Core::DateTime::parse(format, date_string);
        if (maybe_datetime.has_value())
            return 1000.0 * maybe_datetime->timestamp();
    }

    vm.host_unrecognized_date_string(date_string);
    return NAN;
}

DateConstructor::DateConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Date.as_string(), realm.intrinsics().function_prototype())
{
}

void DateConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 21.4.3.3 Date.prototype, https://tc39.es/ecma262/#sec-date.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().date_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.now, now, 0, attr);
    define_native_function(realm, vm.names.parse, parse, 1, attr);
    define_native_function(realm, vm.names.UTC, utc, 7, attr);

    define_direct_property(vm.names.length, Value(7), Attribute::Configurable);
}

// 21.4.2.1 Date ( ...values ), https://tc39.es/ecma262/#sec-date
ThrowCompletionOr<Value> DateConstructor::call()
{
    // 1. If NewTarget is undefined, then
    //     a. Let now be the time value (UTC) identifying the current time.
    auto now = AK::UnixDateTime::now().milliseconds_since_epoch();

    //     b. Return ToDateString(now).
    return PrimitiveString::create(vm(), to_date_string(now));
}

// 21.4.2.1 Date ( ...values ), https://tc39.es/ecma262/#sec-date
ThrowCompletionOr<GC::Ref<Object>> DateConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    double date_value;

    // 2. Let numberOfArgs be the number of elements in values.
    // 3. If numberOfArgs = 0, then
    if (vm.argument_count() == 0) {
        // a. Let dv be the time value (UTC) identifying the current time.
        auto now = AK::UnixDateTime::now().milliseconds_since_epoch();
        date_value = static_cast<double>(now);
    }
    // 4. Else if numberOfArgs = 1, then
    else if (vm.argument_count() == 1) {
        // a. Let value be values[0].
        auto value = vm.argument(0);
        double time_value;

        // b. If Type(value) is Object and value has a [[DateValue]] internal slot, then
        if (value.is_object() && is<Date>(value.as_object())) {
            // i. Let tv be ! thisTimeValue(value).
            time_value = MUST(this_time_value(vm, value));
        }
        // c. Else,
        else {
            // i. Let v be ? ToPrimitive(value).
            auto primitive = TRY(value.to_primitive(vm));

            // ii. If Type(v) is String, then
            if (primitive.is_string()) {
                // 1. Assert: The next step never returns an abrupt completion because Type(v) is String.
                // 2. Let tv be the result of parsing v as a date, in exactly the same manner as for the parse method (21.4.3.2).
                time_value = parse_date_string(vm, primitive.as_string().byte_string());
            }
            // iii. Else,
            else {
                // 1. Let tv be ? ToNumber(v).
                time_value = TRY(primitive.to_number(vm)).as_double();
            }
        }

        // d. Let dv be TimeClip(tv).
        date_value = time_clip(time_value);
    }
    // 5. Else,
    else {
        // a. Assert: numberOfArgs ≥ 2.
        // b. Let y be ? ToNumber(values[0]).
        auto year = TRY(vm.argument(0).to_number(vm)).as_double();
        // c. Let m be ? ToNumber(values[1]).
        auto month = TRY(vm.argument(1).to_number(vm)).as_double();

        auto arg_or = [&vm](size_t i, double fallback) -> ThrowCompletionOr<double> {
            return vm.argument_count() > i ? TRY(vm.argument(i).to_number(vm)).as_double() : fallback;
        };

        // d. If numberOfArgs > 2, let dt be ? ToNumber(values[2]); else let dt be 1𝔽.
        auto date = TRY(arg_or(2, 1));
        // e. If numberOfArgs > 3, let h be ? ToNumber(values[3]); else let h be +0𝔽.
        auto hours = TRY(arg_or(3, 0));
        // f. If numberOfArgs > 4, let min be ? ToNumber(values[4]); else let min be +0𝔽.
        auto minutes = TRY(arg_or(4, 0));
        // g. If numberOfArgs > 5, let s be ? ToNumber(values[5]); else let s be +0𝔽.
        auto seconds = TRY(arg_or(5, 0));
        // h. If numberOfArgs > 6, let milli be ? ToNumber(values[6]); else let milli be +0𝔽.
        auto milliseconds = TRY(arg_or(6, 0));

        // i. If y is NaN, let yr be NaN.
        // j. Else,
        if (!isnan(year)) {
            // i. Let yi be ! ToIntegerOrInfinity(y).
            auto year_integer = to_integer_or_infinity(year);

            // ii. If 0 ≤ yi ≤ 99, let yr be 1900𝔽 + 𝔽(yi); otherwise, let yr be y.
            if (0 <= year_integer && year_integer <= 99)
                year = 1900 + year_integer;
        }

        // k. Let finalDate be MakeDate(MakeDay(yr, m, dt), MakeTime(h, min, s, milli)).
        auto day = make_day(year, month, date);
        auto time = make_time(hours, minutes, seconds, milliseconds);
        auto final_date = make_date(day, time);

        // l. Let dv be TimeClip(UTC(finalDate)).
        date_value = time_clip(utc_time(final_date));
    }

    // 6. Let O be ? OrdinaryCreateFromConstructor(NewTarget, "%Date.prototype%", « [[DateValue]] »).
    // 7. Set O.[[DateValue]] to dv.
    // 8. Return O.
    return TRY(ordinary_create_from_constructor<Date>(vm, new_target, &Intrinsics::date_prototype, date_value));
}

// 21.4.3.1 Date.now ( ), https://tc39.es/ecma262/#sec-date.now
JS_DEFINE_NATIVE_FUNCTION(DateConstructor::now)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return Value(floor(tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0));
}

// 21.4.3.2 Date.parse ( string ), https://tc39.es/ecma262/#sec-date.parse
JS_DEFINE_NATIVE_FUNCTION(DateConstructor::parse)
{
    if (!vm.argument_count())
        return js_nan();

    auto date_string = TRY(vm.argument(0).to_byte_string(vm));

    return Value(parse_date_string(vm, date_string));
}

// 21.4.3.4 Date.UTC ( year [ , month [ , date [ , hours [ , minutes [ , seconds [ , ms ] ] ] ] ] ] ), https://tc39.es/ecma262/#sec-date.utc
JS_DEFINE_NATIVE_FUNCTION(DateConstructor::utc)
{
    auto arg_or = [&vm](size_t i, double fallback) -> ThrowCompletionOr<double> {
        return vm.argument_count() > i ? TRY(vm.argument(i).to_number(vm)).as_double() : fallback;
    };

    // 1. Let y be ? ToNumber(year).
    auto year = TRY(vm.argument(0).to_number(vm)).as_double();
    // 2. If month is present, let m be ? ToNumber(month); else let m be +0𝔽.
    auto month = TRY(arg_or(1, 0));
    // 3. If date is present, let dt be ? ToNumber(date); else let dt be 1𝔽.
    auto date = TRY(arg_or(2, 1));
    // 4. If hours is present, let h be ? ToNumber(hours); else let h be +0𝔽.
    auto hours = TRY(arg_or(3, 0));
    // 5. If minutes is present, let min be ? ToNumber(minutes); else let min be +0𝔽.
    auto minutes = TRY(arg_or(4, 0));
    // 6. If seconds is present, let s be ? ToNumber(seconds); else let s be +0𝔽.
    auto seconds = TRY(arg_or(5, 0));
    // 7. If ms is present, let milli be ? ToNumber(ms); else let milli be +0𝔽.
    auto milliseconds = TRY(arg_or(6, 0));

    // 8. If y is NaN, let yr be NaN.
    // 9. Else,
    if (!isnan(year)) {
        // a. Let yi be ! ToIntegerOrInfinity(y).
        auto year_integer = to_integer_or_infinity(year);

        // b. If 0 ≤ yi ≤ 99, let yr be 1900𝔽 + 𝔽(yi); otherwise, let yr be y.
        if (0 <= year_integer && year_integer <= 99)
            year = 1900 + year_integer;
    }

    // 10. Return TimeClip(MakeDate(MakeDay(yr, m, dt), MakeTime(h, min, s, milli))).
    auto day = make_day(year, month, date);
    auto time = make_time(hours, minutes, seconds, milliseconds);
    return Value(time_clip(make_date(day, time)));
}

}
