/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/String.h>
#include <AK/TypeCasts.h>
#include <AK/Variant.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/ISO8601.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS::Temporal {

static Optional<OptionType> to_option_type(Value value)
{
    if (value.is_boolean())
        return OptionType::Boolean;
    if (value.is_string())
        return OptionType::String;
    if (value.is_number())
        return OptionType::Number;
    return {};
}

// 13.1 IterableToListOfType ( items, elementTypes ), https://tc39.es/proposal-temporal/#sec-iterabletolistoftype
ThrowCompletionOr<MarkedVector<Value>> iterable_to_list_of_type(VM& vm, Value items, Vector<OptionType> const& element_types)
{
    // 1. Let iteratorRecord be ? GetIterator(items, sync).
    auto iterator_record = TRY(get_iterator(vm, items, IteratorHint::Sync));

    // 2. Let values be a new empty List.
    MarkedVector<Value> values(vm.heap());

    // 3. Let next be true.
    auto next = true;
    // 4. Repeat, while next is not false,
    while (next) {
        // a. Set next to ? IteratorStep(iteratorRecord).
        auto iterator_result = TRY(iterator_step(vm, iterator_record));
        next = iterator_result;

        // b. If next is not false, then
        if (next) {
            // i. Let nextValue be ? IteratorValue(next).
            auto next_value = TRY(iterator_value(vm, *iterator_result));
            // ii. If Type(nextValue) is not an element of elementTypes, then
            if (auto type = to_option_type(next_value); !type.has_value() || !element_types.contains_slow(*type)) {
                // 1. Let completion be ThrowCompletion(a newly created TypeError object).
                auto completion = vm.throw_completion<TypeError>(ErrorType::IterableToListOfTypeInvalidValue, next_value.to_string_without_side_effects());
                // 2. Return ? IteratorClose(iteratorRecord, completion).
                return iterator_close(vm, iterator_record, move(completion));
            }
            // iii. Append nextValue to the end of the List values.
            values.append(next_value);
        }
    }

    // 5. Return values.
    return { move(values) };
}

// 13.2 GetOptionsObject ( options ), https://tc39.es/proposal-temporal/#sec-getoptionsobject
ThrowCompletionOr<Object*> get_options_object(VM& vm, Value options)
{
    auto& realm = *vm.current_realm();

    // 1. If options is undefined, then
    if (options.is_undefined()) {
        // a. Return OrdinaryObjectCreate(null).
        return Object::create(realm, nullptr).ptr();
    }

    // 2. If Type(options) is Object, then
    if (options.is_object()) {
        // a. Return options.
        return &options.as_object();
    }

    // 3. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::NotAnObject, "Options");
}

// 13.3 GetOption ( options, property, type, values, fallback ), https://tc39.es/proposal-temporal/#sec-getoption
ThrowCompletionOr<Value> get_option(VM& vm, Object const& options, PropertyKey const& property, OptionType type, ReadonlySpan<StringView> values, OptionDefault const& default_)
{
    VERIFY(property.is_string());

    // 1. Let value be ? Get(options, property).
    auto value = TRY(options.get(property));

    // 2. If value is undefined, then
    if (value.is_undefined()) {
        // a. If default is required, throw a RangeError exception.
        if (default_.has<GetOptionRequired>())
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, "undefined"sv, property.as_string());

        // b. Return default.
        return default_.visit(
            [](GetOptionRequired) -> ThrowCompletionOr<Value> { VERIFY_NOT_REACHED(); },
            [](Empty) -> ThrowCompletionOr<Value> { return js_undefined(); },
            [](bool b) -> ThrowCompletionOr<Value> { return Value(b); },
            [](double d) -> ThrowCompletionOr<Value> { return Value(d); },
            [&vm](StringView s) -> ThrowCompletionOr<Value> { return PrimitiveString::create(vm, s); });
    }

    // 5. If type is "boolean", then
    if (type == OptionType::Boolean) {
        // a. Set value to ToBoolean(value).
        value = Value(value.to_boolean());
    }
    // 6. Else if type is "number", then
    else if (type == OptionType::Number) {
        // a. Set value to ? ToNumber(value).
        value = TRY(value.to_number(vm));

        // b. If value is NaN, throw a RangeError exception.
        if (value.is_nan())
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, vm.names.NaN.as_string(), property.as_string());
    }
    // 7. Else,
    else {
        // a. Assert: type is "string".
        VERIFY(type == OptionType::String);

        // b. Set value to ? ToString(value).
        value = TRY(value.to_primitive_string(vm));
    }

    // 8. If values is not undefined and values does not contain an element equal to value, throw a RangeError exception.
    if (!values.is_empty()) {
        // NOTE: Every location in the spec that invokes GetOption with type=boolean also has values=undefined.
        VERIFY(value.is_string());
        if (auto value_string = value.as_string().utf8_string(); !values.contains_slow(value_string))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, value_string, property.as_string());
    }

    // 9. Return value.
    return value;
}

// 13.4 ToTemporalOverflow ( options ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaloverflow
ThrowCompletionOr<String> to_temporal_overflow(VM& vm, Object const* options)
{
    // 1. If options is undefined, return "constrain".
    if (options == nullptr)
        return "constrain"_string;

    // 2. Return ? GetOption(options, "overflow", "string", « "constrain", "reject" », "constrain").
    auto option = TRY(get_option(vm, *options, vm.names.overflow, OptionType::String, { "constrain"sv, "reject"sv }, "constrain"sv));

    VERIFY(option.is_string());
    return option.as_string().utf8_string();
}

// 13.5 ToTemporalDisambiguation ( options ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaldisambiguation
ThrowCompletionOr<String> to_temporal_disambiguation(VM& vm, Object const* options)
{
    // 1. If options is undefined, return "compatible".
    if (options == nullptr)
        return "compatible"_string;

    // 2. Return ? GetOption(options, "disambiguation", "string", « "compatible", "earlier", "later", "reject" », "compatible").
    auto option = TRY(get_option(vm, *options, vm.names.disambiguation, OptionType::String, { "compatible"sv, "earlier"sv, "later"sv, "reject"sv }, "compatible"sv));

    VERIFY(option.is_string());
    return option.as_string().utf8_string();
}

// 13.6 ToTemporalRoundingMode ( normalizedOptions, fallback ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalroundingmode
ThrowCompletionOr<String> to_temporal_rounding_mode(VM& vm, Object const& normalized_options, StringView fallback)
{
    // 1. Return ? GetOption(normalizedOptions, "roundingMode", "string", « "ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc", "halfEven" », fallback).
    auto option = TRY(get_option(
        vm, normalized_options, vm.names.roundingMode, OptionType::String,
        {
            "ceil"sv,
            "floor"sv,
            "expand"sv,
            "trunc"sv,
            "halfCeil"sv,
            "halfFloor"sv,
            "halfExpand"sv,
            "halfTrunc"sv,
            "halfEven"sv,
        },
        fallback));

    VERIFY(option.is_string());
    return option.as_string().utf8_string();
}

// 13.7 NegateTemporalRoundingMode ( roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-negatetemporalroundingmode
StringView negate_temporal_rounding_mode(StringView rounding_mode)
{
    // 1. If roundingMode is "ceil", return "floor".
    if (rounding_mode == "ceil"sv)
        return "floor"sv;

    // 2. If roundingMode is "floor", return "ceil".
    if (rounding_mode == "floor"sv)
        return "ceil"sv;

    // 3. If roundingMode is "halfCeil", return "halfFloor".
    if (rounding_mode == "halfCeil"sv)
        return "halfFloor"sv;

    // 4. If roundingMode is "halfFloor", return "halfCeil".
    if (rounding_mode == "halfFloor"sv)
        return "halfCeil"sv;

    // 5. Return roundingMode.
    return rounding_mode;
}

// 13.8 ToTemporalOffset ( options, fallback ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaloffset
ThrowCompletionOr<String> to_temporal_offset(VM& vm, Object const* options, StringView fallback)
{
    // 1. If options is undefined, return fallback.
    if (options == nullptr)
        return TRY_OR_THROW_OOM(vm, String::from_utf8(fallback));

    // 2. Return ? GetOption(options, "offset", "string", « "prefer", "use", "ignore", "reject" », fallback).
    auto option = TRY(get_option(vm, *options, vm.names.offset, OptionType::String, { "prefer"sv, "use"sv, "ignore"sv, "reject"sv }, fallback));

    VERIFY(option.is_string());
    return option.as_string().utf8_string();
}

// 13.9 ToCalendarNameOption ( normalizedOptions ), https://tc39.es/proposal-temporal/#sec-temporal-tocalendarnameoption
ThrowCompletionOr<String> to_calendar_name_option(VM& vm, Object const& normalized_options)
{
    // 1. Return ? GetOption(normalizedOptions, "calendarName", "string", « "auto", "always", "never", "critical" », "auto").
    auto option = TRY(get_option(vm, normalized_options, vm.names.calendarName, OptionType::String, { "auto"sv, "always"sv, "never"sv, "critical"sv }, "auto"sv));

    VERIFY(option.is_string());
    return option.as_string().utf8_string();
}

// 13.10 ToTimeZoneNameOption ( normalizedOptions ), https://tc39.es/proposal-temporal/#sec-temporal-totimezonenameoption
ThrowCompletionOr<String> to_time_zone_name_option(VM& vm, Object const& normalized_options)
{
    // 1. Return ? GetOption(normalizedOptions, "timeZoneName", "string", « "auto", "never", "critical" », "auto").
    auto option = TRY(get_option(vm, normalized_options, vm.names.timeZoneName, OptionType::String, { "auto"sv, "never"sv, "critical"sv }, "auto"sv));

    VERIFY(option.is_string());
    return option.as_string().utf8_string();
}

// 13.11 ToShowOffsetOption ( normalizedOptions ), https://tc39.es/proposal-temporal/#sec-temporal-toshowoffsetoption
ThrowCompletionOr<String> to_show_offset_option(VM& vm, Object const& normalized_options)
{
    // 1. Return ? GetOption(normalizedOptions, "offset", "string", « "auto", "never" », "auto").
    auto option = TRY(get_option(vm, normalized_options, vm.names.offset, OptionType::String, { "auto"sv, "never"sv }, "auto"sv));

    VERIFY(option.is_string());
    return option.as_string().utf8_string();
}

// 13.12 ToTemporalRoundingIncrement ( normalizedOptions, dividend, inclusive ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalroundingincrement
ThrowCompletionOr<u64> to_temporal_rounding_increment(VM& vm, Object const& normalized_options)
{
    // 1. Let increment be ? GetOption(normalizedOptions, "roundingIncrement", "number", undefined, 1𝔽)
    auto increment_value = TRY(get_option(vm, normalized_options, vm.names.roundingIncrement, OptionType::Number, {}, 1.0));
    VERIFY(increment_value.is_number());
    auto increment = increment_value.as_double();

    // 2. If increment is not finite, throw a RangeError exception
    if (!increment_value.is_finite_number())
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, increment, "roundingIncrement");

    // 3. If increment < 1𝔽, throw a RangeError exception.
    if (increment < 1) {
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, increment, "roundingIncrement");
    }

    // 4. Return truncate(ℝ(increment))
    return static_cast<u64>(trunc(increment));
}

