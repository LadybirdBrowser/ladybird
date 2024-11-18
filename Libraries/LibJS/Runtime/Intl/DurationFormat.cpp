/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/DurationFormat.h>
#include <LibJS/Runtime/Intl/ListFormat.h>
#include <LibJS/Runtime/Intl/ListFormatConstructor.h>
#include <LibJS/Runtime/Intl/NumberFormatConstructor.h>
#include <LibJS/Runtime/Intl/PluralRules.h>
#include <LibJS/Runtime/Intl/PluralRulesConstructor.h>
#include <LibJS/Runtime/Intl/RelativeTimeFormat.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(DurationFormat);

// 1 DurationFormat Objects, https://tc39.es/proposal-intl-duration-format/#durationformat-objects
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

DurationFormat::ValueStyle DurationFormat::date_style_from_string(StringView date_style)
{
    if (date_style == "long"sv)
        return ValueStyle::Long;
    if (date_style == "short"sv)
        return ValueStyle::Short;
    if (date_style == "narrow"sv)
        return ValueStyle::Narrow;
    VERIFY_NOT_REACHED();
}

DurationFormat::ValueStyle DurationFormat::time_style_from_string(StringView time_style)
{
    if (time_style == "long"sv)
        return ValueStyle::Long;
    if (time_style == "short"sv)
        return ValueStyle::Short;
    if (time_style == "narrow"sv)
        return ValueStyle::Narrow;
    if (time_style == "numeric"sv)
        return ValueStyle::Numeric;
    if (time_style == "2-digit"sv)
        return ValueStyle::TwoDigit;
    if (time_style == "fractional"sv)
        return ValueStyle::Fractional;
    VERIFY_NOT_REACHED();
}

DurationFormat::ValueStyle DurationFormat::sub_second_style_from_string(StringView sub_second_style)
{
    if (sub_second_style == "long"sv)
        return ValueStyle::Long;
    if (sub_second_style == "short"sv)
        return ValueStyle::Short;
    if (sub_second_style == "narrow"sv)
        return ValueStyle::Narrow;
    if (sub_second_style == "numeric"sv)
        return ValueStyle::Numeric;
    if (sub_second_style == "fractional"sv)
        return ValueStyle::Fractional;
    VERIFY_NOT_REACHED();
}

