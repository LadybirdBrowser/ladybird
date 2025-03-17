/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Intl/DateTimeFormat.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <math.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(DateTimeFormat);

// 11 DateTimeFormat Objects, https://tc39.es/ecma402/#datetimeformat-objects
DateTimeFormat::DateTimeFormat(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

void DateTimeFormat::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_bound_format);
}

static Optional<Unicode::DateTimeFormat const&> get_or_create_formatter(StringView locale, StringView time_zone, OwnPtr<Unicode::DateTimeFormat>& formatter, Optional<Unicode::CalendarPattern> const& format)
{
    if (formatter)
        return *formatter;
    if (!format.has_value())
        return {};

    formatter = Unicode::DateTimeFormat::create_for_pattern_options(locale, time_zone, *format);
    return *formatter;
}

Optional<Unicode::DateTimeFormat const&> DateTimeFormat::temporal_plain_date_formatter()
{
    return get_or_create_formatter(m_icu_locale, m_temporal_time_zone, m_temporal_plain_date_formatter, m_temporal_plain_date_format);
}

Optional<Unicode::DateTimeFormat const&> DateTimeFormat::temporal_plain_year_month_formatter()
{
    return get_or_create_formatter(m_icu_locale, m_temporal_time_zone, m_temporal_plain_year_month_formatter, m_temporal_plain_year_month_format);
}

Optional<Unicode::DateTimeFormat const&> DateTimeFormat::temporal_plain_month_day_formatter()
{
    return get_or_create_formatter(m_icu_locale, m_temporal_time_zone, m_temporal_plain_month_day_formatter, m_temporal_plain_month_day_format);
}

Optional<Unicode::DateTimeFormat const&> DateTimeFormat::temporal_plain_time_formatter()
{
    return get_or_create_formatter(m_icu_locale, m_temporal_time_zone, m_temporal_plain_time_formatter, m_temporal_plain_time_format);
}

Optional<Unicode::DateTimeFormat const&> DateTimeFormat::temporal_plain_date_time_formatter()
{
    return get_or_create_formatter(m_icu_locale, m_temporal_time_zone, m_temporal_plain_date_time_formatter, m_temporal_plain_date_time_format);
}

Optional<Unicode::DateTimeFormat const&> DateTimeFormat::temporal_instant_formatter()
{
    return get_or_create_formatter(m_icu_locale, m_temporal_time_zone, m_temporal_instant_formatter, m_temporal_instant_format);
}

// 11.5.5 FormatDateTimePattern ( dateTimeFormat, patternParts, x, rangeFormatOptions ), https://tc39.es/ecma402/#sec-formatdatetimepattern
// 15.9.4 FormatDateTimePattern ( dateTimeFormat, format, pattern, x, epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-formatdatetimepattern
Vector<Unicode::DateTimeFormat::Partition> format_date_time_pattern(ValueFormat const& format_record)
{
    return format_record.formatter.format_to_parts(format_record.epoch_milliseconds);
}

// 11.5.6 PartitionDateTimePattern ( dateTimeFormat, x ), https://tc39.es/ecma402/#sec-partitiondatetimepattern
// 15.9.5 PartitionDateTimePattern ( dateTimeFormat, x ), https://tc39.es/proposal-temporal/#sec-partitiondatetimepattern
ThrowCompletionOr<Vector<Unicode::DateTimeFormat::Partition>> partition_date_time_pattern(VM& vm, DateTimeFormat& date_time_format, FormattableDateTime const& time)
{
    // 1. Let xFormatRecord be ? HandleDateTimeValue(dateTimeFormat, x).
    auto format_record = TRY(handle_date_time_value(vm, date_time_format, time));

    // 5. Let result be ? FormatDateTimePattern(dateTimeFormat, format, pattern, xFormatRecord.[[EpochNanoseconds]]).
    return format_date_time_pattern(format_record);
}