// 13.13 ValidateTemporalRoundingIncrement ( increment, dividend, inclusive ), https://tc39.es/proposal-temporal/#sec-validatetemporalroundingincrement
ThrowCompletionOr<void> validate_temporal_rounding_increment(VM& vm, u64 increment, u64 dividend, bool inclusive)
{
    u64 maximum;
    // 1. If inclusive is true, then
    if (inclusive) {
        // a. Let maximum be dividend.
        maximum = dividend;
    }
    // 2. Else if dividend is more than 1, then
    else if (dividend > 1) {
        // a. Let maximum be dividend - 1.
        maximum = dividend - 1;
    }
    // 3. Else
    else {
        // a. Let maximum be 1.
        maximum = 1;
    }
    // 4. If increment > maximum, throw a RangeError exception.
    if (increment > maximum) {
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, increment, "roundingIncrement");
    }

    // 5. If dividend modulo increment is not zero, then
    if (modulo(dividend, increment) != 0) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, increment, "roundingIncrement");
    }

    // 6. Return UNUSED.
    return {};
}

// 13.14 ToSecondsStringPrecisionRecord ( normalizedOptions ), https://tc39.es/proposal-temporal/#sec-temporal-tosecondsstringprecisionrecord
ThrowCompletionOr<SecondsStringPrecision> to_seconds_string_precision_record(VM& vm, Object const& normalized_options)
{
    // 1. Let smallestUnit be ? GetTemporalUnit(normalizedOptions, "smallestUnit", time, undefined).
    auto smallest_unit = TRY(get_temporal_unit(vm, normalized_options, vm.names.smallestUnit, UnitGroup::Time, Optional<StringView> {}));

    // 2. If smallestUnit is "hour", throw a RangeError exception.
    if (smallest_unit == "hour"sv)
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, *smallest_unit, "smallestUnit"sv);

    // 3. If smallestUnit is "minute", then
    if (smallest_unit == "minute"sv) {
        // a. Return the Record { [[Precision]]: "minute", [[Unit]]: "minute", [[Increment]]: 1 }.
        return SecondsStringPrecision { .precision = "minute"sv, .unit = "minute"sv, .increment = 1 };
    }

    // 4. If smallestUnit is "second", then
    if (smallest_unit == "second"sv) {
        // a. Return the Record { [[Precision]]: 0, [[Unit]]: "second", [[Increment]]: 1 }.
        return SecondsStringPrecision { .precision = 0, .unit = "second"sv, .increment = 1 };
    }

    // 5. If smallestUnit is "millisecond", then
    if (smallest_unit == "millisecond"sv) {
        // a. Return the Record { [[Precision]]: 3, [[Unit]]: "millisecond", [[Increment]]: 1 }.
        return SecondsStringPrecision { .precision = 3, .unit = "millisecond"sv, .increment = 1 };
    }

    // 6. If smallestUnit is "microsecond", then
    if (smallest_unit == "microsecond"sv) {
        // a. Return the Record { [[Precision]]: 6, [[Unit]]: "microsecond", [[Increment]]: 1 }.
        return SecondsStringPrecision { .precision = 6, .unit = "microsecond"sv, .increment = 1 };
    }

    // 7. If smallestUnit is "nanosecond", then
    if (smallest_unit == "nanosecond"sv) {
        // a. Return the Record { [[Precision]]: 9, [[Unit]]: "nanosecond", [[Increment]]: 1 }.
        return SecondsStringPrecision { .precision = 9, .unit = "nanosecond"sv, .increment = 1 };
    }

    // 8. Assert: smallestUnit is undefined.
    VERIFY(!smallest_unit.has_value());

    // 9. Let fractionalDigitsVal be ? Get(normalizedOptions, "fractionalSecondDigits").
    auto fractional_digits_value = TRY(normalized_options.get(vm.names.fractionalSecondDigits));

    // 10. If Type(fractionalDigitsVal) is not Number, then
    if (!fractional_digits_value.is_number()) {
        // a. If fractionalDigitsVal is not undefined, then
        if (!fractional_digits_value.is_undefined()) {
            // i. If ? ToString(fractionalDigitsVal) is not "auto", throw a RangeError exception.
            if (TRY(fractional_digits_value.to_string(vm)) != "auto"sv)
                return vm.template throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, fractional_digits_value, "fractionalSecondDigits"sv);
        }

        // b. Return the Record { [[Precision]]: "auto", [[Unit]]: "nanosecond", [[Increment]]: 1 }.
        return SecondsStringPrecision { .precision = "auto"sv, .unit = "nanosecond"sv, .increment = 1 };
    }

    // 11. If fractionalDigitsVal is NaN, +∞𝔽, or -∞𝔽, throw a RangeError exception.
    if (fractional_digits_value.is_nan() || fractional_digits_value.is_infinity())
        return vm.template throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, fractional_digits_value, "fractionalSecondDigits"sv);

    // 12. Let fractionalDigitCount be truncate(ℝ(fractionalDigitsVal)).
    auto fractional_digit_count_unchecked = trunc(fractional_digits_value.as_double());

    // 13. If fractionalDigitCount < 0 or fractionalDigitCount > 9, throw a RangeError exception.
    if (fractional_digit_count_unchecked < 0 || fractional_digit_count_unchecked > 9)
        return vm.template throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, fractional_digits_value, "fractionalSecondDigits"sv);

    auto fractional_digit_count = static_cast<u8>(fractional_digit_count_unchecked);

    // 14. If fractionalDigitCount is 0, then
    if (fractional_digit_count == 0) {
        // a. Return the Record { [[Precision]]: 0, [[Unit]]: "second", [[Increment]]: 1 }.
        return SecondsStringPrecision { .precision = 0, .unit = "second"sv, .increment = 1 };
    }

    // 15. If fractionalDigitCount is 1, 2, or 3, then
    if (fractional_digit_count == 1 || fractional_digit_count == 2 || fractional_digit_count == 3) {
        // a. Return the Record { [[Precision]]: fractionalDigitCount, [[Unit]]: "millisecond", [[Increment]]: 10^(3 - fractionalDigitCount) }.
        return SecondsStringPrecision { .precision = fractional_digit_count, .unit = "millisecond"sv, .increment = (u32)pow(10, 3 - fractional_digit_count) };
    }

    // 16. If fractionalDigitCount is 4, 5, or 6, then
    if (fractional_digit_count == 4 || fractional_digit_count == 5 || fractional_digit_count == 6) {
        // a. Return the Record { [[Precision]]: fractionalDigitCount, [[Unit]]: "microsecond", [[Increment]]: 10^(6 - fractionalDigitCount) }.
        return SecondsStringPrecision { .precision = fractional_digit_count, .unit = "microsecond"sv, .increment = (u32)pow(10, 6 - fractional_digit_count) };
    }

    // 17. Assert: fractionalDigitCount is 7, 8, or 9.
    VERIFY(fractional_digit_count == 7 || fractional_digit_count == 8 || fractional_digit_count == 9);

    // 18. Return the Record { [[Precision]]: fractionalDigitCount, [[Unit]]: "nanosecond", [[Increment]]: 10^(9 - fractionalDigitCount) }.
    return SecondsStringPrecision { .precision = fractional_digit_count, .unit = "nanosecond"sv, .increment = (u32)pow(10, 9 - fractional_digit_count) };
}

struct TemporalUnit {
    StringView singular;
    StringView plural;
    UnitGroup category;
};

// https://tc39.es/proposal-temporal/#table-temporal-units
static Vector<TemporalUnit> temporal_units = {
    { "year"sv, "years"sv, UnitGroup::Date },
    { "month"sv, "months"sv, UnitGroup::Date },
    { "week"sv, "weeks"sv, UnitGroup::Date },
    { "day"sv, "days"sv, UnitGroup::Date },
    { "hour"sv, "hours"sv, UnitGroup::Time },
    { "minute"sv, "minutes"sv, UnitGroup::Time },
    { "second"sv, "seconds"sv, UnitGroup::Time },
    { "millisecond"sv, "milliseconds"sv, UnitGroup::Time },
    { "microsecond"sv, "microseconds"sv, UnitGroup::Time },
    { "nanosecond"sv, "nanoseconds"sv, UnitGroup::Time }
};

// 13.15 GetTemporalUnit ( normalizedOptions, key, unitGroup, default [ , extraValues ] ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalunit
ThrowCompletionOr<Optional<String>> get_temporal_unit(VM& vm, Object const& normalized_options, PropertyKey const& key, UnitGroup unit_group, TemporalUnitDefault const& default_, Vector<StringView> const& extra_values)
{
    // 1. Let singularNames be a new empty List.
    Vector<StringView> singular_names;

    // 2. For each row of Table 13, except the header row, in table order, do
    for (auto const& row : temporal_units) {
        // a. Let unit be the value in the Singular column of the row.
        auto unit = row.singular;

        // b. If the Category column of the row is date and unitGroup is date or datetime, append unit to singularNames.
        if (row.category == UnitGroup::Date && (unit_group == UnitGroup::Date || unit_group == UnitGroup::DateTime))
            singular_names.append(unit);
        // c. Else if the Category column of the row is time and unitGroup is time or datetime, append unit to singularNames.
        else if (row.category == UnitGroup::Time && (unit_group == UnitGroup::Time || unit_group == UnitGroup::DateTime))
            singular_names.append(unit);
    }

    // 3. If extraValues is present, then
    if (!extra_values.is_empty()) {
        // a. Set singularNames to the list-concatenation of singularNames and extraValues.
        singular_names.extend(extra_values);
    }

    OptionDefault default_value;

    // 4. If default is required, then
    if (default_.has<TemporalUnitRequired>()) {
        // a. Let defaultValue be undefined.
        default_value = {};
    }
    // 5. Else,
    else {
        auto default_string = default_.get<Optional<StringView>>();

        // a. Let defaultValue be default.
        default_value = default_string.has_value() ? OptionDefault { *default_string } : OptionDefault {};

        // b. If defaultValue is not undefined and singularNames does not contain defaultValue, then
        if (default_string.has_value() && !singular_names.contains_slow(*default_string)) {
            // i. Append defaultValue to singularNames.
            singular_names.append(*default_string);
        }
    }

    // 6. Let allowedValues be a copy of singularNames.
    auto allowed_values = singular_names;

    // 7. For each element singularName of singularNames, do
    for (auto const& singular_name : singular_names) {
        for (auto const& row : temporal_units) {
            // a. If singularName is listed in the Singular column of Table 13, then
            if (singular_name == row.singular) {
                // i. Let pluralName be the value in the Plural column of the corresponding row.
                auto plural_name = row.plural;

                // ii. Append pluralName to allowedValues.
                allowed_values.append(plural_name);
            }
        }
    }

    // 8. NOTE: For each singular Temporal unit name that is contained within allowedValues, the corresponding plural name is also contained within it.

    // 9. Let value be ? GetOption(normalizedOptions, key, "string", allowedValues, defaultValue).
    auto option_value = TRY(get_option(vm, normalized_options, key, OptionType::String, allowed_values.span(), default_value));

    // 10. If value is undefined and default is required, throw a RangeError exception.
    if (option_value.is_undefined() && default_.has<TemporalUnitRequired>())
        return vm.throw_completion<RangeError>(ErrorType::IsUndefined, ByteString::formatted("{} option value", key.as_string()));

    auto value = option_value.is_undefined()
        ? Optional<String> {}
        : option_value.as_string().utf8_string();

    // 11. If value is listed in the Plural column of Table 13, then
    for (auto const& row : temporal_units) {
        if (row.plural == value) {
            // a. Set value to the value in the Singular column of the corresponding row.
            value = TRY_OR_THROW_OOM(vm, String::from_utf8(row.singular));
        }
    }

    // 12. Return value.
    return value;
}