DurationFormat::Display DurationFormat::display_from_string(StringView display)
{
    if (display == "auto"sv)
        return Display::Auto;
    if (display == "always"sv)
        return Display::Always;
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

// 1.1.3 ToDurationRecord ( input ), https://tc39.es/proposal-intl-duration-format/#sec-todurationrecord
ThrowCompletionOr<DurationRecord> to_duration_record(VM& vm, Value input)
{
    // 1. If Type(input) is not Object, then
    if (!input.is_object()) {
        // a. If Type(input) is String, throw a RangeError exception.
        if (input.is_string())
            return vm.throw_completion<RangeError>(ErrorType::NotAnObject, input);

        // b. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, input);
    }

    auto& input_object = input.as_object();

    // 2. Let result be a new Duration Record with each field set to 0.
    DurationRecord result = {};
    bool any_defined = false;

    auto set_duration_record_value = [&](auto const& name, auto& value_slot) -> ThrowCompletionOr<void> {
        auto value = TRY(input_object.get(name));

        if (!value.is_undefined()) {
            value_slot = TRY(Temporal::to_integer_if_integral(vm, value, ErrorType::TemporalInvalidDurationPropertyValueNonIntegral, name, value));
            any_defined = true;
        }

        return {};
    };

    // 3. Let days be ? Get(input, "days").
    // 4. If days is not undefined, set result.[[Days]] to ? ToIntegerIfIntegral(days).
    TRY(set_duration_record_value(vm.names.days, result.days));

    // 5. Let hours be ? Get(input, "hours").
    // 6. If hours is not undefined, set result.[[Hours]] to ? ToIntegerIfIntegral(hours).
    TRY(set_duration_record_value(vm.names.hours, result.hours));

    // 7. Let microseconds be ? Get(input, "microseconds").
    // 8. If microseconds is not undefined, set result.[[Microseconds]] to ? ToIntegerIfIntegral(microseconds).
    TRY(set_duration_record_value(vm.names.microseconds, result.microseconds));

    // 9. Let milliseconds be ? Get(input, "milliseconds").
    // 10. If milliseconds is not undefined, set result.[[Milliseconds]] to ? ToIntegerIfIntegral(milliseconds).
    TRY(set_duration_record_value(vm.names.milliseconds, result.milliseconds));

    // 11. Let minutes be ? Get(input, "minutes").
    // 12. If minutes is not undefined, set result.[[Minutes]] to ? ToIntegerIfIntegral(minutes).
    TRY(set_duration_record_value(vm.names.minutes, result.minutes));

    // 13. Let months be ? Get(input, "months").
    // 14. If months is not undefined, set result.[[Months]] to ? ToIntegerIfIntegral(months).
    TRY(set_duration_record_value(vm.names.months, result.months));

    // 15. Let nanoseconds be ? Get(input, "nanoseconds").
    // 16. If nanoseconds is not undefined, set result.[[Nanoseconds]] to ? ToIntegerIfIntegral(nanoseconds).
    TRY(set_duration_record_value(vm.names.nanoseconds, result.nanoseconds));

    // 17. Let seconds be ? Get(input, "seconds").
    // 18. If seconds is not undefined, set result.[[Seconds]] to ? ToIntegerIfIntegral(seconds).
    TRY(set_duration_record_value(vm.names.seconds, result.seconds));

    // 19. Let weeks be ? Get(input, "weeks").
    // 20. If weeks is not undefined, set result.[[Weeks]] to ? ToIntegerIfIntegral(weeks).
    TRY(set_duration_record_value(vm.names.weeks, result.weeks));

    // 21. Let years be ? Get(input, "years").
    // 22. If years is not undefined, set result.[[Years]] to ? ToIntegerIfIntegral(years).
    TRY(set_duration_record_value(vm.names.years, result.years));

    // 23. If years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, and nanoseconds are all undefined, throw a TypeError exception.
    if (!any_defined)
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidDurationLikeObject);

    // 24. If IsValidDuration( result.[[Years]], result.[[Months]], result.[[Weeks]], result.[[Days]], result.[[Hours]], result.[[Minutes]], result.[[Seconds]], result.[[Milliseconds]], result.[[Microseconds]], result.[[Nanoseconds]]) is false, then
    if (!Temporal::is_valid_duration(result.years, result.months, result.weeks, result.days, result.hours, result.minutes, result.seconds, result.milliseconds, result.microseconds, result.nanoseconds)) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDurationLikeObject);
    }

    // 25. Return result.
    return result;
}

// 1.1.4 DurationSign ( duration ), https://tc39.es/proposal-intl-duration-format/#sec-durationsign
i8 duration_sign(DurationRecord const& duration)
{
    // 1. For each value v of « duration.[[Years]], duration.[[Months]], duration.[[Weeks]], duration.[[Days]], duration.[[Hours]], duration.[[Minutes]], duration.[[Seconds]], duration.[[Milliseconds]], duration.[[Microseconds]], duration.[[Nanoseconds]] », do
    for (auto value : { duration.years, duration.months, duration.weeks, duration.days, duration.hours, duration.minutes, duration.seconds, duration.milliseconds, duration.microseconds, duration.nanoseconds }) {
        // a. If v < 0, return -1.
        if (value < 0)
            return -1;

        // b. If v > 0, return 1.
        if (value > 0)
            return 1;
    }

    // 2. Return 0.
    return 0;
}