// 11.5.7 FormatDateTime ( dateTimeFormat, x ), https://tc39.es/ecma402/#sec-formatdatetime
// 15.9.6 FormatDateTime ( dateTimeFormat, x ), https://tc39.es/proposal-temporal/#sec-formatdatetime
ThrowCompletionOr<String> format_date_time(VM& vm, DateTimeFormat& date_time_format, FormattableDateTime const& time)
{
    // 1. Let parts be ? PartitionDateTimePattern(dateTimeFormat, x).
    // 2. Let result be the empty String.
    String result;

    // NOTE: We short-circuit PartitionDateTimePattern as we do not need individual partitions.
    {
        // 1. Let xFormatRecord be ? HandleDateTimeValue(dateTimeFormat, x).
        auto format_record = TRY(handle_date_time_value(vm, date_time_format, time));

        result = format_record.formatter.format(format_record.epoch_milliseconds);
    }

    // 4. Return result.
    return result;
}

// 11.5.8 FormatDateTimeToParts ( dateTimeFormat, x ), https://tc39.es/ecma402/#sec-formatdatetimetoparts
// 15.9.7 FormatDateTimeToParts ( dateTimeFormat, x ), https://tc39.es/proposal-temporal/#sec-formatdatetimetoparts
ThrowCompletionOr<GC::Ref<Array>> format_date_time_to_parts(VM& vm, DateTimeFormat& date_time_format, FormattableDateTime const& time)
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

// 11.5.9 PartitionDateTimeRangePattern ( dateTimeFormat, x, y ), https://tc39.es/ecma402/#sec-partitiondatetimerangepattern
// 15.9.8 PartitionDateTimeRangePattern ( dateTimeFormat, x, y ), https://tc39.es/proposal-temporal/#sec-partitiondatetimerangepattern
ThrowCompletionOr<Vector<Unicode::DateTimeFormat::Partition>> partition_date_time_range_pattern(VM& vm, DateTimeFormat& date_time_format, FormattableDateTime const& start, FormattableDateTime const& end)
{
    // 1. If IsTemporalObject(x) is true or IsTemporalObject(y) is true, then
    if (is_temporal_object(start) || is_temporal_object(end)) {
        // a. If SameTemporalType(x, y) is false, throw a TypeError exception.
        if (!same_temporal_type(start, end))
            return vm.throw_completion<TypeError>(ErrorType::IntlTemporalFormatRangeTypeMismatch);
    }

    // 2. Let xFormatRecord be ? HandleDateTimeValue(dateTimeFormat, x).
    auto start_format_record = TRY(handle_date_time_value(vm, date_time_format, start));

    // 3. Let yFormatRecord be ? HandleDateTimeValue(dateTimeFormat, y).
    auto end_format_record = TRY(handle_date_time_value(vm, date_time_format, end));

    return start_format_record.formatter.format_range_to_parts(start_format_record.epoch_milliseconds, end_format_record.epoch_milliseconds);
}

// 11.5.10 FormatDateTimeRange ( dateTimeFormat, x, y ), https://tc39.es/ecma402/#sec-formatdatetimerange
// 15.9.9 FormatDateTimeRange ( dateTimeFormat, x, y ), https://tc39.es/proposal-temporal/#sec-formatdatetimerange
ThrowCompletionOr<String> format_date_time_range(VM& vm, DateTimeFormat& date_time_format, FormattableDateTime const& start, FormattableDateTime const& end)
{
    // 1. Let parts be ? PartitionDateTimeRangePattern(dateTimeFormat, x, y).
    // 2. Let result be the empty String.
    String result;

    // NOTE: We short-circuit PartitionDateTimeRangePattern as we do not need individual partitions.
    {
        // 1. If IsTemporalObject(x) is true or IsTemporalObject(y) is true, then
        if (is_temporal_object(start) || is_temporal_object(end)) {
            // a. If SameTemporalType(x, y) is false, throw a TypeError exception.
            if (!same_temporal_type(start, end))
                return vm.throw_completion<TypeError>(ErrorType::IntlTemporalFormatRangeTypeMismatch);
        }

        // 2. Let xFormatRecord be ? HandleDateTimeValue(dateTimeFormat, x).
        auto start_format_record = TRY(handle_date_time_value(vm, date_time_format, start));

        // 3. Let yFormatRecord be ? HandleDateTimeValue(dateTimeFormat, y).
        auto end_format_record = TRY(handle_date_time_value(vm, date_time_format, end));

        result = start_format_record.formatter.format_range(start_format_record.epoch_milliseconds, end_format_record.epoch_milliseconds);
    }

    // 4. Return result.
    return result;
}