// FIXME: This function is a hack to undo a RelativeTo back to the old API, which was just a Value.
//        We should get rid of this once all callers have been converted to the new API.
Value relative_to_converted_to_value(RelativeTo const& relative_to)
{
    if (relative_to.plain_relative_to)
        return relative_to.plain_relative_to;

    if (relative_to.zoned_relative_to)
        return relative_to.zoned_relative_to;

    return js_undefined();
}

// 13.16 ToRelativeTemporalObject ( options ), https://tc39.es/proposal-temporal/#sec-temporal-torelativetemporalobject
ThrowCompletionOr<RelativeTo> to_relative_temporal_object(VM& vm, Object const& options)
{
    auto& realm = *vm.current_realm();

    // 1. Assert: Type(options) is Object.

    // 2. Let value be ? Get(options, "relativeTo").
    auto value = TRY(options.get(vm.names.relativeTo));

    // 3. If value is undefined, then
    if (value.is_undefined()) {
        // a. Return value.
        return RelativeTo {};
    }

    // 4. Let offsetBehaviour be option.
    auto offset_behavior = OffsetBehavior::Option;

    // 5. Let matchBehaviour be match exactly.
    auto match_behavior = MatchBehavior::MatchExactly;

    ISODateTime result;
    Value offset_string;
    Value time_zone;
    Object* calendar = nullptr;

    // 6. If Type(value) is Object, then
    if (value.is_object()) {
        auto& value_object = value.as_object();
        // a. If value has an [[InitializedTemporalZonedDateTime]] internal slot, then
        if (is<ZonedDateTime>(value_object)) {
            auto& zoned_relative_to = static_cast<ZonedDateTime&>(value_object);

            // i. Let timeZoneRec be ? CreateTimeZoneMethodsRecord(value.[[TimeZone]], « GET-OFFSET-NANOSECONDS-FOR, GET-POSSIBLE-INSTANTS-FOR »).
            auto time_zone_record = TRY(create_time_zone_methods_record(vm, NonnullGCPtr<Object> { zoned_relative_to.time_zone() }, { { TimeZoneMethod::GetOffsetNanosecondsFor, TimeZoneMethod::GetPossibleInstantsFor } }));

            // ii. Return the Record { [[PlainRelativeTo]]: undefined, [[ZonedRelativeTo]]: value, [[TimeZoneRec]]: timeZoneRec }.
            return RelativeTo {
                .plain_relative_to = {},
                .zoned_relative_to = zoned_relative_to,
                .time_zone_record = time_zone_record,
            };
        }

        // b. If value has an [[InitializedTemporalDate]] internal slot, then
        if (is<PlainDate>(value_object)) {
            // i. Return the Record { [[PlainRelativeTo]]: value, [[ZonedRelativeTo]]: undefined, [[TimeZoneRec]]: undefined }.
            return RelativeTo {
                .plain_relative_to = static_cast<PlainDate&>(value_object),
                .zoned_relative_to = {},
                .time_zone_record = {},
            };
        }

        // c. If value has an [[InitializedTemporalDateTime]] internal slot, then
        if (is<PlainDateTime>(value_object)) {
            auto& plain_date_time = static_cast<PlainDateTime&>(value_object);

            // i. Let plainDate be ! CreateTemporalDate(value.[[ISOYear]], value.[[ISOMonth]], value.[[ISODay]], value.[[Calendar]]).
            auto* plain_date = TRY(create_temporal_date(vm, plain_date_time.iso_year(), plain_date_time.iso_month(), plain_date_time.iso_day(), plain_date_time.calendar()));

            // ii. Return the Record { [[PlainRelativeTo]]: plainDate, [[ZonedRelativeTo]]: undefined, [[TimeZoneRec]]: undefined }.
            return RelativeTo {
                .plain_relative_to = plain_date,
                .zoned_relative_to = {},
                .time_zone_record = {},
            };
        }

        // d. Let calendar be ? GetTemporalCalendarWithISODefault(value).
        calendar = TRY(get_temporal_calendar_with_iso_default(vm, value_object));

        // e. Let fieldNames be ? CalendarFields(calendar, « "day", "hour", "microsecond", "millisecond", "minute", "month", "monthCode", "nanosecond", "second", "year" »).
        auto field_names = TRY(calendar_fields(vm, *calendar, { "day"sv, "hour"sv, "microsecond"sv, "millisecond"sv, "minute"sv, "month"sv, "monthCode"sv, "nanosecond"sv, "second"sv, "year"sv }));

        // f. Let fields be ? PrepareTemporalFields(value, fieldNames, «»).
        auto* fields = TRY(prepare_temporal_fields(vm, value_object, field_names, Vector<StringView> {}));

        // g. Let dateOptions be OrdinaryObjectCreate(null).
        auto date_options = Object::create(realm, nullptr);

        // h. Perform ! CreateDataPropertyOrThrow(dateOptions, "overflow", "constrain").
        MUST(date_options->create_data_property_or_throw(vm.names.overflow, PrimitiveString::create(vm, "constrain"_string)));

        // i. Let result be ? InterpretTemporalDateTimeFields(calendar, fields, dateOptions).
        result = TRY(interpret_temporal_date_time_fields(vm, *calendar, *fields, date_options));

        // j. Let offsetString be ? Get(value, "offset").
        offset_string = TRY(value_object.get(vm.names.offset));

        // k. Let timeZone be ? Get(value, "timeZone").
        time_zone = TRY(value_object.get(vm.names.timeZone));

        // l. If timeZone is not undefined, then
        if (!time_zone.is_undefined()) {
            // i. Set timeZone to ? ToTemporalTimeZone(timeZone).
            time_zone = TRY(to_temporal_time_zone(vm, time_zone));
        }

        // m. If offsetString is undefined, then
        if (offset_string.is_undefined()) {
            // i. Set offsetBehaviour to wall.
            offset_behavior = OffsetBehavior::Wall;
        }
    }
    // 7. Else,
    else {
        // a. Let string be ? ToString(value).
        auto string = TRY(value.to_string(vm));

        // b. Let result be ? ParseTemporalRelativeToString(string).
        result = TRY(parse_temporal_relative_to_string(vm, string));

        // c. Let calendar be ? ToTemporalCalendarWithISODefault(result.[[Calendar]]).
        calendar = TRY(to_temporal_calendar_with_iso_default(vm, result.calendar.has_value() ? PrimitiveString::create(vm, *result.calendar) : js_undefined()));

        // d. Let offsetString be result.[[TimeZone]].[[OffsetString]].
        offset_string = result.time_zone.offset_string.has_value() ? PrimitiveString::create(vm, *result.time_zone.offset_string) : js_undefined();

        // e. Let timeZoneName be result.[[TimeZone]].[[Name]].
        auto time_zone_name = result.time_zone.name;

        // f. If timeZoneName is undefined, then
        if (!time_zone_name.has_value()) {
            // i. Let timeZone be undefined.
            time_zone = js_undefined();
        }
        // g. Else,
        else {
            // i. If IsTimeZoneOffsetString(timeZoneName) is false, then
            if (!is_time_zone_offset_string(*time_zone_name)) {
                // 1. If IsAvailableTimeZoneName(timeZoneName) is false, throw a RangeError exception.
                if (!is_available_time_zone_name(*time_zone_name))
                    return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeZoneName, *time_zone_name);

                // 2. Set timeZoneName to ! CanonicalizeTimeZoneName(timeZoneName).
                time_zone_name = MUST_OR_THROW_OOM(canonicalize_time_zone_name(vm, *time_zone_name));
            }

            // ii. Let timeZone be ! CreateTemporalTimeZone(timeZoneName).
            time_zone = MUST_OR_THROW_OOM(create_temporal_time_zone(vm, time_zone_name.release_value()));

            // iii. If result.[[TimeZone]].[[Z]] is true, then
            if (result.time_zone.z) {
                // 1. Set offsetBehaviour to exact.
                offset_behavior = OffsetBehavior::Exact;
            }
            // iv. Else if offsetString is undefined, then
            else if (offset_string.is_undefined()) {
                // 1. Set offsetBehaviour to wall.
                offset_behavior = OffsetBehavior::Wall;
            }

            // v. Set matchBehaviour to match minutes.
            match_behavior = MatchBehavior::MatchMinutes;
        }
    }

    // 8. If timeZone is undefined, then
    if (time_zone.is_undefined()) {
        // a. Return ? CreateTemporalDate(result.[[Year]], result.[[Month]], result.[[Day]], calendar).
        auto* plain_date = TRY(create_temporal_date(vm, result.year, result.month, result.day, *calendar));
        return RelativeTo {
            .plain_relative_to = plain_date,
            .zoned_relative_to = {},
            .time_zone_record = {},
        };
    }

    double offset_ns;

    // 9. If offsetBehaviour is option, then
    if (offset_behavior == OffsetBehavior::Option) {
        // a. Set offsetString to ? ToString(offsetString).
        // NOTE: offsetString is not used after this path, so we don't need to put this into the original offset_string which is of type JS::Value.
        auto actual_offset_string = TRY(offset_string.to_string(vm));

        // b. If IsTimeZoneOffsetString(offsetString) is false, throw a RangeError exception.
        if (!is_time_zone_offset_string(actual_offset_string))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeZoneName, actual_offset_string);

        // c. Let offsetNs be ParseTimeZoneOffsetString(offsetString).
        offset_ns = parse_time_zone_offset_string(actual_offset_string);
    }
    // 10. Else,
    else {
        // a. Let offsetNs be 0.
        offset_ns = 0;
    }

    // 11. Let epochNanoseconds be ? InterpretISODateTimeOffset(result.[[Year]], result.[[Month]], result.[[Day]], result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]], offsetBehaviour, offsetNs, timeZone, "compatible", "reject", matchBehaviour).
    auto const* epoch_nanoseconds = TRY(interpret_iso_date_time_offset(vm, result.year, result.month, result.day, result.hour, result.minute, result.second, result.millisecond, result.microsecond, result.nanosecond, offset_behavior, offset_ns, time_zone, "compatible"sv, "reject"sv, match_behavior));

    // 12. Return ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
    auto time_zone_record = TRY(create_time_zone_methods_record(vm, NonnullGCPtr<Object> { time_zone.as_object() }, { { TimeZoneMethod::GetOffsetNanosecondsFor, TimeZoneMethod::GetPossibleInstantsFor } }));
    auto* zoned_relative_to = MUST(create_temporal_zoned_date_time(vm, *epoch_nanoseconds, time_zone.as_object(), *calendar));
    return RelativeTo {
        .plain_relative_to = {},
        .zoned_relative_to = zoned_relative_to,
        .time_zone_record = time_zone_record,
    };
}