// 1.1.6 GetDurationUnitOptions ( unit, options, baseStyle, stylesList, digitalBase, prevStyle, twoDigitHours ), https://tc39.es/proposal-intl-duration-format/#sec-getdurationunitoptions
ThrowCompletionOr<DurationUnitOptions> get_duration_unit_options(VM& vm, String const& unit, Object const& options, StringView base_style, ReadonlySpan<StringView> styles_list, StringView digital_base, StringView previous_style, bool two_digit_hours)
{
    // 1. Let style be ? GetOption(options, unit, string, stylesList, undefined).
    auto style_value = TRY(get_option(vm, options, unit.to_byte_string(), OptionType::String, styles_list, Empty {}));
    StringView style;

    // 2. Let displayDefault be "always".
    auto display_default = "always"sv;

    // 3. If style is undefined, then
    if (style_value.is_undefined()) {
        // a. If baseStyle is "digital", then
        if (base_style == "digital"sv) {
            // i. If unit is not one of "hours", "minutes", or "seconds", then
            if (!unit.is_one_of("hours"sv, "minutes"sv, "seconds"sv)) {
                // 1. Set displayDefault to "auto".
                display_default = "auto"sv;
            }

            // ii. Set style to digitalBase.
            style = digital_base;
        }
        // b. Else,
        else {
            // i. If prevStyle is "fractional", "numeric" or "2-digit", then
            if (previous_style.is_one_of("fractional"sv, "numeric"sv, "2-digit"sv)) {
                // 1. If unit is not one of "minutes" or "seconds", then
                if (!unit.is_one_of("minutes"sv, "seconds"sv)) {
                    // a. Set displayDefault to "auto".
                    display_default = "auto"sv;
                }

                // 2. Set style to "numeric".
                style = "numeric"sv;
            }
            // ii. Else,
            else {
                // 1. Set displayDefault to "auto".
                display_default = "auto"sv;

                // 2. Set style to baseStyle.
                style = base_style;
            }
        }
    } else {
        style = style_value.as_string().utf8_string_view();
    }

    // 4. If style is "numeric", then
    if (style == "numeric"sv) {
        // a. If unit is one of "milliseconds", "microseconds", or "nanoseconds", then
        if (unit.is_one_of("milliseconds"sv, "microseconds"sv, "nanoseconds"sv)) {
            // i. Set style to "fractional".
            style = "fractional"sv;

            // ii. Set displayDefault to "auto".
            display_default = "auto"sv;
        }
    }

    // 5. Let displayField be the string-concatenation of unit and "Display".
    auto display_field = MUST(String::formatted("{}Display", unit));

    // 6. Let display be ? GetOption(options, displayField, string, « "auto", "always" », displayDefault).
    auto display_value = TRY(get_option(vm, options, display_field.to_byte_string(), OptionType::String, { "auto"sv, "always"sv }, display_default));
    auto display = display_value.as_string().utf8_string();

    // 7. If display is "always" and style is "fractional", then
    if (display == "always"sv && style == "fractional"sv) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::IntlFractionalUnitsMixedWithAlwaysDisplay, unit, display_field);
    }

    // 8. If prevStyle is "fractional", then
    if (previous_style == "fractional"sv) {
        // a. If style is not "fractional", then
        if (style != "fractional"sv) {
            // i. Throw a RangeError exception.
            return vm.throw_completion<RangeError>(ErrorType::IntlFractionalUnitFollowedByNonFractionalUnit, unit);
        }
    }

    // 9. If prevStyle is "numeric" or "2-digit", then
    if (previous_style.is_one_of("numeric"sv, "2-digit"sv)) {
        // a. If style is not "fractional", "numeric" or "2-digit", then
        if (!style.is_one_of("fractional"sv, "numeric"sv, "2-digit"sv)) {
            // i. Throw a RangeError exception.
            return vm.throw_completion<RangeError>(ErrorType::IntlNonNumericOr2DigitAfterNumericOr2Digit);
        }

        // b. If unit is "minutes" or "seconds", then
        if (unit.is_one_of("minutes"sv, "seconds"sv)) {
            // i. Set style to "2-digit".
            style = "2-digit"sv;
        }
    }

    // 10. If unit is "hours" and twoDigitHours is true, then
    if (unit == "hours"sv && two_digit_hours) {
        // a. Set style to "2-digit".
        style = "2-digit"sv;
    }

    // 11. Return the Record { [[Style]]: style, [[Display]]: display  }.
    return DurationUnitOptions { .style = MUST(String::from_utf8(style)), .display = move(display) };
}

// 1.1.7 AddFractionalDigits ( durationFormat, duration ), https://tc39.es/proposal-intl-duration-format/#sec-addfractionaldigits
double add_fractional_digits(DurationFormat const& duration_format, DurationRecord const& duration)
{
    // 1. Let result be 0.
    double result = 0;

    // 2. Let exponent be 3.
    double exponent = 3;

    // 3. For each row of Table 2, except the header row, in table order, do
    for (auto const& duration_instances_component : duration_instances_components) {
        // a. Let style be the value of durationFormat's internal slot whose name is the Style Slot value of the current row.
        auto style = (duration_format.*duration_instances_component.get_style_slot)();

        // b. If style is "fractional", then
        if (style == DurationFormat::ValueStyle::Fractional) {
            // i. Assert: The Unit value of the current row is "milliseconds", "microseconds", or "nanoseconds".
            VERIFY(duration_instances_component.unit.is_one_of("milliseconds"sv, "microseconds"sv, "nanoseconds"sv));

            // ii. Let value be the value of duration's field whose name is the Value Field value of the current row.
            auto value = duration.*duration_instances_component.value_slot;

            // iii. Set value to value / 10^exponent.
            value = value / pow(10, exponent);

            // iv. Set result to result + value.
            result += value;

            // v. Set exponent to exponent + 3.
            exponent += 3;
        }
    }

    // 4. Return result.
    return result;
}