// 11.5.11 FormatDateTimeRangeToParts ( dateTimeFormat, x, y ), https://tc39.es/ecma402/#sec-formatdatetimerangetoparts
// 15.9.10 FormatDateTimeRangeToParts ( dateTimeFormat, x, y ), https://tc39.es/proposal-temporal/#sec-formatdatetimerangetoparts
ThrowCompletionOr<GC::Ref<Array>> format_date_time_range_to_parts(VM& vm, DateTimeFormat& date_time_format, FormattableDateTime const& start, FormattableDateTime const& end)
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

// 15.9.1 GetDateTimeFormat ( formats, matcher, options, required, defaults, inherit ), https://tc39.es/proposal-temporal/#sec-getdatetimeformat
Optional<Unicode::CalendarPattern> get_date_time_format(Unicode::CalendarPattern const& options, OptionRequired required, OptionDefaults defaults, OptionInherit inherit)
{
    using enum Unicode::CalendarPattern::Field;

    auto required_options = [&]() -> ReadonlySpan<Unicode::CalendarPattern::Field> {
        static constexpr auto date_fields = AK::Array { Weekday, Year, Month, Day };
        static constexpr auto time_fields = AK::Array { DayPeriod, Hour, Minute, Second, FractionalSecondDigits };
        static constexpr auto year_month_fields = AK::Array { Year, Month };
        static constexpr auto month_day_fields = AK::Array { Month, Day };
        static constexpr auto any_fields = AK::Array { Weekday, Year, Month, Day, DayPeriod, Hour, Minute, Second, FractionalSecondDigits };

        switch (required) {
        // 1. If required is DATE, then
        case OptionRequired::Date:
            // a. Let requiredOptions be « "weekday", "year", "month", "day" ».
            return date_fields;
        // 2. Else if required is TIME, then
        case OptionRequired::Time:
            // a. Let requiredOptions be « "dayPeriod", "hour", "minute", "second", "fractionalSecondDigits" ».
            return time_fields;
        // 3. Else if required is YEAR-MONTH, then
        case OptionRequired::YearMonth:
            // a. Let requiredOptions be « "year", "month" ».
            return year_month_fields;
        // 4. Else if required is MONTH-DAY, then
        case OptionRequired::MonthDay:
            // a. Let requiredOptions be « "month", "day" ».
            return month_day_fields;
        // 5. Else,
        case OptionRequired::Any:
            // a. Assert: required is ANY.
            // b. Let requiredOptions be « "weekday", "year", "month", "day", "dayPeriod", "hour", "minute", "second", "fractionalSecondDigits" ».
            return any_fields;
        }
        VERIFY_NOT_REACHED();
    }();

    auto default_options = [&]() -> ReadonlySpan<Unicode::CalendarPattern::Field> {
        static constexpr auto date_fields = AK::Array { Year, Month, Day };
        static constexpr auto time_fields = AK::Array { Hour, Minute, Second };
        static constexpr auto year_month_fields = AK::Array { Year, Month };
        static constexpr auto month_day_fields = AK::Array { Month, Day };
        static constexpr auto all_fields = AK::Array { Year, Month, Day, Hour, Minute, Second };

        switch (defaults) {
        // 6. If defaults is DATE, then
        case OptionDefaults::Date:
            // a. Let defaultOptions be « "year", "month", "day" ».
            return date_fields;
        // 7. Else if defaults is TIME, then
        case OptionDefaults::Time:
            // a. Let defaultOptions be « "hour", "minute", "second" ».
            return time_fields;
        // 8. Else if defaults is YEAR-MONTH, then
        case OptionDefaults::YearMonth:
            // a. Let defaultOptions be « "year", "month" ».
            return year_month_fields;
        // 9. Else if defaults is MONTH-DAY, then
        case OptionDefaults::MonthDay:
            // a. Let defaultOptions be « "month", "day" ».
            return month_day_fields;
        // 10. Else,
        case OptionDefaults::ZonedDateTime:
        case OptionDefaults::All:
            // a. Assert: defaults is ZONED-DATE-TIME or ALL.
            // b. Let defaultOptions be « "year", "month", "day", "hour", "minute", "second" ».
            return all_fields;
        }
        VERIFY_NOT_REACHED();
    }();

    Unicode::CalendarPattern format_options {};

    // 11. If inherit is ALL, then
    if (inherit == OptionInherit::All) {
        // a. Let formatOptions be a copy of options.
        format_options = options;
    }
    // 12. Else,
    else {
        // a. Let formatOptions be a new Record.

        // b. If required is one of DATE, YEAR-MONTH, or ANY, then
        if (required == OptionRequired::Date || required == OptionRequired::YearMonth || required == OptionRequired::Any) {
            // i. Set formatOptions.[[era]] to options.[[era]].
            format_options.era = options.era;
        }

        // c. If required is TIME or ANY, then
        if (required == OptionRequired::Time || required == OptionRequired::Any) {
            // i. Set formatOptions.[[hourCycle]] to options.[[hourCycle]].
            format_options.hour_cycle = options.hour_cycle;
            format_options.hour12 = options.hour12;
        }
    }

    // 13. Let anyPresent be false.
    auto any_present = false;

    // 14. For each property name prop of « "weekday", "year", "month", "day", "era", "dayPeriod", "hour", "minute", "second", "fractionalSecondDigits" », do
    static constexpr auto all_fields = AK::Array { Weekday, Year, Month, Day, Era, DayPeriod, Hour, Minute, Second, FractionalSecondDigits };

    options.for_each_calendar_field_zipped_with(format_options, all_fields, [&](auto const& option, auto&) {
        // a. If options.[[<prop>]] is not undefined, set anyPresent to true.
        if (option.has_value()) {
            any_present = true;
            return IterationDecision::Break;
        }

        return IterationDecision::Continue;
    });

    // 15. Let needDefaults be true.
    auto need_defaults = true;

    // 16. For each property name prop of requiredOptions, do
    options.for_each_calendar_field_zipped_with(format_options, required_options, [&](auto const& option, auto& format_option) {
        // a. Let value be options.[[<prop>]].
        // b. If value is not undefined, then
        if (option.has_value()) {
            // i. Set formatOptions.[[<prop>]] to value.
            format_option = *option;

            // ii. Set needDefaults to false.
            need_defaults = false;
        }

        return IterationDecision::Continue;
    });

    // 17. If needDefaults is true, then
    if (need_defaults) {
        // a. If anyPresent is true and inherit is RELEVANT, return null.
        if (any_present && inherit == OptionInherit::Relevant)
            return {};

        // b. For each property name prop of defaultOptions, do
        options.for_each_calendar_field_zipped_with(format_options, default_options, [&](auto const&, auto& format_option) {
            using ValueType = typename RemoveCVReference<decltype(format_option)>::ValueType;

            if constexpr (IsSame<ValueType, Unicode::CalendarPatternStyle>) {
                // i. Set formatOptions.[[<prop>]] to "numeric".
                format_option = Unicode::CalendarPatternStyle::Numeric;
            }

            return IterationDecision::Continue;
        });

        // c. If defaults is ZONED-DATE-TIME and formatOptions.[[timeZoneName]] is undefined, then
        if (defaults == OptionDefaults::ZonedDateTime && !format_options.time_zone_name.has_value()) {
            // i. Set formatOptions.[[timeZoneName]] to "short".
            format_options.time_zone_name = Unicode::CalendarPatternStyle::Short;
        }
    }

    // 18. If matcher is "basic", then
    //     a. Let bestFormat be BasicFormatMatcher(formatOptions, formats).
    // 19. Else,
    //     a. Let bestFormat be BestFitFormatMatcher(formatOptions, formats).
    // 20. Return bestFormat.
    return format_options;
}

