/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/StringBuilder.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/DurationFormat.h>
#include <LibJS/Runtime/Intl/ListFormat.h>
#include <LibJS/Runtime/Intl/ListFormatConstructor.h>
#include <LibJS/Runtime/Intl/MathematicalValue.h>
#include <LibJS/Runtime/Intl/NumberFormatConstructor.h>
#include <LibJS/Runtime/Intl/PluralRules.h>
#include <LibJS/Runtime/Intl/PluralRulesConstructor.h>
#include <LibJS/Runtime/Intl/RelativeTimeFormat.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(DurationFormat);

// 13 DurationFormat Objects, https://tc39.es/ecma402/#durationformat-objects
DurationFormat::DurationFormat(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

DurationFormat::Style DurationFormat::style_from_string(StringView style)
{
    if (style == "long"sv)
        return Style::Long;
    if (style == "short"sv)
        return Style::Short;
    if (style == "narrow"sv)
        return Style::Narrow;
    if (style == "digital"sv)
        return Style::Digital;
    VERIFY_NOT_REACHED();
}

StringView DurationFormat::style_to_string(Style style)
{
    switch (style) {
    case Style::Long:
        return "long"sv;
    case Style::Short:
        return "short"sv;
    case Style::Narrow:
        return "narrow"sv;
    case Style::Digital:
        return "digital"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

DurationFormat::Display DurationFormat::display_from_string(StringView display)
{
    if (display == "auto"sv)
        return Display::Auto;
    if (display == "always"sv)
        return Display::Always;
    VERIFY_NOT_REACHED();
}

DurationFormat::ValueStyle DurationFormat::value_style_from_string(StringView value_style)
{
    if (value_style == "long"sv)
        return ValueStyle::Long;
    if (value_style == "short"sv)
        return ValueStyle::Short;
    if (value_style == "narrow"sv)
        return ValueStyle::Narrow;
    if (value_style == "numeric"sv)
        return ValueStyle::Numeric;
    if (value_style == "2-digit"sv)
        return ValueStyle::TwoDigit;
    if (value_style == "fractional"sv)
        return ValueStyle::Fractional;
    VERIFY_NOT_REACHED();
}

StringView DurationFormat::value_style_to_string(ValueStyle value_style)
{
    switch (value_style) {
    case ValueStyle::Long:
        return "long"sv;
    case ValueStyle::Short:
        return "short"sv;
    case ValueStyle::Narrow:
        return "narrow"sv;
    case ValueStyle::Numeric:
        return "numeric"sv;
    case ValueStyle::TwoDigit:
        return "2-digit"sv;
    case ValueStyle::Fractional:
        return "fractional"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView DurationFormat::display_to_string(Display display)
{
    switch (display) {
    case Display::Auto:
        return "auto"sv;
    case Display::Always:
        return "always"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

static PropertyKey const& unit_to_property_key(VM& vm, DurationFormat::Unit unit)
{
    switch (unit) {
    case DurationFormat::Unit::Years:
        return vm.names.years;
    case DurationFormat::Unit::Months:
        return vm.names.months;
    case DurationFormat::Unit::Weeks:
        return vm.names.weeks;
    case DurationFormat::Unit::Days:
        return vm.names.days;
    case DurationFormat::Unit::Hours:
        return vm.names.hours;
    case DurationFormat::Unit::Minutes:
        return vm.names.minutes;
    case DurationFormat::Unit::Seconds:
        return vm.names.seconds;
    case DurationFormat::Unit::Milliseconds:
        return vm.names.milliseconds;
    case DurationFormat::Unit::Microseconds:
        return vm.names.microseconds;
    case DurationFormat::Unit::Nanoseconds:
        return vm.names.nanoseconds;
    }
    VERIFY_NOT_REACHED();
}

static PropertyKey const& unit_to_number_format_property_key(VM& vm, DurationFormat::Unit unit)
{
    switch (unit) {
    case DurationFormat::Unit::Years:
        return vm.names.year;
    case DurationFormat::Unit::Months:
        return vm.names.month;
    case DurationFormat::Unit::Weeks:
        return vm.names.week;
    case DurationFormat::Unit::Days:
        return vm.names.day;
    case DurationFormat::Unit::Hours:
        return vm.names.hour;
    case DurationFormat::Unit::Minutes:
        return vm.names.minute;
    case DurationFormat::Unit::Seconds:
        return vm.names.second;
    case DurationFormat::Unit::Milliseconds:
        return vm.names.millisecond;
    case DurationFormat::Unit::Microseconds:
        return vm.names.microsecond;
    case DurationFormat::Unit::Nanoseconds:
        return vm.names.nanosecond;
    }
    VERIFY_NOT_REACHED();
}

static GC::Ref<NumberFormat> construct_number_format(VM& vm, DurationFormat const& duration_format, GC::Ref<Object> options)
{
    auto& realm = *vm.current_realm();

    auto number_format = MUST(construct(vm, realm.intrinsics().intl_number_format_constructor(), PrimitiveString::create(vm, duration_format.locale()), options));
    return static_cast<NumberFormat&>(*number_format);
}

static GC::Ref<ListFormat> construct_list_format(VM& vm, DurationFormat const& duration_format, GC::Ref<Object> options)
{
    auto& realm = *vm.current_realm();

    auto list_format = MUST(construct(vm, realm.intrinsics().intl_list_format_constructor(), PrimitiveString::create(vm, duration_format.locale()), options));
    return static_cast<ListFormat&>(*list_format);
}

// 13.5.6.1 ValidateDurationUnitStyle ( unit, style, display, prevStyle ), https://tc39.es/ecma402/#sec-validatedurationunitstyle
// AD-HOC: Our implementation takes extra parameters for better exception messages.
static ThrowCompletionOr<void> validate_duration_unit_style(VM& vm, PropertyKey const& unit, DurationFormat::ValueStyle style, DurationFormat::Display display, Optional<DurationFormat::ValueStyle> previous_style, StringView display_field)
{
    // 1. If display is "always" and style is "fractional", throw a RangeError exception.
    if (display == DurationFormat::Display::Always && style == DurationFormat::ValueStyle::Fractional)
        return vm.throw_completion<RangeError>(ErrorType::IntlFractionalUnitsMixedWithAlwaysDisplay, unit, display_field);

    // 2. If prevStyle is "fractional" and style is not "fractional", throw a RangeError exception.
    if (previous_style == DurationFormat::ValueStyle::Fractional && style != DurationFormat::ValueStyle::Fractional)
        return vm.throw_completion<RangeError>(ErrorType::IntlFractionalUnitFollowedByNonFractionalUnit, unit);

    // 3. If prevStyle is "numeric" or "2-digit" and style is not one of "fractional", "numeric" or "2-digit", throw a RangeError exception.
    if (first_is_one_of(previous_style, DurationFormat::ValueStyle::Numeric, DurationFormat::ValueStyle::TwoDigit)
        && !first_is_one_of(style, DurationFormat::ValueStyle::Fractional, DurationFormat::ValueStyle::Numeric, DurationFormat::ValueStyle::TwoDigit)) {
        return vm.throw_completion<RangeError>(ErrorType::IntlNonNumericOr2DigitAfterNumericOr2Digit);
    }

    // 4. Return unused.
    return {};
}

// 13.5.6 GetDurationUnitOptions ( unit, options, baseStyle, stylesList, digitalBase, prevStyle, twoDigitHours ), https://tc39.es/ecma402/#sec-getdurationunitoptions
ThrowCompletionOr<DurationFormat::DurationUnitOptions> get_duration_unit_options(VM& vm, DurationFormat::Unit unit, Object const& options, DurationFormat::Style base_style, ReadonlySpan<StringView> styles_list, DurationFormat::ValueStyle digital_base, Optional<DurationFormat::ValueStyle> previous_style, bool two_digit_hours)
{
    auto const& unit_property_key = unit_to_property_key(vm, unit);

    // 1. Let style be ? GetOption(options, unit, STRING, stylesList, undefined).
    auto style_value = TRY(get_option(vm, options, unit_property_key, OptionType::String, styles_list, Empty {}));
    DurationFormat::ValueStyle style;

    // 2. Let displayDefault be "always".
    auto display_default = "always"sv;

    // 3. If style is undefined, then
    if (style_value.is_undefined()) {
        // a. If baseStyle is "digital", then
        if (base_style == DurationFormat::Style::Digital) {
            // i. Set style to digitalBase.
            style = digital_base;

            // ii. If unit is not one of "hours", "minutes", or "seconds", set displayDefault to "auto".
            if (!first_is_one_of(unit, DurationFormat::Unit::Hours, DurationFormat::Unit::Minutes, DurationFormat::Unit::Seconds))
                display_default = "auto"sv;
        }
        // b. Else if prevStyle is one of "fractional", "numeric" or "2-digit", then
        else if (first_is_one_of(previous_style, DurationFormat::ValueStyle::Fractional, DurationFormat::ValueStyle::Numeric, DurationFormat::ValueStyle::TwoDigit)) {
            // i. Set style to "numeric".
            style = DurationFormat::ValueStyle::Numeric;

            // ii. If unit is not "minutes" or "seconds", set displayDefault to "auto".
            if (!first_is_one_of(unit, DurationFormat::Unit::Minutes, DurationFormat::Unit::Seconds))
                display_default = "auto"sv;
        }
        // c. Else,
        else {
            // i. Set style to baseStyle.
            style = static_cast<DurationFormat::ValueStyle>(base_style);

            // ii. Set displayDefault to "auto".
            display_default = "auto"sv;
        }
    } else {
        style = DurationFormat::value_style_from_string(style_value.as_string().utf8_string_view());
    }

    // 4. If style is "numeric" and IsFractionalSecondUnitName(unit) is true, then
    if (style == DurationFormat::ValueStyle::Numeric && is_fractional_second_unit_name(unit)) {
        // a. Set style to "fractional".
        style = DurationFormat::ValueStyle::Fractional;

        // b. Set displayDefault to "auto".
        display_default = "auto"sv;
    }

    // 5. Let displayField be the string-concatenation of unit and "Display".
    auto display_field = MUST(String::formatted("{}Display", unit_property_key));

    // 6. Let display be ? GetOption(options, displayField, STRING, « "auto", "always" », displayDefault).
    auto display_value = TRY(get_option(vm, options, display_field, OptionType::String, { "auto"sv, "always"sv }, display_default));
    auto display = DurationFormat::display_from_string(display_value.as_string().utf8_string());

    // 7. Perform ? ValidateDurationUnitStyle(unit, style, display, prevStyle).
    TRY(validate_duration_unit_style(vm, unit_property_key, style, display, previous_style, display_field));

    // 8. If unit is "hours" and twoDigitHours is true, set style to "2-digit".
    if (unit == DurationFormat::Unit::Hours && two_digit_hours)
        style = DurationFormat::ValueStyle::TwoDigit;

    // 9. If unit is "minutes" or "seconds" and prevStyle is "numeric" or "2-digit", set style to "2-digit".
    if (first_is_one_of(unit, DurationFormat::Unit::Minutes, DurationFormat::Unit::Seconds)
        && first_is_one_of(previous_style, DurationFormat::ValueStyle::Numeric, DurationFormat::ValueStyle::TwoDigit)) {
        style = DurationFormat::ValueStyle::TwoDigit;
    }

    // 10. Return the Duration Unit Options Record { [[Style]]: style, [[Display]]: display }.
    return DurationFormat::DurationUnitOptions { .style = style, .display = display };
}

// 13.5.7 ComputeFractionalDigits ( durationFormat, duration ), https://tc39.es/ecma402/#sec-computefractionaldigits
// 15.9.6 ComputeFractionalDigits ( durationFormat, duration ), https://tc39.es/proposal-temporal/#sec-computefractionaldigits
Crypto::BigFraction compute_fractional_digits(DurationFormat const& duration_format, Temporal::Duration const& duration)
{
    // 1. Let result be 0.
    Crypto::BigFraction result;

    // 2. Let exponent be 3.
    double exponent = 3;

    // 3. For each row of Table 24, except the header row, in table order, do
    for (auto const& duration_instances_component : duration_instances_components) {

        // a. Let unitOptions be the value of durationFormat's internal slot whose name is the Internal Slot value of the current row.
        auto unit_options = (duration_format.*duration_instances_component.get_internal_slot)();

        // b. If unitOptions.[[Style]] is "fractional", then
        if (unit_options.style == DurationFormat::ValueStyle::Fractional) {
            // i. Let unit be the Unit value of the current row.
            auto unit = duration_instances_component.unit;

            // ii. Assert: IsFractionalSecondUnitName(unit) is true.
            VERIFY(is_fractional_second_unit_name(unit));

            // iii. Let value be the value of duration's field whose name is the Value Field value of the current row.
            auto value = (duration.*duration_instances_component.value_slot)();

            // iv. Set result to result + (value / 10**exponent).
            result = result + Crypto::BigFraction { Crypto::SignedBigInteger { value }, Crypto::UnsignedBigInteger { pow(10, exponent) } };

            // v. Set exponent to exponent + 3.
            exponent += 3;
        }
    }

    // 4. Return result.
    return result;
}

// 13.5.8 NextUnitFractional ( durationFormat, unit ), https://tc39.es/ecma402/#sec-nextunitfractional
bool next_unit_fractional(DurationFormat const& duration_format, DurationFormat::Unit unit)
{
    // 1. If unit is "seconds" and durationFormat.[[MillisecondsOptions]].[[Style]] is "fractional", return true.
    if (unit == DurationFormat::Unit::Seconds && duration_format.milliseconds_options().style == DurationFormat::ValueStyle::Fractional)
        return true;

    // 2. If unit is "milliseconds" and durationFormat.[[MicrosecondsOptions]].[[Style]] is "fractional", return true.
    if (unit == DurationFormat::Unit::Milliseconds && duration_format.microseconds_options().style == DurationFormat::ValueStyle::Fractional)
        return true;

    // 3. If unit is "microseconds" and durationFormat.[[NanosecondsOptions]].[[Style]] is "fractional", return true.
    if (unit == DurationFormat::Unit::Microseconds && duration_format.nanoseconds_options().style == DurationFormat::ValueStyle::Fractional)
        return true;

    // 4. Return false.
    return false;
}

// 13.5.9 FormatNumericHours ( durationFormat, hoursValue, signDisplayed ), https://tc39.es/ecma402/#sec-formatnumerichours
Vector<DurationFormatPart> format_numeric_hours(VM& vm, DurationFormat const& duration_format, MathematicalValue const& hours_value, bool sign_displayed)
{
    auto& realm = *vm.current_realm();

    // 1. Let result be a new empty List.
    Vector<DurationFormatPart> result;

    // 2. Let hoursStyle be durationFormat.[[HoursOptions]].[[Style]].
    auto hours_style = duration_format.hours_options().style;

    // 3. Assert: hoursStyle is "numeric" or hoursStyle is "2-digit".
    VERIFY(hours_style == DurationFormat::ValueStyle::Numeric || hours_style == DurationFormat::ValueStyle::TwoDigit);

    // 4. Let nfOpts be OrdinaryObjectCreate(null).
    auto number_format_options = Object::create(realm, nullptr);

    // 5. Let numberingSystem be durationFormat.[[NumberingSystem]].
    auto const& numbering_system = duration_format.numbering_system();

    // 6. Perform ! CreateDataPropertyOrThrow(nfOpts, "numberingSystem", numberingSystem).
    MUST(number_format_options->create_data_property_or_throw(vm.names.numberingSystem, PrimitiveString::create(vm, numbering_system)));

    // 7. If hoursStyle is "2-digit", then
    if (hours_style == DurationFormat::ValueStyle::TwoDigit) {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumIntegerDigits", 2𝔽).
        MUST(number_format_options->create_data_property_or_throw(vm.names.minimumIntegerDigits, Value { 2 }));
    }

    // 8. If signDisplayed is false, then
    if (!sign_displayed) {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "signDisplay", "never").
        MUST(number_format_options->create_data_property_or_throw(vm.names.signDisplay, PrimitiveString::create(vm, "never"sv)));
    }

    // 9. Perform ! CreateDataPropertyOrThrow(nfOpts, "useGrouping", false).
    MUST(number_format_options->create_data_property_or_throw(vm.names.useGrouping, Value { false }));

    // 10. Let nf be ! Construct(%Intl.NumberFormat%, « durationFormat.[[Locale]], nfOpts »).
    auto number_format = construct_number_format(vm, duration_format, number_format_options);

    // 11. Let hoursParts be PartitionNumberPattern(nf, hoursValue).
    auto hours_parts = partition_number_pattern(number_format, hours_value);

    // 12. For each Record { [[Type]], [[Value]] } part of hoursParts, do
    result.ensure_capacity(hours_parts.size());

    for (auto& part : hours_parts) {
        // a. Append the Record { [[Type]]: part.[[Type]], [[Value]]: part.[[Value]], [[Unit]]: "hour" } to result.
        result.unchecked_append({ .type = part.type, .value = move(part.value), .unit = "hour"sv });
    }

    // 13. Return result.
    return result;
}

// 13.5.10 FormatNumericMinutes ( durationFormat, minutesValue, hoursDisplayed, signDisplayed ), https://tc39.es/ecma402/#sec-formatnumericminutes
Vector<DurationFormatPart> format_numeric_minutes(VM& vm, DurationFormat const& duration_format, MathematicalValue const& minutes_value, bool hours_displayed, bool sign_displayed)
{
    auto& realm = *vm.current_realm();

    // 1. Let result be a new empty List.
    Vector<DurationFormatPart> result;

    // 2. If hoursDisplayed is true, then
    if (hours_displayed) {
        // a. Let separator be durationFormat.[[HourMinuteSeparator]].
        auto separator = duration_format.hour_minute_separator();

        // b. Append the Record { [[Type]]: "literal", [[Value]]: separator, [[Unit]]: EMPTY } to result.
        result.append({ .type = "literal"sv, .value = move(separator), .unit = {} });
    }

    // 3. Let minutesStyle be durationFormat.[[MinutesOptions]].[[Style]].
    auto minutes_style = duration_format.minutes_options().style;

    // 4. Assert: minutesStyle is "numeric" or minutesStyle is "2-digit".
    VERIFY(minutes_style == DurationFormat::ValueStyle::Numeric || minutes_style == DurationFormat::ValueStyle::TwoDigit);

    // 5. Let nfOpts be OrdinaryObjectCreate(null).
    auto number_format_options = Object::create(realm, nullptr);

    // 6. Let numberingSystem be durationFormat.[[NumberingSystem]].
    auto const& numbering_system = duration_format.numbering_system();

    // 7. Perform ! CreateDataPropertyOrThrow(nfOpts, "numberingSystem", numberingSystem).
    MUST(number_format_options->create_data_property_or_throw(vm.names.numberingSystem, PrimitiveString::create(vm, numbering_system)));

    // 8. If minutesStyle is "2-digit", then
    if (minutes_style == DurationFormat::ValueStyle::TwoDigit) {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumIntegerDigits", 2𝔽).
        MUST(number_format_options->create_data_property_or_throw(vm.names.minimumIntegerDigits, Value { 2 }));
    }

    // 9. If signDisplayed is false, then
    if (!sign_displayed) {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "signDisplay", "never").
        MUST(number_format_options->create_data_property_or_throw(vm.names.signDisplay, PrimitiveString::create(vm, "never"sv)));
    }

    // 10. Perform ! CreateDataPropertyOrThrow(nfOpts, "useGrouping", false).
    MUST(number_format_options->create_data_property_or_throw(vm.names.useGrouping, Value { false }));

    // 11. Let nf be ! Construct(%Intl.NumberFormat%, « durationFormat.[[Locale]], nfOpts »).
    auto number_format = construct_number_format(vm, duration_format, number_format_options);

    // 12. Let minutesParts be PartitionNumberPattern(nf, minutesValue).
    auto minutes_parts = partition_number_pattern(number_format, minutes_value);

    // 13. For each Record { [[Type]], [[Value]] } part of minutesParts, do
    result.ensure_capacity(result.size() + minutes_parts.size());

    for (auto& part : minutes_parts) {
        // a. Append the Record { [[Type]]: part.[[Type]], [[Value]]: part.[[Value]], [[Unit]]: "minute" } to result.
        result.unchecked_append({ .type = part.type, .value = move(part.value), .unit = "minute"sv });
    }

    // 14. Return result.
    return result;
}

// 13.5.11 FormatNumericSeconds ( durationFormat, secondsValue, minutesDisplayed, signDisplayed ), https://tc39.es/ecma402/#sec-formatnumericseconds
Vector<DurationFormatPart> format_numeric_seconds(VM& vm, DurationFormat const& duration_format, MathematicalValue const& seconds_value, bool minutes_displayed, bool sign_displayed)
{
    auto& realm = *vm.current_realm();

    // 1. Let result be a new empty List.
    Vector<DurationFormatPart> result;

    // 2. If minutesDisplayed is true, then
    if (minutes_displayed) {
        // a. Let separator be durationFormat.[[MinuteSecondSeparator]].
        auto separator = duration_format.minute_second_separator();

        // b. Append the Record { [[Type]]: "literal", [[Value]]: separator, [[Unit]]: EMPTY } to result.
        result.append({ .type = "literal"sv, .value = move(separator), .unit = {} });
    }

    // 3. Let secondsStyle be durationFormat.[[SecondsOptions]].[[Style]].
    auto seconds_style = duration_format.seconds_options().style;

    // 4. Assert: secondsStyle is "numeric" or secondsStyle is "2-digit".
    VERIFY(seconds_style == DurationFormat::ValueStyle::Numeric || seconds_style == DurationFormat::ValueStyle::TwoDigit);

    // 5. Let nfOpts be OrdinaryObjectCreate(null).
    auto number_format_options = Object::create(realm, nullptr);

    // 6. Let numberingSystem be durationFormat.[[NumberingSystem]].
    auto const& numbering_system = duration_format.numbering_system();

    // 7. Perform ! CreateDataPropertyOrThrow(nfOpts, "numberingSystem", numberingSystem).
    MUST(number_format_options->create_data_property_or_throw(vm.names.numberingSystem, PrimitiveString::create(vm, numbering_system)));

    // 8. If secondsStyle is "2-digit", then
    if (seconds_style == DurationFormat::ValueStyle::TwoDigit) {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumIntegerDigits", 2𝔽).
        MUST(number_format_options->create_data_property_or_throw(vm.names.minimumIntegerDigits, Value { 2 }));
    }

    // 9. If signDisplayed is false, then
    if (!sign_displayed) {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "signDisplay", "never").
        MUST(number_format_options->create_data_property_or_throw(vm.names.signDisplay, PrimitiveString::create(vm, "never"sv)));
    }

    // 10. Perform ! CreateDataPropertyOrThrow(nfOpts, "useGrouping", false).
    MUST(number_format_options->create_data_property_or_throw(vm.names.useGrouping, Value { false }));

    // 11. Let fractionDigits be durationFormat.[[FractionalDigits]].
    // 12. If fractionDigits is undefined, then
    if (!duration_format.has_fractional_digits()) {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "maximumFractionDigits", 9𝔽).
        MUST(number_format_options->create_data_property_or_throw(vm.names.maximumFractionDigits, Value { 9 }));

        // b. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumFractionDigits", +0𝔽).
        MUST(number_format_options->create_data_property_or_throw(vm.names.minimumFractionDigits, Value { 0 }));
    }
    // 13. Else,
    else {
        auto fraction_digits = duration_format.fractional_digits();

        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "maximumFractionDigits", fractionDigits).
        MUST(number_format_options->create_data_property_or_throw(vm.names.maximumFractionDigits, Value { fraction_digits }));

        // b. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumFractionDigits", fractionDigits).
        MUST(number_format_options->create_data_property_or_throw(vm.names.minimumFractionDigits, Value { fraction_digits }));
    }

    // 14. Perform ! CreateDataPropertyOrThrow(nfOpts, "roundingMode", "trunc").
    MUST(number_format_options->create_data_property_or_throw(vm.names.roundingMode, PrimitiveString::create(vm, "trunc"sv)));

    // 15. Let nf be ! Construct(%Intl.NumberFormat%, « durationFormat.[[Locale]], nfOpts »).
    auto number_format = construct_number_format(vm, duration_format, number_format_options);

    // 16. Let secondsParts be PartitionNumberPattern(nf, secondsValue).
    auto seconds_parts = partition_number_pattern(number_format, seconds_value);

    // 17. For each Record { [[Type]], [[Value]] } part of secondsParts, do
    result.ensure_capacity(result.size() + seconds_parts.size());

    for (auto& part : seconds_parts) {
        // a. Append the Record { [[Type]]: part.[[Type]], [[Value]]: part.[[Value]], [[Unit]]: "second" } to result.
        result.unchecked_append({ .type = part.type, .value = move(part.value), .unit = "second"sv });
    }

    // 18. Return result.
    return result;
}

// 13.5.12 FormatNumericUnits ( durationFormat, duration, firstNumericUnit, signDisplayed ), https://tc39.es/ecma402/#sec-formatnumericunits
// 15.9.7 FormatNumericUnits ( durationFormat, duration, firstNumericUnit, signDisplayed ), https://tc39.es/proposal-temporal/#sec-formatnumericunits
Vector<DurationFormatPart> format_numeric_units(VM& vm, DurationFormat const& duration_format, Temporal::Duration const& duration, DurationFormat::Unit first_numeric_unit, bool sign_displayed)
{
    // 1. Assert: firstNumericUnit is "hours", "minutes", or "seconds".
    VERIFY(first_is_one_of(first_numeric_unit, DurationFormat::Unit::Hours, DurationFormat::Unit::Minutes, DurationFormat::Unit::Seconds));

    // 2. Let numericPartsList be a new empty List.
    Vector<DurationFormatPart> numeric_parts_list;

    // 3. Let hoursValue be duration.[[Hours]].
    auto hours_value = duration.hours();

    // 4. Let hoursDisplay be durationFormat.[[HoursOptions]].[[Display]].
    auto hours_display = duration_format.hours_options().display;

    // 5. Let minutesDisplay be durationFormat.[[MinutesOptions]].[[Display]].
    auto minutes_value = duration.minutes();

    // 6. Let minutesDisplay be durationFormat.[[MinutesDisplay]].
    auto minutes_display = duration_format.minutes_options().display;

    // 7. Let secondsValue be duration.[[Seconds]].
    Crypto::BigFraction seconds_value { duration.seconds() };

    // 8. If duration.[[Milliseconds]] is not 0 or duration.[[Microseconds]] is not 0 or duration.[[Nanoseconds]] is not 0, then
    if (duration.milliseconds() != 0 || duration.microseconds() != 0 || duration.nanoseconds() != 0) {
        // a. Set secondsValue to secondsValue + ComputeFractionalDigits(durationFormat, duration).
        seconds_value = seconds_value + compute_fractional_digits(duration_format, duration);
    }

    // 9. Let secondsDisplay be durationFormat.[[SecondsOptions]].[[Display]].
    auto seconds_display = duration_format.seconds_options().display;

    // 10. Let hoursFormatted be false.
    auto hours_formatted = false;

    // 11. If firstNumericUnit is "hours", then
    if (first_numeric_unit == DurationFormat::Unit::Hours) {
        // a. If hoursValue is not 0 or hoursDisplay is "always", then
        if (hours_value != 0 || hours_display == DurationFormat::Display::Always) {
            // i. Set hoursFormatted to true.
            hours_formatted = true;
        }
    }

    // 12. If secondsValue is not 0 or secondsDisplay is "always", then
    //     a. Let secondsFormatted be true.
    // 13. Else,
    //     a. Let secondsFormatted be false.
    auto seconds_formatted = !seconds_value.is_zero() || seconds_display == DurationFormat::Display::Always;

    // 14. Let minutesFormatted be false.
    auto minutes_formatted = false;

    // 15. If firstNumericUnit is "hours" or firstNumericUnit is "minutes", then
    if (first_is_one_of(first_numeric_unit, DurationFormat::Unit::Hours, DurationFormat::Unit::Minutes)) {
        // a. If hoursFormatted is true and secondsFormatted is true, then
        if (hours_formatted && seconds_formatted) {
            // i. Set minutesFormatted to true.
            minutes_formatted = true;
        }
        // b. Else if minutesValue is not 0 or minutesDisplay is "always", then
        else if (minutes_value != 0 || minutes_display == DurationFormat::Display::Always) {
            // i. Set minutesFormatted to true.
            minutes_formatted = true;
        }
    }

    // 16. If hoursFormatted is true, then
    if (hours_formatted) {
        MathematicalValue hours_mv { hours_value };

        // a. If signDisplayed is true, then
        if (sign_displayed) {
            // i. If hoursValue is 0 and DurationSign(duration) is -1, then
            if (hours_value == 0 && duration_sign(duration) == -1) {
                // 1. Set hoursValue to NEGATIVE-ZERO.
                hours_mv = MathematicalValue { MathematicalValue::Symbol::NegativeZero };
            }
        }

        // b. Let hoursParts be FormatNumericHours(durationFormat, hoursValue, signDisplayed).
        auto hours_parts = format_numeric_hours(vm, duration_format, hours_mv, sign_displayed);

        // b. Set numericPartsList to the list-concatenation of numericPartsList and hoursParts.
        numeric_parts_list.extend(move(hours_parts));

        // c. Set signDisplayed to false.
        sign_displayed = false;
    }

    // 17. If minutesFormatted is true, then
    if (minutes_formatted) {
        MathematicalValue minutes_mv { minutes_value };

        // a. If signDisplayed is true, then
        if (sign_displayed) {
            // i. If minutesValue is 0 and DurationSign(duration) is -1, then
            if (minutes_value == 0 && duration_sign(duration) == -1) {
                // 1. Set minutesValue to NEGATIVE-ZERO.
                minutes_mv = MathematicalValue { MathematicalValue::Symbol::NegativeZero };
            }
        }

        // b. Let minutesParts be FormatNumericMinutes(durationFormat, minutesValue, hoursFormatted, signDisplayed).
        auto minutes_parts = format_numeric_minutes(vm, duration_format, minutes_mv, hours_formatted, sign_displayed);

        // c. Set numericPartsList to the list-concatenation of numericPartsList and minutesParts.
        numeric_parts_list.extend(move(minutes_parts));

        // d. Set signDisplayed to false.
        sign_displayed = false;
    }

    // 18. If secondsFormatted is true, then
    if (seconds_formatted) {
        // a. Let secondsParts be FormatNumericSeconds(durationFormat, secondsValue, minutesFormatted, signDisplayed).
        auto seconds_parts = format_numeric_seconds(vm, duration_format, MathematicalValue { seconds_value.to_string(9) }, minutes_formatted, sign_displayed);

        // b. Set numericPartsList to the list-concatenation of numericPartsList and secondsParts.
        numeric_parts_list.extend(move(seconds_parts));
    }

    // 19. Return numericPartsList.
    return numeric_parts_list;
}

// 13.5.13 IsFractionalSecondUnitName ( unit ), https://tc39.es/ecma402/#sec-isfractionalsecondunitname
bool is_fractional_second_unit_name(DurationFormat::Unit unit)
{
    // 1. If unit is one of "milliseconds", "microseconds", or "nanoseconds", return true.
    // 2. Return false.
    return first_is_one_of(unit, DurationFormat::Unit::Milliseconds, DurationFormat::Unit::Microseconds, DurationFormat::Unit::Nanoseconds);
}

// 13.5.14 ListFormatParts ( durationFormat, partitionedPartsList ), https://tc39.es/ecma402/#sec-listformatparts
Vector<DurationFormatPart> list_format_parts(VM& vm, DurationFormat const& duration_format, Vector<Vector<DurationFormatPart>>& partitioned_parts_list)
{
    auto& realm = *vm.current_realm();

    // 1. Let lfOpts be OrdinaryObjectCreate(null).
    auto list_format_options = Object::create(realm, nullptr);

    // 2. Perform ! CreateDataPropertyOrThrow(lfOpts, "type", "unit").
    MUST(list_format_options->create_data_property_or_throw(vm.names.type, PrimitiveString::create(vm, "unit"sv)));

    // 3. Let listStyle be durationFormat.[[Style]].
    auto list_style = duration_format.style();

    // 4. If listStyle is "digital", then
    if (list_style == DurationFormat::Style::Digital) {
        // a. Set listStyle to "short".
        list_style = DurationFormat::Style::Short;
    }

    // 5. Perform ! CreateDataPropertyOrThrow(lfOpts, "style", listStyle).
    auto locale_list_style = Unicode::style_to_string(static_cast<Unicode::Style>(list_style));
    MUST(list_format_options->create_data_property_or_throw(vm.names.style, PrimitiveString::create(vm, locale_list_style)));

    // 6. Let lf be ! Construct(%Intl.ListFormat%, « durationFormat.[[Locale]], lfOpts »).
    auto list_format = construct_list_format(vm, duration_format, list_format_options);

    // 7. Let strings be a new empty List.
    Vector<String> strings;
    strings.ensure_capacity(partitioned_parts_list.size());

    // 8. For each element parts of partitionedPartsList, do
    for (auto const& parts : partitioned_parts_list) {
        // a. Let string be the empty String.
        StringBuilder string;

        // b. For each Record { [[Type]], [[Value]], [[Unit]] } part in parts, do
        for (auto const& part : parts) {
            // i. Set string to the string-concatenation of string and part.[[Value]].
            string.append(part.value);
        }

        // c. Append string to strings.
        strings.unchecked_append(MUST(string.to_string()));
    }

    // 9. Let formattedPartsList be CreatePartsFromList(lf, strings).
    auto formatted_parts_list = create_parts_from_list(list_format, strings);

    // 10. Let partitionedPartsIndex be 0.
    size_t partitioned_parts_index = 0;

    // 11. Let partitionedLength be the number of elements in partitionedPartsList.
    auto partitioned_length = partitioned_parts_list.size();

    // 12. Let flattenedPartsList be a new empty List.
    Vector<DurationFormatPart> flattened_parts_list;

    // 13. For each Record { [[Type]], [[Value]] } listPart in formattedPartsList, do
    for (auto& list_part : formatted_parts_list) {
        // a. If listPart.[[Type]] is "element", then
        if (list_part.type == "element"sv) {
            // i. Assert: partitionedPartsIndex < partitionedLength.
            VERIFY(partitioned_parts_index < partitioned_length);

            // ii. Let parts be partitionedPartsList[partitionedPartsIndex].
            auto& parts = partitioned_parts_list[partitioned_parts_index];

            // iii. For each Record { [[Type]], [[Value]], [[Unit]] } part in parts, do
            for (auto& part : parts) {
                // 1. Append part to flattenedPartsList.
                flattened_parts_list.append(move(part));
            }

            // iv. Set partitionedPartsIndex to partitionedPartsIndex + 1.
            ++partitioned_parts_index;
        }
        // b. Else,
        else {
            // i. Assert: listPart.[[Type]] is "literal".
            VERIFY(list_part.type == "literal"sv);

            // ii. Append the Record { [[Type]]: "literal", [[Value]]: listPart.[[Value]], [[Unit]]: empty } to flattenedPartsList.
            flattened_parts_list.append({ .type = "literal"sv, .value = move(list_part.value), .unit = {} });
        }
    }

    // 14. Return flattenedPartsList.
    return flattened_parts_list;
}

// 13.5.15 PartitionDurationFormatPattern ( durationFormat, duration ), https://tc39.es/ecma402/#sec-partitiondurationformatpattern
// 15.9.8 PartitionDurationFormatPattern ( durationFormat, duration ), https://tc39.es/proposal-temporal/#sec-formatnumericunits
Vector<DurationFormatPart> partition_duration_format_pattern(VM& vm, DurationFormat const& duration_format, Temporal::Duration const& duration)
{
    auto& realm = *vm.current_realm();

    // 1. Let result be a new empty List.
    Vector<Vector<DurationFormatPart>> result;

    // 2. Let signDisplayed be true.
    auto sign_displayed = true;

    // 3. Let numericUnitFound be false.
    auto numeric_unit_found = false;

    // 4. While numericUnitFound is false, repeat for each row in Table 24 in table order, except the header row:
    for (size_t i = 0; !numeric_unit_found && i < duration_instances_components.size(); ++i) {
        auto const& duration_instances_component = duration_instances_components[i];

        // a. Let value be the value of duration's field whose name is the Value Field value of the current row.
        Crypto::BigFraction value { (duration.*duration_instances_component.value_slot)() };

        // b. Let unitOptions be the value of durationFormat's internal slot whose name is the Internal Slot value of the current row.
        // c. Let style be unitOptions.[[Style]].
        // d. Let display be unitOptions.[[Display]].
        auto [style, display] = (duration_format.*duration_instances_component.get_internal_slot)();

        // e. Let unit be the Unit value of the current row.
        auto unit = duration_instances_component.unit;

        // f. Let numberFormatUnit be the NumberFormat Unit value of the current row.
        auto const& number_format_unit = unit_to_number_format_property_key(vm, duration_instances_component.unit);

        // g. If style is "numeric" or "2-digit", then
        if (style == DurationFormat::ValueStyle::Numeric || style == DurationFormat::ValueStyle::TwoDigit) {
            // i. Let numericPartsList be FormatNumericUnits(durationFormat, duration, unit, signDisplayed).
            auto numeric_parts_list = format_numeric_units(vm, duration_format, duration, unit, sign_displayed);

            // ii. If numericPartsList is not empty, append numericPartsList to result.
            if (!numeric_parts_list.is_empty())
                result.append(move(numeric_parts_list));

            // iii. Set numericUnitFound to true.
            numeric_unit_found = true;
        }
        // h. Else,
        else {
            // i. Let nfOpts be OrdinaryObjectCreate(null).
            auto number_format_options = Object::create(realm, nullptr);

            // ii. If NextUnitFractional(durationFormat, unit) is true, then
            if (next_unit_fractional(duration_format, unit)) {
                // 1. Set value to value + ComputeFractionalDigits(durationFormat, duration).
                value = value + compute_fractional_digits(duration_format, duration);

                // 2. Let fractionDigits be durationFormat.[[FractionalDigits]].
                // 3. If fractionDigits is undefined, then
                if (!duration_format.has_fractional_digits()) {
                    // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "maximumFractionDigits", 9𝔽).
                    MUST(number_format_options->create_data_property_or_throw(vm.names.maximumFractionDigits, Value { 9 }));

                    // b. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumFractionDigits", +0𝔽).
                    MUST(number_format_options->create_data_property_or_throw(vm.names.minimumFractionDigits, Value { 0 }));
                }
                // 4. Else,
                else {
                    auto fraction_digits = duration_format.fractional_digits();

                    // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "maximumFractionDigits", fractionDigits).
                    MUST(number_format_options->create_data_property_or_throw(vm.names.maximumFractionDigits, Value { fraction_digits }));

                    // b. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumFractionDigits", fractionDigits).
                    MUST(number_format_options->create_data_property_or_throw(vm.names.minimumFractionDigits, Value { fraction_digits }));
                }

                // 5. Perform ! CreateDataPropertyOrThrow(nfOpts, "roundingMode", "trunc").
                MUST(number_format_options->create_data_property_or_throw(vm.names.roundingMode, PrimitiveString::create(vm, "trunc"sv)));

                // 6. Set numericUnitFound to true.
                numeric_unit_found = true;
            }

            // iii. If display is "always" or value is not 0, then
            if (display == DurationFormat::Display::Always || !value.is_zero()) {
                MathematicalValue value_mv { value.to_string(9) };

                // 1. Perform ! CreateDataPropertyOrThrow(nfOpts, "numberingSystem", durationFormat.[[NumberingSystem]]).
                MUST(number_format_options->create_data_property_or_throw(vm.names.numberingSystem, PrimitiveString::create(vm, duration_format.numbering_system())));

                // 2. If signDisplayed is true, then
                if (sign_displayed) {
                    // a. Set signDisplayed to false.
                    sign_displayed = false;

                    // b. If value is 0 and DurationSign(duration) is -1, set value to NEGATIVE-ZERO.
                    if (value.is_zero() && duration_sign(duration) == -1)
                        value_mv = MathematicalValue { MathematicalValue::Symbol::NegativeZero };
                }
                // 3. Else,
                else {
                    // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "signDisplay", "never").
                    MUST(number_format_options->create_data_property_or_throw(vm.names.signDisplay, PrimitiveString::create(vm, "never"sv)));
                }

                // 3. Perform ! CreateDataPropertyOrThrow(nfOpts, "style", "unit").
                MUST(number_format_options->create_data_property_or_throw(vm.names.style, PrimitiveString::create(vm, "unit"sv)));

                // 4. Perform ! CreateDataPropertyOrThrow(nfOpts, "unit", numberFormatUnit).
                MUST(number_format_options->create_data_property_or_throw(vm.names.unit, PrimitiveString::create(vm, number_format_unit.as_string())));

                // 5. Perform ! CreateDataPropertyOrThrow(nfOpts, "unitDisplay", style).
                auto locale_style = Unicode::style_to_string(static_cast<Unicode::Style>(style));
                MUST(number_format_options->create_data_property_or_throw(vm.names.unitDisplay, PrimitiveString::create(vm, locale_style)));

                // 6. Let nf be ! Construct(%Intl.NumberFormat%, « durationFormat.[[Locale]], nfOpts »).
                auto number_format = construct_number_format(vm, duration_format, number_format_options);

                // 7. Let parts be PartitionNumberPattern(nf, value).
                auto parts = partition_number_pattern(number_format, value_mv);

                // 8. Let list be a new empty List.
                Vector<DurationFormatPart> list;

                // 10. For each Record { [[Type]], [[Value]] } part of parts, do
                list.ensure_capacity(parts.size());

                for (auto& part : parts) {
                    // a. Append the Record { [[Type]]: part.[[Type]], [[Value]]: part.[[Value]], [[Unit]]: numberFormatUnit } to list.
                    list.unchecked_append({ .type = part.type, .value = move(part.value), .unit = number_format_unit.as_string() });
                }

                // 11. Append list to result.
                result.append(list);
            }
        }
    }

    // 5. Return ListFormatParts(durationFormat, result).
    return list_format_parts(vm, duration_format, result);
}

}
