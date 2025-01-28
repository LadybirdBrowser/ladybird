/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/BigFraction/BigFraction.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

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

// 13.1 ISODateToEpochDays ( year, month, date ), https://tc39.es/proposal-temporal/#sec-isodatetoepochdays
double iso_date_to_epoch_days(double year, double month, double date)
{
    // 1. Let resolvedYear be year + floor(month / 12).
    // 2. Let resolvedMonth be month modulo 12.
    // 3. Find a time t such that EpochTimeToEpochYear(t) = resolvedYear, EpochTimeToMonthInYear(t) = resolvedMonth, and EpochTimeToDate(t) = 1.
    // 4. Return EpochTimeToDayNumber(t) + date - 1.

    // EDITOR'S NOTE: This operation corresponds to ECMA-262 operation MakeDay(year, month, date). It calculates the
    //                result in mathematical values instead of Number values. These two operations would be unified when
    //                https://github.com/tc39/ecma262/issues/1087 is fixed.

    // Since we don't have a real MV type to work with, let's defer to MakeDay.
    return JS::make_day(year, month, date);
}

// 13.2 EpochDaysToEpochMs ( day, time ), https://tc39.es/proposal-temporal/#sec-epochdaystoepochms
double epoch_days_to_epoch_ms(double day, double time)
{
    // 1. Return day × ℝ(msPerDay) + time.
    return day * JS::ms_per_day + time;
}

// 13.4 CheckISODaysRange ( isoDate ), https://tc39.es/proposal-temporal/#sec-checkisodaysrange
ThrowCompletionOr<void> check_iso_days_range(VM& vm, ISODate iso_date)
{
    // 1. If abs(ISODateToEpochDays(isoDate.[[Year]], isoDate.[[Month]] - 1, isoDate.[[Day]])) > 10**8, then
    if (fabs(iso_date_to_epoch_days(iso_date.year, iso_date.month - 1, iso_date.day)) > 100'000'000) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODate);
    }

    // 2. Return unused.
    return {};
}

// 13.6 GetTemporalOverflowOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporaloverflowoption
ThrowCompletionOr<Overflow> get_temporal_overflow_option(VM& vm, Object const& options)
{
    // 1. Let stringValue be ? GetOption(options, "overflow", STRING, « "constrain", "reject" », "constrain").
    auto string_value = TRY(get_option(vm, options, vm.names.overflow, OptionType::String, { "constrain"sv, "reject"sv }, "constrain"sv));

    // 2. If stringValue is "constrain", return CONSTRAIN.
    if (string_value.as_string().utf8_string() == "constrain"sv)
        return Overflow::Constrain;

    // 3. Return REJECT.
    return Overflow::Reject;
}

// 13.7 GetTemporalDisambiguationOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporaldisambiguationoption
ThrowCompletionOr<Disambiguation> get_temporal_disambiguation_option(VM& vm, Object const& options)
{
    // 1. Let stringValue be ? GetOption(options, "disambiguation", STRING, « "compatible", "earlier", "later", "reject" », "compatible").
    auto string_value = TRY(get_option(vm, options, vm.names.disambiguation, OptionType::String, { "compatible"sv, "earlier"sv, "later"sv, "reject"sv }, "compatible"sv));
    auto string_view = string_value.as_string().utf8_string_view();

    // 2. If stringValue is "compatible", return COMPATIBLE.
    if (string_view == "compatible"sv)
        return Disambiguation::Compatible;

    // 3. If stringValue is "earlier", return EARLIER.
    if (string_view == "earlier"sv)
        return Disambiguation::Earlier;

    // 4. If stringValue is "later", return LATER.
    if (string_view == "later"sv)
        return Disambiguation::Later;

    // 5. Return REJECT.
    return Disambiguation::Reject;
}

// 13.8 NegateRoundingMode ( roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-negateroundingmode
RoundingMode negate_rounding_mode(RoundingMode rounding_mode)
{
    // 1. If roundingMode is CEIL, return FLOOR.
    if (rounding_mode == RoundingMode::Ceil)
        return RoundingMode::Floor;

    // 2. If roundingMode is FLOOR, return CEIL.
    if (rounding_mode == RoundingMode::Floor)
        return RoundingMode::Ceil;

    // 3. If roundingMode is HALF-CEIL, return HALF-FLOOR.
    if (rounding_mode == RoundingMode::HalfCeil)
        return RoundingMode::HalfFloor;

    // 4. If roundingMode is HALF-FLOOR, return HALF-CEIL.
    if (rounding_mode == RoundingMode::HalfFloor)
        return RoundingMode::HalfCeil;

    // 5. Return roundingMode.
    return rounding_mode;
}

// 13.9 GetTemporalOffsetOption ( options, fallback ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporaloffsetoption
ThrowCompletionOr<OffsetOption> get_temporal_offset_option(VM& vm, Object const& options, OffsetOption fallback)
{
    auto string_fallback = [&]() {
        switch (fallback) {
        // 1. If fallback is PREFER, let stringFallback be "prefer".
        case OffsetOption::Prefer:
            return "prefer"sv;
        // 2. Else if fallback is USE, let stringFallback be "use".
        case OffsetOption::Use:
            return "use"sv;
        // 3. Else if fallback is IGNORE, let stringFallback be "ignore".
        case OffsetOption::Ignore:
            return "ignore"sv;
        // 4. Else, let stringFallback be "reject".
        case OffsetOption::Reject:
            return "reject"sv;
        }
        VERIFY_NOT_REACHED();
    }();

    // 5. Let stringValue be ? GetOption(options, "offset", STRING, « "prefer", "use", "ignore", "reject" », stringFallback).
    auto string_value = TRY(get_option(vm, options, vm.names.offset, OptionType::String, { "prefer"sv, "use"sv, "ignore"sv, "reject"sv }, string_fallback));
    auto string_view = string_value.as_string().utf8_string_view();

    // 6. If stringValue is "prefer", return PREFER.
    if (string_view == "prefer"sv)
        return OffsetOption::Prefer;

    // 7. If stringValue is "use", return USE.
    if (string_view == "use"sv)
        return OffsetOption::Use;

    // 8. If stringValue is "ignore", return IGNORE.
    if (string_view == "ignore"sv)
        return OffsetOption::Ignore;

    // 9. Return REJECT.
    return OffsetOption::Reject;
}

// 13.10 GetTemporalShowCalendarNameOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalshowcalendarnameoption
ThrowCompletionOr<ShowCalendar> get_temporal_show_calendar_name_option(VM& vm, Object const& options)
{
    // 1. Let stringValue be ? GetOption(options, "calendarName", STRING, « "auto", "always", "never", "critical" », "auto").
    auto string_value = TRY(get_option(vm, options, vm.names.calendarName, OptionType::String, { "auto"sv, "always"sv, "never"sv, "critical"sv }, "auto"sv));
    auto string_view = string_value.as_string().utf8_string_view();

    // 2. If stringValue is "always", return ALWAYS.
    if (string_view == "always"sv)
        return ShowCalendar::Always;

    // 3. If stringValue is "never", return NEVER.
    if (string_view == "never"sv)
        return ShowCalendar::Never;

    // 4. If stringValue is "critical", return CRITICAL.
    if (string_view == "critical"sv)
        return ShowCalendar::Critical;

    // 5. Return AUTO.
    return ShowCalendar::Auto;
}

// 13.11 GetTemporalShowTimeZoneNameOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalshowtimezonenameoption
ThrowCompletionOr<ShowTimeZoneName> get_temporal_show_time_zone_name_option(VM& vm, Object const& options)
{
    // 1. Let stringValue be ? GetOption(options, "timeZoneName", STRING, « "auto", "never", "critical" », "auto").
    auto string_value = TRY(get_option(vm, options, vm.names.timeZoneName, OptionType::String, { "auto"sv, "never"sv, "critical"sv }, "auto"sv));
    auto string_view = string_value.as_string().utf8_string_view();

    // 2. If stringValue is "never", return NEVER.
    if (string_view == "never"sv)
        return ShowTimeZoneName::Never;

    // 3. If stringValue is "critical", return CRITICAL.
    if (string_view == "critical"sv)
        return ShowTimeZoneName::Critical;

    // 4. Return AUTO.
    return ShowTimeZoneName::Auto;
}

// 13.12 GetTemporalShowOffsetOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalshowoffsetoption
ThrowCompletionOr<ShowOffset> get_temporal_show_offset_option(VM& vm, Object const& options)
{
    // 1. Let stringValue be ? GetOption(options, "offset", STRING, « "auto", "never" », "auto").
    auto string_value = TRY(get_option(vm, options, vm.names.offset, OptionType::String, { "auto"sv, "never"sv }, "auto"sv));
    auto string_view = string_value.as_string().utf8_string_view();

    // 2. If stringValue is "never", return never.
    if (string_view == "never"sv)
        return ShowOffset::Never;

    // 3. Return auto.
    return ShowOffset::Auto;
}