// 1.1.8 NextUnitFractional ( durationFormat, unit ), https://tc39.es/proposal-intl-duration-format/#sec-nextunitfractional
bool next_unit_fractional(DurationFormat const& duration_format, StringView unit)
{
    // 1. Assert: unit is "seconds", "milliseconds", or "microseconds".
    VERIFY(unit.is_one_of("seconds"sv, "milliseconds"sv, "microseconds"sv));

    // 2. If unit is "seconds" and durationFormat.[[MillisecondsStyle]] is "fractional", return true.
    if (unit == "seconds"sv && duration_format.milliseconds_style() == DurationFormat::ValueStyle::Fractional)
        return true;

    // 3. Else if unit is "milliseconds" and durationFormat.[[MicrosecondsStyle]] is "fractional", return true.
    if (unit == "milliseconds"sv && duration_format.microseconds_style() == DurationFormat::ValueStyle::Fractional)
        return true;

    // 4. Else if unit is "microseconds" and durationFormat.[[NanosecondsStyle]] is "fractional", return true.
    if (unit == "microseconds"sv && duration_format.nanoseconds_style() == DurationFormat::ValueStyle::Fractional)
        return true;

    // 5. Return false.
    return false;
}

// 1.1.9 FormatNumericHours ( durationFormat, hoursValue, signDisplayed ), https://tc39.es/proposal-intl-duration-format/#sec-formatnumerichours
Vector<DurationFormatPart> format_numeric_hours(VM& vm, DurationFormat const& duration_format, double hours_value, bool sign_displayed)
{
    auto& realm = *vm.current_realm();

    // 1. Let hoursStyle be durationFormat.[[HoursStyle]].
    auto hours_style = duration_format.hours_style();

    // 2. Assert: hoursStyle is "numeric" or hoursStyle is "2-digit".
    VERIFY(hours_style == DurationFormat::ValueStyle::Numeric || hours_style == DurationFormat::ValueStyle::TwoDigit);

    // 3. Let result be a new empty List.
    Vector<DurationFormatPart> result;

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

    // 10. Let nf be ! Construct(%NumberFormat%, « durationFormat.[[Locale]], nfOpts »).
    auto number_format = construct_number_format(vm, duration_format, number_format_options);

    // 11. Let hoursParts be ! PartitionNumberPattern(nf, hoursValue).
    auto hours_parts = partition_number_pattern(number_format, MathematicalValue { hours_value });

    // 12. For each Record { [[Type]], [[Value]] } part of hoursParts, do
    result.ensure_capacity(hours_parts.size());

    for (auto& part : hours_parts) {
        // a. Append the Record { [[Type]]: part.[[Type]], [[Value]]: part.[[Value]], [[Unit]]: "hour" } to result.
        result.unchecked_append({ .type = part.type, .value = move(part.value), .unit = "hour"sv });
    }

    // 13. Return result.
    return result;
}

// 1.1.10 FormatNumericMinutes ( durationFormat, minutesValue, hoursDisplayed, signDisplayed ), https://tc39.es/proposal-intl-duration-format/#sec-formatnumericminutes
Vector<DurationFormatPart> format_numeric_minutes(VM& vm, DurationFormat const& duration_format, double minutes_value, bool hours_displayed, bool sign_displayed)
{
    auto& realm = *vm.current_realm();

    // 1. Let result be a new empty List.
    Vector<DurationFormatPart> result;

    // 2. If hoursDisplayed is true, then
    if (hours_displayed) {
        // a. Let separator be durationFormat.[[DigitalFormat]].[[HoursMinutesSeparator]].
        auto separator = duration_format.hours_minutes_separator();

        // b. Append the Record { [[Type]]: "literal", [[Value]]: separator, [[Unit]]: empty } to result.
        result.append({ .type = "literal"sv, .value = move(separator), .unit = {} });
    }

    // 3. Let minutesStyle be durationFormat.[[MinutesStyle]].
    auto minutes_style = duration_format.minutes_style();

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

    // 11. Let nf be ! Construct(%NumberFormat%, « durationFormat.[[Locale]], nfOpts »).
    auto number_format = construct_number_format(vm, duration_format, number_format_options);

    // 12. Let minutesParts be ! PartitionNumberPattern(nf, minutesValue).
    auto minutes_parts = partition_number_pattern(number_format, MathematicalValue { minutes_value });

    // 13. For each Record { [[Type]], [[Value]] } part of minutesParts, do
    result.ensure_capacity(result.size() + minutes_parts.size());

    for (auto& part : minutes_parts) {
        // a. Append the Record { [[Type]]: part.[[Type]], [[Value]]: part.[[Value]], [[Unit]]: "minute" } to result.
        result.unchecked_append({ .type = part.type, .value = move(part.value), .unit = "minute"sv });
    }

    // 14. Return result.
    return result;
}

