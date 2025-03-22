/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/DateTimeFormat.h>
#include <LibJS/Runtime/Intl/DateTimeFormatConstructor.h>
#include <LibJS/Runtime/Temporal/ISO8601.h>
#include <LibUnicode/DateTimeFormat.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(DateTimeFormatConstructor);

// 11.1 The Intl.DateTimeFormat Constructor, https://tc39.es/ecma402/#sec-intl-datetimeformat-constructor
DateTimeFormatConstructor::DateTimeFormatConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.DateTimeFormat.as_string(), realm.intrinsics().function_prototype())
{
}

void DateTimeFormatConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 11.2.1 Intl.DateTimeFormat.prototype, https://tc39.es/ecma402/#sec-intl.datetimeformat.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().intl_date_time_format_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.supportedLocalesOf, supported_locales_of, 1, attr);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 11.1.1 Intl.DateTimeFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.datetimeformat
ThrowCompletionOr<Value> DateTimeFormatConstructor::call()
{
    // 1. If NewTarget is undefined, let newTarget be the active function object, else let newTarget be NewTarget.
    return TRY(construct(*this));
}

// 11.1.1 Intl.DateTimeFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.datetimeformat
ThrowCompletionOr<GC::Ref<Object>> DateTimeFormatConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 2. Let dateTimeFormat be ? CreateDateTimeFormat(newTarget, locales, options, ANY, DATE).
    auto date_time_format = TRY(create_date_time_format(vm, new_target, locales, options, OptionRequired::Any, OptionDefaults::Date));

    // 3. If the implementation supports the normative optional constructor mode of 4.3 Note 1, then
    //     a. Let this be the this value.
    //     b. Return ? ChainDateTimeFormat(dateTimeFormat, NewTarget, this).

    // 4. Return dateTimeFormat.
    return date_time_format;
}

// 11.2.2 Intl.DateTimeFormat.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-intl.datetimeformat.supportedlocalesof
JS_DEFINE_NATIVE_FUNCTION(DateTimeFormatConstructor::supported_locales_of)
{
    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let availableLocales be %DateTimeFormat%.[[AvailableLocales]].

    // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales));

    // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
    return TRY(filter_locales(vm, requested_locales, options));
}