// 13.17 LargerOfTwoTemporalUnits ( u1, u2 ), https://tc39.es/proposal-temporal/#sec-temporal-largeroftwotemporalunits
StringView larger_of_two_temporal_units(StringView unit1, StringView unit2)
{
    // 1. Assert: Both u1 and u2 are listed in the Singular column of Table 13.

    // 2. For each row of Table 13, except the header row, in table order, do
    for (auto const& row : temporal_units) {
        // a. Let unit be the value in the Singular column of the row.
        auto unit = row.singular;

        // b. If SameValue(u1, unit) is true, return unit.
        if (unit1 == unit)
            return unit;

        // c. If SameValue(u2, unit) is true, return unit.
        if (unit2 == unit)
            return unit;
    }
    VERIFY_NOT_REACHED();
}

// 13.18 MergeLargestUnitOption ( options, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-mergelargestunitoption
ThrowCompletionOr<Object*> merge_largest_unit_option(VM& vm, Object const& options, String largest_unit)
{
    auto& realm = *vm.current_realm();

    // 1. Let merged be OrdinaryObjectCreate(null).
    auto merged = Object::create(realm, nullptr);

    // 2. Let keys be ? EnumerableOwnPropertyNames(options, key).
    auto keys = TRY(options.enumerable_own_property_names(Object::PropertyKind::Key));

    // 3. For each element nextKey of keys, do
    for (auto& key : keys) {
        auto next_key = MUST(PropertyKey::from_value(vm, key));

        // a. Let propValue be ? Get(options, nextKey).
        auto prop_value = TRY(options.get(next_key));

        // b. Perform ! CreateDataPropertyOrThrow(merged, nextKey, propValue).
        MUST(merged->create_data_property_or_throw(next_key, prop_value));
    }

    // 4. Perform ! CreateDataPropertyOrThrow(merged, "largestUnit", largestUnit).
    MUST(merged->create_data_property_or_throw(vm.names.largestUnit, PrimitiveString::create(vm, move(largest_unit))));

    // 5. Return merged.
    return merged.ptr();
}

// 13.19 MaximumTemporalDurationRoundingIncrement ( unit ), https://tc39.es/proposal-temporal/#sec-temporal-maximumtemporaldurationroundingincrement
Optional<u16> maximum_temporal_duration_rounding_increment(StringView unit)
{
    // 1. If unit is "year", "month", "week", or "day", then
    if (unit.is_one_of("year"sv, "month"sv, "week"sv, "day"sv)) {
        // a. Return undefined.
        return {};
    }

    // 2. If unit is "hour", then
    if (unit == "hour"sv) {
        // a. Return 24.
        return 24;
    }

    // 3. If unit is "minute" or "second", then
    if (unit.is_one_of("minute"sv, "second"sv)) {
        // a. Return 60.
        return 60;
    }

    // 4. Assert: unit is one of "millisecond", "microsecond", or "nanosecond".
    VERIFY(unit.is_one_of("millisecond"sv, "microsecond"sv, "nanosecond"sv));

    // 5. Return 1000.
    return 1000;
}

// 13.20 RejectObjectWithCalendarOrTimeZone ( object ), https://tc39.es/proposal-temporal/#sec-temporal-rejectobjectwithcalendarortimezone
ThrowCompletionOr<void> reject_object_with_calendar_or_time_zone(VM& vm, Object& object)
{
    // 1. Assert: Type(object) is Object.

    // 2. If object has an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], [[InitializedTemporalMonthDay]], [[InitializedTemporalTime]], [[InitializedTemporalYearMonth]], or [[InitializedTemporalZonedDateTime]] internal slot, then
    if (is<PlainDate>(object) || is<PlainDateTime>(object) || is<PlainMonthDay>(object) || is<PlainTime>(object) || is<PlainYearMonth>(object) || is<ZonedDateTime>(object)) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::TemporalObjectMustNotHave, "calendar or timeZone");
    }

    // 3. Let calendarProperty be ? Get(object, "calendar").
    auto calendar_property = TRY(object.get(vm.names.calendar));

    // 4. If calendarProperty is not undefined, then
    if (!calendar_property.is_undefined()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::TemporalObjectMustNotHave, "calendar");
    }

    // 5. Let timeZoneProperty be ? Get(object, "timeZone").
    auto time_zone_property = TRY(object.get(vm.names.timeZone));

    // 6. If timeZoneProperty is not undefined, then
    if (!time_zone_property.is_undefined()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::TemporalObjectMustNotHave, "timeZone");
    }

    return {};
}

// 13.21 FormatSecondsStringPart ( second, millisecond, microsecond, nanosecond, precision ), https://tc39.es/proposal-temporal/#sec-temporal-formatsecondsstringpart
ThrowCompletionOr<String> format_seconds_string_part(VM& vm, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, Variant<StringView, u8> const& precision)
{
    // 1. Assert: second, millisecond, microsecond, and nanosecond are integers.

    // Non-standard sanity check
    if (precision.has<StringView>())
        VERIFY(precision.get<StringView>().is_one_of("minute"sv, "auto"sv));

    // 2. If precision is "minute", return "".
    if (precision.has<StringView>() && precision.get<StringView>() == "minute"sv)
        return String {};

    // 3. Let secondsString be the string-concatenation of the code unit 0x003A (COLON) and ToZeroPaddedDecimalString(second, 2).
    auto seconds_string = TRY_OR_THROW_OOM(vm, String::formatted(":{:02}", second));

    // 4. Let fraction be millisecond × 10^6 + microsecond × 10^3 + nanosecond.
    u32 fraction = millisecond * 1'000'000 + microsecond * 1'000 + nanosecond;

    String fraction_string;

    // 5. If precision is "auto", then
    if (precision.has<StringView>() && precision.get<StringView>() == "auto"sv) {
        // a. If fraction is 0, return secondsString.
        if (fraction == 0)
            return seconds_string;

        // b. Set fraction to ToZeroPaddedDecimalString(fraction, 9).
        fraction_string = TRY_OR_THROW_OOM(vm, String::formatted("{:09}", fraction));

        // c. Set fraction to the longest possible substring of fraction starting at position 0 and not ending with the code unit 0x0030 (DIGIT ZERO).
        fraction_string = TRY_OR_THROW_OOM(vm, fraction_string.trim("0"sv, TrimMode::Right));
    }
    // 6. Else,
    else {
        // a. If precision is 0, return secondsString.
        if (precision.get<u8>() == 0)
            return seconds_string;

        // b. Set fraction to ToZeroPaddedDecimalString(fraction, 9)
        fraction_string = TRY_OR_THROW_OOM(vm, String::formatted("{:09}", fraction));

        // c. Set fraction to the substring of fraction from 0 to precision.
        fraction_string = TRY_OR_THROW_OOM(vm, fraction_string.substring_from_byte_offset(0, precision.get<u8>()));
    }

    // 7. Return the string-concatenation of secondsString, the code unit 0x002E (FULL STOP), and fraction.
    return TRY_OR_THROW_OOM(vm, String::formatted("{}.{}", seconds_string, fraction_string));
}

// 13.23 GetUnsignedRoundingMode ( roundingMode, isNegative ), https://tc39.es/proposal-temporal/#sec-temporal-getunsignedroundingmode
UnsignedRoundingMode get_unsigned_rounding_mode(StringView rounding_mode, bool is_negative)
{
    // 1. If isNegative is true, return the specification type in the third column of Table 14 where the first column is roundingMode and the second column is "negative".
    if (is_negative) {
        if (rounding_mode == "ceil"sv)
            return UnsignedRoundingMode::Zero;
        if (rounding_mode == "floor"sv)
            return UnsignedRoundingMode::Infinity;
        if (rounding_mode == "expand"sv)
            return UnsignedRoundingMode::Infinity;
        if (rounding_mode == "trunc"sv)
            return UnsignedRoundingMode::Zero;
        if (rounding_mode == "halfCeil"sv)
            return UnsignedRoundingMode::HalfZero;
        if (rounding_mode == "halfFloor"sv)
            return UnsignedRoundingMode::HalfInfinity;
        if (rounding_mode == "halfExpand"sv)
            return UnsignedRoundingMode::HalfInfinity;
        if (rounding_mode == "halfTrunc"sv)
            return UnsignedRoundingMode::HalfZero;
        if (rounding_mode == "halfEven"sv)
            return UnsignedRoundingMode::HalfEven;
        VERIFY_NOT_REACHED();
    }
    // 2. Else, return the specification type in the third column of Table 14 where the first column is roundingMode and the second column is "positive".
    else {
        if (rounding_mode == "ceil"sv)
            return UnsignedRoundingMode::Infinity;
        if (rounding_mode == "floor"sv)
            return UnsignedRoundingMode::Zero;
        if (rounding_mode == "expand"sv)
            return UnsignedRoundingMode::Infinity;
        if (rounding_mode == "trunc"sv)
            return UnsignedRoundingMode::Zero;
        if (rounding_mode == "halfCeil"sv)
            return UnsignedRoundingMode::HalfInfinity;
        if (rounding_mode == "halfFloor"sv)
            return UnsignedRoundingMode::HalfZero;
        if (rounding_mode == "halfExpand"sv)
            return UnsignedRoundingMode::HalfInfinity;
        if (rounding_mode == "halfTrunc"sv)
            return UnsignedRoundingMode::HalfZero;
        if (rounding_mode == "halfEven"sv)
            return UnsignedRoundingMode::HalfEven;
        VERIFY_NOT_REACHED();
    }
}

// NOTE: We have two variants of these functions, one using doubles and one using BigInts - most of the time
// doubles will be fine, but take care to choose the right one. The spec is not very clear about this, as
// it uses mathematical values which can be arbitrarily (but not infinitely) large.
// Incidentally V8's Temporal implementation does the same :^)

// 13.24 ApplyUnsignedRoundingMode ( x, r1, r2, unsignedRoundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-applyunsignedroundingmode
double apply_unsigned_rounding_mode(double x, double r1, double r2, Optional<UnsignedRoundingMode> const& unsigned_rounding_mode)
{
    // 1. If x is equal to r1, return r1.
    if (x == r1)
        return r1;

    // 2. Assert: r1 < x < r2.
    VERIFY(r1 < x && x < r2);

    // 3. Assert: unsignedRoundingMode is not undefined.
    VERIFY(unsigned_rounding_mode.has_value());

    // 4. If unsignedRoundingMode is zero, return r1.
    if (unsigned_rounding_mode == UnsignedRoundingMode::Zero)
        return r1;

    // 5. If unsignedRoundingMode is infinity, return r2.
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

    // 11. If unsignedRoundingMode is half-zero, return r1.
    if (unsigned_rounding_mode == UnsignedRoundingMode::HalfZero)
        return r1;

    // 12. If unsignedRoundingMode is half-infinity, return r2.
    if (unsigned_rounding_mode == UnsignedRoundingMode::HalfInfinity)
        return r2;

    // 13. Assert: unsignedRoundingMode is half-even.
    VERIFY(unsigned_rounding_mode == UnsignedRoundingMode::HalfEven);

    // 14. Let cardinality be (r1 / (r2 – r1)) modulo 2.
    auto cardinality = modulo((r1 / (r2 - r1)), 2);

    // 15. If cardinality is 0, return r1.
    if (cardinality == 0)
        return r1;

    // 16. Return r2.
    return r2;
}

