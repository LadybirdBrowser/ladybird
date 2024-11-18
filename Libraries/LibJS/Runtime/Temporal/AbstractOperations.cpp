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
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/ISO8601.h>
#include <LibJS/Runtime/Temporal/Instant.h>

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

    // FIXME: Implement the remaining steps of this AO when we have implemented PlainRelativeTo and ZonedRelativeTo.
    return RelativeTo { .plain_relative_to = {}, .zoned_relative_to = {} };
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
Crypto::SignedBigInteger apply_unsigned_rounding_mode(Crypto::SignedDivisionResult const& x, Crypto::SignedBigInteger const& r1, Crypto::SignedBigInteger const& r2, UnsignedRoundingMode unsigned_rounding_mode, Crypto::UnsignedBigInteger const& increment)
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
    if (division_result.quotient.is_negative()) {
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
    auto rounded = apply_unsigned_rounding_mode(division_result, r1, r2, unsigned_rounding_mode, increment);

    // 8. If isNegative is NEGATIVE, set rounded to -rounded.
    if (is_negative == Sign::Negative)
        rounded.negate();

    // 9. Return rounded × increment.
    return rounded.multiplied_by(increment);
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

}