// 11.1.2 CreateDateTimeFormat ( newTarget, locales, options, required, defaults ), https://tc39.es/ecma402/#sec-createdatetimeformat
// 15.7.1 CreateDateTimeFormat ( newTarget, locales, options, required, defaults [ , toLocaleStringTimeZone ] ), https://tc39.es/proposal-temporal/#sec-createdatetimeformat
ThrowCompletionOr<GC::Ref<DateTimeFormat>> create_date_time_format(VM& vm, FunctionObject& new_target, Value locales_value, Value options_value, OptionRequired required, OptionDefaults defaults, Optional<String> const& to_locale_string_time_zone)
{
    // 1. Let dateTimeFormat be ? OrdinaryCreateFromConstructor(newTarget, "%Intl.DateTimeFormat.prototype%", « [[InitializedDateTimeFormat]], [[Locale]], [[Calendar]], [[NumberingSystem]], [[TimeZone]], [[HourCycle]], [[DateStyle]], [[TimeStyle]], [[DateTimeFormat]], [[BoundFormat]] »).
    auto date_time_format = TRY(ordinary_create_from_constructor<DateTimeFormat>(vm, new_target, &Intrinsics::intl_date_time_format_prototype));

    // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    auto requested_locales = TRY(canonicalize_locale_list(vm, locales_value));

    // 3. Set options to ? CoerceOptionsToObject(options).
    auto* options = TRY(coerce_options_to_object(vm, options_value));

    // 4. Let opt be a new Record.
    LocaleOptions opt {};

    // 5. Let matcher be ? GetOption(options, "localeMatcher", string, « "lookup", "best fit" », "best fit").
    auto matcher = TRY(get_option(vm, *options, vm.names.localeMatcher, OptionType::String, AK::Array { "lookup"sv, "best fit"sv }, "best fit"sv));

    // 6. Set opt.[[localeMatcher]] to matcher.
    opt.locale_matcher = matcher;

    // 7. Let calendar be ? GetOption(options, "calendar", string, empty, undefined).
    auto calendar = TRY(get_option(vm, *options, vm.names.calendar, OptionType::String, {}, Empty {}));

    // 8. If calendar is not undefined, then
    if (!calendar.is_undefined()) {
        // a. If calendar cannot be matched by the type Unicode locale nonterminal, throw a RangeError exception.
        if (!Unicode::is_type_identifier(calendar.as_string().utf8_string_view()))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, calendar, "calendar"sv);
    }

    // 9. Set opt.[[ca]] to calendar.
    opt.ca = locale_key_from_value(calendar);

    // 10. Let numberingSystem be ? GetOption(options, "numberingSystem", string, empty, undefined).
    auto numbering_system = TRY(get_option(vm, *options, vm.names.numberingSystem, OptionType::String, {}, Empty {}));

    // 11. If numberingSystem is not undefined, then
    if (!numbering_system.is_undefined()) {
        // a. If numberingSystem cannot be matched by the type Unicode locale nonterminal, throw a RangeError exception.
        if (!Unicode::is_type_identifier(numbering_system.as_string().utf8_string_view()))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, numbering_system, "numberingSystem"sv);
    }

    // 12. Set opt.[[nu]] to numberingSystem.
    opt.nu = locale_key_from_value(numbering_system);

    // 13. Let hour12 be ? GetOption(options, "hour12", boolean, empty, undefined).
    auto hour12 = TRY(get_option(vm, *options, vm.names.hour12, OptionType::Boolean, {}, Empty {}));

    // 14. Let hourCycle be ? GetOption(options, "hourCycle", string, « "h11", "h12", "h23", "h24" », undefined).
    auto hour_cycle = TRY(get_option(vm, *options, vm.names.hourCycle, OptionType::String, AK::Array { "h11"sv, "h12"sv, "h23"sv, "h24"sv }, Empty {}));

    // 15. If hour12 is not undefined, then
    if (!hour12.is_undefined()) {
        // a. Set hourCycle to null.
        hour_cycle = js_null();
    }

    // 16. Set opt.[[hc]] to hourCycle.
    opt.hc = locale_key_from_value(hour_cycle);

    // 17. Let r be ResolveLocale(%Intl.DateTimeFormat%.[[AvailableLocales]], requestedLocales, opt, %Intl.DateTimeFormat%.[[RelevantExtensionKeys]], %Intl.DateTimeFormat%.[[LocaleData]]).
    auto result = resolve_locale(requested_locales, opt, DateTimeFormat::relevant_extension_keys());

    // 18. Set dateTimeFormat.[[Locale]] to r.[[Locale]].
    date_time_format->set_locale(move(result.locale));
    date_time_format->set_icu_locale(move(result.icu_locale));

    // 19. Let resolvedCalendar be r.[[ca]].
    // 20. Set dateTimeFormat.[[Calendar]] to resolvedCalendar.
    if (auto* resolved_calendar = result.ca.get_pointer<String>())
        date_time_format->set_calendar(move(*resolved_calendar));

    // 21. Set dateTimeFormat.[[NumberingSystem]] to r.[[nu]].
    if (auto* resolved_numbering_system = result.nu.get_pointer<String>())
        date_time_format->set_numbering_system(move(*resolved_numbering_system));

    // 22. Let resolvedLocaleData be r.[[LocaleData]].

    Optional<Unicode::HourCycle> hour_cycle_value;
    Optional<bool> hour12_value;

    // 23. If hour12 is true, then
    //     a. Let hc be resolvedLocaleData.[[hourCycle12]].
    // 24. Else if hour12 is false, then
    //     a. Let hc be resolvedLocaleData.[[hourCycle24]].
    if (hour12.is_boolean()) {
        // NOTE: We let LibUnicode figure out the appropriate hour cycle.
        hour12_value = hour12.as_bool();
    }
    // 25. Else,
    else {
        // a. Assert: hour12 is undefined.
        VERIFY(hour12.is_undefined());

        // b. Let hc be r.[[hc]].
        if (auto* resolved_hour_cycle = result.hc.get_pointer<String>())
            hour_cycle_value = Unicode::hour_cycle_from_string(*resolved_hour_cycle);

        // c. If hc is null, set hc to resolvedLocaleData.[[hourCycle]].
        if (!hour_cycle_value.has_value())
            hour_cycle_value = Unicode::default_hour_cycle(date_time_format->locale());
    }

    // 26. Set dateTimeFormat.[[HourCycle]] to hc.
    // NOTE: The [[HourCycle]] is stored and accessed from [[DateTimeFormat]].

    // 27. Let timeZone be ? Get(options, "timeZone").
    auto time_zone_value = TRY(options->get(vm.names.timeZone));
    String time_zone;

    // 28. If timeZone is undefined, then
    if (time_zone_value.is_undefined()) {
        // a. If toLocaleStringTimeZone is present, then
        if (to_locale_string_time_zone.has_value()) {
            // i. Set timeZone to toLocaleStringTimeZone.
            time_zone = *to_locale_string_time_zone;
        }
        // b. Else,
        else {
            // i. Set timeZone to SystemTimeZoneIdentifier().
            time_zone = system_time_zone_identifier();
        }
    }
    // 29. Else,
    else {
        // a. If toLocaleStringTimeZone is present, throw a TypeError exception.
        if (to_locale_string_time_zone.has_value())
            return vm.throw_completion<TypeError>(ErrorType::IntlInvalidDateTimeFormatOption, vm.names.timeZone, "a toLocaleString time zone"sv);

        // b. Set timeZone to ? ToString(timeZone).
        time_zone = TRY(time_zone_value.to_string(vm));
    }

    // 30. If IsTimeZoneOffsetString(timeZone) is true, then
    bool is_time_zone_offset_string = JS::is_offset_time_zone_identifier(time_zone);

    if (is_time_zone_offset_string) {
        // a. Let parseResult be ParseText(StringToCodePoints(timeZone), UTCOffset[~SubMinutePrecision]).
        auto parse_result = Temporal::parse_utc_offset(time_zone, Temporal::SubMinutePrecision::No);

        // b. Assert: parseResult is a Parse Node.
        VERIFY(parse_result.has_value());

        // c. Let offsetNanoseconds be ? ParseDateTimeUTCOffset(timeZone).
        auto offset_nanoseconds = TRY(parse_date_time_utc_offset(vm, time_zone));

        // d. Let offsetMinutes be offsetNanoseconds / (6 × 10**10).
        auto offset_minutes = offset_nanoseconds / 60'000'000'000;

        // e. Assert: offsetMinutes is an integer.
        VERIFY(trunc(offset_minutes) == offset_minutes);

        // f. Set timeZone to FormatOffsetTimeZoneIdentifier(offsetMinutes).
        time_zone = format_offset_time_zone_identifier(offset_minutes);
    }
    // 31. Else,
    else {
        // a. Let timeZoneIdentifierRecord be GetAvailableNamedTimeZoneIdentifier(timeZone).
        auto time_zone_identifier_record = get_available_named_time_zone_identifier(time_zone);

        // b. If timeZoneIdentifierRecord is EMPTY, throw a RangeError exception.
        if (!time_zone_identifier_record.has_value())
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, time_zone, vm.names.timeZone);

        // c. Set timeZone to timeZoneIdentifierRecord.[[PrimaryIdentifier]].
        time_zone = time_zone_identifier_record->primary_identifier;
    }

    // 32. Set dateTimeFormat.[[TimeZone]] to timeZone.
    date_time_format->set_time_zone(time_zone);

    // NOTE: ICU requires time zone offset strings to be of the form "GMT+00:00"
    if (is_time_zone_offset_string)
        time_zone = MUST(String::formatted("GMT{}", time_zone));

    // AD-HOC: We must store the massaged time zone for creating ICU formatters for Temporal objects.
    date_time_format->set_temporal_time_zone(time_zone);

    // 33. Let formatOptions be a new Record.
    Unicode::CalendarPattern format_options {};

    // 34. Set formatOptions.[[hourCycle]] to hc.
    format_options.hour_cycle = hour_cycle_value;
    format_options.hour12 = hour12_value;

    // 35. Let hasExplicitFormatComponents be false.
    // NOTE: Instead of using a boolean, we track any explicitly provided component name for nicer exception messages.
    PropertyKey const* explicit_format_component = nullptr;

    // 36. For each row of Table 16, except the header row, in table order, do
    TRY(for_each_calendar_field(vm, format_options, [&](auto& option, PropertyKey const& property, auto const& values) -> ThrowCompletionOr<void> {
        using ValueType = typename RemoveReference<decltype(option)>::ValueType;

        // a. Let prop be the name given in the Property column of the current row.

        // b. If prop is "fractionalSecondDigits", then
        if constexpr (IsIntegral<ValueType>) {
            // i. Let value be ? GetNumberOption(options, "fractionalSecondDigits", 1, 3, undefined).
            auto value = TRY(get_number_option(vm, *options, property, 1, 3, {}));

            // d. Set formatOptions.[[<prop>]] to value.
            if (value.has_value()) {
                option = static_cast<ValueType>(value.value());

                // e. If value is not undefined, then
                //     i. Set hasExplicitFormatComponents to true.
                explicit_format_component = &property;
            }
        }
        // c. Else,
        else {
            // i. Let values be a List whose elements are the strings given in the Values column of the current row.
            // ii. Let value be ? GetOption(options, prop, string, values, undefined).
            auto value = TRY(get_option(vm, *options, property, OptionType::String, values, Empty {}));

            // d. Set formatOptions.[[<prop>]] to value.
            if (!value.is_undefined()) {
                option = Unicode::calendar_pattern_style_from_string(value.as_string().utf8_string_view());

                // e. If value is not undefined, then
                //     i. Set hasExplicitFormatComponents to true.
                explicit_format_component = &property;
            }
        }

        return {};
    }));

    // 37. Let formatMatcher be ? GetOption(options, "formatMatcher", string, « "basic", "best fit" », "best fit").
    [[maybe_unused]] auto format_matcher = TRY(get_option(vm, *options, vm.names.formatMatcher, OptionType::String, AK::Array { "basic"sv, "best fit"sv }, "best fit"sv));

    // 38. Let dateStyle be ? GetOption(options, "dateStyle", string, « "full", "long", "medium", "short" », undefined).
    auto date_style = TRY(get_option(vm, *options, vm.names.dateStyle, OptionType::String, AK::Array { "full"sv, "long"sv, "medium"sv, "short"sv }, Empty {}));

    // 39. Set dateTimeFormat.[[DateStyle]] to dateStyle.
    if (!date_style.is_undefined())
        date_time_format->set_date_style(date_style.as_string().utf8_string_view());

    // 40. Let timeStyle be ? GetOption(options, "timeStyle", string, « "full", "long", "medium", "short" », undefined).
    auto time_style = TRY(get_option(vm, *options, vm.names.timeStyle, OptionType::String, AK::Array { "full"sv, "long"sv, "medium"sv, "short"sv }, Empty {}));

    // 41. Set dateTimeFormat.[[TimeStyle]] to timeStyle.
    if (!time_style.is_undefined())
        date_time_format->set_time_style(time_style.as_string().utf8_string_view());

    // 42. Let formats be resolvedLocaleData.[[formats]].[[<resolvedCalendar>]].

    OwnPtr<Unicode::DateTimeFormat> formatter;

    // 43. If dateStyle is not undefined or timeStyle is not undefined, then
    if (date_time_format->has_date_style() || date_time_format->has_time_style()) {
        // a. If hasExplicitFormatComponents is true, then
        if (explicit_format_component != nullptr) {
            // i. Throw a TypeError exception.
            return vm.throw_completion<TypeError>(ErrorType::IntlInvalidDateTimeFormatOption, *explicit_format_component, "dateStyle or timeStyle"sv);
        }

        // b. If required is date and timeStyle is not undefined, then
        if (required == OptionRequired::Date && !time_style.is_undefined()) {
            // i. Throw a TypeError exception.
            return vm.throw_completion<TypeError>(ErrorType::IntlInvalidDateTimeFormatOption, "timeStyle"sv, "date"sv);
        }

        // c. If required is time and dateStyle is not undefined, then
        if (required == OptionRequired::Time && !date_style.is_undefined()) {
            // i. Throw a TypeError exception.
            return vm.throw_completion<TypeError>(ErrorType::IntlInvalidDateTimeFormatOption, "dateStyle"sv, "time"sv);
        }

        // d. Let styles be resolvedLocaleData.[[styles]].[[<resolvedCalendar>]].
        // e. Let bestFormat be DateTimeStyleFormat(dateStyle, timeStyle, styles).
        formatter = Unicode::DateTimeFormat::create_for_date_and_time_style(
            date_time_format->icu_locale(),
            time_zone,
            format_options.hour_cycle,
            format_options.hour12,
            date_time_format->date_style(),
            date_time_format->time_style());

        auto best_format = formatter->chosen_pattern();
        using enum Unicode::CalendarPattern::Field;

        // f. If dateStyle is not undefined, then
        if (!date_style.is_undefined()) {
            // i. Set dateTimeFormat.[[TemporalPlainDateFormat]] to AdjustDateTimeStyleFormat(formats, bestFormat, formatMatcher, « [[weekday]], [[era]], [[year]], [[month]], [[day]] »).
            auto temporal_plain_date_format = adjust_date_time_style_format(best_format, { { Weekday, Era, Year, Month, Day } });
            date_time_format->set_temporal_plain_date_format(move(temporal_plain_date_format));

            // ii. Set dateTimeFormat.[[TemporalPlainYearMonthFormat]] to AdjustDateTimeStyleFormat(formats, bestFormat, formatMatcher, « [[era]], [[year]], [[month]] »).
            auto temporal_plain_year_month_format = adjust_date_time_style_format(best_format, { { Era, Year, Month } });
            date_time_format->set_temporal_plain_year_month_format(move(temporal_plain_year_month_format));

            // iii. Set dateTimeFormat.[[TemporalPlainMonthDayFormat]] to AdjustDateTimeStyleFormat(formats, bestFormat, formatMatcher, « [[month]], [[day]] »).
            auto temporal_plain_month_day_format = adjust_date_time_style_format(best_format, { { Month, Day } });
            date_time_format->set_temporal_plain_month_day_format(move(temporal_plain_month_day_format));
        }
        // g. Else,
        else {
            // i. Set dateTimeFormat.[[TemporalPlainDateFormat]] to null.
            // ii. Set dateTimeFormat.[[TemporalPlainYearMonthFormat]] to null.
            // iii. Set dateTimeFormat.[[TemporalPlainMonthDayFormat]] to null.
        }

        // h. If timeStyle is not undefined, then
        if (!time_style.is_undefined()) {
            // i. Set dateTimeFormat.[[TemporalPlainTimeFormat]] to AdjustDateTimeStyleFormat(formats, bestFormat, formatMatcher, « [[dayPeriod]], [[hour]], [[minute]], [[second]], [[fractionalSecondDigits]] »).
            auto temporal_plain_time_format = adjust_date_time_style_format(best_format, { { DayPeriod, Hour, Minute, Second, FractionalSecondDigits } });
            date_time_format->set_temporal_plain_time_format(move(temporal_plain_time_format));
        }
        // i. Else,
        else {
            // i. Set dateTimeFormat.[[TemporalPlainTimeFormat]] to null.
        }

        // j. Set dateTimeFormat.[[TemporalPlainDateTimeFormat]] to AdjustDateTimeStyleFormat(formats, bestFormat, formatMatcher, « [[weekday]], [[era]], [[year]], [[month]], [[day]], [[dayPeriod]], [[hour]], [[minute]], [[second]], [[fractionalSecondDigits]] »).
        auto temporal_plain_date_time_format = adjust_date_time_style_format(best_format, { { Weekday, Era, Year, Month, Day, DayPeriod, Hour, Minute, Second, FractionalSecondDigits } });
        date_time_format->set_temporal_plain_date_time_format(move(temporal_plain_date_time_format));

        // k. Set dateTimeFormat.[[TemporalInstantFormat]] to bestFormat.
        date_time_format->set_temporal_instant_format(move(best_format));
    }
    // 44. Else,
    else {
        // a. Let bestFormat be GetDateTimeFormat(formats, formatMatcher, formatOptions, required, defaults, ALL).
        auto best_format = get_date_time_format(format_options, required, defaults, OptionInherit::All).release_value();

        // b. Set dateTimeFormat.[[TemporalPlainDateFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, DATE, DATE, RELEVANT).
        auto temporal_plain_date_format = get_date_time_format(format_options, OptionRequired::Date, OptionDefaults::Date, OptionInherit::Relevant);
        date_time_format->set_temporal_plain_date_format(move(temporal_plain_date_format));

        // c. Set dateTimeFormat.[[TemporalPlainYearMonthFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, YEAR-MONTH, YEAR-MONTH, RELEVANT).
        auto temporal_plain_year_month_format = get_date_time_format(format_options, OptionRequired::YearMonth, OptionDefaults::YearMonth, OptionInherit::Relevant);
        date_time_format->set_temporal_plain_year_month_format(move(temporal_plain_year_month_format));

        // d. Set dateTimeFormat.[[TemporalPlainMonthDayFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, MONTH-DAY, MONTH-DAY, RELEVANT).
        auto temporal_plain_month_day_format = get_date_time_format(format_options, OptionRequired::MonthDay, OptionDefaults::MonthDay, OptionInherit::Relevant);
        date_time_format->set_temporal_plain_month_day_format(move(temporal_plain_month_day_format));

        // e. Set dateTimeFormat.[[TemporalPlainTimeFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, TIME, TIME, RELEVANT).
        auto temporal_plain_time_format = get_date_time_format(format_options, OptionRequired::Time, OptionDefaults::Time, OptionInherit::Relevant);
        date_time_format->set_temporal_plain_time_format(move(temporal_plain_time_format));

        // f. Set dateTimeFormat.[[TemporalPlainDateTimeFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, ANY, ALL, RELEVANT).
        auto temporal_plain_date_time_format = get_date_time_format(format_options, OptionRequired::Any, OptionDefaults::All, OptionInherit::Relevant);
        date_time_format->set_temporal_plain_date_time_format(move(temporal_plain_date_time_format));

        // g. If toLocaleStringTimeZone is present, then
        if (to_locale_string_time_zone.has_value()) {
            // i. Set dateTimeFormat.[[TemporalInstantFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, ANY, ZONED-DATE-TIME, ALL).
            auto temporal_instant_format = get_date_time_format(format_options, OptionRequired::Any, OptionDefaults::ZonedDateTime, OptionInherit::All);
            date_time_format->set_temporal_instant_format(move(temporal_instant_format));
        }
        // h. Else,
        else {
            // i. Set dateTimeFormat.[[TemporalInstantFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, ANY, ALL, ALL).
            auto temporal_instant_format = get_date_time_format(format_options, OptionRequired::Any, OptionDefaults::All, OptionInherit::All);
            date_time_format->set_temporal_instant_format(move(temporal_instant_format));
        }

        formatter = Unicode::DateTimeFormat::create_for_pattern_options(
            date_time_format->icu_locale(),
            time_zone,
            best_format);
    }

    // 45. Set dateTimeFormat.[[DateTimeFormat]] to bestFormat.
    date_time_format->set_date_time_format(formatter->chosen_pattern());

    // Non-standard, create an ICU number formatter for this Intl object.
    date_time_format->set_formatter(formatter.release_nonnull());

    // 46. Return dateTimeFormat.
    return date_time_format;
}

// 11.1.3 FormatOffsetTimeZoneIdentifier ( offsetMinutes ), https://tc39.es/ecma402/#sec-formatoffsettimezoneidentifier
String format_offset_time_zone_identifier(double offset_minutes)
{
    // 1. If offsetMinutes ≥ 0, let sign be the code unit 0x002B (PLUS SIGN); otherwise, let sign be the code unit 0x002D (HYPHEN-MINUS).
    auto sign = offset_minutes >= 0.0 ? '+' : '-';

    // 2. Let absoluteMinutes be abs(offsetMinutes).
    auto absolute_minutes = fabs(offset_minutes);

    // 3. Let hours be floor(absoluteMinutes / 60).
    auto hours = static_cast<i64>(floor(absolute_minutes / 60.0));

    // 4. Let minutes be absoluteMinutes modulo 60.
    auto minutes = static_cast<i64>(modulo(absolute_minutes, 60.0));

    // 5. Return the string-concatenation of sign, ToZeroPaddedDecimalString(hours, 2), the code unit 0x003A (COLON), and ToZeroPaddedDecimalString(minutes, 2).
    return MUST(String::formatted("{}{:02}:{:02}", sign, hours, minutes));
}

}
