/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2020, Nico Weber <thakis@chromium.org>
 * Copyright (c) 2021, Petróczi Zoltán <petroczizoltan@tutanota.com>
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibCore/DateTime.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/DateConstructor.h>
#include <LibJS/Runtime/DateParser.h>
#include <LibJS/Runtime/DatePrototype.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/Now.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

GC_DEFINE_ALLOCATOR(DateConstructor);

static double parse_date_string(VM& vm, StringView date_string)
{
    double result = DateParser::parse(date_string);
    if (result == NAN)
        vm.host_unrecognized_date_string(date_string);

    return result;
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
// 14.6.1 Date ( ...values ), https://tc39.es/proposal-temporal/#sec-temporal-date
ThrowCompletionOr<Value> DateConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, return ToDateString(SystemUTCEpochMilliseconds()).
    return PrimitiveString::create(vm, to_date_string(Temporal::system_utc_epoch_milliseconds(vm)));
}

// 21.4.2.1 Date ( ...values ), https://tc39.es/ecma262/#sec-date
// 14.6.1 Date ( ...values ), https://tc39.es/proposal-temporal/#sec-temporal-date
ThrowCompletionOr<GC::Ref<Object>> DateConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    double date_value;

    // 2. Let numberOfArgs be the number of elements in values.
    // 3. If numberOfArgs = 0, then
    if (vm.argument_count() == 0) {
        // a. Let dv be SystemUTCEpochMilliseconds().
        date_value = Temporal::system_utc_epoch_milliseconds(vm);
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
                time_value = parse_date_string(vm, primitive.as_string().utf8_string_view());
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
// 14.7.1 Date.now ( ), https://tc39.es/proposal-temporal/#sec-temporal-date.now
JS_DEFINE_NATIVE_FUNCTION(DateConstructor::now)
{
    // 1. Return SystemUTCEpochMilliseconds().
    return Temporal::system_utc_epoch_milliseconds(vm);
}

// 21.4.3.2 Date.parse ( string ), https://tc39.es/ecma262/#sec-date.parse
JS_DEFINE_NATIVE_FUNCTION(DateConstructor::parse)
{
    if (!vm.argument_count())
        return js_nan();

    // This function applies the ToString operator to its argument. If ToString results in an abrupt completion the
    // Completion Record is immediately returned.
    auto date_string = TRY(vm.argument(0).to_string(vm));

    // Otherwise, this function interprets the resulting String as a date and time; it returns a Number, the UTC time
    // value corresponding to the date and time.
    return parse_date_string(vm, date_string);
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