// 13.24 ApplyUnsignedRoundingMode ( x, r1, r2, unsignedRoundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-applyunsignedroundingmode
Crypto::SignedBigInteger apply_unsigned_rounding_mode(Crypto::SignedDivisionResult const& x, Crypto::SignedBigInteger const& r1, Crypto::SignedBigInteger const& r2, Optional<UnsignedRoundingMode> const& unsigned_rounding_mode, Crypto::UnsignedBigInteger const& increment)
{
    // 1. If x is equal to r1, return r1.
    if (x.quotient == r1 && x.remainder.unsigned_value().is_zero())
        return r1;

    // 2. Assert: r1 < x < r2.
    // NOTE: Skipped for the sake of performance

    // 3. Assert: unsignedRoundingMode is not undefined.
    VERIFY(unsigned_rounding_mode.has_value());

    // 4. If unsignedRoundingMode is zero, return r1.
    if (unsigned_rounding_mode == UnsignedRoundingMode::Zero)
        return r1;

    // 5. If unsignedRoundingMode is infinity, return r2.
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
    // NOTE: Skipped for the sake of performance

    // 11. If unsignedRoundingMode is half-zero, return r1.
    if (unsigned_rounding_mode == UnsignedRoundingMode::HalfZero)
        return r1;

    // 12. If unsignedRoundingMode is half-infinity, return r2.
    if (unsigned_rounding_mode == UnsignedRoundingMode::HalfInfinity)
        return r2;

    // 13. Assert: unsignedRoundingMode is half-even.
    VERIFY(unsigned_rounding_mode == UnsignedRoundingMode::HalfEven);

    // 14. Let cardinality be (r1 / (r2 – r1)) modulo 2.
    auto cardinality = modulo(r1.divided_by(r2.minus(r1)).quotient, "2"_bigint);

    // 15. If cardinality is 0, return r1.
    if (cardinality.unsigned_value().is_zero())
        return r1;

    // 16. Return r2.
    return r2;
}

// 13.25 RoundNumberToIncrement ( x, increment, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundnumbertoincrement
double round_number_to_increment(double x, u64 increment, StringView rounding_mode)
{
    VERIFY(rounding_mode.is_one_of("ceil"sv, "floor"sv, "expand"sv, "trunc"sv, "halfCeil"sv, "halfFloor"sv, "halfExpand"sv, "halfTrunc"sv, "halfEven"sv));

    // 1. Let quotient be x / increment.
    auto quotient = x / static_cast<double>(increment);

    bool is_negative;

    // 2. If quotient < 0, then
    if (quotient < 0) {
        // a. Let isNegative be true.
        is_negative = true;

        // b. Set quotient to -quotient.
        quotient = -quotient;
    }
    // 3. Else,
    else {
        // a. Let isNegative be false.
        is_negative = false;
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

    // 8. If isNegative is true, set rounded to -rounded.
    if (is_negative)
        rounded = -rounded;

    // 9. Return rounded × increment.
    return rounded * static_cast<double>(increment);
}

// 13.25 RoundNumberToIncrement ( x, increment, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundnumbertoincrement
Crypto::SignedBigInteger round_number_to_increment(Crypto::SignedBigInteger const& x, u64 increment, StringView rounding_mode)
{
    VERIFY(rounding_mode.is_one_of("ceil"sv, "floor"sv, "expand"sv, "trunc"sv, "halfCeil"sv, "halfFloor"sv, "halfExpand"sv, "halfTrunc"sv, "halfEven"sv));

    // OPTIMIZATION: If the increment is 1 the number is always rounded
    if (increment == 1)
        return x;

    auto increment_big_int = Crypto::UnsignedBigInteger { increment };

    // 1. Let quotient be x / increment.
    auto division_result = x.divided_by(increment_big_int);

    // OPTIMIZATION: If there's no remainder the number is already rounded
    if (division_result.remainder.unsigned_value().is_zero())
        return x;

    bool is_negative;

    // 2. If quotient < 0, then
    if (division_result.quotient.is_negative()) {
        // a. Let isNegative be true.
        is_negative = true;

        // b. Set quotient to -quotient.
        division_result.quotient.negate();
        division_result.remainder.negate();
    }
    // 3. Else,
    else {
        // a. Let isNegative be false.
        is_negative = false;
    }

    // 4. Let unsignedRoundingMode be GetUnsignedRoundingMode(roundingMode, isNegative).
    auto unsigned_rounding_mode = get_unsigned_rounding_mode(rounding_mode, is_negative);

    // 5. Let r1 be the largest integer such that r1 ≤ quotient.
    auto r1 = division_result.quotient;

    // 6. Let r2 be the smallest integer such that r2 > quotient.
    auto r2 = division_result.quotient.plus("1"_bigint);

    // 7. Let rounded be ApplyUnsignedRoundingMode(quotient, r1, r2, unsignedRoundingMode).
    auto rounded = apply_unsigned_rounding_mode(division_result, r1, r2, unsigned_rounding_mode, increment_big_int);

    // 8. If isNegative is true, set rounded to -rounded.
    if (is_negative)
        rounded.negate();

    // 9. Return rounded × increment.
    return rounded.multiplied_by(increment_big_int);
}

// 13.26 RoundNumberToIncrementAsIfPositive ( x, increment, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundnumbertoincrementasifpositive
Crypto::SignedBigInteger round_number_to_increment_as_if_positive(Crypto::SignedBigInteger const& x, u64 increment, StringView rounding_mode)
{
    VERIFY(rounding_mode.is_one_of("ceil"sv, "floor"sv, "expand"sv, "trunc"sv, "halfCeil"sv, "halfFloor"sv, "halfExpand"sv, "halfTrunc"sv, "halfEven"sv));

    // OPTIMIZATION: If the increment is 1 the number is always rounded
    if (increment == 1)
        return x;

    auto increment_big_int = Crypto::UnsignedBigInteger { increment };

    // 1. Let quotient be x / increment.
    auto division_result = x.divided_by(increment_big_int);

    // OPTIMIZATION: If there's no remainder the number is already rounded
    if (division_result.remainder.unsigned_value().is_zero())
        return x;

    // 2. Let unsignedRoundingMode be GetUnsignedRoundingMode(roundingMode, false).
    auto unsigned_rounding_mode = get_unsigned_rounding_mode(rounding_mode, false);

    // 3. Let r1 be the largest integer such that r1 ≤ quotient.
    auto r1 = division_result.quotient;

    // 4. Let r2 be the smallest integer such that r2 > quotient.
    auto r2 = division_result.quotient.plus("1"_bigint);

    // 5. Let rounded be ApplyUnsignedRoundingMode(quotient, r1, r2, unsignedRoundingMode).
    auto rounded = apply_unsigned_rounding_mode(division_result, r1, r2, unsigned_rounding_mode, increment_big_int);

    // 6. Return rounded × increment.
    return rounded.multiplied_by(increment_big_int);
}

// 13.28 ParseISODateTime ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parseisodatetime
ThrowCompletionOr<ISODateTime> parse_iso_date_time(VM& vm, StringView iso_string)
{
    // 1. Let parseResult be empty.
    Optional<ParseResult> parse_result;

    static constexpr auto productions_valid_with_any_calendar = AK::Array {
        Production::TemporalDateTimeString,
        Production::TemporalInstantString,
        Production::TemporalTimeString,
        Production::TemporalZonedDateTimeString,
    };

    // 2. For each nonterminal goal of « TemporalDateTimeString, TemporalInstantString, TemporalTimeString, TemporalZonedDateTimeString », do
    for (auto goal : productions_valid_with_any_calendar) {
        // a. If parseResult is not a Parse Node, set parseResult to ParseText(StringToCodePoints(isoString), goal).
        parse_result = parse_iso8601(goal, iso_string);
        if (parse_result.has_value())
            break;
    }

    static constexpr auto productions_valid_only_with_iso8601_calendar = AK::Array {
        Production::TemporalMonthDayString,
        Production::TemporalYearMonthString,
    };

    // 3. For each nonterminal goal of « TemporalMonthDayString, TemporalYearMonthString », do
    for (auto goal : productions_valid_only_with_iso8601_calendar) {
        // a. If parseResult is not a Parse Node, then
        if (!parse_result.has_value()) {
            // i. Set parseResult to ParseText(StringToCodePoints(isoString), goal).
            parse_result = parse_iso8601(goal, iso_string);

            // NOTE: This is not done in parse_iso_date_time(VM, ParseResult) below because MonthDay and YearMonth must re-parse their strings,
            //       as the string could actually be a superset string above in `productions_valid_with_any_calendar` and thus not hit this code path at all.
            //       All other users of parse_iso_date_time(VM, ParseResult) pass in a ParseResult resulting from a production in `productions_valid_with_any_calendar`,
            //       and thus cannot hit this code path as they would first parse in step 2 and not step 3.
            // ii. If parseResult is a Parse Node, then
            if (parse_result.has_value()) {
                // 1. For each Annotation Parse Node annotation contained within parseResult, do
                for (auto const& annotation : parse_result->annotations) {
                    // a. Let key be the source text matched by the AnnotationKey Parse Node contained within annotation.
                    auto const& key = annotation.key;

                    // b. Let value be the source text matched by the AnnotationValue Parse Node contained within annotation.
                    auto const& value = annotation.value;

                    // c. If CodePointsToString(key) is "u-ca" and the ASCII-lowercase of CodePointsToString(value) is not "iso8601", throw a RangeError exception.
                    if (key == "u-ca"sv && value.to_lowercase_string() != "iso8601"sv) {
                        if (goal == Production::TemporalMonthDayString)
                            return vm.throw_completion<RangeError>(ErrorType::TemporalOnlyISO8601WithMonthDayString);
                        else
                            return vm.throw_completion<RangeError>(ErrorType::TemporalOnlyISO8601WithYearMonthString);
                    }
                }
            }
        }
    }

    // 4. If parseResult is not a Parse Node, throw a RangeError exception.
    if (!parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODateTime);

    return parse_iso_date_time(vm, *parse_result);
}

// 13.28 ParseISODateTime ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parseisodatetime
ThrowCompletionOr<ISODateTime> parse_iso_date_time(VM& vm, ParseResult const& parse_result)
{
    // NOTE: Steps 1-4 is handled in parse_iso_date_time(VM, StringView) above.
    // 5. Let each of year, month, day, hour, minute, second, and fSeconds be the source text matched by the respective DateYear, DateMonth, DateDay, TimeHour, TimeMinute, TimeSecond, and TimeFraction Parse Node contained within parseResult, or an empty sequence of code points if not present.
    auto year = parse_result.date_year;
    auto month = parse_result.date_month;
    auto day = parse_result.date_day;
    auto hour = parse_result.time_hour;
    auto minute = parse_result.time_minute;
    auto second = parse_result.time_second;
    auto f_seconds = parse_result.time_fraction;

    // 6. If the first code point of year is U+2212 (MINUS SIGN), replace the first code point with U+002D (HYPHEN-MINUS).
    Optional<String> normalized_year;
    if (year.has_value()) {
        normalized_year = year->starts_with("\xE2\x88\x92"sv)
            ? TRY_OR_THROW_OOM(vm, String::formatted("-{}", year->substring_view(3)))
            : TRY_OR_THROW_OOM(vm, String::from_utf8(*year));
    }

    // 7. Let yearMV be ! ToIntegerOrInfinity(CodePointsToString(year)).
    auto year_mv = *normalized_year.value_or("0"_string).to_number<i32>();

    // 8. If month is empty, then
    //    a. Let monthMV be 1.
    // 9. Else,
    //    a. Let monthMV be ! ToIntegerOrInfinity(CodePointsToString(month)).
    auto month_mv = *month.value_or("1"sv).to_number<u8>();

    // 10. If day is empty, then
    //    a. Let dayMV be 1.
    // 11. Else,
    //    a. Let dayMV be ! ToIntegerOrInfinity(CodePointsToString(day)).
    auto day_mv = *day.value_or("1"sv).to_number<u8>();

    // 12. Let hourMV be ! ToIntegerOrInfinity(CodePointsToString(hour)).
    auto hour_mv = *hour.value_or("0"sv).to_number<u8>();

    // 13. Let minuteMV be ! ToIntegerOrInfinity(CodePointsToString(minute)).
    auto minute_mv = *minute.value_or("0"sv).to_number<u8>();

    // 14. Let secondMV be ! ToIntegerOrInfinity(CodePointsToString(second)).
    auto second_mv = *second.value_or("0"sv).to_number<u8>();

    // 15. If secondMV is 60, then
    if (second_mv == 60) {
        // a. Set secondMV to 59.
        second_mv = 59;
    }

    u16 millisecond_mv;
    u16 microsecond_mv;
    u16 nanosecond_mv;

    // 16. If fSeconds is not empty, then
    if (f_seconds.has_value()) {
        // a. Let fSecondsDigits be the substring of CodePointsToString(fSeconds) from 1.
        auto f_seconds_digits = f_seconds->substring_view(1);

        // b. Let fSecondsDigitsExtended be the string-concatenation of fSecondsDigits and "000000000".
        auto f_seconds_digits_extended = TRY_OR_THROW_OOM(vm, String::formatted("{}000000000", f_seconds_digits));

        // c. Let millisecond be the substring of fSecondsDigitsExtended from 0 to 3.
        auto millisecond = TRY_OR_THROW_OOM(vm, f_seconds_digits_extended.substring_from_byte_offset_with_shared_superstring(0, 3));

        // d. Let microsecond be the substring of fSecondsDigitsExtended from 3 to 6.
        auto microsecond = TRY_OR_THROW_OOM(vm, f_seconds_digits_extended.substring_from_byte_offset_with_shared_superstring(3, 3));

        // e. Let nanosecond be the substring of fSecondsDigitsExtended from 6 to 9.
        auto nanosecond = TRY_OR_THROW_OOM(vm, f_seconds_digits_extended.substring_from_byte_offset_with_shared_superstring(6, 3));

        // f. Let millisecondMV be ! ToIntegerOrInfinity(millisecond).
        millisecond_mv = *millisecond.to_number<u16>();

        // g. Let microsecondMV be ! ToIntegerOrInfinity(microsecond).
        microsecond_mv = *microsecond.to_number<u16>();

        // h. Let nanosecondMV be ! ToIntegerOrInfinity(nanosecond).
        nanosecond_mv = *nanosecond.to_number<u16>();
    }
    // 17. Else,
    else {
        // a. Let millisecondMV be 0.
        millisecond_mv = 0;

        // b. Let microsecondMV be 0.
        microsecond_mv = 0;

        // c. Let nanosecondMV be 0.
        nanosecond_mv = 0;
    }

    // 18. If IsValidISODate(yearMV, monthMV, dayMV) is false, throw a RangeError exception.
    if (!is_valid_iso_date(year_mv, month_mv, day_mv))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODate);

    // 19. If IsValidTime(hourMV, minuteMV, secondMV, millisecondMV, microsecondMV, nanosecondMV) is false, throw a RangeError exception.
    if (!is_valid_time(hour_mv, minute_mv, second_mv, millisecond_mv, microsecond_mv, nanosecond_mv))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTime);

    // 20. Let timeZoneResult be the Record { [[Z]]: false, [[OffsetString]]: undefined, [[Name]]: undefined }.
    auto time_zone_result = TemporalTimeZone { .z = false, .offset_string = {}, .name = {} };

    // 21. If parseResult contains a TimeZoneIdentifier Parse Node, then
    if (parse_result.time_zone_identifier.has_value()) {
        // a. Let name be the source text matched by the TimeZoneIdentifier Parse Node contained within parseResult.
        auto name = parse_result.time_zone_identifier;

        // b. Set timeZoneResult.[[Name]] to CodePointsToString(name).
        time_zone_result.name = TRY_OR_THROW_OOM(vm, String::from_utf8(*name));
    }

    // 22. If parseResult contains a UTCDesignator Parse Node, then
    if (parse_result.utc_designator.has_value()) {
        // a. Set timeZoneResult.[[Z]] to true.
        time_zone_result.z = true;
    }
    // 23. Else,
    else {
        // a. If parseResult contains a TimeZoneNumericUTCOffset Parse Node, then
        if (parse_result.time_zone_numeric_utc_offset.has_value()) {
            // i. Let offset be the source text matched by the TimeZoneNumericUTCOffset Parse Node contained within parseResult.
            auto offset = parse_result.time_zone_numeric_utc_offset;

            // ii. Set timeZoneResult.[[OffsetString]] to CodePointsToString(offset).
            time_zone_result.offset_string = TRY_OR_THROW_OOM(vm, String::from_utf8(*offset));
        }
    }

    // 23. Let calendar be undefined.
    Optional<String> calendar;

    // 25. For each Annotation Parse Node annotation contained within parseResult, do
    for (auto const& annotation : parse_result.annotations) {
        // a. Let key be the source text matched by the AnnotationKey Parse Node contained within annotation.
        auto const& key = annotation.key;

        // b. If CodePointsToString(key) is "u-ca", then
        if (key == "u-ca"sv) {
            // i. If calendar is undefined, then
            if (!calendar.has_value()) {
                // 1. Let value be the source text matched by the AnnotationValue Parse Node contained within annotation.
                auto const& value = annotation.value;

                // 2. Let calendar be CodePointsToString(value).
                calendar = TRY_OR_THROW_OOM(vm, String::from_utf8(value));
            }
        }
        // c. Else,
        else {
            // i. If annotation contains an AnnotationCriticalFlag Parse Node, throw a RangeError exception.
            if (annotation.critical)
                return vm.throw_completion<RangeError>(ErrorType::TemporalUnknownCriticalAnnotation, key);
        }
    }

    // 26. Return the Record { [[Year]]: yearMV, [[Month]]: monthMV, [[Day]]: dayMV, [[Hour]]: hourMV, [[Minute]]: minuteMV, [[Second]]: secondMV, [[Millisecond]]: millisecondMV, [[Microsecond]]: microsecondMV, [[Nanosecond]]: nanosecondMV, [[TimeZone]]: timeZoneResult, [[Calendar]]: calendar }.
    return ISODateTime { .year = year_mv, .month = month_mv, .day = day_mv, .hour = hour_mv, .minute = minute_mv, .second = second_mv, .millisecond = millisecond_mv, .microsecond = microsecond_mv, .nanosecond = nanosecond_mv, .time_zone = move(time_zone_result), .calendar = move(calendar) };
}