// 15.9.2 AdjustDateTimeStyleFormat ( formats, baseFormat, matcher, allowedOptions ), https://tc39.es/proposal-temporal/#sec-adjustdatetimestyleformat
Unicode::CalendarPattern adjust_date_time_style_format(Unicode::CalendarPattern const& base_format, ReadonlySpan<Unicode::CalendarPattern::Field> allowed_options)
{
    // 1. Let formatOptions be a new Record.
    Unicode::CalendarPattern format_options;

    // 2. For each field name fieldName of allowedOptions, do
    base_format.for_each_calendar_field_zipped_with(format_options, allowed_options, [&](auto const& base_option, auto& format_option) {
        // a. Set the field of formatOptions whose name is fieldName to the value of the field of baseFormat whose name is fieldName.
        format_option = base_option;
        return IterationDecision::Continue;
    });

    // 3. If matcher is "basic", then
    //     a. Let bestFormat be BasicFormatMatcher(formatOptions, formats).
    // 4. Else,
    //     a. Let bestFormat be BestFitFormatMatcher(formatOptions, formats).
    // 5. Return bestFormat.
    return format_options;
}

// 15.9.11 ToDateTimeFormattable ( value ), https://tc39.es/proposal-temporal/#sec-todatetimeformattable
ThrowCompletionOr<FormattableDateTime> to_date_time_formattable(VM& vm, Value value)
{
    // 1. If IsTemporalObject(value) is true, return value.
    if (value.is_object()) {
        auto& object = value.as_object();

        if (is<Temporal::Instant>(object))
            return FormattableDateTime { static_cast<Temporal::Instant&>(object) };
        if (is<Temporal::PlainDate>(object))
            return FormattableDateTime { static_cast<Temporal::PlainDate&>(object) };
        if (is<Temporal::PlainDateTime>(object))
            return FormattableDateTime { static_cast<Temporal::PlainDateTime&>(object) };
        if (is<Temporal::PlainMonthDay>(object))
            return FormattableDateTime { static_cast<Temporal::PlainMonthDay&>(object) };
        if (is<Temporal::PlainTime>(object))
            return FormattableDateTime { static_cast<Temporal::PlainTime&>(object) };
        if (is<Temporal::PlainYearMonth>(object))
            return FormattableDateTime { static_cast<Temporal::PlainYearMonth&>(object) };
        if (is<Temporal::ZonedDateTime>(object))
            return FormattableDateTime { static_cast<Temporal::ZonedDateTime&>(object) };
    }

    // 2. Return ? ToNumber(value).
    return FormattableDateTime { TRY(value.to_number(vm)).as_double() };
}