// 1.1.11 FormatNumericSeconds ( durationFormat, secondsValue, minutesDisplayed, signDisplayed ), https://tc39.es/proposal-intl-duration-format/#sec-formatnumericseconds
Vector<DurationFormatPart> format_numeric_seconds(VM& vm, DurationFormat const& duration_format, double seconds_value, bool minutes_displayed, bool sign_displayed)
{
    auto& realm = *vm.current_realm();

    // 1. Let secondsStyle be durationFormat.[[SecondsStyle]].
    auto seconds_style = duration_format.seconds_style();

    // 2. Assert: secondsStyle is "numeric" or secondsStyle is "2-digit".
    VERIFY(seconds_style == DurationFormat::ValueStyle::Numeric || seconds_style == DurationFormat::ValueStyle::TwoDigit);

    // 3. Let result be a new empty List.
    Vector<DurationFormatPart> result;

    // 4. If minutesDisplayed is true, then
    if (minutes_displayed) {
        // a. Let separator be durationFormat.[[DigitalFormat]].[[MinutesSecondsSeparator]].
        auto separator = duration_format.minutes_seconds_separator();

        // b. Append the Record { [[Type]]: "literal", [[Value]]: separator, [[Unit]]: empty } to result.
        result.append({ .type = "literal"sv, .value = move(separator), .unit = {} });
    }

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

    u8 maximum_fraction_digits = 0;
    u8 minimum_fraction_digits = 0;

    // 12. If durationFormat.[[FractionalDigits]] is undefined, then
    if (!duration_format.has_fractional_digits()) {
        // a. Let maximumFractionDigits be 9𝔽.
        maximum_fraction_digits = 9;

        // b. Let minimumFractionDigits be +0𝔽.
        minimum_fraction_digits = 0;
    }
    // 13. Else,
    else {
        // a. Let maximumFractionDigits be durationFormat.[[FractionalDigits]].
        maximum_fraction_digits = duration_format.fractional_digits();

        // b. Let minimumFractionDigits be durationFormat.[[FractionalDigits]].
        minimum_fraction_digits = duration_format.fractional_digits();
    }

    // 14. Perform ! CreateDataPropertyOrThrow(nfOpts, "maximumFractionDigits", maximumFractionDigits).
    MUST(number_format_options->create_data_property_or_throw(vm.names.maximumFractionDigits, Value { maximum_fraction_digits }));

    // 15. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumFractionDigits", minimumFractionDigits).
    MUST(number_format_options->create_data_property_or_throw(vm.names.minimumFractionDigits, Value { minimum_fraction_digits }));

    // 16. Perform ! CreateDataPropertyOrThrow(nfOpts, "roundingMode", "trunc").
    MUST(number_format_options->create_data_property_or_throw(vm.names.roundingMode, PrimitiveString::create(vm, "trunc"sv)));

    // FIXME: We obviously have to create the NumberFormat object after its options are fully initialized.
    //        https://github.com/tc39/proposal-intl-duration-format/pull/203
    // 11. Let nf be ! Construct(%NumberFormat%, « durationFormat.[[Locale]], nfOpts »).
    auto number_format = construct_number_format(vm, duration_format, number_format_options);

    // 17. Let secondsParts be ! PartitionNumberPattern(nf, secondsValue).
    auto seconds_parts = partition_number_pattern(number_format, MathematicalValue { seconds_value });

    // 18. For each Record { [[Type]], [[Value]] } part of secondsParts, do
    result.ensure_capacity(result.size() + seconds_parts.size());

    for (auto& part : seconds_parts) {
        // a. Append the Record { [[Type]]: part.[[Type]], [[Value]]: part.[[Value]], [[Unit]]: "second" } to result.
        result.unchecked_append({ .type = part.type, .value = move(part.value), .unit = "second"sv });
    }

    // 19. Return result.
    return result;
}