// 13.29 ParseTemporalInstantString ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporalinstantstring
ThrowCompletionOr<TemporalInstant> parse_temporal_instant_string(VM& vm, StringView iso_string)
{
    // 1. If ParseText(StringToCodePoints(isoString), TemporalInstantString) is a List of errors, throw a RangeError exception.
    auto parse_result = parse_iso8601(Production::TemporalInstantString, iso_string);
    if (!parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidInstantString, iso_string);

    // 2. Let result be ? ParseISODateTime(isoString).
    auto result = TRY(parse_iso_date_time(vm, *parse_result));

    // 3. Let offsetString be result.[[TimeZone]].[[OffsetString]].
    auto offset_string = result.time_zone.offset_string;

    // 4. If result.[[TimeZone]].[[Z]] is true, then
    if (result.time_zone.z) {
        // a. Set offsetString to "+00:00".
        offset_string = "+00:00"_string;
    }

    // 6. Assert: offsetString is not undefined.
    VERIFY(offset_string.has_value());

    // 7. Return the Record { [[Year]]: result.[[Year]], [[Month]]: result.[[Month]], [[Day]]: result.[[Day]], [[Hour]]: result.[[Hour]], [[Minute]]: result.[[Minute]], [[Second]]: result.[[Second]], [[Millisecond]]: result.[[Millisecond]], [[Microsecond]]: result.[[Microsecond]], [[Nanosecond]]: result.[[Nanosecond]], [[TimeZoneOffsetString]]: offsetString }.
    return TemporalInstant { .year = result.year, .month = result.month, .day = result.day, .hour = result.hour, .minute = result.minute, .second = result.second, .millisecond = result.millisecond, .microsecond = result.microsecond, .nanosecond = result.nanosecond, .time_zone_offset = move(offset_string) };
}

// 13.30 ParseTemporalZonedDateTimeString ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporalzoneddatetimestring
ThrowCompletionOr<ISODateTime> parse_temporal_zoned_date_time_string(VM& vm, StringView iso_string)
{
    // 1. If ParseText(StringToCodePoints(isoString), TemporalZonedDateTimeString) is a List of errors, throw a RangeError exception.
    auto parse_result = parse_iso8601(Production::TemporalZonedDateTimeString, iso_string);
    if (!parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidZonedDateTimeString, iso_string);

    // 2. Return ? ParseISODateTime(isoString).
    return parse_iso_date_time(vm, *parse_result);
}

// 13.31 ParseTemporalCalendarString ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporalcalendarstring
ThrowCompletionOr<String> parse_temporal_calendar_string(VM& vm, StringView iso_string)
{
    // 1. Let parseResult be Completion(ParseISODateTime(isoString)).
    auto parse_result_completion = parse_iso_date_time(vm, iso_string);

    // 2. If parseResult is a normal completion, then
    if (!parse_result_completion.is_error()) {
        // a. Let calendar be parseResult.[[Value]].[[Calendar]].
        auto calendar = parse_result_completion.value().calendar;

        // b. If calendar is undefined, return "iso8601".
        if (!calendar.has_value())
            return "iso8601"_string;
        // c. Else, return calendar.
        else
            return calendar.release_value();
    }
    // 3. Else,
    else {
        // a. Set parseResult to ParseText(StringToCodePoints(isoString), AnnotationValue).
        auto parse_result = parse_iso8601(Production::AnnotationValue, iso_string);

        // b. If parseResult is a List of errors, throw a RangeError exception.
        if (!parse_result.has_value())
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarString, iso_string);
        // c. Else, return isoString.
        else
            return TRY_OR_THROW_OOM(vm, String::from_utf8(iso_string));
    }
}

// 13.32 ParseTemporalDateString ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporaldatestring
ThrowCompletionOr<TemporalDate> parse_temporal_date_string(VM& vm, StringView iso_string)
{
    // 1. Let parts be ? ParseTemporalDateTimeString(isoString).
    auto parts = TRY(parse_temporal_date_time_string(vm, iso_string));

    // 2. Return the Record { [[Year]]: parts.[[Year]], [[Month]]: parts.[[Month]], [[Day]]: parts.[[Day]], [[Calendar]]: parts.[[Calendar]] }.
    return TemporalDate { .year = parts.year, .month = parts.month, .day = parts.day, .calendar = move(parts.calendar) };
}