// 15.9.12 IsTemporalObject ( value ), https://tc39.es/proposal-temporal/#sec-temporal-istemporalobject
bool is_temporal_object(FormattableDateTime const& value)
{
    // 1. If value is not an Object, then
    //     a. Return false.
    // 2. If value does not have an [[InitializedTemporalDate]], [[InitializedTemporalTime]], [[InitializedTemporalDateTime]],
    //    [[InitializedTemporalZonedDateTime]], [[InitializedTemporalYearMonth]], [[InitializedTemporalMonthDay]], or
    //    [[InitializedTemporalInstant]] internal slot, then
    //     a. Return false.
    // 3. Return true.
    return !value.has<double>();
}

// 15.9.13 SameTemporalType ( x, y ), https://tc39.es/proposal-temporal/#sec-temporal-istemporalobject
bool same_temporal_type(FormattableDateTime const& x, FormattableDateTime const& y)
{
    // 1. If either of IsTemporalObject(x) or IsTemporalObject(y) is false, return false.
    if (!is_temporal_object(x) || !is_temporal_object(y))
        return false;

    // 2. If x has an [[InitializedTemporalDate]] internal slot and y does not, return false.
    // 3. If x has an [[InitializedTemporalTime]] internal slot and y does not, return false.
    // 4. If x has an [[InitializedTemporalDateTime]] internal slot and y does not, return false.
    // 5. If x has an [[InitializedTemporalZonedDateTime]] internal slot and y does not, return false.
    // 6. If x has an [[InitializedTemporalYearMonth]] internal slot and y does not, return false.
    // 7. If x has an [[InitializedTemporalMonthDay]] internal slot and y does not, return false.
    // 8. If x has an [[InitializedTemporalInstant]] internal slot and y does not, return false.
    // 9. Return true.
    return x.index() == y.index();
}

static double to_epoch_milliseconds(Crypto::SignedBigInteger const& epoch_nanoseconds)
{
    return big_floor(epoch_nanoseconds, Temporal::NANOSECONDS_PER_MILLISECOND).to_double();
}