// 1.1.12 FormatNumericUnits ( durationFormat, duration, firstNumericUnit, signDisplayed ), https://tc39.es/proposal-intl-duration-format/#sec-formatnumericunits
Vector<DurationFormatPart> format_numeric_units(VM& vm, DurationFormat const& duration_format, DurationRecord const& duration, StringView first_numeric_unit, bool sign_displayed)
{
    // 1. Assert: firstNumericUnit is "hours", "minutes", or "seconds".
    VERIFY(first_numeric_unit.is_one_of("hours"sv, "minutes"sv, "seconds"sv));

    // 2. Let numericPartsList be a new empty List.
    Vector<DurationFormatPart> numeric_parts_list;

    // 3. Let hoursValue be duration.[[Hours]].
    auto hours_value = duration.hours;

    // 4. Let hoursDisplay be durationFormat.[[HoursDisplay]].
    auto hours_display = duration_format.hours_display();

    // 5. Let minutesValue be duration.[[Minutes]].
    auto minutes_value = duration.minutes;

    // 6. Let minutesDisplay be durationFormat.[[MinutesDisplay]].
    auto minutes_display = duration_format.minutes_display();

    // 7. Let secondsValue be duration.[[Seconds]].
    auto seconds_value = duration.seconds;

    // 8. If duration.[[Milliseconds]] is not 0 or duration.[[Microseconds]] is not 0 or duration.[[Nanoseconds]] is not 0, then
    if (duration.milliseconds != 0 || duration.microseconds != 0 || duration.nanoseconds != 0) {
        // a. Set secondsValue to secondsValue + AddFractionalDigits(durationFormat, duration).
        seconds_value += add_fractional_digits(duration_format, duration);
    }

    // 9. Let secondsDisplay be durationFormat.[[SecondsDisplay]].
    auto seconds_display = duration_format.seconds_display();

    // 10. Let hoursFormatted be false.
    auto hours_formatted = false;

    // 11. If firstNumericUnit is "hours", then
    if (first_numeric_unit == "hours"sv) {
        // a. If hoursValue is not 0 or hoursDisplay is not "auto", then
        if (hours_value != 0 || hours_display != DurationFormat::Display::Auto) {
            // i. Set hoursFormatted to true.
            hours_formatted = true;
        }
    }

    // 12. If secondsValue is not 0 or secondsDisplay is not "auto", then
    //     a. Let secondsFormatted be true.
    // 13. Else,
    //     a. Let secondsFormatted be false.
    auto seconds_formatted = seconds_value != 0 || seconds_display != DurationFormat::Display::Auto;

    // 14. Let minutesFormatted be false.
    auto minutes_formatted = false;

    // 15. If firstNumericUnit is "hours" or firstNumericUnit is "minutes", then
    if (first_numeric_unit.is_one_of("hours"sv, "minutes"sv)) {
        // a. If hoursFormatted is true and secondsFormatted is true, then
        if (hours_formatted && seconds_formatted) {
            // i. Set minutesFormatted to true.
            minutes_formatted = true;
        }
        // b. Else if minutesValue is not 0 or minutesDisplay is not "auto", then
        else if (minutes_value != 0 || minutes_display != DurationFormat::Display::Auto) {
            // i. Set minutesFormatted to true.
            minutes_formatted = true;
        }
    }

    // 16. If hoursFormatted is true, then
    if (hours_formatted) {
        // a. If signDisplayed is true, then
        if (sign_displayed) {
            // i. If hoursValue is 0 and DurationSign(duration) is -1, then
            if (hours_value == 0 && duration_sign(duration) == -1) {
                // 1. Set hoursValue to negative-zero.
                hours_value = -0.0;
            }
        }

        // b. Append FormatNumericHours(durationFormat, hoursValue, signDisplayed) to numericPartsList.
        numeric_parts_list.extend(format_numeric_hours(vm, duration_format, hours_value, sign_displayed));

        // c. Set signDisplayed to false.
        sign_displayed = false;
    }

    // 17. If minutesFormatted is true, then
    if (minutes_formatted) {
        // a. If signDisplayed is true, then
        if (sign_displayed) {
            // i. If minutesValue is 0 and DurationSign(duration) is -1, then
            if (minutes_value == 0 && duration_sign(duration) == -1) {
                // 1. Set minutesValue to negative-zero.
                minutes_value = -0.0;
            }
        }

        // b. Append FormatNumericMinutes(durationFormat, minutesValue, hoursFormatted, signDisplayed) to numericPartsList.
        numeric_parts_list.extend(format_numeric_minutes(vm, duration_format, minutes_value, hours_formatted, sign_displayed));

        // c. Set signDisplayed to false.
        sign_displayed = false;
    }

    // 18. If secondsFormatted is true, then
    if (seconds_formatted) {
        // a. Append FormatNumericSeconds(durationFormat, secondsValue, minutesFormatted, signDisplayed) to numericPartsList.
        numeric_parts_list.extend(format_numeric_seconds(vm, duration_format, seconds_value, minutes_formatted, sign_displayed));

        // b. Set signDisplayed to false.
        sign_displayed = false;
    }

    // 19. Return numericPartsList.
    return numeric_parts_list;
}