// 13.13 GetDirectionOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-getdirectionoption
ThrowCompletionOr<Direction> get_direction_option(VM& vm, Object const& options)
{
    // 1. Let stringValue be ? GetOption(options, "direction", STRING, « "next", "previous" », REQUIRED).
    auto string_value = TRY(get_option(vm, options, vm.names.direction, OptionType::String, { "next"sv, "previous"sv }, Required {}));
    auto string_view = string_value.as_string().utf8_string_view();

    // 2. If stringValue is "next", return NEXT.
    if (string_view == "next"sv)
        return Direction::Next;

    // 3. Return PREVIOUS.
    return Direction::Previous;
}

// 13.14 ValidateTemporalRoundingIncrement ( increment, dividend, inclusive ), https://tc39.es/proposal-temporal/#sec-validatetemporalroundingincrement
ThrowCompletionOr<void> validate_temporal_rounding_increment(VM& vm, u64 increment, u64 dividend, bool inclusive)
{
    u64 maximum = 0;

    // 1. If inclusive is true, then
    if (inclusive) {
        // a. Let maximum be dividend.
        maximum = dividend;
    }
    // 2. Else,
    else {
        // a. Assert: dividend > 1.
        VERIFY(dividend > 1);

        // b. Let maximum be dividend - 1.
        maximum = dividend - 1;
    }

    // 3. If increment > maximum, throw a RangeError exception.
    if (increment > maximum)
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, increment, "roundingIncrement");

    // 5. If dividend modulo increment ≠ 0, then
    if (modulo(dividend, increment) != 0) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, increment, "roundingIncrement");
    }

    // 6. Return UNUSED.
    return {};
}

// 13.15 GetTemporalFractionalSecondDigitsOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalfractionalseconddigitsoption
ThrowCompletionOr<Precision> get_temporal_fractional_second_digits_option(VM& vm, Object const& options)
{
    // 1. Let digitsValue be ? Get(options, "fractionalSecondDigits").
    auto digits_value = TRY(options.get(vm.names.fractionalSecondDigits));

    // 2. If digitsValue is undefined, return AUTO.
    if (digits_value.is_undefined())
        return Precision { Auto {} };

    // 3. If digitsValue is not a Number, then
    if (!digits_value.is_number()) {
        // a. If ? ToString(digitsValue) is not "auto", throw a RangeError exception.
        auto digits_value_string = TRY(digits_value.to_string(vm));

        if (digits_value_string != "auto"sv)
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, digits_value, vm.names.fractionalSecondDigits);

        // b. Return AUTO.
        return Precision { Auto {} };
    }

    // 4. If digitsValue is NaN, +∞𝔽, or -∞𝔽, throw a RangeError exception.
    if (digits_value.is_nan() || digits_value.is_infinity())
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, digits_value, vm.names.fractionalSecondDigits);

    // 5. Let digitCount be floor(ℝ(digitsValue)).
    auto digit_count = floor(digits_value.as_double());

    // 6. If digitCount < 0 or digitCount > 9, throw a RangeError exception.
    if (digit_count < 0 || digit_count > 9)
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, digits_value, vm.names.fractionalSecondDigits);

    // 7. Return digitCount.
    return Precision { static_cast<u8>(digit_count) };
}

// 13.16 ToSecondsStringPrecisionRecord ( smallestUnit, fractionalDigitCount ), https://tc39.es/proposal-temporal/#sec-temporal-tosecondsstringprecisionrecord
SecondsStringPrecision to_seconds_string_precision_record(UnitValue smallest_unit, Precision fractional_digit_count)
{
    if (auto const* unit = smallest_unit.get_pointer<Unit>()) {
        // 1. If smallestUnit is MINUTE, then
        if (*unit == Unit::Minute) {
            // a. Return the Record { [[Precision]]: MINUTE, [[Unit]]: MINUTE, [[Increment]]: 1  }.
            return { .precision = SecondsStringPrecision::Minute {}, .unit = Unit::Minute, .increment = 1 };
        }

        // 2. If smallestUnit is SECOND, then
        if (*unit == Unit::Second) {
            // a. Return the Record { [[Precision]]: 0, [[Unit]]: SECOND, [[Increment]]: 1  }.
            return { .precision = 0, .unit = Unit::Second, .increment = 1 };
        }

        // 3. If smallestUnit is MILLISECOND, then
        if (*unit == Unit::Millisecond) {
            // a. Return the Record { [[Precision]]: 3, [[Unit]]: MILLISECOND, [[Increment]]: 1  }.
            return { .precision = 3, .unit = Unit::Millisecond, .increment = 1 };
        }

        // 4. If smallestUnit is MICROSECOND, then
        if (*unit == Unit::Microsecond) {
            // a. Return the Record { [[Precision]]: 6, [[Unit]]: MICROSECOND, [[Increment]]: 1  }.
            return { .precision = 6, .unit = Unit::Microsecond, .increment = 1 };
        }

        // 5. If smallestUnit is NANOSECOND, then
        if (*unit == Unit::Nanosecond) {
            // a. Return the Record { [[Precision]]: 9, [[Unit]]: NANOSECOND, [[Increment]]: 1  }.
            return { .precision = 9, .unit = Unit::Nanosecond, .increment = 1 };
        }
    }

    // 6. Assert: smallestUnit is UNSET.
    VERIFY(smallest_unit.has<Unset>());

    // 7. If fractionalDigitCount is auto, then
    if (fractional_digit_count.has<Auto>()) {
        // a. Return the Record { [[Precision]]: AUTO, [[Unit]]: NANOSECOND, [[Increment]]: 1  }.
        return { .precision = Auto {}, .unit = Unit::Nanosecond, .increment = 1 };
    }

    auto fractional_digits = fractional_digit_count.get<u8>();

    // 8. If fractionalDigitCount = 0, then
    if (fractional_digits == 0) {
        // a. Return the Record { [[Precision]]: 0, [[Unit]]: SECOND, [[Increment]]: 1  }.
        return { .precision = 0, .unit = Unit::Second, .increment = 1 };
    }

    // 9. If fractionalDigitCount is in the inclusive interval from 1 to 3, then
    if (fractional_digits >= 1 && fractional_digits <= 3) {
        // a. Return the Record { [[Precision]]: fractionalDigitCount, [[Unit]]: MILLISECOND, [[Increment]]: 10**(3 - fractionalDigitCount)  }.
        return { .precision = fractional_digits, .unit = Unit::Millisecond, .increment = static_cast<u8>(pow(10, 3 - fractional_digits)) };
    }

    // 10. If fractionalDigitCount is in the inclusive interval from 4 to 6, then
    if (fractional_digits >= 4 && fractional_digits <= 6) {
        // a. Return the Record { [[Precision]]: fractionalDigitCount, [[Unit]]: MICROSECOND, [[Increment]]: 10**(6 - fractionalDigitCount)  }.
        return { .precision = fractional_digits, .unit = Unit::Microsecond, .increment = static_cast<u8>(pow(10, 6 - fractional_digits)) };
    }

    // 11. Assert: fractionalDigitCount is in the inclusive interval from 7 to 9.
    VERIFY(fractional_digits >= 7 && fractional_digits <= 9);

    // 12. Return the Record { [[Precision]]: fractionalDigitCount, [[Unit]]: NANOSECOND, [[Increment]]: 10**(9 - fractionalDigitCount)  }.
    return { .precision = fractional_digits, .unit = Unit::Nanosecond, .increment = static_cast<u8>(pow(10, 9 - fractional_digits)) };
}