// 15.9.15 HandleDateTimeTemporalDate ( dateTimeFormat, temporalDate ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporaldate
ThrowCompletionOr<ValueFormat> handle_date_time_temporal_date(VM& vm, DateTimeFormat& date_time_format, Temporal::PlainDate const& temporal_date)
{
    // 1. If temporalDate.[[Calendar]] is not dateTimeFormat.[[Calendar]] or "iso8601", throw a RangeError exception.
    if (!temporal_date.calendar().is_one_of(date_time_format.calendar(), "iso8601"sv))
        return vm.throw_completion<RangeError>(ErrorType::IntlTemporalInvalidCalendar, "Temporal.PlainDate"sv, temporal_date.calendar(), date_time_format.calendar());

    // 2. Let isoDateTime be CombineISODateAndTimeRecord(temporalDate.[[ISODate]], NoonTimeRecord()).
    auto iso_date_time = Temporal::combine_iso_date_and_time_record(temporal_date.iso_date(), Temporal::noon_time_record());

    // 3. Let epochNs be ? GetEpochNanosecondsFor(dateTimeFormat.[[TimeZone]], isoDateTime, COMPATIBLE).
    auto epoch_nanoseconds = TRY(Temporal::get_epoch_nanoseconds_for(vm, date_time_format.time_zone(), iso_date_time, Temporal::Disambiguation::Compatible));

    // 4. Let format be dateTimeFormat.[[TemporalPlainDateFormat]].
    auto formatter = date_time_format.temporal_plain_date_formatter();

    // 5. If format is null, throw a TypeError exception.
    if (!formatter.has_value())
        return vm.throw_completion<TypeError>(ErrorType::IntlTemporalFormatIsNull, "Temporal.PlainDate"sv);

    // 6. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: epochNs  }.
    return ValueFormat { .formatter = *formatter, .epoch_milliseconds = to_epoch_milliseconds(epoch_nanoseconds) };
}

// 15.9.16 HandleDateTimeTemporalYearMonth ( dateTimeFormat, temporalYearMonth ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporalyearmonth
ThrowCompletionOr<ValueFormat> handle_date_time_temporal_year_month(VM& vm, DateTimeFormat& date_time_format, Temporal::PlainYearMonth const& temporal_year_month)
{
    // 1. If temporalYearMonth.[[Calendar]] is not equal to dateTimeFormat.[[Calendar]], then
    if (temporal_year_month.calendar() != date_time_format.calendar()) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::IntlTemporalInvalidCalendar, "Temporal.PlainYearMonth"sv, temporal_year_month.calendar(), date_time_format.calendar());
    }

    // 2. Let isoDateTime be CombineISODateAndTimeRecord(temporalYearMonth.[[ISODate]], NoonTimeRecord()).
    auto iso_date_time = Temporal::combine_iso_date_and_time_record(temporal_year_month.iso_date(), Temporal::noon_time_record());

    // 3. Let epochNs be ? GetEpochNanosecondsFor(dateTimeFormat.[[TimeZone]], isoDateTime, COMPATIBLE).
    auto epoch_nanoseconds = TRY(Temporal::get_epoch_nanoseconds_for(vm, date_time_format.time_zone(), iso_date_time, Temporal::Disambiguation::Compatible));

    // 4. Let format be dateTimeFormat.[[TemporalPlainYearMonthFormat]].
    auto formatter = date_time_format.temporal_plain_year_month_formatter();

    // 5. If format is null, throw a TypeError exception.
    if (!formatter.has_value())
        return vm.throw_completion<TypeError>(ErrorType::IntlTemporalFormatIsNull, "Temporal.PlainYearMonth"sv);

    // 6. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: epochNs  }.
    return ValueFormat { .formatter = *formatter, .epoch_milliseconds = to_epoch_milliseconds(epoch_nanoseconds) };
}