// 1.1.13 ListFormatParts ( durationFormat, partitionedPartsList ), https://tc39.es/proposal-intl-duration-format/#sec-listformatparts
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

    // 6. Let lf be ! Construct(%ListFormat%, « durationFormat.[[Locale]], lfOpts »).
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

// 1.1.7 PartitionDurationFormatPattern ( durationFormat, duration ), https://tc39.es/proposal-intl-duration-format/#sec-partitiondurationformatpattern
Vector<DurationFormatPart> partition_duration_format_pattern(VM& vm, DurationFormat const& duration_format, DurationRecord const& duration)
{
    auto& realm = *vm.current_realm();

    // 1. Let result be a new empty List.
    Vector<Vector<DurationFormatPart>> result;

    // 2. Let signDisplayed be true.
    auto sign_displayed = true;

    // 3. Let numericUnitFound be false.
    auto numeric_unit_found = false;

    // 4. While numericUnitFound is false, repeat for each row in Table 2 in table order, except the header row:
    for (size_t i = 0; !numeric_unit_found && i < duration_instances_components.size(); ++i) {
        auto const& duration_instances_component = duration_instances_components[i];

        // a. Let value be the value of duration's field whose name is the Value Field value of the current row.
        auto value = duration.*duration_instances_component.value_slot;

        // b. Let style be the value of durationFormat's internal slot whose name is the Style Slot value of the current row.
        auto style = (duration_format.*duration_instances_component.get_style_slot)();

        // c. Let display be the value of durationFormat's internal slot whose name is the Display Slot value of the current row.
        auto display = (duration_format.*duration_instances_component.get_display_slot)();

        // d. Let unit be the Unit value of the current row.
        auto unit = duration_instances_component.unit;

        // e. If style is "numeric" or "2-digit", then
        if (style == DurationFormat::ValueStyle::Numeric || style == DurationFormat::ValueStyle::TwoDigit) {
            // i. Append FormatNumericUnits(durationFormat, duration, unit, signDisplayed) to result.
            result.append(format_numeric_units(vm, duration_format, duration, unit, sign_displayed));

            // ii. Set numericUnitFound to true.
            numeric_unit_found = true;
        }
        // f. Else,
        else {
            // i. Let nfOpts be OrdinaryObjectCreate(null).
            auto number_format_options = Object::create(realm, nullptr);

            // ii. If unit is "seconds", "milliseconds", or "microseconds", then
            if (unit.is_one_of("seconds"sv, "milliseconds"sv, "microseconds"sv)) {
                // 1. If NextUnitFractional(durationFormat, unit) is true, then
                if (next_unit_fractional(duration_format, unit)) {

                    // a. Set value to value + AddFractionalDigits(durationFormat, duration).
                    value += add_fractional_digits(duration_format, duration);

                    u8 maximum_fraction_digits = 0;
                    u8 minimum_fraction_digits = 0;

                    // b. If durationFormat.[[FractionalDigits]] is undefined, then
                    if (!duration_format.has_fractional_digits()) {
                        // a. Let maximumFractionDigits be 9𝔽.
                        maximum_fraction_digits = 9;

                        // b. Let minimumFractionDigits be +0𝔽.
                        minimum_fraction_digits = 0;
                    }
                    // c. Else,
                    else {
                        // a. Let maximumFractionDigits be durationFormat.[[FractionalDigits]].
                        maximum_fraction_digits = duration_format.fractional_digits();

                        // b. Let minimumFractionDigits be durationFormat.[[FractionalDigits]].
                        minimum_fraction_digits = duration_format.fractional_digits();
                    }

                    // d. Perform ! CreateDataPropertyOrThrow(nfOpts, "maximumFractionDigits", maximumFractionDigits).
                    MUST(number_format_options->create_data_property_or_throw(vm.names.maximumFractionDigits, Value { maximum_fraction_digits }));

                    // e. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumFractionDigits", minimumFractionDigits).
                    MUST(number_format_options->create_data_property_or_throw(vm.names.minimumFractionDigits, Value { minimum_fraction_digits }));

                    // f. Perform ! CreateDataPropertyOrThrow(nfOpts, "roundingMode", "trunc").
                    MUST(number_format_options->create_data_property_or_throw(vm.names.roundingMode, PrimitiveString::create(vm, "trunc"sv)));

                    // g. Set numericUnitFound to true.
                    numeric_unit_found = true;
                }
            }

            // iii. If value is not 0 or display is not "auto", then
            if (value != 0 || display != DurationFormat::Display::Auto) {
                // 1. Let numberingSystem be durationFormat.[[NumberingSystem]].
                auto const& numbering_system = duration_format.numbering_system();

                // 2. Perform ! CreateDataPropertyOrThrow(nfOpts, "numberingSystem", numberingSystem).
                MUST(number_format_options->create_data_property_or_throw(vm.names.numberingSystem, PrimitiveString::create(vm, numbering_system)));

                // 3. If signDisplayed is true, then
                if (sign_displayed) {
                    // a. Set signDisplayed to false.
                    sign_displayed = false;

                    // b. If value is 0 and DurationSign(duration) is -1, then
                    if (value == 0 && duration_sign(duration) == -1) {
                        // i. Set value to negative-zero.
                        value = -0.0;
                    }
                }
                // 4. Else,
                else {
                    // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "signDisplay", "never").
                    MUST(number_format_options->create_data_property_or_throw(vm.names.signDisplay, PrimitiveString::create(vm, "never"sv)));
                }

                // 5. Let numberFormatUnit be the NumberFormat Unit value of the current row.
                auto number_format_unit = duration_instances_component.number_format_unit;

                // 6. Perform ! CreateDataPropertyOrThrow(nfOpts, "style", "unit").
                MUST(number_format_options->create_data_property_or_throw(vm.names.style, PrimitiveString::create(vm, "unit"sv)));

                // 7. Perform ! CreateDataPropertyOrThrow(nfOpts, "unit", numberFormatUnit).
                MUST(number_format_options->create_data_property_or_throw(vm.names.unit, PrimitiveString::create(vm, number_format_unit)));

                // 8. Perform ! CreateDataPropertyOrThrow(nfOpts, "unitDisplay", style).
                auto locale_style = Unicode::style_to_string(static_cast<Unicode::Style>(style));
                MUST(number_format_options->create_data_property_or_throw(vm.names.unitDisplay, PrimitiveString::create(vm, locale_style)));

                // 9. Let nf be ! Construct(%NumberFormat%, « durationFormat.[[Locale]], nfOpts »).
                auto number_format = construct_number_format(vm, duration_format, number_format_options);

                // 10. Let parts be ! PartitionNumberPattern(nf, value).
                auto parts = partition_number_pattern(number_format, MathematicalValue { value });

                // 11. Let list be a new empty List.
                Vector<DurationFormatPart> list;

                // 12. For each Record { [[Type]], [[Value]] } part of parts, do
                list.ensure_capacity(parts.size());

                for (auto& part : parts) {
                    // a. Append the Record { [[Type]]: part.[[Type]], [[Value]]: part.[[Value]], [[Unit]]: numberFormatUnit } to list.
                    list.unchecked_append({ .type = part.type, .value = move(part.value), .unit = number_format_unit });
                }

                // 13. Append list to result.
                result.append(list);
            }
        }
    }

    // 5. Return ListFormatParts(durationFormat, result).
    return list_format_parts(vm, duration_format, result);
}

}