// 13.17 GetTemporalUnitValuedOption ( options, key, unitGroup, default [ , extraValues ] ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalunitvaluedoption
ThrowCompletionOr<UnitValue> get_temporal_unit_valued_option(VM& vm, Object const& options, PropertyKey const& key, UnitGroup unit_group, UnitDefault const& default_, ReadonlySpan<UnitValue> extra_values)
{
    // 1. Let allowedValues be a new empty List.
    Vector<UnitValue> allowed_values;

    // 2. For each row of Table 21, except the header row, in table order, do
    for (auto const& row : temporal_units) {
        // a. Let unit be the value in the "Value" column of the row.
        auto unit = row.value;

        // b. If the "Category" column of the row is DATE and unitGroup is DATE or DATETIME, append unit to allowedValues.
        if (row.category == UnitCategory::Date && (unit_group == UnitGroup::Date || unit_group == UnitGroup::DateTime))
            allowed_values.append(unit);

        // c. Else if the "Category" column of the row is TIME and unitGroup is TIME or DATETIME, append unit to allowedValues.
        if (row.category == UnitCategory::Time && (unit_group == UnitGroup::Time || unit_group == UnitGroup::DateTime))
            allowed_values.append(unit);
    }

    // 3. If extraValues is present, then
    if (!extra_values.is_empty()) {
        // a. Set allowedValues to the list-concatenation of allowedValues and extraValues.
        for (auto value : extra_values)
            allowed_values.append(value);
    }

    OptionDefault default_value;

    // 4. If default is UNSET, then
    if (default_.has<Unset>()) {
        // a. Let defaultValue be undefined.
        default_value = {};
    }
    // 5. Else if default is REQUIRED, then
    else if (default_.has<Required>()) {
        // a. Let defaultValue be REQUIRED.
        default_value = Required {};
    }
    // 6. Else if default is AUTO, then
    else if (default_.has<Auto>()) {
        // a. Append default to allowedValues.
        allowed_values.append(Auto {});

        // b. Let defaultValue be "auto".
        default_value = "auto"sv;
    }
    // 7. Else,
    else {
        auto unit = default_.get<Unit>();

        // a. Assert: allowedValues contains default.

        // b. Let defaultValue be the value in the "Singular property name" column of Table 21 corresponding to the row
        //    with default in the "Value" column.
        default_value = temporal_units[to_underlying(unit)].singular_property_name;
    }

    // 8. Let allowedStrings be a new empty List.
    Vector<StringView> allowed_strings;

    // 9. For each element value of allowedValues, do
    for (auto value : allowed_values) {
        // a. If value is auto, then
        if (value.has<Auto>()) {
            // i. Append "auto" to allowedStrings.
            allowed_strings.append("auto"sv);
        }
        // b. Else,
        else {
            auto unit = value.get<Unit>();

            // i. Let singularName be the value in the "Singular property name" column of Table 21 corresponding to the
            //    row with value in the "Value" column.
            auto singular_name = temporal_units[to_underlying(unit)].singular_property_name;

            // ii. Append singularName to allowedStrings.
            allowed_strings.append(singular_name);

            // iii. Let pluralName be the value in the "Plural property name" column of the corresponding row.
            auto plural_name = temporal_units[to_underlying(unit)].plural_property_name;

            // iv. Append pluralName to allowedStrings.
            allowed_strings.append(plural_name);
        }
    }

    // 10. NOTE: For each singular Temporal unit name that is contained within allowedStrings, the corresponding plural
    //     name is also contained within it.

    // 11. Let value be ? GetOption(options, key, STRING, allowedStrings, defaultValue).
    auto value = TRY(get_option(vm, options, key, OptionType::String, allowed_strings, default_value));

    // 12. If value is undefined, return UNSET.
    if (value.is_undefined())
        return UnitValue { Unset {} };

    auto value_string = value.as_string().utf8_string_view();

    // 13. If value is "auto", return AUTO.
    if (value_string == "auto"sv)
        return UnitValue { Auto {} };

    // 14. Return the value in the "Value" column of Table 21 corresponding to the row with value in its "Singular
    //     property name" or "Plural property name" column.
    for (auto const& row : temporal_units) {
        if (value_string.is_one_of(row.singular_property_name, row.plural_property_name))
            return UnitValue { row.value };
    }

    VERIFY_NOT_REACHED();
}

// 13.18 GetTemporalRelativeToOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalrelativetooption
ThrowCompletionOr<RelativeTo> get_temporal_relative_to_option(VM& vm, Object const& options)
{
    // 1. Let value be ? Get(options, "relativeTo").
    auto value = TRY(options.get(vm.names.relativeTo));

    // 2. If value is undefined, return the Record { [[PlainRelativeTo]]: undefined, [[ZonedRelativeTo]]: undefined }.
    if (value.is_undefined())
        return RelativeTo { .plain_relative_to = {}, .zoned_relative_to = {} };

    // 3. Let offsetBehaviour be OPTION.
    auto offset_behavior = OffsetBehavior::Option;

    // 4. Let matchBehaviour be MATCH-EXACTLY.
    auto match_behavior = MatchBehavior::MatchExactly;

    String calendar;
    Optional<String> time_zone;
    Optional<String> offset_string;

    ISODate iso_date;
    Variant<ParsedISODateTime::StartOfDay, Time> time { Time {} };

    // 5. If value is an Object, then
    if (value.is_object()) {
        auto& object = value.as_object();

        // a. If value has an [[InitializedTemporalZonedDateTime]] internal slot, then
        if (is<ZonedDateTime>(object)) {
            // i. Return the Record { [[PlainRelativeTo]]: undefined, [[ZonedRelativeTo]]: value }.
            return RelativeTo { .plain_relative_to = {}, .zoned_relative_to = static_cast<ZonedDateTime&>(object) };
        }

        // b. If value has an [[InitializedTemporalDate]] internal slot, then
        if (is<PlainDate>(object)) {
            // i. Return the Record { [[PlainRelativeTo]]: value, [[ZonedRelativeTo]]: undefined }.
            return RelativeTo { .plain_relative_to = static_cast<PlainDate&>(object), .zoned_relative_to = {} };
        }

        // c. If value has an [[InitializedTemporalDateTime]] internal slot, then
        if (is<PlainDateTime>(object)) {
            auto const& plain_date_time = static_cast<PlainDateTime const&>(object);

            // i. Let plainDate be ! CreateTemporalDate(value.[[ISODateTime]].[[ISODate]], value.[[Calendar]]).
            auto plain_date = MUST(create_temporal_date(vm, plain_date_time.iso_date_time().iso_date, plain_date_time.calendar()));

            // ii. Return the Record { [[PlainRelativeTo]]: plainDate, [[ZonedRelativeTo]]: undefined }.
            return RelativeTo { .plain_relative_to = plain_date, .zoned_relative_to = {} };
        }

        // d. Let calendar be ? GetTemporalCalendarIdentifierWithISODefault(value).
        calendar = TRY(get_temporal_calendar_identifier_with_iso_default(vm, object));

        // e. Let fields be ? PrepareCalendarFields(calendar, value, « YEAR, MONTH, MONTH-CODE, DAY », « HOUR, MINUTE, SECOND, MILLISECOND, MICROSECOND, NANOSECOND, OFFSET, TIME-ZONE », «»).
        static constexpr auto calendar_field_names = to_array({ CalendarField::Year, CalendarField::Month, CalendarField::MonthCode, CalendarField::Day });
        static constexpr auto non_calendar_field_names = to_array({ CalendarField::Hour, CalendarField::Minute, CalendarField::Second, CalendarField::Millisecond, CalendarField::Microsecond, CalendarField::Nanosecond, CalendarField::Offset, CalendarField::TimeZone });
        auto fields = TRY(prepare_calendar_fields(vm, calendar, object, calendar_field_names, non_calendar_field_names, CalendarFieldList {}));

        // f. Let result be ? InterpretTemporalDateTimeFields(calendar, fields, CONSTRAIN).
        auto result = TRY(interpret_temporal_date_time_fields(vm, calendar, fields, Overflow::Constrain));

        // g. Let timeZone be fields.[[TimeZone]].
        time_zone = move(fields.time_zone);

        // h. Let offsetString be fields.[[OffsetString]].
        offset_string = move(fields.offset_string);

        // i. If offsetString is UNSET, then
        if (!offset_string.has_value()) {
            // i. Set offsetBehaviour to WALL.
            offset_behavior = OffsetBehavior::Wall;
        }

        // j. Let isoDate be result.[[ISODate]].
        iso_date = result.iso_date;

        // k. Let time be result.[[Time]].
        time = result.time;
    }
    // 6. Else,
    else {
        // a. If value is not a String, throw a TypeError exception.
        if (!value.is_string())
            return vm.throw_completion<TypeError>(ErrorType::NotAString, vm.names.relativeTo);

        // b. Let result be ? ParseISODateTime(value, « TemporalDateTimeString[+Zoned], TemporalDateTimeString[~Zoned] »).
        auto result = TRY(parse_iso_date_time(vm, value.as_string().utf8_string_view(), { { Production::TemporalZonedDateTimeString, Production::TemporalDateTimeString } }));

        // c. Let offsetString be result.[[TimeZone]].[[OffsetString]].
        offset_string = move(result.time_zone.offset_string);

        // d. Let annotation be result.[[TimeZone]].[[TimeZoneAnnotation]].
        auto annotation = move(result.time_zone.time_zone_annotation);

        // e. If annotation is EMPTY, then
        if (!annotation.has_value()) {
            // i. Let timeZone be UNSET.
            time_zone = {};
        }
        // f. Else,
        else {
            // i. Let timeZone be ? ToTemporalTimeZoneIdentifier(annotation).
            time_zone = TRY(to_temporal_time_zone_identifier(vm, *annotation));

            // ii. If result.[[TimeZone]].[[Z]] is true, then
            if (result.time_zone.z_designator) {
                // 1. Set offsetBehaviour to EXACT.
                offset_behavior = OffsetBehavior::Exact;
            }
            // iii. Else if offsetString is EMPTY, then
            else if (!offset_string.has_value()) {
                // 1. Set offsetBehaviour to WALL.
                offset_behavior = OffsetBehavior::Wall;
            }

            // iv. Set matchBehaviour to MATCH-MINUTES.
            match_behavior = MatchBehavior::MatchMinutes;
        }

        // g. Let calendar be result.[[Calendar]].
        // h. If calendar is EMPTY, set calendar to "iso8601".
        calendar = result.calendar.value_or("iso8601"_string);

        // i. Set calendar to ? CanonicalizeCalendar(calendar).
        calendar = TRY(canonicalize_calendar(vm, calendar));

        // j. Let isoDate be CreateISODateRecord(result.[[Year]], result.[[Month]], result.[[Day]]).
        iso_date = create_iso_date_record(*result.year, result.month, result.day);

        // k. Let time be result.[[Time]].
        time = result.time;
    }

    // 7. If timeZone is UNSET, then
    if (!time_zone.has_value()) {
        // a. Let plainDate be ? CreateTemporalDate(isoDate, calendar).
        auto plain_date = TRY(create_temporal_date(vm, iso_date, move(calendar)));

        // b. Return the Record { [[PlainRelativeTo]]: plainDate, [[ZonedRelativeTo]]: undefined }.
        return RelativeTo { .plain_relative_to = plain_date, .zoned_relative_to = {} };
    }

    double offset_nanoseconds = 0;

    // 8. If offsetBehaviour is OPTION, then
    if (offset_behavior == OffsetBehavior::Option) {
        // a. Let offsetNs be ! ParseDateTimeUTCOffset(offsetString).
        offset_nanoseconds = parse_date_time_utc_offset(*offset_string);
    }
    // 9. Else,
    else {
        // a. Let offsetNs be 0.
        offset_nanoseconds = 0;
    }

    // 10. Let epochNanoseconds be ? InterpretISODateTimeOffset(isoDate, time, offsetBehaviour, offsetNs, timeZone, COMPATIBLE, REJECT, matchBehaviour).
    auto epoch_nanoseconds = TRY(interpret_iso_date_time_offset(vm, iso_date, time, offset_behavior, offset_nanoseconds, *time_zone, Disambiguation::Compatible, OffsetOption::Reject, match_behavior));

    // 11. Let zonedRelativeTo be ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
    auto zoned_relative_to = MUST(create_temporal_zoned_date_time(vm, BigInt::create(vm, move(epoch_nanoseconds)), time_zone.release_value(), move(calendar)));

    // 12. Return the Record { [[PlainRelativeTo]]: undefined, [[ZonedRelativeTo]]: zonedRelativeTo }.
    return RelativeTo { .plain_relative_to = {}, .zoned_relative_to = zoned_relative_to };
}