// 15.9.17 HandleDateTimeTemporalMonthDay ( dateTimeFormat, temporalMonthDay ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporalmonthday
ThrowCompletionOr<ValueFormat> handle_date_time_temporal_month_day(VM& vm, DateTimeFormat& date_time_format, Temporal::PlainMonthDay const& temporal_month_day)
{
    // 1. If temporalMonthDay.[[Calendar]] is not equal to dateTimeFormat.[[Calendar]], then
    if (temporal_month_day.calendar() != date_time_format.calendar()) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::IntlTemporalInvalidCalendar, "Temporal.PlainMonthDay"sv, temporal_month_day.calendar(), date_time_format.calendar());
    }

    // 2. Let isoDateTime be CombineISODateAndTimeRecord(temporalMonthDay.[[ISODate]], NoonTimeRecord()).
    auto iso_date_time = Temporal::combine_iso_date_and_time_record(temporal_month_day.iso_date(), Temporal::noon_time_record());

    // 3. Let epochNs be ? GetEpochNanosecondsFor(dateTimeFormat.[[TimeZone]], isoDateTime, COMPATIBLE).
    auto epoch_nanoseconds = TRY(Temporal::get_epoch_nanoseconds_for(vm, date_time_format.time_zone(), iso_date_time, Temporal::Disambiguation::Compatible));

    // 4. Let format be dateTimeFormat.[[TemporalPlainMonthDayFormat]].
    auto formatter = date_time_format.temporal_plain_month_day_formatter();

    // 5. If format is null, throw a TypeError exception.
    if (!formatter.has_value())
        return vm.throw_completion<TypeError>(ErrorType::IntlTemporalFormatIsNull, "Temporal.PlainMonthDay"sv);

    // 6. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: epochNs  }.
    return ValueFormat { .formatter = *formatter, .epoch_milliseconds = to_epoch_milliseconds(epoch_nanoseconds) };
}

// 15.9.18 HandleDateTimeTemporalTime ( dateTimeFormat, temporalTime ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporaltime
ThrowCompletionOr<ValueFormat> handle_date_time_temporal_time(VM& vm, DateTimeFormat& date_time_format, Temporal::PlainTime const& temporal_time)
{
    // 1. Let isoDate be CreateISODateRecord(1970, 1, 1).
    auto iso_date = Temporal::create_iso_date_record(1970, 1, 1);

    // 2. Let isoDateTime be CombineISODateAndTimeRecord(isoDate, temporalTime.[[Time]]).
    auto iso_date_time = Temporal::combine_iso_date_and_time_record(iso_date, temporal_time.time());

    // 3. Let epochNs be ? GetEpochNanosecondsFor(dateTimeFormat.[[TimeZone]], isoDateTime, COMPATIBLE).
    auto epoch_nanoseconds = TRY(Temporal::get_epoch_nanoseconds_for(vm, date_time_format.time_zone(), iso_date_time, Temporal::Disambiguation::Compatible));

    // 4. Let format be dateTimeFormat.[[TemporalPlainTimeFormat]].
    auto formatter = date_time_format.temporal_plain_time_formatter();

    // 5. If format is null, throw a TypeError exception.
    if (!formatter.has_value())
        return vm.throw_completion<TypeError>(ErrorType::IntlTemporalFormatIsNull, "Temporal.PlainTime"sv);

    // 6. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: epochNs  }.
    return ValueFormat { .formatter = *formatter, .epoch_milliseconds = to_epoch_milliseconds(epoch_nanoseconds) };
}

// 15.9.19 HandleDateTimeTemporalDateTime ( dateTimeFormat, dateTime ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporaldatetime
ThrowCompletionOr<ValueFormat> handle_date_time_temporal_date_time(VM& vm, DateTimeFormat& date_time_format, Temporal::PlainDateTime const& date_time)
{
    // 1. If dateTime.[[Calendar]] is not "iso8601" and not equal to dateTimeFormat.[[Calendar]], then
    if (!date_time.calendar().is_one_of(date_time_format.calendar(), "iso8601"sv)) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::IntlTemporalInvalidCalendar, "Temporal.PlainDateTime"sv, date_time.calendar(), date_time_format.calendar());
    }

    // 2. Let epochNs be ? GetEpochNanosecondsFor(dateTimeFormat.[[TimeZone]], dateTime.[[ISODateTime]], COMPATIBLE).
    auto epoch_nanoseconds = TRY(Temporal::get_epoch_nanoseconds_for(vm, date_time_format.time_zone(), date_time.iso_date_time(), Temporal::Disambiguation::Compatible));

    // 3. Let format be dateTimeFormat.[[TemporalPlainDateTimeFormat]].
    auto formatter = date_time_format.temporal_plain_date_time_formatter();
    VERIFY(formatter.has_value());

    // 4. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: epochNs  }.
    return ValueFormat { .formatter = *formatter, .epoch_milliseconds = to_epoch_milliseconds(epoch_nanoseconds) };
}

