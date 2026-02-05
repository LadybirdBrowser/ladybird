/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainMonthDayConstructor.h>
#include <LibJS/Runtime/VM.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainMonthDay);

// 10 Temporal.PlainMonthDay Objects, https://tc39.es/proposal-temporal/#sec-temporal-plainmonthday-objects
PlainMonthDay::PlainMonthDay(ISODate iso_date, String calendar, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_iso_date(iso_date)
    , m_calendar(move(calendar))
{
}

// 10.5.1 ToTemporalMonthDay ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalmonthday
ThrowCompletionOr<GC::Ref<PlainMonthDay>> to_temporal_month_day(VM& vm, Value item, Value options)
{
    // 1. If options is not present, set options to undefined.

    // 2. If item is a Object, then
    if (item.is_object()) {
        auto const& object = item.as_object();

        // a. If item has an [[InitializedTemporalMonthDay]] internal slot, then
        if (is<PlainMonthDay>(object)) {
            auto const& plain_month_day = static_cast<PlainMonthDay const&>(object);

            // i. Let resolvedOptions be ? GetOptionsObject(options).
            auto resolved_options = TRY(get_options_object(vm, options));

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            TRY(get_temporal_overflow_option(vm, resolved_options));

            // iii. Return ! CreateTemporalMonthDay(item.[[ISODate]], item.[[Calendar]]).
            return MUST(create_temporal_month_day(vm, plain_month_day.iso_date(), plain_month_day.calendar()));
        }

        // b. Let calendar be ? GetTemporalCalendarIdentifierWithISODefault(item).
        auto calendar = TRY(get_temporal_calendar_identifier_with_iso_default(vm, object));

        // c. Let fields be ? PrepareCalendarFields(calendar, item, « YEAR, MONTH, MONTH-CODE, DAY », «», «»).
        auto fields = TRY(prepare_calendar_fields(vm, calendar, object, { { CalendarField::Year, CalendarField::Month, CalendarField::MonthCode, CalendarField::Day } }, {}, CalendarFieldList {}));

        // d. Let resolvedOptions be ? GetOptionsObject(options).
        auto resolved_options = TRY(get_options_object(vm, options));

        // e. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

        // f. Let isoDate be ? CalendarMonthDayFromFields(calendar, fields, overflow).
        auto iso_date = TRY(calendar_month_day_from_fields(vm, calendar, fields, overflow));

        // g. Return ! CreateTemporalMonthDay(isoDate, calendar).
        return MUST(create_temporal_month_day(vm, iso_date, move(calendar)));
    }

    // 3. If item is not a String, throw a TypeError exception.
    if (!item.is_string())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidPlainMonthDay);

    // 4. Let result be ? ParseISODateTime(item, « TemporalMonthDayString »).
    auto parse_result = TRY(parse_iso_date_time(vm, item.as_string().utf8_string_view(), { { Production::TemporalMonthDayString } }));

    // 5. Let calendar be result.[[Calendar]].
    // 6. If calendar is empty, set calendar to "iso8601".
    auto calendar = parse_result.calendar.value_or("iso8601"_string);

    // 7. Set calendar to ? CanonicalizeCalendar(calendar).
    calendar = TRY(canonicalize_calendar(vm, calendar));

    // 8. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 9. Perform ? GetTemporalOverflowOption(resolvedOptions).
    TRY(get_temporal_overflow_option(vm, resolved_options));

    // 10. If calendar is "iso8601", then
    if (calendar == "iso8601"sv) {
        // a. Let referenceISOYear be 1972 (the first ISO 8601 leap year after the epoch).
        static constexpr i32 reference_iso_year = 1972;

        // b. Let isoDate be CreateISODateRecord(referenceISOYear, result.[[Month]], result.[[Day]]).
        auto iso_date = create_iso_date_record(reference_iso_year, parse_result.month, parse_result.day);

        // c. Return ! CreateTemporalMonthDay(isoDate, calendar).
        return MUST(create_temporal_month_day(vm, iso_date, move(calendar)));
    }

    // 11. Let isoDate be CreateISODateRecord(result.[[Year]], result.[[Month]], result.[[Day]]).
    auto iso_date = create_iso_date_record(*parse_result.year, parse_result.month, parse_result.day);

    // 12. If ISODateWithinLimits(isoDate) is false, throw a RangeError exception.
    if (!iso_date_within_limits(iso_date))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainMonthDay);

    // 13. Set result to ISODateToFields(calendar, isoDate, MONTH-DAY).
    auto result = iso_date_to_fields(calendar, iso_date, DateType::MonthDay);

    // 14. NOTE: The following operation is called with CONSTRAIN regardless of the value of overflow, in order for the
    //     calendar to store a canonical value in the [[Year]] field of the [[ISODate]] internal slot of the result.
    // 15. Set isoDate to ? CalendarMonthDayFromFields(calendar, result, CONSTRAIN).
    iso_date = TRY(calendar_month_day_from_fields(vm, calendar, result, Overflow::Constrain));

    // 16. Return ! CreateTemporalMonthDay(isoDate, calendar).
    return MUST(create_temporal_month_day(vm, iso_date, move(calendar)));
}