// 13.19 LargerOfTwoTemporalUnits ( u1, u2 ), https://tc39.es/proposal-temporal/#sec-temporal-largeroftwotemporalunits
Unit larger_of_two_temporal_units(Unit unit1, Unit unit2)
{
    // 1. For each row of Table 21, except the header row, in table order, do
    for (auto const& row : temporal_units) {
        // a. Let unit be the value in the "Value" column of the row.
        auto unit = row.value;

        // b. If u1 is unit, return unit.
        if (unit1 == unit)
            return unit;

        // c. If u2 is unit, return unit.
        if (unit2 == unit)
            return unit;
    }

    VERIFY_NOT_REACHED();
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

// 13.22 MaximumTemporalDurationRoundingIncrement ( unit ), https://tc39.es/proposal-temporal/#sec-temporal-maximumtemporaldurationroundingincrement
RoundingIncrement maximum_temporal_duration_rounding_increment(Unit unit)
{
    // 1. Return the value from the "Maximum duration rounding increment" column of the row of Table 21 in which unit is
    //    in the "Value" column.
    return temporal_units[to_underlying(unit)].maximum_duration_rounding_increment;
}

// AD-HOC
Crypto::UnsignedBigInteger const& temporal_unit_length_in_nanoseconds(Unit unit)
{
    switch (unit) {
    case Unit::Day:
        return NANOSECONDS_PER_DAY;
    case Unit::Hour:
        return NANOSECONDS_PER_HOUR;
    case Unit::Minute:
        return NANOSECONDS_PER_MINUTE;
    case Unit::Second:
        return NANOSECONDS_PER_SECOND;
    case Unit::Millisecond:
        return NANOSECONDS_PER_MILLISECOND;
    case Unit::Microsecond:
        return NANOSECONDS_PER_MICROSECOND;
    case Unit::Nanosecond:
        return NANOSECONDS_PER_NANOSECOND;
    default:
        VERIFY_NOT_REACHED();
    }
}

// 13.23 IsPartialTemporalObject ( value ), https://tc39.es/proposal-temporal/#sec-temporal-ispartialtemporalobject
ThrowCompletionOr<bool> is_partial_temporal_object(VM& vm, Value value)
{
    // 1. If value is not an Object, return false.
    if (!value.is_object())
        return false;

    auto const& object = value.as_object();

    // 2. If value has an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], [[InitializedTemporalMonthDay]],
    //    [[InitializedTemporalTime]], [[InitializedTemporalYearMonth]], or [[InitializedTemporalZonedDateTime]] internal
    //    slot, return false.
    if (is<PlainDate>(object))
        return false;
    if (is<PlainDateTime>(object))
        return false;
    if (is<PlainMonthDay>(object))
        return false;
    if (is<PlainTime>(object))
        return false;
    if (is<PlainYearMonth>(object))
        return false;
    if (is<ZonedDateTime>(object))
        return false;

    // 3. Let calendarProperty be ? Get(value, "calendar").
    auto calendar_property = TRY(object.get(vm.names.calendar));

    // 4. If calendarProperty is not undefined, return false.
    if (!calendar_property.is_undefined())
        return false;

    // 5. Let timeZoneProperty be ? Get(value, "timeZone").
    auto time_zone_property = TRY(object.get(vm.names.timeZone));

    // 6. If timeZoneProperty is not undefined, return false.
    if (!time_zone_property.is_undefined())
        return false;

    // 7. Return true.
    return true;
}

// 13.24 FormatFractionalSeconds ( subSecondNanoseconds, precision ), https://tc39.es/proposal-temporal/#sec-temporal-formatfractionalseconds
String format_fractional_seconds(u64 sub_second_nanoseconds, Precision precision)
{
    String fraction_string;

    // 1. If precision is auto, then
    if (precision.has<Auto>()) {
        // a. If subSecondNanoseconds = 0, return the empty String.
        if (sub_second_nanoseconds == 0)
            return String {};

        // b. Let fractionString be ToZeroPaddedDecimalString(subSecondNanoseconds, 9).
        fraction_string = MUST(String::formatted("{:09}", sub_second_nanoseconds));

        // c. Set fractionString to the longest prefix of fractionString ending with a code unit other than 0x0030 (DIGIT ZERO).
        fraction_string = MUST(fraction_string.trim("0"sv, TrimMode::Right));
    }
    // 2. Else,
    else {
        // a. If precision = 0, return the empty String.
        if (precision.get<u8>() == 0)
            return String {};

        // b. Let fractionString be ToZeroPaddedDecimalString(subSecondNanoseconds, 9).
        fraction_string = MUST(String::formatted("{:09}", sub_second_nanoseconds));

        // c. Set fractionString to the substring of fractionString from 0 to precision.
        fraction_string = MUST(fraction_string.substring_from_byte_offset(0, precision.get<u8>()));
    }

    // 3. Return the string-concatenation of the code unit 0x002E (FULL STOP) and fractionString.
    return MUST(String::formatted(".{}", fraction_string));
}

// 13.25 FormatTimeString ( hour, minute, second, subSecondNanoseconds, precision [ , style ] ), https://tc39.es/proposal-temporal/#sec-temporal-formattimestring
String format_time_string(u8 hour, u8 minute, u8 second, u64 sub_second_nanoseconds, SecondsStringPrecision::Precision precision, Optional<TimeStyle> style)
{
    // 1. If style is present and style is UNSEPARATED, let separator be the empty String; otherwise, let separator be ":".
    auto separator = style == TimeStyle::Unseparated ? ""sv : ":"sv;

    // 2. Let hh be ToZeroPaddedDecimalString(hour, 2).
    // 3. Let mm be ToZeroPaddedDecimalString(minute, 2).

    // 4. If precision is minute, return the string-concatenation of hh, separator, and mm.
    if (precision.has<SecondsStringPrecision::Minute>())
        return MUST(String::formatted("{:02}{}{:02}", hour, separator, minute));

    // 5. Let ss be ToZeroPaddedDecimalString(second, 2).
    // 6. Let subSecondsPart be FormatFractionalSeconds(subSecondNanoseconds, precision).
    auto sub_seconds_part = format_fractional_seconds(sub_second_nanoseconds, precision.downcast<Auto, u8>());

    // 7. Return the string-concatenation of hh, separator, mm, separator, ss, and subSecondsPart.
    return MUST(String::formatted("{:02}{}{:02}{}{:02}{}", hour, separator, minute, separator, second, sub_seconds_part));
}

// 13.26 GetUnsignedRoundingMode ( roundingMode, sign ), https://tc39.es/proposal-temporal/#sec-getunsignedroundingmode
UnsignedRoundingMode get_unsigned_rounding_mode(RoundingMode rounding_mode, Sign sign)
{
    // 1. Return the specification type in the "Unsigned Rounding Mode" column of Table 22 for the row where the value
    //    in the "Rounding Mode" column is roundingMode and the value in the "Sign" column is sign.
    switch (rounding_mode) {
    case RoundingMode::Ceil:
        return sign == Sign::Positive ? UnsignedRoundingMode::Infinity : UnsignedRoundingMode::Zero;
    case RoundingMode::Floor:
        return sign == Sign::Positive ? UnsignedRoundingMode::Zero : UnsignedRoundingMode::Infinity;
    case RoundingMode::Expand:
        return UnsignedRoundingMode::Infinity;
    case RoundingMode::Trunc:
        return UnsignedRoundingMode::Zero;
    case RoundingMode::HalfCeil:
        return sign == Sign::Positive ? UnsignedRoundingMode::HalfInfinity : UnsignedRoundingMode::HalfZero;
    case RoundingMode::HalfFloor:
        return sign == Sign::Positive ? UnsignedRoundingMode::HalfZero : UnsignedRoundingMode::HalfInfinity;
    case RoundingMode::HalfExpand:
        return UnsignedRoundingMode::HalfInfinity;
    case RoundingMode::HalfTrunc:
        return UnsignedRoundingMode::HalfZero;
    case RoundingMode::HalfEven:
        return UnsignedRoundingMode::HalfEven;
    }

    VERIFY_NOT_REACHED();
}

// 13.27 ApplyUnsignedRoundingMode ( x, r1, r2, unsignedRoundingMode ), https://tc39.es/proposal-temporal/#sec-applyunsignedroundingmode
double apply_unsigned_rounding_mode(double x, double r1, double r2, UnsignedRoundingMode unsigned_rounding_mode)
{
    // 1. If x = r1, return r1.
    if (x == r1)
        return r1;

    // 2. Assert: r1 < x < r2.
    VERIFY(r1 < x && x < r2);

    // 3. Assert: unsignedRoundingMode is not undefined.

    // 4. If unsignedRoundingMode is ZERO, return r1.
    if (unsigned_rounding_mode == UnsignedRoundingMode::Zero)
        return r1;

    // 5. If unsignedRoundingMode is INFINITY, return r2.
    if (unsigned_rounding_mode == UnsignedRoundingMode::Infinity)
        return r2;

    // 6. Let d1 be x – r1.
    auto d1 = x - r1;

    // 7. Let d2 be r2 – x.
    auto d2 = r2 - x;

    // 8. If d1 < d2, return r1.
    if (d1 < d2)
        return r1;

    // 9. If d2 < d1, return r2.
    if (d2 < d1)
        return r2;

    // 10. Assert: d1 is equal to d2.
    VERIFY(d1 == d2);

    // 11. If unsignedRoundingMode is HALF-ZERO, return r1.
    if (unsigned_rounding_mode == UnsignedRoundingMode::HalfZero)
        return r1;

    // 12. If unsignedRoundingMode is HALF-INFINITY, return r2.
    if (unsigned_rounding_mode == UnsignedRoundingMode::HalfInfinity)
        return r2;

    // 13. Assert: unsignedRoundingMode is HALF-EVEN.
    VERIFY(unsigned_rounding_mode == UnsignedRoundingMode::HalfEven);

    // 14. Let cardinality be (r1 / (r2 – r1)) modulo 2.
    auto cardinality = modulo((r1 / (r2 - r1)), 2);

    // 15. If cardinality = 0, return r1.
    if (cardinality == 0)
        return r1;

    // 16. Return r2.
    return r2;
}

// 13.27 ApplyUnsignedRoundingMode ( x, r1, r2, unsignedRoundingMode ), https://tc39.es/proposal-temporal/#sec-applyunsignedroundingmode
Crypto::SignedBigInteger apply_unsigned_rounding_mode(Crypto::SignedDivisionResult const& x, Crypto::SignedBigInteger r1, Crypto::SignedBigInteger r2, UnsignedRoundingMode unsigned_rounding_mode, Crypto::UnsignedBigInteger const& increment)
{
    // 1. If x = r1, return r1.
    if (x.quotient == r1 && x.remainder.unsigned_value().is_zero())
        return r1;

    // 2. Assert: r1 < x < r2.
    // NOTE: Skipped for the sake of performance.

    // 3. Assert: unsignedRoundingMode is not undefined.

    // 4. If unsignedRoundingMode is ZERO, return r1.
    if (unsigned_rounding_mode == UnsignedRoundingMode::Zero)
        return r1;

    // 5. If unsignedRoundingMode is INFINITY, return r2.
    if (unsigned_rounding_mode == UnsignedRoundingMode::Infinity)
        return r2;

    // 6. Let d1 be x – r1.
    auto d1 = x.remainder.unsigned_value();

    // 7. Let d2 be r2 – x.
    auto d2 = increment.minus(x.remainder.unsigned_value());

    // 8. If d1 < d2, return r1.
    if (d1 < d2)
        return r1;

    // 9. If d2 < d1, return r2.
    if (d2 < d1)
        return r2;

    // 10. Assert: d1 is equal to d2.
    // NOTE: Skipped for the sake of performance.

    // 11. If unsignedRoundingMode is HALF-ZERO, return r1.
    if (unsigned_rounding_mode == UnsignedRoundingMode::HalfZero)
        return r1;

    // 12. If unsignedRoundingMode is HALF-INFINITY, return r2.
    if (unsigned_rounding_mode == UnsignedRoundingMode::HalfInfinity)
        return r2;

    // 13. Assert: unsignedRoundingMode is HALF-EVEN.
    VERIFY(unsigned_rounding_mode == UnsignedRoundingMode::HalfEven);

    // 14. Let cardinality be (r1 / (r2 – r1)) modulo 2.
    auto cardinality = modulo(r1.divided_by(r2.minus(r1)).quotient, "2"_bigint);

    // 15. If cardinality = 0, return r1.
    if (cardinality.unsigned_value().is_zero())
        return r1;

    // 16. Return r2.
    return r2;
}

// 13.28 RoundNumberToIncrement ( x, increment, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundnumbertoincrement
double round_number_to_increment(double x, u64 increment, RoundingMode rounding_mode)
{
    // 1. Let quotient be x / increment.
    auto quotient = x / static_cast<double>(increment);

    Sign is_negative;

    // 2. If quotient < 0, then
    if (quotient < 0) {
        // a. Let isNegative be NEGATIVE.
        is_negative = Sign::Negative;

        // b. Set quotient to -quotient.
        quotient = -quotient;
    }
    // 3. Else,
    else {
        // a. Let isNegative be POSITIVE.
        is_negative = Sign::Positive;
    }

    // 4. Let unsignedRoundingMode be GetUnsignedRoundingMode(roundingMode, isNegative).
    auto unsigned_rounding_mode = get_unsigned_rounding_mode(rounding_mode, is_negative);

    // 5. Let r1 be the largest integer such that r1 ≤ quotient.
    auto r1 = floor(quotient);

    // 6. Let r2 be the smallest integer such that r2 > quotient.
    auto r2 = ceil(quotient);
    if (quotient == r2)
        r2++;

    // 7. Let rounded be ApplyUnsignedRoundingMode(quotient, r1, r2, unsignedRoundingMode).
    auto rounded = apply_unsigned_rounding_mode(quotient, r1, r2, unsigned_rounding_mode);

    // 8. If isNegative is NEGATIVE, set rounded to -rounded.
    if (is_negative == Sign::Negative)
        rounded = -rounded;

    // 9. Return rounded × increment.
    return rounded * static_cast<double>(increment);
}

// 13.28 RoundNumberToIncrement ( x, increment, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundnumbertoincrement
Crypto::SignedBigInteger round_number_to_increment(Crypto::SignedBigInteger const& x, Crypto::UnsignedBigInteger const& increment, RoundingMode rounding_mode)
{
    // OPTIMIZATION: If the increment is 1 the number is always rounded.
    if (increment == 1)
        return x;

    // 1. Let quotient be x / increment.
    auto division_result = x.divided_by(increment);

    // OPTIMIZATION: If there's no remainder the number is already rounded.
    if (division_result.remainder.unsigned_value().is_zero())
        return x;

    Sign is_negative;

    // 2. If quotient < 0, then
    if (division_result.quotient.is_negative() || division_result.remainder.is_negative()) {
        // a. Let isNegative be NEGATIVE.
        is_negative = Sign::Negative;

        // b. Set quotient to -quotient.
        division_result.quotient.negate();
        division_result.remainder.negate();
    }
    // 3. Else,
    else {
        // a. Let isNegative be POSITIVE.
        is_negative = Sign::Positive;
    }

    // 4. Let unsignedRoundingMode be GetUnsignedRoundingMode(roundingMode, isNegative).
    auto unsigned_rounding_mode = get_unsigned_rounding_mode(rounding_mode, is_negative);

    // 5. Let r1 be the largest integer such that r1 ≤ quotient.
    auto r1 = division_result.quotient;

    // 6. Let r2 be the smallest integer such that r2 > quotient.
    auto r2 = division_result.quotient.plus(1_bigint);

    // 7. Let rounded be ApplyUnsignedRoundingMode(quotient, r1, r2, unsignedRoundingMode).
    auto rounded = apply_unsigned_rounding_mode(division_result, move(r1), move(r2), unsigned_rounding_mode, increment);

    // 8. If isNegative is NEGATIVE, set rounded to -rounded.
    if (is_negative == Sign::Negative)
        rounded.negate();

    // 9. Return rounded × increment.
    return rounded.multiplied_by(increment);
}

// 13.29 RoundNumberToIncrementAsIfPositive ( x, increment, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundnumbertoincrementasifpositive
Crypto::SignedBigInteger round_number_to_increment_as_if_positive(Crypto::SignedBigInteger const& x, Crypto::UnsignedBigInteger const& increment, RoundingMode rounding_mode)
{
    // OPTIMIZATION: If the increment is 1 the number is always rounded.
    if (increment == 1)
        return x;

    // 1. Let quotient be x / increment.
    auto division_result = x.divided_by(increment);

    // OPTIMIZATION: If there's no remainder the number is already rounded.
    if (division_result.remainder.unsigned_value().is_zero())
        return x;

    // 2. Let unsignedRoundingMode be GetUnsignedRoundingMode(roundingMode, POSITIVE).
    auto unsigned_rounding_mode = get_unsigned_rounding_mode(rounding_mode, Sign::Positive);

    // 3. Let r1 be the largest integer such that r1 ≤ quotient.
    // 4. Let r2 be the smallest integer such that r2 > quotient.
    Crypto::SignedBigInteger r1;
    Crypto::SignedBigInteger r2;

    if (x.is_negative()) {
        r1 = division_result.quotient.minus("1"_sbigint);
        r2 = division_result.quotient;
    } else {
        r1 = division_result.quotient;
        r2 = division_result.quotient.plus("1"_sbigint);
    }

    // 5. Let rounded be ApplyUnsignedRoundingMode(quotient, r1, r2, unsignedRoundingMode).
    auto rounded = apply_unsigned_rounding_mode(division_result, move(r1), move(r2), unsigned_rounding_mode, increment);

    // 6. Return rounded × increment.
    return rounded.multiplied_by(increment);
}

// 13.33 ParseISODateTime ( isoString, allowedFormats ), https://tc39.es/proposal-temporal/#sec-temporal-parseisodatetime
ThrowCompletionOr<ParsedISODateTime> parse_iso_date_time(VM& vm, StringView iso_string, ReadonlySpan<Production> allowed_formats)
{
    // 1. Let parseResult be EMPTY.
    Optional<ParseResult> parse_result;

    // 2. Let calendar be EMPTY.
    Optional<String> calendar;

    // 3. Let yearAbsent be false.
    auto year_absent = false;

    // 4. For each nonterminal goal of allowedFormats, do
    for (auto goal : allowed_formats) {
        // a. If parseResult is not a Parse Node, then
        if (parse_result.has_value())
            break;

        // i. Set parseResult to ParseText(StringToCodePoints(isoString), goal).
        parse_result = parse_iso8601(goal, iso_string);

        // ii. If parseResult is a Parse Node, then
        if (parse_result.has_value()) {
            // 1. Let calendarWasCritical be false.
            auto calendar_was_critical = false;

            // 2. For each Annotation Parse Node annotation contained within parseResult, do
            for (auto const& annotation : parse_result->annotations) {
                // a. Let key be the source text matched by the AnnotationKey Parse Node contained within annotation.
                auto const& key = annotation.key;

                // b. Let value be the source text matched by the AnnotationValue Parse Node contained within annotation.
                auto const& value = annotation.value;

                // c. If CodePointsToString(key) is "u-ca", then
                if (key == "u-ca"sv) {
                    // i. If calendar is EMPTY, then
                    if (!calendar.has_value()) {
                        // i. Set calendar to CodePointsToString(value).
                        calendar = String::from_utf8_without_validation(value.bytes());

                        // ii. If annotation contains an AnnotationCriticalFlag Parse Node, set calendarWasCritical to true.
                        if (annotation.critical)
                            calendar_was_critical = true;
                    }
                    // ii. Else,
                    else {
                        // i. If annotation contains an AnnotationCriticalFlag Parse Node, or calendarWasCritical is true,
                        //    throw a RangeError exception.
                        if (annotation.critical || calendar_was_critical)
                            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCriticalAnnotation, key, value);
                    }
                }
                // d. Else,
                else {
                    // i. If annotation contains an AnnotationCriticalFlag Parse Node, throw a RangeError exception.
                    if (annotation.critical)
                        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCriticalAnnotation, key, value);
                }
            }

            // 3. If goal is TemporalMonthDayString or TemporalYearMonthString, calendar is not EMPTY, and the
            //    ASCII-lowercase of calendar is not "iso8601", throw a RangeError exception.
            if (goal == Production::TemporalMonthDayString || goal == Production::TemporalYearMonthString) {
                if (calendar.has_value() && !calendar->equals_ignoring_ascii_case("iso8601"sv))
                    return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarIdentifier, *calendar);
            }

            // 4. If goal is TemporalMonthDayString and parseResult does not contain a DateYear Parse Node, set
            //    yearAbsent to true.
            if (goal == Production::TemporalMonthDayString && !parse_result->date_year.has_value())
                year_absent = true;
        }
    }

    // 5. If parseResult is not a Parse Node, throw a RangeError exception.
    if (!parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODateTime);

    // 6. NOTE: Applications of StringToNumber below do not lose precision, since each of the parsed values is guaranteed
    //    to be a sufficiently short string of decimal digits.

    // 7. Let each of year, month, day, hour, minute, second, and fSeconds be the source text matched by the respective
    //    DateYear, DateMonth, DateDay, the first Hour, the first MinuteSecond, TimeSecond, and the first
    //    TemporalDecimalFraction Parse Node contained within parseResult, or an empty sequence of code points if not present.
    auto year = parse_result->date_year.value_or({});
    auto month = parse_result->date_month.value_or({});
    auto day = parse_result->date_day.value_or({});
    auto hour = parse_result->time_hour.value_or({});
    auto minute = parse_result->time_minute.value_or({});
    auto second = parse_result->time_second.value_or({});
    auto fractional_seconds = parse_result->time_fraction.value_or({});

    // 8. Let yearMV be ℝ(StringToNumber(CodePointsToString(year))).
    auto year_value = string_to_number(year);

    // 9. If month is empty, then
    //        a. Let monthMV be 1.
    // 10. Else,
    //         a. Let monthMV be ℝ(StringToNumber(CodePointsToString(month))).
    auto month_value = month.is_empty() ? 1 : string_to_number(month);

    // 11. If day is empty, then
    //         a. Let dayMV be 1.
    // 12. Else,
    //         a. Let dayMV be ℝ(StringToNumber(CodePointsToString(day))).
    auto day_value = day.is_empty() ? 1 : string_to_number(day);

    // 13. If hour is empty, then
    //         a. Let hourMV be 0.
    // 14. Else,
    //         a. Let hourMV be ℝ(StringToNumber(CodePointsToString(hour))).
    auto hour_value = hour.is_empty() ? 0 : string_to_number(hour);

    // 15. If minute is empty, then
    //         a. Let minuteMV be 0.
    // 16. Else,
    //         a. Let minuteMV be ℝ(StringToNumber(CodePointsToString(minute))).
    auto minute_value = minute.is_empty() ? 0 : string_to_number(minute);

    // 17. If second is empty, then
    //         a. Let secondMV be 0.
    // 18. Else,
    //         a. Let secondMV be ℝ(StringToNumber(CodePointsToString(second))).
    //         b. If secondMV = 60, then
    //                i. Set secondMV to 59.
    auto second_value = second.is_empty() ? 0 : min(string_to_number(second), 59.0);

    double millisecond_value = 0;
    double microsecond_value = 0;
    double nanosecond_value = 0;

    // 19. If fSeconds is not empty, then
    if (!fractional_seconds.is_empty()) {
        // a. Let fSecondsDigits be the substring of CodePointsToString(fSeconds) from 1.
        auto fractional_seconds_digits = fractional_seconds.substring_view(1);

        // b. Let fSecondsDigitsExtended be the string-concatenation of fSecondsDigits and "000000000".
        auto fractional_seconds_extended = MUST(String::formatted("{}000000000", fractional_seconds_digits));

        // c. Let millisecond be the substring of fSecondsDigitsExtended from 0 to 3.
        auto millisecond = fractional_seconds_extended.bytes_as_string_view().substring_view(0, 3);

        // d. Let microsecond be the substring of fSecondsDigitsExtended from 3 to 6.
        auto microsecond = fractional_seconds_extended.bytes_as_string_view().substring_view(3, 3);

        // e. Let nanosecond be the substring of fSecondsDigitsExtended from 6 to 9.
        auto nanosecond = fractional_seconds_extended.bytes_as_string_view().substring_view(6, 3);

        // f. Let millisecondMV be ℝ(StringToNumber(millisecond)).
        millisecond_value = string_to_number(millisecond);

        // g. Let microsecondMV be ℝ(StringToNumber(microsecond)).
        microsecond_value = string_to_number(microsecond);

        // h. Let nanosecondMV be ℝ(StringToNumber(nanosecond)).
        nanosecond_value = string_to_number(nanosecond);
    }
    // 20. Else,
    else {
        // a. Let millisecondMV be 0.
        // b. Let microsecondMV be 0.
        // c. Let nanosecondMV be 0.
    }

    // 21. Assert: IsValidISODate(yearMV, monthMV, dayMV) is true.
    VERIFY(is_valid_iso_date(year_value, month_value, day_value));

    Variant<ParsedISODateTime::StartOfDay, Time> time { ParsedISODateTime::StartOfDay {} };

    // 22. If hour is empty, then
    if (hour.is_empty()) {
        // a. Let time be START-OF-DAY.
    }
    // 23. Else,
    else {
        // a. Let time be CreateTimeRecord(hourMV, minuteMV, secondMV, millisecondMV, microsecondMV, nanosecondMV).
        time = create_time_record(hour_value, minute_value, second_value, millisecond_value, microsecond_value, nanosecond_value);
    }

    // 24. Let timeZoneResult be ISO String Time Zone Parse Record { [[Z]]: false, [[OffsetString]]: EMPTY, [[TimeZoneAnnotation]]: EMPTY }.
    ParsedISOTimeZone time_zone_result;

    // 25. If parseResult contains a TimeZoneIdentifier Parse Node, then
    if (parse_result->time_zone_identifier.has_value()) {
        // a. Let identifier be the source text matched by the TimeZoneIdentifier Parse Node contained within parseResult.
        // b. Set timeZoneResult.[[TimeZoneAnnotation]] to CodePointsToString(identifier).
        time_zone_result.time_zone_annotation = String::from_utf8_without_validation(parse_result->time_zone_identifier->bytes());
    }

    // 26. If parseResult contains a UTCDesignator Parse Node, then
    if (parse_result->utc_designator.has_value()) {
        // a. Set timeZoneResult.[[Z]] to true.
        time_zone_result.z_designator = true;
    }
    // 27. Else if parseResult contains a UTCOffset[+SubMinutePrecision] Parse Node, then
    else if (parse_result->date_time_offset.has_value()) {
        // a. Let offset be the source text matched by the UTCOffset[+SubMinutePrecision] Parse Node contained within parseResult.
        // b. Set timeZoneResult.[[OffsetString]] to CodePointsToString(offset).
        time_zone_result.offset_string = String::from_utf8_without_validation(parse_result->date_time_offset->source_text.bytes());
    }

    // 28. If yearAbsent is true, let yearReturn be EMPTY; else let yearReturn be yearMV.
    Optional<i32> year_return;
    if (!year_absent)
        year_return = static_cast<i32>(year_value);

    // 29. Return ISO Date-Time Parse Record { [[Year]]: yearReturn, [[Month]]: monthMV, [[Day]]: dayMV, [[Time]]: time, [[TimeZone]]: timeZoneResult, [[Calendar]]: calendar  }.
    return ParsedISODateTime { .year = year_return, .month = static_cast<u8>(month_value), .day = static_cast<u8>(day_value), .time = move(time), .time_zone = move(time_zone_result), .calendar = move(calendar) };
}