// 13.33 ParseTemporalDateTimeString ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporaldatetimestring
ThrowCompletionOr<ISODateTime> parse_temporal_date_time_string(VM& vm, StringView iso_string)
{
    // 1. Let parseResult be ParseText(StringToCodePoints(isoString), TemporalDateTimeString).
    auto parse_result = parse_iso8601(Production::TemporalDateTimeString, iso_string);

    // 2. If parseResult is a List of errors, throw a RangeError exception.
    if (!parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDateTimeString, iso_string);

    // 3. If parseResult contains a UTCDesignator Parse Node, throw a RangeError exception.
    if (parse_result->utc_designator.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDateTimeStringUTCDesignator, iso_string);

    // 4. Return ? ParseISODateTime(isoString).
    return parse_iso_date_time(vm, *parse_result);
}

// 13.34 ParseTemporalDurationString ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporaldurationstring
ThrowCompletionOr<DurationRecord> parse_temporal_duration_string(VM& vm, StringView iso_string)
{
    // 1. Let duration be ParseText(StringToCodePoints(isoString), TemporalDurationString).
    auto parse_result = parse_iso8601(Production::TemporalDurationString, iso_string);

    // 2. If duration is a List of errors, throw a RangeError exception.
    if (!parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDurationString, iso_string);

    // 3. Let each of sign, years, months, weeks, days, hours, fHours, minutes, fMinutes, seconds, and fSeconds be the source text matched by the respective Sign, DurationYears, DurationMonths, DurationWeeks, DurationDays, DurationWholeHours, DurationHoursFraction, DurationWholeMinutes, DurationMinutesFraction, DurationWholeSeconds, and DurationSecondsFraction Parse Node contained within duration, or an empty sequence of code points if not present.
    auto sign_part = parse_result->sign;
    auto years_part = parse_result->duration_years;
    auto months_part = parse_result->duration_months;
    auto weeks_part = parse_result->duration_weeks;
    auto days_part = parse_result->duration_days;
    auto hours_part = parse_result->duration_whole_hours;
    auto f_hours_part = parse_result->duration_hours_fraction;
    auto minutes_part = parse_result->duration_whole_minutes;
    auto f_minutes_part = parse_result->duration_minutes_fraction;
    auto seconds_part = parse_result->duration_whole_seconds;
    auto f_seconds_part = parse_result->duration_seconds_fraction;

    // 4. Let yearsMV be ! ToIntegerOrInfinity(CodePointsToString(years)).
    auto years = years_part.value_or("0"sv).to_number<double>().release_value();

    // 5. Let monthsMV be ! ToIntegerOrInfinity(CodePointsToString(months)).
    auto months = months_part.value_or("0"sv).to_number<double>().release_value();

    // 6. Let weeksMV be ! ToIntegerOrInfinity(CodePointsToString(weeks)).
    auto weeks = weeks_part.value_or("0"sv).to_number<double>().release_value();

    // 7. Let daysMV be ! ToIntegerOrInfinity(CodePointsToString(days)).
    auto days = days_part.value_or("0"sv).to_number<double>().release_value();

    // 8. Let hoursMV be ! ToIntegerOrInfinity(CodePointsToString(hours)).
    auto hours = hours_part.value_or("0"sv).to_number<double>().release_value();

    double minutes;

    // 9. If fHours is not empty, then
    if (f_hours_part.has_value()) {
        // a. If any of minutes, fMinutes, seconds, fSeconds is not empty, throw a RangeError exception.
        if (minutes_part.has_value() || f_minutes_part.has_value() || seconds_part.has_value() || f_seconds_part.has_value())
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDurationStringFractionNotLast, iso_string, "hours"sv, "minutes or seconds"sv);

        // b. Let fHoursDigits be the substring of CodePointsToString(fHours) from 1.
        auto f_hours_digits = f_hours_part->substring_view(1);

        // c. Let fHoursScale be the length of fHoursDigits.
        auto f_hours_scale = (double)f_hours_digits.length();

        // d. Let minutesMV be ! ToIntegerOrInfinity(fHoursDigits) / 10^fHoursScale × 60.
        minutes = f_hours_digits.to_number<double>().release_value() / pow(10., f_hours_scale) * 60;
    }
    // 10. Else,
    else {
        // a. Let minutesMV be ! ToIntegerOrInfinity(CodePointsToString(minutes)).
        minutes = minutes_part.value_or("0"sv).to_number<double>().release_value();
    }

    double seconds;

    // 11. If fMinutes is not empty, then
    if (f_minutes_part.has_value()) {
        // a. If any of seconds, fSeconds is not empty, throw a RangeError exception.
        if (seconds_part.has_value() || f_seconds_part.has_value())
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDurationStringFractionNotLast, iso_string, "minutes"sv, "seconds"sv);

        // b. Let fMinutesDigits be the substring of CodePointsToString(fMinutes) from 1.
        auto f_minutes_digits = f_minutes_part->substring_view(1);

        // c. Let fMinutesScale be the length of fMinutesDigits.
        auto f_minutes_scale = (double)f_minutes_digits.length();

        // d. Let secondsMV be ! ToIntegerOrInfinity(fMinutesDigits) / 10^fMinutesScale × 60.
        seconds = f_minutes_digits.to_number<double>().release_value() / pow(10, f_minutes_scale) * 60;
    }
    // 12. Else if seconds is not empty, then
    else if (seconds_part.has_value()) {
        // a. Let secondsMV be ! ToIntegerOrInfinity(CodePointsToString(seconds)).
        seconds = seconds_part.value_or("0"sv).to_number<double>().release_value();
    }
    // 13. Else,
    else {
        // a. Let secondsMV be remainder(minutesMV, 1) × 60.
        seconds = fmod(minutes, 1) * 60;
    }

    double milliseconds;

    // 14. If fSeconds is not empty, then
    if (f_seconds_part.has_value()) {
        // a. Let fSecondsDigits be the substring of CodePointsToString(fSeconds) from 1.
        auto f_seconds_digits = f_seconds_part->substring_view(1);

        // b. Let fSecondsScale be the length of fSecondsDigits.
        auto f_seconds_scale = (double)f_seconds_digits.length();

        // c. Let millisecondsMV be ! ToIntegerOrInfinity(fSecondsDigits) / 10^fSecondsScale × 1000.
        milliseconds = f_seconds_digits.to_number<double>().release_value() / pow(10, f_seconds_scale) * 1000;
    }
    // 15. Else,
    else {
        // a. Let millisecondsMV be remainder(secondsMV, 1) × 1000.
        milliseconds = fmod(seconds, 1) * 1000;
    }

    // FIXME: This suffers from floating point (im)precision issues - e.g. "PT0.0000001S" ends up
    //        getting parsed as 99.999999 nanoseconds, which is floor()'d to 99 instead of the
    //        expected 100. Oof. This is the reason all of these are suffixed with "MV" in the spec:
    //        mathematical values are not supposed to have this issue.

    // 16. Let microsecondsMV be remainder(millisecondsMV, 1) × 1000.
    auto microseconds = fmod(milliseconds, 1) * 1000;

    // 17. Let nanosecondsMV be remainder(microsecondsMV, 1) × 1000.
    auto nanoseconds = fmod(microseconds, 1) * 1000;

    i8 factor;

    // 18. If sign contains the code point U+002D (HYPHEN-MINUS) or U+2212 (MINUS SIGN), then
    if (sign_part.has_value() && sign_part->is_one_of("-", "\u2212")) {
        // a. Let factor be -1.
        factor = -1;
    }
    // 19. Else,
    else {
        // a. Let factor be 1.
        factor = 1;
    }

    // 20. Return ? CreateDurationRecord(yearsMV × factor, monthsMV × factor, weeksMV × factor, daysMV × factor, hoursMV × factor, floor(minutesMV) × factor, floor(secondsMV) × factor, floor(millisecondsMV) × factor, floor(microsecondsMV) × factor, floor(nanosecondsMV) × factor).
    return create_duration_record(vm, years * factor, months * factor, weeks * factor, days * factor, hours * factor, floor(minutes) * factor, floor(seconds) * factor, floor(milliseconds) * factor, floor(microseconds) * factor, floor(nanoseconds) * factor);
}

// 13.35 ParseTemporalMonthDayString ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporalmonthdaystring
ThrowCompletionOr<TemporalMonthDay> parse_temporal_month_day_string(VM& vm, StringView iso_string)
{
    // 1. Let parseResult be ParseText(StringToCodePoints(isoString), TemporalMonthDayString).
    auto parse_result = parse_iso8601(Production::TemporalMonthDayString, iso_string);

    // 2. If parseResult is a List of errors, throw a RangeError exception.
    if (!parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthDayString, iso_string);

    // 3. If parseResult contains a UTCDesignator Parse Node, throw a RangeError exception.
    if (parse_result->utc_designator.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthDayStringUTCDesignator, iso_string);

    // 4. Let result be ? ParseISODateTime(isoString).
    // NOTE: We must re-parse the string, as MonthDay strings with non-iso8601 calendars are invalid and will cause parse_iso_date_time to throw.
    //       However, the string could be "2022-12-29[u-ca=gregorian]" for example, which is not a MonthDay string but instead a DateTime string and thus should not throw.
    auto result = TRY(parse_iso_date_time(vm, iso_string));

    // 5. Let year be result.[[Year]].
    Optional<i32> year = result.year;

    // 6. If parseResult does not contain a DateYear Parse Node, then
    if (!parse_result->date_year.has_value()) {
        // a. Set year to undefined.
        year = {};
    }

    // 7. Return the Record { [[Year]]: year, [[Month]]: result.[[Month]], [[Day]]: result.[[Day]], [[Calendar]]: result.[[Calendar]] }.
    return TemporalMonthDay { .year = year, .month = result.month, .day = result.day, .calendar = move(result.calendar) };
}

// 13.36 ParseTemporalRelativeToString ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporalrelativetostring
ThrowCompletionOr<ISODateTime> parse_temporal_relative_to_string(VM& vm, StringView iso_string)
{
    // 1. Let parseResult be ParseText(StringToCodePoints(isoString), TemporalDateTimeString).
    auto parse_result = parse_iso8601(Production::TemporalDateTimeString, iso_string);

    // 2. If parseResult is a List of errors, throw a RangeError exception.
    if (!parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDateTimeString, iso_string);

    // 3. If parseResult contains a UTCDesignator ParseNode but no TimeZoneAnnotation Parse Node, throw a RangeError exception.
    if (parse_result->utc_designator.has_value() && !parse_result->time_zone_annotation.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidRelativeToStringUTCDesignatorWithoutBracketedTimeZone, iso_string);

    // 4. Return ? ParseISODateTime(isoString).
    return parse_iso_date_time(vm, *parse_result);
}

// 13.37 ParseTemporalTimeString ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporaltimestring
ThrowCompletionOr<TemporalTime> parse_temporal_time_string(VM& vm, StringView iso_string)
{
    // 1. Let parseResult be ParseText(StringToCodePoints(isoString), TemporalTimeString).
    auto parse_result = parse_iso8601(Production::TemporalTimeString, iso_string);

    // 2. If parseResult is a List of errors, throw a RangeError exception.
    if (!parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeString, iso_string);

    // 3. If parseResult contains a UTCDesignator Parse Node, throw a RangeError exception.
    if (parse_result->utc_designator.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeStringUTCDesignator, iso_string);

    // 4. Let result be ? ParseISODateTime(isoString).
    auto result = TRY(parse_iso_date_time(vm, *parse_result));

    // 5. Return the Record { [[Hour]]: result.[[Hour]], [[Minute]]: result.[[Minute]], [[Second]]: result.[[Second]], [[Millisecond]]: result.[[Millisecond]], [[Microsecond]]: result.[[Microsecond]], [[Nanosecond]]: result.[[Nanosecond]], [[Calendar]]: result.[[Calendar]] }.
    return TemporalTime { .hour = result.hour, .minute = result.minute, .second = result.second, .millisecond = result.millisecond, .microsecond = result.microsecond, .nanosecond = result.nanosecond, .calendar = move(result.calendar) };
}

