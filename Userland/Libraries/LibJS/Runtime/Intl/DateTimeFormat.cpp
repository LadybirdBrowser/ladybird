/*
 * Copyright (c) 2021-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Find.h>
#include <AK/IterationDecision.h>
#include <AK/NumericLimits.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16View.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Intl/DateTimeFormat.h>
#include <LibJS/Runtime/Intl/NumberFormat.h>
#include <LibJS/Runtime/Intl/NumberFormatConstructor.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Utf16String.h>
#include <LibLocale/Locale.h>
#include <LibLocale/NumberFormat.h>
#include <math.h>

namespace JS::Intl {

JS_DEFINE_ALLOCATOR(DateTimeFormat);

static Crypto::SignedBigInteger const s_one_million_bigint { 1'000'000 };

// 11 DateTimeFormat Objects, https://tc39.es/ecma402/#datetimeformat-objects
DateTimeFormat::DateTimeFormat(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

void DateTimeFormat::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_bound_format)
        visitor.visit(m_bound_format);
}

DateTimeFormat::Style DateTimeFormat::style_from_string(StringView style)
{
    if (style == "full"sv)
        return Style::Full;
    if (style == "long"sv)
        return Style::Long;
    if (style == "medium"sv)
        return Style::Medium;
    if (style == "short"sv)
        return Style::Short;
    VERIFY_NOT_REACHED();
}

StringView DateTimeFormat::style_to_string(Style style)
{
    switch (style) {
    case Style::Full:
        return "full"sv;
    case Style::Long:
        return "long"sv;
    case Style::Medium:
        return "medium"sv;
    case Style::Short:
        return "short"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

// 11.5.1 DateTimeStyleFormat ( dateStyle, timeStyle, styles ), https://tc39.es/ecma402/#sec-date-time-style-format
Optional<::Locale::CalendarPattern> date_time_style_format(StringView data_locale, DateTimeFormat& date_time_format)
{
    ::Locale::CalendarPattern time_format {};
    ::Locale::CalendarPattern date_format {};

    auto get_pattern = [&](auto type, auto style) -> Optional<::Locale::CalendarPattern> {
        auto formats = ::Locale::get_calendar_format(data_locale, date_time_format.calendar(), type);

        if (formats.has_value()) {
            switch (style) {
            case DateTimeFormat::Style::Full:
                return formats->full_format;
            case DateTimeFormat::Style::Long:
                return formats->long_format;
            case DateTimeFormat::Style::Medium:
                return formats->medium_format;
            case DateTimeFormat::Style::Short:
                return formats->short_format;
            }
        }

        return {};
    };

    // 1. If timeStyle is not undefined, then
    if (date_time_format.has_time_style()) {
        // a. Assert: timeStyle is one of "full", "long", "medium", or "short".
        // b. Let timeFormat be styles.[[TimeFormat]].[[<timeStyle>]].
        auto pattern = get_pattern(::Locale::CalendarFormatType::Time, date_time_format.time_style());
        if (!pattern.has_value())
            return {};

        time_format = pattern.release_value();
    }

    // 2. If dateStyle is not undefined, then
    if (date_time_format.has_date_style()) {
        // a. Assert: dateStyle is one of "full", "long", "medium", or "short".
        // b. Let dateFormat be styles.[[DateFormat]].[[<dateStyle>]].
        auto pattern = get_pattern(::Locale::CalendarFormatType::Date, date_time_format.date_style());
        if (!pattern.has_value())
            return {};

        date_format = pattern.release_value();
    }

    // 3. If dateStyle is not undefined and timeStyle is not undefined, then
    if (date_time_format.has_date_style() && date_time_format.has_time_style()) {
        // a. Let format be a new Record.
        ::Locale::CalendarPattern format {};

        // b. Add to format all fields from dateFormat except [[pattern]] and [[rangePatterns]].
        format.for_each_calendar_field_zipped_with(date_format, [](auto& format_field, auto const& date_format_field, auto) {
            format_field = date_format_field;
        });

        // c. Add to format all fields from timeFormat except [[pattern]], [[rangePatterns]], [[pattern12]], and [[rangePatterns12]], if present.
        format.for_each_calendar_field_zipped_with(time_format, [](auto& format_field, auto const& time_format_field, auto) {
            if (time_format_field.has_value())
                format_field = time_format_field;
        });

        // d. Let connector be styles.[[DateTimeFormat]].[[<dateStyle>]].
        auto connector = get_pattern(::Locale::CalendarFormatType::DateTime, date_time_format.date_style());
        if (!connector.has_value())
            return {};

        // e. Let pattern be the string connector with the substring "{0}" replaced with timeFormat.[[pattern]] and the substring "{1}" replaced with dateFormat.[[pattern]].
        auto pattern = MUST(connector->pattern.replace("{0}"sv, time_format.pattern, ReplaceMode::FirstOnly));
        pattern = MUST(pattern.replace("{1}"sv, date_format.pattern, ReplaceMode::FirstOnly));

        // f. Set format.[[pattern]] to pattern.
        format.pattern = move(pattern);

        // g. If timeFormat has a [[pattern12]] field, then
        if (time_format.pattern12.has_value()) {
            // i. Let pattern12 be the string connector with the substring "{0}" replaced with timeFormat.[[pattern12]] and the substring "{1}" replaced with dateFormat.[[pattern]].
            auto pattern12 = MUST(connector->pattern.replace("{0}"sv, *time_format.pattern12, ReplaceMode::FirstOnly));
            pattern12 = MUST(pattern12.replace("{1}"sv, date_format.pattern, ReplaceMode::FirstOnly));

            // ii. Set format.[[pattern12]] to pattern12.
            format.pattern12 = move(pattern12);
        }

        // NOTE: Our implementation of steps h-j differ from the spec. LibUnicode does not attach range patterns to the
        //       format pattern; rather, lookups for range patterns are performed separately based on the format pattern's
        //       skeleton. So we form a new skeleton here and defer the range pattern lookups.
        format.skeleton = ::Locale::combine_skeletons(date_format.skeleton, time_format.skeleton);

        // k. Return format.
        return format;
    }

    // 4. If timeStyle is not undefined, then
    if (date_time_format.has_time_style()) {
        // a. Return timeFormat.
        return time_format;
    }

    // 5. Assert: dateStyle is not undefined.
    VERIFY(date_time_format.has_date_style());

    // 6. Return dateFormat.
    return date_format;
}

// 11.5.2 BasicFormatMatcher ( options, formats ), https://tc39.es/ecma402/#sec-basicformatmatcher
Optional<::Locale::CalendarPattern> basic_format_matcher(::Locale::CalendarPattern const& options, Vector<::Locale::CalendarPattern> formats)
{
    // 1. Let removalPenalty be 120.
    constexpr int removal_penalty = 120;

    // 2. Let additionPenalty be 20.
    constexpr int addition_penalty = 20;

    // 3. Let longLessPenalty be 8.
    constexpr int long_less_penalty = 8;

    // 4. Let longMorePenalty be 6.
    constexpr int long_more_penalty = 6;

    // 5. Let shortLessPenalty be 6.
    constexpr int short_less_penalty = 6;

    // 6. Let shortMorePenalty be 3.
    constexpr int short_more_penalty = 3;

    // 7. Let offsetPenalty be 1.
    constexpr int offset_penalty = 1;

    // 8. Let bestScore be -Infinity.
    int best_score = NumericLimits<int>::min();

    // 9. Let bestFormat be undefined.
    Optional<::Locale::CalendarPattern> best_format;

    // 10. Assert: Type(formats) is List.
    // 11. For each element format of formats, do
    for (auto& format : formats) {
        // a. Let score be 0.
        int score = 0;

        // b. For each property name property shown in Table 6, do
        format.for_each_calendar_field_zipped_with(options, [&](auto const& format_prop, auto const& options_prop, auto type) {
            using ValueType = typename RemoveReference<decltype(options_prop)>::ValueType;

            // i. If options has a field [[<property>]], let optionsProp be options.[[<property>]]; else let optionsProp be undefined.
            // ii. If format has a field [[<property>]], let formatProp be format.[[<property>]]; else let formatProp be undefined.

            // iii. If optionsProp is undefined and formatProp is not undefined, decrease score by additionPenalty.
            if (!options_prop.has_value() && format_prop.has_value()) {
                score -= addition_penalty;
            }
            // iv. Else if optionsProp is not undefined and formatProp is undefined, decrease score by removalPenalty.
            else if (options_prop.has_value() && !format_prop.has_value()) {
                score -= removal_penalty;
            }
            // v. Else if property is "timeZoneName", then
            else if (type == ::Locale::CalendarPattern::Field::TimeZoneName) {
                // This is needed to avoid a compile error. Although we only enter this branch for TimeZoneName,
                // the lambda we are in will be generated with property types other than CalendarPatternStyle.
                auto compare_prop = [](auto prop, auto test) { return prop == static_cast<ValueType>(test); };

                // 1. If optionsProp is "short" or "shortGeneric", then
                if (compare_prop(options_prop, ::Locale::CalendarPatternStyle::Short) || compare_prop(options_prop, ::Locale::CalendarPatternStyle::ShortGeneric)) {
                    // a. If formatProp is "shortOffset", decrease score by offsetPenalty.
                    if (compare_prop(format_prop, ::Locale::CalendarPatternStyle::ShortOffset))
                        score -= offset_penalty;
                    // b. Else if formatProp is "longOffset", decrease score by (offsetPenalty + shortMorePenalty).
                    else if (compare_prop(format_prop, ::Locale::CalendarPatternStyle::LongOffset))
                        score -= offset_penalty + short_more_penalty;
                    // c. Else if optionsProp is "short" and formatProp is "long", decrease score by shortMorePenalty.
                    else if (compare_prop(options_prop, ::Locale::CalendarPatternStyle::Short) || compare_prop(format_prop, ::Locale::CalendarPatternStyle::Long))
                        score -= short_more_penalty;
                    // d. Else if optionsProp is "shortGeneric" and formatProp is "longGeneric", decrease score by shortMorePenalty.
                    else if (compare_prop(options_prop, ::Locale::CalendarPatternStyle::ShortGeneric) || compare_prop(format_prop, ::Locale::CalendarPatternStyle::LongGeneric))
                        score -= short_more_penalty;
                    // e. Else if optionsProp ≠ formatProp, decrease score by removalPenalty.
                    else if (options_prop != format_prop)
                        score -= removal_penalty;
                }
                // 2. Else if optionsProp is "shortOffset" and formatProp is "longOffset", decrease score by shortMorePenalty.
                else if (compare_prop(options_prop, ::Locale::CalendarPatternStyle::ShortOffset) || compare_prop(format_prop, ::Locale::CalendarPatternStyle::LongOffset)) {
                    score -= short_more_penalty;
                }
                // 3. Else if optionsProp is "long" or "longGeneric", then
                else if (compare_prop(options_prop, ::Locale::CalendarPatternStyle::Long) || compare_prop(options_prop, ::Locale::CalendarPatternStyle::LongGeneric)) {
                    // a. If formatProp is "longOffset", decrease score by offsetPenalty.
                    if (compare_prop(format_prop, ::Locale::CalendarPatternStyle::LongOffset))
                        score -= offset_penalty;
                    // b. Else if formatProp is "shortOffset", decrease score by (offsetPenalty + longLessPenalty).
                    else if (compare_prop(format_prop, ::Locale::CalendarPatternStyle::ShortOffset))
                        score -= offset_penalty + long_less_penalty;
                    // c. Else if optionsProp is "long" and formatProp is "short", decrease score by longLessPenalty.
                    else if (compare_prop(options_prop, ::Locale::CalendarPatternStyle::Long) || compare_prop(format_prop, ::Locale::CalendarPatternStyle::Short))
                        score -= long_less_penalty;
                    // d. Else if optionsProp is "longGeneric" and formatProp is "shortGeneric", decrease score by longLessPenalty.
                    else if (compare_prop(options_prop, ::Locale::CalendarPatternStyle::LongGeneric) || compare_prop(format_prop, ::Locale::CalendarPatternStyle::ShortGeneric))
                        score -= long_less_penalty;
                    // e. Else if optionsProp ≠ formatProp, decrease score by removalPenalty.
                    else if (options_prop != format_prop)
                        score -= removal_penalty;
                }
                // 4. Else if optionsProp is "longOffset" and formatProp is "shortOffset", decrease score by longLessPenalty.
                else if (compare_prop(options_prop, ::Locale::CalendarPatternStyle::LongOffset) || compare_prop(format_prop, ::Locale::CalendarPatternStyle::ShortOffset)) {
                    score -= long_less_penalty;
                }
                // 5. Else if optionsProp ≠ formatProp, decrease score by removalPenalty.
                else if (options_prop != format_prop) {
                    score -= removal_penalty;
                }
            }
            // vi. Else if optionsProp ≠ formatProp, then
            else if (options_prop != format_prop) {
                using ValuesType = Conditional<IsIntegral<ValueType>, AK::Array<u8, 3>, AK::Array<::Locale::CalendarPatternStyle, 5>>;
                ValuesType values {};

                // 1. If property is "fractionalSecondDigits", then
                if constexpr (IsIntegral<ValueType>) {
                    // a. Let values be « 1𝔽, 2𝔽, 3𝔽 ».
                    values = { 1, 2, 3 };
                }
                // 2. Else,
                else {
                    // a. Let values be « "2-digit", "numeric", "narrow", "short", "long" ».
                    values = {
                        ::Locale::CalendarPatternStyle::TwoDigit,
                        ::Locale::CalendarPatternStyle::Numeric,
                        ::Locale::CalendarPatternStyle::Narrow,
                        ::Locale::CalendarPatternStyle::Short,
                        ::Locale::CalendarPatternStyle::Long,
                    };
                }

                // 3. Let optionsPropIndex be the index of optionsProp within values.
                auto options_prop_index = static_cast<int>(find_index(values.begin(), values.end(), *options_prop));

                // 4. Let formatPropIndex be the index of formatProp within values.
                auto format_prop_index = static_cast<int>(find_index(values.begin(), values.end(), *format_prop));

                // 5. Let delta be max(min(formatPropIndex - optionsPropIndex, 2), -2).
                int delta = max(min(format_prop_index - options_prop_index, 2), -2);

                // 6. If delta = 2, decrease score by longMorePenalty.
                if (delta == 2)
                    score -= long_more_penalty;
                // 7. Else if delta = 1, decrease score by shortMorePenalty.
                else if (delta == 1)
                    score -= short_more_penalty;
                // 8. Else if delta = -1, decrease score by shortLessPenalty.
                else if (delta == -1)
                    score -= short_less_penalty;
                // 9. Else if delta = -2, decrease score by longLessPenalty.
                else if (delta == -2)
                    score -= long_less_penalty;
            }
        });

        // c. If score > bestScore, then
        if (score > best_score) {
            // i. Let bestScore be score.
            best_score = score;

            // ii. Let bestFormat be format.
            best_format = format;
        }
    }

    if (!best_format.has_value())
        return {};

    // Non-standard, if the user provided options that differ from the best format's options, keep
    // the user's options. This is expected by TR-35:
    //
    //     It is not necessary to supply dateFormatItems with skeletons for every field length; fields
    //     in the skeleton and pattern are expected to be expanded in parallel to handle a request.
    //     https://unicode.org/reports/tr35/tr35-dates.html#Matching_Skeletons
    //
    // Rather than generating an prohibitively large amount of nearly-duplicate patterns, which only
    // differ by field length, we expand the field lengths here.
    best_format->for_each_calendar_field_zipped_with(options, [&](auto& best_format_field, auto const& option_field, auto field_type) {
        switch (field_type) {
        case ::Locale::CalendarPattern::Field::FractionalSecondDigits:
            if ((best_format_field.has_value() || best_format->second.has_value()) && option_field.has_value())
                best_format_field = option_field;
            break;

        case ::Locale::CalendarPattern::Field::Hour:
        case ::Locale::CalendarPattern::Field::Minute:
        case ::Locale::CalendarPattern::Field::Second:
            break;

        default:
            if (best_format_field.has_value() && option_field.has_value())
                best_format_field = option_field;
            break;
        }
    });

    // 12. Return bestFormat.
    return best_format;
}

// 11.5.3 BestFitFormatMatcher ( options, formats ), https://tc39.es/ecma402/#sec-bestfitformatmatcher
Optional<::Locale::CalendarPattern> best_fit_format_matcher(::Locale::CalendarPattern const& options, Vector<::Locale::CalendarPattern> formats)
{
    // When the BestFitFormatMatcher abstract operation is called with two arguments options and formats, it performs
    // implementation dependent steps, which should return a set of component representations that a typical user of
    // the selected locale would perceive as at least as good as the one returned by BasicFormatMatcher.
    return basic_format_matcher(options, move(formats));
}

struct StyleAndValue {
    StringView name {};
    ::Locale::CalendarPatternStyle style {};
    i32 value { 0 };
};

static Optional<StyleAndValue> find_calendar_field(StringView name, ::Locale::CalendarPattern const& options, ::Locale::CalendarPattern const* range_options, LocalTime const& local_time)
{
    auto make_style_and_value = [](auto name, auto style, auto fallback_style, auto value) {
        if (style.has_value())
            return StyleAndValue { name, *style, static_cast<i32>(value) };
        return StyleAndValue { name, fallback_style, static_cast<i32>(value) };
    };

    constexpr auto weekday = "weekday"sv;
    constexpr auto era = "era"sv;
    constexpr auto year = "year"sv;
    constexpr auto month = "month"sv;
    constexpr auto day = "day"sv;
    constexpr auto hour = "hour"sv;
    constexpr auto minute = "minute"sv;
    constexpr auto second = "second"sv;

    Optional<::Locale::CalendarPatternStyle> empty;

    if (name == weekday)
        return make_style_and_value(weekday, range_options ? range_options->weekday : empty, *options.weekday, local_time.weekday);
    if (name == era)
        return make_style_and_value(era, range_options ? range_options->era : empty, *options.era, local_time.era);
    if (name == year)
        return make_style_and_value(year, range_options ? range_options->year : empty, *options.year, local_time.year);
    if (name == month)
        return make_style_and_value(month, range_options ? range_options->month : empty, *options.month, local_time.month);
    if (name == day)
        return make_style_and_value(day, range_options ? range_options->day : empty, *options.day, local_time.day);
    if (name == hour)
        return make_style_and_value(hour, range_options ? range_options->hour : empty, *options.hour, local_time.hour);
    if (name == minute)
        return make_style_and_value(minute, range_options ? range_options->minute : empty, *options.minute, local_time.minute);
    if (name == second)
        return make_style_and_value(second, range_options ? range_options->second : empty, *options.second, local_time.second);
    return {};
}

static Optional<StringView> resolve_day_period(StringView locale, StringView calendar, ::Locale::CalendarPatternStyle style, ReadonlySpan<PatternPartition> pattern_parts, LocalTime local_time)
{
    // Use the "noon" day period if the locale has it, but only if the time is either exactly 12:00.00 or would be displayed as such.
    if (local_time.hour == 12) {
        auto it = find_if(pattern_parts.begin(), pattern_parts.end(), [&](auto const& part) {
            if (part.type == "minute"sv && local_time.minute != 0)
                return true;
            if (part.type == "second"sv && local_time.second != 0)
                return true;
            if (part.type == "fractionalSecondDigits"sv && local_time.millisecond != 0)
                return true;
            return false;
        });

        if (it == pattern_parts.end()) {
            auto noon_symbol = ::Locale::get_calendar_day_period_symbol(locale, calendar, style, ::Locale::DayPeriod::Noon);
            if (noon_symbol.has_value())
                return *noon_symbol;
        }
    }

    return ::Locale::get_calendar_day_period_symbol_for_hour(locale, calendar, style, local_time.hour);
}

// 11.5.5 FormatDateTimePattern ( dateTimeFormat, patternParts, x, rangeFormatOptions ), https://tc39.es/ecma402/#sec-formatdatetimepattern
ThrowCompletionOr<Vector<PatternPartition>> format_date_time_pattern(VM& vm, DateTimeFormat& date_time_format, Vector<PatternPartition> pattern_parts, double time, ::Locale::CalendarPattern const* range_format_options)
{
    auto& realm = *vm.current_realm();

    // 1. Let x be TimeClip(x).
    time = time_clip(time);

    // 2. If x is NaN, throw a RangeError exception.
    if (isnan(time))
        return vm.throw_completion<RangeError>(ErrorType::IntlInvalidTime);

    // 3. Let locale be dateTimeFormat.[[Locale]].
    auto const& locale = date_time_format.locale();
    auto const& data_locale = date_time_format.data_locale();

    auto construct_number_format = [&](auto& options) -> ThrowCompletionOr<NumberFormat*> {
        auto number_format = TRY(construct(vm, realm.intrinsics().intl_number_format_constructor(), PrimitiveString::create(vm, locale), options));
        return static_cast<NumberFormat*>(number_format.ptr());
    };

    // 4. Let nfOptions be OrdinaryObjectCreate(null).
    auto number_format_options = Object::create(realm, nullptr);

    // 5. Perform ! CreateDataPropertyOrThrow(nfOptions, "useGrouping", false).
    MUST(number_format_options->create_data_property_or_throw(vm.names.useGrouping, Value(false)));

    // 6. Let nf be ? Construct(%NumberFormat%, « locale, nfOptions »).
    auto* number_format = TRY(construct_number_format(number_format_options));

    // 7. Let nf2Options be OrdinaryObjectCreate(null).
    auto number_format_options2 = Object::create(realm, nullptr);

    // 8. Perform ! CreateDataPropertyOrThrow(nf2Options, "minimumIntegerDigits", 2).
    MUST(number_format_options2->create_data_property_or_throw(vm.names.minimumIntegerDigits, Value(2)));

    // 9. Perform ! CreateDataPropertyOrThrow(nf2Options, "useGrouping", false).
    MUST(number_format_options2->create_data_property_or_throw(vm.names.useGrouping, Value(false)));

    // 10. Let nf2 be ? Construct(%NumberFormat%, « locale, nf2Options »).
    auto* number_format2 = TRY(construct_number_format(number_format_options2));

    // 11. Let fractionalSecondDigits be dateTimeFormat.[[FractionalSecondDigits]].
    Optional<u8> fractional_second_digits;
    NumberFormat* number_format3 = nullptr;

    // 12. If fractionalSecondDigits is not undefined, then
    if (date_time_format.has_fractional_second_digits()) {
        fractional_second_digits = date_time_format.fractional_second_digits();

        // a. Let nf3Options be OrdinaryObjectCreate(null).
        auto number_format_options3 = Object::create(realm, nullptr);

        // b. Perform ! CreateDataPropertyOrThrow(nf3Options, "minimumIntegerDigits", fractionalSecondDigits).
        MUST(number_format_options3->create_data_property_or_throw(vm.names.minimumIntegerDigits, Value(*fractional_second_digits)));

        // c. Perform ! CreateDataPropertyOrThrow(nf3Options, "useGrouping", false).
        MUST(number_format_options3->create_data_property_or_throw(vm.names.useGrouping, Value(false)));

        // d. Let nf3 be ? Construct(%NumberFormat%, « locale, nf3Options »).
        number_format3 = TRY(construct_number_format(number_format_options3));
    }

    // 13. Let tm be ToLocalTime(ℤ(ℝ(x) × 10^6), dateTimeFormat.[[Calendar]], dateTimeFormat.[[TimeZone]]).
    auto time_bigint = Crypto::SignedBigInteger { time }.multiplied_by(s_one_million_bigint);
    auto local_time = TRY(to_local_time(vm, time_bigint, date_time_format.calendar(), date_time_format.time_zone()));

    // 14. Let result be a new empty List.
    Vector<PatternPartition> result;

    // 15. For each Record { [[Type]], [[Value]] } patternPart in patternParts, do
    for (auto& pattern_part : pattern_parts) {
        // a. Let p be patternPart.[[Type]].
        auto part = pattern_part.type;

        // b. If p is "literal", then
        if (part == "literal"sv) {
            // i. Append a new Record { [[Type]]: "literal", [[Value]]: patternPart.[[Value]] } as the last element of the list result.
            result.append({ "literal"sv, move(pattern_part.value) });
        }

        // c. Else if p is equal to "fractionalSecondDigits", then
        else if (part == "fractionalSecondDigits"sv) {
            // i. Let v be tm.[[Millisecond]].
            auto value = local_time.millisecond;

            // ii. Let v be floor(v × 10^(fractionalSecondDigits - 3)).
            value = floor(value * pow(10, static_cast<int>(*fractional_second_digits) - 3));

            // iii. Let fv be FormatNumeric(nf3, v).
            auto formatted_value = format_numeric(*number_format3, Value(value));

            // iv. Append a new Record { [[Type]]: "fractionalSecond", [[Value]]: fv } as the last element of result.
            result.append({ "fractionalSecond"sv, move(formatted_value) });
        }

        // d. Else if p is equal to "dayPeriod", then
        else if (part == "dayPeriod"sv) {
            String formatted_value;

            // i. Let f be the value of dateTimeFormat's internal slot whose name is the Internal Slot column of the matching row.
            auto style = date_time_format.day_period();

            // ii. Let fv be a String value representing the day period of tm in the form given by f; the String value depends upon the implementation and the effective locale of dateTimeFormat.
            auto symbol = resolve_day_period(data_locale, date_time_format.calendar(), style, pattern_parts, local_time);
            if (symbol.has_value())
                formatted_value = MUST(String::from_utf8(*symbol));

            // iii. Append a new Record { [[Type]]: p, [[Value]]: fv } as the last element of the list result.
            result.append({ "dayPeriod"sv, move(formatted_value) });
        }

        // e. Else if p is equal to "timeZoneName", then
        else if (part == "timeZoneName"sv) {
            // i. Let f be dateTimeFormat.[[TimeZoneName]].
            auto style = date_time_format.time_zone_name();

            // ii. Let v be dateTimeFormat.[[TimeZone]].
            auto const& value = date_time_format.time_zone();

            // iii. Let fv be a String value representing v in the form given by f; the String value depends upon the implementation and the effective locale of dateTimeFormat.
            //      The String value may also depend on the value of the [[InDST]] field of tm if f is "short", "long", "shortOffset", or "longOffset".
            //      If the implementation does not have a localized representation of f, then use the String value of v itself.
            auto formatted_value = ::Locale::format_time_zone(data_locale, value, style, local_time.time_since_epoch());

            // iv. Append a new Record { [[Type]]: p, [[Value]]: fv } as the last element of the list result.
            result.append({ "timeZoneName"sv, move(formatted_value) });
        }

        // f. Else if p matches a Property column of the row in Table 6, then
        else if (auto style_and_value = find_calendar_field(part, date_time_format, range_format_options, local_time); style_and_value.has_value()) {
            String formatted_value;

            // i. If rangeFormatOptions is not undefined, let f be the value of rangeFormatOptions's field whose name matches p.
            // ii. Else, let f be the value of dateTimeFormat's internal slot whose name is the Internal Slot column of the matching row.
            // NOTE: find_calendar_field handles resolving rangeFormatOptions and dateTimeFormat fields.
            auto style = style_and_value->style;

            // iii. Let v be the value of tm's field whose name is the Internal Slot column of the matching row.
            auto value = style_and_value->value;

            // iv. If p is "year" and v ≤ 0, let v be 1 - v.
            if ((part == "year"sv) && (value <= 0))
                value = 1 - value;

            // v. If p is "month", increase v by 1.
            if (part == "month"sv)
                ++value;

            if (part == "hour"sv) {
                auto hour_cycle = date_time_format.hour_cycle();

                // vi. If p is "hour" and dateTimeFormat.[[HourCycle]] is "h11" or "h12", then
                if ((hour_cycle == ::Locale::HourCycle::H11) || (hour_cycle == ::Locale::HourCycle::H12)) {
                    // 1. Let v be v modulo 12.
                    value = value % 12;

                    // 2. If v is 0 and dateTimeFormat.[[HourCycle]] is "h12", let v be 12.
                    if ((value == 0) && (hour_cycle == ::Locale::HourCycle::H12))
                        value = 12;
                }

                // vii. If p is "hour" and dateTimeFormat.[[HourCycle]] is "h24", then
                if (hour_cycle == ::Locale::HourCycle::H24) {
                    // 1. If v is 0, let v be 24.
                    if (value == 0)
                        value = 24;
                }
            }

            switch (style) {
            // viii. If f is "numeric", then
            case ::Locale::CalendarPatternStyle::Numeric:
                // 1. Let fv be FormatNumeric(nf, v).
                formatted_value = format_numeric(*number_format, Value(value));
                break;

            // ix. Else if f is "2-digit", then
            case ::Locale::CalendarPatternStyle::TwoDigit:
                // 1. Let fv be FormatNumeric(nf2, v).
                formatted_value = format_numeric(*number_format2, Value(value));

                // 2. If the "length" property of fv is greater than 2, let fv be the substring of fv containing the last two characters.
                // NOTE: The first length check here isn't enough, but lets us avoid UTF-16 transcoding when the formatted value is ASCII.
                if (formatted_value.bytes_as_string_view().length() > 2) {
                    auto utf16_formatted_value = Utf16String::create(formatted_value);
                    if (utf16_formatted_value.length_in_code_units() > 2)
                        formatted_value = MUST(utf16_formatted_value.substring_view(utf16_formatted_value.length_in_code_units() - 2).to_utf8());
                }

                break;

            // x. Else if f is "narrow", "short", or "long", then let fv be a String value representing v in the form given by f; the String value depends upon the implementation and the effective locale and calendar of dateTimeFormat.
            //    If p is "month" and rangeFormatOptions is undefined, then the String value may also depend on whether dateTimeFormat.[[Day]] is undefined.
            //    If p is "month" and rangeFormatOptions is not undefined, then the String value may also depend on whether rangeFormatOptions.[[day]] is undefined.
            //    If p is "era" and rangeFormatOptions is undefined, then the String value may also depend on whether dateTimeFormat.[[Era]] is undefined.
            //    If p is "era" and rangeFormatOptions is not undefined, then the String value may also depend on whether rangeFormatOptions.[[era]] is undefined.
            //    If the implementation does not have a localized representation of f, then use the String value of v itself.
            case ::Locale::CalendarPatternStyle::Narrow:
            case ::Locale::CalendarPatternStyle::Short:
            case ::Locale::CalendarPatternStyle::Long: {
                Optional<StringView> symbol;

                if (part == "era"sv)
                    symbol = ::Locale::get_calendar_era_symbol(data_locale, date_time_format.calendar(), style, static_cast<::Locale::Era>(value));
                else if (part == "month"sv)
                    symbol = ::Locale::get_calendar_month_symbol(data_locale, date_time_format.calendar(), style, static_cast<::Locale::Month>(value - 1));
                else if (part == "weekday"sv)
                    symbol = ::Locale::get_calendar_weekday_symbol(data_locale, date_time_format.calendar(), style, static_cast<::Locale::Weekday>(value));

                if (symbol.has_value())
                    formatted_value = MUST(String::from_utf8(*symbol));
                else
                    formatted_value = MUST(String::number(value));

                break;
            }

            default:
                VERIFY_NOT_REACHED();
            }

            // xi. Append a new Record { [[Type]]: p, [[Value]]: fv } as the last element of the list result.
            result.append({ style_and_value->name, move(formatted_value) });
        }

        // g. Else if p is equal to "ampm", then
        else if (part == "ampm"sv) {
            String formatted_value;

            // i. Let v be tm.[[Hour]].
            auto value = local_time.hour;

            // ii. If v is greater than 11, then
            if (value > 11) {
                // 1. Let fv be an implementation and locale dependent String value representing "post meridiem".
                auto symbol = ::Locale::get_calendar_day_period_symbol(data_locale, date_time_format.calendar(), ::Locale::CalendarPatternStyle::Short, ::Locale::DayPeriod::PM);
                formatted_value = MUST(String::from_utf8(symbol.value_or("PM"sv)));
            }
            // iii. Else,
            else {
                // 1. Let fv be an implementation and locale dependent String value representing "ante meridiem".
                auto symbol = ::Locale::get_calendar_day_period_symbol(data_locale, date_time_format.calendar(), ::Locale::CalendarPatternStyle::Short, ::Locale::DayPeriod::AM);
                formatted_value = MUST(String::from_utf8(symbol.value_or("AM"sv)));
            }

            // iv. Append a new Record { [[Type]]: "dayPeriod", [[Value]]: fv } as the last element of the list result.
            result.append({ "dayPeriod"sv, move(formatted_value) });
        }

        // h. Else if p is equal to "relatedYear", then
        else if (part == "relatedYear"sv) {
            // i. Let v be tm.[[RelatedYear]].
            // ii. Let fv be FormatNumeric(nf, v).
            // iii. Append a new Record { [[Type]]: "relatedYear", [[Value]]: fv } as the last element of the list result.

            // FIXME: Implement this when relatedYear is supported.
        }

        // i. Else if p is equal to "yearName", then
        else if (part == "yearName"sv) {
            // i. Let v be tm.[[YearName]].
            // ii. Let fv be an implementation and locale dependent String value representing v.
            // iii. Append a new Record { [[Type]]: "yearName", [[Value]]: fv } as the last element of the list result.

            // FIXME: Implement this when yearName is supported.
        }

        // Non-standard, TR-35 requires the decimal separator before injected {fractionalSecondDigits} partitions
        // to adhere to the selected locale. This depends on other generated data, so it is deferred to here.
        else if (part == "decimal"sv) {
            auto decimal_symbol = ::Locale::get_number_system_symbol(data_locale, date_time_format.numbering_system(), ::Locale::NumericSymbol::Decimal).value_or("."sv);
            result.append({ "literal"sv, MUST(String::from_utf8(decimal_symbol)) });
        }

        // j. Else,
        else {
            // i. Let unknown be an implementation-, locale-, and numbering system-dependent String based on x and p.
            // ii. Append a new Record { [[Type]]: "unknown", [[Value]]: unknown } as the last element of result.

            // LibUnicode doesn't generate any "unknown" patterns.
            VERIFY_NOT_REACHED();
        }
    }

    // 16. Return result.
    return result;
}

// 11.5.6 PartitionDateTimePattern ( dateTimeFormat, x ), https://tc39.es/ecma402/#sec-partitiondatetimepattern
ThrowCompletionOr<Vector<PatternPartition>> partition_date_time_pattern(VM& vm, DateTimeFormat& date_time_format, double time)
{
    // 1. Let patternParts be PartitionPattern(dateTimeFormat.[[Pattern]]).
    auto pattern_parts = partition_pattern(date_time_format.pattern());

    // 2. Let result be ? FormatDateTimePattern(dateTimeFormat, patternParts, x, undefined).
    auto result = TRY(format_date_time_pattern(vm, date_time_format, move(pattern_parts), time, nullptr));

    // 3. Return result.
    return result;
}

// 11.5.7 FormatDateTime ( dateTimeFormat, x ), https://tc39.es/ecma402/#sec-formatdatetime
ThrowCompletionOr<String> format_date_time(VM& vm, DateTimeFormat& date_time_format, double time)
{
    // 1. Let parts be ? PartitionDateTimePattern(dateTimeFormat, x).
    auto parts = TRY(partition_date_time_pattern(vm, date_time_format, time));

    // 2. Let result be the empty String.
    StringBuilder result;

    // 3. For each Record { [[Type]], [[Value]] } part in parts, do
    for (auto& part : parts) {
        // a. Set result to the string-concatenation of result and part.[[Value]].
        result.append(part.value);
    }

    // 4. Return result.
    return MUST(result.to_string());
}

// 11.5.8 FormatDateTimeToParts ( dateTimeFormat, x ), https://tc39.es/ecma402/#sec-formatdatetimetoparts
ThrowCompletionOr<NonnullGCPtr<Array>> format_date_time_to_parts(VM& vm, DateTimeFormat& date_time_format, double time)
{
    auto& realm = *vm.current_realm();

    // 1. Let parts be ? PartitionDateTimePattern(dateTimeFormat, x).
    auto parts = TRY(partition_date_time_pattern(vm, date_time_format, time));

    // 2. Let result be ! ArrayCreate(0).
    auto result = MUST(Array::create(realm, 0));

    // 3. Let n be 0.
    size_t n = 0;

    // 4. For each Record { [[Type]], [[Value]] } part in parts, do
    for (auto& part : parts) {
        // a. Let O be OrdinaryObjectCreate(%Object.prototype%).
        auto object = Object::create(realm, realm.intrinsics().object_prototype());

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        MUST(object->create_data_property_or_throw(vm.names.type, PrimitiveString::create(vm, part.type)));

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        MUST(object->create_data_property_or_throw(vm.names.value, PrimitiveString::create(vm, move(part.value))));

        // d. Perform ! CreateDataProperty(result, ! ToString(n), O).
        MUST(result->create_data_property_or_throw(n, object));

        // e. Increment n by 1.
        ++n;
    }

    // 5. Return result.
    return result;
}

template<typename Callback>
void for_each_range_pattern_field(LocalTime const& time1, LocalTime const& time2, Callback&& callback)
{
    // Table 4: Range pattern fields, https://tc39.es/ecma402/#table-datetimeformat-rangepatternfields
    if (callback(static_cast<u8>(time1.era), static_cast<u8>(time2.era), ::Locale::CalendarRangePattern::Field::Era) == IterationDecision::Break)
        return;
    if (callback(time1.year, time2.year, ::Locale::CalendarRangePattern::Field::Year) == IterationDecision::Break)
        return;
    if (callback(time1.month, time2.month, ::Locale::CalendarRangePattern::Field::Month) == IterationDecision::Break)
        return;
    if (callback(time1.day, time2.day, ::Locale::CalendarRangePattern::Field::Day) == IterationDecision::Break)
        return;
    if (callback(time1.hour, time2.hour, ::Locale::CalendarRangePattern::Field::AmPm) == IterationDecision::Break)
        return;
    if (callback(time1.hour, time2.hour, ::Locale::CalendarRangePattern::Field::DayPeriod) == IterationDecision::Break)
        return;
    if (callback(time1.hour, time2.hour, ::Locale::CalendarRangePattern::Field::Hour) == IterationDecision::Break)
        return;
    if (callback(time1.minute, time2.minute, ::Locale::CalendarRangePattern::Field::Minute) == IterationDecision::Break)
        return;
    if (callback(time1.second, time2.second, ::Locale::CalendarRangePattern::Field::Second) == IterationDecision::Break)
        return;
    if (callback(time1.millisecond, time2.millisecond, ::Locale::CalendarRangePattern::Field::FractionalSecondDigits) == IterationDecision::Break)
        return;
}

template<typename Callback>
static ThrowCompletionOr<void> for_each_range_pattern_with_source(::Locale::CalendarRangePattern& pattern, Callback&& callback)
{
    TRY(callback(pattern.start_range, "startRange"sv));
    TRY(callback(pattern.separator, "shared"sv));
    TRY(callback(pattern.end_range, "endRange"sv));
    return {};
}

// 11.5.9 PartitionDateTimeRangePattern ( dateTimeFormat, x, y ), https://tc39.es/ecma402/#sec-partitiondatetimerangepattern
ThrowCompletionOr<Vector<PatternPartitionWithSource>> partition_date_time_range_pattern(VM& vm, DateTimeFormat& date_time_format, double start, double end)
{
    // 1. Let x be TimeClip(x).
    start = time_clip(start);

    // 2. If x is NaN, throw a RangeError exception.
    if (isnan(start))
        return vm.throw_completion<RangeError>(ErrorType::IntlInvalidTime);

    // 3. Let y be TimeClip(y).
    end = time_clip(end);

    // 4. If y is NaN, throw a RangeError exception.
    if (isnan(end))
        return vm.throw_completion<RangeError>(ErrorType::IntlInvalidTime);

    // 5. Let tm1 be ToLocalTime(ℤ(ℝ(x) × 10^6), dateTimeFormat.[[Calendar]], dateTimeFormat.[[TimeZone]]).
    auto start_bigint = Crypto::SignedBigInteger { start }.multiplied_by(s_one_million_bigint);
    auto start_local_time = TRY(to_local_time(vm, start_bigint, date_time_format.calendar(), date_time_format.time_zone()));

    // 6. Let tm2 be ToLocalTime(ℤ(ℝ(y) × 10^6), dateTimeFormat.[[Calendar]], dateTimeFormat.[[TimeZone]]).
    auto end_bigint = Crypto::SignedBigInteger { end }.multiplied_by(s_one_million_bigint);
    auto end_local_time = TRY(to_local_time(vm, end_bigint, date_time_format.calendar(), date_time_format.time_zone()));

    // 7. Let rangePatterns be dateTimeFormat.[[RangePatterns]].
    auto range_patterns = date_time_format.range_patterns();

    // 8. Let rangePattern be undefined.
    Optional<::Locale::CalendarRangePattern> range_pattern;

    // 9. Let dateFieldsPracticallyEqual be true.
    bool date_fields_practically_equal = true;

    // 10. Let patternContainsLargerDateField be false.
    bool pattern_contains_larger_date_field = false;

    // 11. While dateFieldsPracticallyEqual is true and patternContainsLargerDateField is false, repeat for each row of Table 4 in order, except the header row:
    for_each_range_pattern_field(start_local_time, end_local_time, [&](auto start_value, auto end_value, auto field_name) {
        // a. Let fieldName be the name given in the Range Pattern Field column of the row.

        // b. If rangePatterns has a field [[<fieldName>]], let rp be rangePatterns.[[<fieldName>]]; else let rp be undefined.
        Optional<::Locale::CalendarRangePattern> pattern;
        for (auto const& range : range_patterns) {
            if (range.field == field_name) {
                pattern = range;
                break;
            }
        }

        // c. If rangePattern is not undefined and rp is undefined, then
        if (range_pattern.has_value() && !pattern.has_value()) {
            // i. Set patternContainsLargerDateField to true.
            pattern_contains_larger_date_field = true;
        }
        // d. Else,
        else {
            // i. Let rangePattern be rp.
            range_pattern = pattern;

            switch (field_name) {
            // ii. If fieldName is equal to [[AmPm]], then
            case ::Locale::CalendarRangePattern::Field::AmPm: {
                // 1. Let v1 be tm1.[[Hour]].
                // 2. Let v2 be tm2.[[Hour]].

                // 3. If v1 is greater than 11 and v2 less or equal than 11, or v1 is less or equal than 11 and v2 is greater than 11, then
                if ((start_value > 11 && end_value <= 11) || (start_value <= 11 && end_value > 11)) {
                    // a. Set dateFieldsPracticallyEqual to false.
                    date_fields_practically_equal = false;
                }

                break;
            }
            // iii. Else if fieldName is equal to [[DayPeriod]], then
            case ::Locale::CalendarRangePattern::Field::DayPeriod: {
                // 1. Let v1 be a String value representing the day period of tm1; the String value depends upon the implementation and the effective locale of dateTimeFormat.
                auto start_period = ::Locale::get_calendar_day_period_symbol_for_hour(date_time_format.data_locale(), date_time_format.calendar(), ::Locale::CalendarPatternStyle::Short, start_value);

                // 2. Let v2 be a String value representing the day period of tm2; the String value depends upon the implementation and the effective locale of dateTimeFormat.
                auto end_period = ::Locale::get_calendar_day_period_symbol_for_hour(date_time_format.data_locale(), date_time_format.calendar(), ::Locale::CalendarPatternStyle::Short, end_value);

                // 3. If v1 is not equal to v2, then
                if (start_period != end_period) {
                    // a. Set dateFieldsPracticallyEqual to false.
                    date_fields_practically_equal = false;
                }

                break;
            }
            // iv. Else if fieldName is equal to [[FractionalSecondDigits]], then
            case ::Locale::CalendarRangePattern::Field::FractionalSecondDigits: {
                // 1. Let fractionalSecondDigits be dateTimeFormat.[[FractionalSecondDigits]].
                Optional<u8> fractional_second_digits;
                if (date_time_format.has_fractional_second_digits())
                    fractional_second_digits = date_time_format.fractional_second_digits();

                // 2. If fractionalSecondDigits is undefined, then
                if (!fractional_second_digits.has_value()) {
                    // a. Set fractionalSecondDigits to 3.
                    fractional_second_digits = 3;
                }

                // 3. Let v1 be tm1.[[Millisecond]].
                // 4. Let v2 be tm2.[[Millisecond]].

                // 5. Let v1 be floor(v1 × 10( fractionalSecondDigits - 3 )).
                start_value = floor(start_value * pow(10, static_cast<int>(*fractional_second_digits) - 3));

                // 6. Let v2 be floor(v2 × 10( fractionalSecondDigits - 3 )).
                end_value = floor(end_value * pow(10, static_cast<int>(*fractional_second_digits) - 3));

                // 7. If v1 is not equal to v2, then
                if (start_value != end_value) {
                    // a. Set dateFieldsPracticallyEqual to false.
                    date_fields_practically_equal = false;
                }

                break;
            }

            // v. Else,
            default: {
                // 1. Let v1 be tm1.[[<fieldName>]].
                // 2. Let v2 be tm2.[[<fieldName>]].

                // 3. If v1 is not equal to v2, then
                if (start_value != end_value) {
                    // a. Set dateFieldsPracticallyEqual to false.
                    date_fields_practically_equal = false;
                }

                break;
            }
            }
        }

        if (date_fields_practically_equal && !pattern_contains_larger_date_field)
            return IterationDecision::Continue;
        return IterationDecision::Break;
    });

    // 12. If dateFieldsPracticallyEqual is true, then
    if (date_fields_practically_equal) {
        // a. Let pattern be dateTimeFormat.[[Pattern]].
        auto const& pattern = date_time_format.pattern();

        // b. Let patternParts be PartitionPattern(pattern).
        auto pattern_parts = partition_pattern(pattern);

        // c. Let result be ? FormatDateTimePattern(dateTimeFormat, patternParts, x, undefined).
        auto raw_result = TRY(format_date_time_pattern(vm, date_time_format, move(pattern_parts), start, nullptr));
        auto result = PatternPartitionWithSource::create_from_parent_list(move(raw_result));

        // d. For each Record { [[Type]], [[Value]] } r in result, do
        for (auto& part : result) {
            // i. Set r.[[Source]] to "shared".
            part.source = "shared"sv;
        }

        // e. Return result.
        return result;
    }

    // 13. Let result be a new empty List.
    Vector<PatternPartitionWithSource> result;

    // 14. If rangePattern is undefined, then
    if (!range_pattern.has_value()) {
        // a. Let rangePattern be rangePatterns.[[Default]].
        range_pattern = ::Locale::get_calendar_default_range_format(date_time_format.data_locale(), date_time_format.calendar());

        // Non-standard, range_pattern will be empty if Unicode data generation is disabled.
        if (!range_pattern.has_value())
            return result;

        // Non-standard, LibUnicode leaves the CLDR's {0} and {1} partitions in the default patterns
        // to be replaced at runtime with the DateTimeFormat object's pattern.
        auto const& pattern = date_time_format.pattern();

        if (range_pattern->start_range.contains("{0}"sv)) {
            range_pattern->start_range = MUST(range_pattern->start_range.replace("{0}"sv, pattern, ReplaceMode::FirstOnly));
            range_pattern->end_range = MUST(range_pattern->end_range.replace("{1}"sv, pattern, ReplaceMode::FirstOnly));
        } else {
            range_pattern->start_range = MUST(range_pattern->start_range.replace("{1}"sv, pattern, ReplaceMode::FirstOnly));
            range_pattern->end_range = MUST(range_pattern->end_range.replace("{0}"sv, pattern, ReplaceMode::FirstOnly));
        }

        // FIXME: The above is not sufficient. For example, if the start date is days before the end date, and only the timeStyle
        //        option is provided, the resulting range will not include the differing dates. We will likely need to implement
        //        step 3 here: https://unicode.org/reports/tr35/tr35-dates.html#intervalFormats
    }

    // 15. For each Record { [[Pattern]], [[Source]] } rangePatternPart in rangePattern.[[PatternParts]], do
    TRY(for_each_range_pattern_with_source(*range_pattern, [&](auto const& pattern, auto source) -> ThrowCompletionOr<void> {
        // a. Let pattern be rangePatternPart.[[Pattern]].
        // b. Let source be rangePatternPart.[[Source]].

        // c. If source is "startRange" or "shared", then
        //     i. Let z be x.
        // d. Else,
        //     i. Let z be y.
        auto time = ((source == "startRange") || (source == "shared")) ? start : end;

        // e. Let patternParts be PartitionPattern(pattern).
        auto pattern_parts = partition_pattern(pattern);

        // f. Let partResult be ? FormatDateTimePattern(dateTimeFormat, patternParts, z, rangePattern).
        auto raw_part_result = TRY(format_date_time_pattern(vm, date_time_format, move(pattern_parts), time, &range_pattern.value()));
        auto part_result = PatternPartitionWithSource::create_from_parent_list(move(raw_part_result));

        // g. For each Record { [[Type]], [[Value]] } r in partResult, do
        for (auto& part : part_result) {
            // i. Set r.[[Source]] to source.
            part.source = source;
        }

        // h. Add all elements in partResult to result in order.
        result.extend(move(part_result));
        return {};
    }));

    // 16. Return result.
    return result;
}

// 11.5.10 FormatDateTimeRange ( dateTimeFormat, x, y ), https://tc39.es/ecma402/#sec-formatdatetimerange
ThrowCompletionOr<String> format_date_time_range(VM& vm, DateTimeFormat& date_time_format, double start, double end)
{
    // 1. Let parts be ? PartitionDateTimeRangePattern(dateTimeFormat, x, y).
    auto parts = TRY(partition_date_time_range_pattern(vm, date_time_format, start, end));

    // 2. Let result be the empty String.
    StringBuilder result;

    // 3. For each Record { [[Type]], [[Value]], [[Source]] } part in parts, do
    for (auto& part : parts) {
        // a. Set result to the string-concatenation of result and part.[[Value]].
        result.append(part.value);
    }

    // 4. Return result.
    return MUST(result.to_string());
}

// 11.5.11 FormatDateTimeRangeToParts ( dateTimeFormat, x, y ), https://tc39.es/ecma402/#sec-formatdatetimerangetoparts
ThrowCompletionOr<NonnullGCPtr<Array>> format_date_time_range_to_parts(VM& vm, DateTimeFormat& date_time_format, double start, double end)
{
    auto& realm = *vm.current_realm();

    // 1. Let parts be ? PartitionDateTimeRangePattern(dateTimeFormat, x, y).
    auto parts = TRY(partition_date_time_range_pattern(vm, date_time_format, start, end));

    // 2. Let result be ! ArrayCreate(0).
    auto result = MUST(Array::create(realm, 0));

    // 3. Let n be 0.
    size_t n = 0;

    // 4. For each Record { [[Type]], [[Value]], [[Source]] } part in parts, do
    for (auto& part : parts) {
        // a. Let O be OrdinaryObjectCreate(%ObjectPrototype%).
        auto object = Object::create(realm, realm.intrinsics().object_prototype());

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        MUST(object->create_data_property_or_throw(vm.names.type, PrimitiveString::create(vm, part.type)));

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        MUST(object->create_data_property_or_throw(vm.names.value, PrimitiveString::create(vm, move(part.value))));

        // d. Perform ! CreateDataPropertyOrThrow(O, "source", part.[[Source]]).
        MUST(object->create_data_property_or_throw(vm.names.source, PrimitiveString::create(vm, part.source)));

        // e. Perform ! CreateDataProperty(result, ! ToString(n), O).
        MUST(result->create_data_property_or_throw(n, object));

        // f. Increment n by 1.
        ++n;
    }

    // 5. Return result.
    return result;
}

// 11.5.12 ToLocalTime ( epochNs, calendar, timeZoneIdentifier ), https://tc39.es/ecma402/#sec-tolocaltime
ThrowCompletionOr<LocalTime> to_local_time(VM& vm, Crypto::SignedBigInteger const& epoch_ns, StringView calendar, StringView time_zone_identifier)
{
    double offset_ns { 0 };

    // 1. If IsTimeZoneOffsetString(timeZoneIdentifier) is true, then
    if (is_time_zone_offset_string(time_zone_identifier)) {
        // a. Let offsetNs be ParseTimeZoneOffsetString(timeZoneIdentifier).
        offset_ns = parse_time_zone_offset_string(time_zone_identifier);
    }
    // 2. Else,
    else {
        // a. Assert: IsValidTimeZoneName(timeZoneIdentifier) is true.
        VERIFY(Temporal::is_available_time_zone_name(time_zone_identifier));

        // b. Let offsetNs be GetNamedTimeZoneOffsetNanoseconds(timeZoneIdentifier, epochNs).
        offset_ns = get_named_time_zone_offset_nanoseconds(time_zone_identifier, epoch_ns);
    }

    // NOTE: Unlike the spec, we still perform the below computations with BigInts until we are ready
    //       to divide the number by 10^6. The spec expects an MV here. If we try to use i64, we will
    //       overflow; if we try to use a double, we lose quite a bit of accuracy.

    // 3. Let tz be ℝ(epochNs) + offsetNs.
    auto zoned_time_ns = epoch_ns.plus(Crypto::SignedBigInteger { offset_ns });

    // 4. If calendar is "gregory", then
    if (calendar == "gregory"sv) {
        auto zoned_time_ms = zoned_time_ns.divided_by(s_one_million_bigint).quotient;
        auto zoned_time = floor(zoned_time_ms.to_double(Crypto::UnsignedBigInteger::RoundingMode::ECMAScriptNumberValueFor));

        auto year = year_from_time(zoned_time);

        // a. Return a record with fields calculated from tz according to Table 8.
        return LocalTime {
            // WeekDay(𝔽(floor(tz / 10^6)))
            .weekday = week_day(zoned_time),
            // Let year be YearFromTime(𝔽(floor(tz / 10^6))). If year < 1𝔽, return "BC", else return "AD".
            .era = year < 1 ? ::Locale::Era::BC : ::Locale::Era::AD,
            // YearFromTime(𝔽(floor(tz / 10^6)))
            .year = year,
            // undefined.
            .related_year = js_undefined(),
            // undefined.
            .year_name = js_undefined(),
            // MonthFromTime(𝔽(floor(tz / 10^6)))
            .month = month_from_time(zoned_time),
            // DateFromTime(𝔽(floor(tz / 10^6)))
            .day = date_from_time(zoned_time),
            // HourFromTime(𝔽(floor(tz / 10^6)))
            .hour = hour_from_time(zoned_time),
            // MinFromTime(𝔽(floor(tz / 10^6)))
            .minute = min_from_time(zoned_time),
            // SecFromTime(𝔽(floor(tz / 10^6)))
            .second = sec_from_time(zoned_time),
            // msFromTime(𝔽(floor(tz / 10^6)))
            .millisecond = ms_from_time(zoned_time),
        };
    }

    // 5. Else,
    //     a. Return a record with the fields of Column 1 of Table 8 calculated from tz for the given calendar. The calculations should use best available information about the specified calendar.
    // FIXME: Implement this when non-Gregorian calendars are supported by LibUnicode.
    return vm.throw_completion<InternalError>(ErrorType::NotImplemented, "Non-Gregorian calendars"sv);
}

}