// 13.34 ParseTemporalCalendarString ( string ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporalcalendarstring
ThrowCompletionOr<String> parse_temporal_calendar_string(VM& vm, String const& string)
{
    // 1. Let parseResult be Completion(ParseISODateTime(string, « TemporalDateTimeString[+Zoned], TemporalDateTimeString[~Zoned],
    //    TemporalInstantString, TemporalTimeString, TemporalMonthDayString, TemporalYearMonthString »)).
    static constexpr auto productions = to_array<Production>({
        Production::TemporalZonedDateTimeString,
        Production::TemporalDateTimeString,
        Production::TemporalInstantString,
        Production::TemporalTimeString,
        Production::TemporalMonthDayString,
        Production::TemporalYearMonthString,
    });

    auto parse_result = parse_iso_date_time(vm, string, productions);

    // 2. If parseResult is a normal completion, then
    if (!parse_result.is_error()) {
        // a. Let calendar be parseResult.[[Value]].[[Calendar]].
        auto calendar = parse_result.value().calendar;

        // b. If calendar is empty, return "iso8601".
        // c. Else, return calendar.
        return calendar.value_or("iso8601"_string);
    }

    // 3. Set parseResult to ParseText(StringToCodePoints(string), AnnotationValue).
    auto annotation_parse_result = parse_iso8601(Production::AnnotationValue, string);

    // 4. If parseResult is a List of errors, throw a RangeError exception.
    if (!annotation_parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarString, string);

    // 5. Return string.
    return string;
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

// 13.36 ParseTemporalTimeZoneString ( timeZoneString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporaltimezonestring
ThrowCompletionOr<TimeZone> parse_temporal_time_zone_string(VM& vm, StringView time_zone_string)
{
    // 1. Let parseResult be ParseText(StringToCodePoints(timeZoneString), TimeZoneIdentifier).
    auto parse_result = parse_iso8601(Production::TimeZoneIdentifier, time_zone_string);

    // 2. If parseResult is a Parse Node, then
    if (parse_result.has_value()) {
        // a. Return ! ParseTimeZoneIdentifier(timeZoneString).
        return parse_time_zone_identifier(parse_result.release_value());
    }

    // 3. Let result be ? ParseISODateTime(timeZoneString, « TemporalDateTimeString[+Zoned], TemporalDateTimeString[~Zoned],
    //    TemporalInstantString, TemporalTimeString, TemporalMonthDayString, TemporalYearMonthString »).
    static constexpr auto productions = to_array<Production>({
        Production::TemporalZonedDateTimeString,
        Production::TemporalDateTimeString,
        Production::TemporalInstantString,
        Production::TemporalTimeString,
        Production::TemporalMonthDayString,
        Production::TemporalYearMonthString,
    });

    auto result = TRY(parse_iso_date_time(vm, time_zone_string, productions));

    // 4. Let timeZoneResult be result.[[TimeZone]].
    auto time_zone_result = move(result.time_zone);

    // 5. If timeZoneResult.[[TimeZoneAnnotation]] is not empty, then
    if (time_zone_result.time_zone_annotation.has_value()) {
        // a. Return ! ParseTimeZoneIdentifier(timeZoneResult.[[TimeZoneAnnotation]]).
        return MUST(parse_time_zone_identifier(vm, *time_zone_result.time_zone_annotation));
    }

    // 6. If timeZoneResult.[[Z]] is true, then
    if (time_zone_result.z_designator) {
        // a. Return ! ParseTimeZoneIdentifier("UTC").
        return MUST(parse_time_zone_identifier(vm, "UTC"sv));
    }

    // 7. If timeZoneResult.[[OffsetString]] is not empty, then
    if (time_zone_result.offset_string.has_value()) {
        // a. Return ? ParseTimeZoneIdentifier(timeZoneResult.[[OffsetString]]).
        return TRY(parse_time_zone_identifier(vm, *time_zone_result.offset_string));
    }

    // 8. Throw a RangeError exception.
    return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeZoneString, time_zone_string);
}

