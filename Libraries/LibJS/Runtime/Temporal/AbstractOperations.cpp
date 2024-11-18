/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/BigFraction/BigFraction.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/ISO8601.h>

namespace JS::Temporal {

// https://tc39.es/proposal-temporal/#table-temporal-units
struct TemporalUnit {
    Unit value;
    StringView singular_property_name;
    StringView plural_property_name;
    UnitCategory category;
    RoundingIncrement maximum_duration_rounding_increment;
};
static auto temporal_units = to_array<TemporalUnit>({
    { Unit::Year, "year"sv, "years"sv, UnitCategory::Date, Unset {} },
    { Unit::Month, "month"sv, "months"sv, UnitCategory::Date, Unset {} },
    { Unit::Week, "week"sv, "weeks"sv, UnitCategory::Date, Unset {} },
    { Unit::Day, "day"sv, "days"sv, UnitCategory::Date, Unset {} },
    { Unit::Hour, "hour"sv, "hours"sv, UnitCategory::Time, 24 },
    { Unit::Minute, "minute"sv, "minutes"sv, UnitCategory::Time, 60 },
    { Unit::Second, "second"sv, "seconds"sv, UnitCategory::Time, 60 },
    { Unit::Millisecond, "millisecond"sv, "milliseconds"sv, UnitCategory::Time, 1000 },
    { Unit::Microsecond, "microsecond"sv, "microseconds"sv, UnitCategory::Time, 1000 },
    { Unit::Nanosecond, "nanosecond"sv, "nanoseconds"sv, UnitCategory::Time, 1000 },
});

StringView temporal_unit_to_string(Unit unit)
{
    return temporal_units[to_underlying(unit)].singular_property_name;
}

// 13.18 GetTemporalRelativeToOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalrelativetooption
ThrowCompletionOr<RelativeTo> get_temporal_relative_to_option(VM& vm, Object const& options)
{
    // 1. Let value be ? Get(options, "relativeTo").
    auto value = TRY(options.get(vm.names.relativeTo));

    // 2. If value is undefined, return the Record { [[PlainRelativeTo]]: undefined, [[ZonedRelativeTo]]: undefined }.
    if (value.is_undefined())
        return RelativeTo { .plain_relative_to = {}, .zoned_relative_to = {} };

    // FIXME: Implement the remaining steps of this AO when we have implemented PlainRelativeTo and ZonedRelativeTo.
    return RelativeTo { .plain_relative_to = {}, .zoned_relative_to = {} };
}

// 13.20 IsCalendarUnit ( unit ), https://tc39.es/proposal-temporal/#sec-temporal-iscalendarunit
bool is_calendar_unit(Unit unit)
{
    // 1. If unit is year, return true.
    if (unit == Unit::Year)
        return true;

    // 2. If unit is month, return true.
    if (unit == Unit::Month)
        return true;

    // 3. If unit is week, return true.
    if (unit == Unit::Week)
        return true;

    // 4. Return false.
    return false;
}

// 13.21 TemporalUnitCategory ( unit ), https://tc39.es/proposal-temporal/#sec-temporal-temporalunitcategory
UnitCategory temporal_unit_category(Unit unit)
{
    // 1. Return the value from the "Category" column of the row of Table 21 in which unit is in the "Value" column.
    return temporal_units[to_underlying(unit)].category;
}

// 13.35 ParseTemporalDurationString ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporaldurationstring
ThrowCompletionOr<GC::Ref<Duration>> parse_temporal_duration_string(VM& vm, StringView iso_string)
{
    // 1. Let duration be ParseText(StringToCodePoints(isoString), TemporalDurationString).
    auto parse_result = parse_iso8601(Production::TemporalDurationString, iso_string);

    // 2. If duration is a List of errors, throw a RangeError exception.
    if (!parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDurationString, iso_string);

    // 3. Let sign be the source text matched by the ASCIISign Parse Node contained within duration, or an empty sequence
    //    of code points if not present.
    auto sign = parse_result->sign;

    // 4. If duration contains a DurationYearsPart Parse Node, then
    //        a. Let yearsNode be that DurationYearsPart Parse Node contained within duration.
    //        b. Let years be the source text matched by the DecimalDigits Parse Node contained within yearsNode.
    // 5. Else,
    //        a. Let years be an empty sequence of code points.
    auto years = parse_result->duration_years.value_or({});

    // 6. If duration contains a DurationMonthsPart Parse Node, then
    //        a. Let monthsNode be the DurationMonthsPart Parse Node contained within duration.
    //        b. Let months be the source text matched by the DecimalDigits Parse Node contained within monthsNode.
    // 7. Else,
    //        a. Let months be an empty sequence of code points.
    auto months = parse_result->duration_months.value_or({});

    // 8. If duration contains a DurationWeeksPart Parse Node, then
    //        a. Let weeksNode be the DurationWeeksPart Parse Node contained within duration.
    //        b. Let weeks be the source text matched by the DecimalDigits Parse Node contained within weeksNode.
    // 9. Else,
    //        a. Let weeks be an empty sequence of code points.
    auto weeks = parse_result->duration_weeks.value_or({});

    // 10. If duration contains a DurationDaysPart Parse Node, then
    //         a. Let daysNode be the DurationDaysPart Parse Node contained within duration.
    //         b. Let days be the source text matched by the DecimalDigits Parse Node contained within daysNode.
    // 11. Else,
    //         a. Let days be an empty sequence of code points.
    auto days = parse_result->duration_days.value_or({});

    // 12. If duration contains a DurationHoursPart Parse Node, then
    //         a. Let hoursNode be the DurationHoursPart Parse Node contained within duration.
    //         b. Let hours be the source text matched by the DecimalDigits Parse Node contained within hoursNode.
    //         c. Let fHours be the source text matched by the TemporalDecimalFraction Parse Node contained within
    //            hoursNode, or an empty sequence of code points if not present.
    // 13. Else,
    //         a. Let hours be an empty sequence of code points.
    //         b. Let fHours be an empty sequence of code points.
    auto hours = parse_result->duration_hours.value_or({});
    auto fractional_hours = parse_result->duration_hours_fraction.value_or({});

    // 14. If duration contains a DurationMinutesPart Parse Node, then
    //         a. Let minutesNode be the DurationMinutesPart Parse Node contained within duration.
    //         b. Let minutes be the source text matched by the DecimalDigits Parse Node contained within minutesNode.
    //         c. Let fMinutes be the source text matched by the TemporalDecimalFraction Parse Node contained within
    //            minutesNode, or an empty sequence of code points if not present.
    // 15. Else,
    //         a. Let minutes be an empty sequence of code points.
    //         b. Let fMinutes be an empty sequence of code points.
    auto minutes = parse_result->duration_minutes.value_or({});
    auto fractional_minutes = parse_result->duration_minutes_fraction.value_or({});

    // 16. If duration contains a DurationSecondsPart Parse Node, then
    //         a. Let secondsNode be the DurationSecondsPart Parse Node contained within duration.
    //         b. Let seconds be the source text matched by the DecimalDigits Parse Node contained within secondsNode.
    //         c. Let fSeconds be the source text matched by the TemporalDecimalFraction Parse Node contained within
    //            secondsNode, or an empty sequence of code points if not present.
    // 17. Else,
    //         a. Let seconds be an empty sequence of code points.
    //         b. Let fSeconds be an empty sequence of code points.
    auto seconds = parse_result->duration_seconds.value_or({});
    auto fractional_seconds = parse_result->duration_seconds_fraction.value_or({});

    // 18. Let yearsMV be ? ToIntegerWithTruncation(CodePointsToString(years)).
    auto years_value = TRY(to_integer_with_truncation(vm, years, ErrorType::TemporalInvalidDurationString, iso_string));

    // 19. Let monthsMV be ? ToIntegerWithTruncation(CodePointsToString(months)).
    auto months_value = TRY(to_integer_with_truncation(vm, months, ErrorType::TemporalInvalidDurationString, iso_string));

    // 20. Let weeksMV be ? ToIntegerWithTruncation(CodePointsToString(weeks)).
    auto weeks_value = TRY(to_integer_with_truncation(vm, weeks, ErrorType::TemporalInvalidDurationString, iso_string));

    // 21. Let daysMV be ? ToIntegerWithTruncation(CodePointsToString(days)).
    auto days_value = TRY(to_integer_with_truncation(vm, days, ErrorType::TemporalInvalidDurationString, iso_string));

    // 22. Let hoursMV be ? ToIntegerWithTruncation(CodePointsToString(hours)).
    auto hours_value = TRY(to_integer_with_truncation(vm, hours, ErrorType::TemporalInvalidDurationString, iso_string));

    Crypto::BigFraction minutes_value;
    Crypto::BigFraction seconds_value;
    Crypto::BigFraction milliseconds_value;

    auto remainder_one = [](Crypto::BigFraction const& value) {
        // FIXME: We should add a generic remainder() method to BigFraction, or a method equivalent to modf(). But for
        //        now, since we know we are only dividing by powers of 10, we can implement a very situationally specific
        //        method to extract the fractional part of the BigFraction.
        auto res = value.numerator().divided_by(value.denominator());
        return Crypto::BigFraction { move(res.remainder), value.denominator() };
    };

    // 23. If fHours is not empty, then
    if (!fractional_hours.is_empty()) {
        // a. Assert: minutes, fMinutes, seconds, and fSeconds are empty.
        VERIFY(minutes.is_empty());
        VERIFY(fractional_minutes.is_empty());
        VERIFY(seconds.is_empty());
        VERIFY(fractional_seconds.is_empty());

        // b. Let fHoursDigits be the substring of CodePointsToString(fHours) from 1.
        auto fractional_hours_digits = fractional_hours.substring_view(1);

        // c. Let fHoursScale be the length of fHoursDigits.
        auto fractional_hours_scale = fractional_hours_digits.length();

        // d. Let minutesMV be ? ToIntegerWithTruncation(fHoursDigits) / 10**fHoursScale × 60.
        auto minutes_integer = TRY(to_integer_with_truncation(vm, fractional_hours_digits, ErrorType::TemporalInvalidDurationString, iso_string));
        minutes_value = Crypto::BigFraction { minutes_integer } / Crypto::BigFraction { pow(10.0, fractional_hours_scale) } * Crypto::BigFraction { 60.0 };
    }
    // 24. Else,
    else {
        // a. Let minutesMV be ? ToIntegerWithTruncation(CodePointsToString(minutes)).
        auto minutes_integer = TRY(to_integer_with_truncation(vm, minutes, ErrorType::TemporalInvalidDurationString, iso_string));
        minutes_value = Crypto::BigFraction { minutes_integer };
    }

    // 25. If fMinutes is not empty, then
    if (!fractional_minutes.is_empty()) {
        // a. Assert: seconds and fSeconds are empty.
        VERIFY(seconds.is_empty());
        VERIFY(fractional_seconds.is_empty());

        // b. Let fMinutesDigits be the substring of CodePointsToString(fMinutes) from 1.
        auto fractional_minutes_digits = fractional_minutes.substring_view(1);

        // c. Let fMinutesScale be the length of fMinutesDigits.
        auto fractional_minutes_scale = fractional_minutes_digits.length();

        // d. Let secondsMV be ? ToIntegerWithTruncation(fMinutesDigits) / 10**fMinutesScale × 60.
        auto seconds_integer = TRY(to_integer_with_truncation(vm, fractional_minutes_digits, ErrorType::TemporalInvalidDurationString, iso_string));
        seconds_value = Crypto::BigFraction { seconds_integer } / Crypto::BigFraction { pow(10.0, fractional_minutes_scale) } * Crypto::BigFraction { 60.0 };
    }
    // 26. Else if seconds is not empty, then
    else if (!seconds.is_empty()) {
        // a. Let secondsMV be ? ToIntegerWithTruncation(CodePointsToString(seconds)).
        auto seconds_integer = TRY(to_integer_with_truncation(vm, seconds, ErrorType::TemporalInvalidDurationString, iso_string));
        seconds_value = Crypto::BigFraction { seconds_integer };
    }
    // 27. Else,
    else {
        // a. Let secondsMV be remainder(minutesMV, 1) × 60.
        seconds_value = remainder_one(minutes_value) * Crypto::BigFraction { 60.0 };
    }

    // 28. If fSeconds is not empty, then
    if (!fractional_seconds.is_empty()) {
        // a. Let fSecondsDigits be the substring of CodePointsToString(fSeconds) from 1.
        auto fractional_seconds_digits = fractional_seconds.substring_view(1);

        // b. Let fSecondsScale be the length of fSecondsDigits.
        auto fractional_seconds_scale = fractional_seconds_digits.length();

        // c. Let millisecondsMV be ? ToIntegerWithTruncation(fSecondsDigits) / 10**fSecondsScale × 1000.
        auto milliseconds_integer = TRY(to_integer_with_truncation(vm, fractional_seconds_digits, ErrorType::TemporalInvalidDurationString, iso_string));
        milliseconds_value = Crypto::BigFraction { milliseconds_integer } / Crypto::BigFraction { pow(10.0, fractional_seconds_scale) } * Crypto::BigFraction { 1000.0 };

    }
    // 29. Else,
    else {
        // a. Let millisecondsMV be remainder(secondsMV, 1) × 1000.
        milliseconds_value = remainder_one(seconds_value) * Crypto::BigFraction { 1000.0 };
    }

    // 30. Let microsecondsMV be remainder(millisecondsMV, 1) × 1000.
    auto microseconds_value = remainder_one(milliseconds_value) * Crypto::BigFraction { 1000.0 };

    // 31. Let nanosecondsMV be remainder(microsecondsMV, 1) × 1000.
    auto nanoseconds_value = remainder_one(microseconds_value) * Crypto::BigFraction { 1000.0 };

    // 32. If sign contains the code point U+002D (HYPHEN-MINUS), then
    //     a. Let factor be -1.
    // 33. Else,
    //     a. Let factor be 1.
    i8 factor = sign == '-' ? -1 : 1;

    // 34. Set yearsMV to yearsMV × factor.
    years_value *= factor;

    // 35. Set monthsMV to monthsMV × factor.
    months_value *= factor;

    // 36. Set weeksMV to weeksMV × factor.
    weeks_value *= factor;

    // 37. Set daysMV to daysMV × factor.
    days_value *= factor;

    // 38. Set hoursMV to hoursMV × factor.
    hours_value *= factor;

    // 39. Set minutesMV to floor(minutesMV) × factor.
    auto factored_minutes_value = floor(minutes_value.to_double()) * factor;

    // 40. Set secondsMV to floor(secondsMV) × factor.
    auto factored_seconds_value = floor(seconds_value.to_double()) * factor;

    // 41. Set millisecondsMV to floor(millisecondsMV) × factor.
    auto factored_milliseconds_value = floor(milliseconds_value.to_double()) * factor;

    // 42. Set microsecondsMV to floor(microsecondsMV) × factor.
    auto factored_microseconds_value = floor(microseconds_value.to_double()) * factor;

    // 43. Set nanosecondsMV to floor(nanosecondsMV) × factor.
    auto factored_nanoseconds_value = floor(nanoseconds_value.to_double()) * factor;

    // 44. Return ? CreateTemporalDuration(yearsMV, monthsMV, weeksMV, daysMV, hoursMV, minutesMV, secondsMV, millisecondsMV, microsecondsMV, nanosecondsMV).
    return TRY(create_temporal_duration(vm, years_value, months_value, weeks_value, days_value, hours_value, factored_minutes_value, factored_seconds_value, factored_milliseconds_value, factored_microseconds_value, factored_nanoseconds_value));
}

// 14.4.1.1 GetOptionsObject ( options ), https://tc39.es/proposal-temporal/#sec-getoptionsobject
ThrowCompletionOr<GC::Ref<Object>> get_options_object(VM& vm, Value options)
{
    auto& realm = *vm.current_realm();

    // 1. If options is undefined, then
    if (options.is_undefined()) {
        // a. Return OrdinaryObjectCreate(null).
        return Object::create(realm, nullptr);
    }

    // 2. If options is an Object, then
    if (options.is_object()) {
        // a. Return options.
        return options.as_object();
    }

    // 3. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::NotAnObject, "Options");
}

// 14.4.1.2 GetOption ( options, property, type, values, default ), https://tc39.es/proposal-temporal/#sec-getoption
ThrowCompletionOr<Value> get_option(VM& vm, Object const& options, PropertyKey const& property, OptionType type, ReadonlySpan<StringView> values, OptionDefault const& default_)
{
    VERIFY(property.is_string());

    // 1. Let value be ? Get(options, property).
    auto value = TRY(options.get(property));

    // 2. If value is undefined, then
    if (value.is_undefined()) {
        // a. If default is REQUIRED, throw a RangeError exception.
        if (default_.has<DefaultRequired>())
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, "undefined"sv, property.as_string());

        // b. Return default.
        return default_.visit(
            [](DefaultRequired) -> Value { VERIFY_NOT_REACHED(); },
            [](Empty) -> Value { return js_undefined(); },
            [](bool default_) -> Value { return Value { default_ }; },
            [](double default_) -> Value { return Value { default_ }; },
            [&](StringView default_) -> Value { return PrimitiveString::create(vm, default_); });
    }

    // 3. If type is BOOLEAN, then
    if (type == OptionType::Boolean) {
        // a. Set value to ToBoolean(value).
        value = Value { value.to_boolean() };
    }
    // 4. Else,
    else {
        // a. Assert: type is STRING.
        VERIFY(type == OptionType::String);

        // b. Set value to ? ToString(value).
        value = TRY(value.to_primitive_string(vm));
    }

    // 5. If values is not EMPTY and values does not contain value, throw a RangeError exception.
    if (!values.is_empty()) {
        // NOTE: Every location in the spec that invokes GetOption with type=boolean also has values=undefined.
        VERIFY(value.is_string());

        if (auto value_string = value.as_string().utf8_string(); !values.contains_slow(value_string))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, value_string, property.as_string());
    }

    // 6. Return value.
    return value;
}

}