// 15.9.20 HandleDateTimeTemporalInstant ( dateTimeFormat, instant ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporalinstant
ValueFormat handle_date_time_temporal_instant(DateTimeFormat& date_time_format, Temporal::Instant const& instant)
{
    // 1. Let format be dateTimeFormat.[[TemporalInstantFormat]].
    auto formatter = date_time_format.temporal_instant_formatter();
    VERIFY(formatter.has_value());

    // 2. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: instant.[[EpochNanoseconds]]  }.
    return ValueFormat { .formatter = *formatter, .epoch_milliseconds = to_epoch_milliseconds(instant.epoch_nanoseconds()->big_integer()) };
}

// 15.9.21 HandleDateTimeOthers ( dateTimeFormat, x ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimeothers
ThrowCompletionOr<ValueFormat> handle_date_time_others(VM& vm, DateTimeFormat& date_time_format, double time)
{
    // 1. Set x to TimeClip(x).
    time = time_clip(time);

    // 2. If x is NaN, throw a RangeError exception.
    if (isnan(time))
        return vm.throw_completion<RangeError>(ErrorType::IntlInvalidTime);

    // 3. Let epochNanoseconds be ℤ(ℝ(x) × 10**6).

    // 4. Let format be dateTimeFormat.[[DateTimeFormat]].
    auto const& formatter = date_time_format.formatter();

    // 5. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: epochNanoseconds  }.
    return ValueFormat { .formatter = formatter, .epoch_milliseconds = time };
}

// 15.9.22 HandleDateTimeValue ( dateTimeFormat, x ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimevalue
ThrowCompletionOr<ValueFormat> handle_date_time_value(VM& vm, DateTimeFormat& date_time_format, FormattableDateTime const& formattable)
{
    return formattable.visit(
        // 1. If x is an Object, then
        // a. If x has an [[InitializedTemporalDate]] internal slot, then
        [&](GC::Ref<Temporal::PlainDate> temporal_date) {
            // i. Return ? HandleDateTimeTemporalDate(dateTimeFormat, x).
            return handle_date_time_temporal_date(vm, date_time_format, temporal_date);
        },
        // b. If x has an [[InitializedTemporalYearMonth]] internal slot, then
        [&](GC::Ref<Temporal::PlainYearMonth> temporal_year_month) {
            // i. Return ? HandleDateTimeTemporalYearMonth(dateTimeFormat, x).
            return handle_date_time_temporal_year_month(vm, date_time_format, temporal_year_month);
        },
        // c. If x has an [[InitializedTemporalMonthDay]] internal slot, then
        [&](GC::Ref<Temporal::PlainMonthDay> temporal_month_day) {
            // i. Return ? HandleDateTimeTemporalMonthDay(dateTimeFormat, x).
            return handle_date_time_temporal_month_day(vm, date_time_format, temporal_month_day);
        },
        // d. If x has an [[InitializedTemporalTime]] internal slot, then
        [&](GC::Ref<Temporal::PlainTime> temporal_time) {
            // i. Return ? HandleDateTimeTemporalTime(dateTimeFormat, x).
            return handle_date_time_temporal_time(vm, date_time_format, temporal_time);
        },
        // e. If x has an [[InitializedTemporalDateTime]] internal slot, then
        [&](GC::Ref<Temporal::PlainDateTime> date_time) {
            // i. Return ? HandleDateTimeTemporalDateTime(dateTimeFormat, x).
            return handle_date_time_temporal_date_time(vm, date_time_format, date_time);
        },
        // f. If x has an [[InitializedTemporalInstant]] internal slot, then
        [&](GC::Ref<Temporal::Instant> instant) -> ThrowCompletionOr<ValueFormat> {
            // i. Return HandleDateTimeTemporalInstant(dateTimeFormat, x).
            return handle_date_time_temporal_instant(date_time_format, instant);
        },
        // g. Assert: x has an [[InitializedTemporalZonedDateTime]] internal slot.
        [&](GC::Ref<Temporal::ZonedDateTime>) -> ThrowCompletionOr<ValueFormat> {
            // h. Throw a TypeError exception.
            return vm.throw_completion<TypeError>(ErrorType::IntlTemporalZonedDateTime);
        },
        // 2. Return ? HandleDateTimeOthers(dateTimeFormat, x).
        [&](double time) {
            return handle_date_time_others(vm, date_time_format, time);
        });
}

}