// 13.40 ToMonthCode ( argument ), https://tc39.es/proposal-temporal/#sec-temporal-tomonthcode
ThrowCompletionOr<String> to_month_code(VM& vm, Value argument)
{
    // 1. Let monthCode be ? ToPrimitive(argument, STRING).
    auto month_code = TRY(argument.to_primitive(vm, Value::PreferredType::String));

    // 2. If monthCode is not a String, throw a TypeError exception.
    if (!month_code.is_string())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidMonthCode);
    auto month_code_string = month_code.as_string().utf8_string_view();

    // 3. If the length of monthCode is not 3 or 4, throw a RangeError exception.
    if (month_code_string.length() != 3 && month_code_string.length() != 4)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthCode);

    // 4. If the first code unit of monthCode is not 0x004D (LATIN CAPITAL LETTER M), throw a RangeError exception.
    if (month_code_string[0] != 'M')
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthCode);

    // 5. If the second code unit of monthCode is not in the inclusive interval from 0x0030 (DIGIT ZERO) to 0x0039 (DIGIT NINE),
    //    throw a RangeError exception.
    if (!is_ascii_digit(month_code_string[1]) || parse_ascii_digit(month_code_string[1]) > 9)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthCode);

    // 6. If the third code unit of monthCode is not in the inclusive interval from 0x0030 (DIGIT ZERO) to 0x0039 (DIGIT NINE),
    //    throw a RangeError exception.
    if (!is_ascii_digit(month_code_string[2]) || parse_ascii_digit(month_code_string[2]) > 9)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthCode);

    // 7. If the length of monthCode is 4 and the fourth code unit of monthCode is not 0x004C (LATIN CAPITAL LETTER L),
    //    throw a RangeError exception.
    if (month_code_string.length() == 4 && month_code_string[3] != 'L')
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthCode);

    // 8. Let monthCodeDigits be the substring of monthCode from 1 to 3.
    auto month_code_digits = month_code_string.substring_view(1, 2);

    // 9. Let monthCodeInteger be ℝ(StringToNumber(monthCodeDigits)).
    auto month_code_integer = month_code_digits.to_number<u8>().value();

    // 10. If monthCodeInteger is 0 and the length of monthCode is not 4, throw a RangeError exception.
    if (month_code_integer == 0 && month_code_string.length() != 4)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthCode);

    // 11. Return monthCode.
    return month_code.as_string().utf8_string();
}