// 13.38 ParseTemporalTimeZoneString ( timeZoneString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporaltimezonestring
ThrowCompletionOr<TemporalTimeZone> parse_temporal_time_zone_string(VM& vm, StringView time_zone_string)
{
    // 1. Let parseResult be ParseText(StringToCodePoints(timeZoneString), TimeZoneIdentifier).
    auto parse_result = parse_iso8601(Production::TimeZoneIdentifier, time_zone_string);

    // 2. If parseResult is a Parse Node, then
    if (parse_result.has_value()) {
        // a. Return the Record { [[Z]]: false, [[OffsetString]]: undefined, [[Name]]: timeZoneString }.
        return TemporalTimeZone { .z = false, .offset_string = {}, .name = TRY_OR_THROW_OOM(vm, String::from_utf8(time_zone_string)) };
    }

    // 3. Let result be ? ParseISODateTime(timeZoneString).
    auto result = TRY(parse_iso_date_time(vm, time_zone_string));

    // 4. Let timeZoneResult be result.[[TimeZone]].
    auto const& time_zone_result = result.time_zone;

    // 5. If timeZoneResult.[[Z]] is false, timeZoneResult.[[OffsetString]] is undefined, and timeZoneResult.[[Name]] is undefined, throw a RangeError exception.
    if (!time_zone_result.z && !time_zone_result.offset_string.has_value() && !time_zone_result.name.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeZoneString, time_zone_string);

    // 6. Return timeZoneResult.
    return time_zone_result;
}

// 13.39 ParseTemporalYearMonthString ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporalyearmonthstring
ThrowCompletionOr<TemporalYearMonth> parse_temporal_year_month_string(VM& vm, StringView iso_string)
{
    // 1. Let parseResult be ParseText(StringToCodePoints(isoString), TemporalYearMonthString).
    auto parse_result = parse_iso8601(Production::TemporalYearMonthString, iso_string);

    // 2. If parseResult is a List of errors, throw a RangeError exception.
    if (!parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidYearMonthString, iso_string);

    // 3. If parseResult contains a UTCDesignator Parse Node, throw a RangeError exception.
    if (parse_result->utc_designator.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidYearMonthStringUTCDesignator, iso_string);

    // 4. Let result be ? ParseISODateTime(isoString).
    // NOTE: We must re-parse the string, as YearMonth strings with non-iso8601 calendars are invalid and will cause parse_iso_date_time to throw.
    //       However, the string could be "2022-12-29[u-ca=invalid]" for example, which is not a YearMonth string but instead a DateTime string and thus should not throw.
    auto result = TRY(parse_iso_date_time(vm, iso_string));

    // 5. Return the Record { [[Year]]: result.[[Year]], [[Month]]: result.[[Month]], [[Day]]: result.[[Day]], [[Calendar]]: result.[[Calendar]] }.
    return TemporalYearMonth { .year = result.year, .month = result.month, .day = result.day, .calendar = move(result.calendar) };
}

// 13.40 ToPositiveIntegerWithTruncation ( argument ), https://tc39.es/proposal-temporal/#sec-temporal-topositiveintegerwithtruncation
ThrowCompletionOr<double> to_positive_integer_with_truncation(VM& vm, Value argument)
{
    // 1. Let integer be ? ToIntegerWithTruncation(argument).
    auto integer = TRY(to_integer_with_truncation(vm, argument, ErrorType::TemporalPropertyMustBePositiveInteger));

    // 2. If integer ≤ 0, throw a RangeError exception.
    if (integer <= 0) {
        return vm.throw_completion<RangeError>(ErrorType::TemporalPropertyMustBePositiveInteger);
    }

    // 3. Return integer.
    return integer;
}

// 13.43 PrepareTemporalFields ( fields, fieldNames, requiredFields ), https://tc39.es/proposal-temporal/#sec-temporal-preparetemporalfields
ThrowCompletionOr<Object*> prepare_temporal_fields(VM& vm, Object const& fields, Vector<String> const& field_names, Variant<PrepareTemporalFieldsPartial, Vector<StringView>> const& required_fields)
{
    auto& realm = *vm.current_realm();

    // 1. Let result be OrdinaryObjectCreate(null).
    auto result = Object::create(realm, nullptr);
    VERIFY(result);

    // 2. Let any be false.
    auto any = false;

    // 3. For each value property of fieldNames, do
    for (auto& property : field_names) {
        // a. Let value be ? Get(fields, property).
        auto value = TRY(fields.get(property.to_byte_string()));

        // b. If value is not undefined, then
        if (!value.is_undefined()) {
            // i. Set any to true.
            any = true;

            // ii. If property is in the Property column of Table 15 and there is a Conversion value in the same row, then
            // 1. Let Conversion be the Conversion value of the same row.
            // 2. If Conversion is ToIntegerWithTruncation, then
            if (property.is_one_of("year"sv, "hour"sv, "minute"sv, "second"sv, "millisecond"sv, "microsecond"sv, "nanosecond"sv, "eraYear"sv)) {
                // a. Set value to ? ToIntegerWithTruncation(value).
                // b. Set value to 𝔽(value).
                value = Value(TRY(to_integer_with_truncation(vm, value, ErrorType::TemporalPropertyMustBeFinite)));
            }
            // 3. Else if Conversion is ToPositiveIntegerWithTruncation, then
            else if (property.is_one_of("month"sv, "day"sv)) {
                // a. Set value to ? ToPositiveIntegerWithTruncation(value).
                // b. Set value to 𝔽(value).
                value = Value(TRY(to_positive_integer_with_truncation(vm, value)));
            }
            // 4. Else,
            else if (property.is_one_of("monthCode"sv, "offset"sv, "era"sv)) {
                // a. Assert: Conversion is ToString.
                // b. Set value to ? ToString(value).
                value = TRY(value.to_primitive_string(vm));
            }

            // iii. Perform ! CreateDataPropertyOrThrow(result, property, value).
            MUST(result->create_data_property_or_throw(property.to_byte_string(), value));
        }
        // c. Else if requiredFields is a List, then
        else if (required_fields.has<Vector<StringView>>()) {
            // i. If requiredFields contains property, then
            if (required_fields.get<Vector<StringView>>().contains_slow(property)) {
                // 1. Throw a TypeError exception.
                return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, property);
            }
            // ii. If property is in the Property column of Table 13, then
            // NOTE: The other properties in the table are automatically handled as their default value is undefined
            if (property.is_one_of("hour"sv, "minute"sv, "second"sv, "millisecond"sv, "microsecond"sv, "nanosecond"sv)) {
                // 1. Set value to the corresponding Default value of the same row.
                value = Value(0);
            }

            // iii. Perform ! CreateDataPropertyOrThrow(result, property, value).
            MUST(result->create_data_property_or_throw(property.to_byte_string(), value));
        }
    }

    // 4. If requiredFields is partial and any is false, then
    if (required_fields.has<PrepareTemporalFieldsPartial>() && !any) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::TemporalObjectMustHaveOneOf, TRY_OR_THROW_OOM(vm, String::join(", "sv, field_names)));
    }

    // 5. Return result.
    return result.ptr();
}

// 13.44 GetDifferenceSettings ( operation, options, unitGroup, disallowedUnits, fallbackSmallestUnit, smallestLargestDefaultUnit ), https://tc39.es/proposal-temporal/#sec-temporal-getdifferencesettings
ThrowCompletionOr<DifferenceSettings> get_difference_settings(VM& vm, DifferenceOperation operation, Value options_value, UnitGroup unit_group, Vector<StringView> const& disallowed_units, TemporalUnitDefault const& fallback_smallest_unit, StringView smallest_largest_default_unit)
{
    // 1. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, options_value));

    // 2. Let smallestUnit be ? GetTemporalUnit(options, "smallestUnit", unitGroup, fallbackSmallestUnit).
    auto smallest_unit = TRY(get_temporal_unit(vm, *options, vm.names.smallestUnit, unit_group, fallback_smallest_unit));

    // 3. If disallowedUnits contains smallestUnit, throw a RangeError exception.
    if (disallowed_units.contains_slow(*smallest_unit))
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, *smallest_unit, "smallestUnit"sv);

    // 4. Let defaultLargestUnit be ! LargerOfTwoTemporalUnits(smallestLargestDefaultUnit, smallestUnit).
    auto default_largest_unit = larger_of_two_temporal_units(smallest_largest_default_unit, *smallest_unit);

    // 5. Let largestUnit be ? GetTemporalUnit(options, "largestUnit", unitGroup, "auto").
    auto largest_unit = TRY(get_temporal_unit(vm, *options, vm.names.largestUnit, unit_group, { "auto"sv }));

    // 6. If disallowedUnits contains largestUnit, throw a RangeError exception.
    if (disallowed_units.contains_slow(*largest_unit))
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, *largest_unit, "largestUnit"sv);

    // 7. If largestUnit is "auto", set largestUnit to defaultLargestUnit.
    if (largest_unit == "auto"sv)
        largest_unit = TRY_OR_THROW_OOM(vm, String::from_utf8(default_largest_unit));

    // 8. If LargerOfTwoTemporalUnits(largestUnit, smallestUnit) is not largestUnit, throw a RangeError exception.
    if (larger_of_two_temporal_units(*largest_unit, *smallest_unit) != largest_unit)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidUnitRange, *smallest_unit, *largest_unit);

    // 9. Let roundingMode be ? ToTemporalRoundingMode(options, "trunc").
    auto rounding_mode = TRY(to_temporal_rounding_mode(vm, *options, "trunc"sv));

    // 10. If operation is since, then
    if (operation == DifferenceOperation::Since) {
        // a. Set roundingMode to ! NegateTemporalRoundingMode(roundingMode).
        rounding_mode = TRY_OR_THROW_OOM(vm, String::from_utf8(negate_temporal_rounding_mode(rounding_mode)));
    }

    // 11. Let maximum be ! MaximumTemporalDurationRoundingIncrement(smallestUnit).
    auto maximum = maximum_temporal_duration_rounding_increment(*smallest_unit);

    // 12. Let roundingIncrement be ? ToTemporalRoundingIncrement(options).
    auto rounding_increment = TRY(to_temporal_rounding_increment(vm, *options));

    // 13. If maximum is not undefined, perform ? ValidateTemporalRoundingIncrement(roundingIncrement, maximum, false).
    if (maximum.has_value()) {
        TRY(validate_temporal_rounding_increment(vm, rounding_increment, *maximum, false));
    }

    // 14. Return the Record { [[SmallestUnit]]: smallestUnit, [[LargestUnit]]: largestUnit, [[RoundingMode]]: roundingMode, [[RoundingIncrement]]: roundingIncrement, [[Options]]: options }.
    return DifferenceSettings {
        .smallest_unit = smallest_unit.release_value(),
        .largest_unit = largest_unit.release_value(),
        .rounding_mode = move(rounding_mode),
        .rounding_increment = rounding_increment,
        .options = *options,
    };
}

}