// 10.5.2 CreateTemporalMonthDay ( isoDate, calendar [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporalmonthday
ThrowCompletionOr<GC::Ref<PlainMonthDay>> create_temporal_month_day(VM& vm, ISODate iso_date, String calendar, GC::Ptr<FunctionObject> new_target)
{
    auto& realm = *vm.current_realm();

    // 1. If ISODateWithinLimits(isoDate) is false, throw a RangeError exception.
    if (!iso_date_within_limits(iso_date))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainMonthDay);

    // 2. If newTarget is not present, set newTarget to %Temporal.PlainMonthDay%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_plain_month_day_constructor();

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainMonthDay.prototype%", « [[InitializedTemporalMonthDay]], [[ISODate]], [[Calendar]] »).
    // 4. Set object.[[ISODate]] to isoDate.
    // 5. Set object.[[Calendar]] to calendar.
    auto object = TRY(ordinary_create_from_constructor<PlainMonthDay>(vm, *new_target, &Intrinsics::temporal_plain_month_day_prototype, iso_date, move(calendar)));

    // 6. Return object.
    return object;
}

// 10.5.3 TemporalMonthDayToString ( monthDay, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-temporalmonthdaytostring
String temporal_month_day_to_string(PlainMonthDay const& month_day, ShowCalendar show_calendar)
{
    // 1. Let month be ToZeroPaddedDecimalString(monthDay.[[ISODate]].[[Month]], 2).
    // 2. Let day be ToZeroPaddedDecimalString(monthDay.[[ISODate]].[[Day]], 2).
    // 3. Let result be the string-concatenation of month, the code unit 0x002D (HYPHEN-MINUS), and day.
    auto result = MUST(String::formatted("{:02}-{:02}", month_day.iso_date().month, month_day.iso_date().day));

    // 4. If showCalendar is one of ALWAYS or CRITICAL, or if monthDay.[[Calendar]] is not "iso8601", then
    if (show_calendar == ShowCalendar::Always || show_calendar == ShowCalendar::Critical || month_day.calendar() != "iso8601"sv) {
        // a. Let year be PadISOYear(monthDay.[[ISODate]].[[Year]]).
        auto year = pad_iso_year(month_day.iso_date().year);

        // b. Set result to the string-concatenation of year, the code unit 0x002D (HYPHEN-MINUS), and result.
        result = MUST(String::formatted("{}-{}", year, result));
    }

    // 5. Let calendarString be FormatCalendarAnnotation(monthDay.[[Calendar]], showCalendar).
    auto calendar_string = format_calendar_annotation(month_day.calendar(), show_calendar);

    // 6. Set result to the string-concatenation of result and calendarString.
    result = MUST(String::formatted("{}{}", result, calendar_string));

    // 7. Return result.
    return result;
}

}