// 13.41 ToOffsetString ( argument ), https://tc39.es/proposal-temporal/#sec-temporal-tooffsetstring
ThrowCompletionOr<String> to_offset_string(VM& vm, Value argument)
{
    // 1. Let offset be ? ToPrimitive(argument, STRING).
    auto offset = TRY(argument.to_primitive(vm, Value::PreferredType::String));

    // 2. If offset is not a String, throw a TypeError exception.
    if (!offset.is_string())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidTimeZoneString, offset);

    // 3. Perform ? ParseDateTimeUTCOffset(offset).
    TRY(parse_date_time_utc_offset(vm, offset.as_string().utf8_string_view()));

    // 4. Return offset.
    return offset.as_string().utf8_string();
}

// 13.42 ISODateToFields ( calendar, isoDate, type ), https://tc39.es/proposal-temporal/#sec-temporal-isodatetofields
CalendarFields iso_date_to_fields(StringView calendar, ISODate iso_date, DateType type)
{
    // 1. Let fields be an empty Calendar Fields Record with all fields set to unset.
    auto fields = CalendarFields::unset();

    // 2. Let calendarDate be CalendarISOToDate(calendar, isoDate).
    auto calendar_date = calendar_iso_to_date(calendar, iso_date);

    // 3. Set fields.[[MonthCode]] to calendarDate.[[MonthCode]].
    fields.month_code = calendar_date.month_code;

    // 4. If type is MONTH-DAY or DATE, then
    if (type == DateType::MonthDay || type == DateType::Date) {
        // a. Set fields.[[Day]] to calendarDate.[[Day]].
        fields.day = calendar_date.day;
    }

    // 5. If type is YEAR-MONTH or DATE, then
    if (type == DateType::YearMonth || type == DateType::Date) {
        // a. Set fields.[[Year]] to calendarDate.[[Year]].
        fields.year = calendar_date.year;
    }

    // 6. Return fields.
    return fields;
}

// 13.43 GetDifferenceSettings ( operation, options, unitGroup, disallowedUnits, fallbackSmallestUnit, smallestLargestDefaultUnit ), https://tc39.es/proposal-temporal/#sec-temporal-getdifferencesettings
ThrowCompletionOr<DifferenceSettings> get_difference_settings(VM& vm, DurationOperation operation, Object const& options, UnitGroup unit_group, ReadonlySpan<Unit> disallowed_units, Unit fallback_smallest_unit, Unit smallest_largest_default_unit)
{
    // 1. NOTE: The following steps read options and perform independent validation in alphabetical order.

    // 2. Let largestUnit be ? GetTemporalUnitValuedOption(options, "largestUnit", unitGroup, AUTO).
    auto largest_unit = TRY(get_temporal_unit_valued_option(vm, options, vm.names.largestUnit, unit_group, Auto {}));

    // 3. If disallowedUnits contains largestUnit, throw a RangeError exception.
    if (auto* unit = largest_unit.get_pointer<Unit>(); unit && disallowed_units.contains_slow(*unit))
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, temporal_unit_to_string(*unit), vm.names.largestUnit);

    // 4. Let roundingIncrement be ? GetRoundingIncrementOption(options).
    auto rounding_increment = TRY(get_rounding_increment_option(vm, options));

    // 5. Let roundingMode be ? GetRoundingModeOption(options, TRUNC).
    auto rounding_mode = TRY(get_rounding_mode_option(vm, options, RoundingMode::Trunc));

    // 6. If operation is SINCE, then
    if (operation == DurationOperation::Since) {
        // a. Set roundingMode to NegateRoundingMode(roundingMode).
        rounding_mode = negate_rounding_mode(rounding_mode);
    }

    // 7. Let smallestUnit be ? GetTemporalUnitValuedOption(options, "smallestUnit", unitGroup, fallbackSmallestUnit).
    auto smallest_unit = TRY(get_temporal_unit_valued_option(vm, options, vm.names.smallestUnit, unit_group, fallback_smallest_unit));
    auto smallest_unit_value = smallest_unit.get<Unit>();

    // 8. If disallowedUnits contains smallestUnit, throw a RangeError exception.
    if (disallowed_units.contains_slow(smallest_unit_value))
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, temporal_unit_to_string(smallest_unit_value), vm.names.smallestUnit);

    // 9. Let defaultLargestUnit be LargerOfTwoTemporalUnits(smallestLargestDefaultUnit, smallestUnit).
    auto default_largest_unit = larger_of_two_temporal_units(smallest_largest_default_unit, smallest_unit.get<Unit>());

    // 10. If largestUnit is AUTO, set largestUnit to defaultLargestUnit.
    if (largest_unit.has<Auto>())
        largest_unit = default_largest_unit;
    auto largest_unit_value = largest_unit.get<Unit>();

    // 11. If LargerOfTwoTemporalUnits(largestUnit, smallestUnit) is not largestUnit, throw a RangeError exception.
    if (larger_of_two_temporal_units(largest_unit_value, smallest_unit_value) != largest_unit_value)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidUnitRange, temporal_unit_to_string(smallest_unit_value), temporal_unit_to_string(largest_unit_value));

    // 12. Let maximum be MaximumTemporalDurationRoundingIncrement(smallestUnit).
    auto maximum = maximum_temporal_duration_rounding_increment(smallest_unit_value);

    // 13. If maximum is not UNSET, perform ? ValidateTemporalRoundingIncrement(roundingIncrement, maximum, false).
    if (!maximum.has<Unset>())
        TRY(validate_temporal_rounding_increment(vm, rounding_increment, maximum.get<u64>(), false));

    // 14. Return the Record { [[SmallestUnit]]: smallestUnit, [[LargestUnit]]: largestUnit, [[RoundingMode]]: roundingMode, [[RoundingIncrement]]: roundingIncrement,  }.
    return DifferenceSettings { .smallest_unit = smallest_unit_value, .largest_unit = largest_unit_value, .rounding_mode = rounding_mode, .rounding_increment = rounding_increment };
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
        if (default_.has<Required>())
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, "undefined"sv, property.as_string());

        // b. Return default.
        return default_.visit(
            [](Required) -> Value { VERIFY_NOT_REACHED(); },
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

// 14.4.1.3 GetRoundingModeOption ( options, fallback ), https://tc39.es/proposal-temporal/#sec-temporal-getroundingmodeoption
ThrowCompletionOr<RoundingMode> get_rounding_mode_option(VM& vm, Object const& options, RoundingMode fallback)
{
    // 1. Let allowedStrings be the List of Strings from the "String Identifier" column of Table 26.
    static constexpr auto allowed_strings = to_array({ "ceil"sv, "floor"sv, "expand"sv, "trunc"sv, "halfCeil"sv, "halfFloor"sv, "halfExpand"sv, "halfTrunc"sv, "halfEven"sv });

    // 2. Let stringFallback be the value from the "String Identifier" column of the row with fallback in its "Rounding Mode" column.
    auto string_fallback = allowed_strings[to_underlying(fallback)];

    // 3. Let stringValue be ? GetOption(options, "roundingMode", STRING, allowedStrings, stringFallback).
    auto string_value = TRY(get_option(vm, options, vm.names.roundingMode, OptionType::String, allowed_strings, string_fallback));

    // 4. Return the value from the "Rounding Mode" column of the row with stringValue in its "String Identifier" column.
    return static_cast<RoundingMode>(allowed_strings.first_index_of(string_value.as_string().utf8_string_view()).value());
}

// 14.4.1.4 GetRoundingIncrementOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-getroundingincrementoption
ThrowCompletionOr<u64> get_rounding_increment_option(VM& vm, Object const& options)
{
    // 1. Let value be ? Get(options, "roundingIncrement").
    auto value = TRY(options.get(vm.names.roundingIncrement));

    // 2. If value is undefined, return 1𝔽.
    if (value.is_undefined())
        return 1;

    // 3. Let integerIncrement be ? ToIntegerWithTruncation(value).
    auto integer_increment = TRY(to_integer_with_truncation(vm, value, ErrorType::OptionIsNotValidValue, value, "roundingIncrement"sv));

    // 4. If integerIncrement < 1 or integerIncrement > 10**9, throw a RangeError exception.
    if (integer_increment < 1 || integer_increment > 1'000'000'000u)
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, value, "roundingIncrement");

    // 5. Return integerIncrement.
    return static_cast<u64>(integer_increment);
}

// 14.5.1 GetUTCEpochNanoseconds ( isoDateTime ), https://tc39.es/proposal-temporal/#sec-getutcepochnanoseconds
Crypto::SignedBigInteger get_utc_epoch_nanoseconds(ISODateTime const& iso_date_time)
{
    return JS::get_utc_epoch_nanoseconds(
        iso_date_time.iso_date.year,
        iso_date_time.iso_date.month,
        iso_date_time.iso_date.day,
        iso_date_time.time.hour,
        iso_date_time.time.minute,
        iso_date_time.time.second,
        iso_date_time.time.millisecond,
        iso_date_time.time.microsecond,
        iso_date_time.time.nanosecond);
}

// AD-HOC
// FIXME: We should add a generic floor() method to our BigInt classes. But for now, since we know we are only dividing
//        by powers of 10, we can implement a very situationally specific method to compute the floor of a division.
Crypto::SignedBigInteger big_floor(Crypto::SignedBigInteger const& numerator, Crypto::UnsignedBigInteger const& denominator)
{
    auto result = numerator.divided_by(denominator);

    if (result.remainder.is_zero())
        return result.quotient;
    if (!result.quotient.is_negative() && result.remainder.is_positive())
        return result.quotient;

    return result.quotient.minus(Crypto::SignedBigInteger { 1 });
}

}
